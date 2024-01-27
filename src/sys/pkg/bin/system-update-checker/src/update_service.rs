// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::channel::TargetChannelManager;
use crate::connect::ServiceConnector;
use crate::update_manager::{
    RealCommitQuerier, RealUpdateApplier, RealUpdateChecker, UpdateManager,
    UpdateManagerControlHandle,
};
use anyhow::{Context as _, Error};
use event_queue::{ClosedClient, ControlHandle, Notify};
use fidl::endpoints::ClientEnd;
use fidl_fuchsia_update::{
    AttemptsMonitorMarker, CheckNotStartedReason, ManagerRequest, ManagerRequestStream,
};
use fidl_fuchsia_update_ext::{AttemptOptions, CheckOptions, State};
use futures::prelude::*;
use std::convert::TryInto;

pub type RealTargetChannelUpdater = TargetChannelManager<ServiceConnector>;
pub type RealUpdateManager = UpdateManager<
    RealTargetChannelUpdater,
    RealUpdateChecker,
    RealUpdateApplier,
    RealStateNotifier,
    RealCommitQuerier,
    RealAttemptNotifier,
>;

#[derive(Clone)]
pub struct UpdateService {
    update_manager: UpdateManagerControlHandle<RealStateNotifier, RealAttemptNotifier>,
}

impl UpdateService {
    pub fn new(
        update_manager: UpdateManagerControlHandle<RealStateNotifier, RealAttemptNotifier>,
    ) -> Self {
        Self { update_manager }
    }
}

impl UpdateService {
    pub async fn handle_request_stream(
        &mut self,
        mut request_stream: ManagerRequestStream,
    ) -> Result<(), Error> {
        while let Some(event) =
            request_stream.try_next().await.context("error extracting request from stream")?
        {
            match event {
                ManagerRequest::CheckNow { options, monitor, responder } => {
                    let options = match options.try_into().context("invalid CheckNow options") {
                        Ok(options) => options,
                        Err(e) => {
                            // Notify the client they provided invalid options and stop serving on
                            // this channel.
                            responder
                                .send(Err(CheckNotStartedReason::InvalidOptions))
                                .context("error sending CheckNow response")?;
                            return Err(e);
                        }
                    };

                    let monitor = if let Some(monitor) = monitor {
                        Some(RealStateNotifier {
                            proxy: monitor.into_proxy().context("CheckNow monitor into_proxy")?,
                        })
                    } else {
                        None
                    };

                    let res = self.handle_check_now(options, monitor).await;
                    responder.send(res).context("error sending CheckNow response")?;
                }

                ManagerRequest::PerformPendingReboot { responder } => {
                    responder.send(false).context("error sending PerformPendingReboot response")?;
                }

                ManagerRequest::MonitorAllUpdateChecks {
                    attempts_monitor,
                    control_handle: _control_handle,
                } => self.handle_monitor_all_updates(attempts_monitor).await?,
            }
        }
        Ok(())
    }

    async fn handle_check_now(
        &mut self,
        options: CheckOptions,
        monitor: Option<RealStateNotifier>,
    ) -> Result<(), CheckNotStartedReason> {
        self.update_manager.try_start_update(options, monitor).await
    }

    async fn handle_monitor_all_updates(
        &mut self,
        attempts_monitor: ClientEnd<AttemptsMonitorMarker>,
    ) -> Result<(), Error> {
        let proxy = attempts_monitor.into_proxy().context("MonitorAllUpdates into proxy")?;
        let callback =
            Box::new(move |control_handle| RealAttemptNotifier { proxy, control_handle });
        self.update_manager.handle_all_these_updates(callback).await;
        Ok(())
    }
}

pub struct RealStateNotifier {
    proxy: fidl_fuchsia_update::MonitorProxy,
}

impl Notify for RealStateNotifier {
    type Event = State;
    type NotifyFuture = futures::future::Map<
        <fidl_fuchsia_update::MonitorProxy as fidl_fuchsia_update::MonitorProxyInterface>::OnStateResponseFut,
        fn(Result<(), fidl::Error>) -> Result<(), ClosedClient>
    >;

    fn notify(&self, state: State) -> Self::NotifyFuture {
        self.proxy.on_state(&state.into()).map(|result| result.map_err(|_| ClosedClient))
    }
}

pub struct RealAttemptNotifier {
    proxy: fidl_fuchsia_update::AttemptsMonitorProxy,
    control_handle: ControlHandle<RealStateNotifier>,
}

impl Notify for RealAttemptNotifier {
    type Event = AttemptOptions;
    type NotifyFuture = futures::future::BoxFuture<'static, Result<(), ClosedClient>>;

    fn notify(&self, options: fidl_fuchsia_update_ext::AttemptOptions) -> Self::NotifyFuture {
        let mut update_attempt_event_queue = self.control_handle.clone();
        let proxy = self.proxy.clone();

        async move {
            let (monitor_proxy, monitor_server_end) =
                fidl::endpoints::create_proxy::<fidl_fuchsia_update::MonitorMarker>()
                    .map_err(|_| ClosedClient)?;
            update_attempt_event_queue
                .add_client(RealStateNotifier { proxy: monitor_proxy })
                .await
                .map_err(|_| ClosedClient)?;
            proxy.on_start(&options.into(), monitor_server_end).await.map_err(|_| ClosedClient)
        }
        .boxed()
    }
}

#[allow(clippy::bool_assert_comparison)]
#[cfg(test)]
mod tests {
    use super::*;
    use crate::update_manager::tests::{
        BlockingUpdateChecker, FakeCommitQuerier, FakeTargetChannelUpdater, FakeUpdateApplier,
        FakeUpdateChecker, LATEST_SYSTEM_IMAGE,
    };
    use crate::update_manager::{
        CommitQuerier, TargetChannelUpdater, UpdateApplier, UpdateChecker,
    };
    use assert_matches::assert_matches;
    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_update::{
        AttemptsMonitorRequest, ManagerMarker, ManagerProxy, MonitorRequest, MonitorRequestStream,
    };
    use fidl_fuchsia_update_ext::{
        Initiator, InstallationErrorData, InstallationProgress, InstallingData, UpdateInfo,
    };
    use fuchsia_async as fasync;
    use std::sync::Arc;

    async fn spawn_update_service<T, C, A, Cq>(
        channel_updater: T,
        update_checker: C,
        update_applier: A,
        commit_status_provider: Cq,
    ) -> (ManagerProxy, UpdateService)
    where
        T: TargetChannelUpdater,
        C: UpdateChecker,
        A: UpdateApplier,
        Cq: CommitQuerier,
    {
        let mut update_service = UpdateService {
            update_manager:
                UpdateManager::<T, C, A, RealStateNotifier, Cq, RealAttemptNotifier>::from_checker_and_applier(
                    Arc::new(channel_updater),
                    update_checker,
                    update_applier,
                    commit_status_provider,
                )
                .await
                .spawn(),
        };
        let update_service_clone = update_service.clone();
        let (proxy, stream) =
            create_proxy_and_stream::<ManagerMarker>().expect("create_proxy_and_stream");
        fasync::Task::spawn(async move {
            update_service.handle_request_stream(stream).map(|_| ()).await
        })
        .detach();
        (proxy, update_service_clone)
    }

    async fn collect_all_on_state_events(monitor: MonitorRequestStream) -> Vec<State> {
        monitor
            .map(|r| {
                let MonitorRequest::OnState { state, responder } = r.unwrap();
                responder.send().unwrap();
                state.into()
            })
            .collect()
            .await
    }

    async fn next_n_on_state_events(
        mut request_stream: MonitorRequestStream,
        n: usize,
    ) -> (MonitorRequestStream, Vec<State>) {
        let mut v = Vec::with_capacity(n);
        for _ in 0..n {
            let MonitorRequest::OnState { state, responder } =
                request_stream.next().await.unwrap().unwrap();
            responder.send().unwrap();
            v.push(state.into());
        }
        (request_stream, v)
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_check_now_monitor_sees_on_state_events() {
        let proxy = spawn_update_service(
            FakeTargetChannelUpdater::new(),
            FakeUpdateChecker::new_update_available(),
            FakeUpdateApplier::new_error(),
            FakeCommitQuerier::new(),
        )
        .await
        .0;
        let (client_end, request_stream) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let expected_update_info = Some(UpdateInfo {
            version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
            download_size: None,
        });
        let options = CheckOptions::builder().initiator(Initiator::User).build();

        assert_matches!(proxy.check_now(&options.into(), Some(client_end)).await.unwrap(), Ok(()));

        assert_eq!(
            collect_all_on_state_events(request_stream).await,
            vec![
                State::CheckingForUpdates,
                State::InstallingUpdate(InstallingData {
                    update: expected_update_info.clone(),
                    installation_progress: None,
                }),
                State::InstallationError(InstallationErrorData {
                    update: expected_update_info,
                    installation_progress: None,
                }),
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_multiple_clients_see_on_state_events() {
        let (blocking_update_checker, unblocker) = BlockingUpdateChecker::new_checker_and_sender();
        let (proxy0, mut service) = spawn_update_service(
            FakeTargetChannelUpdater::new(),
            blocking_update_checker,
            FakeUpdateApplier::new_error(),
            FakeCommitQuerier::new(),
        )
        .await;
        let expected_update_info = Some(UpdateInfo {
            version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
            download_size: None,
        });

        let (proxy1, stream1) =
            create_proxy_and_stream::<ManagerMarker>().expect("create_proxy_and_stream");
        fasync::Task::spawn(
            async move { service.handle_request_stream(stream1).map(|_| ()).await },
        )
        .detach();

        let (client_end0, request_stream0) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let (client_end1, request_stream1) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let options = CheckOptions::builder()
            .initiator(Initiator::User)
            .allow_attaching_to_existing_update_check(true)
            .build();

        // Add both monitor clients. We use a blocker to ensure we only start the update check when
        // both Monitor clients are enqueued. This prevents the second client from getting an
        // additional state event (since the event queue sends the last event when you add a client)
        assert_matches!(
            proxy0.check_now(&options.clone().into(), Some(client_end0)).await.unwrap(),
            Ok(())
        );
        assert_matches!(
            proxy1.check_now(&options.into(), Some(client_end1)).await.unwrap(),
            Ok(())
        );
        assert_matches!(unblocker.send(()), Ok(()));

        let events = next_n_on_state_events(request_stream0, 3).await.1;
        assert_eq!(
            events,
            vec![
                State::CheckingForUpdates,
                State::InstallingUpdate(InstallingData {
                    update: expected_update_info.clone(),
                    installation_progress: None,
                }),
                State::InstallationError(InstallationErrorData {
                    update: expected_update_info.clone(),
                    installation_progress: None,
                }),
            ]
        );

        assert_eq!(
            collect_all_on_state_events(request_stream1).await,
            vec![
                State::CheckingForUpdates,
                State::InstallingUpdate(InstallingData {
                    update: expected_update_info.clone(),
                    installation_progress: None,
                }),
                State::InstallationError(InstallationErrorData {
                    update: expected_update_info.clone(),
                    installation_progress: None,
                }),
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_global_attempt_monitor_gets_events() {
        let proxy = spawn_update_service(
            FakeTargetChannelUpdater::new(),
            FakeUpdateChecker::new_update_available(),
            FakeUpdateApplier::new_success(),
            FakeCommitQuerier::new(),
        )
        .await
        .0;

        let (client_end, mut request_stream) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let check_options = CheckOptions::builder().initiator(Initiator::User).build();

        assert_matches!(proxy.monitor_all_update_checks(client_end), Ok(()));
        assert_matches!(proxy.check_now(&check_options.into(), None).await.unwrap(), Ok(()));

        let AttemptsMonitorRequest::OnStart { options, monitor, responder } =
            request_stream.next().await.unwrap().unwrap();

        assert_matches!(responder.send(), Ok(()));
        assert_matches!(options.initiator, Some(fidl_fuchsia_update::Initiator::User));

        let (_, events) = next_n_on_state_events(monitor.into_stream().unwrap(), 3).await;

        assert_eq!(
            events,
            vec![
                State::CheckingForUpdates,
                State::InstallingUpdate(InstallingData {
                    update: Some(UpdateInfo {
                        version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
                        download_size: None,
                    }),
                    installation_progress: None,
                }),
                State::InstallingUpdate(InstallingData {
                    update: Some(UpdateInfo {
                        version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
                        download_size: Some(1000),
                    }),
                    installation_progress: Some(InstallationProgress {
                        fraction_completed: Some(0.42)
                    })
                }),
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_global_attempt_monitor_gets_state_events() {
        let proxy = spawn_update_service(
            FakeTargetChannelUpdater::new(),
            FakeUpdateChecker::new_update_available(),
            FakeUpdateApplier::new_success(),
            FakeCommitQuerier::new(),
        )
        .await
        .0;

        let (attempt_client_end, mut attempt_request_stream) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let (client_end, request_stream) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let options = CheckOptions::builder().initiator(Initiator::User).build();

        assert_matches!(proxy.monitor_all_update_checks(attempt_client_end), Ok(()));
        assert_matches!(proxy.check_now(&options.into(), Some(client_end)).await.unwrap(), Ok(()));

        let AttemptsMonitorRequest::OnStart { options: _, monitor, responder } =
            attempt_request_stream.next().await.unwrap().unwrap();

        assert_matches!(responder.send(), Ok(()));

        let (_, attempt_monitor_events) =
            next_n_on_state_events(monitor.into_stream().unwrap(), 2).await;
        let (_, events) = next_n_on_state_events(request_stream, 2).await;

        assert_eq!(events, attempt_monitor_events);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_check_now_monitor_already_in_progress() {
        let (blocking_update_checker, unblocker) = BlockingUpdateChecker::new_checker_and_sender();
        let proxy = spawn_update_service(
            FakeTargetChannelUpdater::new(),
            blocking_update_checker,
            FakeUpdateApplier::new_error(),
            FakeCommitQuerier::new(),
        )
        .await
        .0;
        let expected_update_info = Some(UpdateInfo {
            version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
            download_size: None,
        });
        let (client_end0, request_stream0) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let (client_end1, request_stream1) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let options = CheckOptions::builder()
            .initiator(Initiator::User)
            .allow_attaching_to_existing_update_check(false)
            .build();

        //Start a hang on InstallingUpdate
        assert_matches!(
            proxy.check_now(&options.clone().into(), Some(client_end0)).await.unwrap(),
            Ok(())
        );

        // When we do the next check, we should get an already in progress error since we're not
        // allowed to attach another client
        assert_eq!(
            proxy.check_now(&options.into(), Some(client_end1)).await.unwrap(),
            Err(CheckNotStartedReason::AlreadyInProgress)
        );

        // When we resume, only the first client should see the on state events
        assert_matches!(unblocker.send(()), Ok(()));
        assert_eq!(
            collect_all_on_state_events(request_stream0).await,
            vec![
                State::CheckingForUpdates,
                State::InstallingUpdate(InstallingData {
                    update: expected_update_info.clone(),
                    installation_progress: None,
                }),
                State::InstallationError(InstallationErrorData {
                    update: expected_update_info,
                    installation_progress: None,
                }),
            ]
        );
        assert_eq!(collect_all_on_state_events(request_stream1).await, vec![]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_attempt_monitor_already_in_progress() {
        let (blocking_update_checker, unblocker) = BlockingUpdateChecker::new_checker_and_sender();
        let proxy = spawn_update_service(
            FakeTargetChannelUpdater::new(),
            blocking_update_checker,
            FakeUpdateApplier::new_error(),
            FakeCommitQuerier::new(),
        )
        .await
        .0;
        let expected_update_info = Some(UpdateInfo {
            version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
            download_size: None,
        });
        let (attempt_client_end, mut attempt_request_stream) =
            fidl::endpoints::create_request_stream::<AttemptsMonitorMarker>()
                .expect("create_request_stream");
        let (client_end0, _request_stream0) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let (client_end1, _request_stream1) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let options = CheckOptions::builder()
            .initiator(Initiator::User)
            .allow_attaching_to_existing_update_check(false)
            .build();

        assert_matches!(proxy.monitor_all_update_checks(attempt_client_end), Ok(()));
        //Start a hang on InstallingUpdate
        assert_matches!(
            proxy.check_now(&options.clone().into(), Some(client_end0)).await.unwrap(),
            Ok(())
        );

        // When we do the next check, we should get an already in progress error since we're not
        // allowed to attach another client
        assert_eq!(
            proxy.check_now(&options.into(), Some(client_end1)).await.unwrap(),
            Err(CheckNotStartedReason::AlreadyInProgress)
        );

        let AttemptsMonitorRequest::OnStart { options: _, monitor, responder } =
            attempt_request_stream.next().await.unwrap().unwrap();
        assert_matches!(responder.send(), Ok(()));

        // When we resume, attempt_monitor should see the on state events
        assert_matches!(unblocker.send(()), Ok(()));
        assert_eq!(
            collect_all_on_state_events(monitor.into_stream().unwrap()).await,
            vec![
                State::CheckingForUpdates,
                State::InstallingUpdate(InstallingData {
                    update: expected_update_info.clone(),
                    installation_progress: None,
                }),
                State::InstallationError(InstallationErrorData {
                    update: expected_update_info,
                    installation_progress: None,
                }),
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_attempt_monitor_persists_dropped_update() {
        let (blocking_update_checker, unblocker) = BlockingUpdateChecker::new_checker_and_sender();
        let (proxy0, mut service) = spawn_update_service(
            FakeTargetChannelUpdater::new(),
            blocking_update_checker,
            FakeUpdateApplier::new_success(),
            FakeCommitQuerier::new(),
        )
        .await;

        let (monitor_client, monitor_request_stream) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let (attempt_client_end, mut attempt_request_stream) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let options = CheckOptions::builder()
            .initiator(Initiator::User)
            .allow_attaching_to_existing_update_check(true)
            .build();

        assert_matches!(proxy0.monitor_all_update_checks(attempt_client_end), Ok(()));
        assert_matches!(
            proxy0.check_now(&options.clone().into(), Some(monitor_client)).await.unwrap(),
            Ok(())
        );

        let (_, events) = next_n_on_state_events(monitor_request_stream, 1).await;
        assert_eq!(events, vec![State::CheckingForUpdates]);

        drop(proxy0);

        let (proxy1, stream1) =
            create_proxy_and_stream::<ManagerMarker>().expect("create_proxy_and_stream");
        fasync::Task::spawn(
            async move { service.handle_request_stream(stream1).map(|_| ()).await },
        )
        .detach();

        assert_matches!(proxy1.check_now(&options.into(), None).await.unwrap(), Ok(()));
        let AttemptsMonitorRequest::OnStart { options, monitor, responder } =
            attempt_request_stream.next().await.unwrap().unwrap();

        assert_matches!(responder.send(), Ok(()));
        assert_matches!(options.initiator, Some(fidl_fuchsia_update::Initiator::User));

        assert_matches!(unblocker.send(()), Ok(()));
        let (_, events) = next_n_on_state_events(monitor.into_stream().unwrap(), 3).await;
        assert_eq!(
            events,
            vec![
                State::CheckingForUpdates,
                State::InstallingUpdate(InstallingData {
                    update: Some(UpdateInfo {
                        version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
                        download_size: None,
                    }),
                    installation_progress: None,
                }),
                State::InstallingUpdate(InstallingData {
                    update: Some(UpdateInfo {
                        version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
                        download_size: Some(1000),
                    }),
                    installation_progress: Some(InstallationProgress {
                        fraction_completed: Some(0.42)
                    })
                }),
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_attempt_monitor_two_updates() {
        let proxy = spawn_update_service(
            FakeTargetChannelUpdater::new(),
            FakeUpdateChecker::new_update_available(),
            FakeUpdateApplier::new_error(),
            FakeCommitQuerier::new(),
        )
        .await
        .0;

        let (attempt_client_end, mut attempt_request_stream) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let check_options = CheckOptions::builder()
            .initiator(Initiator::User)
            .allow_attaching_to_existing_update_check(true)
            .build();

        assert_matches!(proxy.monitor_all_update_checks(attempt_client_end), Ok(()));
        assert_matches!(
            proxy.check_now(&check_options.clone().into(), None).await.unwrap(),
            Ok(())
        );

        let AttemptsMonitorRequest::OnStart { options, monitor, responder } =
            attempt_request_stream.next().await.unwrap().unwrap();

        assert_matches!(responder.send(), Ok(()));
        assert_matches!(options.initiator, Some(fidl_fuchsia_update::Initiator::User));

        let (_, events) = next_n_on_state_events(monitor.into_stream().unwrap(), 3).await;
        assert_eq!(
            events,
            vec![
                State::CheckingForUpdates,
                State::InstallingUpdate(InstallingData {
                    update: Some(UpdateInfo {
                        version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
                        download_size: None,
                    }),
                    installation_progress: None,
                }),
                State::InstallationError(InstallationErrorData {
                    update: Some(UpdateInfo {
                        version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
                        download_size: None,
                    }),
                    installation_progress: None,
                }),
            ]
        );

        // Do another update check!
        assert_matches!(proxy.check_now(&check_options.into(), None).await.unwrap(), Ok(()));
        let AttemptsMonitorRequest::OnStart { options, monitor, responder } =
            attempt_request_stream.next().await.unwrap().unwrap();

        assert_matches!(responder.send(), Ok(()));
        assert_matches!(options.initiator, Some(fidl_fuchsia_update::Initiator::User));

        let (_, events) = next_n_on_state_events(monitor.into_stream().unwrap(), 3).await;
        assert_eq!(
            events,
            vec![
                State::CheckingForUpdates,
                State::InstallingUpdate(InstallingData {
                    update: Some(UpdateInfo {
                        version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
                        download_size: None,
                    }),
                    installation_progress: None,
                }),
                State::InstallationError(InstallationErrorData {
                    update: Some(UpdateInfo {
                        version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
                        download_size: None,
                    }),
                    installation_progress: None,
                }),
            ]
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_check_now_invalid_options() {
        let proxy = spawn_update_service(
            FakeTargetChannelUpdater::new(),
            FakeUpdateChecker::new_update_available(),
            FakeUpdateApplier::new_error(),
            FakeCommitQuerier::new(),
        )
        .await
        .0;
        let (client_end, request_stream) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");

        // Invalid because initiator is a required field.
        let invalid_options = fidl_fuchsia_update::CheckOptions {
            initiator: None,
            allow_attaching_to_existing_update_check: None,
            ..Default::default()
        };

        let res = proxy.check_now(&invalid_options, Some(client_end)).await.unwrap();

        assert_eq!(res, Err(CheckNotStartedReason::InvalidOptions));
        assert_eq!(collect_all_on_state_events(request_stream).await, vec![]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_check_now_monitor_already_in_progress_but_allow_attaching_to_existing_update_check(
    ) {
        let (blocking_update_checker, unblocker) = BlockingUpdateChecker::new_checker_and_sender();
        let proxy = spawn_update_service(
            FakeTargetChannelUpdater::new(),
            blocking_update_checker,
            FakeUpdateApplier::new_error(),
            FakeCommitQuerier::new(),
        )
        .await
        .0;
        let (client_end0, request_stream0) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let (client_end1, request_stream1) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let options = CheckOptions::builder()
            .initiator(Initiator::User)
            .allow_attaching_to_existing_update_check(true)
            .build();
        let expected_update_info = Some(UpdateInfo {
            version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
            download_size: None,
        });
        let expected_states = vec![
            State::CheckingForUpdates,
            State::InstallingUpdate(InstallingData {
                update: expected_update_info.clone(),
                installation_progress: None,
            }),
            State::InstallationError(InstallationErrorData {
                update: expected_update_info,
                installation_progress: None,
            }),
        ];

        // Start a hang on InstallingUpdate
        assert_matches!(
            proxy.check_now(&options.clone().into(), Some(client_end0)).await.unwrap(),
            Ok(())
        );

        // When we do the next check, we should get an OK since we're allowed to attach to
        // an existing check
        assert_matches!(proxy.check_now(&options.into(), Some(client_end1)).await.unwrap(), Ok(()));

        // When we resume, both clients should see the on state events
        assert_matches!(unblocker.send(()), Ok(()));
        assert_eq!(collect_all_on_state_events(request_stream0).await, expected_states.clone());
        assert_eq!(collect_all_on_state_events(request_stream1).await, expected_states);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_update_attempt_persists_across_client_disconnect_reconnect() {
        let (blocking_update_checker, unblocker) = BlockingUpdateChecker::new_checker_and_sender();
        let fake_update_applier = FakeUpdateApplier::new_error();
        let (proxy0, mut service) = spawn_update_service(
            FakeTargetChannelUpdater::new(),
            blocking_update_checker,
            fake_update_applier.clone(),
            FakeCommitQuerier::new(),
        )
        .await;
        let expected_update_info = Some(UpdateInfo {
            version_available: Some(LATEST_SYSTEM_IMAGE.to_string()),
            download_size: None,
        });

        let (client_end0, request_stream0) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let (client_end1, request_stream1) =
            fidl::endpoints::create_request_stream().expect("create_request_stream");
        let options = CheckOptions::builder()
            .initiator(Initiator::User)
            .allow_attaching_to_existing_update_check(true)
            .build();

        assert_matches!(
            proxy0.check_now(&options.clone().into(), Some(client_end0)).await.unwrap(),
            Ok(())
        );

        let (_, events) = next_n_on_state_events(request_stream0, 1).await;
        assert_eq!(events, vec![State::CheckingForUpdates]);

        drop(proxy0);

        let (proxy1, stream1) =
            create_proxy_and_stream::<ManagerMarker>().expect("create_proxy_and_stream");
        fasync::Task::spawn(
            async move { service.handle_request_stream(stream1).map(|_| ()).await },
        )
        .detach();

        // The first update check is still in progress and blocked, but we'll get an OK
        // since we allow_attaching_to_existing_update_check=true
        assert_matches!(
            proxy1.check_now(&options.into(), Some(client_end1)).await.unwrap(),
            Ok(())
        );

        // Once we unblock, the update should resume
        assert_matches!(unblocker.send(()), Ok(()));
        assert_eq!(
            collect_all_on_state_events(request_stream1).await,
            vec![
                // the second request stream gets this since the event queue sent the last event :)
                State::CheckingForUpdates,
                State::InstallingUpdate(InstallingData {
                    update: expected_update_info.clone(),
                    installation_progress: None,
                }),
                State::InstallationError(InstallationErrorData {
                    update: expected_update_info,
                    installation_progress: None,
                }),
            ]
        );
        assert_eq!(fake_update_applier.call_count(), 1);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_perform_pending_reboot_returns_false() {
        let proxy = spawn_update_service(
            FakeTargetChannelUpdater::new(),
            FakeUpdateChecker::new_update_available(),
            FakeUpdateApplier::new_error(),
            FakeCommitQuerier::new(),
        )
        .await
        .0;

        let res = proxy.perform_pending_reboot().await.unwrap();

        assert_eq!(res, false);
    }
}
