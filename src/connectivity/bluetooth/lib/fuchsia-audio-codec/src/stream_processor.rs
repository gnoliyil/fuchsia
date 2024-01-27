// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context as _, Error},
    fidl_fuchsia_media::*,
    fidl_fuchsia_mediacodec::*,
    fidl_fuchsia_sysmem::*,
    fuchsia_stream_processors::*,
    futures::{
        future::{maybe_done, MaybeDone},
        io::{self, AsyncWrite},
        ready,
        stream::{FusedStream, Stream},
        task::{Context, Poll, Waker},
        Future, StreamExt,
    },
    parking_lot::{Mutex, RwLock},
    std::{
        collections::{HashSet, VecDeque},
        convert::{TryFrom, TryInto},
        mem,
        pin::Pin,
        sync::Arc,
    },
    tracing::{trace, warn},
};

use crate::{
    buffer_collection_constraints::*,
    sysmem_allocator::{BufferName, SysmemAllocatedBuffers, SysmemAllocation},
};

fn fidl_error_to_io_error(e: fidl::Error) -> io::Error {
    io::Error::new(io::ErrorKind::Other, format_err!("Fidl Error: {}", e))
}

#[derive(Debug)]
/// Listener is a three-valued Option that captures the waker that a listener needs to be woken
/// upon when it polls the future instead of at registration time.
enum Listener {
    /// No one is listening.
    None,
    /// Someone is listening, but either have been woken and not repolled, or never polled yet.
    New,
    /// Someone is listening, and can be woken with the waker.
    Some(Waker),
}

impl Listener {
    /// Adds a waker to be awoken with `Listener::wake`.
    /// Panics if no one is listening.
    fn register(&mut self, waker: Waker) {
        *self = match mem::replace(self, Listener::None) {
            Listener::None => panic!("Polled a listener with no pollers"),
            _ => Listener::Some(waker),
        };
    }

    /// If a listener has polled, wake the listener and replace it with New.
    /// Noop if no one has registered.
    fn wake(&mut self) {
        if let Listener::None = self {
            return;
        }
        match mem::replace(self, Listener::New) {
            Listener::None => panic!("Should have been polled"),
            Listener::Some(waker) => waker.wake(),
            Listener::New => {}
        }
    }

    /// Get a reference to the waker, if there is one waiting.
    fn waker(&self) -> Option<&Waker> {
        if let Listener::Some(ref waker) = self {
            Some(waker)
        } else {
            None
        }
    }
}

impl Default for Listener {
    fn default() -> Self {
        Listener::None
    }
}

/// A queue of encoded packets, to be sent to the `listener` when it polls next.
struct OutputQueue {
    /// The listener. Woken when a packet arrives after a previous poll() returned Pending.
    listener: Listener,
    /// A queue of encoded packets to be delivered to the receiver.
    queue: VecDeque<Packet>,
    /// True when the stream has received an end-of-stream message. The stream will return None
    /// after the `queue` is empty.
    ended: bool,
}

impl OutputQueue {
    /// Adds a packet to the queue and wakes the listener if necessary.
    fn enqueue(&mut self, packet: Packet) {
        self.queue.push_back(packet);
        self.listener.wake();
    }

    /// Signals the end of the stream has happened.
    /// Wakes the listener if necessary.
    fn mark_ended(&mut self) {
        self.ended = true;
        self.listener.wake();
    }

    fn waker(&self) -> Option<&Waker> {
        self.listener.waker()
    }

    /// Wakes the listener so that it will repoll, if it is waiting.
    fn wake(&mut self) {
        self.listener.wake();
    }
}

impl Default for OutputQueue {
    fn default() -> Self {
        OutputQueue { listener: Listener::default(), queue: VecDeque::new(), ended: false }
    }
}

impl Stream for OutputQueue {
    type Item = Packet;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        match self.queue.pop_front() {
            Some(packet) => Poll::Ready(Some(packet)),
            None if self.ended => Poll::Ready(None),
            None => {
                self.listener.register(cx.waker().clone());
                Poll::Pending
            }
        }
    }
}

// The minimum specified by codec is too small to contain the typical pcm frame chunk size for the
// encoder case (1024). Increase to a reasonable amount.
const MIN_INPUT_BUFFER_SIZE: u32 = 4096;
// Go with codec default for output, for frame alignment.
const MIN_OUTPUT_BUFFER_SIZE: u32 = 0;

/// Index of an input buffer to be shared between the client and the StreamProcessor.
#[derive(PartialEq, Eq, Hash, Clone, Debug)]
struct InputBufferIndex(u32);

/// The StreamProcessorInner handles the events that come from the StreamProcessor, mostly related
/// to setup of the buffers and handling the output packets as they arrive.
struct StreamProcessorInner {
    /// The proxy to the stream processor.
    processor: StreamProcessorProxy,
    /// The proxy to the sysmem allocator.
    sysmem_client: AllocatorProxy,
    /// The event stream from the StreamProcessor.  We handle these internally.
    events: StreamProcessorEventStream,
    /// The size in bytes of each input packet
    input_packet_size: u64,
    /// The set of input buffers that are available for writing by the client, without the one
    /// possibly being used by the input_cursor.
    client_owned: HashSet<InputBufferIndex>,
    /// A cursor on the next input buffer location to be written to when new input data arrives.
    input_cursor: Option<(InputBufferIndex, u64)>,
    /// An queue of the indexes of output buffers that have been filled by the processor and a
    /// waiter if someone is waiting on it.
    /// Also holds the output waker, if it is registered.
    output_queue: Mutex<OutputQueue>,
    /// Waker that is waiting on input to be ready.
    input_waker: Option<Waker>,
    /// Allocation for the input buffers.
    input_allocation: MaybeDone<SysmemAllocation>,
    /// Allocation for the output buffers.
    output_allocation: MaybeDone<SysmemAllocation>,
}

impl StreamProcessorInner {
    /// Handles an event from the StreamProcessor. A number of these events come on stream start to
    /// setup the input and output buffers, and from then on the output packets and end of stream
    /// marker, and the input packets are marked as usable after they are processed.
    fn handle_event(&mut self, evt: StreamProcessorEvent) -> Result<(), Error> {
        match evt {
            StreamProcessorEvent::OnInputConstraints { input_constraints } => {
                let _input_constraints = ValidStreamBufferConstraints::try_from(input_constraints)?;
                let buffer_constraints =
                    Self::buffer_constraints_from_min_size(MIN_INPUT_BUFFER_SIZE);
                let processor = self.processor.clone();
                let mut partial_settings = Self::partial_settings();
                let token_fn = move |token| {
                    partial_settings.sysmem_token = Some(token);
                    // FIDL failures will be caught via the request stream.
                    if let Err(e) = processor.set_input_buffer_partial_settings(partial_settings) {
                        warn!("Couldn't set input buffer settings: {:?}", e);
                    }
                };
                self.input_allocation = maybe_done(SysmemAllocation::allocate(
                    self.sysmem_client.clone(),
                    BufferName { name: "StreamProcessorInput", priority: 1 },
                    None,
                    buffer_constraints,
                    token_fn,
                )?);
            }
            StreamProcessorEvent::OnOutputConstraints { output_config } => {
                let output_constraints = ValidStreamOutputConstraints::try_from(output_config)?;
                if !output_constraints.buffer_constraints_action_required {
                    return Ok(());
                }
                let buffer_constraints =
                    Self::buffer_constraints_from_min_size(MIN_OUTPUT_BUFFER_SIZE);
                let processor = self.processor.clone();
                let mut partial_settings = Self::partial_settings();
                let token_fn = move |token| {
                    partial_settings.sysmem_token = Some(token);
                    // FIDL failures will be caught via the request stream.
                    if let Err(e) = processor.set_output_buffer_partial_settings(partial_settings) {
                        warn!("Couldn't set output buffer settings: {:?}", e);
                    }
                };

                self.output_allocation = maybe_done(SysmemAllocation::allocate(
                    self.sysmem_client.clone(),
                    BufferName { name: "StreamProcessorOutput", priority: 1 },
                    None,
                    buffer_constraints,
                    token_fn,
                )?);
            }
            StreamProcessorEvent::OnOutputPacket { output_packet, .. } => {
                let mut lock = self.output_queue.lock();
                lock.enqueue(output_packet);
            }
            StreamProcessorEvent::OnFreeInputPacket {
                free_input_packet: PacketHeader { packet_index: Some(idx), .. },
            } => {
                if !self.client_owned.insert(InputBufferIndex(idx)) {
                    warn!("Freed an input packet that was already freed: {:?}", idx);
                }
                self.setup_input_cursor();
            }
            StreamProcessorEvent::OnOutputEndOfStream { .. } => {
                let mut lock = self.output_queue.lock();
                lock.mark_ended();
            }
            StreamProcessorEvent::OnOutputFormat { .. } => {}
            e => trace!("Unhandled stream processor event: {:?}", e),
        }
        Ok(())
    }

    /// Process one event, and return Poll::Ready if the item has been processed,
    /// and Poll::Pending if no event has been processed and the waker will be woken if
    /// another event happens.
    fn process_event(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), Error>> {
        match ready!(self.events.poll_next_unpin(cx)) {
            Some(Err(e)) => Poll::Ready(Err(e.into())),
            Some(Ok(event)) => Poll::Ready(self.handle_event(event)),
            None => Poll::Ready(Err(format_err!("Client disconnected"))),
        }
    }

    fn buffer_constraints_from_min_size(min_buffer_size: u32) -> BufferCollectionConstraints {
        let mut collection_constraints = BUFFER_COLLECTION_CONSTRAINTS_DEFAULT;
        collection_constraints.has_buffer_memory_constraints = true;
        collection_constraints.buffer_memory_constraints.min_size_bytes = min_buffer_size;
        collection_constraints
    }

    fn partial_settings() -> StreamBufferPartialSettings {
        StreamBufferPartialSettings {
            buffer_lifetime_ordinal: Some(1),
            buffer_constraints_version_ordinal: Some(1),
            sysmem_token: None,
            ..StreamBufferPartialSettings::EMPTY
        }
    }

    fn input_buffers(&mut self) -> &mut SysmemAllocatedBuffers {
        Pin::new(&mut self.input_allocation)
            .output_mut()
            .expect("allocation completed")
            .as_mut()
            .expect("succcessful allocation")
    }

    fn output_buffers(&mut self) -> &mut SysmemAllocatedBuffers {
        Pin::new(&mut self.output_allocation)
            .output_mut()
            .expect("allocation completed")
            .as_mut()
            .expect("succcessful allocation")
    }

    /// Called when the input_allocation future finishes.
    /// Takes the buffers out of the allocator, and sets up the input cursor to accept data.
    fn input_allocation_complete(&mut self) -> Result<(), Error> {
        let _ = Pin::new(&mut self.input_allocation)
            .output_mut()
            .ok_or(format_err!("allocation isn't complete"))?;

        let settings = self.input_buffers().settings();
        self.input_packet_size = settings.size_bytes.try_into()?;
        let buffer_count = self.input_buffers().len();
        for i in 0..buffer_count {
            let _ = self.client_owned.insert(InputBufferIndex(i.try_into()?));
        }
        // allocation is complete, and we can write to the input.
        self.setup_input_cursor();
        Ok(())
    }

    /// Called when the output allocation future finishes.
    /// Takes the buffers out of the allocator, and sets up the output buffers for retrieval of output,
    /// signaling to the processor that the output buffers are set.
    fn output_allocation_complete(&mut self) -> Result<(), Error> {
        let _ = Pin::new(&mut self.output_allocation)
            .output_mut()
            .ok_or(format_err!("allocation isn't complete"))?;
        self.processor
            .complete_output_buffer_partial_settings(/*buffer_lifetime_ordinal=*/ 1)
            .context("setting output buffer settings")?;
        Ok(())
    }

    /// Poll any of the allocations that are waiting to complete, returning Pending if
    /// any are still waiting to finish, and Ready if one has failed or both have completed.
    fn poll_buffer_allocation(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), Error>> {
        if let MaybeDone::Future(_) = self.input_allocation {
            match Pin::new(&mut self.input_allocation).poll(cx) {
                Poll::Ready(()) => {
                    if let Err(e) = self.input_allocation_complete() {
                        return Poll::Ready(Err(e));
                    }
                }
                Poll::Pending => {}
            };
        }
        if let MaybeDone::Future(_) = self.output_allocation {
            match Pin::new(&mut self.output_allocation).poll(cx) {
                Poll::Ready(()) => {
                    if let Err(e) = self.output_allocation_complete() {
                        return Poll::Ready(Err(e));
                    }
                }
                Poll::Pending => {}
            };
        }
        Poll::Pending
    }

    /// Provides the current registered waiting context with priority given to the output waker.
    fn waiting_waker(&self) -> Option<Waker> {
        match (self.output_queue.lock().waker(), &self.input_waker) {
            // No one is waiting.
            (None, None) => None,
            (Some(waker), _) => Some(waker.clone()),
            (_, Some(waker)) => Some(waker.clone()),
        }
    }

    /// Process all the events that are currently available from the StreamProcessor and Allocators,
    /// waking any known waker to be woken when another event arrives.
    /// Returns Ok(()) if this was accomplished or Err() if an error occurred while processing.
    fn poll_events(&mut self) -> Result<(), Error> {
        let waker = loop {
            let waker = match self.waiting_waker() {
                // No one still needs to be woken.  This means all the wakers have been awoke,
                // and will repoll.
                None => return Ok(()),
                Some(waker) => waker,
            };
            match self.process_event(&mut Context::from_waker(&waker)) {
                Poll::Pending => break waker,
                Poll::Ready(Err(e)) => {
                    warn!("Stream processing error: {:?}", e);
                    return Err(e.into());
                }
                // Didn't set the waker to be awoken, so let's try again.
                Poll::Ready(Ok(())) => {}
            }
        };

        if let Poll::Ready(Err(e)) = self.poll_buffer_allocation(&mut Context::from_waker(&waker)) {
            warn!("Stream buffer allocation error: {:?}", e);
            return Err(e.into());
        }
        Ok(())
    }

    fn wake_output(&mut self) {
        self.output_queue.lock().wake();
    }

    fn wake_input(&mut self) {
        if let Some(w) = self.input_waker.take() {
            w.wake();
        }
    }

    /// Attempts to set up a new input cursor, out of the current set of client owned input buffers.
    /// If the cursor is already set, this does nothing.
    fn setup_input_cursor(&mut self) {
        if self.input_cursor.is_some() {
            // Nothing to be done
            return;
        }
        let next_idx = match self.client_owned.iter().next() {
            None => return,
            Some(idx) => idx.clone(),
        };
        let _ = self.client_owned.remove(&next_idx);
        self.input_cursor = Some((next_idx, 0));
        self.wake_input();
    }

    /// Reads an output packet from the output buffers, and marks the packets as recycled so the
    /// output buffer can be reused. Allocates a new vector to hold the data.
    fn read_output_packet(&mut self, packet: Packet) -> Result<Vec<u8>, Error> {
        let packet = ValidPacket::try_from(packet)?;

        let output_size = packet.valid_length_bytes as usize;
        let offset = packet.start_offset as u64;
        let mut output = vec![0; output_size];
        let buf_idx = packet.buffer_index;
        let vmo = self.output_buffers().get_mut(buf_idx).expect("output vmo should exist");
        vmo.read(&mut output, offset)?;
        self.processor.recycle_output_packet(packet.header.into())?;
        Ok(output)
    }
}

/// Struct representing a CodecFactory .
/// Input sent to the encoder via `StreamProcessor::write_bytes` is queued for delivery, and delivered
/// whenever a packet is full or `StreamProcessor::send_packet` is called.  Output can be retrieved using
/// an `StreamProcessorStream` from `StreamProcessor::take_output_stream`.
pub struct StreamProcessor {
    inner: Arc<RwLock<StreamProcessorInner>>,
}

/// An StreamProcessorStream is a Stream of processed data from a stream processor.
/// Returned from `StreamProcessor::take_output_stream`.
pub struct StreamProcessorOutputStream {
    inner: Arc<RwLock<StreamProcessorInner>>,
}

impl StreamProcessor {
    /// Create a new StreamProcessor given the proxy.
    /// Takes the event stream of the proxy.
    fn create(processor: StreamProcessorProxy, sysmem_client: AllocatorProxy) -> Self {
        let events = processor.take_event_stream();
        Self {
            inner: Arc::new(RwLock::new(StreamProcessorInner {
                processor,
                sysmem_client,
                events,
                input_packet_size: 0,
                client_owned: HashSet::new(),
                input_cursor: None,
                output_queue: Default::default(),
                input_waker: None,
                input_allocation: maybe_done(SysmemAllocation::pending()),
                output_allocation: maybe_done(SysmemAllocation::pending()),
            })),
        }
    }

    /// Create a new StreamProcessor encoder, with the given `input_domain` and `encoder_settings`.  See
    /// stream_processor.fidl for descriptions of these parameters.  This is only meant for audio
    /// encoding.
    pub fn create_encoder(
        input_domain: DomainFormat,
        encoder_settings: EncoderSettings,
    ) -> Result<StreamProcessor, Error> {
        let sysmem_client = fuchsia_component::client::connect_to_protocol::<AllocatorMarker>()
            .context("Connecting to sysmem")?;

        let format_details = FormatDetails {
            domain: Some(input_domain),
            encoder_settings: Some(encoder_settings),
            format_details_version_ordinal: Some(1),
            mime_type: Some("audio/pcm".to_string()),
            oob_bytes: None,
            pass_through_parameters: None,
            timebase: None,
            ..FormatDetails::EMPTY
        };

        let encoder_params = CreateEncoderParams {
            input_details: Some(format_details),
            require_hw: Some(false),
            ..CreateEncoderParams::EMPTY
        };

        let codec_svc = fuchsia_component::client::connect_to_protocol::<CodecFactoryMarker>()
            .context("Failed to connect to Codec Factory")?;

        let (processor, stream_processor_serverend) = fidl::endpoints::create_proxy()?;

        codec_svc.create_encoder(encoder_params, stream_processor_serverend)?;

        Ok(StreamProcessor::create(processor, sysmem_client))
    }

    /// Create a new StreamProcessor decoder, with the given `mime_type` and optional `oob_bytes`.  See
    /// stream_processor.fidl for descriptions of these parameters.  This is only meant for audio
    /// decoding.
    pub fn create_decoder(
        mime_type: &str,
        oob_bytes: Option<Vec<u8>>,
    ) -> Result<StreamProcessor, Error> {
        let sysmem_client = fuchsia_component::client::connect_to_protocol::<AllocatorMarker>()
            .context("Connecting to sysmem")?;

        let format_details = FormatDetails {
            mime_type: Some(mime_type.to_string()),
            oob_bytes: oob_bytes,
            format_details_version_ordinal: Some(1),
            encoder_settings: None,
            domain: None,
            pass_through_parameters: None,
            timebase: None,
            ..FormatDetails::EMPTY
        };

        let decoder_params = CreateDecoderParams {
            input_details: Some(format_details),
            permit_lack_of_split_header_handling: Some(true),
            ..CreateDecoderParams::EMPTY
        };

        let codec_svc = fuchsia_component::client::connect_to_protocol::<CodecFactoryMarker>()
            .context("Failed to connect to Codec Factory")?;

        let (processor, stream_processor_serverend) = fidl::endpoints::create_proxy()?;

        codec_svc.create_decoder(decoder_params, stream_processor_serverend)?;

        Ok(StreamProcessor::create(processor, sysmem_client))
    }

    /// Take a stream object which will produce the output of the processor.
    /// Only one StreamProcessorOutputStream object can exist at a time, and this will return an Error if it is
    /// already taken.
    pub fn take_output_stream(&mut self) -> Result<StreamProcessorOutputStream, Error> {
        {
            let read = self.inner.read();
            let mut lock = read.output_queue.lock();
            if let Listener::None = lock.listener {
                lock.listener = Listener::New;
            } else {
                return Err(format_err!("Output stream already taken"));
            }
        }
        Ok(StreamProcessorOutputStream { inner: self.inner.clone() })
    }

    /// Deliver input to the stream processor.  Returns the number of bytes delivered.
    fn write_bytes(&mut self, bytes: &[u8]) -> Result<usize, io::Error> {
        let mut bytes_idx = 0;
        while bytes.len() > bytes_idx {
            {
                let mut write = self.inner.write();
                let (idx, size) = match write.input_cursor.take() {
                    None => return Ok(bytes_idx),
                    Some(x) => x,
                };
                let space_left = write.input_packet_size - size;
                let left_to_write = bytes.len() - bytes_idx;
                let buffer_vmo = write.input_buffers().get_mut(idx.0).expect("need buffer vmo");
                if space_left as usize > left_to_write {
                    let write_buf = &bytes[bytes_idx..];
                    let write_len = write_buf.len();
                    buffer_vmo.write(write_buf, size)?;
                    bytes_idx += write_len;
                    write.input_cursor = Some((idx, size + write_len as u64));
                    assert!(bytes.len() == bytes_idx);
                    return Ok(bytes_idx);
                }
                let end_idx = bytes_idx + space_left as usize;
                let write_buf = &bytes[bytes_idx..end_idx];
                let write_len = write_buf.len();
                buffer_vmo.write(write_buf, size)?;
                bytes_idx += write_len;
                // this buffer is done, ship it!
                assert_eq!(size + write_len as u64, write.input_packet_size);
                write.input_cursor = Some((idx, write.input_packet_size));
            }
            self.send_packet()?;
        }
        Ok(bytes_idx)
    }

    /// Flush the input buffer to the processor, relinquishing the ownership of the buffer
    /// currently in the input cursor, and picking a new input buffer.  If there is no input
    /// buffer left, the input cursor is left as None.
    pub fn send_packet(&mut self) -> Result<(), io::Error> {
        let mut write = self.inner.write();
        if write.input_cursor.is_none() {
            // Nothing to flush, nothing can have been written to an empty input cursor.
            return Ok(());
        }
        let (idx, size) = write.input_cursor.take().expect("input cursor is none");
        if size == 0 {
            // Can't send empty packet to processor.
            write.input_cursor = Some((idx, size));
            return Ok(());
        }
        let packet = Packet {
            header: Some(PacketHeader {
                buffer_lifetime_ordinal: Some(1),
                packet_index: Some(idx.0),
                ..PacketHeader::EMPTY
            }),
            buffer_index: Some(idx.0),
            stream_lifetime_ordinal: Some(1),
            start_offset: Some(0),
            valid_length_bytes: Some(size as u32),
            start_access_unit: Some(true),
            known_end_access_unit: Some(true),
            ..Packet::EMPTY
        };
        write.processor.queue_input_packet(packet).map_err(fidl_error_to_io_error)?;
        // pick another buffer for the input cursor
        write.setup_input_cursor();
        Ok(())
    }

    /// Test whether it is possible to write to the StreamProcessor. If there are no input buffers
    /// available, returns Poll::Pending and arranges for the input task to receive a
    /// notification when an input buffer may be available or the encoder is closed.
    fn poll_writable(&mut self, cx: &mut Context<'_>) -> Poll<Result<(), io::Error>> {
        let mut write = self.inner.write();
        // Drop the current input waker, since we have a new one.
        // If the output waker is set, it should already be queued to be woken for the codec.
        write.input_waker = None;
        if write.input_cursor.is_some() {
            return Poll::Ready(Ok(()));
        }
        write.input_waker = Some(cx.waker().clone());
        // This can:
        //  - wake the input waker (somehow received a input packet)
        //  - poll with the output waker, setting it up to be woken
        //  - poll with the input waker to be woken
        if let Err(e) = write.poll_events() {
            return Poll::Ready(Err(io::Error::new(io::ErrorKind::Other, e)));
        }
        Poll::Pending
    }

    pub fn close(&mut self) -> Result<(), io::Error> {
        self.send_packet()?;

        let mut write = self.inner.write();

        write.processor.queue_input_end_of_stream(1).map_err(fidl_error_to_io_error)?;
        // TODO: indicate this another way so that we can send an error if someone tries to write
        // it after it's closed.
        write.input_cursor = None;
        write.wake_input();
        write.wake_output();
        Ok(())
    }
}

impl AsyncWrite for StreamProcessor {
    fn poll_write(
        mut self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        ready!(self.poll_writable(cx))?;
        match self.write_bytes(buf) {
            Ok(written) => Poll::Ready(Ok(written)),
            Err(e) => Poll::Ready(Err(e.into())),
        }
    }

    fn poll_flush(mut self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(self.send_packet())
    }

    fn poll_close(mut self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(self.send_packet())
    }
}

impl Stream for StreamProcessorOutputStream {
    type Item = Result<Vec<u8>, Error>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let mut write = self.inner.write();
        // If we have a item ready, just return it.
        let packet = {
            let mut queue = write.output_queue.lock();
            match queue.poll_next_unpin(cx) {
                Poll::Ready(Some(packet)) => Some(Some(packet)),
                Poll::Ready(None) => Some(None),
                Poll::Pending => {
                    // The waker has been set for when the queue gets data.
                    // We also need to set the same waker if an event happens.
                    None
                }
            }
        };
        // We always need to set a waker for the events loop (this may be the same waker as above,
        // or the input waker if the stream returned a packet)
        if let Err(e) = write.poll_events() {
            return Poll::Ready(Some(Err(e.into())));
        }
        match packet {
            Some(Some(packet)) => Poll::Ready(Some(write.read_output_packet(packet))),
            Some(None) => Poll::Ready(None),
            None => Poll::Pending,
        }
    }
}

impl FusedStream for StreamProcessorOutputStream {
    fn is_terminated(&self) -> bool {
        self.inner.read().output_queue.lock().ended
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use byteorder::{ByteOrder, NativeEndian};
    use fixture::fixture;
    use fuchsia_async as fasync;
    use futures::{io::AsyncWriteExt, Future, FutureExt};
    use futures_test::task::new_count_waker;
    use hex;
    use sha2::{Digest as _, Sha256};
    use std::fs::File;
    use std::io::{Read, Write};

    use stream_processor_test::ExpectedDigest;

    const PCM_SAMPLE_SIZE: usize = 2;

    #[derive(Clone, Debug)]
    pub struct PcmAudio {
        pcm_format: PcmFormat,
        buffer: Vec<u8>,
    }

    impl PcmAudio {
        pub fn create_saw_wave(pcm_format: PcmFormat, frame_count: usize) -> Self {
            const FREQUENCY: f32 = 20.0;
            const AMPLITUDE: f32 = 0.2;

            let pcm_frame_size = PCM_SAMPLE_SIZE * pcm_format.channel_map.len();
            let samples_per_frame = pcm_format.channel_map.len();
            let sample_count = frame_count * samples_per_frame;

            let mut buffer = vec![0; frame_count * pcm_frame_size];

            for i in 0..sample_count {
                let frame = (i / samples_per_frame) as f32;
                let value =
                    ((frame * FREQUENCY / (pcm_format.frames_per_second as f32)) % 1.0) * AMPLITUDE;
                let sample = (value * i16::max_value() as f32) as i16;

                let mut sample_bytes = [0; std::mem::size_of::<i16>()];
                NativeEndian::write_i16(&mut sample_bytes, sample);

                let offset = i * PCM_SAMPLE_SIZE;
                buffer[offset] = sample_bytes[0];
                buffer[offset + 1] = sample_bytes[1];
            }

            Self { pcm_format, buffer }
        }

        pub fn frame_size(&self) -> usize {
            self.pcm_format.channel_map.len() * PCM_SAMPLE_SIZE
        }
    }

    // Note: stolen from audio_encoder_test, update to stream_processor_test lib when this gets
    // moved.
    pub struct BytesValidator {
        pub output_file: Option<&'static str>,
        pub expected_digest: ExpectedDigest,
    }

    impl BytesValidator {
        fn write_and_hash(&self, mut file: impl Write, bytes: &[u8]) -> Result<(), Error> {
            let mut hasher = Sha256::default();

            file.write_all(&bytes)?;
            hasher.update(&bytes);

            let digest: [u8; 32] = hasher.finalize().into();
            if self.expected_digest.bytes != digest {
                return Err(format_err!(
                    "Expected {}; got {}",
                    self.expected_digest,
                    hex::encode(digest)
                ))
                .into();
            }

            Ok(())
        }

        fn output_file(&self) -> Result<impl Write, Error> {
            Ok(if let Some(file) = self.output_file {
                Box::new(std::fs::File::create(file)?) as Box<dyn Write>
            } else {
                Box::new(std::io::sink()) as Box<dyn Write>
            })
        }

        fn validate(&self, bytes: &[u8]) -> Result<(), Error> {
            self.write_and_hash(self.output_file()?, &bytes)
        }
    }

    #[fuchsia::test]
    fn encode_sbc() {
        let mut exec = fasync::TestExecutor::new();

        let pcm_format = PcmFormat {
            pcm_mode: AudioPcmMode::Linear,
            bits_per_sample: 16,
            frames_per_second: 44100,
            channel_map: vec![AudioChannelId::Cf],
        };

        let sub_bands = SbcSubBands::SubBands4;
        let block_count = SbcBlockCount::BlockCount8;

        let input_frames = 3000;

        let pcm_audio = PcmAudio::create_saw_wave(pcm_format.clone(), input_frames);

        let sbc_encoder_settings = EncoderSettings::Sbc(SbcEncoderSettings {
            sub_bands,
            block_count,
            allocation: SbcAllocation::AllocLoudness,
            channel_mode: SbcChannelMode::Mono,
            bit_pool: 59, // Recommended from the SBC spec for these parameters.
        });

        let input_domain = DomainFormat::Audio(AudioFormat::Uncompressed(
            AudioUncompressedFormat::Pcm(pcm_format),
        ));

        let mut encoder = StreamProcessor::create_encoder(input_domain, sbc_encoder_settings)
            .expect("to create Encoder");

        let frames_per_packet: usize = 8; // Randomly chosen by fair d10 roll.
        let packet_size = pcm_audio.frame_size() * frames_per_packet;
        let mut packets = pcm_audio.buffer.as_slice().chunks(packet_size);
        let first_packet = packets.next().unwrap();

        // Write an initial frame to the encoder.
        // This is required to get past allocating the input/output buffers.
        let written =
            exec.run_singlethreaded(&mut encoder.write(first_packet)).expect("successful write");
        assert_eq!(written, first_packet.len());

        let mut encoded_stream = encoder.take_output_stream().expect("Stream should be taken");

        // Shouldn't be able to take the stream twice
        assert!(encoder.take_output_stream().is_err());

        // Polling the encoded stream before the encoder has started up should wake it when
        // output starts happening, set up the poll here.
        let encoded_fut = encoded_stream.next();

        let (waker, encoder_fut_wake_count) = new_count_waker();
        let mut counting_ctx = Context::from_waker(&waker);

        fasync::pin_mut!(encoded_fut);
        assert!(encoded_fut.poll(&mut counting_ctx).is_pending());

        let mut frames_sent = first_packet.len() / pcm_audio.frame_size();

        for packet in packets {
            let mut written_fut = encoder.write(&packet);

            let written_bytes =
                exec.run_singlethreaded(&mut written_fut).expect("to write to encoder");

            assert_eq!(packet.len(), written_bytes);
            frames_sent += packet.len() / pcm_audio.frame_size();
        }

        encoder.close().expect("stream should always be closable");

        assert_eq!(input_frames, frames_sent);

        // When an unprocessed event has happened on the stream, even if intervening events have been
        // procesed by the input processes, it should wake the output future to process the events.
        let woke_count = encoder_fut_wake_count.get();
        while encoder_fut_wake_count.get() == woke_count {
            let _ = exec.run_until_stalled(&mut futures::future::pending::<()>());
        }
        assert_eq!(encoder_fut_wake_count.get(), woke_count + 1);

        // Get data from the output now.
        let mut encoded = Vec::new();

        loop {
            let mut encoded_fut = encoded_stream.next();

            match exec.run_singlethreaded(&mut encoded_fut) {
                Some(Ok(enc_data)) => {
                    assert!(!enc_data.is_empty());
                    encoded.extend_from_slice(&enc_data);
                }
                Some(Err(e)) => {
                    panic!("Unexpected error when polling encoded data: {}", e);
                }
                None => {
                    break;
                }
            }
        }

        // Match the encoded data to the known hash.
        let expected_digest = ExpectedDigest::new(
            "Sbc: 44.1kHz/Loudness/Mono/bitpool 56/blocks 8/subbands 4",
            "5c65a88bda3f132538966d87df34aa8675f85c9892b7f9f5571f76f3c7813562",
        );
        let hash_validator = BytesValidator { output_file: None, expected_digest };

        assert_eq!(6110, encoded.len(), "Encoded size should be equal");

        let validated = hash_validator.validate(encoded.as_slice());
        assert!(validated.is_ok(), "Failed hash: {:?}", validated);
    }

    fn fix_sbc_test_file<F>(_name: &str, test: F)
    where
        F: FnOnce(Vec<u8>) -> (),
    {
        const SBC_TEST_FILE: &str = "/pkg/data/s16le44100mono.sbc";

        let mut sbc_data = Vec::new();
        let _ = File::open(SBC_TEST_FILE)
            .expect("open test file")
            .read_to_end(&mut sbc_data)
            .expect("read test file");

        test(sbc_data)
    }

    #[fixture(fix_sbc_test_file)]
    #[fuchsia::test]
    fn decode_sbc(sbc_data: Vec<u8>) {
        let mut exec = fasync::TestExecutor::new();

        const SBC_FRAME_SIZE: usize = 72;
        const INPUT_FRAMES: usize = 23;

        // SBC codec info corresponding to Mono reference stream.
        let oob_data = Some([0x82, 0x00, 0x00, 0x00].to_vec());
        let mut decoder =
            StreamProcessor::create_decoder("audio/sbc", oob_data).expect("to create decoder");

        let mut decoded_stream = decoder.take_output_stream().expect("Stream should be taken");

        // Shouldn't be able to take the stream twice
        assert!(decoder.take_output_stream().is_err());

        let mut frames_sent = 0;

        let frames_per_packet: usize = 1; // Randomly chosen by fair d10 roll.
        let packet_size = SBC_FRAME_SIZE * frames_per_packet;

        for frames in sbc_data.as_slice().chunks(packet_size) {
            let mut written_fut = decoder.write(&frames);

            let written_bytes =
                exec.run_singlethreaded(&mut written_fut).expect("to write to decoder");

            assert_eq!(frames.len(), written_bytes);
            frames_sent += frames.len() / SBC_FRAME_SIZE;
        }

        assert_eq!(INPUT_FRAMES, frames_sent);

        let flush_fut = decoder.flush();
        fasync::pin_mut!(flush_fut);
        exec.run_singlethreaded(&mut flush_fut).expect("to flush the decoder");

        decoder.close().expect("stream should always be closable");

        // Get data from the output now.
        let mut decoded = Vec::new();

        loop {
            let mut decoded_fut = decoded_stream.next();

            match exec.run_singlethreaded(&mut decoded_fut) {
                Some(Ok(dec_data)) => {
                    assert!(!dec_data.is_empty());
                    decoded.extend_from_slice(&dec_data);
                }
                Some(Err(e)) => {
                    panic!("Unexpected error when polling decoded data: {}", e);
                }
                None => {
                    break;
                }
            }
        }

        // Match the decoded data to the known hash.
        let expected_digest = ExpectedDigest::new(
            "Pcm: 44.1kHz/16bit/Mono",
            "ff2e7afea51217886d3df15b9a623b4e49c9bd9bd79c58ac01bc94c5511e08d6",
        );
        let hash_validator = BytesValidator { output_file: None, expected_digest };

        assert_eq!(256 * INPUT_FRAMES, decoded.len(), "Decoded size should be equal");

        let validated = hash_validator.validate(decoded.as_slice());
        assert!(validated.is_ok(), "Failed hash: {:?}", validated);
    }

    #[fixture(fix_sbc_test_file)]
    #[fuchsia::test]
    fn decode_sbc_wakes_output_to_process_events(sbc_data: Vec<u8>) {
        let mut exec = fasync::TestExecutor::new();
        const SBC_FRAME_SIZE: usize = 72;

        // SBC codec info corresponding to Mono reference stream.
        let oob_data = Some([0x82, 0x00, 0x00, 0x00].to_vec());
        let mut decoder =
            StreamProcessor::create_decoder("audio/sbc", oob_data).expect("to create decoder");

        let mut chunks = sbc_data.as_slice().chunks(SBC_FRAME_SIZE);
        let next_frame = chunks.next().unwrap();

        // Write an initial frame to the encoder.
        // This is required to get past allocating the input/output buffers.
        let written =
            exec.run_singlethreaded(&mut decoder.write(next_frame)).expect("successful write");
        assert_eq!(written, next_frame.len());

        let mut decoded_stream = decoder.take_output_stream().expect("Stream should be taken");

        // Polling the decoded stream before the decoder has started up should wake it when
        // output starts happening, set up the poll here.
        let decoded_fut = decoded_stream.next();

        let (waker, decoder_fut_wake_count) = new_count_waker();
        let mut counting_ctx = Context::from_waker(&waker);

        fasync::pin_mut!(decoded_fut);
        assert!(decoded_fut.poll(&mut counting_ctx).is_pending());

        // Send only one frame. This is not eneough to automatically cause output to be generated
        // by pushing data.
        let frame = chunks.next().unwrap();
        let mut written_fut = decoder.write(&frame);
        let written_bytes = exec.run_singlethreaded(&mut written_fut).expect("to write to decoder");
        assert_eq!(frame.len(), written_bytes);

        let flush_fut = decoder.flush();
        fasync::pin_mut!(flush_fut);
        exec.run_singlethreaded(&mut flush_fut).expect("to flush the decoder");

        // When an unprocessed event has happened on the stream, even if intervening events have been
        // procesed by the input processes, it should wake the output future to process the events.
        assert_eq!(decoder_fut_wake_count.get(), 0);
        while decoder_fut_wake_count.get() == 0 {
            let _ = exec.run_until_stalled(&mut futures::future::pending::<()>());
        }
        assert_eq!(decoder_fut_wake_count.get(), 1);

        let mut decoded = Vec::new();
        // Drops the previous decoder future, which is fine.
        let mut decoded_fut = decoded_stream.next();

        match exec.run_singlethreaded(&mut decoded_fut) {
            Some(Ok(dec_data)) => {
                assert!(!dec_data.is_empty());
                decoded.extend_from_slice(&dec_data);
            }
            x => panic!("Expected decoded frame, got {:?}", x),
        }

        assert_eq!(512, decoded.len(), "Decoded size should be equal to one frame");
    }

    #[fixture(fix_sbc_test_file)]
    #[fuchsia::test]
    fn decode_sbc_wakes_input_to_process_events(sbc_data: Vec<u8>) {
        let mut exec = fasync::TestExecutor::new();
        const SBC_FRAME_SIZE: usize = 72;

        // SBC codec info corresponding to Mono reference stream.
        let oob_data = Some([0x82, 0x00, 0x00, 0x00].to_vec());
        let mut decoder =
            StreamProcessor::create_decoder("audio/sbc", oob_data).expect("to create decoder");

        let mut decoded_stream = decoder.take_output_stream().expect("Stream should be taken");

        // Polling the decoded stream before the decoder has started up should wake it when
        // output starts happening, set up the poll here.
        let decoded_fut = decoded_stream.next();
        fasync::pin_mut!(decoded_fut);

        match exec.run_until_stalled(&mut decoded_fut) {
            Poll::Pending => {}
            x => panic!("Expected decoded stream to be pending (no input) got {:?}", x),
        };

        let mut chunks = sbc_data.as_slice().chunks(SBC_FRAME_SIZE);
        let next_frame = chunks.next().unwrap();

        // Write an initial frame to the encoder.
        // This is to get past allocating the input/output buffers stage.
        let written =
            exec.run_singlethreaded(&mut decoder.write(next_frame)).expect("successful write");
        assert_eq!(written, next_frame.len());

        // Write to the encoder until we cannot write anymore, because there are no input buffers
        // available.  This should happen when all the input buffers are full and and the input
        // buffers are waiting to be written.
        let (waker, write_fut_wake_count) = new_count_waker();
        let mut counting_ctx = Context::from_waker(&waker);

        for frame in chunks {
            let mut written_fut = decoder.write(&frame);
            if written_fut.poll_unpin(&mut counting_ctx).is_pending() {
                // We should have never been woken until now, because we always were ready before,
                // and the output waker is processing codec events.
                assert_eq!(0, write_fut_wake_count.get());
                break;
            }
            // Flush the packet, to make input buffers get spent faster.
            let flush_fut = decoder.flush();
            fasync::pin_mut!(flush_fut);
            exec.run_singlethreaded(&mut flush_fut).expect("to flush the decoder");
        }

        // We should be able to get a decoded output, once the codec does it's thing.
        let decoded_frame = exec.run_singlethreaded(&mut decoded_fut);
        assert_eq!(512, decoded_frame.unwrap().unwrap().len(), "Decoded frame size wrong");

        // Fill the input buffer again so the input waker is registered.
        let chunks = sbc_data.as_slice().chunks(SBC_FRAME_SIZE);
        for frame in chunks {
            let mut written_fut = decoder.write(&frame);
            if written_fut.poll_unpin(&mut counting_ctx).is_pending() {
                break;
            }
            // Flush the packet, to make input buffers get spent faster.
            let flush_fut = decoder.flush();
            fasync::pin_mut!(flush_fut);
            exec.run_singlethreaded(&mut flush_fut).expect("to flush the decoder");
        }

        // The input waker should be the one waiting on events from the codec (and woken up)
        // At some point, we will get an event from the encoder, with no output waker set, and this
        // should wake the input waker, which is waiting to be woken up.
        let woken_count = write_fut_wake_count.get();
        while write_fut_wake_count.get() == woken_count {
            let _ = exec.run_until_stalled(&mut futures::future::pending::<()>());
        }

        // Note: at this point, we may not be able to write another frame, but the waiter should
        // repoll, and set the waker again.
    }
}
