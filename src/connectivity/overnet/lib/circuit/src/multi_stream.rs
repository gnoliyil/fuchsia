// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::error::{Error, Result};
use crate::stream;
use crate::{Node, Quality};

use futures::channel::mpsc::{unbounded, UnboundedReceiver, UnboundedSender};
use futures::channel::oneshot;
use futures::future::Either;
use futures::prelude::*;
use futures::StreamExt;
use std::collections::HashMap;
use std::future::Future;
use std::sync::{Arc, Mutex as SyncMutex};

/// Status of an individual stream.
enum StreamStatus {
    /// Stream is open to traffic. Incoming traffic should be delivered to the given writer.
    Open(stream::Writer),
    /// Stream hasn't been officially closed but the user has stopped listening for incoming data.
    /// The outgoing side of the stream might still be running.
    ReadClosed,
    /// Stream is closed. We never reuse stream IDs so a closed stream should never reopen.
    Closed,
}

/// Multiplexes multiple streams into one.
///
/// While running, this function serves a protocol on the `reader` and `writer` that multiplexes
/// multiple streams into a single stream.
///
/// The protocol is a series of variable length frames each consisting of:
/// * 4 bytes - A stream ID in little-endian.
/// * 2 bytes - a LENGTH in little-endian.
/// * LENGTH bytes - data.
///
/// To start a new stream, an endpoint simply begins sending frames with that stream ID. To close a
/// stream, an endpoint can send a zero-length frame. Once a stream is closed that stream ID should
/// never be used again.
///
/// We distinguish the two endpoints of the protocol as a client and a server. The only difference
/// between the two is in which stream IDs they may initiate; Any new streams started by the client
/// should have odd-numbered IDs, and any new streams initiated by the server should have
/// even-numbered IDs. The `is_server` argument controls this behavior.
///
/// Any new streams initiated by the other end of the connection will be returned to us via the
/// `streams_out` channel, and we may initiate a stream by sending a reader and writer to the
/// `streams_in` channel.
pub async fn multi_stream(
    reader: stream::Reader,
    writer: stream::Writer,
    is_server: bool,
    streams_out: UnboundedSender<(stream::Reader, stream::Writer, oneshot::Sender<Result<()>>)>,
    streams_in: UnboundedReceiver<(stream::Reader, stream::Writer)>,
    stream_errors_out: UnboundedSender<Error>,
    remote_name: String,
) -> Result<()> {
    let (new_readers_sender, new_readers) = unbounded();
    let (stream_errors_sender, stream_errors) = unbounded();
    let first_stream_id = if is_server { 1 } else { 0 };
    let mut stream_ids = (first_stream_id..).step_by(2);

    let handle_read = async move {
        let streams = SyncMutex::new(HashMap::<u32, StreamStatus>::new());

        // Creates a stream (that's Stream in the rust async sense, not in the sense of our
        // protocol) which when polled will read some data from the reader and attempt to
        // demultiplex it.
        //
        // Each time we call `.next()` on this stream one read operation is performed, and the
        // stream yields a `Result` indicating if an error occurred. Therefore, to pump read-side
        // IO, we simply need to pull values from this stream until one of them is an error.
        let read_result_stream = futures::stream::unfold((), |_| async {
            let got = reader
                .read(6, |buf| {
                    let mut streams = streams.lock().unwrap();
                    let (size, new_stream) =
                        handle_one_chunk(&mut *streams, is_server, buf, &remote_name)?;

                    if let Some((id, first_chunk_data)) = new_stream {
                        streams_out
                            .unbounded_send(handle_new_stream(
                                &mut *streams,
                                id,
                                first_chunk_data,
                                &new_readers_sender,
                                &stream_errors_sender,
                            )?)
                            .map_err(|_| {
                                Error::ConnectionClosed(Some(
                                    "New stream handler disappeared".to_owned(),
                                ))
                            })?;
                    }

                    Ok(((), size))
                })
                .await;

            Some((got, ()))
        });

        // Send stream errors to our error output. This functions as a stream that never returns anything
        // so we can wrap it in a select with read_result_stream and thereby continuously poll it as
        // we handle errors.
        let stream_errors = stream_errors.filter_map(move |x| {
            let stream_errors_out = stream_errors_out.clone();
            async move {
                if let Err(x) = x.await.unwrap_or(Err(Error::ConnectionClosed(Some(
                    "Stream handler hung up without returning a status".to_owned(),
                )))) {
                    let _ = stream_errors_out.unbounded_send(x);
                }
                None
            }
        });

        let read_result_stream = futures::stream::select(stream_errors, read_result_stream);

        // The `futures::stream::select` function requires both of the streams you give it to be the
        // same type and effectively interleaves the output of the two streams. We want to poll two
        // streams at once but handle their output differently, so we wrap them in an `Either`.
        let read_result_stream = read_result_stream.map(Either::Left);

        // `streams_in` will yield any new streams the user wants to create. If the user hangs up
        // that channel, we don't need to react, as the existing streams may still be in use. So we
        // make streams_in poll forever once the user stops supplying new streams.
        let streams_in = streams_in.chain(futures::stream::pending()).map(Either::Right);

        let events = futures::stream::select(read_result_stream, streams_in);
        futures::pin_mut!(events);

        let mut ret = Ok(());
        while let Some(event) = events.next().await {
            match event {
                Either::Left(Ok(())) => (),
                Either::Left(other) => {
                    ret = other;
                    break;
                }
                Either::Right((reader, writer)) => {
                    let id = stream_ids.next().expect("This iterator should be infinite!");
                    if new_readers_sender.unbounded_send((id, reader)).is_err() {
                        break;
                    }
                    streams.lock().unwrap().insert(id, StreamStatus::Open(writer));
                }
            }
        }

        let mut streams = streams.lock().unwrap();

        for (_, stream) in streams.drain() {
            if let StreamStatus::Open(writer) = stream {
                writer.close(format!("Multi-stream terminated: {ret:?}"));
            }
        }

        ret
    };

    let writer = Arc::new(SyncMutex::new(writer));
    let handle_write = new_readers.for_each_concurrent(None, move |(id, stream)| {
        let writer = Arc::clone(&writer);
        async move {
            let res = write_as_chunks(id, &stream, writer).await;
            stream.close(format!("Stream terminated: {res:?}"));
            res
        }
    });

    futures::pin_mut!(handle_read);
    futures::pin_mut!(handle_write);

    match futures::future::select(handle_read, handle_write).await {
        Either::Left((res, _)) => res,
        Either::Right(((), handle_read)) => handle_read.await,
    }
}

/// Handle chunk data for a new stream being initiated by the other end of the connection.
///
/// We take the `id` of the stream, and whatever data was sent with the initial message in
/// `first_chunk_data`. We return a `Reader` and `Writer` that will get the de-multiplexed data for
/// that stream, as well as a oneshot sender which takes an error, and can be used if the back end
/// driving us has trouble setting up the new stream.
///
/// The oneshot receiver which will get the error once sent is handed off to `stream_errors_sender`,
/// and the reader which feeds the returned writer is handed off to `new_readers_sender`. The writer
/// which handles the returned reader is stored in the `streams` table.
fn handle_new_stream(
    streams: &mut HashMap<u32, StreamStatus>,
    id: u32,
    first_chunk_data: &[u8],
    new_readers_sender: &UnboundedSender<(u32, stream::Reader)>,
    stream_errors_sender: &UnboundedSender<oneshot::Receiver<Result<()>>>,
) -> Result<(stream::Reader, stream::Writer, oneshot::Sender<Result<()>>)> {
    let (reader, remote_writer) = stream::stream();
    let (remote_reader, writer) = stream::stream();

    remote_writer
        .write(first_chunk_data.len(), |out| {
            out[..first_chunk_data.len()].copy_from_slice(first_chunk_data);
            Ok(first_chunk_data.len())
        })
        .expect("We just created this stream!");
    new_readers_sender.unbounded_send((id, remote_reader)).map_err(|_| {
        Error::ConnectionClosed(Some("New stream reader handler disappeared".to_owned()))
    })?;
    let (sender, receiver) = oneshot::channel();
    streams.insert(id, StreamStatus::Open(remote_writer));
    if stream_errors_sender.unbounded_send(receiver).is_ok() {
        Ok((reader, writer, sender))
    } else {
        Err(Error::ConnectionClosed(Some("Error reporting channel closed".to_owned())))
    }
}

/// Handles one chunk of data from the incoming stream.
///
/// The incoming stream is an interleaving of several byte streams. They are interleaved by
/// splitting them into chunks and attaching a header to each. This function assumes `buf` has been
/// filled with data from the incoming stream, and tries to parse the header for the first chunk and
/// process the data within.
///
/// If the buffer isn't long enough to contain an entire chunk, this will return `BufferTooShort`,
/// otherwise it will return the size of the chunk processed in the first element of the returned
/// tuple.
///
/// Once a chunk is parsed, the table of individual streams given with the `streams` argument will
/// be used to route the incoming data. The second element of the returned tuple will be `Some` if
/// and only if the chunk indicates a new stream is being started, in which case it will contain the
/// ID of the new stream, and the data portion of the chunk.
fn handle_one_chunk<'a>(
    streams: &mut HashMap<u32, StreamStatus>,
    is_server: bool,
    buf: &'a [u8],
    remote_name: &str,
) -> Result<(usize, Option<(u32, &'a [u8])>)> {
    if buf.len() < 6 {
        return Err(Error::BufferTooShort(6));
    }

    let id = u32::from_le_bytes(buf[..4].try_into().unwrap());
    let length = u16::from_le_bytes(buf[4..6].try_into().unwrap());

    let length = length as usize;
    let chunk_length = length + 6;
    let buf = &buf[6..];

    if length == 0 {
        if buf.len() < 2 {
            return Err(Error::BufferTooShort(8));
        }
        let length = u16::from_le_bytes(buf[..2].try_into().unwrap());
        let length = length as usize;

        if buf.len() < length + 2 {
            return Err(Error::BufferTooShort(8 + length));
        }

        let epitaph =
            if length > 0 { Some(String::from_utf8_lossy(&buf[2..][..length])) } else { None };

        if let Some(old) = streams.insert(id, StreamStatus::Closed) {
            if let StreamStatus::Open(old) = old {
                if let Some(epitaph) = epitaph {
                    old.close(format!("{remote_name} reported: {epitaph}"));
                } else {
                    old.close(format!("{remote_name} reported no epitaph"));
                }
            }
            Ok((length + 8, None))
        } else {
            Err(Error::BadStreamId)
        }
    } else if buf.len() < length {
        Err(Error::BufferTooShort(chunk_length))
    } else if let Some(stream) = streams.get(&id) {
        match stream {
            StreamStatus::Open(stream) => {
                if stream
                    .write(length, |out| {
                        out[..length].copy_from_slice(&buf[..length]);
                        Ok(length)
                    })
                    .is_err()
                {
                    // The user isn't listening for incoming data anymore. Don't
                    // treat that as a fatal error, and don't send a hangup in case
                    // the user is still sending data. Just quietly ignore it. If
                    // the user hangs up the other side of the connection that's
                    // when we can complain.
                    let _ = streams.insert(id, StreamStatus::ReadClosed);
                }
                Ok((chunk_length, None))
            }
            StreamStatus::ReadClosed => Ok((chunk_length, None)),
            StreamStatus::Closed => Err(Error::BadStreamId),
        }
    } else if (id & 1 == 0) == is_server {
        Ok((chunk_length, Some((id, &buf[..length]))))
    } else {
        Err(Error::BadStreamId)
    }
}

/// Reads data from the given reader, then splits it into chunks of no more than 2^16 - 1 bytes,
/// attaches a header to each chunk containing the given `id` number and the size of the chunk, then
/// writes each chunk to the given writer.
///
/// The point, of course, is that multiple functions can do this to the same writer, and since the
/// chunks are labeled, the data can be parsed back out into separate streams on the other end.
async fn write_as_chunks(id: u32, reader: &stream::Reader, writer: Arc<SyncMutex<stream::Writer>>) {
    loop {
        // We want to handle errors with the read and errors with the write differently, so we
        // return a nested result.
        //
        // In short, this will return Ok(Ok(())) if all is well, Ok(Err(...)) if we read data
        // successfully but then failed to write it, and Err(...) if we failed to read.
        let got = reader
            .read(1, |buf| {
                let mut total_len = 0;
                while buf.len() > total_len {
                    let buf = &buf[total_len..];
                    let buf =
                        if buf.len() > u16::MAX as usize { &buf[..u16::MAX as usize] } else { buf };

                    let len: u16 = buf
                        .len()
                        .try_into()
                        .expect("We just truncated the length so it would fit!");

                    if let e @ Err(_) = writer.lock().unwrap().write(6 + buf.len(), |out_buf| {
                        out_buf[..4].copy_from_slice(&id.to_le_bytes());
                        let out_buf = &mut out_buf[4..];
                        out_buf[..2].copy_from_slice(&len.to_le_bytes());
                        out_buf[2..][..buf.len()].copy_from_slice(buf);
                        Ok(buf.len() + 6)
                    }) {
                        return Ok((e, total_len));
                    } else {
                        total_len += buf.len();
                    }
                }

                Ok((Ok(()), total_len))
            })
            .await;

        match got {
            Err(Error::ConnectionClosed(epitaph)) => {
                let epitaph = epitaph.as_ref().map(|x| x.as_bytes()).unwrap_or(b"");
                let length_u16: u16 = epitaph.len().try_into().unwrap_or(u16::MAX);
                let length = length_u16 as usize;

                // If the stream was closed, send a frame indicating such.
                let write_result = writer.lock().unwrap().write(8 + length, |out_buf| {
                    out_buf[..4].copy_from_slice(&id.to_le_bytes());
                    let out_buf = &mut out_buf[4..];
                    out_buf[..2].copy_from_slice(&0u16.to_le_bytes());
                    let out_buf = &mut out_buf[2..];
                    out_buf[..2].copy_from_slice(&length_u16.to_le_bytes());
                    let out_buf = &mut out_buf[2..];
                    out_buf[..length].copy_from_slice(&epitaph[..length]);
                    Ok(8 + length)
                });

                match write_result {
                    Ok(()) | Err(Error::ConnectionClosed(_)) => break,
                    other => unreachable!("Unexpected write error: {other:?}"),
                }
            }
            Ok(Ok(())) => (),
            Ok(Err(Error::ConnectionClosed(_))) => break,
            Ok(other) => unreachable!("Unexpected write error: {other:?}"),
            other => unreachable!("Unexpected read error: {other:?}"),
        }
    }
}

/// Creates a new connection to a circuit node, and merges all streams produced and consumed by that
/// connection into a multi-stream. In this way you can service a connection between nodes with a
/// single stream of bytes.
///
/// The `is_server` boolean should be `true` at one end of the connection and `false` at the other.
/// Usually it will be `true` for the node receiving a connection and `false` for the node
/// initiating one.
///
/// Traffic will be written to, and read from, the given `reader` and `writer`.
///
/// The `quality` will be used to make routing decisions when establishing streams across multiple
/// nodes.
pub fn multi_stream_node_connection(
    node: &Node,
    reader: stream::Reader,
    writer: stream::Writer,
    is_server: bool,
    quality: Quality,
    stream_errors_out: UnboundedSender<Error>,
    remote_name: String,
) -> impl Future<Output = Result<()>> + Send {
    let (new_stream_sender, streams_in) = unbounded();
    let (streams_out, new_stream_receiver) = unbounded();

    let control_stream = if is_server {
        let (control_reader, control_writer_remote) = stream::stream();
        let (control_reader_remote, control_writer) = stream::stream();

        new_stream_sender
            .unbounded_send((control_reader_remote, control_writer_remote))
            .expect("We just created this channel!");
        Some((control_reader, control_writer))
    } else {
        None
    };

    let stream_fut = multi_stream(
        reader,
        writer,
        is_server,
        streams_out,
        streams_in,
        stream_errors_out,
        remote_name,
    );
    let node_fut = node.link_node(control_stream, new_stream_sender, new_stream_receiver, quality);

    async move {
        futures::pin_mut!(node_fut);
        futures::pin_mut!(stream_fut);

        // If either the node connection or the multi stream dies we assume the other will also die
        // shortly after, so we always await both futures to completion.
        match futures::future::select(node_fut, stream_fut).await {
            Either::Left((res, stream_fut)) => res.and(stream_fut.await),
            Either::Right((res, node_fut)) => res.and(node_fut.await),
        }
    }
}

/// Same as `multi_stream_node_connection` but reads and writes to and from implementors of the
/// standard `AsyncRead` and `AsyncWrite` traits rather than circuit streams.
pub async fn multi_stream_node_connection_to_async(
    node: &Node,
    rx: &mut (dyn AsyncRead + Unpin + Send),
    tx: &mut (dyn AsyncWrite + Unpin + Send),
    is_server: bool,
    quality: Quality,
    stream_errors_out: UnboundedSender<Error>,
    remote_name: String,
) -> Result<()> {
    let (reader, remote_writer) = stream::stream();
    let (remote_reader, writer) = stream::stream();
    let conn_fut = multi_stream_node_connection(
        node,
        remote_reader,
        remote_writer,
        is_server,
        quality,
        stream_errors_out,
        remote_name.clone(),
    );
    let remote_name = &remote_name;

    let read_fut = async move {
        let mut buf = [0u8; 4096];
        loop {
            let n = match rx.read(&mut buf).await {
                Ok(0) => {
                    writer.close(format!("{remote_name} closed the connection"));
                    break Ok(());
                }
                Ok(n) => n,
                Err(e) => {
                    writer.close(format!("{remote_name} connection failed (read): {e:?}"));
                    return Err(Error::from(e));
                }
            };
            writer.write(n, |write_buf| {
                write_buf[..n].copy_from_slice(&buf[..n]);
                Ok(n)
            })?
        }
    };

    let write_fut = async move {
        loop {
            let mut buf = [0u8; 4096];
            let len = reader
                .read(1, |read_buf| {
                    let read_buf = &read_buf[..std::cmp::min(buf.len(), read_buf.len())];
                    buf[..read_buf.len()].copy_from_slice(read_buf);
                    Ok((read_buf.len(), read_buf.len()))
                })
                .await?;
            let write_res = async {
                tx.write_all(&buf[..len]).await?;
                tx.flush().await?;
                Result::<_, Error>::Ok(())
            }
            .await;

            if let Err(e) = write_res {
                reader.close(format!("{remote_name} connection failed (write): {e:?}"));
                return Err(e.into());
            }
        }
    };

    futures::future::try_join3(read_fut, write_fut, conn_fut)
        .await
        .map(|((), (), ())| ())
        .map_err(Into::into)
}

#[cfg(test)]
mod test {
    use super::*;
    use fuchsia_async as fasync;

    #[fuchsia::test]
    async fn one_stream() {
        let (a_reader, b_writer) = stream::stream();
        let (b_reader, a_writer) = stream::stream();
        let (create_stream_a, a_streams_in) = unbounded();
        let (_create_stream_b, b_streams_in) = unbounded();
        let (a_streams_out, _get_stream_a) = unbounded();
        let (b_streams_out, mut get_stream_b) = unbounded();
        // Connection closure errors are very timing-dependent so we'll tend to be flaky if we
        // observe them in a test.
        let (errors_sink_a, _black_hole) = unbounded();
        let errors_sink_b = errors_sink_a.clone();

        let _a = fasync::Task::spawn(async move {
            assert!(matches!(
                multi_stream(
                    a_reader,
                    a_writer,
                    true,
                    a_streams_out,
                    a_streams_in,
                    errors_sink_a,
                    "b".to_owned()
                )
                .await,
                Ok(()) | Err(Error::ConnectionClosed(None))
            ))
        });
        let _b = fasync::Task::spawn(async move {
            assert!(matches!(
                multi_stream(
                    b_reader,
                    b_writer,
                    false,
                    b_streams_out,
                    b_streams_in,
                    errors_sink_b,
                    "a".to_owned()
                )
                .await,
                Ok(()) | Err(Error::ConnectionClosed(None))
            ))
        });

        let (ab_reader_a, ab_reader_write) = stream::stream();
        let (ab_writer_read, ab_writer_a) = stream::stream();

        create_stream_a.unbounded_send((ab_writer_read, ab_reader_write)).unwrap();

        ab_writer_a
            .write(8, |buf| {
                buf[..8].copy_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);
                Ok(8)
            })
            .unwrap();

        let (ab_reader_b, ab_writer_b, err) = get_stream_b.next().await.unwrap();
        err.send(Ok(())).unwrap();

        ab_writer_b
            .write(8, |buf| {
                buf[..8].copy_from_slice(&[9, 10, 11, 12, 13, 14, 15, 16]);
                Ok(8)
            })
            .unwrap();

        ab_reader_b
            .read(8, |buf| {
                assert_eq!(&buf[..8], &[1, 2, 3, 4, 5, 6, 7, 8]);
                Ok(((), 8))
            })
            .await
            .unwrap();
        ab_reader_a
            .read(8, |buf| {
                assert_eq!(&buf[..8], &[9, 10, 11, 12, 13, 14, 15, 16]);
                Ok(((), 8))
            })
            .await
            .unwrap();

        std::mem::drop(ab_writer_b);
        assert!(matches!(
            ab_reader_a.read::<_, ()>(1, |_| unreachable!()).await,
            Err(Error::ConnectionClosed(_))
        ));

        std::mem::drop(ab_reader_b);
        ab_writer_a
            .write(8, |buf| {
                buf[..8].copy_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);
                Ok(8)
            })
            .unwrap();
    }

    #[fuchsia::test]
    async fn fallible_stream() {
        let (a_reader, b_writer) = stream::stream();
        let (b_reader, a_writer) = stream::stream();
        let (create_stream_a, a_streams_in) = unbounded();
        let (_create_stream_b, b_streams_in) = unbounded();
        let (a_streams_out, _get_stream_a) = unbounded();
        let (b_streams_out, mut get_stream_b) = unbounded();
        let (errors_sink_a, _black_hole) = unbounded();
        // Connection closure errors are very timing-dependent so we'll tend to be flaky if we
        // observe them in a test.
        let (errors_sink_b, mut b_errors) = unbounded();

        let _a = fasync::Task::spawn(async move {
            assert!(matches!(
                multi_stream(
                    a_reader,
                    a_writer,
                    true,
                    a_streams_out,
                    a_streams_in,
                    errors_sink_a,
                    "b".to_owned()
                )
                .await,
                Ok(()) | Err(Error::ConnectionClosed(None))
            ))
        });
        let _b = fasync::Task::spawn(async move {
            assert!(matches!(
                multi_stream(
                    b_reader,
                    b_writer,
                    false,
                    b_streams_out,
                    b_streams_in,
                    errors_sink_b,
                    "a".to_owned()
                )
                .await,
                Ok(()) | Err(Error::ConnectionClosed(None))
            ))
        });

        // The first stream fails to be created.
        let (fail_reader, fail_reader_write) = stream::stream();
        let (_ignore, fail_writer) = stream::stream();
        create_stream_a.unbounded_send((fail_reader, fail_writer)).unwrap();

        // There's a laziness to stream creation in the protocol so we need to send a little data to
        // actually create the stream.
        fail_reader_write
            .write(8, |buf| {
                buf[..8].copy_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);
                Ok(8)
            })
            .unwrap();

        let (_, _, err) = get_stream_b.next().await.unwrap();
        err.send(Err(Error::ConnectionClosed(Some("Testing".to_owned())))).unwrap();

        loop {
            if let Some(Error::ConnectionClosed(Some(s))) = b_errors.next().await {
                if s == "Testing" {
                    break;
                }
            } else {
                panic!("Error stream closed without reporting our error.");
            }
        }

        let (ab_reader_a, ab_reader_write) = stream::stream();
        let (ab_writer_read, ab_writer_a) = stream::stream();

        create_stream_a.unbounded_send((ab_writer_read, ab_reader_write)).unwrap();

        ab_writer_a
            .write(8, |buf| {
                buf[..8].copy_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);
                Ok(8)
            })
            .unwrap();

        let (ab_reader_b, ab_writer_b, err) = get_stream_b.next().await.unwrap();
        err.send(Ok(())).unwrap();

        ab_writer_b
            .write(8, |buf| {
                buf[..8].copy_from_slice(&[9, 10, 11, 12, 13, 14, 15, 16]);
                Ok(8)
            })
            .unwrap();

        ab_reader_b
            .read(8, |buf| {
                assert_eq!(&buf[..8], &[1, 2, 3, 4, 5, 6, 7, 8]);
                Ok(((), 8))
            })
            .await
            .unwrap();
        ab_reader_a
            .read(8, |buf| {
                assert_eq!(&buf[..8], &[9, 10, 11, 12, 13, 14, 15, 16]);
                Ok(((), 8))
            })
            .await
            .unwrap();

        std::mem::drop(ab_writer_b);
        assert!(matches!(
            ab_reader_a.read::<_, ()>(1, |_| unreachable!()).await,
            Err(Error::ConnectionClosed(_))
        ));

        std::mem::drop(ab_reader_b);
        ab_writer_a
            .write(8, |buf| {
                buf[..8].copy_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);
                Ok(8)
            })
            .unwrap();
    }

    #[fuchsia::test]
    async fn two_streams() {
        let (a_reader, b_writer) = stream::stream();
        let (b_reader, a_writer) = stream::stream();
        let (create_stream_a, a_streams_in) = unbounded();
        let (create_stream_b, b_streams_in) = unbounded();
        let (a_streams_out, mut get_stream_a) = unbounded();
        let (b_streams_out, mut get_stream_b) = unbounded();
        // Connection closure errors are very timing-dependent so we'll tend to be flaky if we
        // observe them in a test.
        let (errors_sink, _black_hole) = unbounded();

        let _a = fasync::Task::spawn(multi_stream(
            a_reader,
            a_writer,
            true,
            a_streams_out,
            a_streams_in,
            errors_sink.clone(),
            "b".to_owned(),
        ));
        let _b = fasync::Task::spawn(multi_stream(
            b_reader,
            b_writer,
            false,
            b_streams_out,
            b_streams_in,
            errors_sink.clone(),
            "a".to_owned(),
        ));

        let (ab_reader_a, ab_reader_write) = stream::stream();
        let (ab_writer_read, ab_writer_a) = stream::stream();
        let (ba_reader_b, ba_reader_write) = stream::stream();
        let (ba_writer_read, ba_writer_b) = stream::stream();

        create_stream_a.unbounded_send((ab_writer_read, ab_reader_write)).unwrap();
        create_stream_b.unbounded_send((ba_writer_read, ba_reader_write)).unwrap();

        ab_writer_a
            .write(8, |buf| {
                buf[..8].copy_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);
                Ok(8)
            })
            .unwrap();
        ba_writer_b
            .write(8, |buf| {
                buf[..8].copy_from_slice(&[25, 26, 27, 28, 29, 30, 31, 32]);
                Ok(8)
            })
            .unwrap();

        let (ab_reader_b, ab_writer_b, err_ab) = get_stream_b.next().await.unwrap();
        let (ba_reader_a, ba_writer_a, err_ba) = get_stream_a.next().await.unwrap();
        err_ab.send(Ok(())).unwrap();
        err_ba.send(Ok(())).unwrap();

        ab_writer_b
            .write(8, |buf| {
                buf[..8].copy_from_slice(&[9, 10, 11, 12, 13, 14, 15, 16]);
                Ok(8)
            })
            .unwrap();
        ba_writer_a
            .write(8, |buf| {
                buf[..8].copy_from_slice(&[17, 18, 19, 20, 21, 22, 23, 24]);
                Ok(8)
            })
            .unwrap();

        ab_reader_b
            .read(8, |buf| {
                assert_eq!(&buf[..8], &[1, 2, 3, 4, 5, 6, 7, 8]);
                Ok(((), 8))
            })
            .await
            .unwrap();
        ab_reader_a
            .read(8, |buf| {
                assert_eq!(&buf[..8], &[9, 10, 11, 12, 13, 14, 15, 16]);
                Ok(((), 8))
            })
            .await
            .unwrap();
        ba_reader_b
            .read(8, |buf| {
                assert_eq!(&buf[..8], &[17, 18, 19, 20, 21, 22, 23, 24]);
                Ok(((), 8))
            })
            .await
            .unwrap();
        ba_reader_a
            .read(8, |buf| {
                assert_eq!(&buf[..8], &[25, 26, 27, 28, 29, 30, 31, 32]);
                Ok(((), 8))
            })
            .await
            .unwrap();
    }

    #[fuchsia::test]
    async fn node_connect() {
        let (new_peer_sender_a, mut new_peers) = unbounded();
        let (new_peer_sender_b, _new_peers_b) = unbounded();
        let (incoming_streams_sender_a, _streams_a) = unbounded();
        let (incoming_streams_sender_b, mut streams) = unbounded();
        let a = Node::new("a", "test", new_peer_sender_a, incoming_streams_sender_a).unwrap();
        let b = Node::new("b", "test", new_peer_sender_b, incoming_streams_sender_b).unwrap();
        // Connection closure errors are very timing-dependent so we'll tend to be flaky if we
        // observe them in a test.
        let (errors_sink, _black_hole) = unbounded();

        let (ab_reader, ab_writer) = stream::stream();
        let (ba_reader, ba_writer) = stream::stream();

        let _a_conn = fasync::Task::spawn(multi_stream_node_connection(
            &a,
            ba_reader,
            ab_writer,
            true,
            Quality::IN_PROCESS,
            errors_sink.clone(),
            "b".to_owned(),
        ));
        let _b_conn = fasync::Task::spawn(multi_stream_node_connection(
            &b,
            ab_reader,
            ba_writer,
            false,
            Quality::IN_PROCESS,
            errors_sink.clone(),
            "a".to_owned(),
        ));

        let new_peer = new_peers.next().await.unwrap();
        assert_eq!("b", &new_peer);

        let (_reader, peer_writer) = stream::stream();
        let (peer_reader, writer) = stream::stream();
        a.connect_to_peer(peer_reader, peer_writer, "b").await.unwrap();

        writer
            .write(8, |buf| {
                buf[..8].copy_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8]);
                Ok(8)
            })
            .unwrap();

        let (reader, _writer, from) = streams.next().await.unwrap();
        assert_eq!("a", &from);

        reader
            .read(8, |buf| {
                assert_eq!(&[1, 2, 3, 4, 5, 6, 7, 8], &buf);
                Ok(((), 8))
            })
            .await
            .unwrap();
    }
}
