// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    events::{
        router::EventConsumer,
        types::{DiagnosticsReadyPayload, Event, EventPayload, Moniker},
    },
    identity::ComponentIdentity,
    inspect::container::{
        InspectArtifactsContainer, InspectHandle, UnpopulatedInspectDataContainer,
    },
    pipeline::Pipeline,
    trie,
};
use async_lock::RwLock;
use async_trait::async_trait;
use diagnostics_hierarchy::HierarchyMatcher;
use fidl_fuchsia_diagnostics::{self, Selector};
use flyweights::FlyStr;
use fuchsia_async as fasync;
use fuchsia_zircon::Koid;
use futures::channel::{mpsc, oneshot};
use futures::prelude::*;
use std::{
    collections::HashMap,
    sync::{Arc, Weak},
};
use tracing::{debug, warn};

pub struct InspectRepository {
    inner: RwLock<InspectRepositoryInner>,
    pipelines: Vec<Weak<Pipeline>>,
}

impl Default for InspectRepository {
    fn default() -> Self {
        Self::new(vec![])
    }
}

impl InspectRepository {
    pub fn new(pipelines: Vec<Weak<Pipeline>>) -> InspectRepository {
        let (snd, rcv) = mpsc::unbounded();
        Self {
            pipelines,
            inner: RwLock::new(InspectRepositoryInner {
                diagnostics_containers: trie::Trie::new(),
                diagnostics_dir_closed_snd: snd,
                _diagnostics_dir_closed_drain: fasync::Task::spawn(async move {
                    rcv.for_each_concurrent(None, |rx| rx).await
                }),
            }),
        }
    }
    /// Return all of the DirectoryProxies that contain Inspect hierarchies
    /// which contain data that should be selected from.
    pub async fn fetch_inspect_data(
        &self,
        component_selectors: &Option<Vec<Selector>>,
        moniker_to_static_matcher_map: Option<HashMap<Moniker, Arc<HierarchyMatcher>>>,
    ) -> Vec<UnpopulatedInspectDataContainer> {
        self.inner
            .read()
            .await
            .fetch_inspect_data(component_selectors, moniker_to_static_matcher_map)
            .await
    }

    async fn add_inspect_artifacts(
        self: &Arc<Self>,
        identity: Arc<ComponentIdentity>,
        proxy_handle: impl Into<InspectHandle>,
    ) {
        // Hold the lock while we insert and update pipelines.
        let mut guard = self.inner.write().await;
        // insert_inspect_artifact_container returns None when we were already tracking the
        // directory for this component. If that's the case we can return early.
        let Some(on_closed_fut) = guard.insert_inspect_artifact_container(
            Arc::clone(&identity), proxy_handle)  else {
            return;
        };

        let identity_clone = Arc::clone(&identity);
        let this_weak = Arc::downgrade(self);
        guard
            .diagnostics_dir_closed_snd
            .send(fasync::Task::spawn(async move {
                if let Ok(koid_to_remove) = on_closed_fut.await {
                    if let Some(this) = this_weak.upgrade() {
                        // Hold the lock while we remove and update pipelines.
                        let mut guard = this.inner.write().await;

                        if let Some((_, container)) =
                            guard.diagnostics_containers.get_mut(&identity_clone.unique_key())
                        {
                            if container.remove_handle(koid_to_remove) != 0 {
                                return;
                            }
                        }

                        guard.diagnostics_containers.remove(&identity_clone.unique_key());

                        for pipeline_weak in &this.pipelines {
                            if let Some(pipeline) = pipeline_weak.upgrade() {
                                pipeline.write().await.remove(&identity_clone.relative_moniker);
                            }
                        }
                    }
                }
            }))
            .await
            .unwrap(); // this can't fail unless `self` has been destroyed.

        // Let each pipeline know that a new component arrived, and allow the pipeline
        // to eagerly bucket static selectors based on that component's moniker.
        for pipeline_weak in self.pipelines.iter() {
            if let Some(pipeline) = pipeline_weak.upgrade() {
                pipeline
                    .write()
                    .await
                    .add_inspect_artifacts(&identity.relative_moniker)
                    .unwrap_or_else(|e| {
                        warn!(%identity, ?e,
                            "Failed to add inspect artifacts to pipeline wrapper");
                    });
            }
        }
    }

    pub(crate) async fn add_inspect_handle(
        self: &Arc<Self>,
        component: Arc<ComponentIdentity>,
        handle: impl Into<InspectHandle>,
    ) {
        debug!(identity = %component, "Diagnostics directory is ready.");
        // Update the central repository to reference the new diagnostics source.
        self.add_inspect_artifacts(Arc::clone(&component), handle).await;
    }
}

#[cfg(test)]
impl InspectRepository {
    pub(crate) async fn terminate_inspect(&self, identity: &ComponentIdentity) {
        self.inner.write().await.diagnostics_containers.remove(&identity.unique_key());
    }

    async fn has_match(&self, identity: &Arc<ComponentIdentity>) -> bool {
        let lock = self.inner.read().await;
        lock.get_diagnostics_containers().iter().any(|(_key, val)| match val {
            Some((actual_id, _)) => actual_id == identity,
            _ => false,
        })
    }

    /// Wait for data to appear for `identity`. Will run indefinitely if no data shows up.
    async fn wait_for_artifact(&self, identity: &Arc<ComponentIdentity>) {
        loop {
            if self.has_match(identity).await {
                return;
            }

            fasync::Timer::new(fuchsia_zircon::Time::after(fuchsia_zircon::Duration::from_millis(
                100,
            )))
            .await;
        }
    }

    /// Wait until nothing is present for `identity`. Will run indefinitely if data persists.
    pub(crate) async fn wait_until_gone(&self, identity: &Arc<ComponentIdentity>) {
        loop {
            if !self.has_match(identity).await {
                return;
            }

            fasync::Timer::new(fuchsia_zircon::Time::after(fuchsia_zircon::Duration::from_millis(
                100,
            )))
            .await;
        }
    }

    /// Execute `assertions` on the `InspectArtifactsContainer` found for `identity`, waiting
    /// for that data to appear.
    pub(crate) async fn execute_on_identity<F, Fut>(
        &self,
        identity: &Arc<ComponentIdentity>,
        assertions: F,
    ) -> bool
    where
        F: FnOnce(&InspectArtifactsContainer) -> Fut,
        Fut: futures::Future<Output = ()>,
    {
        self.wait_for_artifact(identity).await;
        if let Some((_, Some((_, container)))) =
            self.inner.read().await.get_diagnostics_containers().iter().find(
                |(_key, val)| match val {
                    Some((actual_id, _)) => actual_id == identity,
                    _ => false,
                },
            )
        {
            assertions(container).await;
            true
        } else {
            // this can only happen if somehow the data is removed after waiting for it
            false
        }
    }
}

#[async_trait]
impl EventConsumer for InspectRepository {
    async fn handle(self: Arc<Self>, event: Event) {
        match event.payload {
            EventPayload::DiagnosticsReady(DiagnosticsReadyPayload { component, directory }) => {
                self.add_inspect_handle(component, directory).await;
            }
            _ => unreachable!("Inspect repository is only subscribed to diagnostics ready"),
        }
    }
}

struct InspectRepositoryInner {
    /// All the diagnostics directories that we are tracking.
    // Once we don't have v1 components the key would represent the moniker. For now, for
    // simplciity, we maintain the ComponentIdentity inside as that one contains the instance id
    // which would make the identity unique in v1.
    diagnostics_containers: trie::Trie<FlyStr, (Arc<ComponentIdentity>, InspectArtifactsContainer)>,

    /// Tasks waiting for PEER_CLOSED signals on diagnostics directories are sent here.
    diagnostics_dir_closed_snd: mpsc::UnboundedSender<fasync::Task<()>>,

    /// Task draining all diagnostics directory PEER_CLOSED signal futures.
    _diagnostics_dir_closed_drain: fasync::Task<()>,
}

impl InspectRepositoryInner {
    // Inserts an InspectArtifactsContainer into the data repository.
    fn insert_inspect_artifact_container(
        &mut self,
        identity: Arc<ComponentIdentity>,
        proxy_handle: impl Into<InspectHandle>,
    ) -> Option<oneshot::Receiver<Koid>> {
        let unique_key: Vec<_> = identity.unique_key().into();
        let diag_repo_entry_opt = self.diagnostics_containers.get_mut(&unique_key);

        match diag_repo_entry_opt {
            None => {
                // An entry with no values implies that the somehow we observed the
                // creation of a component lower in the topology before observing this
                // one. If this is the case, just instantiate as though it's our first
                // time encountering this moniker segment.
                let (inspect_container, on_closed_fut) =
                    InspectArtifactsContainer::new(proxy_handle);
                self.diagnostics_containers.set(unique_key, (identity, inspect_container));
                Some(on_closed_fut)
            }
            Some((_, ref mut artifacts_container)) => artifacts_container.push_handle(proxy_handle),
        }
    }

    async fn fetch_inspect_data(
        &self,
        component_selectors: &Option<Vec<Selector>>,
        moniker_to_static_matcher_map: Option<HashMap<Moniker, Arc<HierarchyMatcher>>>,
    ) -> Vec<UnpopulatedInspectDataContainer> {
        let mut containers = vec![];
        for (_, diagnostics_artifacts_container_opt) in self.diagnostics_containers.iter() {
            let (identity, container) = match &diagnostics_artifacts_container_opt {
                Some((identity, container)) => (identity, container),
                None => continue,
            };

            let optional_hierarchy_matcher = match &moniker_to_static_matcher_map {
                Some(map) => {
                    match map.get(&identity.relative_moniker) {
                        Some(inspect_matcher) => Some(Arc::clone(inspect_matcher)),
                        // Return early if there were static selectors, and none were for this
                        // moniker.
                        None => continue,
                    }
                }
                None => None,
            };

            // Verify that the dynamic selectors contain an entry that applies to
            // this moniker as well.
            if !match component_selectors {
                Some(component_selectors) => component_selectors.iter().any(|s| {
                    selectors::match_component_moniker_against_selector(
                        &identity.relative_moniker,
                        s,
                    )
                    .ok()
                    .unwrap_or(false)
                }),
                None => true,
            } {
                continue;
            }

            // This artifact contains inspect and matches a passed selector.
            if let Some(unpopulated) =
                container.create_unpopulated(identity, optional_hierarchy_matcher)
            {
                containers.push(unpopulated);
            }
        }
        containers
    }
}

#[cfg(test)]
impl InspectRepositoryInner {
    pub(crate) async fn get(
        &self,
        identity: &ComponentIdentity,
    ) -> Option<&(Arc<ComponentIdentity>, InspectArtifactsContainer)> {
        self.diagnostics_containers.get(&identity.unique_key())
    }

    pub(crate) fn get_diagnostics_containers(
        &self,
    ) -> &trie::Trie<FlyStr, (Arc<ComponentIdentity>, InspectArtifactsContainer)> {
        &self.diagnostics_containers
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::events::types::ComponentIdentifier;
    use fidl_fuchsia_io as fio;
    use fuchsia_zircon as zx;
    use fuchsia_zircon::DurationNum;
    use selectors::{self, FastError};

    const TEST_URL: &str = "fuchsia-pkg://test";

    #[fuchsia::test]
    async fn inspect_repo_disallows_duplicated_dirs() {
        let inspect_repo = Arc::new(InspectRepository::default());
        let moniker = vec!["a", "b", "foo.cmx"].into();
        let instance_id = "1234".to_string().into_boxed_str();

        let component_id = ComponentIdentifier::Legacy { instance_id, moniker };
        let identity = Arc::new(ComponentIdentity::from_identifier_and_url(component_id, TEST_URL));

        let (proxy, _) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()
            .expect("create directory proxy");

        Arc::clone(&inspect_repo)
            .handle(Event {
                timestamp: zx::Time::get_monotonic(),
                payload: EventPayload::DiagnosticsReady(DiagnosticsReadyPayload {
                    component: Arc::clone(&identity),
                    directory: proxy,
                }),
            })
            .await;

        let (proxy, _) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()
            .expect("create directory proxy");

        Arc::clone(&inspect_repo)
            .handle(Event {
                timestamp: zx::Time::get_monotonic(),
                payload: EventPayload::DiagnosticsReady(DiagnosticsReadyPayload {
                    component: Arc::clone(&identity),
                    directory: proxy,
                }),
            })
            .await;

        assert!(inspect_repo.inner.read().await.get(&identity).await.is_some());
    }

    #[fuchsia::test]
    async fn data_repo_updates_existing_entry_to_hold_inspect_data() {
        let data_repo = Arc::new(InspectRepository::default());
        let moniker = vec!["a", "b", "foo.cmx"].into();
        let instance_id = "1234".to_string().into_boxed_str();

        let component_id = ComponentIdentifier::Legacy { instance_id, moniker };
        let identity = Arc::new(ComponentIdentity::from_identifier_and_url(component_id, TEST_URL));
        let (proxy, _) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()
            .expect("create directory proxy");

        Arc::clone(&data_repo)
            .handle(Event {
                timestamp: zx::Time::get_monotonic(),
                payload: EventPayload::DiagnosticsReady(DiagnosticsReadyPayload {
                    component: Arc::clone(&identity),
                    directory: proxy,
                }),
            })
            .await;

        {
            let guard = data_repo.inner.read().await;
            let (identity, _) = guard.get(&identity).await.unwrap();
            assert_eq!(identity.url, TEST_URL);
        }
    }

    #[fuchsia::test]
    async fn repo_removes_entries_when_inspect_is_disconnected() {
        let data_repo = Arc::new(InspectRepository::default());
        let moniker = vec!["a", "b", "foo.cmx"].into();
        let instance_id = "1234".to_string().into_boxed_str();
        let component_id = ComponentIdentifier::Legacy { instance_id, moniker };
        let identity = Arc::new(ComponentIdentity::from_identifier_and_url(component_id, TEST_URL));
        let (proxy, server_end) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()
            .expect("create directory proxy");
        {
            Arc::clone(&data_repo)
                .handle(Event {
                    timestamp: zx::Time::get_monotonic(),
                    payload: EventPayload::DiagnosticsReady(DiagnosticsReadyPayload {
                        component: Arc::clone(&identity),
                        directory: proxy,
                    }),
                })
                .await;
            assert!(data_repo.inner.read().await.get(&identity).await.is_some());
        }
        drop(server_end);
        while data_repo.inner.read().await.get(&identity).await.is_some() {
            fasync::Timer::new(fasync::Time::after(100_i64.millis())).await;
        }
    }

    #[fuchsia::test]
    async fn repo_integrates_with_the_pipeline() {
        let selector = selectors::parse_selector::<FastError>(r#"a/b/foo.cmx:root"#).unwrap();
        let static_selectors_opt = Some(vec![selector]);
        let pipeline = Arc::new(Pipeline::for_test(static_selectors_opt));
        let data_repo = Arc::new(InspectRepository::new(vec![Arc::downgrade(&pipeline)]));
        let moniker = vec!["a", "b", "foo.cmx"].into();
        let instance_id = "1234".to_string();
        let component_id =
            ComponentIdentifier::Legacy { instance_id: instance_id.into_boxed_str(), moniker };
        let identity = Arc::new(ComponentIdentity::from_identifier_and_url(component_id, TEST_URL));
        let (proxy, server_end) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()
            .expect("create directory proxy");
        {
            Arc::clone(&data_repo)
                .handle(Event {
                    timestamp: zx::Time::get_monotonic(),
                    payload: EventPayload::DiagnosticsReady(DiagnosticsReadyPayload {
                        component: Arc::clone(&identity),
                        directory: proxy,
                    }),
                })
                .await;
            assert!(data_repo.inner.read().await.get(&identity).await.is_some());
            assert!(pipeline
                .read()
                .await
                .static_selectors_matchers()
                .unwrap()
                .get(&Moniker::from(vec!["a", "b", "foo.cmx"]))
                .is_some())
        }

        // When the directory disconnects, both the pipeline matchers and the repo are cleaned
        drop(server_end);
        while data_repo.inner.read().await.get(&identity).await.is_some() {
            fasync::Timer::new(fasync::Time::after(100_i64.millis())).await;
        }

        assert!(pipeline
            .read()
            .await
            .static_selectors_matchers()
            .unwrap()
            .get(&Moniker::from(vec!["a", "b", "foo.cmx"]))
            .is_none())
    }

    #[fuchsia::test]
    async fn data_repo_filters_inspect_by_selectors() {
        let data_repo = Arc::new(InspectRepository::default());
        let realm_path = vec!["a".to_string(), "b".to_string()];
        let instance_id = "1234".to_string().into_boxed_str();

        let mut moniker = realm_path.clone();
        moniker.push("foo.cmx".to_string());
        let component_id = ComponentIdentifier::Legacy { instance_id, moniker: moniker.into() };
        let identity = Arc::new(ComponentIdentity::from_identifier_and_url(component_id, TEST_URL));

        Arc::clone(&data_repo)
            .handle(Event {
                timestamp: zx::Time::get_monotonic(),
                payload: EventPayload::DiagnosticsReady(DiagnosticsReadyPayload {
                    component: Arc::clone(&identity),
                    directory: fuchsia_fs::directory::open_in_namespace(
                        "/tmp",
                        fuchsia_fs::OpenFlags::RIGHT_READABLE,
                    )
                    .expect("open root"),
                }),
            })
            .await;

        let mut moniker = realm_path;
        moniker.push("foo2.cmx".to_string());
        let component_id2 = ComponentIdentifier::Legacy {
            instance_id: "12345".to_string().into_boxed_str(),
            moniker: moniker.into(),
        };
        let identity2 =
            Arc::new(ComponentIdentity::from_identifier_and_url(component_id2, TEST_URL));

        Arc::clone(&data_repo)
            .handle(Event {
                timestamp: zx::Time::get_monotonic(),
                payload: EventPayload::DiagnosticsReady(DiagnosticsReadyPayload {
                    component: Arc::clone(&identity2),
                    directory: fuchsia_fs::directory::open_in_namespace(
                        "/tmp",
                        fuchsia_fs::OpenFlags::RIGHT_READABLE,
                    )
                    .expect("open root"),
                }),
            })
            .await;

        assert_eq!(2, data_repo.inner.read().await.fetch_inspect_data(&None, None).await.len());

        let selectors = Some(vec![
            selectors::parse_selector::<FastError>("a/b/foo.cmx:root").expect("parse selector")
        ]);
        assert_eq!(
            1,
            data_repo.inner.read().await.fetch_inspect_data(&selectors, None).await.len()
        );

        let selectors = Some(vec![
            selectors::parse_selector::<FastError>("a/b/f*.cmx:root").expect("parse selector")
        ]);
        assert_eq!(
            2,
            data_repo.inner.read().await.fetch_inspect_data(&selectors, None).await.len()
        );

        let selectors = Some(vec![
            selectors::parse_selector::<FastError>("foo.cmx:root").expect("parse selector")
        ]);
        assert_eq!(
            0,
            data_repo.inner.read().await.fetch_inspect_data(&selectors, None).await.len()
        );
    }
}
