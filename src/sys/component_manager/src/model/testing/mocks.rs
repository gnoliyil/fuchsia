// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        builtin::runner::BuiltinRunnerFactory,
        model::{component::WeakComponentInstance, resolver::Resolver, routing::RouteRequest},
    },
    ::routing::{
        capability_source::ComponentCapability,
        policy::ScopedPolicyChecker,
        resolving::{ComponentAddress, ResolvedComponent, ResolvedPackage, ResolverError},
        router::RouteBundle,
    },
    anyhow::format_err,
    assert_matches::assert_matches,
    async_trait::async_trait,
    cm_runner::Runner,
    cm_rust::{CapabilityTypeName, ComponentDecl, ConfigValuesData},
    fidl::prelude::*,
    fidl::{
        endpoints::{create_endpoints, ClientEnd, ServerEnd},
        epitaph::ChannelEpitaphExt,
    },
    fidl_fidl_examples_routing_echo::{EchoMarker, EchoRequest, EchoRequestStream},
    fidl_fuchsia_component_runner as fcrunner,
    fidl_fuchsia_diagnostics_types::{
        ComponentDiagnostics, ComponentTasks, Task as DiagnosticsTask,
    },
    fidl_fuchsia_io as fio, fuchsia_async as fasync,
    fuchsia_zircon::{self as zx, AsHandleRef, HandleBased, Koid},
    futures::{
        channel::oneshot,
        future::{AbortHandle, Abortable},
        lock::Mutex,
        prelude::*,
    },
    std::{
        boxed::Box,
        collections::{HashMap, HashSet},
        mem,
        sync::{Arc, Mutex as SyncMutex},
    },
    tracing::warn,
    version_history,
    vfs::{
        directory::entry::DirectoryEntry, execution_scope::ExecutionScope, file::vmo::read_only,
        path::Path, pseudo_directory, remote::RoutingFn, service,
    },
};

#[derive(Clone, Copy)]
pub enum DeclType {
    Use,
    Expose,
}

/// Creates a routing function that does the following:
/// - Redirects all directory capabilities to a directory with the file "hello".
/// - Redirects all service capabilities to the echo service.
pub fn proxy_routing_factory(
    decl_type: DeclType,
) -> impl Fn(WeakComponentInstance, ComponentCapability, RouteRequest) -> RoutingFn {
    move |_component: WeakComponentInstance, cap: ComponentCapability, request: RouteRequest| {
        new_proxy_routing_fn(decl_type, cap.type_name(), request)
    }
}

async fn echo_protocol_fn(mut stream: EchoRequestStream) {
    while let Some(EchoRequest::EchoString { value, responder }) = stream.try_next().await.unwrap()
    {
        responder.send(value.as_ref().map(|s| &**s)).unwrap();
    }
}

fn new_proxy_routing_fn(
    decl_type: DeclType,
    ty: CapabilityTypeName,
    request: RouteRequest,
) -> RoutingFn {
    Box::new(
        move |scope: ExecutionScope,
              flags: fio::OpenFlags,
              path: Path,
              server_end: ServerEnd<fio::NodeMarker>| {
            match ty {
                CapabilityTypeName::Protocol => {
                    match decl_type {
                        DeclType::Use => {
                            assert_matches!(request, RouteRequest::UseProtocol(_));
                        }
                        DeclType::Expose => {
                            assert_matches!(request, RouteRequest::ExposeProtocol(_));
                        }
                    }
                    scope.spawn(async move {
                        let server_end: ServerEnd<EchoMarker> =
                            ServerEnd::new(server_end.into_channel());
                        let stream: EchoRequestStream = server_end.into_stream().unwrap();
                        echo_protocol_fn(stream).await;
                    });
                }
                CapabilityTypeName::Service => {
                    match decl_type {
                        DeclType::Use => {
                            assert_matches!(request, RouteRequest::UseService(_));
                        }
                        DeclType::Expose => {
                            assert_matches!(
                                &request,
                                RouteRequest::ExposeService(RouteBundle::Aggregate(v))
                                if v.len() == 2
                            );
                        }
                    }
                    let svc_host = service::host(echo_protocol_fn);
                    let sub_dir = pseudo_directory!(
                        "default" => pseudo_directory!(
                            "echo" => svc_host
                        )
                    );
                    sub_dir.open(ExecutionScope::new(), flags, path, server_end);
                }
                CapabilityTypeName::Directory => {
                    match decl_type {
                        DeclType::Use => {
                            assert_matches!(request, RouteRequest::UseDirectory(_));
                        }
                        DeclType::Expose => {
                            assert_matches!(request, RouteRequest::ExposeDirectory(_));
                        }
                    }
                    let sub_dir = pseudo_directory!(
                        "hello" => read_only(b"friend"),
                    );
                    sub_dir.open(ExecutionScope::new(), flags, path, server_end);
                }
                CapabilityTypeName::Storage => {
                    match decl_type {
                        DeclType::Use => {
                            assert_matches!(request, RouteRequest::UseStorage(_));
                        }
                        DeclType::Expose => {
                            panic!("can't expose storage");
                        }
                    }
                    let sub_dir = pseudo_directory!(
                        "hello" => read_only(b"friend"),
                    );
                    sub_dir.open(ExecutionScope::new(), flags, path, server_end);
                }
                CapabilityTypeName::Runner => panic!("runner capability unsupported"),
                CapabilityTypeName::Resolver => panic!("resolver capability unsupported"),
                CapabilityTypeName::EventStream => panic!("event stream capability unsupported"),
            }
        },
    )
}

#[derive(Debug, Clone)]
pub struct MockResolver {
    components: HashMap<String, ComponentDecl>,
    configs: HashMap<String, ConfigValuesData>,
    blockers: HashMap<String, Arc<Mutex<Option<(oneshot::Sender<()>, oneshot::Receiver<()>)>>>>,
}

impl MockResolver {
    pub fn new() -> Self {
        MockResolver {
            components: HashMap::new(),
            configs: HashMap::new(),
            blockers: HashMap::new(),
        }
    }

    async fn resolve_async(
        &self,
        component_url: String,
    ) -> Result<ResolvedComponent, ResolverError> {
        const NAME_PREFIX: &str = "test:///";
        debug_assert!(component_url.starts_with(NAME_PREFIX), "invalid component url");
        let (_, name) = component_url.split_at(NAME_PREFIX.len());
        let decl = self
            .components
            .get(name)
            .ok_or(ResolverError::manifest_not_found(format_err!("not in the hashmap")))?;
        let config_values = if let Some(config_decl) = &decl.config {
            let config_path = match &config_decl.value_source {
                cm_rust::ConfigValueSource::PackagePath(path) => path,
            };
            Some(
                self.configs
                    .get(config_path)
                    .ok_or_else(|| {
                        ResolverError::manifest_invalid(format_err!("config values not provided"))
                    })?
                    .clone(),
            )
        } else {
            None
        };
        let (client, server): (ClientEnd<fio::DirectoryMarker>, ServerEnd<fio::DirectoryMarker>) =
            create_endpoints();

        let sub_dir = pseudo_directory!(
            "fake_file" => read_only(b"content"),
        );
        sub_dir.open(
            ExecutionScope::new(),
            fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::DIRECTORY,
            vfs::path::Path::dot(),
            ServerEnd::new(server.into_channel()),
        );

        if let Some(blocker) = self.blockers.get(name) {
            let mut blocker = blocker.lock().await;
            if let Some(blocker) = blocker.take() {
                let (send, recv) = blocker;
                send.send(()).unwrap();
                let _ = recv.await.unwrap();
            }
        }

        Ok(ResolvedComponent {
            resolved_by: "mocks::MockResolver".into(),
            resolved_url: format!("test:///{}_resolved", name),
            context_to_resolve_children: None,
            decl: decl.clone(),
            package: Some(ResolvedPackage { url: "pkg".to_string(), directory: client }),
            config_values,
            abi_revision: Some(version_history::LATEST_VERSION.abi_revision.clone()),
        })
    }

    pub fn add_component(&mut self, name: &str, component: ComponentDecl) {
        self.components.insert(name.to_string(), component);
    }

    pub fn add_config_values(&mut self, path: &str, values: ConfigValuesData) {
        self.configs.insert(path.to_string(), values);
    }

    pub fn add_blocker(
        &mut self,
        path: &str,
        send: oneshot::Sender<()>,
        recv: oneshot::Receiver<()>,
    ) {
        self.blockers.insert(path.to_string(), Arc::new(Mutex::new(Some((send, recv)))));
    }

    pub fn get_component_decl(&self, name: &str) -> Option<ComponentDecl> {
        self.components.get(name).map(Clone::clone)
    }
}

#[async_trait]
impl Resolver for MockResolver {
    async fn resolve(
        &self,
        component_address: &ComponentAddress,
    ) -> Result<ResolvedComponent, ResolverError> {
        self.resolve_async(component_address.url().to_string()).await
    }
}

pub type HostFn = Box<dyn Fn(ServerEnd<fio::DirectoryMarker>) + Send + Sync>;

pub type ManagedNamespace = Mutex<Vec<fcrunner::ComponentNamespaceEntry>>;

struct MockRunnerInner {
    /// List of URLs started by this runner instance.
    urls_run: Vec<String>,

    /// Vector of waiters that wish to be notified when a new URL is run (used by `wait_for_urls`).
    url_waiters: Vec<futures::channel::oneshot::Sender<()>>,

    /// Namespace for each component, mapping resolved URL to the component's namespace.
    namespaces: HashMap<String, Arc<Mutex<Vec<fcrunner::ComponentNamespaceEntry>>>>,

    /// Functions for serving the `outgoing` and `runtime` directories
    /// of a given compoment. When a component is started, these
    /// functions will be called with the server end of the directories.
    outgoing_host_fns: HashMap<String, Arc<HostFn>>,
    runtime_host_fns: HashMap<String, Arc<HostFn>>,

    /// Set of URLs that the MockRunner will fail the `start` call for.
    failing_urls: HashSet<String>,

    /// Map from the `Koid` of `Channel` owned by a `ComponentController` to
    /// the messages received by that controller.
    runner_requests: Arc<Mutex<HashMap<Koid, Vec<ControlMessage>>>>,

    controllers: HashMap<Koid, AbortHandle>,

    last_checker: Option<ScopedPolicyChecker>,
}

pub struct MockRunner {
    // The internal runner state.
    //
    // Inner state is guarded by a std::sync::Mutex to avoid helper
    // functions needing "async" (and propagating to callers).
    // std::sync::MutexGuard doesn't have the "Send" trait, so the
    // compiler will prevent us calling ".await" while holding the lock.
    inner: SyncMutex<MockRunnerInner>,
}

impl MockRunner {
    pub fn new() -> Self {
        MockRunner {
            inner: SyncMutex::new(MockRunnerInner {
                urls_run: vec![],
                url_waiters: vec![],
                namespaces: HashMap::new(),
                outgoing_host_fns: HashMap::new(),
                runtime_host_fns: HashMap::new(),
                failing_urls: HashSet::new(),
                runner_requests: Arc::new(Mutex::new(HashMap::new())),
                controllers: HashMap::new(),
                last_checker: None,
            }),
        }
    }

    /// Cause the URL `url` to return an error when started.
    pub fn add_failing_url(&self, url: &str) {
        self.inner.lock().unwrap().failing_urls.insert(url.to_string());
    }

    /// Cause the component `name` to return an error when started.
    pub fn cause_failure(&self, name: &str) {
        self.add_failing_url(&format!("test:///{}_resolved", name))
    }

    /// Register `function` to serve the outgoing directory of component `name`
    pub fn add_host_fn(&self, name: &str, function: HostFn) {
        self.inner.lock().unwrap().outgoing_host_fns.insert(name.to_string(), Arc::new(function));
    }

    /// Register `function` to serve the runtime directory of component `name`
    pub fn add_runtime_host_fn(&self, name: &str, function: HostFn) {
        self.inner.lock().unwrap().runtime_host_fns.insert(name.to_string(), Arc::new(function));
    }

    /// Get the input namespace for component `name`.
    pub fn get_namespace(&self, name: &str) -> Option<Arc<ManagedNamespace>> {
        self.inner.lock().unwrap().namespaces.get(name).map(Arc::clone)
    }

    pub fn get_request_map(&self) -> Arc<Mutex<HashMap<Koid, Vec<ControlMessage>>>> {
        self.inner.lock().unwrap().runner_requests.clone()
    }

    /// Returns a future that completes when `expected_url` is launched by the runner.
    pub async fn wait_for_url(&self, expected_url: &str) {
        self.wait_for_urls(&[expected_url]).await
    }

    /// Returns a future that completes when `expected_urls` were launched by the runner, in any
    /// order.
    pub async fn wait_for_urls(&self, expected_urls: &[&str]) {
        loop {
            let (sender, receiver) = oneshot::channel();
            {
                let mut inner = self.inner.lock().unwrap();
                let expected_urls: HashSet<&str> = expected_urls.iter().map(|s| *s).collect();
                let urls_run: HashSet<&str> = inner.urls_run.iter().map(|s| s as &str).collect();
                if expected_urls.is_subset(&urls_run) {
                    return;
                } else {
                    inner.url_waiters.push(sender);
                }
            }
            receiver.await.expect("failed to receive url notice")
        }
    }

    pub fn abort_controller(&self, koid: &Koid) {
        let state = self.inner.lock().unwrap();
        let controller = state.controllers.get(koid).expect("koid was not available");
        controller.abort();
    }

    pub fn last_checker(&self) -> Option<ScopedPolicyChecker> {
        self.inner.lock().unwrap().last_checker.take()
    }
}

impl BuiltinRunnerFactory for MockRunner {
    fn get_scoped_runner(self: Arc<Self>, checker: ScopedPolicyChecker) -> Arc<dyn Runner> {
        {
            let mut state = self.inner.lock().unwrap();
            state.last_checker = Some(checker);
        }
        self
    }
}

#[async_trait]
impl Runner for MockRunner {
    async fn start(
        &self,
        start_info: fcrunner::ComponentStartInfo,
        server_end: ServerEnd<fcrunner::ComponentControllerMarker>,
    ) {
        let outgoing_host_fn;
        let runtime_host_fn;
        let runner_requests;
        let resolved_url = start_info.resolved_url.unwrap();

        // The koid is the only unique piece of information we have about a
        // component start request. Two start requests for the same component
        // URL look identical to the Runner, the only difference being the
        // Channel passed to the Runner to use for the ComponentController
        // protocol.
        let channel_koid = server_end.as_handle_ref().basic_info().expect("basic info failed").koid;
        {
            let mut state = self.inner.lock().unwrap();

            // Trigger a failure if previously requested.
            if state.failing_urls.contains(&resolved_url) {
                let status = zx::Status::UNAVAILABLE;
                server_end.into_channel().close_with_epitaph(status).unwrap();
                return;
            }

            // Fetch host functions, which will provide the outgoing and runtime directories
            // for the component.
            //
            // If functions were not provided, then start_info.outgoing_dir will be
            // automatically closed once it goes out of scope at the end of this
            // function.
            outgoing_host_fn = state.outgoing_host_fns.get(&resolved_url).map(Arc::clone);
            runtime_host_fn = state.runtime_host_fns.get(&resolved_url).map(Arc::clone);
            runner_requests = state.runner_requests.clone();

            // Create a namespace for the component.
            state
                .namespaces
                .insert(resolved_url.clone(), Arc::new(Mutex::new(start_info.ns.unwrap())));

            let abort_handle =
                MockController::new(server_end, runner_requests, channel_koid).serve();
            state.controllers.insert(channel_koid, abort_handle);

            // Start serving the outgoing/runtime directories.
            if let Some(outgoing_host_fn) = outgoing_host_fn {
                outgoing_host_fn(start_info.outgoing_dir.unwrap());
            }
            if let Some(runtime_host_fn) = runtime_host_fn {
                runtime_host_fn(start_info.runtime_dir.unwrap());
            }

            // Record that this URL has been started.
            state.urls_run.push(resolved_url.clone());
            let url_waiters = mem::replace(&mut state.url_waiters, vec![]);
            for waiter in url_waiters {
                waiter.send(()).expect("failed to send url notice");
            }
        }
    }
}

#[derive(Debug, PartialEq, Clone)]
pub enum ControlMessage {
    Stop,
    Kill,
}

#[derive(Clone)]
/// What the MockController should do when it receives a message.
pub struct ControllerActionResponse {
    pub close_channel: bool,
    pub delay: Option<zx::Duration>,
}

pub struct MockController {
    pub messages: Arc<Mutex<HashMap<Koid, Vec<ControlMessage>>>>,
    request_stream: fcrunner::ComponentControllerRequestStream,
    koid: Koid,
    stop_resp: ControllerActionResponse,
    kill_resp: ControllerActionResponse,
}

impl MockController {
    /// Create a `MockController` that listens to the `server_end` and inserts
    /// `ControlMessage`'s into the Vec in the HashMap keyed under the provided
    /// `Koid`. When either a request to stop or kill a component is received
    /// the `MockController` will close the control channel immediately.
    pub fn new(
        server_end: ServerEnd<fcrunner::ComponentControllerMarker>,
        messages: Arc<Mutex<HashMap<Koid, Vec<ControlMessage>>>>,
        koid: Koid,
    ) -> MockController {
        Self::new_with_responses(
            server_end,
            messages,
            koid,
            ControllerActionResponse { close_channel: true, delay: None },
            ControllerActionResponse { close_channel: true, delay: None },
        )
    }

    /// Create a MockController that listens to the `server_end` and inserts
    /// `ControlMessage`'s into the Vec in the HashMap keyed under the provided
    /// `Koid`. The `stop_response` controls the delay used before taking any
    /// action on the control channel when a request to stop is received. The
    /// `kill_respone` provides the same control when the a request to kill is
    /// received.
    pub fn new_with_responses(
        server_end: ServerEnd<fcrunner::ComponentControllerMarker>,
        messages: Arc<Mutex<HashMap<Koid, Vec<ControlMessage>>>>,
        koid: Koid,
        stop_response: ControllerActionResponse,
        kill_response: ControllerActionResponse,
    ) -> MockController {
        MockController {
            messages: messages,
            request_stream: server_end.into_stream().expect("stream conversion failed"),
            koid: koid,
            stop_resp: stop_response,
            kill_resp: kill_response,
        }
    }

    /// Create a future which takes ownership of `server_end` and inserts
    /// `ControlMessage`s into `messages` based on events sent on the
    /// `ComponentController` channel.
    pub fn into_serve_future(mut self) -> impl Future<Output = ()> {
        let job_dup = fuchsia_runtime::job_default()
            .duplicate_handle(zx::Rights::SAME_RIGHTS)
            .expect("duplicate default job");
        self.request_stream
            .control_handle()
            .send_on_publish_diagnostics(ComponentDiagnostics {
                tasks: Some(ComponentTasks {
                    component_task: Some(DiagnosticsTask::Job(job_dup)),
                    ..Default::default()
                }),
                ..Default::default()
            })
            .unwrap_or_else(|e| {
                warn!("sending diagnostics failed: {:?}", e);
            });
        async move {
            self.messages.lock().await.insert(self.koid, Vec::new());
            while let Ok(Some(request)) = self.request_stream.try_next().await {
                match request {
                    fcrunner::ComponentControllerRequest::Stop { control_handle: c } => {
                        self.messages
                            .lock()
                            .await
                            .get_mut(&self.koid)
                            .expect("component channel koid key missing from mock runner map")
                            .push(ControlMessage::Stop);
                        if let Some(delay) = self.stop_resp.delay {
                            let delay_copy = delay.clone();
                            let close_channel = self.stop_resp.close_channel;
                            fasync::Task::spawn(async move {
                                fasync::Timer::new(fasync::Time::after(delay_copy)).await;
                                if close_channel {
                                    c.shutdown_with_epitaph(zx::Status::OK);
                                }
                            })
                            .detach();
                        } else if self.stop_resp.close_channel {
                            c.shutdown_with_epitaph(zx::Status::OK);
                            break;
                        }
                    }
                    fcrunner::ComponentControllerRequest::Kill { control_handle: c } => {
                        self.messages
                            .lock()
                            .await
                            .get_mut(&self.koid)
                            .expect("component channel koid key missing from mock runner map")
                            .push(ControlMessage::Kill);
                        if let Some(delay) = self.kill_resp.delay {
                            let delay_copy = delay.clone();
                            let close_channel = self.kill_resp.close_channel;
                            fasync::Task::spawn(async move {
                                fasync::Timer::new(fasync::Time::after(delay_copy)).await;
                                if close_channel {
                                    c.shutdown_with_epitaph(zx::Status::OK);
                                }
                            })
                            .detach();
                            if self.kill_resp.close_channel {
                                break;
                            }
                        } else if self.kill_resp.close_channel {
                            c.shutdown_with_epitaph(zx::Status::OK);
                            break;
                        }
                    }
                }
            }
        }
    }

    /// Spawn an async execution context which takes ownership of `server_end`
    /// and inserts `ControlMessage`s into `messages` based on events sent on
    /// the `ComponentController` channel. This simply spawns a future which
    /// awaits self.run().
    pub fn serve(self) -> AbortHandle {
        // Listen to the ComponentController server end and record the messages
        // that arrive. Exit after the first one, as this is the contract we
        // have implemented so far. Exiting will cause our handle to the
        // channel to drop and close the channel.

        let (handle, registration) = AbortHandle::new_pair();
        let fut = Abortable::new(self.into_serve_future(), registration);
        // Send default job for the sake of testing only.
        fasync::Task::spawn(async move {
            fut.await.unwrap_or(()); // Ignore cancellation.
        })
        .detach();
        handle
    }
}
