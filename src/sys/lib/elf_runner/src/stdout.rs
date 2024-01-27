// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::config::StreamSink,
    anyhow::{anyhow, Error},
    async_trait::async_trait,
    cm_logger::scoped::ScopedLogger,
    fidl::prelude::*,
    fidl_fuchsia_logger::LogSinkMarker,
    fidl_fuchsia_process as fproc,
    fuchsia_async::{self as fasync, ReadableHandle as _, ReadableState},
    fuchsia_runtime::{HandleInfo, HandleType},
    fuchsia_zircon as zx,
    futures::{Stream, StreamExt},
    once_cell::unsync::OnceCell,
    runner::component::ComponentNamespace,
    std::{
        boxed::Box,
        pin::Pin,
        sync::Arc,
        task::{Context, Poll},
    },
    tracing::{info, warn, Subscriber},
    zx::HandleBased,
};

const STDOUT_FD: i32 = 1;
const STDERR_FD: i32 = 2;
const SVC_DIRECTORY_NAME: &str = "/svc";
const SYSLOG_PROTOCOL_NAME: &str = LogSinkMarker::PROTOCOL_NAME;
const NEWLINE: u8 = b'\n';

/// Max size for message when draining input stream socket. This number is
/// slightly smaller than size allowed by Archivist (LogSink service implementation).
const MAX_MESSAGE_SIZE: usize = 30720;

/// Bind stdout or stderr streams to syslog. This function binds either or both
/// output streams to syslog depending on value provided for each streams'
/// StreamSink. If the value for an output stream is set to StreamSink::Log,
/// that stream's file descriptor will be bound to syslog. All writes on that
// fd will be forwarded to syslog and will register as log entries. For stdout,
// the messages will be tagged with severity INFO. For stderr, the messages
// will be tagged with severity WARN. A task is created to listen to writes on
// the appropriate file descriptor and forward the message to syslog. This
// function returns both the task for each file descriptor and its
// corresponding HandleInfo.
pub fn bind_streams_to_syslog(
    ns: &ComponentNamespace,
    stdout_sink: StreamSink,
    stderr_sink: StreamSink,
) -> (Vec<fasync::Task<()>>, Vec<fproc::HandleInfo>) {
    let mut tasks: Vec<fasync::Task<()>> = Vec::new();
    let mut handles: Vec<fproc::HandleInfo> = Vec::new();

    // connect to the namespace's logger if we'll need it, wrap in OnceCell so we only do it once
    // (can't use Lazy here because we need to capture `ns`)
    let logger = OnceCell::new();
    let mut forward_stream = |sink, fd, level| {
        if matches!(sink, StreamSink::Log) {
            // create the handle before dealing with the logger so components still receive an inert
            // handle if connecting to LogSink fails
            let (socket, handle_info) = new_socket_bound_to_fd(fd);
            handles.push(handle_info);

            if let Some(l) = logger.get_or_init(|| create_namespace_logger(ns).map(Arc::new)) {
                tasks.push(forward_socket_to_syslog(l.clone(), socket, level));
            } else {
                warn!("Tried forwarding file descriptor {fd} but didn't have a LogSink available.");
            }
        }
    };

    forward_stream(stdout_sink, STDOUT_FD, OutputLevel::Info);
    forward_stream(stderr_sink, STDERR_FD, OutputLevel::Warn);

    (tasks, handles)
}

fn create_namespace_logger(ns: &ComponentNamespace) -> Option<ScopedLogger> {
    ns.items()
        .iter()
        .find(|(path, _)| path == SVC_DIRECTORY_NAME)
        .and_then(|(_, dir)| ScopedLogger::from_directory(&dir, SYSLOG_PROTOCOL_NAME).ok())
}

fn forward_socket_to_syslog(
    logger: Arc<ScopedLogger>,
    socket: zx::Socket,
    level: OutputLevel,
) -> fasync::Task<()> {
    let mut writer = SyslogWriter::new(logger, level);
    let task = fasync::Task::spawn(async move {
        if let Err(error) = drain_lines(socket, &mut writer).await {
            warn!(%error, "Draining output stream failed");
        }
    });

    task
}

fn new_socket_bound_to_fd(fd: i32) -> (zx::Socket, fproc::HandleInfo) {
    let (tx, rx) = zx::Socket::create_stream();

    (
        rx,
        fproc::HandleInfo {
            handle: tx.into_handle(),
            id: HandleInfo::new(HandleType::FileDescriptor, fd as u16).as_raw(),
        },
    )
}

/// Drains all bytes from socket and writes messages to writer. Bytes read
/// are split into lines and separated into chunks no greater than
/// MAX_MESSAGE_SIZE.
async fn drain_lines(socket: zx::Socket, writer: &mut dyn LogWriter) -> Result<(), Error> {
    let chunker = NewlineChunker::new(socket)?;
    futures::pin_mut!(chunker);

    while let Some(chunk_or_line) = chunker.next().await {
        writer.write(&chunk_or_line?).await?;
    }

    Ok(())
}

/// Splits the bytes from a streaming socket into newlines suitable for forwarding to LogSink.
/// Returned chunks may not be complete newlines if single lines are over the size limit for a log
/// message.
///
/// This implementation prioritizes standing memory usage over the number of copies or allocations
/// made. Log forwarding is not particularly throughput sensitive, but keeping around lots of large
/// buffers takes up memory.
struct NewlineChunker {
    socket: fasync::Socket,
    buffer: Vec<u8>,
    is_terminated: bool,
}

impl NewlineChunker {
    fn new(socket: zx::Socket) -> Result<Self, Error> {
        let socket = fasync::Socket::from_socket(socket)
            .map_err(|s| anyhow!("Failed to create fasync::socket from zx::socket: {}", s))?;
        Ok(Self { socket, buffer: vec![], is_terminated: false })
    }

    /// Removes and returns the next line or maximum-size chunk from the head of the buffer if
    /// available.
    fn next_chunk_from_buffer(&mut self) -> Option<Vec<u8>> {
        let new_tail_start =
            if let Some(mut newline_pos) = self.buffer.iter().position(|&b| b == NEWLINE) {
                // start the tail 1 past the last newline encountered
                while let Some(&NEWLINE) = self.buffer.get(newline_pos + 1) {
                    newline_pos += 1;
                }
                newline_pos + 1
            } else if self.buffer.len() >= MAX_MESSAGE_SIZE {
                // we have to check the length *after* looking for newlines in case a single socket
                // read was larger than the max size but contained newlines in the first
                // MAX_MESSAGE_SIZE bytes
                MAX_MESSAGE_SIZE
            } else {
                // no newlines, and the bytes in the buffer are too few to force chunking
                return None;
            };

        // the tail becomes the head for the next chunk
        let new_tail = self.buffer.split_off(new_tail_start);
        let mut next_chunk = std::mem::replace(&mut self.buffer, new_tail);

        // remove the newlines from the end of the chunk we're returning
        while let Some(&NEWLINE) = next_chunk.last() {
            next_chunk.pop();
        }
        Some(next_chunk)
    }

    fn end_of_stream(&mut self) -> Poll<Option<Result<Vec<u8>, Error>>> {
        if !self.buffer.is_empty() {
            // the buffer is under the forced chunk size because the first return didn't happen
            Poll::Ready(Some(Ok(std::mem::replace(&mut self.buffer, vec![]))))
        } else {
            // end the stream
            self.is_terminated = true;
            Poll::Ready(None)
        }
    }
}

impl Stream for NewlineChunker {
    type Item = Result<Vec<u8>, Error>;
    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let this = self.get_mut();

        if this.is_terminated {
            return Poll::Ready(None);
        }

        // first check to see if previous socket reads have left us with lines in the buffer
        if let Some(chunk) = this.next_chunk_from_buffer() {
            return Poll::Ready(Some(Ok(chunk)));
        }

        // we don't have a chunk to return, poll for reading the socket
        match futures::ready!(this.socket.poll_readable(cx))? {
            ReadableState::Closed => return this.end_of_stream(),
            ReadableState::Readable | ReadableState::ReadableAndClosed => {}
        }

        // find out how much buffer we should make available
        let bytes_in_socket = this.socket.as_ref().outstanding_read_bytes()?;
        if bytes_in_socket == 0 {
            // if there are no bytes available this socket should not be considered readable
            this.socket.need_readable(cx)?;
            return Poll::Pending;
        }

        // don't make the buffer bigger than necessary to get a chunk out
        let bytes_to_read = std::cmp::min(bytes_in_socket, MAX_MESSAGE_SIZE);
        let prev_len = this.buffer.len();

        // grow the size of the buffer to make space for the pending read, if it fails we'll need
        // to shrink it back down before any subsequent calls to poll_next
        this.buffer.resize(prev_len + bytes_to_read, 0);

        let bytes_read = match this.socket.as_ref().read(&mut this.buffer[prev_len..]) {
            Ok(b) => b,
            Err(zx::Status::PEER_CLOSED) => return this.end_of_stream(),
            Err(zx::Status::SHOULD_WAIT) => {
                // reset the size of the buffer to exclude the 0's we wrote above
                this.buffer.truncate(prev_len);
                return Poll::Ready(Some(Err(anyhow!(
                    "Got SHOULD_WAIT from socket read after confirming outstanding_read_bytes > 0"
                ))));
            }
            Err(e) => {
                // reset the size of the buffer to exclude the 0's we wrote above
                this.buffer.truncate(prev_len);
                return Poll::Ready(Some(Err(e.into())));
            }
        };

        // handle possible short reads
        this.buffer.truncate(prev_len + bytes_read);

        // we got something out of the socket
        if let Some(chunk) = this.next_chunk_from_buffer() {
            // and its enough for a chunk
            Poll::Ready(Some(Ok(chunk)))
        } else {
            // it is not enough for a chunk, request notification when there's more
            this.socket.need_readable(cx)?;
            Poll::Pending
        }
    }
}

/// Object capable of writing a stream of bytes.
#[async_trait]
trait LogWriter: Send {
    async fn write(&mut self, bytes: &[u8]) -> Result<(), Error>;
}

struct SyslogWriter {
    logger: Arc<dyn Subscriber + Send + Sync>,
    level: OutputLevel,
}

enum OutputLevel {
    Info,
    Warn,
}

impl SyslogWriter {
    fn new(logger: Arc<dyn Subscriber + Send + Sync>, level: OutputLevel) -> Self {
        Self { logger, level }
    }
}

#[async_trait]
impl LogWriter for SyslogWriter {
    async fn write(&mut self, bytes: &[u8]) -> Result<(), Error> {
        let msg = String::from_utf8_lossy(&bytes);
        tracing::subscriber::with_default(self.logger.clone(), || match self.level {
            OutputLevel::Info => info!("{}", msg),
            OutputLevel::Warn => warn!("{}", msg),
        });
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::tests::{create_fs_with_mock_logsink, MockServiceFs, MockServiceRequest},
        anyhow::{anyhow, format_err, Context, Error},
        async_trait::async_trait,
        diagnostics_message::MonikerWithUrl,
        fidl_fuchsia_component_runner as fcrunner,
        fidl_fuchsia_logger::LogSinkRequest,
        fuchsia_async::Task,
        fuchsia_zircon as zx,
        futures::{channel::mpsc, try_join, FutureExt, SinkExt, StreamExt},
        rand::{
            distributions::{Alphanumeric, DistString as _},
            thread_rng,
        },
        std::{
            convert::TryFrom,
            sync::{Arc, Mutex},
        },
    };

    #[async_trait]
    impl LogWriter for mpsc::Sender<String> {
        async fn write(&mut self, bytes: &[u8]) -> Result<(), Error> {
            let message =
                std::str::from_utf8(&bytes).expect("Failed to decode bytes to utf8.").to_owned();
            let () =
                self.send(message).await.expect("Failed to send message to other end of mpsc.");
            Ok(())
        }
    }

    #[fuchsia::test]
    async fn syslog_writer_decodes_valid_utf8_message() -> Result<(), Error> {
        let (dir, ns_entries) = create_fs_with_mock_logsink()?;

        let ((), actual) = try_join!(
            write_to_syslog_or_panic(ns_entries, b"Hello World!"),
            read_message_from_syslog(dir)
        )?;

        assert_eq!(actual, Some("Hello World!".to_owned()));
        Ok(())
    }

    #[fuchsia::test]
    async fn syslog_writer_decodes_non_utf8_message() -> Result<(), Error> {
        let (dir, ns_entries) = create_fs_with_mock_logsink()?;

        let ((), actual) = try_join!(
            write_to_syslog_or_panic(ns_entries, b"Hello \xF0\x90\x80World!"),
            read_message_from_syslog(dir)
        )?;

        assert_eq!(actual, Some("Hello �World!".to_owned()));
        Ok(())
    }

    #[fuchsia::test]
    async fn drain_lines_splits_into_max_size_chunks() -> Result<(), Error> {
        let (tx, rx) = zx::Socket::create_stream();
        let (mut sender, recv) = create_mock_logger();
        let msg = get_random_string(MAX_MESSAGE_SIZE * 4);

        let () = take_and_write_to_socket(tx, &msg)?;
        let (actual, ()) =
            try_join!(recv.collect().map(Result::<Vec<String>, Error>::Ok), async move {
                drain_lines(rx, &mut sender).await
            })?;

        assert_eq!(
            actual,
            msg.as_bytes()
                .chunks(MAX_MESSAGE_SIZE)
                .map(|bytes| std::str::from_utf8(bytes).expect("Bytes are not utf8.").to_owned())
                .collect::<Vec<String>>()
        );

        Ok(())
    }

    #[fuchsia::test]
    async fn drain_lines_splits_at_newline() -> Result<(), Error> {
        let (tx, rx) = zx::Socket::create_stream();
        let (mut sender, recv) = create_mock_logger();
        let msg = std::iter::repeat_with(|| {
            Alphanumeric.sample_string(&mut thread_rng(), MAX_MESSAGE_SIZE - 1)
        })
        .take(3)
        .collect::<Vec<_>>()
        .join("\n");

        let () = take_and_write_to_socket(tx, &msg)?;
        let (actual, ()) =
            try_join!(recv.collect().map(Result::<Vec<String>, Error>::Ok), async move {
                drain_lines(rx, &mut sender).await
            })?;

        assert_eq!(actual, msg.split("\n").map(str::to_owned).collect::<Vec<String>>());
        Ok(())
    }

    #[fuchsia::test]
    async fn drain_lines_writes_when_message_is_received() -> Result<(), Error> {
        let (tx, rx) = zx::Socket::create_stream();
        let (mut sender, mut recv) = create_mock_logger();
        let messages: Vec<String> = vec!["Hello!\n".to_owned(), "World!\n".to_owned()];

        let ((), ()) = try_join!(async move { drain_lines(rx, &mut sender).await }, async move {
            for mut message in messages.into_iter() {
                let () = write_to_socket(&tx, &message)?;
                let logged_messaged =
                    recv.next().await.context("Receiver channel closed. Got no message.")?;
                // Logged message should strip '\n' so we need to do the same before assertion.
                message.pop();
                assert_eq!(logged_messaged, message);
            }

            Ok(())
        })?;

        Ok(())
    }

    #[fuchsia::test]
    async fn drain_lines_waits_for_entire_lines() -> Result<(), Error> {
        let (tx, rx) = zx::Socket::create_stream();
        let (mut sender, mut recv) = create_mock_logger();

        let ((), ()) = try_join!(async move { drain_lines(rx, &mut sender).await }, async move {
            let () = write_to_socket(&tx, "Hello\nWorld")?;
            let logged_messaged =
                recv.next().await.context("Receiver channel closed. Got no message.")?;
            assert_eq!(logged_messaged, "Hello");
            let () = write_to_socket(&tx, "Hello\nAgain")?;
            std::mem::drop(tx);
            let logged_messaged =
                recv.next().await.context("Receiver channel closed. Got no message.")?;
            assert_eq!(logged_messaged, "WorldHello");
            let logged_messaged =
                recv.next().await.context("Receiver channel closed. Got no message.")?;
            assert_eq!(logged_messaged, "Again");
            Ok(())
        })?;

        Ok(())
    }

    #[fuchsia::test]
    async fn drain_lines_collapses_repeated_newlines() {
        let (tx, rx) = zx::Socket::create_stream();
        let (mut sender, mut recv) = create_mock_logger();

        let drainer = Task::spawn(async move { drain_lines(rx, &mut sender).await });

        write_to_socket(&tx, "Hello\n\nWorld\n").unwrap();
        assert_eq!(recv.next().await.unwrap(), "Hello");
        assert_eq!(recv.next().await.unwrap(), "World");

        drop(tx);
        drainer.await.unwrap();
        assert_eq!(recv.next().await, None);
    }

    async fn write_to_syslog_or_panic(
        ns_entries: Vec<fcrunner::ComponentNamespaceEntry>,
        message: &[u8],
    ) -> Result<(), Error> {
        let ns = ComponentNamespace::try_from(ns_entries)
            .context("Failed to create ComponentNamespace")?;
        let logger = create_namespace_logger(&ns).context("Failed to create ScopedLogger")?;
        let mut writer = SyslogWriter::new(Arc::new(logger), OutputLevel::Info);
        writer.write(message).await.context("Failed to write message")?;

        Ok(())
    }

    /// Retrieve message logged to socket. The wire format is expected to
    /// match with the LogSink protocol format.
    pub fn get_message_logged_to_socket(socket: zx::Socket) -> Option<String> {
        let mut buffer: [u8; 1024] = [0; 1024];
        match socket.read(&mut buffer) {
            Ok(read_len) => {
                let msg = diagnostics_message::from_structured(
                    MonikerWithUrl {
                        moniker: "test-pkg/test-component.cmx".to_string(),
                        url: "fuchsia-pkg://fuchsia.com/test-pkg#meta/test-component.cm"
                            .to_string(),
                    },
                    &buffer[..read_len],
                )
                .expect("must be able to decode a valid message from buffer");

                msg.msg().map(String::from)
            }
            Err(_) => None,
        }
    }

    async fn read_message_from_syslog(
        dir: MockServiceFs<'static>,
    ) -> Result<Option<String>, Error> {
        let message_logged = Arc::new(Mutex::new(Option::<String>::None));
        dir.for_each_concurrent(None, |request: MockServiceRequest| match request {
            MockServiceRequest::LogSink(mut r) => {
                let message_logged_copy = Arc::clone(&message_logged);
                async move {
                    match r.next().await.expect("stream error").expect("fidl error") {
                        LogSinkRequest::Connect { .. } => {
                            panic!("Unexpected call to `Connect`");
                        }
                        LogSinkRequest::ConnectStructured { socket, .. } => {
                            *message_logged_copy.lock().unwrap() =
                                get_message_logged_to_socket(socket);
                        }
                        LogSinkRequest::WaitForInterestChange { .. } => {
                            // we expect this request to come but asserting on it is flakey
                        }
                    }
                }
            }
        })
        .await;

        let message_logged =
            message_logged.lock().map_err(|_| anyhow!("Failed to lock mutex"))?.clone();
        Ok(message_logged)
    }

    fn take_and_write_to_socket(socket: zx::Socket, message: &str) -> Result<(), Error> {
        write_to_socket(&socket, &message)
    }

    fn write_to_socket(socket: &zx::Socket, message: &str) -> Result<(), Error> {
        let bytes_written =
            socket.write(message.as_bytes()).context("Failed to write to socket")?;
        match bytes_written == message.len() {
            true => Ok(()),
            false => Err(format_err!("Bytes written to socket doesn't match len of message. Message len = {}. Bytes written = {}", message.len(), bytes_written)),
        }
    }

    fn create_mock_logger() -> (mpsc::Sender<String>, mpsc::Receiver<String>) {
        mpsc::channel::<String>(20)
    }

    fn get_random_string(size: usize) -> String {
        Alphanumeric.sample_string(&mut thread_rng(), size)
    }
}
