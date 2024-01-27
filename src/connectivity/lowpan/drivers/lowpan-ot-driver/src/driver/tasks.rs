// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;

use anyhow::{Context as _, Error};
use fidl_fuchsia_location_namedplace::RegulatoryRegionWatcherMarker;
use fuchsia_component::client::connect_to_protocol;
use futures::never::Never;
use futures::prelude::*;
use lowpan_driver_common::lowpan_fidl::ConnectivityState;
use lowpan_driver_common::net::BackboneInterface;
use lowpan_driver_common::spinel::Canceled;
use lowpan_driver_common::FutureExt;
use openthread::ot::InfraInterface;
use std::ffi::CString;

impl<OT, NI, BI> OtDriver<OT, NI, BI>
where
    OT: Send + ot::InstanceInterface + AsRef<ot::Instance>,
    NI: NetworkInterface,
    BI: BackboneInterface,
{
    /// Main Loop Stream.
    ///
    /// This stream ultimately handles all of the event-handling for the driver,
    /// processing events from both OpenThread, the network interface, and other
    /// relevant sources such as regulatory domain changes.
    pub fn main_loop_stream(&self) -> impl Stream<Item = Result<(), anyhow::Error>> + Send + '_
    where
        OT: AsRef<ot::Instance>,
    {
        // init future
        let init_future = async move {
            self.init_ot();
            Ok(())
        };

        // Stream for handling OpenThread tasklets.
        let tasklets_stream = self
            .driver_state
            .tasklets_stream()
            .inspect(|_| fx_log_trace!("Tasklets did run"))
            .map(Result::Ok)
            .chain(futures::future::ready(Err(anyhow::Error::from(ResetRequested))).into_stream());

        // Stream for handling OpenThread state changes.
        let state_change_stream = self
            .driver_state
            .lock()
            .ot_instance
            .state_changed_stream()
            .then(move |flags| self.on_ot_state_change(flags));

        // Stream for handling regulatory region changes.
        let regulatory_region_stream = futures::stream::unfold(
            connect_to_protocol::<RegulatoryRegionWatcherMarker>()
                .context("RegulatoryRegionWatcherMarker"),
            move |watcher| match watcher {
                Ok(watcher) => watcher
                    .get_update()
                    .map(|x| match x {
                        Ok(region) => Some((Result::<_, Error>::Ok(region), Ok(watcher))),
                        Err(err) => {
                            fx_log_warn!(
                                "Unable to get RegulatoryRegionWatcher instance: {:?}",
                                err
                            );
                            None
                        }
                    })
                    .boxed(),
                Err(err) => {
                    fx_log_warn!("Unable to get RegulatoryRegionWatcher instance: {:?}", err);
                    futures::future::ready(None).boxed()
                }
            },
        )
        .and_then(move |region: String| self.on_regulatory_region_changed(region))
        .map(|x| {
            // We just log errors and continue for now.
            if let Err(e) = x {
                fx_log_warn!("regulatory_region_stream: Error: {:?}", e);
            }
            Result::<_, Error>::Ok(())
        });

        // Stream for handling network interface events.
        let net_if_event_stream = self.net_if.take_event_stream().and_then(move |event| {
            let event_copy = event.clone();
            self.on_network_interface_event(event).or_else(move |err| async move {
                if self.driver_state.lock().is_active_and_ready() {
                    // If this happens while we are active and ready then we
                    // are out of sync and will need a reset.
                    error!("Error while processing {:?}: {:?}", event_copy, &err);
                    Err(err)
                } else {
                    // If this happens while we aren't active and ready then
                    // that is somewhat expected and we can continue after logging.
                    warn!("Error while processing {:?}: {:?}", event_copy, &err);
                    Ok(())
                }
            })
        });

        let backbone_if_event_stream =
            self.backbone_if.event_stream().map(move |event| match event {
                Ok(is_running) => {
                    self.driver_state
                        .lock()
                        .ot_instance
                        .as_ref()
                        .platform_infra_if_on_state_changed(
                            self.backbone_if.get_nicid().try_into().unwrap(),
                            is_running,
                        );
                    Result::<_, Error>::Ok(())
                }
                Err(x) => Err(x),
            });

        // Stream for handling our state machine.
        let state_machine_stream = futures::stream::try_unfold((), move |_| {
            self.state_machine_single()
                .map_ok(|x| Some((x, ())))
                .map_err(|x| x.context("single_main_loop"))
        });

        let discovery_proxy_stream =
            self.driver_state.discovery_proxy_future().into_stream().map(|_: Never| unreachable!());

        // Openthread CLI inbound task
        let (cli_input_sender_local, mut cli_input_receiver) = futures::channel::mpsc::unbounded();
        let openthread_cli_inbound_loop = async move {
            loop {
                while let Some(Some(next)) = cli_input_receiver.next().await {
                    self.driver_state
                        .lock()
                        .ot_instance
                        .cli_input_line(&CString::new(next).unwrap());
                }
            }
        };
        self.driver_state.lock().ot_ctl.cli_input_sender.replace(cli_input_sender_local);

        // SCAN WATCHDOG. Scans are somewhat blocking operations---the device cannot
        // actively participate on the network while one is in progress. Occasionally
        // we can run into bugs like <fxbug.dev/106509>, where the scan never finishes.
        // Because there is no way to cancel an ongoing scan in OpenThread, the only
        // way to get ourselves out of this state is to reset OpenThread. And that's
        // what the `scan_watchdog`, defined below, is supposed to do. It monitors
        // for when the scan starts and then makes sure that it lasts no longer than
        // `SCAN_WATCHDOG_TIMEOUT`. If it does, it will terminate the loop, which
        // will cause OpenThread to be reset.
        let scan_watchdog = async move {
            /// Scan Watchdog Timeout. This timeout should be longer than the longest
            /// reasonable scan period. Scans typically last from 5 to 15 seconds, so
            /// a 60-second scan timeout seems like a reasonable upper-bound.
            const SCAN_WATCHDOG_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(60);

            loop {
                debug!("SCAN_WATCHDOG: Waiting for a scan to start.");
                self.wait_for_state(|x| {
                    x.ot_instance.is_energy_scan_in_progress()
                        || x.ot_instance.is_active_scan_in_progress()
                })
                .await;

                debug!("SCAN_WATCHDOG: Scan started! Waiting for it to complete.");
                self.wait_for_state(|x| {
                    !x.ot_instance.is_energy_scan_in_progress()
                        && !x.ot_instance.is_active_scan_in_progress()
                })
                    .map(|()| Ok(()))
                    .on_timeout(SCAN_WATCHDOG_TIMEOUT, || {
                        let driver_state = self.driver_state.lock();
                        if driver_state.ot_instance.is_energy_scan_in_progress()
                            || driver_state.ot_instance.is_active_scan_in_progress()
                        {
                            error!(
                                "SCAN_WATCHDOG: OpenThread was scanning for longer than {:?}, will restart.",
                                SCAN_WATCHDOG_TIMEOUT
                            );
                            Err(format_err!(
                                "OpenThread was scanning for longer than {:?}",
                                SCAN_WATCHDOG_TIMEOUT
                            ))
                        } else {
                            Ok(())
                        }
                    })
                    .await?;

                debug!("SCAN_WATCHDOG: Scan completed! Watchdog disarmed.");
            }
        };

        init_future.into_stream().chain(futures::stream::select_all([
            tasklets_stream.boxed(),
            regulatory_region_stream.boxed(),
            state_change_stream.boxed(),
            net_if_event_stream.boxed(),
            backbone_if_event_stream.boxed(),
            state_machine_stream.boxed(),
            discovery_proxy_stream.boxed(),
            scan_watchdog.into_stream().boxed(),
            openthread_cli_inbound_loop.into_stream().boxed(),
        ]))
    }

    /// Initializes OpenThread instance. LOCKS DRIVER STATE.
    fn init_ot(&self) {
        let mut driver_state = self.driver_state.lock();

        driver_state.ot_instance.ip6_set_address_fn(Some(
            move |info: ot::Ip6AddressInfo<'_>, is_added| {
                // NOTE: DRIVER STATE IS LOCKED WHEN THIS IS CALLED!
                self.on_ot_ip6_address_info(info, is_added);
            },
        ));

        driver_state.ot_instance.ip6_set_receive_fn(Some(move |msg: OtMessageBox<'_>| {
            // NOTE: DRIVER STATE IS LOCKED WHEN THIS IS CALLED!
            self.on_ot_ip6_receive(msg);
        }));

        if let Err(err) = driver_state.set_discovery_proxy_enabled(true) {
            warn!("Unable to start SRP discovery proxy: {:?}", err);
        }

        if let Err(err) = driver_state.set_advertising_proxy_enabled(true) {
            warn!("Unable to start SRP advertising proxy: {:?}", err);
        }

        // Make sure we are a router.
        driver_state
            .ot_instance
            .set_link_mode(
                ot::LinkModeConfig::IS_FTD
                    | ot::LinkModeConfig::NETWORK_DATA
                    | ot::LinkModeConfig::RX_ON_WHEN_IDLE,
            )
            .unwrap();

        // Make sure SLAAC addresses are turned on.
        driver_state.ot_instance.ip6_set_slaac_enabled(true);

        // Enable the receive filter.
        driver_state.ot_instance.ip6_set_receive_filter_enabled(true);

        // Turn off ICMPv6 ping auto-reply.
        driver_state.ot_instance.icmp6_set_echo_mode(ot::Icmp6EchoMode::HandleDisabled);

        // Enable SRP Server
        driver_state.ot_instance.srp_server_set_enabled(true);
    }

    /// A single iteration of the main task loop
    async fn state_machine_single(&self) -> Result<(), Error> {
        fx_log_info!("main_task");
        if self.get_connectivity_state().is_active_and_ready() {
            fx_log_info!("main_task: Initialized, active, and ready");

            // Exit criteria is when we are no longer active nor ready.
            // When this future terminates, we are no longer online.
            let exit_criteria = self.wait_for_state(|x| !x.is_active_and_ready());

            self.online_task()
                .boxed()
                .map(|x| match x {
                    // We don't care if the error was cancelled.
                    Err(err) if err.is::<Canceled>() => Ok(()),
                    other => other,
                })
                .cancel_upon(exit_criteria.boxed(), Ok(()))
                .map_err(|x| x.context("online_task"))
                .await?;

            fx_log_info!("main_task: online_task terminated");

            self.online_task_cleanup()
                .boxed()
                .map_err(|x| x.context("online_task_cleanup"))
                .await?;
        } else if self.get_connectivity_state().is_commissioning() {
            self.wait_for_state(|x| !x.is_commissioning()).await;
        } else {
            fx_log_info!("main_task: Initialized, but either not active or not ready.");

            // Exit criteria is when we are no longer active nor ready.
            // When this future terminates, we are no longer offline.
            let exit_criteria = self.wait_for_state(|x| x.connectivity_state.is_active_and_ready());

            self.offline_task()
                .boxed()
                .map(|x| match x {
                    // We don't care if the error was cancelled.
                    Err(err) if err.is::<Canceled>() => Ok(()),
                    other => other,
                })
                .cancel_upon(exit_criteria.boxed(), Ok(()))
                .map_err(|x| x.context("offline_task"))
                .await?;

            fx_log_info!("main_task: offline_task terminated");
        }
        Ok(())
    }

    /// Online loop task that is executed while we are both "ready" and "active".
    ///
    /// This task will bring the device into a state where it
    /// is an active participant in the network.
    ///
    /// The resulting future may be terminated at any time.
    async fn online_task(&self) -> Result<(), Error> {
        fx_log_info!("online_loop: Entered");

        {
            let driver_state = self.driver_state.lock();

            // Bring up the network interface.
            driver_state.ot_instance.ip6_set_enabled(true).context("ip6_set_enabled")?;

            // Bring up the mesh stack.
            driver_state.ot_instance.thread_set_enabled(true).context("thread_set_enabled")?;
        }

        fx_log_info!("online_loop: Waiting for us to become online. . .");

        self.wait_for_state(|x| x.connectivity_state != ConnectivityState::Attaching)
            .on_timeout(std::time::Duration::from_secs(10), || ())
            .await;

        {
            let mut driver_state = self.driver_state.lock();

            if driver_state.updated_connectivity_state() == ConnectivityState::Attaching {
                // We are still attaching. Assume we are isolated.
                driver_state.connectivity_state = ConnectivityState::Isolated;

                std::mem::drop(driver_state);

                self.on_connectivity_state_change(
                    ConnectivityState::Isolated,
                    ConnectivityState::Attaching,
                );
            }
        }

        // If we are isolated, wait until we are no longer isolated before
        // bringing the network interface online.
        self.wait_for_state(|x| x.connectivity_state != ConnectivityState::Isolated).await;

        if self.get_connectivity_state().is_online() {
            // Mark the network interface as online.
            self.net_if.set_online(true).await.context("Marking network interface as online")?;

            fx_log_info!("online_loop: We are online, starting outbound packet pump");

            // The pump that pulls outbound data from netstack to the NCP.
            let outbound_packet_pump = self
                .outbound_packet_pump()
                .into_stream()
                .try_collect::<()>()
                .map(|x| x.context("outbound_packet_pump"));

            // This will run indefinitely, unless there is an error.
            outbound_packet_pump.await?;
        }

        Ok(())
    }

    /// Cleanup method that is called after the online task has finished.
    async fn online_task_cleanup(&self) -> Result<(), Error> {
        self.net_if
            .set_online(false)
            .await
            .context("Unable to mark network interface as offline")?;
        Ok(())
    }

    /// Offline loop task that is executed while we are either "not ready" or "inactive".
    ///
    /// This task will bring the device to a state where
    /// it is not an active participant in the network.
    ///
    /// The resulting future may be terminated at any time.
    async fn offline_task(&self) -> Result<(), Error> {
        fx_log_info!("offline_loop: Entered");

        self.net_if
            .set_online(false)
            .await
            .context("Unable to mark network interface as offline")?;

        {
            let driver_state = self.driver_state.lock();

            // Bring down the mesh stack.
            driver_state.ot_instance.thread_set_enabled(false).context("thread_set_enabled")?;

            // Mark the network interface as offline.
            driver_state.ot_instance.ip6_set_enabled(false).context("ip6_set_enabled")?;
        } // Driver state lock goes out of scope here

        fx_log_info!("offline_loop: Waiting");

        #[allow(clippy::unit_arg)]
        Ok(futures::future::pending().await)
    }
}
