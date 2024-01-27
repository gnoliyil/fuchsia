// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Context;
use fidl::endpoints::create_proxy;
use fidl_fuchsia_memory_heapdump_client::{self as fheapdump_client, CollectorError};
use fidl_fuchsia_memory_heapdump_process as fheapdump_process;
use fuchsia_zircon::Koid;
use futures::lock::Mutex;
use futures::StreamExt;
use std::collections::hash_map::{Entry, HashMap};
use std::sync::Arc;
use tracing::{info, warn};

use crate::process::Process;
use crate::process_v1::ProcessV1;
use crate::snapshot_storage::SnapshotStorage;

/// The "root" data structure, containing the state of the collector process.
pub struct Registry {
    // Registered live processes.
    processes: Mutex<HashMap<Koid, Arc<dyn Process>>>,

    // Stored snapshots.
    snapshot_storage: Arc<Mutex<SnapshotStorage>>,
}

impl Registry {
    pub fn new() -> Registry {
        Registry {
            processes: Mutex::new(HashMap::new()),
            snapshot_storage: Arc::new(Mutex::new(SnapshotStorage::new())),
        }
    }

    async fn find_process_by_name(&self, name: &str) -> Result<Arc<dyn Process>, CollectorError> {
        let processes = self.processes.lock().await;
        let mut iterator = processes.values().filter(|p| p.get_name() == name);
        match (iterator.next(), iterator.next()) {
            (Some(process), None) => Ok(process.clone()),
            (None, _) => Err(CollectorError::ProcessSelectorNoMatch),
            (Some(_), Some(_)) => Err(CollectorError::ProcessSelectorAmbiguous),
        }
    }

    async fn find_process_by_koid(&self, koid: &Koid) -> Result<Arc<dyn Process>, CollectorError> {
        let result = self.processes.lock().await.get(koid).cloned();
        result.ok_or(CollectorError::ProcessSelectorNoMatch)
    }

    async fn send_stored_snapshots(
        mut stream: fheapdump_client::StoredSnapshotIteratorRequestStream,
        snapshots: &[fheapdump_client::StoredSnapshot],
    ) -> Result<(), anyhow::Error> {
        // TODO(fxbug.dev/128512): Batch multiple entries in the same message with measure-tape.
        let mut index = 0;
        while let Some(request) = stream.next().await.transpose()? {
            match request {
                fheapdump_client::StoredSnapshotIteratorRequest::GetNext { responder } => {
                    if let Some(elem) = snapshots.get(index) {
                        responder.send(std::slice::from_ref(elem))?;
                        index += 1;
                    } else {
                        responder.send(&[])?;
                    }
                }
            }
        }

        Ok(())
    }

    pub async fn serve_client_stream(
        &self,
        mut stream: fheapdump_client::CollectorRequestStream,
    ) -> Result<(), anyhow::Error> {
        while let Some(request) = stream.next().await.transpose()? {
            match request {
                fheapdump_client::CollectorRequest::TakeLiveSnapshot { payload, responder } => {
                    let process_selector = payload.process_selector;
                    let with_contents = payload.with_contents.unwrap_or(false);

                    let process = match process_selector {
                        Some(fheapdump_client::ProcessSelector::ByName(name)) => {
                            self.find_process_by_name(&name).await
                        }
                        Some(fheapdump_client::ProcessSelector::ByKoid(koid)) => {
                            self.find_process_by_koid(&Koid::from_raw(koid)).await
                        }
                        Some(process_selector @ fheapdump_client::ProcessSelectorUnknown!()) => {
                            warn!(ordinal = process_selector.ordinal(), "Unknown process selector");
                            Err(CollectorError::ProcessSelectorUnsupported)
                        }
                        None => {
                            warn!("Missing process selector");
                            Err(CollectorError::ProcessSelectorUnsupported)
                        }
                    };

                    match process {
                        Ok(process) => match process.take_live_snapshot(with_contents) {
                            Ok(snapshot) => {
                                let (proxy, stream) = create_proxy()
                                    .expect("failed to create snapshot receiver channel");
                                responder.send(Ok(stream))?;

                                if let Err(error) = snapshot.write_to(proxy).await {
                                    warn!(?error, "Error while streaming snapshot");
                                }
                            }
                            Err(error) => {
                                warn!(?error, "Error while taking live snapshot");
                                responder.send(Err(CollectorError::LiveSnapshotFailed))?;
                            }
                        },
                        Err(e) => responder.send(Err(e))?,
                    };
                }
                fheapdump_client::CollectorRequest::ListStoredSnapshots { payload, responder } => {
                    let iterator = payload.iterator.context("missing required iterator")?;
                    let process_selector = payload.process_selector.as_ref();

                    let filter: Result<Box<dyn Fn(Koid, &str) -> bool>, _> = match process_selector
                    {
                        None => Ok(Box::new(|_koid, _name| true)),
                        Some(fheapdump_client::ProcessSelector::ByName(desired_name)) => {
                            Ok(Box::new(|_koid, name| name == *desired_name))
                        }
                        Some(fheapdump_client::ProcessSelector::ByKoid(desired_koid)) => {
                            Ok(Box::new(|koid, _name| koid.raw_koid() == *desired_koid))
                        }
                        Some(process_selector @ fheapdump_client::ProcessSelectorUnknown!()) => {
                            warn!(ordinal = process_selector.ordinal(), "Unknown process selector");
                            Err(CollectorError::ProcessSelectorUnsupported)
                        }
                    };

                    match filter {
                        Ok(filter) => {
                            let snapshots =
                                self.snapshot_storage.lock().await.list_snapshots(filter);
                            responder.send(Ok(()))?;

                            if let Err(error) =
                                Registry::send_stored_snapshots(iterator.into_stream()?, &snapshots)
                                    .await
                            {
                                warn!(?error, "Error while streaming list of stored snapshots");
                            }
                        }
                        Err(error) => responder.send(Err(error))?,
                    }
                }
                fheapdump_client::CollectorRequest::DownloadStoredSnapshot {
                    snapshot_id,
                    responder,
                } => {
                    let snapshot = self.snapshot_storage.lock().await.get_snapshot(snapshot_id);
                    if let Some(snapshot) = snapshot {
                        let (proxy, stream) =
                            create_proxy().expect("failed to create snapshot receiver channel");
                        responder.send(Ok(stream))?;

                        if let Err(error) = snapshot.write_to(proxy).await {
                            warn!(?error, "Error while streaming snapshot");
                        }
                    } else {
                        responder.send(Err(CollectorError::StoredSnapshotNotFound))?;
                    }
                }
            }
        }
        Ok(())
    }

    pub async fn serve_process_stream(
        &self,
        mut stream: fheapdump_process::RegistryRequestStream,
    ) -> Result<(), anyhow::Error> {
        let registration_request = stream
            .next()
            .await
            .ok_or_else(|| anyhow::anyhow!("No registration message was received"))??;

        let process: Arc<dyn Process> = match registration_request {
            fheapdump_process::RegistryRequest::RegisterV1 {
                process,
                allocations_vmo,
                resources_vmo,
                snapshot_sink,
                ..
            } => {
                let process = ProcessV1::new(
                    process,
                    allocations_vmo,
                    resources_vmo,
                    snapshot_sink,
                    self.snapshot_storage.clone(),
                )?;
                Arc::new(process)
            }
        };

        self.serve_process(process).await
    }

    async fn serve_process(&self, process: Arc<dyn Process>) -> Result<(), anyhow::Error> {
        let process_koid = process.get_koid();
        info!(koid = process_koid.raw_koid(), name = process.get_name(), "Process connected");
        match self.processes.lock().await.entry(process_koid) {
            Entry::Vacant(vacant_entry) => vacant_entry.insert(Arc::clone(&process)),
            Entry::Occupied(_) => {
                // This should not happen if the processes are well-behaved.
                anyhow::bail!("Another process with the same koid is already connected")
            }
        };

        let status = process.serve_until_exit().await;

        info!(koid = process_koid.raw_koid(), name = process.get_name(), "Process disconnected");
        self.processes.lock().await.remove(&process_koid).expect("Koid should still be present");

        // Propagate error only after removing the entry from `processes`.
        status
    }

    #[cfg(test)]
    pub async fn list_processes(&self) -> Vec<(Koid, String)> {
        self.processes
            .lock()
            .await
            .iter()
            .map(|(koid, process)| (*koid, process.get_name().to_string()))
            .collect()
    }

    #[cfg(test)]
    pub async fn get_process(&self, koid: &Koid) -> Option<Arc<dyn Process>> {
        self.processes.lock().await.get(koid).cloned()
    }

    #[cfg(test)]
    pub async fn with_snapshot_storage<T>(&self, f: impl FnOnce(&SnapshotStorage) -> T) -> T {
        f(&*self.snapshot_storage.lock().await)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_matches::assert_matches;
    use async_trait::async_trait;
    use fidl::endpoints::create_proxy_and_stream;
    use fuchsia_async as fasync;
    use fuchsia_zircon::sys::ZX_CHANNEL_MAX_MSG_BYTES;
    use futures::{channel::oneshot, pin_mut};
    use itertools::{assert_equal, Itertools};
    use test_case::test_case;

    use crate::process::Snapshot;

    fn create_registry_and_proxy(
        initial_processes: impl IntoIterator<Item = Arc<dyn Process>>,
    ) -> (Arc<Registry>, fheapdump_client::CollectorProxy) {
        // Create a new Registry and register the given processes.
        let registry = Arc::new(Registry::new());
        {
            let mut processes = registry.processes.try_lock().unwrap();
            for process in initial_processes {
                let koid = process.get_koid();
                let insert_result = processes.insert(koid, process);
                assert!(insert_result.is_none(), "found duplicate koid in initial_processes");
            }
        }

        // Create a client and start serving its stream from a detached task.
        let proxy = {
            let registry = registry.clone();
            let (proxy, stream) =
                create_proxy_and_stream::<fheapdump_client::CollectorMarker>().unwrap();

            let worker_fn = async move { registry.serve_client_stream(stream).await.unwrap() };
            fasync::Task::local(worker_fn).detach();

            proxy
        };

        (registry, proxy)
    }

    struct FakeProcess {
        name: String,
        koid: Koid,
        exit_signal: Mutex<Option<oneshot::Receiver<Result<(), anyhow::Error>>>>,
        live_snapshot_succeeds: bool,
    }

    impl FakeProcess {
        /// Returns a new FakeProcess and a oneshot channel to make it exit with a given result.
        pub fn new(
            name: &str,
            koid: Koid,
            live_snapshot_succeeds: bool,
        ) -> (Arc<dyn Process>, oneshot::Sender<anyhow::Result<()>>) {
            let (sender, receiver) = oneshot::channel();
            let fake_process = FakeProcess {
                name: name.to_string(),
                koid,
                exit_signal: Mutex::new(Some(receiver)),
                live_snapshot_succeeds,
            };
            (Arc::new(fake_process), sender)
        }
    }

    #[async_trait]
    impl Process for FakeProcess {
        fn get_name(&self) -> &str {
            &self.name
        }

        fn get_koid(&self) -> Koid {
            self.koid
        }

        async fn serve_until_exit(&self) -> Result<(), anyhow::Error> {
            let exit_signal = self.exit_signal.lock().await.take().unwrap();
            exit_signal.await?
        }

        fn take_live_snapshot(
            &self,
            with_contents: bool,
        ) -> Result<Box<dyn Snapshot>, anyhow::Error> {
            // Either pretend success or failure, depending on the `live_snapshot_succeeds` flag.
            if self.live_snapshot_succeeds {
                Ok(Box::new(FakeSnapshot { with_contents }))
            } else {
                Err(anyhow::anyhow!("Live snapshot failed"))
            }
        }
    }

    struct FakeSnapshot {
        with_contents: bool,
    }

    // A FakeSnapshot contains a single allocation with these values:
    const FAKE_ALLOCATION_ADDRESS: u64 = 1234;
    const FAKE_ALLOCATION_SIZE: u64 = 8;
    const FAKE_ALLOCATION_THREAD_INFO_KOID: u64 = 5555;
    const FAKE_ALLOCATION_THREAD_INFO_NAME: &str = "foobaz";
    const FAKE_ALLOCATION_THREAD_INFO_KEY: u64 = 6789;
    const FAKE_ALLOCATION_STACK_TRACE: [u64; 6] = [11111, 22222, 33333, 22222, 44444, 55555];
    const FAKE_ALLOCATION_STACK_TRACE_KEY: u64 = 9876;
    const FAKE_ALLOCATION_TIMESTAMP: i64 = 123456789;
    const FAKE_ALLOCATION_CONTENTS: [u8; FAKE_ALLOCATION_SIZE as usize] = *b"foobar!!";

    #[async_trait]
    impl Snapshot for FakeSnapshot {
        async fn write_to(
            &self,
            dest: fheapdump_client::SnapshotReceiverProxy,
        ) -> Result<(), anyhow::Error> {
            let fut = dest.batch(&[
                fheapdump_client::SnapshotElement::Allocation(fheapdump_client::Allocation {
                    address: Some(FAKE_ALLOCATION_ADDRESS),
                    size: Some(FAKE_ALLOCATION_SIZE),
                    thread_info_key: Some(FAKE_ALLOCATION_THREAD_INFO_KEY),
                    stack_trace_key: Some(FAKE_ALLOCATION_STACK_TRACE_KEY),
                    timestamp: Some(FAKE_ALLOCATION_TIMESTAMP),
                    ..Default::default()
                }),
                fheapdump_client::SnapshotElement::ThreadInfo(fheapdump_client::ThreadInfo {
                    thread_info_key: Some(FAKE_ALLOCATION_THREAD_INFO_KEY),
                    koid: Some(FAKE_ALLOCATION_THREAD_INFO_KOID),
                    name: Some(FAKE_ALLOCATION_THREAD_INFO_NAME.to_string()),
                    ..Default::default()
                }),
                fheapdump_client::SnapshotElement::StackTrace(fheapdump_client::StackTrace {
                    stack_trace_key: Some(FAKE_ALLOCATION_STACK_TRACE_KEY),
                    program_addresses: Some(FAKE_ALLOCATION_STACK_TRACE.to_vec()),
                    ..Default::default()
                }),
            ]);
            fut.await?;

            if self.with_contents {
                let fut = dest.batch(&[fheapdump_client::SnapshotElement::BlockContents(
                    fheapdump_client::BlockContents {
                        address: Some(FAKE_ALLOCATION_ADDRESS),
                        contents: Some(FAKE_ALLOCATION_CONTENTS.to_vec()),
                        ..Default::default()
                    },
                )]);
                fut.await?;
            }

            let fut = dest.batch(&[]);
            fut.await?;

            Ok(())
        }
    }

    impl FakeSnapshot {
        /// Receives a Snapshot from a SnapshotReceiver channel and asserts that it matches the
        /// output of `write_to`.
        async fn receive_and_assert_match(
            src: fheapdump_client::SnapshotReceiverRequestStream,
            expect_contents: bool,
        ) {
            let mut received_snapshot =
                heapdump_snapshot::Snapshot::receive_from(src).await.unwrap();

            let allocation =
                received_snapshot.allocations.remove(&FAKE_ALLOCATION_ADDRESS).unwrap();
            assert_eq!(allocation.size, FAKE_ALLOCATION_SIZE);
            assert_eq!(allocation.thread_info.koid, FAKE_ALLOCATION_THREAD_INFO_KOID);
            assert_eq!(allocation.thread_info.name, FAKE_ALLOCATION_THREAD_INFO_NAME);
            assert_eq!(allocation.stack_trace.program_addresses, FAKE_ALLOCATION_STACK_TRACE);
            assert_eq!(allocation.timestamp, FAKE_ALLOCATION_TIMESTAMP);
            if expect_contents {
                assert_eq!(
                    allocation.contents.expect("contents must be set"),
                    FAKE_ALLOCATION_CONTENTS
                );
            } else {
                assert_matches!(allocation.contents, None);
            }
            assert!(received_snapshot.allocations.is_empty(), "all the entries have been removed");
        }
    }

    #[test_case(Ok(()) ; "exit ok")]
    #[test_case(Err(anyhow::anyhow!("Simulated error")) ; "exit error")]
    fn test_register_and_unregister(exit_result: Result<(), anyhow::Error>) {
        let mut ex = fasync::TestExecutor::new();
        let registry = Registry::new();

        // Setup fake process and register it.
        let name = "fake";
        let koid = Koid::from_raw(1234);
        let (process, signal) = FakeProcess::new(name, koid, false);
        let serve_fut = registry.serve_process(process);
        pin_mut!(serve_fut);
        assert!(ex.run_until_stalled(&mut serve_fut).is_pending());

        // Verify that the registry now contains the process.
        assert_eq!(ex.run_singlethreaded(registry.list_processes()), [(koid, name.to_string())]);

        // Simulate process exit.
        signal.send(exit_result).unwrap();
        assert!(ex.run_until_stalled(&mut serve_fut).is_ready());

        // Verify that the registry no longer contains the process.
        assert_eq!(ex.run_singlethreaded(registry.list_processes()), []);
    }

    #[test]
    fn test_cannot_register_same_koid_twice() {
        let mut ex = fasync::TestExecutor::new();
        let registry = Registry::new();

        // Create two FakeProcess instances with the same koid.
        let name1 = "fake-1";
        let name2 = "fake-2";
        let koid = Koid::from_raw(1234);
        let (process1, _signal1) = FakeProcess::new(name1, koid, false);
        let (process2, _signal2) = FakeProcess::new(name2, koid, false);

        // Register the first process.
        let serve1_fut = registry.serve_process(process1);
        pin_mut!(serve1_fut);
        assert!(ex.run_until_stalled(&mut serve1_fut).is_pending());

        // Verify that the registry now contains the process.
        assert_eq!(ex.run_singlethreaded(registry.list_processes()), [(koid, name1.to_string())]);

        // Verify that the second process cannot be registered:
        // - serve_process should exit immediately
        // - list_process_koids should not list the koid twice
        let serve2_fut = registry.serve_process(process2);
        pin_mut!(serve2_fut);
        assert!(ex.run_until_stalled(&mut serve2_fut).is_ready());
        assert_eq!(ex.run_singlethreaded(registry.list_processes()), [(koid, name1.to_string())]);

        // Verify that the first process stayed registered as if nothing happened.
        assert!(ex.run_until_stalled(&mut serve1_fut).is_pending());
    }

    #[test_case(Some(fheapdump_client::ProcessSelector::ByKoid(3)), None,
        None ; "valid koid implicitly without contents, snapshot succeeds")]
    #[test_case(Some(fheapdump_client::ProcessSelector::ByKoid(3)), Some(false),
        None ; "valid koid explicitly without contents, snapshot succeeds")]
    #[test_case(Some(fheapdump_client::ProcessSelector::ByKoid(3)), Some(true),
        None ; "valid koid with contents, snapshot succeeds")]
    #[test_case(Some(fheapdump_client::ProcessSelector::ByName("foo".to_string())), None,
        Some(CollectorError::LiveSnapshotFailed) ; "valid name, snapshot fails")]
    #[test_case(Some(fheapdump_client::ProcessSelector::ByName("bar".to_string())), None,
        Some(CollectorError::ProcessSelectorAmbiguous) ; "ambiguous name")]
    #[test_case(Some(fheapdump_client::ProcessSelector::ByName("baz".to_string())), None,
        Some(CollectorError::ProcessSelectorNoMatch) ; "no matching name")]
    #[test_case(Some(fheapdump_client::ProcessSelector::ByKoid(2)), None,
        Some(CollectorError::LiveSnapshotFailed) ; "valid koid, snapshot fails")]
    #[test_case(Some(fheapdump_client::ProcessSelector::ByKoid(99)), None,
        Some(CollectorError::ProcessSelectorNoMatch) ; "no matching koid")]
    #[test_case(None, None,
        Some(CollectorError::ProcessSelectorUnsupported) ; "missing process selector")]
    #[fasync::run_singlethreaded(test)]
    async fn test_take_live_snapshot(
        process_selector: Option<fheapdump_client::ProcessSelector>,
        with_contents: Option<bool>,
        expect_error: Option<CollectorError>,
    ) {
        // Create three FakeProcess instances, two of which with the same name.
        // The first two processes return a LiveSnapshotFailed error; the third one successfully
        // returns a snapshot.
        let (process1, _signal1) = FakeProcess::new("foo", Koid::from_raw(1), false);
        let (process2, _signal2) = FakeProcess::new("bar", Koid::from_raw(2), false);
        let (process3, _signal3) = FakeProcess::new("bar", Koid::from_raw(3), true);

        // Create a Registry and a client connected to it.
        let (_registry, proxy) = create_registry_and_proxy([process1, process2, process3]);

        // Execute the request.
        let request = fheapdump_client::CollectorTakeLiveSnapshotRequest {
            process_selector,
            with_contents,
            ..Default::default()
        };
        let result = proxy.take_live_snapshot(&request).await.expect("FIDL channel error");

        // Verify that the result matches our expectation (either success or a specific error).
        if let Some(expect_error) = expect_error {
            assert_eq!(result.expect_err("request should fail"), expect_error);
        } else {
            let result = result.expect("request should succeed");
            let expect_contents = with_contents == Some(true);
            FakeSnapshot::receive_and_assert_match(result.into_stream().unwrap(), expect_contents)
                .await;
        }
    }

    async fn receive_list_of_snapshots(
        iterator: fheapdump_client::StoredSnapshotIteratorProxy,
    ) -> Vec<fheapdump_client::StoredSnapshot> {
        let mut result = Vec::new();
        loop {
            let batch = iterator.get_next().await.unwrap();
            if batch.is_empty() {
                return result;
            } else {
                result.extend(batch);
            }
        }
    }

    #[test_case(0 ; "empty response")]
    #[test_case(1 ; "one-entry response")]
    #[test_case(ZX_CHANNEL_MAX_MSG_BYTES /* this number of _entries_ will cause pagination */ ;
        "paginated response")]
    #[fasync::run_singlethreaded(test)]
    async fn test_list_stored_snapshots_without_filters(num_entries: u32) {
        // Create a Registry and a client connected to it.
        let (registry, proxy) = create_registry_and_proxy([]);

        // Fill it with fake snapshots.
        let mut expected_snapshots = Vec::new();
        for seq in 0..num_entries {
            // Generate placeholder values.
            let snapshot_name = format!("snapshot-{}", seq);
            let process_koid = (seq % 1234) as u64;
            let process_name = format!("process-{}", seq);

            let snapshot_id = registry.snapshot_storage.lock().await.add_snapshot(
                process_name.clone(),
                Koid::from_raw(process_koid),
                snapshot_name.clone(),
                Box::new(FakeSnapshot { with_contents: false }),
            );

            expected_snapshots.push(fheapdump_client::StoredSnapshot {
                snapshot_id: Some(snapshot_id),
                snapshot_name: Some(snapshot_name),
                process_koid: Some(process_koid),
                process_name: Some(process_name),
                ..Default::default()
            });
        }

        // Get the unfiltered list of snapshots.
        let (iterator, server_end) = create_proxy().unwrap();
        proxy
            .list_stored_snapshots(fheapdump_client::CollectorListStoredSnapshotsRequest {
                iterator: Some(server_end),
                ..Default::default()
            })
            .await
            .unwrap()
            .unwrap();
        let actual_snapshots = receive_list_of_snapshots(iterator).await;

        // Verify the resulting list.
        assert_equal(
            actual_snapshots.iter().sorted_by_key(|e| e.snapshot_id),
            expected_snapshots.iter().sorted_by_key(|e| e.snapshot_id),
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_list_stored_snapshots_with_filters() {
        // Create a Registry and a client connected to it.
        let (registry, proxy) = create_registry_and_proxy([]);

        // Insert two fake snapshots.
        let snapshot_id_koid_1234 = registry.snapshot_storage.lock().await.add_snapshot(
            "process_1234".to_string(),
            Koid::from_raw(1234),
            "test_snapshot_1".to_string(),
            Box::new(FakeSnapshot { with_contents: false }),
        );
        let snapshot_id_name_foobar = registry.snapshot_storage.lock().await.add_snapshot(
            "process_foobar".to_string(),
            Koid::from_raw(4321),
            "test_snapshot_2".to_string(),
            Box::new(FakeSnapshot { with_contents: false }),
        );

        // Verify filtering by koid.
        let (iterator, server_end) = create_proxy().unwrap();
        proxy
            .list_stored_snapshots(fheapdump_client::CollectorListStoredSnapshotsRequest {
                iterator: Some(server_end),
                process_selector: Some(fheapdump_client::ProcessSelector::ByKoid(1234)),
                ..Default::default()
            })
            .await
            .unwrap()
            .unwrap();
        let returned_snapshots = receive_list_of_snapshots(iterator).await;
        assert_eq!(
            returned_snapshots.iter().at_most_one().unwrap().unwrap().snapshot_id,
            Some(snapshot_id_koid_1234)
        );

        // Verify filtering by name.
        let (iterator, server_end) = create_proxy().unwrap();
        proxy
            .list_stored_snapshots(fheapdump_client::CollectorListStoredSnapshotsRequest {
                iterator: Some(server_end),
                process_selector: Some(fheapdump_client::ProcessSelector::ByName(
                    "process_foobar".to_string(),
                )),
                ..Default::default()
            })
            .await
            .unwrap()
            .unwrap();
        let returned_snapshots = receive_list_of_snapshots(iterator).await;
        assert_eq!(
            returned_snapshots.iter().at_most_one().unwrap().unwrap().snapshot_id,
            Some(snapshot_id_name_foobar)
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_download_stored_snapshot() {
        // Create a Registry and a client connected to it.
        let (registry, proxy) = create_registry_and_proxy([]);

        // Insert a fake snapshot and verify that it can be downloaded.
        let snapshot_id = registry.snapshot_storage.lock().await.add_snapshot(
            "foobar".to_string(),
            Koid::from_raw(1234),
            "foobaz".to_string(),
            Box::new(FakeSnapshot { with_contents: false }),
        );
        let result = proxy.download_stored_snapshot(snapshot_id).await.unwrap().unwrap();
        FakeSnapshot::receive_and_assert_match(result.into_stream().unwrap(), false).await;

        // Attempt to request a non-existing snapshot and verify the returned error.
        let bad_snapshot_id = snapshot_id + 1;
        assert_eq!(
            proxy.download_stored_snapshot(bad_snapshot_id).await.unwrap(),
            Err(CollectorError::StoredSnapshotNotFound)
        );
    }
}
