// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::runtime::{EHandle, PacketReceiver, ReceiverRegistration};
use fuchsia_zircon::{self as zx, AsHandleRef, Signals};
use futures::io::{self, AsyncRead, AsyncWrite};
use futures::{
    future::poll_fn,
    ready,
    stream::Stream,
    task::{AtomicWaker, Context},
};
use std::fmt;
use std::pin::Pin;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;
use std::task::Poll;

pub struct SocketPacketReceiver {
    signals: AtomicU32,
    read_task: AtomicWaker,
    write_task: AtomicWaker,
}

impl PacketReceiver for SocketPacketReceiver {
    fn receive_packet(&self, packet: zx::Packet) {
        let observed = if let zx::PacketContents::SignalOne(p) = packet.contents() {
            p.observed()
        } else {
            return;
        };

        let old =
            Signals::from_bits(self.signals.fetch_or(observed.bits(), Ordering::SeqCst)).unwrap();

        let became_closed = observed.contains(Signals::SOCKET_PEER_CLOSED)
            && !old.contains(Signals::SOCKET_PEER_CLOSED);

        if observed.contains(Signals::SOCKET_READABLE) && !old.contains(Signals::SOCKET_READABLE)
            || became_closed
        {
            self.read_task.wake();
        }
        if observed.contains(Signals::SOCKET_WRITABLE) && !old.contains(Signals::SOCKET_WRITABLE)
            || became_closed
        {
            self.write_task.wake();
        }
    }
}

impl SocketPacketReceiver {
    fn new(signals: AtomicU32, read_task: AtomicWaker, write_task: AtomicWaker) -> Self {
        Self { signals, read_task, write_task }
    }
}

/// An I/O object representing a `Socket`.
pub struct Socket {
    handle: zx::Socket,
    receiver: ReceiverRegistration<SocketPacketReceiver>,
}

impl AsRef<zx::Socket> for Socket {
    fn as_ref(&self) -> &zx::Socket {
        &self.handle
    }
}

impl AsHandleRef for Socket {
    fn as_handle_ref(&self) -> zx::HandleRef<'_> {
        self.handle.as_handle_ref()
    }
}

impl Socket {
    /// Create a new `Socket` from a previously-created zx::Socket.
    pub fn from_socket(handle: zx::Socket) -> Result<Self, zx::Status> {
        let ehandle = EHandle::local();

        // Optimistically assume that the handle is readable and writable.
        // Reads and writes will be attempted before queueing a packet.
        // This makes handles slightly faster to read/write the first time
        // they're accessed after being created, provided they start off as
        // readable or writable. In return, there will be an extra wasted
        // syscall per read/write if the handle is not readable or writable.
        let receiver = ehandle.register_receiver(Arc::new(SocketPacketReceiver::new(
            AtomicU32::new(Signals::SOCKET_READABLE.bits() | Signals::SOCKET_WRITABLE.bits()),
            AtomicWaker::new(),
            AtomicWaker::new(),
        )));

        let socket = Self { handle, receiver };

        // Make sure we get notifications when the handle closes.
        socket.schedule_packet(Signals::SOCKET_PEER_CLOSED)?;

        Ok(socket)
    }

    /// Return the underlying Zircon object.
    pub fn into_zx_socket(self) -> zx::Socket {
        self.handle
    }

    /// Returns true if the socket currently has a closed signal set.
    pub fn is_closed(&self) -> bool {
        let signals = zx::Signals::from_bits_truncate(self.receiver.signals.load(Ordering::SeqCst));
        signals.contains(zx::Signals::OBJECT_PEER_CLOSED)
    }

    /// Tests if the resource currently has either the provided `signal`
    /// or the `OBJECT_PEER_CLOSED` signal set.
    fn poll_signal_or_closed(
        &self,
        cx: &mut Context<'_>,
        task: &AtomicWaker,
        signal: zx::Signals,
    ) -> Poll<Result<zx::Signals, zx::Status>> {
        let signals = zx::Signals::from_bits_truncate(self.receiver.signals.load(Ordering::SeqCst));
        let was_closed = signals.contains(zx::Signals::OBJECT_PEER_CLOSED);
        let was_signal = signals.contains(signal);
        if was_closed || was_signal {
            let mask = signal | Signals::OBJECT_PEER_CLOSED;
            Poll::Ready(Ok(signals & mask))
        } else {
            self.need_signal(cx, task, signal)?;
            Poll::Pending
        }
    }

    /// Clear the socket readable signal, and query it again. Can be used to ensure that at least
    /// one poll_read_task invoked after this has an accurate signal.
    ///
    /// Note that this will result in extra traffic on the signal port. Most users should just
    /// allow Read to fail.
    pub fn reacquire_read_signal(&self) -> Result<(), zx::Status> {
        self.reacquire_signal(Signals::SOCKET_READABLE)
    }

    /// Test whether this socket is ready to be read or not.
    ///
    /// If the socket is *not* readable then the current task is scheduled to
    /// get a notification when the socket does become readable. That is, this
    /// is only suitable for calling in a `Future::poll` method and will
    /// automatically handle ensuring a retry once the socket is readable again.
    ///
    /// Returns the cached signals, masked to `OBJECT_READABLE` and
    /// `OBJECT_PEER_CLOSED`. If the read syscall returns `SHOULD_WAIT`, you
    /// must call `need_read` to clear the cached state and wait for the socket
    /// to become readable again.
    pub fn poll_read_task(&self, cx: &mut Context<'_>) -> Poll<Result<zx::Signals, zx::Status>> {
        self.poll_signal_or_closed(cx, &self.receiver.read_task, Signals::SOCKET_READABLE)
    }

    /// Clear the socket writable signal, and query it again. Can be used to ensure that at least
    /// one poll_write_task invoked after this has an accurate signal.
    ///
    /// Note that this will result in extra traffic on the signal port. Most users should just
    /// allow Write to fail.
    pub fn reacquire_write_signal(&self) -> Result<(), zx::Status> {
        self.reacquire_signal(Signals::SOCKET_WRITABLE)
    }

    /// Test whether this socket is ready to be written to or not.
    ///
    /// If the socket is *not* writable then the current task is scheduled to
    /// get a notification when the socket does become writable. That is, this
    /// is only suitable for calling in a `Future::poll` method and will
    /// automatically handle ensuring a retry once the socket is writable again.
    ///
    /// Returns the cached signals, masked to `OBJECT_WRITABLE` and
    /// `OBJECT_PEER_CLOSED`. If the write syscall returns `SHOULD_WAIT`, you
    /// must call `need_write` to clear the cached state and wait for the socket
    /// to become writable again.
    pub fn poll_write_task(&self, cx: &mut Context<'_>) -> Poll<Result<zx::Signals, zx::Status>> {
        self.poll_signal_or_closed(cx, &self.receiver.write_task, Signals::SOCKET_WRITABLE)
    }

    fn need_signal(
        &self,
        cx: &mut Context<'_>,
        task: &AtomicWaker,
        signal: zx::Signals,
    ) -> Result<(), zx::Status> {
        crate::runtime::need_signal(
            cx,
            task,
            &self.receiver.signals,
            signal,
            self.handle.as_handle_ref(),
            self.receiver.port(),
            self.receiver.key(),
        )
    }

    /// Arranges for the current task to receive a notification when a
    /// "readable" signal arrives.
    pub fn need_read(&self, cx: &mut Context<'_>) -> Result<(), zx::Status> {
        self.need_signal(cx, &self.receiver.read_task, Signals::SOCKET_READABLE)
    }

    /// Arranges for the current task to receive a notification when a
    /// "writable" signal arrives.
    pub fn need_write(&self, cx: &mut Context<'_>) -> Result<(), zx::Status> {
        self.need_signal(cx, &self.receiver.write_task, Signals::SOCKET_WRITABLE)
    }

    // Clears a signal, and schedules a packet requesting an updated signal. Does nothing if
    // there is already a pending packet querying that signal.
    fn reacquire_signal(&self, signal: zx::Signals) -> Result<(), zx::Status> {
        let old = zx::Signals::from_bits_truncate(
            self.receiver.signals.fetch_and(!signal.bits(), Ordering::SeqCst),
        );

        // If false, this was already scheduled and fresh signals are already being queried.
        if old.contains(signal) {
            self.schedule_packet(signal)
        } else {
            Ok(())
        }
    }

    fn schedule_packet(&self, signals: Signals) -> Result<(), zx::Status> {
        crate::runtime::schedule_packet(
            self.handle.as_handle_ref(),
            self.receiver.port(),
            self.receiver.key(),
            signals,
        )
    }

    /// Attempt to read from the socket, registering for wakeup if the socket doesn't have any
    /// contents available. Used internally in the `AsyncRead` implementation, exposed for users
    /// who know the concrete type they're using and don't want to pin the socket.
    pub fn poll_read_ref(
        &self,
        cx: &mut Context<'_>,
        buf: &mut [u8],
    ) -> Poll<Result<usize, zx::Status>> {
        ready!(self.poll_read_task(cx))?;
        let res = self.handle.read(buf);
        if res == Err(zx::Status::SHOULD_WAIT) {
            self.need_read(cx)?;
            return Poll::Pending;
        }
        if res == Err(zx::Status::PEER_CLOSED) {
            return Poll::Ready(Ok(0));
        }
        Poll::Ready(res)
    }

    /// Attempt to write into the socket, registering for wakeup if the socket is not ready. Used
    /// internally in the `AsyncWrite` implementation, exposed for users who know the concrete type
    /// they're using and don't want to pin the socket.
    pub fn poll_write_ref(
        &self,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<Result<usize, zx::Status>> {
        ready!(self.poll_write_task(cx))?;
        let res = self.handle.write(buf);
        if res == Err(zx::Status::SHOULD_WAIT) {
            self.need_write(cx)?;
            Poll::Pending
        } else {
            Poll::Ready(res)
        }
    }

    /// Polls for the next data on the socket, appending it to the end of |out| if it has arrived.
    /// Not very useful for a non-datagram socket as it will return all available data
    /// on the socket.
    pub fn poll_datagram(
        &self,
        cx: &mut Context<'_>,
        out: &mut Vec<u8>,
    ) -> Poll<Result<usize, zx::Status>> {
        ready!(self.poll_read_task(cx))?;
        let avail = self.handle.outstanding_read_bytes()?;
        let len = out.len();
        out.resize(len + avail, 0);
        let (_, mut tail) = out.split_at_mut(len);
        match self.handle.read(&mut tail) {
            Err(zx::Status::SHOULD_WAIT) => {
                self.need_read(cx)?;
                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
            Ok(bytes) => {
                if bytes == avail {
                    Poll::Ready(Ok(bytes))
                } else {
                    Poll::Ready(Err(zx::Status::BAD_STATE))
                }
            }
        }
    }

    /// Reads the next datagram that becomes available onto the end of |out|.  Note: Using this
    /// multiple times concurrently is an error and the first one will never complete.
    pub async fn read_datagram<'a>(&'a self, out: &'a mut Vec<u8>) -> Result<usize, zx::Status> {
        poll_fn(move |cx| self.poll_datagram(cx, out)).await
    }

    /// Use this socket as a stream of `Result<Vec<u8>, zx::Status>` datagrams.
    ///
    /// Note: multiple concurrent streams from the same socket are not supported.
    pub fn as_datagram_stream<'a>(&'a self) -> DatagramStream<&'a Self> {
        DatagramStream(self)
    }

    /// Convert this socket into a stream of `Result<Vec<u8>, zx::Status>` datagrams.
    pub fn into_datagram_stream(self) -> DatagramStream<Self> {
        DatagramStream(self)
    }
}

impl fmt::Debug for Socket {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.handle.fmt(f)
    }
}

impl AsyncRead for Socket {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut [u8],
    ) -> Poll<io::Result<usize>> {
        self.poll_read_ref(cx, buf).map_err(Into::into)
    }
}

impl AsyncWrite for Socket {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        self.poll_write_ref(cx, buf).map_err(Into::into)
    }

    fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }

    fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }
}

impl<'a> AsyncRead for &'a Socket {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &mut [u8],
    ) -> Poll<io::Result<usize>> {
        self.poll_read_ref(cx, buf).map_err(Into::into)
    }
}

impl<'a> AsyncWrite for &'a Socket {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        self.poll_write_ref(cx, buf).map_err(Into::into)
    }

    fn poll_flush(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }

    fn poll_close(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }
}

/// A datagram stream from a `Socket`.
#[derive(Debug)]
pub struct DatagramStream<S>(pub S);

fn poll_datagram_as_stream(
    socket: &Socket,
    cx: &mut Context<'_>,
) -> Poll<Option<Result<Vec<u8>, zx::Status>>> {
    let mut res = Vec::<u8>::new();
    Poll::Ready(match ready!(socket.poll_datagram(cx, &mut res)) {
        Ok(_size) => Some(Ok(res)),
        Err(zx::Status::PEER_CLOSED) => None,
        Err(e) => Some(Err(e)),
    })
}

impl Stream for DatagramStream<Socket> {
    type Item = Result<Vec<u8>, zx::Status>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        poll_datagram_as_stream(&self.0, cx)
    }
}

impl Stream for DatagramStream<&Socket> {
    type Item = Result<Vec<u8>, zx::Status>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        poll_datagram_as_stream(self.0, cx)
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{TestExecutor, Time, TimeoutExt, Timer},
        fuchsia_zircon::prelude::*,
        futures::{
            future::join,
            io::{AsyncReadExt as _, AsyncWriteExt as _},
            stream::TryStreamExt,
        },
    };

    #[test]
    fn can_read_write() {
        let mut exec = TestExecutor::new();
        let bytes = &[0, 1, 2, 3];

        let (tx, rx) = zx::Socket::create_stream();
        let (mut tx, mut rx) = (Socket::from_socket(tx).unwrap(), Socket::from_socket(rx).unwrap());

        let receive_future = async {
            let mut buf = vec![];
            rx.read_to_end(&mut buf).await.expect("reading socket");
            assert_eq!(&*buf, bytes);
        };

        // add a timeout to receiver so if test is broken it doesn't take forever
        // Note: if debugging a hang, you may want to lower the timeout to `300.millis()` to get
        // faster feedback. This is set to 10s rather than something shorter to avoid triggering
        // flakes if things happen to be slow.
        let receiver = receive_future.on_timeout(Time::after(10.seconds()), || panic!("timeout"));

        // Sends a message after the timeout has passed
        let sender = async move {
            Timer::new(Time::after(100.millis())).await;
            tx.write_all(bytes).await.expect("writing into socket");
            // close socket to signal no more bytes will be written
            drop(tx);
        };

        let done = join(receiver, sender);
        exec.run_singlethreaded(done);
    }

    #[test]
    fn can_read_datagram() {
        let mut exec = TestExecutor::new();

        let (one, two) = (&[0, 1], &[2, 3, 4, 5]);

        let (tx, rx) = zx::Socket::create_datagram();
        let rx = Socket::from_socket(rx).unwrap();

        let mut out = vec![50];

        assert!(tx.write(one).is_ok());
        assert!(tx.write(two).is_ok());

        let size = exec.run_singlethreaded(rx.read_datagram(&mut out));

        assert!(size.is_ok());
        assert_eq!(one.len(), size.unwrap());

        assert_eq!([50, 0, 1], out.as_slice());

        let size = exec.run_singlethreaded(rx.read_datagram(&mut out));

        assert!(size.is_ok());
        assert_eq!(two.len(), size.unwrap());

        assert_eq!([50, 0, 1, 2, 3, 4, 5], out.as_slice());
    }

    #[test]
    fn stream_datagram() {
        let mut exec = TestExecutor::new();

        let (tx, rx) = zx::Socket::create_datagram();
        let mut rx = Socket::from_socket(rx).unwrap().into_datagram_stream();

        let packets = 20;

        for size in 1..packets + 1 {
            let mut vec = Vec::<u8>::new();
            vec.resize(size, size as u8);
            assert!(tx.write(&vec).is_ok());
        }

        // Close the socket.
        drop(tx);

        let stream_read_fut = async move {
            let mut count = 0;
            while let Some(packet) = rx.try_next().await.expect("received error from stream") {
                count = count + 1;
                assert_eq!(packet.len(), count);
                assert!(packet.iter().all(|&x| x == count as u8));
            }
            assert_eq!(packets, count);
        };

        exec.run_singlethreaded(stream_read_fut);
    }

    #[test]
    fn peer_closed_signal_raised() {
        let mut executor = TestExecutor::new();

        let (s1, s2) = zx::Socket::create_stream();
        let async_s2 = Socket::from_socket(s2).expect("failed to create async socket");

        drop(s1);

        // Dropping s1 raises a closed signal on s2 when the executor next polls the signal port.
        let mut rx_fut = poll_fn(|cx| async_s2.poll_read_task(cx));
        if let Poll::Ready(Ok(signals)) = executor.run_until_stalled(&mut rx_fut) {
            assert!(signals.contains(zx::Signals::OBJECT_PEER_CLOSED));
        } else {
            panic!("Expected future to be ready and Ok");
        }
        assert!(async_s2.is_closed());
    }

    #[test]
    fn reacquiring_read_signal_ensures_freshness() {
        let mut executor = TestExecutor::new();

        let (s1, s2) = zx::Socket::create_stream();
        let async_s2 = Socket::from_socket(s2).expect("failed to create async socket");

        // The read signal is optimistically set on socket creation, so even though there is
        // nothing to read poll_read_task returns Ready.
        let mut rx_fut = poll_fn(|cx| async_s2.poll_read_task(cx));
        assert!(!executor.run_until_stalled(&mut rx_fut).is_pending());

        // Reacquire the read signal. The socket now knows that the signal is not actually set,
        // so returns Pending.
        async_s2.reacquire_read_signal().expect("failed to reacquire read signal");
        let mut rx_fut = poll_fn(|cx| async_s2.poll_read_task(cx));
        assert!(executor.run_until_stalled(&mut rx_fut).is_pending());

        assert_eq!(s1.write(b"hello!").expect("failed to write 6 bytes"), 6);

        // After writing to s1, its peer now has an actual read signal and is Ready.
        assert!(!executor.run_until_stalled(&mut rx_fut).is_pending());
    }

    #[test]
    fn reacquiring_write_signal_ensures_freshness() {
        let mut executor = TestExecutor::new();

        let (s1, s2) = zx::Socket::create_stream();

        // Completely fill the transmit buffer. This socket is no longer writable.
        let socket_info = s2.info().expect("failed to get socket info");
        let bytes = vec![0u8; socket_info.tx_buf_max];
        assert_eq!(socket_info.tx_buf_max, s2.write(&bytes).expect("failed to write to socket"));

        let async_s2 = Socket::from_socket(s2).expect("failed to create async rx socket");

        // The write signal is optimistically set on socket creation, so even though it's not
        // possible to write poll_write_task returns Ready.
        let mut tx_fut = poll_fn(|cx| async_s2.poll_write_task(cx));
        assert!(!executor.run_until_stalled(&mut tx_fut).is_pending());

        // Reacquire the write signal. The socket now knows that the signal is not actually set,
        // so returns Pending.
        async_s2.reacquire_write_signal().expect("failed to reacquire write signal");
        let mut tx_fut = poll_fn(|cx| async_s2.poll_write_task(cx));
        assert!(executor.run_until_stalled(&mut tx_fut).is_pending());

        let mut buffer = [0u8; 5];
        assert_eq!(s1.read(&mut buffer).expect("failed to read 5 bytes"), 5);

        // After reading from s1, its peer is now able to write and should have a write signal.
        assert!(!executor.run_until_stalled(&mut tx_fut).is_pending());
    }
}
