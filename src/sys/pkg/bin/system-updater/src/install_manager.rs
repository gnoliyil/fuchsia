// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::update::{Config, EnvironmentConnector, RebootController, Updater},
    anyhow::anyhow,
    event_queue::{EventQueue, Notify},
    fidl_fuchsia_update_installer::UpdateNotStartedReason,
    fidl_fuchsia_update_installer_ext::State,
    fuchsia_async as fasync, fuchsia_inspect as inspect,
    fuchsia_inspect_contrib::nodes::{BoundedListNode, NodeExt as _},
    fuchsia_zircon as zx,
    futures::{
        channel::{mpsc, oneshot},
        prelude::*,
        select,
        stream::FusedStream,
    },
    std::time::Duration,
    thiserror::Error,
    tracing::{error, warn},
};

const INSPECT_STATUS_NODE_NAME: &str = "status";
// Suspend is allowed at most 7 days, after that update will automatically resume.
const MAX_SUSPEND_DURATION: Duration = Duration::from_secs(7 * 24 * 60 * 60);

/// Start a install manager task that:
///  * Runs an update attempt in a seperate task.
///  * Provides a control handle to forward FIDL requests to the update attempt task.
pub async fn start_install_manager<N, U, E>(
    updater: U,
    node: inspect::Node,
) -> (InstallManagerControlHandle<N>, impl Future<Output = ()>)
where
    N: Notify<Event = State> + Send + 'static,
    U: Updater,
    E: EnvironmentConnector,
{
    let (send, recv) = mpsc::channel(0);
    (InstallManagerControlHandle(send), run::<N, U, E>(recv, updater, node))
}

/// The install manager task.
async fn run<N, U, E>(
    mut recv: mpsc::Receiver<ControlRequest<N>>,
    mut updater: U,
    node: inspect::Node,
) where
    N: Notify<Event = State> + Send + 'static,
    U: Updater,
    E: EnvironmentConnector,
{
    // Set up event queue to keep track of all the Monitors.
    let (monitor_queue_fut, mut monitor_queue) = EventQueue::new();
    let eq_task = fasync::Task::spawn(monitor_queue_fut);

    let mut requests_node = BoundedListNode::new(node.create_child("requests"), 100);

    // Each iteration of this loop is one update attempt.
    loop {
        // There is no active update attempt, so let's wait for a start request.
        let StartRequestData { config, monitor, reboot_controller, responder } =
            match handle_idle_control_request(&mut recv, &mut requests_node).await {
                Some(start_data) => start_data,
                None => {
                    // Ensure all monitors get the remaining state updates.
                    drop(monitor_queue);
                    eq_task.await;
                    return;
                }
            };
        let reboot_controller = reboot_controller.unwrap_or_else(RebootController::unblocked);

        // We connect to FIDL services on each update attempt (rather than once at the
        // beginning) to prevent stale connections.
        let env = match E::connect() {
            Ok(env) => env,
            Err(e) => {
                error!("Error connecting to services: {:#}", anyhow!(e));
                // This fails the update attempt because it drops the responder, which closes
                // the zx channel that we got the start request from.
                continue;
            }
        };

        // Now we can actually start the task that manages the update attempt.
        let update_url = &config.update_url.clone();
        let should_write_recovery = config.should_write_recovery;
        let (attempt_id, attempt_stream) = updater.update(config, env, reboot_controller).await;
        futures::pin_mut!(attempt_stream);

        // Set up inspect nodes.
        let mut status_node = node.create_child(INSPECT_STATUS_NODE_NAME);
        let start_time = zx::Time::get_monotonic();
        let _time_property = node.create_int("start_timestamp_nanos", start_time.into_nanos());

        // Don't forget to add the first monitor to the queue and respond to StartUpdate :)
        if let Err(e) = monitor_queue.add_client(monitor).await {
            warn!("error adding client to monitor queue: {:#}", anyhow!(e));
        }
        let _ = responder.send(Ok(attempt_id.clone()));

        let mut suspend_state = SuspendState::Running;
        let suspend_deadline = start_time + MAX_SUSPEND_DURATION.into();

        // For this update attempt, handle events both from the FIDL server and the update task.
        loop {
            // We use this enum to make the body of the select as short as possible. Otherwise,
            // we'd need to set the crate's recursion_limit to be higher.
            enum Op<N: Notify> {
                Request(ControlRequest<N>),
                Status(Option<State>),
            }
            let op = match suspend_state {
                SuspendState::Suspended => {
                    select! {
                        req = recv.select_next_some() => Op::Request(req),
                        () = fasync::Timer::new(suspend_deadline).fuse() => {
                            suspend_state.resume();
                            continue;
                        }
                    }
                }
                SuspendState::Running => {
                    select! {
                        req = recv.select_next_some() => Op::Request(req),
                        state = attempt_stream.next() => Op::Status(state)
                    }
                }
            };
            match op {
                // We got a FIDL requests (via the mpsc::Receiver).
                Op::Request(req) => {
                    req.write_to_inspect(requests_node.create_entry());
                    handle_active_control_request(
                        req,
                        &mut monitor_queue,
                        &attempt_id,
                        update_url,
                        should_write_recovery,
                        &mut suspend_state,
                        suspend_deadline,
                    )
                    .await
                }
                // The update task has given us a progress update, so let's forward
                // that to all the monitors.
                Op::Status(Some(state)) => {
                    drop(status_node);
                    status_node = node.create_child(INSPECT_STATUS_NODE_NAME);
                    state.write_to_inspect(&status_node);
                    if let Err(e) = monitor_queue.queue_event(state).await {
                        warn!("error sending state to monitor_queue: {:#}", anyhow!(e));
                    }
                }
                // The update task tells us the update is over, so let's notify all monitors.
                Op::Status(None) => {
                    drop(status_node);
                    if let Err(e) = monitor_queue.clear().await {
                        warn!("error clearing clients of monitor_queue: {:#}", anyhow!(e));
                    }
                    break;
                }
            }
        }
    }
}

/// Returns when we get a Start control request (i.e. a StartUpdate FIDL request forwarded
/// from the FIDL server).
async fn handle_idle_control_request<N>(
    recv: &mut mpsc::Receiver<ControlRequest<N>>,
    requests_node: &mut BoundedListNode,
) -> Option<StartRequestData<N>>
where
    N: Notify,
{
    // If the stream of control requests terminated while an update attempt was running,
    // this stream has already yielded None, and so further calls to next() may panic.
    // Instead, check if the stream is terminated via its FusedStream implementation
    // before proceeding.
    if recv.is_terminated() {
        return None;
    }

    // Right now we are in a state where there is no update running.
    while let Some(control_request) = recv.next().await {
        control_request.write_to_inspect(requests_node.create_entry());
        match control_request {
            ControlRequest::Start(start_data) => {
                return Some(start_data);
            }
            ControlRequest::Monitor(MonitorRequestData { responder, .. }) => {
                let _ = responder.send(false);
            }
            ControlRequest::Suspend(responder) => {
                let _ = responder.send(Err(SuspendError::NoUpdateInProgress));
            }
            ControlRequest::Resume(responder) => {
                let _ = responder.send(Err(ResumeError::NoUpdateInProgress));
            }
        }
    }
    None
}

/// Handle the logic for the install manager task will do when receiving a control request
/// while an update is underway.
async fn handle_active_control_request<N>(
    req: ControlRequest<N>,
    monitor_queue: &mut event_queue::ControlHandle<N>,
    attempt_id: &str,
    update_url: &fuchsia_url::AbsolutePackageUrl,
    should_write_recovery: bool,
    suspend_state: &mut SuspendState,
    suspend_deadline: zx::Time,
) where
    N: Notify,
{
    match req {
        ControlRequest::Start(StartRequestData {
            responder,
            config,
            monitor,
            reboot_controller,
        }) => {
            // Only attach monitor if she's compatible with current update check.
            // Note: We can only attach a reboot controller during the FIRST start request.
            // Any subsequent request with a reboot controller should fail.
            if reboot_controller.is_none()
                && config.allow_attach_to_existing_attempt
                && &config.update_url == update_url
                && config.should_write_recovery == should_write_recovery
            {
                if let Err(e) = monitor_queue.add_client(monitor).await {
                    warn!("error adding client to monitor queue: {:#}", anyhow!(e));
                }
                let _ = responder.send(Ok(attempt_id.to_string()));
            } else {
                let _ = responder.send(Err(UpdateNotStartedReason::AlreadyInProgress));
            }
            suspend_state.resume();
        }
        ControlRequest::Monitor(MonitorRequestData { attempt_id: id, monitor, responder }) => {
            // If an attempt ID is provided, ensure it matches the current attempt.
            if let Some(id) = id {
                if id != attempt_id {
                    let _ = responder.send(false);
                    return;
                }
            }

            if let Err(e) = monitor_queue.add_client(monitor).await {
                warn!("error adding client to monitor queue: {:#}", anyhow!(e));
            }
            let _ = responder.send(true);
        }
        ControlRequest::Suspend(responder) => {
            if zx::Time::get_monotonic() > suspend_deadline {
                let _ = responder.send(Err(SuspendError::SuspendLimitExceeded));
                return;
            }
            suspend_state.suspend();
            let _ = responder.send(Ok(()));
        }
        ControlRequest::Resume(responder) => {
            suspend_state.resume();
            let _ = responder.send(Ok(()));
        }
    }
}

/// A handle to forward installer FIDL requests to the install manager task.
#[derive(Clone)]
pub struct InstallManagerControlHandle<N>(mpsc::Sender<ControlRequest<N>>)
where
    N: Notify;

/// Error indicating that the install manager task no longer exists.
#[derive(Debug, Clone, thiserror::Error, PartialEq, Eq)]
#[error("install manager dropped before all its control handles")]
pub struct InstallManagerGone;

impl From<mpsc::SendError> for InstallManagerGone {
    fn from(_: mpsc::SendError) -> Self {
        InstallManagerGone
    }
}

impl From<oneshot::Canceled> for InstallManagerGone {
    fn from(_: oneshot::Canceled) -> Self {
        InstallManagerGone
    }
}

/// This can be used to forward requests to the install manager task.
impl<N> InstallManagerControlHandle<N>
where
    N: Notify,
{
    /// Forward StartUpdate requests to the install manager task.
    pub async fn start_update(
        &mut self,
        config: Config,
        monitor: N,
        reboot_controller: Option<RebootController>,
    ) -> Result<Result<String, UpdateNotStartedReason>, InstallManagerGone> {
        let (responder, receive_response) = oneshot::channel();
        self.0
            .send(ControlRequest::Start(StartRequestData {
                config,
                monitor,
                reboot_controller,
                responder,
            }))
            .await?;
        Ok(receive_response.await?)
    }

    /// Forward MonitorUpdate requests to the install manager task.
    pub async fn monitor_update(
        &mut self,
        attempt_id: Option<String>,
        monitor: N,
    ) -> Result<bool, InstallManagerGone> {
        let (responder, receive_response) = oneshot::channel();
        self.0
            .send(ControlRequest::Monitor(MonitorRequestData { attempt_id, monitor, responder }))
            .await?;
        Ok(receive_response.await?)
    }

    /// Forward SuspendUpdate requests to the install manager task.
    // TODO(fxbug.dev/125721): use this
    #[allow(dead_code)]
    pub async fn suspend_update(&mut self) -> Result<Result<(), SuspendError>, InstallManagerGone> {
        let (responder, receive_response) = oneshot::channel();
        self.0.send(ControlRequest::Suspend(responder)).await?;
        Ok(receive_response.await?)
    }

    /// Forward ResumeUpdate requests to the install manager task.
    // TODO(fxbug.dev/125721): use this
    #[allow(dead_code)]
    pub async fn resume_update(&mut self) -> Result<Result<(), ResumeError>, InstallManagerGone> {
        let (responder, receive_response) = oneshot::channel();
        self.0.send(ControlRequest::Resume(responder)).await?;
        Ok(receive_response.await?)
    }
}

/// Requests that can be forwarded to the install manager task.
enum ControlRequest<N>
where
    N: Notify,
{
    Start(StartRequestData<N>),
    Monitor(MonitorRequestData<N>),
    Suspend(oneshot::Sender<Result<(), SuspendError>>),
    Resume(oneshot::Sender<Result<(), ResumeError>>),
}

impl<N: Notify> ControlRequest<N> {
    fn write_to_inspect(&self, node: &inspect::Node) {
        node.record_time("time");

        match self {
            Self::Start(data) => {
                node.record_string("request", "start");
                node.record_string("config", format!("{:?}", data.config));
            }
            Self::Monitor(data) => {
                node.record_string("request", "monitor");
                if let Some(attempt_id) = &data.attempt_id {
                    node.record_string("attempt id", attempt_id);
                }
            }
            Self::Suspend(_) => {
                node.record_string("request", "suspend");
            }
            Self::Resume(_) => {
                node.record_string("request", "resume");
            }
        }
    }
}

struct StartRequestData<N>
where
    N: Notify,
{
    config: Config,
    monitor: N,
    reboot_controller: Option<RebootController>,
    responder: oneshot::Sender<Result<String, UpdateNotStartedReason>>,
}

struct MonitorRequestData<N>
where
    N: Notify,
{
    attempt_id: Option<String>,
    monitor: N,
    responder: oneshot::Sender<bool>,
}

#[derive(Debug, Error, PartialEq, Eq)]
pub enum SuspendError {
    #[error("no update in progress")]
    NoUpdateInProgress,
    #[error("suspend time exceeded max limit")]
    SuspendLimitExceeded,
}

#[derive(Debug, Error, PartialEq, Eq)]
pub enum ResumeError {
    #[error("no update in progress")]
    NoUpdateInProgress,
}

enum SuspendState {
    Suspended,
    Running,
}

impl SuspendState {
    fn suspend(&mut self) {
        *self = Self::Suspended
    }

    fn resume(&mut self) {
        *self = Self::Running
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::update::{
            ConfigBuilder, Environment, NamespaceBuildInfo, NamespaceCobaltConnector,
            NamespaceSystemInfo,
        },
        async_trait::async_trait,
        event_queue::ClosedClient,
        fidl_fuchsia_hardware_power_statecontrol::AdminMarker as PowerStateControlMarker,
        fidl_fuchsia_paver::{BootManagerMarker, DataSinkMarker},
        fidl_fuchsia_pkg::{PackageCacheMarker, PackageResolverMarker, RetainedPackagesMarker},
        fidl_fuchsia_space::ManagerMarker as SpaceManagerMarker,
        fidl_fuchsia_update_installer_ext::{
            PrepareFailureReason, Progress, UpdateInfo, UpdateInfoAndProgress,
        },
        fuchsia_inspect::{assert_data_tree, testing::AnyProperty, Inspector},
        mpsc::{Receiver, Sender},
        parking_lot::Mutex,
        std::{sync::Arc, task::Poll},
    };

    const CALLBACK_CHANNEL_SIZE: usize = 20;

    struct FakeStateNotifier {
        sender: Arc<Mutex<Sender<State>>>,
    }
    impl FakeStateNotifier {
        fn new_callback_and_receiver() -> (Self, Receiver<State>) {
            let (sender, receiver) = mpsc::channel(CALLBACK_CHANNEL_SIZE);
            (Self { sender: Arc::new(Mutex::new(sender)) }, receiver)
        }
    }
    impl Notify for FakeStateNotifier {
        type Event = State;
        type NotifyFuture = future::Ready<Result<(), ClosedClient>>;
        fn notify(&self, state: State) -> Self::NotifyFuture {
            self.sender.lock().try_send(state).expect("FakeStateNotifier failed to send state");
            future::ready(Ok(()))
        }
    }

    struct StubEnvironmentConnector;
    impl EnvironmentConnector for StubEnvironmentConnector {
        fn connect() -> Result<Environment, anyhow::Error> {
            let (data_sink, _) = fidl::endpoints::create_proxy::<DataSinkMarker>().unwrap();
            let (boot_manager, _) = fidl::endpoints::create_proxy::<BootManagerMarker>().unwrap();
            let (pkg_resolver, _) =
                fidl::endpoints::create_proxy::<PackageResolverMarker>().unwrap();
            let (pkg_cache, _) = fidl::endpoints::create_proxy::<PackageCacheMarker>().unwrap();
            let (retained_packages, _) =
                fidl::endpoints::create_proxy::<RetainedPackagesMarker>().unwrap();
            let (space_manager, _) = fidl::endpoints::create_proxy::<SpaceManagerMarker>().unwrap();
            let (power_state_control, _) =
                fidl::endpoints::create_proxy::<PowerStateControlMarker>().unwrap();

            Ok(Environment {
                data_sink,
                boot_manager,
                pkg_resolver,
                pkg_cache,
                retained_packages,
                space_manager,
                power_state_control,
                build_info: NamespaceBuildInfo,
                cobalt_connector: NamespaceCobaltConnector,
                system_info: NamespaceSystemInfo,
            })
        }
    }

    struct FakeUpdater(mpsc::Receiver<(String, mpsc::Receiver<State>)>);
    impl FakeUpdater {
        fn new(receiver: mpsc::Receiver<(String, mpsc::Receiver<State>)>) -> Self {
            Self(receiver)
        }
    }

    #[async_trait(?Send)]
    impl Updater for FakeUpdater {
        type UpdateStream = mpsc::Receiver<State>;

        async fn update(
            &mut self,
            _config: Config,
            _env: Environment,
            _reboot_controller: RebootController,
        ) -> (String, Self::UpdateStream) {
            self.0.next().await.unwrap()
        }
    }

    async fn start_install_manager_with_update_id(
        id: &str,
    ) -> (
        InstallManagerControlHandle<FakeStateNotifier>,
        fasync::Task<()>,
        mpsc::Sender<(String, mpsc::Receiver<State>)>,
        mpsc::Sender<State>,
    ) {
        let inspector = Inspector::default();
        let node = inspector.root().create_child("test_does_not_use_inspect");
        start_install_manager_with_update_id_and_node(id, node).await
    }

    async fn start_install_manager_with_update_id_and_node(
        id: &str,
        node: inspect::Node,
    ) -> (
        InstallManagerControlHandle<FakeStateNotifier>,
        fasync::Task<()>,
        mpsc::Sender<(String, mpsc::Receiver<State>)>,
        mpsc::Sender<State>,
    ) {
        // We use this channel to send the attempt id and state receiver to the update task, for
        // each update attempt. This allows tests to control when an update attempt ends -- all they
        // need to do is drop the state sender.
        let (mut updater_sender, updater_receiver) = mpsc::channel(0);
        let updater = FakeUpdater::new(updater_receiver);
        let (state_sender, state_receiver) = mpsc::channel(0);
        let (install_manager_ch, fut) =
            start_install_manager::<FakeStateNotifier, FakeUpdater, StubEnvironmentConnector>(
                updater, node,
            )
            .await;
        let install_manager_task = fasync::Task::local(fut);

        // We just use try_send because send calls are blocked on the receiver receiving the
        // event... and the receiver won't receive the event until we do a start_update request.
        updater_sender.try_send((id.to_string(), state_receiver)).unwrap();

        (install_manager_ch, install_manager_task, updater_sender, state_sender)
    }

    #[fasync::run_singlethreaded(test)]
    async fn monitor_update_fails_when_no_update_running() {
        let (mut install_manager_ch, _install_manager_task, _updater_sender, _state_sender) =
            start_install_manager_with_update_id("my-attempt").await;
        let (notifier0, _state_receiver0) = FakeStateNotifier::new_callback_and_receiver();
        let (notifier1, _state_receiver1) = FakeStateNotifier::new_callback_and_receiver();

        assert_eq!(install_manager_ch.monitor_update(None, notifier0).await, Ok(false));
        assert_eq!(
            install_manager_ch.monitor_update(Some("my-attempt".to_string()), notifier1).await,
            Ok(false)
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn monitor_update_fails_with_wrong_id() {
        let (mut install_manager_ch, _install_manager_task, _updater_sender, _state_sender) =
            start_install_manager_with_update_id("my-attempt").await;
        let (notifier0, _state_receiver0) = FakeStateNotifier::new_callback_and_receiver();
        let (notifier1, _state_receiver1) = FakeStateNotifier::new_callback_and_receiver();

        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier0, None)
                .await,
            Ok(Ok("my-attempt".to_string()))
        );

        assert_eq!(
            install_manager_ch.monitor_update(Some("unknown id".to_string()), notifier1).await,
            Ok(false)
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn monitor_connects_via_start_update() {
        let (mut install_manager_ch, _install_manager_task, _updater_sender, mut state_sender) =
            start_install_manager_with_update_id("my-attempt").await;
        let (notifier, state_receiver) = FakeStateNotifier::new_callback_and_receiver();
        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier, None)
                .await,
            Ok(Ok("my-attempt".to_string()))
        );

        let () = state_sender.send(State::Prepare).await.unwrap();
        let () =
            state_sender.send(State::FailPrepare(PrepareFailureReason::Internal)).await.unwrap();
        drop(state_sender);

        assert_eq!(
            state_receiver.collect::<Vec<State>>().await,
            vec![State::Prepare, State::FailPrepare(PrepareFailureReason::Internal)]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn monitor_update_succeeds() {
        let (mut install_manager_ch, _install_manager_task, _updater_sender, mut state_sender) =
            start_install_manager_with_update_id("my-attempt").await;
        let (notifier0, state_receiver0) = FakeStateNotifier::new_callback_and_receiver();
        let (notifier1, state_receiver1) = FakeStateNotifier::new_callback_and_receiver();
        let (notifier2, state_receiver2) = FakeStateNotifier::new_callback_and_receiver();

        // Show we can successfuly add each notifier via either start_update or monitor_update.
        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier0, None)
                .await,
            Ok(Ok("my-attempt".to_string()))
        );
        assert_eq!(install_manager_ch.monitor_update(None, notifier1).await, Ok(true));
        assert_eq!(
            install_manager_ch.monitor_update(Some("my-attempt".to_string()), notifier2).await,
            Ok(true)
        );

        // Send state updates to the update task.
        let () = state_sender.send(State::Prepare).await.unwrap();
        let () =
            state_sender.send(State::FailPrepare(PrepareFailureReason::Internal)).await.unwrap();
        drop(state_sender);

        // Each monitor should get these events.
        assert_eq!(
            state_receiver0.collect::<Vec<State>>().await,
            vec![State::Prepare, State::FailPrepare(PrepareFailureReason::Internal)]
        );
        assert_eq!(
            state_receiver1.collect::<Vec<State>>().await,
            vec![State::Prepare, State::FailPrepare(PrepareFailureReason::Internal)]
        );
        assert_eq!(
            state_receiver2.collect::<Vec<State>>().await,
            vec![State::Prepare, State::FailPrepare(PrepareFailureReason::Internal)]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn succeed_additional_start_requests_when_compatible() {
        let (mut install_manager_ch, _install_manager_task, _updater_sender, mut state_sender) =
            start_install_manager_with_update_id("my-attempt").await;
        let (notifier0, state_receiver0) = FakeStateNotifier::new_callback_and_receiver();
        let (notifier1, state_receiver1) = FakeStateNotifier::new_callback_and_receiver();
        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier0, None)
                .await,
            Ok(Ok("my-attempt".to_string()))
        );

        // The second start_update request is acceptable because the config is compatible.
        assert_eq!(
            install_manager_ch
                .start_update(
                    ConfigBuilder::new().allow_attach_to_existing_attempt(true).build().unwrap(),
                    notifier1,
                    None
                )
                .await,
            Ok(Ok("my-attempt".to_string()))
        );

        // Send state updates to the update task, then end the update by dropping the sender.
        let () = state_sender.send(State::Prepare).await.unwrap();
        let () =
            state_sender.send(State::FailPrepare(PrepareFailureReason::Internal)).await.unwrap();
        drop(state_sender);

        // Each monitor should get these events.
        assert_eq!(
            state_receiver0.collect::<Vec<State>>().await,
            vec![State::Prepare, State::FailPrepare(PrepareFailureReason::Internal)]
        );
        assert_eq!(
            state_receiver1.collect::<Vec<State>>().await,
            vec![State::Prepare, State::FailPrepare(PrepareFailureReason::Internal)]
        );
    }

    #[test]
    fn suspend_resume_succeeds() {
        let mut exec = fasync::TestExecutor::new();
        let inspector = Inspector::default();
        let (mut install_manager_ch, _install_manager_task, _updater_sender, mut state_sender) =
            exec.run_singlethreaded(start_install_manager_with_update_id_and_node(
                "my-attempt",
                inspector.root().create_child("current_attempt"),
            ));
        let (notifier, mut state_receiver) = FakeStateNotifier::new_callback_and_receiver();

        exec.run_singlethreaded(async {
            assert_eq!(
                install_manager_ch
                    .start_update(ConfigBuilder::new().build().unwrap(), notifier, None)
                    .await,
                Ok(Ok("my-attempt".to_string()))
            );

            let () = state_sender.send(State::Prepare).await.unwrap();
            assert_eq!(state_receiver.next().await, Some(State::Prepare));

            assert_eq!(install_manager_ch.suspend_update().await, Ok(Ok(())));
        });

        // send and receive states are pending once suspended
        let mut send_fut = state_sender.send(State::FailPrepare(PrepareFailureReason::Internal));
        assert_eq!(exec.run_until_stalled(&mut send_fut), Poll::Pending);

        let mut recv_fut = state_receiver.next();
        assert_eq!(exec.run_until_stalled(&mut recv_fut), Poll::Pending);

        exec.run_singlethreaded(async {
            assert_eq!(install_manager_ch.resume_update().await, Ok(Ok(())));

            // can send and receive states after resume
            let () = send_fut.await.unwrap();
            assert_eq!(recv_fut.await, Some(State::FailPrepare(PrepareFailureReason::Internal)));
        });

        assert_data_tree!(
            inspector,
            root: {
                current_attempt: contains {
                    requests: {
                        "0": {
                            request: "start",
                            config: AnyProperty,
                            time: AnyProperty,
                        },
                        "1": {
                            request: "suspend",
                            time: AnyProperty,
                        },
                        "2": {
                            request: "resume",
                            time: AnyProperty,
                        },
                    },
                }
            }
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn suspend_resume_errors() {
        let (mut install_manager_ch, _install_manager_task, _updater_sender, mut state_sender) =
            start_install_manager_with_update_id("my-attempt").await;

        assert_eq!(
            install_manager_ch.suspend_update().await,
            Ok(Err(SuspendError::NoUpdateInProgress))
        );

        assert_eq!(
            install_manager_ch.resume_update().await,
            Ok(Err(ResumeError::NoUpdateInProgress))
        );

        let (notifier, state_receiver) = FakeStateNotifier::new_callback_and_receiver();
        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier, None)
                .await,
            Ok(Ok("my-attempt".to_string()))
        );

        let () = state_sender.send(State::Prepare).await.unwrap();

        assert_eq!(install_manager_ch.suspend_update().await, Ok(Ok(())));
        assert_eq!(install_manager_ch.resume_update().await, Ok(Ok(())));

        let () =
            state_sender.send(State::FailPrepare(PrepareFailureReason::Internal)).await.unwrap();
        drop(state_sender);

        assert_eq!(
            state_receiver.collect::<Vec<State>>().await,
            vec![State::Prepare, State::FailPrepare(PrepareFailureReason::Internal)]
        );

        assert_eq!(
            install_manager_ch.suspend_update().await,
            Ok(Err(SuspendError::NoUpdateInProgress))
        );

        assert_eq!(
            install_manager_ch.resume_update().await,
            Ok(Err(ResumeError::NoUpdateInProgress))
        );
    }

    #[test]
    fn suspend_exceed_max_duration() {
        let mut exec = fasync::TestExecutor::new();
        let (mut install_manager_ch, _install_manager_task, _updater_sender, mut state_sender) =
            exec.run_singlethreaded(start_install_manager_with_update_id("my-attempt"));
        let (notifier, mut state_receiver) = FakeStateNotifier::new_callback_and_receiver();

        exec.run_singlethreaded(async {
            assert_eq!(
                install_manager_ch
                    .start_update(ConfigBuilder::new().build().unwrap(), notifier, None)
                    .await,
                Ok(Ok("my-attempt".to_string()))
            );

            let () = state_sender.send(State::Prepare).await.unwrap();
            assert_eq!(state_receiver.next().await, Some(State::Prepare));

            assert_eq!(install_manager_ch.suspend_update().await, Ok(Ok(())));
        });

        // send and receive states are pending once suspended
        let mut send_fut = state_sender.send(State::FailPrepare(PrepareFailureReason::Internal));
        assert_eq!(exec.run_until_stalled(&mut send_fut), Poll::Pending);

        let mut recv_fut = state_receiver.next();
        assert_eq!(exec.run_until_stalled(&mut recv_fut), Poll::Pending);

        let wait_duration = exec.wake_next_timer().unwrap() - exec.now();
        assert!(wait_duration > (MAX_SUSPEND_DURATION - Duration::from_secs(1)).into());
        assert!(wait_duration < (MAX_SUSPEND_DURATION + Duration::from_secs(1)).into());

        // update should be resumed
        exec.run_singlethreaded(async {
            let () = send_fut.await.unwrap();
            assert_eq!(recv_fut.await, Some(State::FailPrepare(PrepareFailureReason::Internal)));
        });
    }

    #[fasync::run_singlethreaded(test)]
    async fn fail_additional_start_requests_when_config_incompatible() {
        let (mut install_manager_ch, _install_manager_task, mut updater_sender, state_sender0) =
            start_install_manager_with_update_id("first-attempt-id").await;
        let (notifier0, mut state_receiver0) = FakeStateNotifier::new_callback_and_receiver();
        let (notifier1, _state_receiver1) = FakeStateNotifier::new_callback_and_receiver();
        let (notifier2, _state_receiver2) = FakeStateNotifier::new_callback_and_receiver();
        let (notifier3, _state_receiver3) = FakeStateNotifier::new_callback_and_receiver();
        let (notifier4, _state_receiver4) = FakeStateNotifier::new_callback_and_receiver();

        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier0, None)
                .await,
            Ok(Ok("first-attempt-id".to_string()))
        );

        // Fails because allow_attach_to_existing_attempt is false.
        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier1, None)
                .await,
            Ok(Err(UpdateNotStartedReason::AlreadyInProgress))
        );

        // Fails because different update URL.
        assert_eq!(
            install_manager_ch
                .start_update(
                    ConfigBuilder::new()
                        .update_url("fuchsia-pkg://fuchsia.test/different-url")
                        .allow_attach_to_existing_attempt(true)
                        .build()
                        .unwrap(),
                    notifier2,
                    None
                )
                .await,
            Ok(Err(UpdateNotStartedReason::AlreadyInProgress))
        );

        // Fails because incompatible configs (i.e. should_write_recovery is different).
        assert_eq!(
            install_manager_ch
                .start_update(
                    ConfigBuilder::new()
                        .allow_attach_to_existing_attempt(true)
                        .should_write_recovery(false)
                        .build()
                        .unwrap(),
                    notifier3,
                    None
                )
                .await,
            Ok(Err(UpdateNotStartedReason::AlreadyInProgress))
        );

        // Fails because we can't attach reboot controller in second start request.
        assert_eq!(
            install_manager_ch
                .start_update(
                    ConfigBuilder::new().allow_attach_to_existing_attempt(true).build().unwrap(),
                    notifier4,
                    Some(RebootController::unblocked()),
                )
                .await,
            Ok(Err(UpdateNotStartedReason::AlreadyInProgress))
        );

        // End the current update attempt.
        drop(state_sender0);
        assert_eq!(state_receiver0.next().await, None);

        // Set what update() should return in the second attempt.
        let (_state_sender1, recv) = mpsc::channel(0);
        updater_sender.try_send(("second-attempt-id".to_string(), recv)).unwrap();

        // Now that there's no update in progress, start_update should work regardless of config.
        let (notifier5, _state_receiver5) = FakeStateNotifier::new_callback_and_receiver();
        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier5, None)
                .await,
            Ok(Ok("second-attempt-id".to_string()))
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn update_attempt_finishes_after_dropping_control_handle() {
        let (mut install_manager_ch, install_manager_task, _updater_sender, mut state_sender) =
            start_install_manager_with_update_id("my-attempt").await;
        let (notifier, state_receiver) = FakeStateNotifier::new_callback_and_receiver();

        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier, None)
                .await,
            Ok(Ok("my-attempt".to_string()))
        );

        // Close the channel that sends ControlRequests to the update manager task.
        drop(install_manager_ch);

        // Even though the ControlRequest channel was dropped, the update attempt should still
        // be running it should be able to receive state events from the monitor stream.
        let () = state_sender.send(State::Prepare).await.unwrap();
        let () =
            state_sender.send(State::FailPrepare(PrepareFailureReason::Internal)).await.unwrap();

        // Even if we drop the sender (which ends the update attempt), the state receivers
        // should still receive all the events we've sent up until this point.
        drop(state_sender);
        assert_eq!(
            state_receiver.collect::<Vec<State>>().await,
            vec![State::Prepare, State::FailPrepare(PrepareFailureReason::Internal)]
        );

        // Ensures the update manager task stops after it sends the buffered state events to monitors.
        install_manager_task.await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn writes_status_update_to_inspect() {
        let inspector = Inspector::default();
        let (mut install_manager_ch, _install_manager_task, _updater_sender, mut state_sender) =
            start_install_manager_with_update_id_and_node(
                "my-attempt",
                inspector.root().create_child("current_attempt"),
            )
            .await;
        let (notifier, mut state_receiver) = FakeStateNotifier::new_callback_and_receiver();
        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier, None)
                .await,
            Ok(Ok("my-attempt".to_string()))
        );

        let () = state_sender.send(State::Prepare).await.unwrap();

        // Note for inspect tests: it is very important that we read the state from monitors
        // to prevent race conditions. We can only guarantee inspect state is written once the
        // status update is forwarded to monitors.
        let _ = state_receiver.next().await;

        assert_data_tree!(
            inspector,
            root: {
                current_attempt: {
                    start_timestamp_nanos: AnyProperty,
                    status: {
                        state: "prepare"
                    },
                    requests: {
                        "0": {
                            request: "start",
                            config: AnyProperty,
                            time: AnyProperty,
                        },
                    },
                }
            }
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn writes_newest_status_update_to_inspect() {
        let inspector = Inspector::default();
        let (mut install_manager_ch, _install_manager_task, _updater_sender, mut state_sender) =
            start_install_manager_with_update_id_and_node(
                "my-attempt",
                inspector.root().create_child("current_attempt"),
            )
            .await;
        let (notifier, mut state_receiver) = FakeStateNotifier::new_callback_and_receiver();
        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier, None)
                .await,
            Ok(Ok("my-attempt".to_string()))
        );

        let () = state_sender.send(State::Prepare).await.unwrap();
        let () = state_sender
            .send(State::Fetch(
                UpdateInfoAndProgress::builder()
                    .info(UpdateInfo::builder().download_size(100).build())
                    .progress(
                        Progress::builder().fraction_completed(0.5).bytes_downloaded(50).build(),
                    )
                    .build(),
            ))
            .await
            .unwrap();
        let _ = state_receiver.next().await;
        let _ = state_receiver.next().await;

        assert_data_tree!(
            inspector,
            root: {
                current_attempt: {
                    start_timestamp_nanos: AnyProperty,
                    status: {
                        state: "fetch",
                        info: {
                            download_size: 100u64,
                        },
                        progress: {
                            fraction_completed: 0.5,
                            bytes_downloaded: 50u64,
                        },
                    },
                    requests: {
                        "0": {
                            request: "start",
                            config: AnyProperty,
                            time: AnyProperty,
                        },
                    },
                }
            }
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn writes_status_update_to_inspect_on_second_attempt() {
        let inspector = Inspector::default();
        let (mut install_manager_ch, _install_manager_task, mut updater_sender, mut state_sender) =
            start_install_manager_with_update_id_and_node(
                "first-attempt-id",
                inspector.root().create_child("current_attempt"),
            )
            .await;
        let (notifier, mut state_receiver) = FakeStateNotifier::new_callback_and_receiver();

        // Start first update attempt and show status node is populated.
        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier, None)
                .await,
            Ok(Ok("first-attempt-id".to_string()))
        );
        let () = state_sender.send(State::Prepare).await.unwrap();
        let _ = state_receiver.next().await;
        assert_data_tree!(
            inspector,
            root: {
                current_attempt: {
                    start_timestamp_nanos: AnyProperty,
                    status: {
                        state: "prepare"
                    },
                    requests: {
                        "0": {
                            request: "start",
                            config: AnyProperty,
                            time: AnyProperty,
                        },
                    },
                }
            }
        );

        // End the first update attempt, show status node is removed.
        drop(state_sender);
        let _ = state_receiver.next().await;
        assert_data_tree!(
            inspector,
            root: {
                current_attempt: {
                    requests: {
                        "0": {
                            request: "start",
                            config: AnyProperty,
                            time: AnyProperty,
                        },
                    },
                }
            }
        );

        // Start second update attempt and show status node is once again populated.
        let (mut state_sender1, recv) = mpsc::channel(0);
        updater_sender.try_send(("second-attempt-id".to_string(), recv)).unwrap();
        let (notifier, mut state_receiver1) = FakeStateNotifier::new_callback_and_receiver();
        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier, None)
                .await,
            Ok(Ok("second-attempt-id".to_string()))
        );
        let () =
            state_sender1.send(State::FailPrepare(PrepareFailureReason::Internal)).await.unwrap();
        let _ = state_receiver1.next().await;
        assert_data_tree!(
            inspector,
            root: {
                current_attempt: {
                    start_timestamp_nanos: AnyProperty,
                    status: {
                        state: "fail_prepare",
                        reason: "Internal",
                    },
                    requests: {
                        "0": {
                            request: "start",
                            config: AnyProperty,
                            time: AnyProperty,
                        },
                        "1": {
                            request: "start",
                            config: AnyProperty,
                            time: AnyProperty,
                        },
                    },
                }
            }
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn writes_empty_attempt_node_if_no_attempt_running() {
        let inspector = Inspector::default();
        let (mut _install_manager_ch, _install_manager_task, _updater_sender, _state_sender) =
            start_install_manager_with_update_id_and_node(
                "my-attempt",
                inspector.root().create_child("current_attempt"),
            )
            .await;

        assert_data_tree!(
            inspector,
            root: {
                current_attempt: {}
            }
        );
    }

    /// The update attempt has started (so we should have a status node), but
    /// we haven't gotten a status update (so the said node should be empty).
    #[fasync::run_singlethreaded(test)]
    async fn writes_empty_status_node() {
        let inspector = Inspector::default();
        let (mut install_manager_ch, _install_manager_task, _updater_sender, _state_sender) =
            start_install_manager_with_update_id_and_node(
                "my-attempt",
                inspector.root().create_child("current_attempt"),
            )
            .await;
        let (notifier, _state_receiver) = FakeStateNotifier::new_callback_and_receiver();
        assert_eq!(
            install_manager_ch
                .start_update(ConfigBuilder::new().build().unwrap(), notifier, None)
                .await,
            Ok(Ok("my-attempt".to_string()))
        );

        assert_data_tree!(
            inspector,
            root: {
                current_attempt: {
                    start_timestamp_nanos: AnyProperty,
                    status: {},
                    requests: {
                        "0": {
                            request: "start",
                            config: AnyProperty,
                            time: AnyProperty,
                        },
                    },
                }
            }
        );
    }
}
