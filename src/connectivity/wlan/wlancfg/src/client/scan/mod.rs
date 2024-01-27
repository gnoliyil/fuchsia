// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Manages Scan requests for the Client Policy API.
use {
    crate::{
        client::types,
        config_management::SavedNetworksManagerApi,
        mode_management::iface_manager_api::{IfaceManagerApi, SmeForScan},
        telemetry::{ScanIssue, TelemetryEvent, TelemetrySender},
    },
    anyhow::{format_err, Error},
    async_trait::async_trait,
    fidl_fuchsia_location_sensor as fidl_location_sensor, fidl_fuchsia_wlan_policy as fidl_policy,
    fidl_fuchsia_wlan_sme as fidl_sme,
    fuchsia_async::{self as fasync, DurationExt, TimeoutExt},
    fuchsia_component::client::connect_to_protocol,
    fuchsia_zircon as zx,
    futures::{
        channel::{mpsc, oneshot},
        lock::Mutex,
        prelude::*,
        select,
        stream::FuturesUnordered,
    },
    itertools::Itertools,
    log::{debug, error, info, warn},
    std::{collections::HashMap, convert::TryFrom, sync::Arc},
    wlan_common,
};

mod fidl_conversion;
mod queue;

pub use fidl_conversion::{
    scan_result_to_policy_scan_result, send_scan_error_over_fidl, send_scan_results_over_fidl,
};

// Delay between scanning retries when the firmware returns "ShouldWait" error code
const SCAN_RETRY_DELAY_MS: i64 = 100;
// Max time allowed for consumers of scan results to retrieve results
const SCAN_CONSUMER_MAX_SECONDS_ALLOWED: i64 = 5;
// A long amount of time that a scan should be able to finish within. If a scan takes longer than
// this is indicates something is wrong.
const SCAN_TIMEOUT: zx::Duration = zx::Duration::from_seconds(60);
/// Capacity of "first come, first serve" slots available to scan requesters
pub const SCAN_REQUEST_BUFFER_SIZE: usize = 100;

// Inidication of the scan caller, for use in logging caller specific metrics
#[derive(Debug, PartialEq)]
pub enum ScanReason {
    ClientRequest,
    NetworkSelection,
    BssSelection,
    BssSelectionAugmentation,
}

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash)]
struct SmeNetworkIdentifier {
    ssid: types::Ssid,
    protection: types::SecurityTypeDetailed,
}

#[async_trait]
pub trait ScanRequestApi: Send + Sync {
    async fn perform_scan(
        &self,
        scan_reason: ScanReason,
        ssids: Vec<types::Ssid>,
        channels: Vec<types::WlanChan>,
    ) -> Result<Vec<types::ScanResult>, types::ScanError>;
}

pub struct ScanRequester {
    pub sender: mpsc::Sender<ApiScanRequest>,
}

pub enum ApiScanRequest {
    Scan(
        ScanReason,
        Vec<types::Ssid>,
        Vec<types::WlanChan>,
        oneshot::Sender<Result<Vec<types::ScanResult>, types::ScanError>>,
    ),
}

#[async_trait]
impl ScanRequestApi for ScanRequester {
    async fn perform_scan(
        &self,
        scan_reason: ScanReason,
        ssids: Vec<types::Ssid>,
        channels: Vec<types::WlanChan>,
    ) -> Result<Vec<types::ScanResult>, types::ScanError> {
        let (responder, receiver) = oneshot::channel();
        self.sender
            .clone()
            .try_send(ApiScanRequest::Scan(scan_reason, ssids, channels, responder))
            .map_err(|e| {
                error!("Failed to send ScanRequest: {:?}", e);
                types::ScanError::GeneralError
            })?;
        receiver.await.map_err(|e| {
            error!("Failed to receive ScanRequest response: {:?}", e);
            types::ScanError::GeneralError
        })?
    }
}

/// Create a future representing the scan manager loop.
pub async fn serve_scanning_loop(
    iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>,
    saved_networks_manager: Arc<dyn SavedNetworksManagerApi>,
    telemetry_sender: TelemetrySender,
    mut scan_request_channel: mpsc::Receiver<ApiScanRequest>,
) -> Result<(), Error> {
    let mut operation_futures = FuturesUnordered::new();

    loop {
        select! {
            request = scan_request_channel.next() => {
                match request {
                    Some(ApiScanRequest::Scan(reason, ssids, channels, responder)) => {
                        let scan_fut = if ssids.is_empty() && channels.is_empty() {
                            perform_scan(
                                iface_manager.clone(),
                                saved_networks_manager.clone(),
                                LocationSensorUpdater { wpa3_supported: true },
                                reason,
                                Some(telemetry_sender.clone())
                            ).boxed()
                        } else {
                            perform_directed_active_scan(
                                iface_manager.clone(),
                                saved_networks_manager.clone(),
                                ssids,
                                channels,
                                reason,
                                Some(telemetry_sender.clone()),
                            ).boxed()
                        };
                        let fut = (async move {
                            if let Err(_original_message) = responder.send(scan_fut.await) {
                                // This occurs if the `ScanRequestApi::perform_scan` future is dropped
                                debug!("could not respond to ScanRequest, channel was closed");
                            }
                        }).boxed();
                        operation_futures.push(fut);
                    },
                    None => {
                        error!("Unexpected 'None' on scan_request_channel");
                    }
                }
            },
            () = operation_futures.select_next_some() => {}
        }
    }
}

/// Allows for consumption of updated scan results.
#[async_trait]
pub trait ScanResultUpdate: Sync + Send {
    async fn update_scan_results(&mut self, scan_results: &Vec<types::ScanResult>);
}

/// Requests a new SME scan and returns the results.
async fn sme_scan(
    sme_proxy: &SmeForScan,
    scan_request: fidl_sme::ScanRequest,
    telemetry_sender: Option<TelemetrySender>,
) -> Result<Vec<wlan_common::scan::ScanResult>, types::ScanError> {
    debug!("Sending scan request to SME");
    let scan_result = sme_proxy.scan(&mut scan_request.clone()).await.map_err(|error| {
        error!("Failed to send scan to SME: {:?}", error);
        types::ScanError::GeneralError
    })?;
    debug!("Finished getting scan results from SME");
    scan_result
        .map(|scan_result_list| {
            scan_result_list
                .iter()
                .filter_map(|scan_result| {
                    wlan_common::scan::ScanResult::try_from(scan_result.clone())
                        .map(|scan_result| Some(scan_result))
                        .unwrap_or_else(|e| {
                            // TODO(fxbug.dev/83708): Report details about which
                            // scan result failed to convert if possible.
                            error!("ScanResult conversion failed: {:?}", e);
                            None
                        })
                })
                .collect::<Vec<_>>()
        })
        .map_err(|scan_error_code| {
            if let Some(telemetry_sender) = telemetry_sender {
                log_metric_for_scan_error(&scan_error_code, telemetry_sender)
            }

            match scan_error_code {
                fidl_sme::ScanErrorCode::ShouldWait
                | fidl_sme::ScanErrorCode::CanceledByDriverOrFirmware => {
                    info!("Scan cancelled by SME, retry indicated: {:?}", scan_error_code);
                    types::ScanError::Cancelled
                }
                _ => {
                    error!("Scan error from SME: {:?}", scan_error_code);
                    types::ScanError::GeneralError
                }
            }
        })
}

/// Handles incoming scan requests by creating a new SME scan request. Will retry scan once if SME
/// returns a ScanErrorCode::Cancelled. On successful scan, also provides scan results to the
/// Emergency Location Provider.
async fn perform_scan(
    iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>,
    saved_networks_manager: Arc<dyn SavedNetworksManagerApi>,
    mut location_sensor_updater: impl ScanResultUpdate,
    _scan_reason: ScanReason,
    telemetry_sender: Option<TelemetrySender>,
) -> Result<Vec<types::ScanResult>, types::ScanError> {
    let mut bss_by_network: HashMap<SmeNetworkIdentifier, Vec<types::Bss>> = HashMap::new();
    let scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
    // Passive Scan. If scan returns cancelled error code, wait one second and retry once.
    for iter in 0..2 {
        let sme_proxy = match iface_manager.lock().await.get_sme_proxy_for_scan().await {
            Ok(proxy) => proxy,
            Err(_) => {
                warn!("Failed to get sme proxy for passive scan");
                return Err(types::ScanError::GeneralError);
            }
        };
        // TODO(fxbug.dev/111468) Log metrics when this times out so we are aware of the issue.
        let scan_results = sme_scan(&sme_proxy, scan_request.clone(), telemetry_sender.clone())
            .on_timeout(SCAN_TIMEOUT, || {
                error!("Timed out waiting on scan response from SME");
                Err(fidl_policy::ScanErrorCode::GeneralError)
            })
            .await;
        report_scan_defect(&sme_proxy, &scan_results).await;

        match scan_results {
            Ok(results) => {
                record_scan_results(vec![], &results, saved_networks_manager.clone()).await;
                insert_bss_to_network_bss_map(&mut bss_by_network, results, true);
                break;
            }
            Err(scan_err) => match scan_err {
                types::ScanError::GeneralError => {
                    return Err(scan_err);
                }
                types::ScanError::Cancelled => {
                    if iter > 0 {
                        return Err(scan_err);
                    }
                    info!("Driver requested a delay before retrying scan");
                    fasync::Timer::new(zx::Duration::from_millis(SCAN_RETRY_DELAY_MS).after_now())
                        .await;
                }
            },
        }
    }

    // If the passive scan results are empty, report an empty scan results metric.
    if bss_by_network.is_empty() {
        if let Some(telemetry_sender) = telemetry_sender.clone() {
            telemetry_sender.send(TelemetryEvent::ScanDefect(ScanIssue::EmptyScanResults))
        }
    }

    let scan_results = network_bss_map_to_scan_result(bss_by_network);

    // Send scan results to Location, but include a timeout so that a stalled consumer doesn't
    // stop us from reaching the `return Ok(scan_results)` at the end of this function.
    // TODO(fxbug.dev/73821): the timeout mechanism is temporary, once the scan manager is
    // implemented with its own event loop, we can send these results to the consumers in a
    // separate task, without blocking the return.
    location_sensor_updater
        .update_scan_results(&scan_results)
        .on_timeout(zx::Duration::from_seconds(SCAN_CONSUMER_MAX_SECONDS_ALLOWED), || {
            error!("Timed out waiting for consumers of scan results");
            ()
        })
        .await;

    Ok(scan_results)
}

/// Perform a directed active scan for a given network on given channels.
async fn perform_directed_active_scan(
    iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>,
    saved_networks_manager: Arc<dyn SavedNetworksManagerApi>,
    ssids: Vec<types::Ssid>,
    channels: Vec<types::WlanChan>,
    _scan_reason: ScanReason,
    telemetry_sender: Option<TelemetrySender>,
) -> Result<Vec<types::ScanResult>, types::ScanError> {
    let scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
        ssids: ssids.iter().map(|ssid| ssid.to_vec()).collect(),
        channels: channels.iter().map(|chan| chan.primary).collect(),
    });

    let sme_proxy = match iface_manager.lock().await.get_sme_proxy_for_scan().await {
        Ok(proxy) => proxy,
        Err(_) => {
            warn!("Failed to get sme proxy for directed active scan");
            return Err(types::ScanError::GeneralError);
        }
    };

    let sme_result = sme_scan(&sme_proxy, scan_request, telemetry_sender).await;

    report_scan_defect(&sme_proxy, &sme_result).await;

    match sme_result {
        Ok(results) => {
            record_scan_results(ssids.clone(), &results, saved_networks_manager).await;

            let mut bss_by_network: HashMap<SmeNetworkIdentifier, Vec<types::Bss>> = HashMap::new();
            insert_bss_to_network_bss_map(&mut bss_by_network, results, false);

            // The active scan targets a specific SSID, ensure only that SSID is present in results
            bss_by_network.retain(|network_id, _| ssids.contains(&network_id.ssid));

            Ok(network_bss_map_to_scan_result(bss_by_network))
        }
        Err(e) => Err(e),
    }
}

/// Update the hidden network probabilties of saved networks seen (or not) in a scan.
async fn record_scan_results(
    target_ssids: Vec<types::Ssid>,
    scan_result_list: &Vec<wlan_common::scan::ScanResult>,
    saved_networks_manager: Arc<dyn SavedNetworksManagerApi>,
) {
    let ids = scan_result_list
        .iter()
        .map(|scan_result| types::NetworkIdentifierDetailed {
            ssid: scan_result.bss_description.ssid.clone(),
            security_type: scan_result.bss_description.protection().into(),
        })
        .collect();
    saved_networks_manager.record_scan_result(target_ssids, ids).await;
}

/// The location sensor module uses scan results to help determine the
/// device's location, for use by the Emergency Location Provider.
struct LocationSensorUpdater {
    pub wpa3_supported: bool,
}
#[async_trait]
impl ScanResultUpdate for LocationSensorUpdater {
    async fn update_scan_results(&mut self, scan_results: &Vec<types::ScanResult>) {
        async fn send_results(scan_results: &Vec<fidl_policy::ScanResult>) -> Result<(), Error> {
            // Get an output iterator
            let (iter, server) =
                fidl::endpoints::create_endpoints::<fidl_policy::ScanResultIteratorMarker>()
                    .map_err(|err| format_err!("failed to create iterator: {:?}", err))?;
            let location_watcher_proxy =
                connect_to_protocol::<fidl_location_sensor::WlanBaseStationWatcherMarker>()
                    .map_err(|err| {
                        format_err!("failed to connect to location sensor service: {:?}", err)
                    })?;
            location_watcher_proxy
                .report_current_stations(iter)
                .map_err(|err| format_err!("failed to call location sensor service: {:?}", err))?;

            // Send results to the iterator
            fidl_conversion::send_scan_results_over_fidl(server, scan_results).await
        }

        let scan_results =
            fidl_conversion::scan_result_to_policy_scan_result(scan_results, self.wpa3_supported);
        // Filter out any errors and just log a message.
        // No error recovery, we'll just try again next time a scan result comes in.
        if let Err(e) = send_results(&scan_results).await {
            info!("Failed to send scan results to location sensor: {:?}", e)
        } else {
            debug!("Updated location sensor")
        };
    }
}

/// Converts sme::ScanResult to our internal BSS type, then adds it to the provided bss_by_network map.
/// Only keeps the first unique instance of a BSSID
fn insert_bss_to_network_bss_map(
    bss_by_network: &mut HashMap<SmeNetworkIdentifier, Vec<types::Bss>>,
    scan_result_list: Vec<wlan_common::scan::ScanResult>,
    observed_in_passive_scan: bool,
) {
    for scan_result in scan_result_list.into_iter() {
        let protection: types::SecurityTypeDetailed =
            scan_result.bss_description.protection().into();
        if (protection) == types::SecurityTypeDetailed::Unknown {
            // Print a space-efficient version of the IEs
            info!(
                "Encountered unknown protection, ies: [{:?}]",
                scan_result.bss_description.ies().iter().map(|n| n.to_string()).join(",")
            );
        };
        let entry = bss_by_network
            .entry(SmeNetworkIdentifier {
                ssid: scan_result.bss_description.ssid.clone(),
                protection: protection,
            })
            .or_insert(vec![]);
        // Check if this BSSID is already in the hashmap
        if !entry.iter().any(|existing_bss| existing_bss.bssid == scan_result.bss_description.bssid)
        {
            entry.push(types::Bss {
                bssid: scan_result.bss_description.bssid,
                rssi: scan_result.bss_description.rssi_dbm,
                snr_db: scan_result.bss_description.snr_db,
                channel: scan_result.bss_description.channel,
                timestamp: scan_result.timestamp,
                observation: if observed_in_passive_scan {
                    types::ScanObservation::Passive
                } else {
                    types::ScanObservation::Active
                },
                compatibility: scan_result.compatibility,
                bss_description: scan_result.bss_description.into(),
            });
        };
    }
}

fn network_bss_map_to_scan_result(
    mut bss_by_network: HashMap<SmeNetworkIdentifier, Vec<types::Bss>>,
) -> Vec<types::ScanResult> {
    let mut scan_results: Vec<types::ScanResult> = bss_by_network
        .drain()
        .map(|(SmeNetworkIdentifier { ssid, protection }, bss_entries)| {
            let compatibility = if bss_entries.iter().any(|bss| bss.is_compatible()) {
                fidl_policy::Compatibility::Supported
            } else {
                fidl_policy::Compatibility::DisallowedNotSupported
            };
            types::ScanResult {
                ssid,
                security_type_detailed: protection,
                entries: bss_entries,
                compatibility,
            }
        })
        .collect();

    scan_results.sort_by(|a, b| a.ssid.cmp(&b.ssid));
    return scan_results;
}

fn log_metric_for_scan_error(reason: &fidl_sme::ScanErrorCode, telemetry_sender: TelemetrySender) {
    let metric_type = match *reason {
        fidl_sme::ScanErrorCode::NotSupported
        | fidl_sme::ScanErrorCode::InternalError
        | fidl_sme::ScanErrorCode::InternalMlmeError => ScanIssue::ScanFailure,
        fidl_sme::ScanErrorCode::ShouldWait
        | fidl_sme::ScanErrorCode::CanceledByDriverOrFirmware => ScanIssue::AbortedScan,
    };

    telemetry_sender.send(TelemetryEvent::ScanDefect(metric_type));
}

async fn report_scan_defect(
    sme_proxy: &SmeForScan,
    scan_result: &Result<Vec<wlan_common::scan::ScanResult>, types::ScanError>,
) {
    match scan_result {
        Ok(results) => {
            if results.is_empty() {
                sme_proxy.log_empty_scan_defect();
            }
        }
        Err(types::ScanError::GeneralError) => sme_proxy.log_failed_scan_defect(),
        Err(types::ScanError::Cancelled) => sme_proxy.log_aborted_scan_defect(),
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            access_point::state_machine as ap_fsm,
            config_management::network_config::Credential,
            mode_management::{
                iface_manager_api::{ConnectAttemptRequest, SmeForScan},
                Defect, IfaceFailure,
            },
            util::testing::{
                fakes::FakeSavedNetworksManager, generate_channel, generate_random_sme_scan_result,
            },
        },
        anyhow::Error,
        fidl::endpoints::{create_proxy, ControlHandle, Responder},
        fidl_fuchsia_wlan_common_security as fidl_security, fuchsia_async as fasync,
        fuchsia_zircon as zx,
        futures::{
            channel::{mpsc, oneshot},
            lock::Mutex,
            task::Poll,
        },
        pin_utils::pin_mut,
        std::{convert::TryInto, sync::Arc},
        test_case::test_case,
        wlan_common::{
            assert_variant, random_fidl_bss_description, scan::Compatibility,
            security::SecurityDescriptor,
        },
    };

    struct FakeIfaceManager {
        pub sme_proxy: fidl_fuchsia_wlan_sme::ClientSmeProxy,
        pub wpa3_capable: bool,
        pub defect_sender: mpsc::UnboundedSender<Defect>,
        pub defect_receiver: mpsc::UnboundedReceiver<Defect>,
    }

    impl FakeIfaceManager {
        pub fn new(proxy: fidl_fuchsia_wlan_sme::ClientSmeProxy) -> Self {
            let (defect_sender, defect_receiver) = mpsc::unbounded();
            FakeIfaceManager {
                sme_proxy: proxy,
                wpa3_capable: true,
                defect_sender,
                defect_receiver,
            }
        }
    }

    #[async_trait]
    impl IfaceManagerApi for FakeIfaceManager {
        async fn disconnect(
            &mut self,
            _network_id: types::NetworkIdentifier,
            _reason: types::DisconnectReason,
        ) -> Result<(), Error> {
            unimplemented!()
        }

        async fn connect(&mut self, _connect_req: ConnectAttemptRequest) -> Result<(), Error> {
            unimplemented!()
        }

        async fn record_idle_client(&mut self, _iface_id: u16) -> Result<(), Error> {
            unimplemented!()
        }

        async fn has_idle_client(&mut self) -> Result<bool, Error> {
            unimplemented!()
        }

        async fn handle_added_iface(&mut self, _iface_id: u16) -> Result<(), Error> {
            unimplemented!()
        }

        async fn handle_removed_iface(&mut self, _iface_id: u16) -> Result<(), Error> {
            unimplemented!()
        }

        async fn get_sme_proxy_for_scan(&mut self) -> Result<SmeForScan, Error> {
            Ok(SmeForScan::new(self.sme_proxy.clone(), 0, self.defect_sender.clone()))
        }

        async fn stop_client_connections(
            &mut self,
            _reason: types::DisconnectReason,
        ) -> Result<(), Error> {
            unimplemented!()
        }

        async fn start_client_connections(&mut self) -> Result<(), Error> {
            unimplemented!()
        }

        async fn start_ap(
            &mut self,
            _config: ap_fsm::ApConfig,
        ) -> Result<oneshot::Receiver<()>, Error> {
            unimplemented!()
        }

        async fn stop_ap(&mut self, _ssid: types::Ssid, _password: Vec<u8>) -> Result<(), Error> {
            unimplemented!()
        }

        async fn stop_all_aps(&mut self) -> Result<(), Error> {
            unimplemented!()
        }

        async fn has_wpa3_capable_client(&mut self) -> Result<bool, Error> {
            Ok(self.wpa3_capable)
        }

        async fn set_country(
            &mut self,
            _country_code: Option<[u8; types::REGION_CODE_LEN]>,
        ) -> Result<(), Error> {
            unimplemented!()
        }
    }

    /// Creates a Client wrapper.
    async fn create_iface_manager(
    ) -> (Arc<Mutex<FakeIfaceManager>>, fidl_sme::ClientSmeRequestStream) {
        let (client_sme, remote) =
            create_proxy::<fidl_sme::ClientSmeMarker>().expect("error creating proxy");
        let iface_manager = FakeIfaceManager::new(client_sme);
        let iface_manager = Arc::new(Mutex::new(iface_manager));
        (iface_manager, remote.into_stream().expect("failed to create stream"))
    }

    /// Creates an SME proxy for tests.
    async fn create_sme_proxy() -> (fidl_sme::ClientSmeProxy, fidl_sme::ClientSmeRequestStream) {
        let (client_sme, remote) =
            create_proxy::<fidl_sme::ClientSmeMarker>().expect("error creating proxy");
        (client_sme, remote.into_stream().expect("failed to create stream"))
    }

    struct MockScanResultConsumer {
        scan_results: Arc<Mutex<Option<Vec<types::ScanResult>>>>,
    }
    impl MockScanResultConsumer {
        fn new() -> (Self, Arc<Mutex<Option<Vec<types::ScanResult>>>>) {
            let scan_results = Arc::new(Mutex::new(None));
            (Self { scan_results: Arc::clone(&scan_results) }, scan_results)
        }
    }
    #[async_trait]
    impl ScanResultUpdate for MockScanResultConsumer {
        async fn update_scan_results(&mut self, scan_results: &Vec<types::ScanResult>) {
            let mut guard = self.scan_results.lock().await;
            *guard = Some(scan_results.clone());
        }
    }

    // Creates test data for the scan functions.
    struct MockScanData {
        passive_input_aps: Vec<fidl_sme::ScanResult>,
        passive_internal_aps: Vec<types::ScanResult>,
    }
    fn create_scan_ap_data() -> MockScanData {
        let passive_result_1 = fidl_sme::ScanResult {
            compatibility: Some(Box::new(fidl_sme::Compatibility {
                mutual_security_protocols: vec![fidl_security::Protocol::Wpa3Personal],
            })),
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa3,
                bssid: [0, 0, 0, 0, 0, 0],
                ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                rssi_dbm: 0,
                snr_db: 1,
                channel: types::WlanChan::new(1, types::Cbw::Cbw20),
            ),
        };
        let passive_result_2 = fidl_sme::ScanResult {
            compatibility: Some(Box::new(fidl_sme::Compatibility {
                mutual_security_protocols: vec![fidl_security::Protocol::Wpa2Personal],
            })),
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa2,
                bssid: [1, 2, 3, 4, 5, 6],
                ssid: types::Ssid::try_from("unique ssid").unwrap(),
                rssi_dbm: 7,
                snr_db: 2,
                channel: types::WlanChan::new(8, types::Cbw::Cbw20),
            ),
        };
        let passive_result_3 = fidl_sme::ScanResult {
            compatibility: None,
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa3,
                bssid: [7, 8, 9, 10, 11, 12],
                ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                rssi_dbm: 13,
                snr_db: 3,
                channel: types::WlanChan::new(11, types::Cbw::Cbw20),
            ),
        };

        let passive_input_aps =
            vec![passive_result_1.clone(), passive_result_2.clone(), passive_result_3.clone()];
        // input_aps contains some duplicate SSIDs, which should be
        // grouped in the output.
        let passive_internal_aps = vec![
            types::ScanResult {
                ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                security_type_detailed: types::SecurityTypeDetailed::Wpa3Personal,
                entries: vec![
                    types::Bss {
                        bssid: types::Bssid([0, 0, 0, 0, 0, 0]),
                        rssi: 0,
                        timestamp: zx::Time::from_nanos(passive_result_1.timestamp_nanos),
                        snr_db: 1,
                        channel: types::WlanChan::new(1, types::Cbw::Cbw20),
                        observation: types::ScanObservation::Passive,
                        compatibility: Compatibility::expect_some([
                            SecurityDescriptor::WPA3_PERSONAL,
                        ]),
                        bss_description: passive_result_1.bss_description.clone(),
                    },
                    types::Bss {
                        bssid: types::Bssid([7, 8, 9, 10, 11, 12]),
                        rssi: 13,
                        timestamp: zx::Time::from_nanos(passive_result_3.timestamp_nanos),
                        snr_db: 3,
                        channel: types::WlanChan::new(11, types::Cbw::Cbw20),
                        observation: types::ScanObservation::Passive,
                        compatibility: None,
                        bss_description: passive_result_3.bss_description.clone(),
                    },
                ],
                compatibility: types::Compatibility::Supported,
            },
            types::ScanResult {
                ssid: types::Ssid::try_from("unique ssid").unwrap(),
                security_type_detailed: types::SecurityTypeDetailed::Wpa2Personal,
                entries: vec![types::Bss {
                    bssid: types::Bssid([1, 2, 3, 4, 5, 6]),
                    rssi: 7,
                    timestamp: zx::Time::from_nanos(passive_result_2.timestamp_nanos),
                    snr_db: 2,
                    channel: types::WlanChan::new(8, types::Cbw::Cbw20),
                    observation: types::ScanObservation::Passive,
                    compatibility: Compatibility::expect_some([SecurityDescriptor::WPA2_PERSONAL]),
                    bss_description: passive_result_2.bss_description.clone(),
                }],
                compatibility: types::Compatibility::Supported,
            },
        ];

        MockScanData { passive_input_aps, passive_internal_aps }
    }

    fn create_telemetry_sender_and_receiver() -> (TelemetrySender, mpsc::Receiver<TelemetryEvent>) {
        let (sender, receiver) = mpsc::channel::<TelemetryEvent>(100);
        let sender = TelemetrySender::new(sender);
        (sender, receiver)
    }

    fn get_fake_defects(
        exec: &mut fasync::TestExecutor,
        iface_manager: Arc<Mutex<FakeIfaceManager>>,
    ) -> Vec<Defect> {
        let defects_fut = async move {
            let mut iface_manager = iface_manager.lock().await;
            let mut defects = Vec::<Defect>::new();
            while let Ok(Some(defect)) = iface_manager.defect_receiver.try_next() {
                defects.push(defect)
            }

            defects
        };
        pin_mut!(defects_fut);
        assert_variant!(exec.run_until_stalled(&mut defects_fut), Poll::Ready(defects) => defects)
    }

    #[fuchsia::test]
    fn sme_scan_with_passive_request() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (sme_proxy, mut sme_stream) = exec.run_singlethreaded(create_sme_proxy());
        let (defect_sender, _) = mpsc::unbounded();
        let sme_proxy = SmeForScan::new(sme_proxy, 0, defect_sender);
        let (telemetry_sender, mut telemetry_receiver) = create_telemetry_sender_and_receiver();

        // Issue request to scan.
        let scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        let scan_fut = sme_scan(&sme_proxy, scan_request.clone(), Some(telemetry_sender));
        pin_mut!(scan_fut);

        // Request scan data from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data
        let MockScanData { passive_input_aps: input_aps, passive_internal_aps: _ } =
            create_scan_ap_data();
        // Validate the SME received the scan_request and send back mock data
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                req, responder,
            }))) => {
                assert_eq!(req, scan_request);
                responder.send(&mut Ok(input_aps.clone())).expect("failed to send scan data");
            }
        );

        // Check for results
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
            let scan_results: Vec<fidl_sme::ScanResult> = result.expect("failed to get scan results")
                .iter().map(|r| r.clone().into()).collect();
            assert_eq!(scan_results, input_aps);
        });

        // No further requests to the sme
        assert_variant!(exec.run_until_stalled(&mut sme_stream.next()), Poll::Pending);

        // No metric should be logged since the scan was successful.
        assert_variant!(telemetry_receiver.try_next(), Ok(None))
    }

    #[fuchsia::test]
    fn sme_scan_with_active_request() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (sme_proxy, mut sme_stream) = exec.run_singlethreaded(create_sme_proxy());
        let (defect_sender, _) = mpsc::unbounded();
        let sme_proxy = SmeForScan::new(sme_proxy, 0, defect_sender);
        let (telemetry_sender, mut telemetry_receiver) = create_telemetry_sender_and_receiver();

        // Issue request to scan.
        let scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
            ssids: vec![
                types::Ssid::try_from("foo_ssid").unwrap().into(),
                types::Ssid::try_from("bar_ssid").unwrap().into(),
            ],
            channels: vec![1, 20],
        });
        let scan_fut = sme_scan(&sme_proxy, scan_request.clone(), Some(telemetry_sender));
        pin_mut!(scan_fut);

        // Request scan data from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data
        let MockScanData { passive_input_aps: input_aps, passive_internal_aps: _ } =
            create_scan_ap_data();
        // Validate the SME received the scan_request and send back mock data
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                req, responder,
            }))) => {
                assert_eq!(req, scan_request);
                responder.send(&mut Ok(input_aps.clone())).expect("failed to send scan data");
            }
        );

        // Check for results
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
            let scan_results: Vec<fidl_sme::ScanResult> = result.expect("failed to get scan results")
                .iter().map(|r| r.clone().into()).collect();
            assert_eq!(scan_results, input_aps);
        });

        // No further requests to the sme
        assert_variant!(exec.run_until_stalled(&mut sme_stream.next()), Poll::Pending);

        // No metric should be logged since the scan was successful.
        assert_variant!(telemetry_receiver.try_next(), Ok(None))
    }

    #[fuchsia::test]
    fn sme_channel_closed_while_awaiting_scan_results() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (sme_proxy, mut sme_stream) = exec.run_singlethreaded(create_sme_proxy());
        let (defect_sender, _) = mpsc::unbounded();
        let sme_proxy = SmeForScan::new(sme_proxy, 0, defect_sender);
        let (telemetry_sender, mut telemetry_receiver) = create_telemetry_sender_and_receiver();

        // Issue request to scan.
        let scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        let scan_fut = sme_scan(&sme_proxy, scan_request, Some(telemetry_sender));
        pin_mut!(scan_fut);

        // Request scan data from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Check that a scan request was sent to the sme and close the channel
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                req: _, responder,
            }))) => {
                // Shutdown SME request stream.
                responder.control_handle().shutdown();
                // TODO(fxbug.dev/81036): Drop the stream to shutdown the channel.
                drop(sme_stream);
            }
        );

        // Check for results
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
            let error = result.expect_err("did not expect scan results");
            assert_eq!(error, types::ScanError::GeneralError);
        });

        // No metric should be logged since the interface went away.
        assert_variant!(telemetry_receiver.try_next(), Ok(None))
    }

    #[fuchsia::test]
    fn basic_scan() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (location_sensor, location_sensor_results) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());
        let (telemetry_sender, mut telemetry_receiver) = create_telemetry_sender_and_receiver();

        // Issue request to scan.
        let scan_fut = perform_scan(
            client,
            saved_networks_manager,
            location_sensor,
            ScanReason::NetworkSelection,
            Some(telemetry_sender),
        );
        pin_mut!(scan_fut);

        // Progress scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data and send it via the SME
        let MockScanData { passive_input_aps: input_aps, passive_internal_aps: internal_aps } =
            create_scan_ap_data();
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                req, responder,
            }))) => {
                assert_eq!(req, fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {}));
                responder.send(&mut Ok(input_aps.clone())).expect("failed to send scan data");
            }
        );

        // Process scan handler
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(results) => {
            assert_eq!(results.unwrap(), internal_aps);
        });

        // Check scan consumer got results
        assert_eq!(
            *exec.run_singlethreaded(location_sensor_results.lock()),
            Some(internal_aps.clone())
        );

        // Since the scanning process went off without a hitch, there should not be any defect
        // metrics logged.
        assert_variant!(telemetry_receiver.try_next(), Ok(None));
    }

    #[fuchsia::test]
    fn empty_scan_results() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (location_sensor, location_sensor_results) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());
        let (telemetry_sender, mut telemetry_receiver) = create_telemetry_sender_and_receiver();

        // Issue request to scan.
        let scan_fut = perform_scan(
            client.clone(),
            saved_networks_manager,
            location_sensor,
            ScanReason::NetworkSelection,
            Some(telemetry_sender),
        );
        pin_mut!(scan_fut);

        // Progress scan handler
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data and send it via the SME
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                req, responder,
            }))) => {
                assert_eq!(req, fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {}));
                responder.send(&mut Ok(vec![])).expect("failed to send scan data");
            }
        );

        // Process response from SME (which is empty) and expect the future to complete.
        assert_variant!(exec.run_until_stalled(&mut scan_fut),  Poll::Ready(results) => {
            assert!(results.unwrap().is_empty());
        });

        // Check scan consumer got results
        assert_eq!(*exec.run_singlethreaded(location_sensor_results.lock()), Some(vec![]));

        // Verify that an empty scan result has been logged
        assert_variant!(
            telemetry_receiver.try_next(),
            Ok(Some(TelemetryEvent::ScanDefect(issue))) => {
                assert_eq!(issue, ScanIssue::EmptyScanResults)
        });

        // Verify that a defect was logged.
        let logged_defects = get_fake_defects(&mut exec, client);
        let expected_defects = vec![Defect::Iface(IfaceFailure::EmptyScanResults { iface_id: 0 })];
        assert_eq!(logged_defects, expected_defects);
    }

    /// Verify that only a passive scan occurs if all saved networks have a 0
    /// hidden network probability.
    #[fuchsia::test]
    fn passive_scan_only_with_zero_hidden_network_probabilities() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (location_sensor, _) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Create passive scan info
        let MockScanData { passive_input_aps: input_aps, passive_internal_aps: internal_aps } =
            create_scan_ap_data();

        // Save a network and set its hidden probability to 0
        let network_id = types::NetworkIdentifier {
            ssid: types::Ssid::try_from("some ssid").unwrap(),
            security_type: types::SecurityType::Wpa3,
        };
        assert!(exec
            .run_singlethreaded(
                saved_networks_manager
                    .store(network_id.clone(), Credential::Password(b"randompass".to_vec()))
            )
            .expect("failed to store network")
            .is_none());
        exec.run_singlethreaded(saved_networks_manager.update_hidden_prob(network_id.clone(), 0.0));
        let config = exec
            .run_singlethreaded(saved_networks_manager.lookup(&network_id.clone()))
            .pop()
            .expect("failed to lookup");
        assert_eq!(config.hidden_probability, 0.0);

        // Issue request to scan.
        let scan_fut = perform_scan(
            client,
            saved_networks_manager.clone(),
            location_sensor,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Progress scan handler
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data and send it via the SME
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                req, responder,
            }))) => {
                assert_eq!(req, fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {}));
                responder.send(&mut Ok(input_aps.clone())).expect("failed to send scan data");
            }
        );

        // Check for results, verifying no active scan results are included.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(results) => {
            assert_eq!(results.unwrap(), internal_aps);
        });
    }

    /// Verify that saved networks seen in passive scans have their hidden network probabilities updated.
    #[fuchsia::test]
    fn passive_scan_updates_hidden_network_probabilities() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (location_sensor, _) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Create the passive scan info
        let MockScanData { passive_input_aps: input_aps, passive_internal_aps: _ } =
            create_scan_ap_data();

        // Save a network that WILL be seen in the passive scan.
        let first_ap = wlan_common::scan::ScanResult::try_from(input_aps[0].clone()).unwrap();
        // If this changes, also change the `security_type` for `seen_in_passive_network` below
        assert_eq!(
            first_ap.bss_description.protection(),
            wlan_common::bss::Protection::Wpa3Personal
        );
        let seen_in_passive_network = types::NetworkIdentifier {
            ssid: first_ap.bss_description.ssid.clone(),
            security_type: types::SecurityType::Wpa3,
        };
        assert!(exec
            .run_singlethreaded(
                saved_networks_manager
                    .store(seen_in_passive_network, Credential::Password(b"randompass".to_vec()))
            )
            .expect("failed to store network")
            .is_none());

        // Save a network that will NOT be seen in the passive scan.
        let not_seen_net_id = types::NetworkIdentifier {
            ssid: types::Ssid::try_from("not_seen_net_id").unwrap(),
            security_type: types::SecurityType::Wpa,
        };
        assert!(exec
            .run_singlethreaded(
                saved_networks_manager
                    .store(not_seen_net_id.clone(), Credential::Password(b"foobarbaz".to_vec()))
            )
            .expect("failed to store network")
            .is_none());

        // Issue request to scan.
        let scan_fut = perform_scan(
            client,
            saved_networks_manager.clone(),
            location_sensor,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Progress scan handler
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data and send it via the SME
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                req, responder,
            }))) => {
                assert_eq!(req, fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {}));
                responder.send(&mut Ok(input_aps.clone())).expect("failed to send scan data");
            }
        );

        // Process response from SME. If an active scan is requested for the unseen network this
        // will be pending, otherwise it will be ready.
        let _ = exec.run_until_stalled(&mut scan_fut);

        // Verify that the passive scan results were recorded.
        assert!(*exec.run_singlethreaded(saved_networks_manager.scan_result_recorded.lock()));

        // Note: the decision to active scan is non-deterministic (using the hidden network probabilities),
        // no need to continue and verify the results in this test case.
    }

    #[fuchsia::test]
    fn insert_bss_to_network_bss_map_duplicated_bss() {
        let mut bss_by_network = HashMap::new();

        // Create some input data with duplicated BSSID and Network Identifiers
        let passive_result = fidl_sme::ScanResult {
            compatibility: Some(Box::new(fidl_sme::Compatibility {
                mutual_security_protocols: vec![fidl_security::Protocol::Wpa3Personal],
            })),
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa3,
                bssid: [0, 0, 0, 0, 0, 0],
                ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                rssi_dbm: 0,
                snr_db: 1,
                channel: types::WlanChan::new(1, types::Cbw::Cbw20),
            ),
        };

        let passive_input_aps = vec![
            passive_result.clone(),
            fidl_sme::ScanResult {
                compatibility: Some(Box::new(fidl_sme::Compatibility {
                    mutual_security_protocols: vec![fidl_security::Protocol::Wpa3Personal],
                })),
                timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                bss_description: random_fidl_bss_description!(
                    Wpa3,
                    bssid: [0, 0, 0, 0, 0, 0],
                    ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                        rssi_dbm: 13,
                        snr_db: 3,
                        channel: types::WlanChan::new(14, types::Cbw::Cbw20),
                ),
            },
        ];

        let expected_id = SmeNetworkIdentifier {
            ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
            protection: fidl_sme::Protection::Wpa3Personal,
        };

        // We should only see one entry for the duplicated BSSs in the passive scan results
        let expected_bss = vec![types::Bss {
            bssid: types::Bssid([0, 0, 0, 0, 0, 0]),
            rssi: 0,
            timestamp: zx::Time::from_nanos(passive_result.timestamp_nanos),
            snr_db: 1,
            channel: types::WlanChan::new(1, types::Cbw::Cbw20),
            observation: types::ScanObservation::Passive,
            compatibility: Compatibility::expect_some([SecurityDescriptor::WPA3_PERSONAL]),
            bss_description: passive_result.bss_description.clone(),
        }];

        insert_bss_to_network_bss_map(
            &mut bss_by_network,
            passive_input_aps
                .iter()
                .map(|scan_result| {
                    scan_result.clone().try_into().expect("Failed to convert ScanResult")
                })
                .collect::<Vec<wlan_common::scan::ScanResult>>(),
            true,
        );
        assert_eq!(bss_by_network.len(), 1);
        assert_eq!(bss_by_network[&expected_id], expected_bss);

        // Create some input data with one duplicate BSSID and one new BSSID
        let active_result = fidl_sme::ScanResult {
            compatibility: Some(Box::new(fidl_sme::Compatibility {
                mutual_security_protocols: vec![fidl_security::Protocol::Wpa3Personal],
            })),
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa3,
                ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                bssid: [1, 2, 3, 4, 5, 6],
                rssi_dbm: 101,
                snr_db: 101,
                channel: types::WlanChan::new(101, types::Cbw::Cbw40),
            ),
        };
        let active_input_aps = vec![
            fidl_sme::ScanResult {
                compatibility: Some(Box::new(fidl_sme::Compatibility {
                    mutual_security_protocols: vec![fidl_security::Protocol::Wpa3Personal],
                })),
                timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                bss_description: random_fidl_bss_description!(
                    Wpa3,
                    bssid: [0, 0, 0, 0, 0, 0],
                    ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                    rssi_dbm: 100,
                    snr_db: 100,
                    channel: types::WlanChan::new(100, types::Cbw::Cbw40),
                ),
            },
            active_result.clone(),
        ];

        // After the active scan, there should be a second bss included in the results
        let expected_bss = vec![
            types::Bss {
                bssid: types::Bssid([0, 0, 0, 0, 0, 0]),
                rssi: 0,
                timestamp: zx::Time::from_nanos(passive_result.timestamp_nanos),
                snr_db: 1,
                channel: types::WlanChan::new(1, types::Cbw::Cbw20),
                observation: types::ScanObservation::Passive,
                compatibility: Compatibility::expect_some([SecurityDescriptor::WPA3_PERSONAL]),
                bss_description: passive_result.bss_description.clone(),
            },
            types::Bss {
                bssid: types::Bssid([1, 2, 3, 4, 5, 6]),
                rssi: 101,
                timestamp: zx::Time::from_nanos(active_result.timestamp_nanos),
                snr_db: 101,
                channel: types::WlanChan::new(101, types::Cbw::Cbw40),
                observation: types::ScanObservation::Active,
                compatibility: Compatibility::expect_some([SecurityDescriptor::WPA3_PERSONAL]),
                bss_description: active_result.bss_description.clone(),
            },
        ];

        insert_bss_to_network_bss_map(
            &mut bss_by_network,
            active_input_aps
                .iter()
                .map(|scan_result| {
                    scan_result.clone().try_into().expect("Failed to convert ScanResult")
                })
                .collect::<Vec<wlan_common::scan::ScanResult>>(),
            false,
        );
        assert_eq!(bss_by_network.len(), 1);
        assert_eq!(bss_by_network[&expected_id], expected_bss);
    }

    #[test_case(
        fidl_sme::ScanErrorCode::InternalError,
        types::ScanError::GeneralError,
        Defect::Iface(IfaceFailure::FailedScan {iface_id: 0})
    )]
    #[test_case(
        fidl_sme::ScanErrorCode::InternalMlmeError,
        types::ScanError::GeneralError,
        Defect::Iface(IfaceFailure::FailedScan {iface_id: 0})
    )]
    #[test_case(
        fidl_sme::ScanErrorCode::NotSupported,
        types::ScanError::GeneralError,
        Defect::Iface(IfaceFailure::FailedScan {iface_id: 0})
    )]
    #[fuchsia::test(add_test_attr = false)]
    fn scan_error_no_retries(
        sme_failure_mode: fidl_sme::ScanErrorCode,
        policy_failure_mode: types::ScanError,
        expected_defect: Defect,
    ) {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (location_sensor, location_sensor_results) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Issue request to scan.
        let scan_fut = perform_scan(
            client.clone(),
            saved_networks_manager,
            location_sensor,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Send back a failure to the scan request that was generated.
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan { req: _, responder }))) => {
                // Send failed scan response.
                responder.send(&mut Err(sme_failure_mode)).expect("failed to send scan error");
            }
        );

        // The scan future should complete with an error.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
            assert_eq!(result, Err(policy_failure_mode));
        });

        // Check scan consumer have no results
        assert_eq!(*exec.run_singlethreaded(location_sensor_results.lock()), None);

        // A defect should have been logged on the IfaceManager.
        let logged_defects = get_fake_defects(&mut exec, client);
        let expected_defects = vec![expected_defect];
        assert_eq!(logged_defects, expected_defects);
    }

    #[test_case(fidl_sme::ScanErrorCode::ShouldWait, false; "Scan error ShouldWait with failed retry")]
    #[test_case(fidl_sme::ScanErrorCode::ShouldWait, true; "Scan error ShouldWait with successful retry")]
    #[test_case(fidl_sme::ScanErrorCode::CanceledByDriverOrFirmware, true; "Scan error CanceledByDriverOrFirmware with successful retry")]
    #[fuchsia::test]
    fn scan_error_retries_once(error_code: fidl_sme::ScanErrorCode, retry_succeeds: bool) {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (location_sensor, location_sensor_results) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Issue request to scan.
        let scan_fut = perform_scan(
            client.clone(),
            saved_networks_manager,
            location_sensor,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Progress scan handler
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Check that a scan request was sent to the sme and send back a cancellation error.
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                 req: _, responder,
            }))) => {
                // Send failed scan response.
                responder.send(&mut Err(error_code)).expect("failed to send scan error");
            }
        );

        // Process scan future. Should hit pending with retry timer.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Wake up the timer and advance the scanning future
        assert!(exec.wake_next_timer().is_some());

        // Process scan future, which will should be awaiting a scan retry,
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        if retry_succeeds {
            // Create mock scan data and send it via the SME
            let MockScanData { passive_input_aps: input_aps, passive_internal_aps: internal_aps } =
                create_scan_ap_data();
            assert_variant!(
                exec.run_until_stalled(&mut sme_stream.next()),
                Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                    req, responder,
                }))) => {
                    assert_eq!(req, fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {}));
                    responder.send(&mut Ok(input_aps.clone())).expect("failed to send scan data");
                }
            );

            // Check the scan results.
            assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(results) => {
                assert_eq!(results.unwrap(), internal_aps);
            });

            // Verify one defect was logged.
            let logged_defects = get_fake_defects(&mut exec, client);
            let expected_defects = vec![Defect::Iface(IfaceFailure::CanceledScan { iface_id: 0 })];
            assert_eq!(logged_defects, expected_defects);
        } else {
            // Send another cancelleation error code.
            assert_variant!(
                exec.run_until_stalled(&mut sme_stream.next()),
                Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                     req: _, responder,
                }))) => {
                    // Send failed scan response.
                    responder.send(&mut Err(error_code)).expect("failed to send scan error");
                }
            );

            // Process scan future, which should now have a response.
            assert_variant!(exec.run_until_stalled(&mut scan_fut),Poll::Ready(results) => {
                assert_eq!(results, Err(types::ScanError::Cancelled));
            });

            // Check scan consumer have no results
            assert_eq!(*exec.run_singlethreaded(location_sensor_results.lock()), None);

            // Verify that both defects were logged.
            let logged_defects = get_fake_defects(&mut exec, client);
            let expected_defects = vec![
                Defect::Iface(IfaceFailure::CanceledScan { iface_id: 0 }),
                Defect::Iface(IfaceFailure::CanceledScan { iface_id: 0 }),
            ];
            assert_eq!(logged_defects, expected_defects);
        }
    }

    #[fuchsia::test]
    fn overlapping_scans() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (location_sensor1, location_sensor_results1) = MockScanResultConsumer::new();
        let (location_sensor2, location_sensor_results2) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        let MockScanData { passive_input_aps, passive_internal_aps } = create_scan_ap_data();

        // Issue request to scan on both iterator.
        let scan_fut0 = perform_scan(
            client.clone(),
            saved_networks_manager.clone(),
            location_sensor1,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut0);

        let scan_fut1 = perform_scan(
            client.clone(),
            saved_networks_manager,
            location_sensor2,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut1);

        // Progress first scan handler forward
        assert_variant!(exec.run_until_stalled(&mut scan_fut0), Poll::Pending);

        // Check that a scan request was sent to the sme and send back results
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                req, responder,
            }))) => {
                // Check that this is the first scan request sent to sme.
                assert_eq!(req, fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {}));

                // Progress second scan handler forward
                assert_variant!(exec.run_until_stalled(&mut scan_fut1), Poll::Pending);
                // Check that the second scan request was sent to the sme and send back results
                assert_variant!(
                    exec.run_until_stalled(&mut sme_stream.next()),
                    Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                        req, responder,
                    }))) => {
                        assert_eq!(req, fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {}));
                        responder.send(&mut Ok(passive_input_aps.clone())).expect("failed to send scan data");
                    }
                );

                // Process SME result.
                assert_variant!(exec.run_until_stalled(&mut scan_fut1), Poll::Ready(results) => {
                    assert_eq!(results.unwrap(), passive_internal_aps.clone());
                });

                // Send the APs for the first iterator
                responder.send(&mut Ok(passive_input_aps.clone())).expect("failed to send scan data");
            }
        );

        // Process response from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut0), Poll::Ready(results) => {
            assert_eq!(results.unwrap(), passive_internal_aps);
        });

        // Check scan consumer got results
        assert_eq!(
            *exec.run_singlethreaded(location_sensor_results1.lock()),
            Some(passive_internal_aps.clone())
        );
        assert_eq!(
            *exec.run_singlethreaded(location_sensor_results2.lock()),
            Some(passive_internal_aps.clone())
        );
    }

    #[fuchsia::test]
    fn directed_active_scan_filters_desired_network() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Issue request to scan.
        let desired_ssid = types::Ssid::try_from("test_ssid").unwrap();
        let desired_channels = vec![generate_channel(1), generate_channel(36)];
        let scan_fut = perform_directed_active_scan(
            client,
            saved_networks_manager,
            vec![desired_ssid.clone()],
            desired_channels.clone(),
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Generate scan results
        let scan_result_aps = vec![
            fidl_sme::ScanResult {
                bss_description: random_fidl_bss_description!(Wpa3Enterprise, ssid: desired_ssid.clone()),
                ..generate_random_sme_scan_result()
            },
            fidl_sme::ScanResult {
                bss_description: random_fidl_bss_description!(Wpa2Wpa3, ssid: desired_ssid.clone()),
                ..generate_random_sme_scan_result()
            },
            fidl_sme::ScanResult {
                bss_description: random_fidl_bss_description!(Wpa2Wpa3, ssid: desired_ssid.clone()),
                ..generate_random_sme_scan_result()
            },
            fidl_sme::ScanResult {
                bss_description: random_fidl_bss_description!(Wpa2, ssid: types::Ssid::try_from("other ssid").unwrap()),
                ..generate_random_sme_scan_result()
            },
        ];

        // Progress scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Respond to the scan request
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                req, responder,
            }))) => {
                // Validate the request
                assert_eq!(req, fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
                    ssids: vec![desired_ssid.to_vec()],
                    channels: desired_channels.iter().map(|chan| chan.primary).collect(),
                }));
                // Send all the APs
                responder.send(&mut Ok(scan_result_aps.clone())).expect("failed to send scan data");
            }
        );

        // Check for results
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
            let mut result = result.unwrap();
            // Two networks with the desired SSID are present
            assert_eq!(result.len(), 2);
            result.sort_by_key(|r| r.security_type_detailed);
            // One network is WPA2WPA3
            assert_eq!(result[0].ssid, desired_ssid.clone());
            assert_eq!(result[0].security_type_detailed, types::SecurityTypeDetailed::Wpa2Wpa3Personal);
            // Two BSSs for this network
            assert_eq!(result[0].entries.len(), 2);
            // Other network is WPA3
            assert_eq!(result[1].ssid, desired_ssid.clone());
            assert_eq!(result[1].security_type_detailed, types::SecurityTypeDetailed::Wpa3Enterprise);
            // One BSS for this network
            assert_eq!(result[1].entries.len(), 1);
        });
    }

    #[fuchsia::test]
    fn scan_returns_error_on_timeout() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, _sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (location_sensor, location_sensor_results) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());
        let (telemetry_sender, _telemetry_receiver) = create_telemetry_sender_and_receiver();

        // Issue request to scan.
        let scan_fut = perform_scan(
            client,
            saved_networks_manager,
            location_sensor,
            ScanReason::NetworkSelection,
            Some(telemetry_sender),
        );
        pin_mut!(scan_fut);

        // Progress scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Wake up the next timer, which should be the timeour on the scan request.
        assert!(exec.wake_next_timer().is_some());

        // Check that an error is returned for the scan and there are no location sensor results.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(results) => {
            assert_eq!(results, Err(types::ScanError::GeneralError));
        });
        assert_eq!(*exec.run_singlethreaded(location_sensor_results.lock()), None);
    }

    #[test_case(fidl_sme::ScanErrorCode::NotSupported, ScanIssue::ScanFailure)]
    #[test_case(fidl_sme::ScanErrorCode::InternalError, ScanIssue::ScanFailure)]
    #[test_case(fidl_sme::ScanErrorCode::InternalMlmeError, ScanIssue::ScanFailure)]
    #[test_case(fidl_sme::ScanErrorCode::ShouldWait, ScanIssue::AbortedScan)]
    #[test_case(fidl_sme::ScanErrorCode::CanceledByDriverOrFirmware, ScanIssue::AbortedScan)]
    #[fuchsia::test(add_test_attr = false)]
    fn test_scan_error_metric_conversion(
        scan_error: fidl_sme::ScanErrorCode,
        expected_issue: ScanIssue,
    ) {
        let (telemetry_sender, mut telemetry_receiver) = create_telemetry_sender_and_receiver();
        log_metric_for_scan_error(&scan_error, telemetry_sender);
        assert_variant!(
            telemetry_receiver.try_next(),
            Ok(Some(TelemetryEvent::ScanDefect(issue))
        ) => {
            assert_eq!(issue, expected_issue)
        });
    }

    #[test_case(Err(types::ScanError::GeneralError), Some(Defect::Iface(IfaceFailure::FailedScan { iface_id: 0 })))]
    #[test_case(Err(types::ScanError::Cancelled), Some(Defect::Iface(IfaceFailure::CanceledScan { iface_id: 0 })))]
    #[test_case(Ok(vec![]), Some(Defect::Iface(IfaceFailure::EmptyScanResults { iface_id: 0 })))]
    #[test_case(Ok(vec![wlan_common::scan::ScanResult::try_from(
            fidl_sme::ScanResult {
                bss_description: random_fidl_bss_description!(Wpa2, ssid: types::Ssid::try_from("other ssid").unwrap()),
                ..generate_random_sme_scan_result()
            },
        ).expect("failed scan result conversion")]),
        None
    )]
    #[fuchsia::test(add_test_attr = false)]
    fn test_scan_defect_reporting(
        scan_result: Result<Vec<wlan_common::scan::ScanResult>, types::ScanError>,
        expected_defect: Option<Defect>,
    ) {
        let mut exec = fasync::TestExecutor::new().expect("failed to create and executor");
        let (iface_manager, _) = exec.run_singlethreaded(create_iface_manager());

        // Get the SME out of the IfaceManager.
        let sme = {
            let cloned_iface_manager = iface_manager.clone();
            let fut = async move {
                let mut iface_manager = cloned_iface_manager.lock().await;
                iface_manager.get_sme_proxy_for_scan().await
            };
            pin_mut!(fut);
            assert_variant!(exec.run_until_stalled(&mut fut), Poll::Ready(Ok(sme)) => sme)
        };

        // Report the desired scan error or success.
        let fut = report_scan_defect(&sme, &scan_result);
        pin_mut!(fut);
        assert_variant!(exec.run_until_stalled(&mut fut), Poll::Ready(()));

        // Based on the expected defect (or lack thereof), ensure that the correct value is obsered
        // on the receiver.
        // Verify that a defect was logged.
        let logged_defects = get_fake_defects(&mut exec, iface_manager);
        match expected_defect {
            Some(defect) => {
                assert_eq!(logged_defects, vec![defect])
            }
            None => assert!(logged_defects.is_empty()),
        }
    }

    #[test_case(
        fidl_sme::ScanErrorCode::InternalError,
        types::ScanError::GeneralError,
        Defect::Iface(IfaceFailure::FailedScan {iface_id: 0})
    )]
    #[test_case(
        fidl_sme::ScanErrorCode::InternalMlmeError,
        types::ScanError::GeneralError,
        Defect::Iface(IfaceFailure::FailedScan {iface_id: 0})
    )]
    #[test_case(
        fidl_sme::ScanErrorCode::NotSupported,
        types::ScanError::GeneralError,
        Defect::Iface(IfaceFailure::FailedScan {iface_id: 0})
    )]
    #[test_case(
        fidl_sme::ScanErrorCode::ShouldWait,
        types::ScanError::Cancelled,
        Defect::Iface(IfaceFailure::CanceledScan {iface_id: 0})
    )]
    #[test_case(
        fidl_sme::ScanErrorCode::CanceledByDriverOrFirmware,
        types::ScanError::Cancelled,
        Defect::Iface(IfaceFailure::CanceledScan {iface_id: 0})
    )]
    #[fuchsia::test(add_test_attr = false)]
    fn active_scan_fails(
        sme_failure_mode: fidl_sme::ScanErrorCode,
        policy_failure_mode: types::ScanError,
        expected_defect: Defect,
    ) {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Issue request to scan.
        let desired_ssid = types::Ssid::try_from("test_ssid").unwrap();
        let desired_channels = vec![];
        let scan_fut = perform_directed_active_scan(
            client.clone(),
            saved_networks_manager,
            vec![desired_ssid.clone()],
            desired_channels.clone(),
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Send back a failure to the scan request that was generated.
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan { req: _, responder }))) => {
                // Send failed scan response.
                responder.send(&mut Err(sme_failure_mode)).expect("failed to send scan error");
            }
        );

        // The scan future should complete with an error.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
            assert_eq!(result, Err(policy_failure_mode));
        });

        // A defect should have been logged on the IfaceManager.
        let logged_defects = get_fake_defects(&mut exec, client);
        assert_eq!(logged_defects, vec![expected_defect]);
    }

    #[fuchsia::test]
    fn active_scan_empty() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Issue request to scan.
        let ssid = "test_ssid";
        let desired_ssid = types::Ssid::try_from(ssid).unwrap();
        let desired_channels = vec![];
        let scan_fut = perform_directed_active_scan(
            client.clone(),
            saved_networks_manager,
            vec![desired_ssid.clone()],
            desired_channels.clone(),
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Send back empty scan results
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                req, responder,
            }))) => {
                assert_eq!(req, fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
                    ssids: vec![ssid.as_bytes().to_vec()],
                    channels: vec![],
                }));
                responder.send(&mut Ok(vec![])).expect("failed to send scan data");
            }
        );

        // The scan future should complete with an error.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(Ok(results)) => {
            assert!(results.is_empty())
        });

        // A defect should have been logged on the IfaceManager.
        let logged_defects = get_fake_defects(&mut exec, client);
        assert_eq!(
            logged_defects,
            vec![Defect::Iface(IfaceFailure::EmptyScanResults { iface_id: 0 })]
        );
    }
}
