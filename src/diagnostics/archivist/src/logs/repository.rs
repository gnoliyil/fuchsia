// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    events::{
        router::EventConsumer,
        types::{Event, EventPayload, LogSinkRequestedPayload},
    },
    identity::ComponentIdentity,
    logs::{
        budget::BudgetManager,
        container::LogsArtifactsContainer,
        debuglog::{DebugLog, DebugLogBridge, KERNEL_IDENTITY},
        multiplex::{Multiplexer, MultiplexerHandle},
        serial::SerialConfig,
    },
    trie,
};
use async_lock::{Mutex, RwLock};
use async_trait::async_trait;
use diagnostics_data::LogsData;
use fidl_fuchsia_diagnostics::{self, LogInterestSelector, Selector, StreamMode};
use flyweights::FlyStr;
use fuchsia_async as fasync;
use fuchsia_inspect as inspect;
use fuchsia_trace as ftrace;
use futures::channel::mpsc;
use futures::prelude::*;
use lazy_static::lazy_static;
use std::{
    collections::{BTreeMap, HashMap},
    sync::{
        atomic::{AtomicUsize, Ordering},
        Arc,
    },
};
use tracing::{debug, error};

lazy_static! {
    pub static ref INTEREST_CONNECTION_ID: AtomicUsize = AtomicUsize::new(0);
}

/// LogsRepository holds all diagnostics data and is a singleton wrapped by multiple
/// [`pipeline::Pipeline`]s in a given Archivist instance.
pub struct LogsRepository {
    log_sender: Arc<RwLock<mpsc::UnboundedSender<fasync::Task<()>>>>,
    mutable_state: RwLock<LogsRepositoryState>,
    /// Processes removal of components emitted by the budget handler. This is done to prevent a
    /// deadlock. It's behind a mutex, but it's only set once. Holds an Arc<Self>.
    component_removal_task: Mutex<Option<fasync::Task<()>>>,
}

impl LogsRepository {
    pub async fn new(
        logs_max_cached_original_bytes: u64,
        serial_config: SerialConfig,
        parent: &fuchsia_inspect::Node,
    ) -> Arc<Self> {
        let (remover_snd, remover_rcv) = mpsc::unbounded();
        let logs_budget = BudgetManager::new(logs_max_cached_original_bytes as usize, remover_snd);
        let (log_sender, log_receiver) = mpsc::unbounded();
        let this = Arc::new(LogsRepository {
            mutable_state: RwLock::new(LogsRepositoryState::new(
                logs_budget,
                log_receiver,
                serial_config,
                parent,
            )),
            log_sender: Arc::new(RwLock::new(log_sender)),
            component_removal_task: Mutex::new(None),
        });
        *this.component_removal_task.lock().await = Some(fasync::Task::spawn(
            Self::process_removal_of_components(remover_rcv, Arc::clone(&this)),
        ));
        this
    }

    /// Drain the kernel's debug log. The returned future completes once
    /// existing messages have been ingested.
    pub async fn drain_debuglog<K>(&self, klog_reader: K)
    where
        K: DebugLog + Send + Sync + 'static,
    {
        let mut mutable_state = self.mutable_state.write().await;
        // We can only have one klog reader, if this is already set, it means we are already
        // draining klog.
        if mutable_state.drain_klog_task.is_some() {
            return;
        }

        let container = mutable_state.get_log_container(KERNEL_IDENTITY.clone()).await;
        mutable_state.drain_klog_task = Some(fasync::Task::spawn(async move {
            debug!("Draining debuglog.");
            let mut kernel_logger = DebugLogBridge::create(klog_reader);
            let mut messages = match kernel_logger.existing_logs().await {
                Ok(messages) => messages,
                Err(e) => {
                    error!(%e, "failed to read from kernel log, important logs may be missing");
                    return;
                }
            };
            messages.sort_by_key(|m| m.timestamp());
            for message in messages {
                container.ingest_message(message).await;
            }

            let res = kernel_logger
                .listen()
                .try_for_each(|message| async {
                    container.ingest_message(message).await;
                    Ok(())
                })
                .await;
            if let Err(e) = res {
                error!(%e, "failed to drain kernel log, important logs may be missing");
            }
        }));
    }

    pub async fn logs_cursor(
        &self,
        mode: StreamMode,
        selectors: Option<Vec<Selector>>,
        parent_trace_id: ftrace::Id,
    ) -> impl Stream<Item = Arc<LogsData>> + Send + 'static {
        let mut repo = self.mutable_state.write().await;
        let (mut merged, mpx_handle) = Multiplexer::new(parent_trace_id);
        if let Some(selectors) = selectors {
            merged.set_selectors(selectors);
        }
        repo.logs_data_store
            .iter()
            .filter_map(|(_, c)| c)
            .map(|c| {
                let cursor = c.cursor(mode, parent_trace_id);
                (Arc::clone(&c.identity), cursor)
            })
            .for_each(|(n, c)| {
                mpx_handle.send(n, c);
            });
        repo.logs_multiplexers.add(mode, mpx_handle).await;
        merged.set_on_drop_id_sender(repo.logs_multiplexers.cleanup_sender());

        merged
    }

    pub async fn get_log_container(
        &self,
        identity: Arc<ComponentIdentity>,
    ) -> Arc<LogsArtifactsContainer> {
        self.mutable_state.write().await.get_log_container(identity).await
    }

    /// Stop accepting new messages, ensuring that pending Cursors return Poll::Ready(None) after
    /// consuming any messages received before this call.
    pub async fn wait_for_termination(&self) {
        let receiver = self.mutable_state.write().await.log_receiver.take().unwrap();
        receiver.for_each_concurrent(None, |rx| rx).await;
        // Process messages from log sink.
        debug!("Log ingestion stopped.");
        let mut repo = self.mutable_state.write().await;
        for container in repo.logs_data_store.iter().filter_map(|(_, v)| v) {
            container.terminate();
        }
        repo.logs_multiplexers.terminate().await;

        debug!("Terminated logs");
        repo.logs_budget.terminate().await;
    }

    /// Closes the connection in which new logger draining tasks are sent. No more logger tasks
    /// will be accepted when this is called and we'll proceed to terminate logs.
    pub async fn stop_accepting_new_log_sinks(&self) {
        self.log_sender.write().await.disconnect();
    }

    /// Returns an id to use for a new interest connection. Used by both LogSettings and Log, to
    /// ensure shared uniqueness of their connections.
    pub fn new_interest_connection(&self) -> usize {
        INTEREST_CONNECTION_ID.fetch_add(1, Ordering::Relaxed)
    }

    /// Updates log selectors associated with an interest connection.
    pub async fn update_logs_interest(
        &self,
        connection_id: usize,
        selectors: Vec<LogInterestSelector>,
    ) {
        self.mutable_state.write().await.update_logs_interest(connection_id, selectors).await;
    }

    /// Indicates that the connection associated with the given ID is now done.
    pub async fn finish_interest_connection(&self, connection_id: usize) {
        self.mutable_state.write().await.finish_interest_connection(connection_id).await;
    }

    async fn process_removal_of_components(
        mut removal_requests: mpsc::UnboundedReceiver<Arc<ComponentIdentity>>,
        logs_repo: Arc<LogsRepository>,
    ) {
        while let Some(identity) = removal_requests.next().await {
            let mut repo = logs_repo.mutable_state.write().await;
            if !repo.is_live(&identity).await {
                debug!(%identity, "Removing component from repository.");
                repo.remove(&identity);
            }
        }
    }

    #[cfg(test)]
    pub(crate) async fn default() -> Arc<Self> {
        LogsRepository::new(
            crate::constants::LEGACY_DEFAULT_MAXIMUM_CACHED_LOGS_BYTES,
            SerialConfig::default(),
            &Default::default(),
        )
        .await
    }
}

#[async_trait]
impl EventConsumer for LogsRepository {
    async fn handle(self: Arc<Self>, event: Event) {
        match event.payload {
            EventPayload::LogSinkRequested(LogSinkRequestedPayload {
                component,
                request_stream,
            }) => {
                debug!(identity = %component, "LogSink requested.");
                let container = self.get_log_container(component).await;
                container
                    .handle_log_sink(request_stream, self.log_sender.read().await.clone())
                    .await;
            }
            _ => unreachable!("Archivist state just subscribes to log sink requested"),
        }
    }
}

pub struct LogsRepositoryState {
    logs_data_store: trie::Trie<FlyStr, Arc<LogsArtifactsContainer>>,
    inspect_node: inspect::Node,

    /// Receives the logger tasks. This will be taken once in wait for termination hence why it's
    /// an option.
    log_receiver: Option<mpsc::UnboundedReceiver<fasync::Task<()>>>,

    /// A reference to the budget manager, kept to be passed to containers.
    logs_budget: BudgetManager,
    /// BatchIterators for logs need to be made aware of new components starting and their logs.
    logs_multiplexers: MultiplexerBroker,

    /// Interest registrations that we have received through fuchsia.logger.Log/ListWithSelectors
    /// or through fuchsia.logger.LogSettings/RegisterInterest.
    interest_registrations: BTreeMap<usize, Vec<LogInterestSelector>>,

    /// The task draining klog and routing to syslog.
    drain_klog_task: Option<fasync::Task<()>>,

    /// The serial configuration to be used.
    serial_config: SerialConfig,
}

impl LogsRepositoryState {
    fn new(
        logs_budget: BudgetManager,
        log_receiver: mpsc::UnboundedReceiver<fasync::Task<()>>,
        serial_config: SerialConfig,
        parent: &fuchsia_inspect::Node,
    ) -> Self {
        Self {
            inspect_node: parent.create_child("log_sources"),
            logs_data_store: trie::Trie::new(),
            logs_budget,
            log_receiver: Some(log_receiver),
            logs_multiplexers: MultiplexerBroker::new(),
            interest_registrations: BTreeMap::new(),
            drain_klog_task: None,
            serial_config,
        }
    }

    /// Returns a container for logs artifacts, constructing one and adding it to the trie if
    /// necessary.
    pub async fn get_log_container(
        &mut self,
        identity: Arc<ComponentIdentity>,
    ) -> Arc<LogsArtifactsContainer> {
        let trie_key: Vec<_> = identity.unique_key().into();

        match self.logs_data_store.get(&trie_key) {
            None => {
                let serial_config = self.serial_config.for_component(&identity);
                let container = Arc::new(
                    LogsArtifactsContainer::new(
                        identity,
                        self.interest_registrations.values().flat_map(|s| s.iter()),
                        &self.inspect_node,
                        self.logs_budget.handle(),
                        serial_config,
                    )
                    .await,
                );
                self.logs_budget.add_container(Arc::clone(&container)).await;
                self.logs_data_store.set(trie_key, Arc::clone(&container));
                self.logs_multiplexers.send(&container).await;
                container
            }
            Some(existing) => Arc::clone(existing),
        }
    }

    async fn is_live(&self, identity: &ComponentIdentity) -> bool {
        match self.logs_data_store.get(&identity.unique_key()) {
            Some(container) => container.should_retain().await,
            None => false,
        }
    }

    async fn update_logs_interest(
        &mut self,
        connection_id: usize,
        selectors: Vec<LogInterestSelector>,
    ) {
        let previous_selectors =
            self.interest_registrations.insert(connection_id, selectors).unwrap_or_default();
        // unwrap safe, we just inserted.
        let new_selectors = self.interest_registrations.get(&connection_id).unwrap();
        for (_, data) in self.logs_data_store.iter() {
            if let Some(logs_data) = data {
                logs_data.update_interest(new_selectors.iter(), &previous_selectors).await;
            }
        }
    }

    pub async fn finish_interest_connection(&mut self, connection_id: usize) {
        let selectors = self.interest_registrations.remove(&connection_id);
        if let Some(selectors) = selectors {
            for (_, data) in self.logs_data_store.iter() {
                if let Some(logs_data) = data {
                    logs_data.reset_interest(&selectors).await;
                }
            }
        }
    }

    pub fn remove(&mut self, identity: &ComponentIdentity) {
        self.logs_data_store.remove(&identity.unique_key());
    }
}

type LiveIteratorsMap = HashMap<usize, (StreamMode, MultiplexerHandle<Arc<LogsData>>)>;

/// Ensures that BatchIterators get access to logs from newly started components.
pub struct MultiplexerBroker {
    live_iterators: Arc<Mutex<LiveIteratorsMap>>,
    cleanup_sender: mpsc::UnboundedSender<usize>,
    _live_iterators_cleanup_task: fasync::Task<()>,
}

impl MultiplexerBroker {
    fn new() -> Self {
        let (cleanup_sender, mut receiver) = mpsc::unbounded();
        let live_iterators = Arc::new(Mutex::new(HashMap::new()));
        let live_iterators_clone = Arc::clone(&live_iterators);
        Self {
            live_iterators,
            cleanup_sender,
            _live_iterators_cleanup_task: fasync::Task::spawn(async move {
                while let Some(id) = receiver.next().await {
                    live_iterators_clone.lock().await.remove(&id);
                }
            }),
        }
    }

    fn cleanup_sender(&self) -> mpsc::UnboundedSender<usize> {
        self.cleanup_sender.clone()
    }

    /// A new BatchIterator has been created and must be notified when future log containers are
    /// created.
    async fn add(&mut self, mode: StreamMode, recipient: MultiplexerHandle<Arc<LogsData>>) {
        match mode {
            // snapshot streams only want to know about what's currently available
            StreamMode::Snapshot => recipient.close(),
            StreamMode::SnapshotThenSubscribe | StreamMode::Subscribe => {
                self.live_iterators
                    .lock()
                    .await
                    .insert(recipient.multiplexer_id(), (mode, recipient));
            }
        }
    }

    /// Notify existing BatchIterators of a new logs container so they can include its messages
    /// in their results.
    pub async fn send(&mut self, container: &Arc<LogsArtifactsContainer>) {
        self.live_iterators.lock().await.retain(|_, (mode, recipient)| {
            recipient.send(
                Arc::clone(&container.identity),
                container.cursor(*mode, recipient.parent_trace_id()),
            )
        });
    }

    /// Notify all multiplexers to terminate their streams once sub streams have terminated.
    async fn terminate(&mut self) {
        for (_, (_, recipient)) in self.live_iterators.lock().await.drain() {
            recipient.close();
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{events::types::ComponentIdentifier, logs::stored_message::StoredMessage},
        diagnostics_log_encoding::{
            encode::Encoder, Argument, Record, Severity as StreamSeverity, Value,
        },
        selectors::{self, FastError},
        std::{io::Cursor, time::Duration},
    };

    #[fuchsia::test]
    async fn data_repo_filters_logs_by_selectors() {
        let repo = LogsRepository::default().await;
        let foo_container = repo
            .get_log_container(Arc::new(ComponentIdentity::from_identifier_and_url(
                ComponentIdentifier::parse_from_moniker("./foo").unwrap(),
                "fuchsia-pkg://foo",
            )))
            .await;
        let bar_container = repo
            .get_log_container(Arc::new(ComponentIdentity::from_identifier_and_url(
                ComponentIdentifier::parse_from_moniker("./bar").unwrap(),
                "fuchsia-pkg://bar",
            )))
            .await;

        foo_container.ingest_message(make_message("a", 1)).await;
        bar_container.ingest_message(make_message("b", 2)).await;
        foo_container.ingest_message(make_message("c", 3)).await;

        let stream = repo.logs_cursor(StreamMode::Snapshot, None, ftrace::Id::random()).await;

        let results =
            stream.map(|value| value.msg().unwrap().to_string()).collect::<Vec<_>>().await;
        assert_eq!(results, vec!["a".to_string(), "b".to_string(), "c".to_string()]);

        let filtered_stream = repo
            .logs_cursor(
                StreamMode::Snapshot,
                Some(vec![selectors::parse_selector::<FastError>("foo:root").unwrap()]),
                ftrace::Id::random(),
            )
            .await;

        let results =
            filtered_stream.map(|value| value.msg().unwrap().to_string()).collect::<Vec<_>>().await;
        assert_eq!(results, vec!["a".to_string(), "c".to_string()]);
    }

    #[fuchsia::test]
    async fn multiplexer_broker_cleanup() {
        let repo = LogsRepository::default().await;
        let stream =
            repo.logs_cursor(StreamMode::SnapshotThenSubscribe, None, ftrace::Id::random()).await;

        assert_eq!(
            repo.mutable_state.read().await.logs_multiplexers.live_iterators.lock().await.len(),
            1
        );

        // When the multiplexer goes away it must be forgotten by the broker.
        drop(stream);
        loop {
            fasync::Timer::new(Duration::from_millis(100)).await;
            if repo.mutable_state.read().await.logs_multiplexers.live_iterators.lock().await.len()
                == 0
            {
                break;
            }
        }
    }

    fn make_message(msg: &str, timestamp: i64) -> StoredMessage {
        let record = Record {
            timestamp,
            severity: StreamSeverity::Debug,
            arguments: vec![
                Argument { name: "pid".to_string(), value: Value::UnsignedInt(1) },
                Argument { name: "tid".to_string(), value: Value::UnsignedInt(2) },
                Argument { name: "message".to_string(), value: Value::Text(msg.to_string()) },
            ],
        };
        let mut buffer = Cursor::new(vec![0u8; 1024]);
        let mut encoder = Encoder::new(&mut buffer);
        encoder.write_record(&record).unwrap();
        let encoded = &buffer.get_ref()[..buffer.position() as usize];
        StoredMessage::structured(encoded.to_vec(), Default::default())
    }
}
