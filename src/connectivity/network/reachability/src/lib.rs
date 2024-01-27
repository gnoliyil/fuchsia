// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use async_utils::hanging_get::server as hanging_get;
use fidl::endpoints::ControlHandle;
use fidl_fuchsia_net_reachability as freachability;
use fuchsia_async as fasync;
use fuchsia_component::server::{ServiceFsDir, ServiceObjLocal};
use futures::{lock::Mutex, TryFutureExt as _, TryStreamExt as _};
use std::sync::Arc;
use tracing::error;

type WatchResponder = freachability::MonitorWatchResponder;
type NotifyFn = Box<dyn Fn(&freachability::Snapshot, WatchResponder) -> bool>;
type ReachabilityBroker =
    hanging_get::HangingGet<freachability::Snapshot, WatchResponder, NotifyFn>;
type ReachabilityPublisher =
    hanging_get::Publisher<freachability::Snapshot, WatchResponder, NotifyFn>;

pub struct ReachabilityHandler {
    state: Arc<Mutex<ReachabilityState>>,
    broker: Arc<Mutex<ReachabilityBroker>>,
    publisher: Arc<Mutex<ReachabilityPublisher>>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ReachabilityState {
    pub internet_available: bool,
}

impl From<ReachabilityState> for freachability::Snapshot {
    fn from(state: ReachabilityState) -> Self {
        Self { internet_available: Some(state.internet_available), ..Self::EMPTY }
    }
}

impl ReachabilityHandler {
    pub fn new() -> Self {
        let notify_fn: NotifyFn =
            Box::new(|state, responder| match responder.send(state.clone()) {
                Ok(()) => true,
                Err(e) => {
                    error!("Failed to send reachability state to client: {}", e);
                    false
                }
            });
        let state = ReachabilityState { internet_available: false };
        let broker = hanging_get::HangingGet::new(state.clone().into(), notify_fn);
        let publisher = broker.new_publisher();
        Self {
            state: Arc::new(Mutex::new(state)),
            broker: Arc::new(Mutex::new(broker)),
            publisher: Arc::new(Mutex::new(publisher)),
        }
    }

    pub async fn set_state(&mut self, new_state: ReachabilityState) {
        let mut current_state_guard = self.state.lock().await;
        if *current_state_guard != new_state {
            *current_state_guard = new_state;
            self.publisher
                .lock()
                .await
                .set(freachability::Snapshot::from(current_state_guard.clone()));
        }
    }

    pub fn publish_service<'a, 'b>(
        &mut self,
        mut svc_dir: ServiceFsDir<'a, ServiceObjLocal<'b, ()>>,
    ) {
        let _ = svc_dir.add_fidl_service({
            let broker = self.broker.clone();
            move |mut stream: freachability::MonitorRequestStream| {
                let broker = broker.clone();
                fasync::Task::local(
                    async move {
                        let subscriber = broker.lock().await.new_subscriber();
                        // Keep track of whether SetOptions or Watch were already called. Calling
                        // SetOptions after either it or Watch have already been called will result in us
                        // closing the request stream.
                        let mut set_options_called = false;
                        let mut watch_called = false;
                        while let Some(req) = stream.try_next().await? {
                            match req {
                                freachability::MonitorRequest::Watch { responder } => {
                                    watch_called = true;
                                    subscriber.register(responder)?
                                }
                                freachability::MonitorRequest::SetOptions {
                                    payload: _,
                                    control_handle,
                                } => {
                                    if watch_called || set_options_called {
                                        control_handle.shutdown_with_epitaph(
                                            fidl::Status::CONNECTION_ABORTED,
                                        );
                                        break;
                                    }
                                    set_options_called = true;
                                }
                            }
                        }

                        Ok(())
                    }
                    .unwrap_or_else(|e: anyhow::Error| error!("{:?}", e)),
                )
                .detach()
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use anyhow::Error;
    use assert_matches::assert_matches;
    use fidl::endpoints::Proxy;
    use fuchsia_component::server::ServiceFs;
    use futures::StreamExt as _;
    use std::cell::RefCell;
    use std::task::Poll;

    struct TestEnv {
        connector: fuchsia_component::server::ProtocolConnector,
    }

    impl TestEnv {
        fn new(mut service_fs: ServiceFs<ServiceObjLocal<'static, ()>>) -> Self {
            let connector = service_fs.create_protocol_connector().unwrap();
            fasync::Task::local(service_fs.collect()).detach();
            Self { connector }
        }

        fn connect_client(&self) -> FakeClient {
            let watcher_proxy =
                self.connector.connect_to_protocol::<freachability::MonitorMarker>().unwrap();
            FakeClient { watcher_proxy, hanging_watcher_request: RefCell::new(None) }
        }
    }

    struct FakeClient {
        watcher_proxy: freachability::MonitorProxy,
        hanging_watcher_request:
            RefCell<Option<fidl::client::QueryResponseFut<freachability::Snapshot>>>,
    }

    impl FakeClient {
        fn get_reachability_state(
            &self,
            executor: &mut fasync::TestExecutor,
        ) -> Result<Option<freachability::Snapshot>, Error> {
            let mut watch_request = self
                .hanging_watcher_request
                .take()
                .take()
                .unwrap_or_else(|| self.watcher_proxy.watch());

            match executor.run_until_stalled(&mut watch_request) {
                Poll::Pending => {
                    let _: Option<fidl::client::QueryResponseFut<freachability::Snapshot>> =
                        self.hanging_watcher_request.replace(Some(watch_request));
                    Ok(None)
                }
                Poll::Ready(Ok(state)) => Ok(Some(state)),
                Poll::Ready(Err(e)) => Err(e.into()),
            }
        }
    }

    // Tests that the handler correctly implements the hanging-get pattern.
    #[test]
    fn test_hanging_get() {
        let mut executor = fasync::TestExecutor::new();
        let mut service_fs = ServiceFs::new_local();
        let mut handler = ReachabilityHandler::new();
        handler.publish_service(service_fs.root_dir());
        let test_env = TestEnv::new(service_fs);
        let client = test_env.connect_client();

        assert_matches!(
            client.get_reachability_state(&mut executor),
            Ok(Some(freachability::Snapshot { internet_available: Some(false), .. }))
        );

        assert_matches!(client.get_reachability_state(&mut executor), Ok(None));

        executor
            .run_singlethreaded(handler.set_state(ReachabilityState { internet_available: true }));

        assert_matches!(
            client.get_reachability_state(&mut executor),
            Ok(Some(freachability::Snapshot { internet_available: Some(true), .. }))
        );
    }

    #[test]
    fn test_hanging_get_multiple_clients() {
        let mut executor = fasync::TestExecutor::new();
        let mut service_fs = ServiceFs::new_local();
        let mut handler = ReachabilityHandler::new();
        handler.publish_service(service_fs.root_dir());
        let test_env = TestEnv::new(service_fs);

        let client1 = test_env.connect_client();
        let client2 = test_env.connect_client();

        assert_matches!(
            client1.get_reachability_state(&mut executor),
            Ok(Some(freachability::Snapshot { internet_available: Some(false), .. }))
        );
        assert_matches!(
            client2.get_reachability_state(&mut executor),
            Ok(Some(freachability::Snapshot { internet_available: Some(false), .. }))
        );

        assert_matches!(client1.get_reachability_state(&mut executor), Ok(None));
        assert_matches!(client2.get_reachability_state(&mut executor), Ok(None));
    }

    // Tests that the handler closes the request stream if the client calls SetOptions after having
    // already called Watch.
    #[test]
    fn test_cannot_call_set_options_after_watch() {
        let mut executor = fasync::TestExecutor::new();
        let mut service_fs = ServiceFs::new_local();
        let mut handler = ReachabilityHandler::new();
        handler.publish_service(service_fs.root_dir());
        let test_env = TestEnv::new(service_fs);
        let client = test_env.connect_client();

        assert_matches!(client.get_reachability_state(&mut executor), Ok(_));
        assert_matches!(
            client.watcher_proxy.set_options(freachability::MonitorOptions {
                ..freachability::MonitorOptions::EMPTY
            }),
            Ok(())
        );
        assert_matches!(executor.run_singlethreaded(client.watcher_proxy.on_closed()), Ok(_));
    }

    // Tests that the handler closes the request stream if the client calls SetOptions after having
    // already called it before.
    #[test]
    fn test_cannot_call_set_options_twice() {
        let mut executor = fasync::TestExecutor::new();
        let mut service_fs = ServiceFs::new_local();
        let mut handler = ReachabilityHandler::new();
        handler.publish_service(service_fs.root_dir());
        let test_env = TestEnv::new(service_fs);
        let client = test_env.connect_client();

        assert_matches!(
            client.watcher_proxy.set_options(freachability::MonitorOptions {
                ..freachability::MonitorOptions::EMPTY
            }),
            Ok(())
        );
        assert_matches!(
            client.watcher_proxy.set_options(freachability::MonitorOptions {
                ..freachability::MonitorOptions::EMPTY
            }),
            Ok(())
        );
        assert_matches!(executor.run_singlethreaded(client.watcher_proxy.on_closed()), Ok(_));
    }
}
