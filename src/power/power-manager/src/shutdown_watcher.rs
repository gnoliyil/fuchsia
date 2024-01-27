// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::error::PowerManagerError;
use crate::message::{Message, MessageReturn};
use crate::node::Node;
use crate::shutdown_request::{RebootReason, ShutdownRequest};
use crate::types::Seconds;
use anyhow::Error;
use async_trait::async_trait;
use fidl::endpoints::Proxy;
use fidl_fuchsia_hardware_power_statecontrol as fpower;
use fuchsia_async::{self as fasync, DurationExt, TimeoutExt};
use fuchsia_component::server::{ServiceFs, ServiceObjLocal};
use fuchsia_inspect::{self as inspect, NumericProperty as _};
use fuchsia_zircon as zx;
use fuchsia_zircon::AsHandleRef;
use futures::prelude::*;
use futures::TryStreamExt;
use log::*;
use serde_json as json;
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

/// Node: ShutdownWatcher
///
/// Summary: Provides an implementation of the
/// fuchsia.hardware.power.statecontrol.RebootMethodsWatcherRegister protocol that allows other
/// components in the system to register to be notified of pending system shutdown requests and the
/// associated shutdown reason.
///
/// Handles Messages:
///     - SystemShutdown
///
/// Sends Messages: N/A
///
/// FIDL dependencies:
///     - fuchsia.hardware.power.statecontrol.RebootMethodsWatcherRegister: the node
///       hosts this service to provide a Register API that other components in the system can use
///       to receive notifications about system shutdown events and reasons
///     - fuchsia.hardware.power.statecontrol.RebootMethodsWatcher: the node receives an instance of
///       this protocol over the RebootMethodsWatcherRegister channel, and uses this channel to send
///       shutdown notifications

/// A builder for constructing the ShutdownWatcher node.
pub struct ShutdownWatcherBuilder<'a, 'b> {
    service_fs: Option<&'a mut ServiceFs<ServiceObjLocal<'b, ()>>>,
    inspect_root: Option<&'a inspect::Node>,
}

impl<'a, 'b> ShutdownWatcherBuilder<'a, 'b> {
    pub fn new() -> Self {
        Self { service_fs: None, inspect_root: None }
    }

    pub fn new_from_json(
        _json_data: json::Value,
        _nodes: &HashMap<String, Rc<dyn Node>>,
        service_fs: &'a mut ServiceFs<ServiceObjLocal<'b, ()>>,
    ) -> Self {
        Self::new().with_service_fs(service_fs)
    }

    pub fn with_service_fs(
        mut self,
        service_fs: &'a mut ServiceFs<ServiceObjLocal<'b, ()>>,
    ) -> Self {
        self.service_fs = Some(service_fs);
        self
    }

    #[cfg(test)]
    pub fn with_inspect_root(mut self, root: &'a inspect::Node) -> Self {
        self.inspect_root = Some(root);
        self
    }

    pub fn build(self) -> Result<Rc<ShutdownWatcher>, Error> {
        // Optionally use the default inspect root node
        let inspect_root = self.inspect_root.unwrap_or(inspect::component::inspector().root());

        let node = Rc::new(ShutdownWatcher {
            reboot_watchers: Rc::new(RefCell::new(HashMap::new())),
            inspect: Rc::new(InspectData::new(inspect_root, "ShutdownWatcher".to_string())),
        });

        // Publish the service only if we were provided with a ServiceFs
        if let Some(service_fs) = self.service_fs {
            node.clone().publish_fidl_service(service_fs);
        }

        Ok(node)
    }
}

pub struct ShutdownWatcher {
    /// Contains all the registered RebootMethodsWatcher channels to be notified when a reboot
    /// request is received.
    reboot_watchers: Rc<RefCell<HashMap<u32, fpower::RebootMethodsWatcherProxy>>>,

    /// Struct for managing Component Inspection data
    inspect: Rc<InspectData>,
}

impl ShutdownWatcher {
    const NOTIFY_RESPONSE_TIMEOUT: Seconds =
        Seconds(fpower::MAX_REBOOT_WATCHER_RESPONSE_TIME_SECONDS as f64);

    /// Start and publish the fuchsia.hardware.power.statecontrol.RebootMethodsWatcherRegister
    /// service
    fn publish_fidl_service<'a, 'b>(
        self: Rc<Self>,
        service_fs: &'a mut ServiceFs<ServiceObjLocal<'b, ()>>,
    ) {
        service_fs.dir("svc").add_fidl_service(move |stream| {
            self.clone().handle_new_service_connection(stream);
        });
    }

    /// Handles a new client connection to the RebootMethodsWatcherRegister service.
    ///
    /// This function is called each time a client connects. For each client, a new Task is created
    /// to handle the client's request stream.
    fn handle_new_service_connection(
        self: Rc<Self>,
        mut stream: fpower::RebootMethodsWatcherRegisterRequestStream,
    ) {
        fuchsia_trace::instant!(
            "power_manager",
            "ShutdownWatcher::handle_new_service_connection",
            fuchsia_trace::Scope::Thread
        );

        fasync::Task::local(
            async move {
                while let Some(req) = stream.try_next().await? {
                    match req {
                        fpower::RebootMethodsWatcherRegisterRequest::Register {
                            watcher,
                            control_handle: _,
                        } => {
                            self.add_reboot_watcher(watcher.into_proxy()?);
                        }
                        fpower::RebootMethodsWatcherRegisterRequest::RegisterWithAck {
                            watcher,
                            responder,
                        } => {
                            self.add_reboot_watcher(watcher.into_proxy()?);
                            let _ = responder.send();
                        }
                    }
                }
                Ok(())
            }
            .unwrap_or_else(|e: anyhow::Error| error!("{:?}", e)),
        )
        .detach();
    }

    /// Adds a new RebootMethodsWatcher channel to the list of registered watchers.
    fn add_reboot_watcher(&self, watcher: fpower::RebootMethodsWatcherProxy) {
        fuchsia_trace::duration!(
            "power_manager",
            "ShutdownWatcher::add_reboot_watcher",
            "watcher" => watcher.as_channel().raw_handle()
        );

        // If the client closes the watcher channel, remove it from our `reboot_watchers` map and
        // update Inspect
        let key = watcher.as_channel().raw_handle();
        let proxy = watcher.clone();
        let reboot_watchers = self.reboot_watchers.clone();
        let inspect = self.inspect.clone();
        fasync::Task::local(async move {
            let _ = proxy.on_closed().await;
            reboot_watchers.borrow_mut().remove(&key);
            inspect.remove_reboot_watcher();
        })
        .detach();

        self.inspect.add_reboot_watcher();
        self.reboot_watchers.borrow_mut().insert(key, watcher);
    }

    /// Handles the SystemShutdown message by notifying the appropriate registered watchers.
    async fn handle_system_shutdown_message(
        &self,
        request: ShutdownRequest,
    ) -> Result<MessageReturn, PowerManagerError> {
        fuchsia_trace::instant!(
            "power_manager",
            "ShutdownWatcher::handle_system_shutdown_message",
            fuchsia_trace::Scope::Thread,
            "request" => format!("{:?}", request).as_str()
        );

        match request {
            ShutdownRequest::Reboot(reason) => {
                self.notify_reboot_watchers(reason, Self::NOTIFY_RESPONSE_TIMEOUT).await
            }
            _ => {}
        };
        Ok(MessageReturn::SystemShutdown)
    }

    /// Notifies the registered reboot watchers of the incoming reboot request reason.
    async fn notify_reboot_watchers(&self, reason: RebootReason, timeout: Seconds) {
        // TODO(fxbug.dev/44484): This string must live for the duration of the function because the
        // trace macro uses it when the function goes out of scope. Therefore, it must be bound here
        // and not used anonymously at the macro callsite.
        let reason_str = format!("{:?}", reason);
        fuchsia_trace::duration!(
            "power_manager",
            "ShutdownWatcher::notify_reboot_watchers",
            "reason" => reason_str.as_str()
        );

        // Take the current watchers out of the RefCell because we'll be modifying the vector
        let watchers = self.reboot_watchers.replace(HashMap::new());

        // Create a future for each watcher that calls the watcher's `on_reboot` method and returns
        // the watcher proxy if the response was received within the timeout, or None otherwise. We
        // take this approach so that watchers that timed out have their channel dropped
        // (fxbug.dev/53760).
        let watcher_futures = watchers.into_iter().map(|(key, watcher_proxy)| async move {
            let deadline = zx::Duration::from_seconds(timeout.0 as i64).after_now();
            match watcher_proxy
                .on_reboot(reason)
                .map_err(|_| ())
                .on_timeout(deadline, || Err(()))
                .await
            {
                Ok(()) => Some((key, watcher_proxy)),
                Err(()) => None,
            }
        });

        // Run all of the futures, collecting the successful watcher proxies into a vector
        let watchers = futures::future::join_all(watcher_futures)
            .await
            .into_iter()
            .filter_map(|watcher_opt| watcher_opt) // Unwrap the Options while filtering out None
            .collect();

        // Repopulate the successful watcher proxies back into the `reboot_watchers` RefCell
        self.reboot_watchers.replace(watchers);
    }
}

#[async_trait(?Send)]
impl Node for ShutdownWatcher {
    fn name(&self) -> String {
        "ShutdownWatcher".to_string()
    }

    async fn handle_message(&self, msg: &Message) -> Result<MessageReturn, PowerManagerError> {
        match msg {
            Message::SystemShutdown(request) => self.handle_system_shutdown_message(*request).await,
            _ => Err(PowerManagerError::Unsupported),
        }
    }
}

struct InspectData {
    reboot_watcher_current_connections: inspect::UintProperty,
    reboot_watcher_total_connections: inspect::UintProperty,
}

impl InspectData {
    fn new(parent: &inspect::Node, name: String) -> Self {
        // Create a local root node and properties
        let root = parent.create_child(name);
        let reboot_watcher_current_connections =
            root.create_uint("reboot_watcher_current_connections", 0);
        let reboot_watcher_total_connections =
            root.create_uint("reboot_watcher_total_connections", 0);

        // Pass ownership of the new node to the parent node, otherwise it'll be dropped
        parent.record(root);

        InspectData { reboot_watcher_current_connections, reboot_watcher_total_connections }
    }

    fn add_reboot_watcher(&self) {
        self.reboot_watcher_current_connections.add(1);
        self.reboot_watcher_total_connections.add(1);
    }

    fn remove_reboot_watcher(&self) {
        self.reboot_watcher_current_connections.subtract(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::utils::run_all_tasks_until_stalled::run_all_tasks_until_stalled;
    use assert_matches::assert_matches;
    use fidl::endpoints::{ControlHandle, RequestStream};
    use inspect::assert_data_tree;

    /// Tests that well-formed configuration JSON does not panic the `new_from_json` function.
    #[fasync::run_singlethreaded(test)]
    async fn test_new_from_json() {
        let json_data = json::json!({
            "type": "ShutdownWatcher",
            "name": "shutdown_watcher",
        });

        let _ = ShutdownWatcherBuilder::new_from_json(
            json_data,
            &HashMap::new(),
            &mut ServiceFs::new_local(),
        );
    }

    /// Tests for the presence and correctness of inspect data
    #[test]
    fn test_inspect_data() {
        let mut exec = fasync::TestExecutor::new();
        let inspector = inspect::Inspector::new();
        let node =
            ShutdownWatcherBuilder::new().with_inspect_root(inspector.root()).build().unwrap();

        assert_data_tree!(
            inspector,
            root: {
                ShutdownWatcher: {
                    reboot_watcher_current_connections: 0u64,
                    reboot_watcher_total_connections: 0u64
                }
            }
        );

        let (watcher_proxy, _) =
            fidl::endpoints::create_proxy::<fpower::RebootMethodsWatcherMarker>().unwrap();
        node.add_reboot_watcher(watcher_proxy.clone());

        assert_data_tree!(
            inspector,
            root: {
                ShutdownWatcher: {
                    reboot_watcher_current_connections: 1u64,
                    reboot_watcher_total_connections: 1u64
                }
            }
        );

        drop(watcher_proxy);
        run_all_tasks_until_stalled(&mut exec);

        assert_data_tree!(
            inspector,
            root: {
                ShutdownWatcher: {
                    reboot_watcher_current_connections: 0u64,
                    reboot_watcher_total_connections: 1u64
                }
            }
        );

        let (watcher_proxy, _) =
            fidl::endpoints::create_proxy::<fpower::RebootMethodsWatcherMarker>().unwrap();
        node.add_reboot_watcher(watcher_proxy.clone());

        assert_data_tree!(
            inspector,
            root: {
                ShutdownWatcher: {
                    reboot_watcher_current_connections: 1u64,
                    reboot_watcher_total_connections: 2u64
                }
            }
        );

        drop(watcher_proxy);
        run_all_tasks_until_stalled(&mut exec);

        assert_data_tree!(
            inspector,
            root: {
                ShutdownWatcher: {
                    reboot_watcher_current_connections: 0u64,
                    reboot_watcher_total_connections: 2u64
                }
            }
        );
    }

    /// Tests that a client can successfully register a reboot watcher, and the registered watcher
    /// receives the expected reboot notification.
    #[fasync::run_singlethreaded(test)]
    async fn test_add_reboot_watcher() {
        let node = ShutdownWatcherBuilder::new().build().unwrap();

        // Create the proxy/stream to register the watcher
        let (register_proxy, register_stream) = fidl::endpoints::create_proxy_and_stream::<
            fpower::RebootMethodsWatcherRegisterMarker,
        >()
        .unwrap();

        // Start the RebootMethodsWatcherRegister server that will handle Register calls from
        // register_proxy
        node.clone().handle_new_service_connection(register_stream);

        // Create the watcher proxy/stream to receive reboot notifications
        let (watcher_client, mut watcher_stream) =
            fidl::endpoints::create_request_stream::<fpower::RebootMethodsWatcherMarker>().unwrap();

        // Call the Register API, passing in the watcher_client end
        assert_matches!(register_proxy.register_with_ack(watcher_client).await, Ok(()));

        // Signal the watchers
        node.notify_reboot_watchers(RebootReason::UserRequest, Seconds(0.0)).await;

        // Verify the watcher_stream gets the correct reboot notification
        assert_matches!(
            watcher_stream.try_next().await.unwrap().unwrap(),
            fpower::RebootMethodsWatcherRequest::OnReboot { reason: RebootReason::UserRequest, .. }
        );
    }

    /// Tests that a reboot watcher is delivered the correct reboot reason
    #[fasync::run_singlethreaded(test)]
    async fn test_reboot_watcher_reason() {
        let node = ShutdownWatcherBuilder::new().build().unwrap();
        let (watcher_proxy, mut watcher_stream) =
            fidl::endpoints::create_proxy_and_stream::<fpower::RebootMethodsWatcherMarker>()
                .unwrap();
        node.add_reboot_watcher(watcher_proxy);
        node.notify_reboot_watchers(RebootReason::HighTemperature, Seconds(0.0)).await;

        let received_reboot_reason = match watcher_stream.try_next().await {
            Ok(Some(fpower::RebootMethodsWatcherRequest::OnReboot { reason, .. })) => reason,
            e => panic!("Unexpected watcher_stream result: {:?}", e),
        };

        assert_eq!(received_reboot_reason, RebootReason::HighTemperature);
    }

    /// Tests that if there are multiple registered reboot watchers, each one will receive the
    /// expected reboot notification.
    #[fasync::run_singlethreaded(test)]
    async fn test_multiple_reboot_watchers() {
        let node = ShutdownWatcherBuilder::new().build().unwrap();

        // Create three separate reboot watchers
        let (watcher_proxy1, mut watcher_stream1) =
            fidl::endpoints::create_proxy_and_stream::<fpower::RebootMethodsWatcherMarker>()
                .unwrap();
        node.add_reboot_watcher(watcher_proxy1);

        let (watcher_proxy2, mut watcher_stream2) =
            fidl::endpoints::create_proxy_and_stream::<fpower::RebootMethodsWatcherMarker>()
                .unwrap();
        node.add_reboot_watcher(watcher_proxy2);

        let (watcher_proxy3, mut watcher_stream3) =
            fidl::endpoints::create_proxy_and_stream::<fpower::RebootMethodsWatcherMarker>()
                .unwrap();
        node.add_reboot_watcher(watcher_proxy3);

        // Close the channel of the first watcher to verify the node still correctly notifies the
        // second and third watchers
        watcher_stream1.control_handle().shutdown();

        node.notify_reboot_watchers(RebootReason::HighTemperature, Seconds(0.0)).await;

        // The first watcher should get None because its channel was closed
        match watcher_stream1.try_next().await {
            Ok(None) => {}
            e => panic!("Unexpected watcher_stream1 result: {:?}", e),
        };

        // Verify the watcher received the correct OnReboot request
        match watcher_stream2.try_next().await {
            Ok(Some(fpower::RebootMethodsWatcherRequest::OnReboot {
                reason: RebootReason::HighTemperature,
                ..
            })) => {}
            e => panic!("Unexpected watcher_stream2 result: {:?}", e),
        };

        // Verify the watcher received the correct OnReboot request
        match watcher_stream3.try_next().await {
            Ok(Some(fpower::RebootMethodsWatcherRequest::OnReboot {
                reason: RebootReason::HighTemperature,
                ..
            })) => {}
            e => panic!("Unexpected watcher_stream3 result: {:?}", e),
        };
    }

    /// Tests that a reboot watcher is able to delay the shutdown.
    #[test]
    fn test_watcher_response_delay() {
        let mut exec = fasync::TestExecutor::new();
        let node = ShutdownWatcherBuilder::new().build().unwrap();

        // Register the reboot watcher
        let (watcher_proxy, mut watcher_stream) =
            fidl::endpoints::create_proxy_and_stream::<fpower::RebootMethodsWatcherMarker>()
                .unwrap();
        node.add_reboot_watcher(watcher_proxy);
        assert_eq!(node.reboot_watchers.borrow().len(), 1);

        // Set up the notify future
        let notify_future =
            node.notify_reboot_watchers(RebootReason::HighTemperature, Seconds(1.0));
        futures::pin_mut!(notify_future);

        // Verify that the notify future can't complete on the first attempt (because the watcher
        // will not have responded)
        assert!(exec.run_until_stalled(&mut notify_future).is_pending());

        // Ack the reboot notification, allowing the shutdown flow to continue
        let fpower::RebootMethodsWatcherRequest::OnReboot { responder, .. } =
            exec.run_singlethreaded(&mut watcher_stream.try_next()).unwrap().unwrap();
        assert_matches!(responder.send(), Ok(()));

        // Verify the notify future can now complete
        assert!(exec.run_until_stalled(&mut notify_future).is_ready());
    }

    /// Tests that a reboot watcher is able to delay the shutdown but will time out after the
    /// expected duration. The test also verifies that when a watcher times out, it is removed
    /// from the list of registered reboot watchers.
    #[test]
    fn test_watcher_response_timeout() {
        let mut exec = fasync::TestExecutor::new_with_fake_time();
        let node = ShutdownWatcherBuilder::new().build().unwrap();
        exec.set_fake_time(fasync::Time::from_nanos(0));

        // Register the reboot watcher
        let (watcher_proxy, _watcher_stream) =
            fidl::endpoints::create_proxy_and_stream::<fpower::RebootMethodsWatcherMarker>()
                .unwrap();
        node.add_reboot_watcher(watcher_proxy);
        assert_eq!(node.reboot_watchers.borrow().len(), 1);

        // Set up the notify future
        let notify_future =
            node.notify_reboot_watchers(RebootReason::HighTemperature, Seconds(1.0));
        futures::pin_mut!(notify_future);

        // Verify that the notify future can't complete on the first attempt (because the watcher
        // will not have responded)
        assert!(exec.run_until_stalled(&mut notify_future).is_pending());

        // Wake the timer that causes the watcher timeout to fire
        assert_eq!(exec.wake_next_timer(), Some(fasync::Time::from_nanos(1e9 as i64)));

        // Verify the notify future can now complete
        assert!(exec.run_until_stalled(&mut notify_future).is_ready());

        // Since the watcher timed out, verify it is removed from `reboot_watchers`
        assert_eq!(node.reboot_watchers.borrow().len(), 0);
    }

    /// Tests that an unsuccessful RebootWatcher registration results in the
    /// RebootMethodsWatcherRegister channel being closed.
    #[fasync::run_singlethreaded(test)]
    async fn test_watcher_register_fail() {
        let node = ShutdownWatcherBuilder::new().build().unwrap();

        // Create the registration proxy/stream
        let (register_proxy, register_stream) = fidl::endpoints::create_proxy_and_stream::<
            fpower::RebootMethodsWatcherRegisterMarker,
        >()
        .unwrap();

        // Start the RebootMethodsWatcherRegister server that will handle Register requests from
        // `register_proxy`
        node.clone().handle_new_service_connection(register_stream);

        // Send an invalid request to the server to force a failure
        assert_matches!(register_proxy.as_channel().write(&[], &mut []), Ok(()));

        // Verify the RebootMethodsWatcherRegister channel is closed
        assert_matches!(register_proxy.on_closed().await, Ok(zx::Signals::CHANNEL_PEER_CLOSED));
    }
}
