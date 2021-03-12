// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Error},
    diagnostics_data::LogsData,
    fidl_fuchsia_developer_remotecontrol::{ArchiveIteratorMarker, ArchiveIteratorProxy},
    fidl_fuchsia_test_manager as ftest_manager, fuchsia_async as fasync,
    futures::Stream,
    futures::{channel::mpsc, stream::BoxStream, SinkExt, StreamExt},
    pin_project::pin_project,
    serde_json,
    std::{
        pin::Pin,
        task::{Context, Poll},
    },
};

#[cfg(not(target_os = "fuchsia"))]
use log::warn;

#[cfg(target_os = "fuchsia")]
use crate::diagnostics::fuchsia::BatchLogStream;

#[derive(Debug)]
pub enum LogStreamProtocol {
    BatchIterator,
    ArchiveIterator,
}

#[pin_project]
pub struct LogStream {
    #[pin]
    stream: BoxStream<'static, Result<LogsData, Error>>,
    iterator_server_end: Option<ftest_manager::LogsIterator>,
}

impl LogStream {
    fn new<S>(stream: S, iterator: ftest_manager::LogsIterator) -> Self
    where
        S: Stream<Item = Result<LogsData, Error>> + Send + 'static,
    {
        Self { stream: stream.boxed(), iterator_server_end: Some(iterator) }
    }

    /// Creates a new `LogStream` forcing the backing log iterator protocol to the given one or the
    /// platform default. In Fuchsia, defaults to BatchIterator. In host, defaults to
    /// ArchiveIterator. If the platform is host and BatchIterator is requested, the request is
    /// ignored since that one is not supported in host.
    pub fn create(force_log_protocol: Option<LogStreamProtocol>) -> Result<LogStream, fidl::Error> {
        get_log_stream(force_log_protocol)
    }

    /// Takes the server end of the backing log iterator protocol.
    pub fn take_iterator_server_end(&mut self) -> Option<ftest_manager::LogsIterator> {
        self.iterator_server_end.take()
    }
}

impl Stream for LogStream {
    type Item = Result<LogsData, Error>;
    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let this = self.project();
        this.stream.poll_next(cx)
    }
}

#[cfg(target_os = "fuchsia")]
fn get_log_stream(force_log_protocol: Option<LogStreamProtocol>) -> Result<LogStream, fidl::Error> {
    match force_log_protocol {
        Some(LogStreamProtocol::BatchIterator) => {
            let (stream, iterator) = BatchLogStream::new()?;
            Ok(LogStream::new(stream, iterator))
        }
        Some(LogStreamProtocol::ArchiveIterator) => {
            let (stream, iterator) = ArchiveLogStream::new()?;
            Ok(LogStream::new(stream, iterator))
        }
        None => {
            let (stream, iterator) = BatchLogStream::new()?;
            Ok(LogStream::new(stream, iterator))
        }
    }
}

#[cfg(not(target_os = "fuchsia"))]
fn get_log_stream(force_log_protocol: Option<LogStreamProtocol>) -> Result<LogStream, fidl::Error> {
    match force_log_protocol {
        Some(LogStreamProtocol::BatchIterator) => {
            warn!("Batch iterator is not supported in host, ignoring force_log_protocol");
            let (stream, iterator) = ArchiveLogStream::new()?;
            Ok(LogStream::new(stream, iterator))
        }
        None | Some(LogStreamProtocol::ArchiveIterator) => {
            let (stream, iterator) = ArchiveLogStream::new()?;
            Ok(LogStream::new(stream, iterator))
        }
    }
}

#[cfg(target_os = "fuchsia")]
mod fuchsia {
    use {
        super::*, diagnostics_data::Logs, diagnostics_reader::Subscription,
        fidl_fuchsia_diagnostics::BatchIteratorMarker,
    };

    #[pin_project]
    pub struct BatchLogStream {
        #[pin]
        subscription: Subscription<Logs>,
    }

    impl BatchLogStream {
        pub fn new() -> Result<(Self, ftest_manager::LogsIterator), fidl::Error> {
            fidl::endpoints::create_proxy::<BatchIteratorMarker>().map(|(proxy, server_end)| {
                let subscription = Subscription::new(proxy);
                (Self { subscription }, ftest_manager::LogsIterator::Batch(server_end))
            })
        }
    }

    impl Stream for BatchLogStream {
        type Item = Result<LogsData, Error>;
        fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
            let this = self.project();
            match this.subscription.poll_next(cx) {
                Poll::Ready(Some(Err(e))) => Poll::Ready(Some(Err(e.into()))),
                Poll::Ready(Some(Ok(value))) => Poll::Ready(Some(Ok(value))),
                Poll::Ready(None) => Poll::Ready(None),
                Poll::Pending => Poll::Pending,
            }
        }
    }
}

#[pin_project]
struct ArchiveLogStream {
    #[pin]
    receiver: mpsc::Receiver<Result<LogsData, Error>>,
    _drain_task: fasync::Task<()>,
}

impl ArchiveLogStream {
    fn new() -> Result<(Self, ftest_manager::LogsIterator), fidl::Error> {
        fidl::endpoints::create_proxy::<ArchiveIteratorMarker>().map(|(proxy, server_end)| {
            let (receiver, _drain_task) = Self::start_streaming_logs(proxy);
            (Self { _drain_task, receiver }, ftest_manager::LogsIterator::Archive(server_end))
        })
    }
}

impl ArchiveLogStream {
    fn start_streaming_logs(
        proxy: ArchiveIteratorProxy,
    ) -> (mpsc::Receiver<Result<LogsData, Error>>, fasync::Task<()>) {
        let (mut sender, receiver) = mpsc::channel(32);
        let task = fasync::Task::spawn(async move {
            loop {
                let result = match proxy.get_next().await {
                    Err(e) => {
                        let _ =
                            sender.send(Err(format_err!("Error calling GetNext: {:?}", e))).await;
                        break;
                    }
                    Ok(batch) => batch,
                };

                let entries = match result {
                    Err(e) => {
                        let _ =
                            sender.send(Err(format_err!("GetNext returned error: {:?}", e))).await;
                        break;
                    }
                    Ok(entries) => entries,
                };

                if entries.is_empty() {
                    break;
                }

                for data_str in entries.into_iter().map(|e| e.data).filter_map(|data| data) {
                    let _ = match serde_json::from_str(&data_str) {
                        Ok(data) => sender.send(Ok(data)).await,
                        Err(e) => sender.send(Err(format_err!("Malformed json: {:?}", e))).await,
                    };
                }
            }
        });
        (receiver, task)
    }
}

impl Stream for ArchiveLogStream {
    type Item = Result<LogsData, Error>;
    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let this = self.project();
        this.receiver.poll_next(cx)
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        diagnostics_data::{
            DiagnosticsHierarchy, LogsData, LogsField, Property, Severity, Timestamp,
        },
        fidl::endpoints::ServerEnd,
        fuchsia_async as fasync,
        futures::TryStreamExt,
    };

    #[cfg(target_os = "fuchsia")]
    mod fuchsia {
        use {
            super::*,
            fidl_fuchsia_diagnostics::{
                BatchIteratorMarker, BatchIteratorRequest, FormattedContent, ReaderError,
            },
            fidl_fuchsia_mem as fmem, fuchsia_zircon as zx,
            futures::StreamExt,
            matches::assert_matches,
        };

        struct BatchIteratorOpts {
            with_error: bool,
        }

        /// Spanws a dummy batch iterator for testing that return 3 logs: "1", "2", "3" all with
        /// the same severity
        async fn spawn_batch_iterator_server(
            server_end: ServerEnd<BatchIteratorMarker>,
            opts: BatchIteratorOpts,
        ) {
            let mut request_stream = server_end.into_stream().expect("got stream");
            let mut values = vec![1i64, 2, 3].into_iter();
            while let Some(BatchIteratorRequest::GetNext { responder }) =
                request_stream.try_next().await.expect("get next request")
            {
                match values.next() {
                    None => {
                        responder.send(&mut Ok(vec![])).expect("send empty response");
                    }
                    Some(value) => {
                        if opts.with_error {
                            responder.send(&mut Err(ReaderError::Io)).expect("send error");
                            continue;
                        }
                        let content = get_json_data(value);
                        let size = content.len() as u64;
                        let vmo = zx::Vmo::create(size).expect("create vmo");
                        vmo.write(content.as_bytes(), 0).expect("write vmo");
                        let result = FormattedContent::Json(fmem::Buffer { vmo, size });
                        responder.send(&mut Ok(vec![result])).expect("send response");
                    }
                }
            }
        }

        #[fasync::run_singlethreaded(test)]
        async fn log_stream_returns_logs() {
            let mut log_stream = LogStream::create(None).expect("got log stream");
            let server_end = match log_stream.take_iterator_server_end() {
                Some(ftest_manager::LogsIterator::Batch(server_end)) => server_end,
                _ => panic!("unexpected logs iterator server end"),
            };
            fasync::Task::spawn(spawn_batch_iterator_server(
                server_end,
                BatchIteratorOpts { with_error: false },
            ))
            .detach();
            assert_eq!(log_stream.next().await.unwrap().expect("got ok result").msg(), Some("1"));
            assert_eq!(log_stream.next().await.unwrap().expect("got ok result").msg(), Some("2"));
            assert_eq!(log_stream.next().await.unwrap().expect("got ok result").msg(), Some("3"));
            assert_matches!(log_stream.next().await, None);
        }

        #[fasync::run_singlethreaded(test)]
        async fn log_stream_can_return_errors() {
            let mut log_stream = LogStream::create(None).expect("got log stream");
            let server_end = match log_stream.take_iterator_server_end() {
                Some(ftest_manager::LogsIterator::Batch(server_end)) => server_end,
                _ => panic!("unexpected logs iterator server end"),
            };
            fasync::Task::spawn(spawn_batch_iterator_server(
                server_end,
                BatchIteratorOpts { with_error: true },
            ))
            .detach();
            assert_matches!(log_stream.next().await, Some(Err(_)));
        }

        #[fasync::run_singlethreaded(test)]
        async fn get_log_stream_on_fuchsia() {
            let mut stream = LogStream::create(None).expect("get log stream ok");
            assert_matches!(
                stream.take_iterator_server_end(),
                Some(ftest_manager::LogsIterator::Batch(_))
            );

            let mut stream = LogStream::create(Some(LogStreamProtocol::ArchiveIterator))
                .expect("get log stream ok");
            assert_matches!(
                stream.take_iterator_server_end(),
                Some(ftest_manager::LogsIterator::Archive(_))
            );

            let mut stream = LogStream::create(Some(LogStreamProtocol::BatchIterator))
                .expect("get log stream ok");
            assert_matches!(
                stream.take_iterator_server_end(),
                Some(ftest_manager::LogsIterator::Batch(_))
            );
        }
    }

    #[cfg(not(target_os = "fuchsia"))]
    mod host {
        use {
            super::*,
            fidl_fuchsia_developer_remotecontrol::{
                ArchiveIteratorEntry, ArchiveIteratorError, ArchiveIteratorMarker,
                ArchiveIteratorRequest,
            },
            matches::assert_matches,
        };

        async fn spawn_archive_iterator_server(
            server_end: ServerEnd<ArchiveIteratorMarker>,
            with_error: bool,
        ) {
            let mut request_stream = server_end.into_stream().expect("got stream");
            let mut values = vec![1, 2, 3].into_iter();
            while let Some(ArchiveIteratorRequest::GetNext { responder }) =
                request_stream.try_next().await.expect("get next request")
            {
                match values.next() {
                    None => {
                        responder.send(&mut Ok(vec![])).expect("send empty response");
                    }
                    Some(value) => {
                        if with_error {
                            responder
                                .send(&mut Err(ArchiveIteratorError::DataReadFailed))
                                .expect("send error");
                            continue;
                        }
                        let json_data = get_json_data(value);
                        let result = ArchiveIteratorEntry {
                            data: Some(json_data),
                            ..ArchiveIteratorEntry::EMPTY
                        };
                        responder.send(&mut Ok(vec![result])).expect("send response");
                    }
                }
            }
        }

        #[fasync::run_singlethreaded(test)]
        async fn get_log_stream_always_returns_archive_in_host() {
            let mut stream = LogStream::create(None).expect("get log stream ok");
            assert_matches!(
                stream.take_iterator_server_end(),
                Some(ftest_manager::LogsIterator::Archive(_))
            );

            let mut stream = LogStream::create(Some(LogStreamProtocol::ArchiveIterator))
                .expect("get log stream ok");
            assert_matches!(
                stream.take_iterator_server_end(),
                Some(ftest_manager::LogsIterator::Archive(_))
            );

            let mut stream = LogStream::create(Some(LogStreamProtocol::BatchIterator))
                .expect("get log stream ok");
            assert_matches!(
                stream.take_iterator_server_end(),
                Some(ftest_manager::LogsIterator::Archive(_))
            );
        }

        #[fasync::run_singlethreaded(test)]
        async fn archive_stream_returns_logs() {
            let mut log_stream = LogStream::create(None).expect("got log stream");
            let server_end = match log_stream.take_iterator_server_end() {
                Some(ftest_manager::LogsIterator::Archive(server_end)) => server_end,
                _ => panic!("unexpected logs iterator server end"),
            };
            fasync::Task::spawn(spawn_archive_iterator_server(server_end, false)).detach();
            assert_eq!(log_stream.next().await.unwrap().expect("got ok result").msg(), Some("1"));
            assert_eq!(log_stream.next().await.unwrap().expect("got ok result").msg(), Some("2"));
            assert_eq!(log_stream.next().await.unwrap().expect("got ok result").msg(), Some("3"));
            assert_matches!(log_stream.next().await, None);
        }

        #[fasync::run_singlethreaded(test)]
        async fn archive_stream_can_return_errors() {
            let mut log_stream = LogStream::create(None).expect("got log stream");
            let server_end = match log_stream.take_iterator_server_end() {
                Some(ftest_manager::LogsIterator::Archive(server_end)) => server_end,
                _ => panic!("unexpected logs iterator server end"),
            };
            fasync::Task::spawn(spawn_archive_iterator_server(server_end, true)).detach();
            assert_matches!(log_stream.next().await, Some(Err(_)));
        }
    }

    fn get_json_data(value: i64) -> String {
        let hierarchy = DiagnosticsHierarchy::new(
            "root",
            vec![Property::String(LogsField::Msg, format!("{}", value))],
            vec![],
        );
        let data = LogsData::for_logs(
            String::from("test/moniker"),
            Some(hierarchy),
            Timestamp::from(0),
            String::from("fake-url"),
            Severity::Info,
            1,
            vec![],
        );
        serde_json::to_string(&data).expect("serialize to json")
    }
}
