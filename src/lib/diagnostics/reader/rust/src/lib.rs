// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use diagnostics_data::DiagnosticsData;
use fidl;
use fidl_fuchsia_diagnostics::{
    ArchiveAccessorMarker, ArchiveAccessorProxy, BatchIteratorMarker, BatchIteratorProxy,
    ClientSelectorConfiguration, Format, FormattedContent, PerformanceConfiguration, ReaderError,
    SelectorArgument, StreamMode, StreamParameters,
};
use fuchsia_async::{self as fasync, DurationExt, Task, TimeoutExt};
use fuchsia_component::client;
use fuchsia_zircon::{self as zx, Duration, DurationNum};
use futures::{channel::mpsc, prelude::*, sink::SinkExt, stream::FusedStream};
use pin_project::pin_project;
use serde::Deserialize;
use std::{
    pin::Pin,
    sync::Arc,
    task::{Context, Poll},
};
use thiserror::Error;

use parking_lot::Mutex;

pub use diagnostics_data::{Data, Inspect, Logs, Severity};
pub use diagnostics_hierarchy::{
    assert_data_tree, assert_json_diff, hierarchy, testing::*, tree_assertion,
    DiagnosticsHierarchy, Property,
};

const RETRY_DELAY_MS: i64 = 300;

/// Errors that this library can return
#[derive(Debug, Error)]
pub enum Error {
    #[error("Failed to connect to the archive accessor")]
    ConnectToArchive(#[source] anyhow::Error),

    #[error("Failed to create the BatchIterator channel ends")]
    CreateIteratorProxy(#[source] fidl::Error),

    #[error("Failed to stream diagnostics from the accessor")]
    StreamDiagnostics(#[source] fidl::Error),

    #[error("Failed to call iterator server")]
    GetNextCall(#[source] fidl::Error),

    #[error("Received error from the GetNext response: {0:?}")]
    GetNextReaderError(ReaderError),

    #[error("Failed to read json received")]
    ReadJson(#[source] serde_json::Error),

    #[error("Failed to read cbor received")]
    ReadCbor(#[source] serde_cbor::Error),

    #[error("Failed to parse the diagnostics data from the json received")]
    ParseDiagnosticsData(#[source] serde_json::Error),

    #[error("Failed to read vmo from the response")]
    ReadVmo(#[source] zx::Status),
}

/// An inspect tree selector for a component.
pub struct ComponentSelector {
    relative_moniker: Vec<String>,
    tree_selectors: Vec<String>,
}

impl ComponentSelector {
    /// Create a new component event selector.
    /// By default it will select the whole tree unless tree selectors are provided.
    /// `relative_moniker` is the realm path relative to the realm of the running component plus the
    /// component name. For example: [a, b, component].
    pub fn new(relative_moniker: Vec<String>) -> Self {
        Self { relative_moniker, tree_selectors: Vec::new() }
    }

    /// Select a section of the inspect tree.
    pub fn with_tree_selector(mut self, tree_selector: impl Into<String>) -> Self {
        self.tree_selectors.push(tree_selector.into());
        self
    }

    fn relative_moniker_str(&self) -> String {
        self.relative_moniker.join("/")
    }
}

pub trait ToSelectorArguments {
    fn to_selector_arguments(self) -> Vec<String>;
}

impl ToSelectorArguments for String {
    fn to_selector_arguments(self) -> Vec<String> {
        vec![self]
    }
}

impl ToSelectorArguments for &str {
    fn to_selector_arguments(self) -> Vec<String> {
        vec![self.to_string()]
    }
}

impl ToSelectorArguments for ComponentSelector {
    fn to_selector_arguments(self) -> Vec<String> {
        let relative_moniker = self.relative_moniker_str();
        // If not tree selectors were provided, select the full tree.
        if self.tree_selectors.is_empty() {
            vec![format!("{}:root", relative_moniker)]
        } else {
            self.tree_selectors.iter().map(|s| format!("{}:{}", relative_moniker, s)).collect()
        }
    }
}

/// Utility for reading inspect data of a running component using the injected Archive
/// Reader service.
#[derive(Clone)]
pub struct ArchiveReader {
    archive: Arc<Mutex<Option<ArchiveAccessorProxy>>>,
    selectors: Vec<String>,
    should_retry: bool,
    minimum_schema_count: usize,
    timeout: Option<Duration>,
    batch_retrieval_timeout_seconds: Option<i64>,
    max_aggregated_content_size_bytes: Option<u64>,
}

// Before unsealing this, consider whether your code belongs in this file.
pub trait SerializableValue: private::Sealed {
    const FORMAT_OF_VALUE: Format;
}

// The "sealed trait" pattern.
//
// https://rust-lang.github.io/api-guidelines/future-proofing.html
mod private {
    pub trait Sealed {}
}
impl private::Sealed for serde_json::Value {}
impl private::Sealed for serde_cbor::Value {}

impl SerializableValue for serde_json::Value {
    const FORMAT_OF_VALUE: Format = Format::Json;
}

impl SerializableValue for serde_cbor::Value {
    const FORMAT_OF_VALUE: Format = Format::Cbor;
}

impl ArchiveReader {
    /// Creates a new data fetcher with default configuration:
    ///  - Maximum retries: 2^64-1
    ///  - Timeout: Never. Use with_timeout() to set a timeout.
    pub fn new() -> Self {
        Self {
            timeout: None,
            selectors: vec![],
            should_retry: true,
            archive: Arc::new(Mutex::new(None)),
            minimum_schema_count: 1,
            batch_retrieval_timeout_seconds: None,
            max_aggregated_content_size_bytes: None,
        }
    }

    pub fn with_archive(&mut self, archive: ArchiveAccessorProxy) -> &mut Self {
        {
            let mut arc = self.archive.lock();
            *arc = Some(archive);
        }
        self
    }

    /// Requests a single component tree (or sub-tree).
    pub fn add_selector(&mut self, selector: impl ToSelectorArguments) -> &mut Self {
        self.selectors.extend(selector.to_selector_arguments().into_iter());
        self
    }

    /// Requests all data for the component identified by the given moniker.
    pub fn select_all_for_moniker(&mut self, moniker: &str) -> &mut Self {
        let selector = format!("{}:root", selectors::sanitize_moniker_for_selectors(&moniker));
        self.add_selector(selector)
    }

    /// Requests to retry when an empty result is received.
    pub fn retry_if_empty(&mut self, retry: bool) -> &mut Self {
        self.should_retry = retry;
        self
    }

    pub fn add_selectors<T, S>(&mut self, selectors: T) -> &mut Self
    where
        T: Iterator<Item = S>,
        S: ToSelectorArguments,
    {
        for selector in selectors {
            self.add_selector(selector);
        }
        self
    }

    /// Sets the maximum time to wait for a response from the Archive.
    /// Do not use in tests unless timeout is the expected behavior.
    pub fn with_timeout(&mut self, duration: Duration) -> &mut Self {
        self.timeout = Some(duration);
        self
    }

    pub fn with_aggregated_result_bytes_limit(&mut self, limit_bytes: u64) -> &mut Self {
        self.max_aggregated_content_size_bytes = Some(limit_bytes);
        self
    }

    /// Set the maximum time to wait for a wait for a single component
    /// to have its diagnostics data "pumped".
    pub fn with_batch_retrieval_timeout_seconds(&mut self, timeout: i64) -> &mut Self {
        self.batch_retrieval_timeout_seconds = Some(timeout);
        self
    }

    /// Sets the minumum number of schemas expected in a result in order for the
    /// result to be considered a success.
    pub fn with_minimum_schema_count(&mut self, minimum_schema_count: usize) -> &mut Self {
        self.minimum_schema_count = minimum_schema_count;
        self
    }

    /// Connects to the ArchiveAccessor and returns data matching provided selectors.
    pub async fn snapshot<D>(&self) -> Result<Vec<Data<D>>, Error>
    where
        D: DiagnosticsData,
    {
        let data_future = self.snapshot_inner::<D, Data<D>>(Format::Cbor);
        let data = match self.timeout {
            Some(timeout) => data_future.on_timeout(timeout.after_now(), || Ok(Vec::new())).await?,
            None => data_future.await?,
        };
        Ok(data)
    }

    /// Connects to the ArchiveAccessor and returns a stream of data containing a snapshot of the
    /// current buffer in the Archivist as well as new data that arrives.
    pub fn snapshot_then_subscribe<D>(&self) -> Result<Subscription<Data<D>>, Error>
    where
        D: DiagnosticsData + 'static,
    {
        let iterator = self.batch_iterator::<D>(StreamMode::SnapshotThenSubscribe, Format::Cbor)?;
        Ok(Subscription::new(iterator))
    }

    /// Connects to the ArchiveAccessor and returns inspect data matching provided selectors.
    /// Returns the raw json for each hierarchy fetched.
    pub async fn snapshot_raw<D, T>(&self) -> Result<T, Error>
    where
        D: DiagnosticsData,
        T: for<'a> Deserialize<'a> + SerializableValue + From<Vec<T>>,
    {
        let data_future = self.snapshot_inner::<D, T>(T::FORMAT_OF_VALUE);
        let data = match self.timeout {
            Some(timeout) => data_future.on_timeout(timeout.after_now(), || Ok(Vec::new())).await?,
            None => data_future.await?,
        };
        Ok(T::from(data))
    }

    /// Connects to the ArchiveAccessor and returns a stream of data containing a snapshot of the
    /// current buffer in the Archivist as well as new data that arrives.
    pub fn snapshot_then_subscribe_raw<D, T: SerializableValue + 'static>(
        &self,
    ) -> Result<Subscription<T>, Error>
    where
        D: DiagnosticsData + 'static,
        T: for<'a> Deserialize<'a> + Send,
    {
        let iterator =
            self.batch_iterator::<D>(StreamMode::SnapshotThenSubscribe, T::FORMAT_OF_VALUE)?;
        Ok(Subscription::new(iterator))
    }

    async fn snapshot_inner<D, T>(&self, format: Format) -> Result<Vec<T>, Error>
    where
        D: DiagnosticsData,
        T: for<'a> Deserialize<'a>,
    {
        loop {
            let mut result = Vec::new();
            let iterator = self.batch_iterator::<D>(StreamMode::Snapshot, format)?;
            drain_batch_iterator(iterator, |d| {
                result.push(d);
                async {}
            })
            .await?;

            if result.len() < self.minimum_schema_count && self.should_retry {
                fasync::Timer::new(fasync::Time::after(RETRY_DELAY_MS.millis())).await;
            } else {
                return Ok(result);
            }
        }
    }

    fn batch_iterator<D>(
        &self,
        mode: StreamMode,
        format: Format,
    ) -> Result<BatchIteratorProxy, Error>
    where
        D: DiagnosticsData,
    {
        // TODO(fxbug.dev/58051) this should be done in an ArchiveReaderBuilder -> Reader init
        let mut archive = self.archive.lock();
        if archive.is_none() {
            *archive = Some(
                client::connect_to_protocol::<ArchiveAccessorMarker>()
                    .map_err(Error::ConnectToArchive)?,
            )
        }

        let archive = archive.as_ref().unwrap();

        let (iterator, server_end) = fidl::endpoints::create_proxy::<BatchIteratorMarker>()
            .map_err(Error::CreateIteratorProxy)?;

        let mut stream_parameters = StreamParameters::default();
        stream_parameters.stream_mode = Some(mode);
        stream_parameters.data_type = Some(D::DATA_TYPE);
        stream_parameters.format = Some(format);

        stream_parameters.client_selector_configuration = if self.selectors.is_empty() {
            Some(ClientSelectorConfiguration::SelectAll(true))
        } else {
            Some(ClientSelectorConfiguration::Selectors(
                self.selectors
                    .iter()
                    .map(|selector| SelectorArgument::RawSelector(selector.clone()))
                    .collect(),
            ))
        };

        stream_parameters.performance_configuration = Some(PerformanceConfiguration {
            max_aggregate_content_size_bytes: self.max_aggregated_content_size_bytes,
            batch_retrieval_timeout_seconds: self.batch_retrieval_timeout_seconds,
            ..Default::default()
        });

        archive
            .stream_diagnostics(stream_parameters, server_end)
            .map_err(Error::StreamDiagnostics)?;
        Ok(iterator)
    }
}

#[derive(Debug, Deserialize)]
#[serde(untagged)]
enum OneOrMany<T> {
    Many(Vec<T>),
    One(T),
}

async fn drain_batch_iterator<T, Fut>(
    iterator: BatchIteratorProxy,
    mut send: impl FnMut(T) -> Fut,
) -> Result<(), Error>
where
    Fut: Future<Output = ()>,
    T: for<'a> Deserialize<'a>,
{
    loop {
        let next_batch = iterator
            .get_next()
            .await
            .map_err(Error::GetNextCall)?
            .map_err(Error::GetNextReaderError)?;
        if next_batch.is_empty() {
            return Ok(());
        }
        for formatted_content in next_batch {
            let output: OneOrMany<T> = match formatted_content {
                FormattedContent::Json(data) => {
                    let mut buf = vec![0; data.size as usize];
                    data.vmo.read(&mut buf, 0).map_err(Error::ReadVmo)?;
                    let hierarchy_json = std::str::from_utf8(&buf).unwrap();
                    serde_json::from_str(&hierarchy_json).map_err(Error::ReadJson)?
                }
                FormattedContent::Cbor(vmo) => {
                    let mut buf =
                        vec![0; vmo.get_content_size().expect("Always returns Ok") as usize];
                    vmo.read(&mut buf, 0).map_err(Error::ReadVmo)?;
                    serde_cbor::from_slice(&buf).map_err(Error::ReadCbor)?
                }
                _ => OneOrMany::Many(vec![]),
            };
            match output {
                OneOrMany::One(data) => send(data).await,
                OneOrMany::Many(datas) => {
                    for data in datas {
                        send(data).await;
                    }
                }
            }
        }
    }
}

#[pin_project]
pub struct Subscription<T> {
    #[pin]
    recv: mpsc::Receiver<Result<T, Error>>,
    _drain_task: Task<()>,
}

const DATA_CHANNEL_SIZE: usize = 32;
const ERROR_CHANNEL_SIZE: usize = 2;

impl<T> Subscription<T>
where
    T: for<'a> Deserialize<'a> + Send + 'static,
{
    /// Creates a new subscription stream to a batch iterator.
    /// The stream will return diagnostics data structures.
    pub fn new(iterator: BatchIteratorProxy) -> Self {
        let (mut sender, recv) = mpsc::channel(DATA_CHANNEL_SIZE);
        let _drain_task = Task::spawn(async move {
            let drain_result = drain_batch_iterator(iterator, |d| {
                let mut sender = sender.clone();
                async move {
                    sender.send(Ok(d)).await.ok();
                }
            })
            .await;

            if let Err(e) = drain_result {
                sender.send(Err(e)).await.ok();
            }
        });

        Subscription { recv, _drain_task }
    }

    /// Splits the subscription into two separate streams: results and errors.
    pub fn split_streams(mut self) -> (SubscriptionResultsStream<T>, mpsc::Receiver<Error>) {
        let (mut errors_sender, errors) = mpsc::channel(ERROR_CHANNEL_SIZE);
        let (mut results_sender, recv) = mpsc::channel(DATA_CHANNEL_SIZE);
        let _drain_task = fasync::Task::spawn(async move {
            while let Some(result) = self.next().await {
                match result {
                    Ok(value) => results_sender.send(value).await.ok(),
                    Err(e) => errors_sender.send(e).await.ok(),
                };
            }
        });
        (SubscriptionResultsStream { recv, _drain_task }, errors)
    }
}

impl<T> Stream for Subscription<T>
where
    T: for<'a> Deserialize<'a>,
{
    type Item = Result<T, Error>;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let this = self.project();
        this.recv.poll_next(cx)
    }
}

impl<T> FusedStream for Subscription<T>
where
    T: for<'a> Deserialize<'a>,
{
    fn is_terminated(&self) -> bool {
        self.recv.is_terminated()
    }
}

#[pin_project]
pub struct SubscriptionResultsStream<T> {
    #[pin]
    recv: mpsc::Receiver<T>,
    _drain_task: fasync::Task<()>,
}

impl<T> Stream for SubscriptionResultsStream<T>
where
    T: for<'a> Deserialize<'a>,
{
    type Item = T;

    fn poll_next(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        let this = self.project();
        this.recv.poll_next(cx)
    }
}

impl<T> FusedStream for SubscriptionResultsStream<T>
where
    T: for<'a> Deserialize<'a>,
{
    fn is_terminated(&self) -> bool {
        self.recv.is_terminated()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use diagnostics_hierarchy::assert_data_tree;
    use diagnostics_log::{Publisher, PublisherOptions};
    use fidl_fuchsia_diagnostics as fdiagnostics;
    use fidl_fuchsia_logger as flogger;
    use fuchsia_component_test::{
        Capability, ChildOptions, RealmBuilder, RealmInstance, Ref, Route,
    };
    use fuchsia_zircon as zx;
    use futures::TryStreamExt;
    use tracing::{error, info};

    const TEST_COMPONENT_URL: &str =
        "fuchsia-pkg://fuchsia.com/diagnostics-reader-tests#meta/inspect_test_component.cm";

    async fn start_component() -> Result<RealmInstance, anyhow::Error> {
        let builder = RealmBuilder::new().await?;
        let test_component = builder
            .add_child("test_component", TEST_COMPONENT_URL, ChildOptions::new().eager())
            .await?;
        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                    .from(Ref::parent())
                    .to(&test_component),
            )
            .await?;
        let instance = builder.build().await?;
        Ok(instance)
    }

    #[fuchsia::test]
    async fn inspect_data_for_component() -> Result<(), anyhow::Error> {
        let instance = start_component().await?;

        let moniker = format!("realm_builder\\:{}/test_component", instance.root.child_name());
        let results = ArchiveReader::new()
            .add_selector(format!("{}:root", moniker))
            .snapshot::<Inspect>()
            .await?;

        assert_eq!(results.len(), 1);
        assert_data_tree!(results[0].payload.as_ref().unwrap(), root: {
            int: 3u64,
            "lazy-node": {
                a: "test",
                child: {
                    double: 3.25,
                },
            }
        });

        let mut reader = ArchiveReader::new();
        reader
            .add_selector(format!("{}:root:int", moniker))
            .add_selector(format!("{}:root/lazy-node:a", moniker));
        let response = reader.snapshot::<Inspect>().await?;

        assert_eq!(response.len(), 1);

        assert_eq!(response[0].metadata.component_url, Some(TEST_COMPONENT_URL.to_string()));
        assert_eq!(response[0].moniker, moniker);

        assert_data_tree!(response[0].payload.as_ref().unwrap(), root: {
            int: 3u64,
            "lazy-node": {
                a: "test"
            }
        });

        Ok(())
    }

    #[fuchsia::test]
    async fn select_all_for_moniker() {
        let instance = start_component().await.expect("started component");

        let moniker = format!("realm_builder:{}/test_component", instance.root.child_name());
        let results = ArchiveReader::new()
            .select_all_for_moniker(&moniker)
            .snapshot::<Inspect>()
            .await
            .expect("snapshotted");

        assert_eq!(results.len(), 1);
        assert_data_tree!(results[0].payload.as_ref().unwrap(), root: {
            int: 3u64,
            "lazy-node": {
                a: "test",
                child: {
                    double: 3.25,
                },
            }
        });
    }

    #[fuchsia::test]
    async fn timeout() -> Result<(), anyhow::Error> {
        let instance = start_component().await?;

        let mut reader = ArchiveReader::new();
        reader
            .add_selector(format!(
                "realm_builder\\:{}/test_component:root",
                instance.root.child_name()
            ))
            .with_timeout(0.nanos());
        let result = reader.snapshot::<Inspect>().await;
        assert!(result.unwrap().is_empty());
        Ok(())
    }

    #[fuchsia::test]
    async fn component_selector() {
        let selector = ComponentSelector::new(vec!["a".to_string()]);
        assert_eq!(selector.relative_moniker_str(), "a");
        let arguments: Vec<String> = selector.to_selector_arguments();
        assert_eq!(arguments, vec!["a:root".to_string()]);

        let selector =
            ComponentSelector::new(vec!["b".to_string(), "c".to_string(), "a".to_string()]);
        assert_eq!(selector.relative_moniker_str(), "b/c/a");

        let selector = selector.with_tree_selector("root/b/c:d").with_tree_selector("root/e:f");
        let arguments: Vec<String> = selector.to_selector_arguments();
        assert_eq!(arguments, vec!["b/c/a:root/b/c:d".to_string(), "b/c/a:root/e:f".to_string(),]);
    }

    #[fuchsia::test]
    async fn custom_archive() {
        let proxy = spawn_fake_archive(serde_json::json!({
            "moniker": "moniker",
            "version": 1,
            "data_source": "Inspect",
            "metadata": {
              "component_url": "component-url",
              "timestamp": 0,
              "filename": "filename",
            },
            "payload": {
                "root": {
                    "x": 1,
                }
            }
        }));
        let result = ArchiveReader::new()
            .with_archive(proxy)
            .snapshot::<Inspect>()
            .await
            .expect("got result");
        assert_eq!(result.len(), 1);
        assert_data_tree!(result[0].payload.as_ref().unwrap(), root: { x: 1u64 });
    }

    #[fuchsia::test]
    async fn handles_lists_correctly_on_snapshot_raw() {
        let value = serde_json::json!({
            "moniker": "moniker",
            "version": 1,
            "data_source": "Inspect",
            "metadata": {
            "component_url": "component-url",
            "timestamp": 0,
            "filename": "filename",
            },
            "payload": {
                "root": {
                    "x": 1,
                }
            }
        });
        let proxy = spawn_fake_archive(serde_json::json!([value.clone()]));
        let mut reader = ArchiveReader::new();
        reader.with_archive(proxy);
        let json_result =
            reader.snapshot_raw::<Inspect, serde_json::Value>().await.expect("got result");
        match json_result {
            serde_json::Value::Array(values) => {
                assert_eq!(values.len(), 1);
                assert_eq!(values[0], value);
            }
            result => panic!("unexpected result: {:?}", result),
        }
        let cbor_result =
            reader.snapshot_raw::<Inspect, serde_cbor::Value>().await.expect("got result");
        match cbor_result {
            serde_cbor::Value::Array(values) => {
                assert_eq!(values.len(), 1);
                let json_result =
                    serde_cbor::value::from_value::<serde_json::Value>(values[0].to_owned())
                        .expect("Should convert cleanly to JSON");
                assert_eq!(json_result, value);
            }
            result => panic!("unexpected result: {:?}", result),
        }
    }

    #[fuchsia::test]
    async fn snapshot_then_subscribe() {
        let (_instance, publisher, reader) = init_isolated_logging().await;
        let (mut stream, _errors) =
            reader.snapshot_then_subscribe::<Logs>().expect("subscribed to logs").split_streams();
        tracing::subscriber::with_default(publisher, || {
            info!("hello from test");
            error!("error from test");
        });
        let log = stream.next().await.unwrap();
        assert_eq!(log.msg().unwrap(), "hello from test");
        let log = stream.next().await.unwrap();
        assert_eq!(log.msg().unwrap(), "error from test");
    }

    #[fuchsia::test]
    async fn snapshot_then_subscribe_raw() {
        let (_instance, publisher, reader) = init_isolated_logging().await;
        let (mut stream, _errors) = reader
            .snapshot_then_subscribe_raw::<Logs, serde_json::Value>()
            .expect("subscribed to logs")
            .split_streams();
        tracing::subscriber::with_default(publisher, || {
            info!("hello from test");
            error!("error from test");
        });
        let log = stream.next().await.unwrap();
        assert_eq!(log["payload"]["root"]["message"]["value"], "hello from test");
        let log = stream.next().await.unwrap();
        assert_eq!(log["payload"]["root"]["message"]["value"], "error from test");
    }

    fn spawn_fake_archive(data_to_send: serde_json::Value) -> fdiagnostics::ArchiveAccessorProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<fdiagnostics::ArchiveAccessorMarker>()
                .expect("create proxy");
        fasync::Task::spawn(async move {
            while let Some(request) = stream.try_next().await.expect("stream request") {
                match request {
                    fdiagnostics::ArchiveAccessorRequest::StreamDiagnostics {
                        result_stream,
                        ..
                    } => {
                        let data = data_to_send.clone();
                        fasync::Task::spawn(async move {
                            let mut called = false;
                            let mut stream = result_stream.into_stream().expect("into stream");
                            while let Some(req) = stream.try_next().await.expect("stream request") {
                                match req {
                                    fdiagnostics::BatchIteratorRequest::GetNext { responder } => {
                                        if called {
                                            responder
                                                .send(&mut Ok(Vec::new()))
                                                .expect("send response");
                                            continue;
                                        }
                                        called = true;
                                        let content = serde_json::to_string_pretty(&data)
                                            .expect("json pretty");
                                        let vmo_size = content.len() as u64;
                                        let vmo =
                                            zx::Vmo::create(vmo_size as u64).expect("create vmo");
                                        vmo.write(content.as_bytes(), 0).expect("write vmo");
                                        let buffer =
                                            fidl_fuchsia_mem::Buffer { vmo, size: vmo_size };
                                        responder
                                            .send(&mut Ok(vec![
                                                fdiagnostics::FormattedContent::Json(buffer),
                                            ]))
                                            .expect("send response");
                                    }
                                }
                            }
                        })
                        .detach();
                    }
                }
            }
        })
        .detach();
        return proxy;
    }

    async fn create_realm() -> RealmBuilder {
        let builder = RealmBuilder::new().await.expect("create realm builder");
        let archivist = builder
            .add_child("archivist", "#meta/archivist-for-embedding.cm", ChildOptions::new().eager())
            .await
            .expect("add child archivist");
        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                    .capability(Capability::protocol_by_name("fuchsia.sys2.EventSource"))
                    .capability(
                        Capability::protocol_by_name("fuchsia.tracing.provider.Registry")
                            .optional(),
                    )
                    .capability(Capability::event_stream("stopped"))
                    .capability(Capability::event_stream("directory_ready"))
                    .capability(Capability::event_stream("capability_requested"))
                    .from(Ref::parent())
                    .to(&archivist),
            )
            .await
            .expect("added routes from parent to archivist");
        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol_by_name("fuchsia.diagnostics.ArchiveAccessor"))
                    .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                    .from(&archivist)
                    .to(Ref::parent()),
            )
            .await
            .expect("added routes from archivist to parent");
        builder
    }

    async fn init_isolated_logging() -> (RealmInstance, Publisher, ArchiveReader) {
        let instance = create_realm().await.build().await.unwrap();
        let log_sink_proxy =
            instance.root.connect_to_protocol_at_exposed_dir::<flogger::LogSinkMarker>().unwrap();
        let accessor_proxy = instance
            .root
            .connect_to_protocol_at_exposed_dir::<fdiagnostics::ArchiveAccessorMarker>()
            .unwrap();
        let mut reader = ArchiveReader::new();
        reader.with_archive(accessor_proxy);
        let options = PublisherOptions::default()
            .wait_for_initial_interest(false)
            .use_log_sink(log_sink_proxy);
        let publisher = Publisher::new(options).unwrap();
        (instance, publisher, reader)
    }
}
