// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        install_manager::InstallManagerControlHandle,
        update::{Config, ControlRequest, RebootController, UpdateAttempt, UpdateHistory},
    },
    anyhow::{anyhow, Context, Error},
    event_queue::{ClosedClient, Notify},
    fidl_fuchsia_update_installer::{
        InstallerRequest, InstallerRequestStream, MonitorProxy, MonitorProxyInterface,
        RebootControllerRequest, UpdateResult,
    },
    fidl_fuchsia_update_installer_ext::State,
    fuchsia_component::server::{ServiceFs, ServiceObjLocal},
    futures::prelude::*,
    parking_lot::Mutex,
    std::{convert::TryInto, sync::Arc},
    tracing::{error, info},
};

pub enum IncomingService {
    Installer(InstallerRequestStream),
}

/// This type can be used to send update state events to monitor server ends.
#[derive(Clone)]
pub struct UpdateStateNotifier {
    proxy: MonitorProxy,
}

impl UpdateStateNotifier {
    pub fn new(proxy: MonitorProxy) -> Self {
        Self { proxy }
    }
}

impl Notify for UpdateStateNotifier {
    type Event = State;
    type NotifyFuture = futures::future::Map<
        <MonitorProxy as MonitorProxyInterface>::OnStateResponseFut,
        fn(Result<(), fidl::Error>) -> Result<(), ClosedClient>,
    >;

    fn notify(&self, state: State) -> Self::NotifyFuture {
        self.proxy.on_state(&state.into()).map(|result| result.map_err(|_| ClosedClient))
    }
}

pub struct FidlServer {
    history: Arc<Mutex<UpdateHistory>>,
    install_manager_ch: InstallManagerControlHandle<UpdateStateNotifier>,
}

impl FidlServer {
    pub fn new(
        history: Arc<Mutex<UpdateHistory>>,
        install_manager_ch: InstallManagerControlHandle<UpdateStateNotifier>,
    ) -> Self {
        Self { history, install_manager_ch }
    }

    /// Runs the FIDL Server.
    pub async fn run(self, mut fs: ServiceFs<ServiceObjLocal<'_, IncomingService>>) {
        fs.dir("svc").add_fidl_service(IncomingService::Installer);

        // Handles each client connection concurrently.
        fs.for_each_concurrent(None, |incoming_service| {
            self.handle_client(incoming_service).unwrap_or_else(|e| {
                error!("error encountered while handling client: {:#}", anyhow!(e))
            })
        })
        .await
    }

    /// Handles an incoming FIDL connection from a client.
    async fn handle_client(&self, incoming_service: IncomingService) -> Result<(), Error> {
        match incoming_service {
            IncomingService::Installer(mut stream) => {
                while let Some(request) =
                    stream.try_next().await.context("error receiving Installer request")?
                {
                    self.handle_installer_request(request).await?;
                }
            }
        }
        Ok(())
    }

    /// Handles fuchsia.update.update.Installer requests.
    async fn handle_installer_request(&self, request: InstallerRequest) -> Result<(), Error> {
        match request {
            InstallerRequest::GetLastUpdateResult { responder } => {
                let history = self.history.lock();
                let last_result = into_update_result(history.last_update_attempt());
                responder.send(&last_result)?;
            }
            InstallerRequest::GetUpdateResult { attempt_id, responder } => {
                let history = self.history.lock();
                let result = into_update_result(history.update_attempt(attempt_id));
                responder.send(&result)?;
            }
            InstallerRequest::StartUpdate {
                url,
                options,
                monitor,
                reboot_controller,
                responder,
            } => {
                let mut install_manager_ch = self.install_manager_ch.clone();

                // Transform FIDL request params into types the install manager can understand.
                let config = Config::from_url_and_options(url.url.parse()?, options.try_into()?);
                let notifier = UpdateStateNotifier::new(
                    monitor
                        .into_proxy()
                        .context("while converting monitor ClientEnd into proxy")?,
                );

                // If a reboot controller is specified, set up a task that fowards reboot controller
                // requests to the update attempt task.
                let reboot_controller = if let Some(server_end) = reboot_controller {
                    let mut stream = server_end.into_stream()?;
                    Some(RebootController::spawn(async move {
                        match stream.next().await {
                            None => {
                                info!("RebootController channel closed, unblocking reboot");
                                ControlRequest::Unblock
                            }
                            Some(Err(e)) => {
                                error!(
                                    "error serving RebootController, unblocking reboot: {:#}",
                                    anyhow!(e)
                                );
                                ControlRequest::Unblock
                            }
                            Some(Ok(RebootControllerRequest::Unblock { .. })) => {
                                ControlRequest::Unblock
                            }
                            Some(Ok(RebootControllerRequest::Detach { .. })) => {
                                ControlRequest::Detach
                            }
                        }
                    }))
                } else {
                    // Not providing a reboot controller is equivalent to immediately unblocking
                    // the reboot.
                    None
                };

                // Forward to the install manager to deal with this.
                let mut response =
                    install_manager_ch.start_update(config, notifier, reboot_controller).await?;
                responder.send(&mut response)?;
            }
            InstallerRequest::MonitorUpdate { attempt_id, monitor, responder } => {
                let mut install_manager_ch = self.install_manager_ch.clone();
                let notifier = UpdateStateNotifier::new(
                    monitor
                        .into_proxy()
                        .context("while converting monitor ClientEnd into proxy")?,
                );

                // Forward to the install manager to deal with this.
                let response = install_manager_ch.monitor_update(attempt_id, notifier).await?;
                responder.send(response)?;
            }
        }
        Ok(())
    }
}

fn into_update_result(attempt: Option<&UpdateAttempt>) -> UpdateResult {
    match attempt {
        None => UpdateResult {
            attempt_id: None,
            url: None,
            options: None,
            state: None,
            ..Default::default()
        },
        Some(attempt) => attempt.into(),
    }
}
