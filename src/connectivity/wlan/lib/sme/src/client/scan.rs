// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{client::inspect, Error},
    fidl_fuchsia_wlan_common as fidl_common, fidl_fuchsia_wlan_internal as fidl_internal,
    fidl_fuchsia_wlan_mlme as fidl_mlme, fidl_fuchsia_wlan_sme as fidl_sme,
    fuchsia_inspect::NumericProperty,
    ieee80211::{Bssid, Ssid},
    log::warn,
    std::{
        collections::{hash_map, HashMap, HashSet},
        convert::TryInto,
        mem,
        sync::Arc,
    },
    wlan_common::{
        bss::BssDescription,
        channel::{Cbw, Channel},
        ie::IesMerger,
    },
};

const PASSIVE_SCAN_CHANNEL_MS: u32 = 200;
const ACTIVE_SCAN_PROBE_DELAY_MS: u32 = 5;
const ACTIVE_SCAN_CHANNEL_MS: u32 = 75;

// A "user"-initiated scan request for the purpose of discovering available networks
#[derive(Debug, PartialEq)]
pub struct DiscoveryScan<T> {
    tokens: Vec<T>,
    scan_request: fidl_sme::ScanRequest,
}

impl<T> DiscoveryScan<T> {
    pub fn new(token: T, scan_request: fidl_sme::ScanRequest) -> Self {
        Self { tokens: vec![token], scan_request }
    }

    pub fn matches(&self, scan: &DiscoveryScan<T>) -> bool {
        self.scan_request == scan.scan_request
    }

    pub fn merges(&mut self, mut scan: DiscoveryScan<T>) {
        self.tokens.append(&mut scan.tokens)
    }
}

pub struct ScanScheduler<T> {
    // The currently running scan. We assume that MLME can handle a single concurrent scan
    // regardless of its own state.
    current: ScanState<T>,
    // Pending discovery requests from the user
    pending_discovery: Vec<DiscoveryScan<T>>,
    device_info: Arc<fidl_mlme::DeviceInfo>,
    spectrum_management_support: fidl_common::SpectrumManagementSupport,
    last_mlme_txn_id: u64,
}

#[derive(Debug)]
enum ScanState<T> {
    NotScanning,
    ScanningToDiscover {
        cmd: DiscoveryScan<T>,
        mlme_txn_id: u64,
        bss_map: HashMap<Bssid, (fidl_internal::BssDescription, IesMerger)>,
    },
}

#[derive(Debug)]
pub struct ScanEnd<T> {
    pub tokens: Vec<T>,
    pub result_code: fidl_mlme::ScanResultCode,
    pub bss_description_list: Vec<BssDescription>,
}

impl<T> ScanScheduler<T> {
    pub fn new(
        device_info: Arc<fidl_mlme::DeviceInfo>,
        spectrum_management_support: fidl_common::SpectrumManagementSupport,
    ) -> Self {
        ScanScheduler {
            current: ScanState::NotScanning,
            pending_discovery: Vec::new(),
            device_info,
            spectrum_management_support,
            last_mlme_txn_id: 0,
        }
    }

    // Initiate a "discovery" scan. The scan might or might not begin immediately.
    // The request can be merged with any pending or ongoing requests.
    // If a ScanRequest is returned, the caller is responsible for forwarding it to MLME.
    pub fn enqueue_scan_to_discover(
        &mut self,
        s: DiscoveryScan<T>,
    ) -> Option<fidl_mlme::ScanRequest> {
        if let ScanState::ScanningToDiscover { cmd, .. } = &mut self.current {
            if cmd.matches(&s) {
                cmd.merges(s);
                return None;
            }
        }
        if let Some(scan_cmd) = self.pending_discovery.iter_mut().find(|cmd| cmd.matches(&s)) {
            scan_cmd.merges(s);
            return None;
        }
        self.pending_discovery.push(s);
        self.start_next_scan()
    }

    // Should be called for every OnScanResult event received from MLME.
    pub fn on_mlme_scan_result(
        &mut self,
        msg: fidl_mlme::ScanResult,
        sme_inspect: &Arc<inspect::SmeTree>,
    ) -> Result<(), Error> {
        match &mut self.current {
            ScanState::NotScanning => Err(Error::ScanResultNotScanning),
            ScanState::ScanningToDiscover { mlme_txn_id, .. } if *mlme_txn_id != msg.txn_id => {
                Err(Error::ScanResultWrongTxnId)
            }
            ScanState::ScanningToDiscover { bss_map, .. } => {
                maybe_insert_bss(bss_map, msg.bss, sme_inspect);
                Ok(())
            }
        }
    }

    // Should be called for every OnScanEnd event received from MLME.
    // If a ScanRequest is returned, the caller is responsible for forwarding it to MLME.
    pub fn on_mlme_scan_end(
        &mut self,
        msg: fidl_mlme::ScanEnd,
        sme_inspect: &Arc<inspect::SmeTree>,
    ) -> Result<(ScanEnd<T>, Option<fidl_mlme::ScanRequest>), Error> {
        match mem::replace(&mut self.current, ScanState::NotScanning) {
            ScanState::NotScanning => Err(Error::ScanEndNotScanning),
            ScanState::ScanningToDiscover { mlme_txn_id, .. } if mlme_txn_id != msg.txn_id => {
                Err(Error::ScanEndWrongTxnId)
            }
            ScanState::ScanningToDiscover { cmd, bss_map, .. } => {
                let scan_end = ScanEnd {
                    tokens: cmd.tokens,
                    result_code: msg.code,
                    bss_description_list: convert_bss_map(bss_map, None::<Ssid>, sme_inspect),
                };

                let request = self.start_next_scan();
                Ok((scan_end, request))
            }
        }
    }

    fn start_next_scan(&mut self) -> Option<fidl_mlme::ScanRequest> {
        let has_pending = !self.pending_discovery.is_empty();
        (matches!(self.current, ScanState::NotScanning) && has_pending).then(|| {
            self.last_mlme_txn_id += 1;
            let scan_cmd = self.pending_discovery.remove(0);
            let request = new_discovery_scan_request(
                self.last_mlme_txn_id,
                &scan_cmd,
                &self.device_info,
                self.spectrum_management_support,
            );
            self.current = ScanState::ScanningToDiscover {
                cmd: scan_cmd,
                mlme_txn_id: self.last_mlme_txn_id,
                bss_map: HashMap::new(),
            };
            request
        })
    }
}

fn maybe_insert_bss(
    bss_map: &mut HashMap<Bssid, (fidl_internal::BssDescription, IesMerger)>,
    mut fidl_bss: fidl_internal::BssDescription,
    sme_inspect: &Arc<inspect::SmeTree>,
) {
    let mut ies = vec![];
    std::mem::swap(&mut ies, &mut fidl_bss.ies);

    match bss_map.entry(Bssid(fidl_bss.bssid)) {
        hash_map::Entry::Occupied(mut entry) => {
            let (ref mut existing_bss, ref mut ies_merger) = entry.get_mut();

            if (fidl_bss.channel.primary != existing_bss.channel.primary)
                && (fidl_bss.rssi_dbm < existing_bss.rssi_dbm)
            {
                // Assume `fidl_bss` is from an "echo" Beacon frame from the same BSSID
                return;
            }

            ies_merger.merge(&ies[..]);
            if ies_merger.buffer_overflow() {
                warn!(
                    "Not merging some IEs due to running out of buffer. BSSID: {:?}",
                    sme_inspect.hasher.hash_mac_addr(&fidl_bss.bssid)
                );
            }
            *existing_bss = fidl_bss;
        }
        hash_map::Entry::Vacant(entry) => {
            let _ = entry.insert((fidl_bss, IesMerger::new(ies)));
        }
    }
}

fn convert_bss_map(
    bss_map: HashMap<Bssid, (fidl_internal::BssDescription, IesMerger)>,
    ssid_selector: Option<Ssid>,
    sme_inspect: &Arc<inspect::SmeTree>,
) -> Vec<BssDescription> {
    let bss_description_list =
        bss_map.into_iter().filter_map(|(_bssid, (mut bss, mut ies_merger))| {
            sme_inspect.scan_merge_ie_failures.add(ies_merger.merge_ie_failures() as u64);

            let mut ies = ies_merger.finalize();
            std::mem::swap(&mut ies, &mut bss.ies);
            let bss: Option<BssDescription> = bss.try_into().ok();
            if bss.is_none() {
                sme_inspect.scan_discard_fidl_bss.add(1);
            }
            bss
        });

    match ssid_selector {
        None => bss_description_list.collect(),
        Some(ssid) => bss_description_list.filter(|v| v.ssid == ssid).collect(),
    }
}

fn new_scan_request(
    mlme_txn_id: u64,
    scan_request: fidl_sme::ScanRequest,
    ssid_list: Vec<Ssid>,
    device_info: &fidl_mlme::DeviceInfo,
    spectrum_management_support: fidl_common::SpectrumManagementSupport,
) -> fidl_mlme::ScanRequest {
    let scan_req = fidl_mlme::ScanRequest {
        txn_id: mlme_txn_id,
        scan_type: fidl_mlme::ScanTypes::Passive,
        probe_delay: 0,
        // TODO(fxbug.dev/88658): SME silently ignores unsupported channels
        channel_list: get_channels_to_scan(
            &device_info,
            spectrum_management_support,
            &scan_request,
        ),
        ssid_list: ssid_list.into_iter().map(Ssid::into).collect(),
        min_channel_time: PASSIVE_SCAN_CHANNEL_MS,
        max_channel_time: PASSIVE_SCAN_CHANNEL_MS,
    };
    match scan_request {
        fidl_sme::ScanRequest::Active(active_scan_params) => fidl_mlme::ScanRequest {
            scan_type: fidl_mlme::ScanTypes::Active,
            ssid_list: active_scan_params.ssids,
            probe_delay: ACTIVE_SCAN_PROBE_DELAY_MS,
            min_channel_time: ACTIVE_SCAN_CHANNEL_MS,
            max_channel_time: ACTIVE_SCAN_CHANNEL_MS,
            ..scan_req
        },
        fidl_sme::ScanRequest::Passive(_) => scan_req,
    }
}

fn new_discovery_scan_request<T>(
    mlme_txn_id: u64,
    discovery_scan: &DiscoveryScan<T>,
    device_info: &fidl_mlme::DeviceInfo,
    spectrum_management_support: fidl_common::SpectrumManagementSupport,
) -> fidl_mlme::ScanRequest {
    new_scan_request(
        mlme_txn_id,
        discovery_scan.scan_request.clone(),
        vec![],
        device_info,
        spectrum_management_support,
    )
}

// TODO(fxbug.dev/88658): SME silently ignores unsupported channels
/// Get channels to scan depending on device's capability and scan type. If scan type is passive,
/// or if scan type is active but the device handles DFS channels, then the channels returned by
/// this function are the intersection of device's supported channels and Fuchsia supported
/// channels. If scan type is active and the device doesn't handle DFS channels, then the return
/// value excludes DFS channels.
///
/// Example:
///
/// Suppose that Fuchsia supported channels are [1, 2, 52], and 1, 2 are non-DFS channels while
/// 112 is DFS channel. Also suppose that device's supported channels are [1, 52] as parameter
/// to fidl_mlme::DeviceInfo below.
///
/// ScanType | Device handles DFS | Return values
/// ---------+--------------------+-----------------
/// Passive  | Y                  | [1, 52]
/// Passive  | N                  | [1, 52]
/// Active   | Y                  | [1, 52]
/// Active   | N                  | [1]
fn get_channels_to_scan(
    device_info: &fidl_mlme::DeviceInfo,
    spectrum_management_support: fidl_common::SpectrumManagementSupport,
    scan_request: &fidl_sme::ScanRequest,
) -> Vec<u8> {
    let mut operating_channels: HashSet<u8> = HashSet::new();
    for band in &device_info.bands {
        operating_channels.extend(&band.operating_channels);
    }

    SUPPORTED_20_MHZ_CHANNELS
        .iter()
        .filter(|channel| operating_channels.contains(channel))
        .filter(|channel| {
            if let &fidl_sme::ScanRequest::Passive(_) = scan_request {
                return true;
            };
            if spectrum_management_support.dfs.supported {
                return true;
            };
            !Channel::new(**channel, Cbw::Cbw20).is_dfs()
        })
        .filter(|chan| {
            // If this is an active scan and there are any channels specified by the caller,
            // only include those channels.
            if let &fidl_sme::ScanRequest::Active(ref options) = scan_request {
                if !options.channels.is_empty() {
                    return options.channels.contains(chan);
                }
            }
            true
        })
        .map(|chan| *chan)
        .collect()
}

// TODO(65792): Evaluate options for where and how we select what channels to scan.
// Firmware will reject channels if they are not allowed by the current regulatory region.
const SUPPORTED_20_MHZ_CHANNELS: &[u8] = &[
    // 5GHz UNII-1
    36, 40, 44, 48, // 5GHz UNII-2 Middle
    52, 56, 60, 64, // 5GHz UNII-2 Extended
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, // 5GHz UNII-3
    149, 153, 157, 161, 165, // 5GHz
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, // 2GHz
];

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_utils;
    use fuchsia_inspect::Inspector;
    use fuchsia_zircon as zx;
    use ieee80211::MacAddr;
    use regex::bytes::Regex;
    use std::{convert::TryFrom, fmt::Write};
    use test_case::test_case;
    use wlan_common::{
        assert_variant, fake_bss_description, fake_fidl_bss_description,
        hasher::WlanHasher,
        test_utils::{
            fake_capabilities::fake_5ghz_band_capability,
            fake_features::fake_spectrum_management_support_empty,
        },
    };

    const CLIENT_ADDR: MacAddr = [0x7A, 0xE7, 0x76, 0xD9, 0xF2, 0x67];

    fn passive_discovery_scan(token: i32) -> DiscoveryScan<i32> {
        DiscoveryScan::new(token, fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {}))
    }

    #[test]
    fn discovery_scan() {
        let mut sched = create_sched();
        let (_inspector, sme_inspect) = sme_inspect();
        let req = sched
            .enqueue_scan_to_discover(passive_discovery_scan(10))
            .expect("expected a ScanRequest");
        let txn_id = req.txn_id;
        sched
            .on_mlme_scan_result(
                fidl_mlme::ScanResult {
                    txn_id,
                    timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                    bss: fidl_internal::BssDescription {
                        bssid: [1; 6],
                        ..fake_fidl_bss_description!(Open, ssid: Ssid::try_from("foo").unwrap())
                    },
                },
                &sme_inspect,
            )
            .expect("expect scan result received");
        assert_variant!(
            sched.on_mlme_scan_result(
                fidl_mlme::ScanResult {
                    txn_id: txn_id + 100, // mismatching transaction id
                    timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                    bss: fidl_internal::BssDescription {
                        bssid: [2; 6],
                        ..fake_fidl_bss_description!(Open, ssid: Ssid::try_from("bar").unwrap())
                    },
                },
                &sme_inspect,
            ),
            Err(Error::ScanResultWrongTxnId)
        );
        sched
            .on_mlme_scan_result(
                fidl_mlme::ScanResult {
                    txn_id,
                    timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                    bss: fidl_internal::BssDescription {
                        bssid: [3; 6],
                        ..fake_fidl_bss_description!(Open, ssid: Ssid::try_from("qux").unwrap())
                    },
                },
                &sme_inspect,
            )
            .expect("expect scan result received");
        let (scan_end, mlme_req) = assert_variant!(
            sched.on_mlme_scan_end(
                fidl_mlme::ScanEnd { txn_id, code: fidl_mlme::ScanResultCode::Success },
                &sme_inspect,
            ),
            Ok((scan_end, mlme_req)) => (scan_end, mlme_req)
        );
        assert!(mlme_req.is_none());
        let (tokens, bss_description_list) = assert_variant!(
            scan_end,
            ScanEnd {
                tokens,
                result_code: fidl_mlme::ScanResultCode::Success,
                bss_description_list
            } => (tokens, bss_description_list),
            "expected discovery scan to be completed successfully"
        );
        assert_eq!(vec![10], tokens);
        let mut ssid_list =
            bss_description_list.into_iter().map(|bss| bss.ssid).collect::<Vec<_>>();
        ssid_list.sort();
        assert_eq!(vec![Ssid::try_from("foo").unwrap(), Ssid::try_from("qux").unwrap()], ssid_list);
    }

    #[test_case(vec![
        fake_fidl_bss_description!(Open, ssid: Ssid::try_from("bar").unwrap()),
        fake_fidl_bss_description!(Open, ssid: Ssid::try_from("baz").unwrap()),
    ], vec![fake_bss_description!(Open, ssid: Ssid::try_from("baz").unwrap())] ;
                "when latest BSS Description is new")]
    #[test_case(vec![
        fake_fidl_bss_description!(Open, rssi_dbm: -36, channel: Channel::new(149, Cbw::Cbw20)),
        fake_fidl_bss_description!(Open, rssi_dbm: -84, channel: Channel::new(165, Cbw::Cbw20)),
    ], vec![fake_bss_description!(Open, rssi_dbm: -36, channel: Channel::new(149, Cbw::Cbw20))] ;
                "when strong signal is first")]
    #[test_case(vec![
        fake_fidl_bss_description!(Open, rssi_dbm: -84, channel: Channel::new(64, Cbw::Cbw20)),
        fake_fidl_bss_description!(Open, rssi_dbm: -36, channel: Channel::new(50, Cbw::Cbw20)),
        fake_fidl_bss_description!(Open, rssi_dbm: -80, channel: Channel::new(36, Cbw::Cbw20)),
    ], vec![fake_bss_description!(Open, rssi_dbm: -36, channel: Channel::new(50, Cbw::Cbw20))];
                "when strong signal is middle")]
    #[test_case(vec![
        fake_fidl_bss_description!(Open, rssi_dbm: -84, channel: Channel::new(64, Cbw::Cbw20)),
        fake_fidl_bss_description!(Open, rssi_dbm: -80, channel: Channel::new(36, Cbw::Cbw20)),
        fake_fidl_bss_description!(Open, rssi_dbm: -36, channel: Channel::new(50, Cbw::Cbw20)),
    ], vec![fake_bss_description!(Open, rssi_dbm: -36, channel: Channel::new(50, Cbw::Cbw20))];
                "when strong signal is last")]
    #[test_case(vec![
        fake_fidl_bss_description!(Open, rssi_dbm: -84, ssid: Ssid::try_from("bar").unwrap(),
                                   channel: Channel::new(149, Cbw::Cbw20)),
        fake_fidl_bss_description!(Open, rssi_dbm: -36, ssid: Ssid::try_from("bar").unwrap(),
                                   channel: Channel::new(165, Cbw::Cbw20)),
        fake_fidl_bss_description!(Open, rssi_dbm: -40, ssid: Ssid::try_from("baz").unwrap(),
                                   channel: Channel::new(165, Cbw::Cbw20)),
    ], vec![fake_bss_description!(Open, rssi_dbm: -40, ssid: Ssid::try_from("baz").unwrap(),
                                  channel: Channel::new(165, Cbw::Cbw20))];
                "overwrite latest chosen channel")]
    fn deduplicate_by_bssid(
        bss_description_list_from_mlme: Vec<fidl_internal::BssDescription>,
        returned_bss_description_list: Vec<BssDescription>,
    ) {
        let mut sched = create_sched();
        let (_inspector, sme_inspect) = sme_inspect();
        let req = sched
            .enqueue_scan_to_discover(passive_discovery_scan(10))
            .expect("expected a ScanRequest");
        let txn_id = req.txn_id;
        for bss in bss_description_list_from_mlme {
            sched
                .on_mlme_scan_result(
                    fidl_mlme::ScanResult {
                        txn_id,
                        timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                        bss,
                    },
                    &sme_inspect,
                )
                .expect("expect scan result received");
        }
        let (scan_end, mlme_req) = assert_variant!(
            sched.on_mlme_scan_end(
                fidl_mlme::ScanEnd { txn_id, code: fidl_mlme::ScanResultCode::Success },
                &sme_inspect,
            ),
            Ok((scan_end, mlme_req)) => (scan_end, mlme_req)
        );
        assert!(mlme_req.is_none());
        let (tokens, bss_description_list) = assert_variant!(
            scan_end,
            ScanEnd {
                tokens,
                result_code: fidl_mlme::ScanResultCode::Success,
                bss_description_list
            } => (tokens, bss_description_list),
            "expected discovery scan to be completed successfully"
        );
        assert_eq!(vec![10], tokens);
        assert_eq!(bss_description_list, returned_bss_description_list);
    }

    #[test]
    fn discovery_scan_merge_ies() {
        let mut sched = create_sched();
        let (_inspector, sme_inspect) = sme_inspect();
        let req = sched
            .enqueue_scan_to_discover(passive_discovery_scan(10))
            .expect("expected a ScanRequest");
        let txn_id = req.txn_id;

        let mut bss = fake_fidl_bss_description!(Open, ssid: Ssid::try_from("ssid").unwrap());
        // Add an extra IE so we can distinguish this result.
        let ie_marker1 = &[0xdd, 0x07, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee];
        bss.ies.extend_from_slice(ie_marker1);
        sched
            .on_mlme_scan_result(
                fidl_mlme::ScanResult {
                    txn_id,
                    timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                    bss,
                },
                &sme_inspect,
            )
            .expect("expect scan result received");

        let mut bss = fake_fidl_bss_description!(Open, ssid: Ssid::try_from("ssid").unwrap());
        // Add an extra IE so we can distinguish this result.
        let ie_marker2 = &[0xdd, 0x07, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff];
        bss.ies.extend_from_slice(ie_marker2);
        sched
            .on_mlme_scan_result(
                fidl_mlme::ScanResult {
                    txn_id,
                    timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                    bss,
                },
                &sme_inspect,
            )
            .expect("expect scan result received");
        let (scan_end, mlme_req) = assert_variant!(
            sched.on_mlme_scan_end(
                fidl_mlme::ScanEnd { txn_id, code: fidl_mlme::ScanResultCode::Success },
                &sme_inspect,
            ),
            Ok((scan_end, mlme_req)) => (scan_end, mlme_req)
        );
        assert!(mlme_req.is_none());
        let (tokens, bss_description_list) = assert_variant!(
            scan_end,
            ScanEnd {
                tokens,
                result_code: fidl_mlme::ScanResultCode::Success,
                bss_description_list
            } => (tokens, bss_description_list),
            "expected discovery scan to be completed successfully"
        );
        assert_eq!(vec![10], tokens);

        assert_eq!(bss_description_list.len(), 1);
        // Verify that both IEs are processed.
        assert!(slice_contains(bss_description_list[0].ies(), ie_marker1));
        assert!(slice_contains(bss_description_list[0].ies(), ie_marker2));
    }

    fn slice_contains(slice: &[u8], subslice: &[u8]) -> bool {
        // https://github.com/rust-lang/regex/issues/451#issuecomment-367987989
        let re = {
            let mut re_string = String::with_capacity(6 + subslice.len() * 4);
            re_string += "(?-u:";
            for b in subslice {
                write!(re_string, "\\x{:02X}", b).unwrap();
            }
            re_string += ")";
            Regex::new(&re_string).unwrap()
        };
        re.is_match(slice)
    }

    #[test_case(&[1, 2, 3], &[] => true; "vacuous")]
    #[test_case(&[1, 2, 3], &[1u8] => true; "one byte")]
    #[test_case(&[1, 2, 3], &[2u8, 3] => true; "multiple bytes")]
    #[test_case(&[1, 1, 1], &[1u8, 1] => true; "multiple matches")]
    #[test_case(&[1, 2, 3], &[0u8] => false; "no match")]
    #[test_case(&[1, 2, 3], &[1u8, 2, 3, 4] => false; "too large")]
    #[test_case(&[0x87, 0x77, 0x78], &[0x77, 0x77] => false; "misaligned match")]
    fn slice_contains_test(slice: &[u8], subslice: &[u8]) -> bool {
        slice_contains(slice, subslice)
    }

    #[test]
    fn test_passive_discovery_scan_args() {
        let mut sched = create_sched();
        let req = sched
            .enqueue_scan_to_discover(passive_discovery_scan(10))
            .expect("expected a ScanRequest");
        assert_eq!(req.txn_id, 1);
        assert_eq!(req.scan_type, fidl_mlme::ScanTypes::Passive);
        assert_eq!(req.channel_list, Vec::<u8>::new());
        assert_eq!(req.ssid_list, Vec::<Vec<u8>>::new());
        assert_eq!(req.probe_delay, 0);
        assert_eq!(req.min_channel_time, 200);
        assert_eq!(req.max_channel_time, 200);
    }

    #[test]
    fn test_active_discovery_scan_args_empty() {
        let device_info = device_info_with_channel(vec![1, 36, 165]);
        let mut sched: ScanScheduler<i32> =
            ScanScheduler::new(Arc::new(device_info), fake_spectrum_management_support_empty());
        let scan_cmd = DiscoveryScan::new(
            10,
            fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
                ssids: vec![],
                channels: vec![],
            }),
        );
        let req = sched.enqueue_scan_to_discover(scan_cmd).expect("expected a ScanRequest");

        assert_eq!(req.txn_id, 1);
        assert_eq!(req.scan_type, fidl_mlme::ScanTypes::Active);
        assert_eq!(req.channel_list, vec![36, 165, 1]);
        assert_eq!(req.ssid_list, Vec::<Vec<u8>>::new());
        assert_eq!(req.probe_delay, 5);
        assert_eq!(req.min_channel_time, 75);
        assert_eq!(req.max_channel_time, 75);
    }

    #[test]
    fn test_active_discovery_scan_args_filled() {
        let device_info = device_info_with_channel(vec![1, 36, 165]);
        let mut sched: ScanScheduler<i32> =
            ScanScheduler::new(Arc::new(device_info), fake_spectrum_management_support_empty());
        let ssid1: Vec<u8> = Ssid::try_from("ssid1").unwrap().into();
        let ssid2: Vec<u8> = Ssid::try_from("ssid2").unwrap().into();
        let scan_cmd = DiscoveryScan::new(
            10,
            fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
                ssids: vec![ssid1.clone(), ssid2.clone()],
                // TODO(fxbug.dev/88658): SME silently ignores unsupported channels
                channels: vec![1, 20, 100],
            }),
        );
        let req = sched.enqueue_scan_to_discover(scan_cmd).expect("expected a ScanRequest");

        assert_eq!(req.txn_id, 1);
        assert_eq!(req.scan_type, fidl_mlme::ScanTypes::Active);
        assert_eq!(req.channel_list, vec![1]);
        assert_eq!(req.ssid_list, vec![ssid1, ssid2]);
        assert_eq!(req.probe_delay, 5);
        assert_eq!(req.min_channel_time, 75);
        assert_eq!(req.max_channel_time, 75);
    }

    #[test]
    fn test_discovery_scans_dedupe_single_group() {
        let mut sched = create_sched();
        let (_inspector, sme_inspect) = sme_inspect();

        // Post one scan command, expect a message to MLME
        let mlme_req = sched
            .enqueue_scan_to_discover(passive_discovery_scan(10))
            .expect("expected a ScanRequest");
        let txn_id = mlme_req.txn_id;

        // Report a scan result
        sched
            .on_mlme_scan_result(
                fidl_mlme::ScanResult {
                    txn_id,
                    timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                    bss: fidl_internal::BssDescription {
                        bssid: [1; 6],
                        ..fake_fidl_bss_description!(Open, ssid: Ssid::try_from("foo").unwrap())
                    },
                },
                &sme_inspect,
            )
            .expect("expect scan result received");

        // Post another command. It should not issue another request to the MLME since
        // there is already an on-going one
        assert!(sched.enqueue_scan_to_discover(passive_discovery_scan(20)).is_none());

        // Report another scan result and the end of the scan transaction
        sched
            .on_mlme_scan_result(
                fidl_mlme::ScanResult {
                    txn_id,
                    timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                    bss: fidl_internal::BssDescription {
                        bssid: [2; 6],
                        ..fake_fidl_bss_description!(Open, ssid: Ssid::try_from("bar").unwrap())
                    },
                },
                &sme_inspect,
            )
            .expect("expect scan result received");
        let (scan_end, mlme_req) = assert_variant!(
            sched.on_mlme_scan_end(
                fidl_mlme::ScanEnd { txn_id, code: fidl_mlme::ScanResultCode::Success },
                &sme_inspect,
            ),
            Ok((scan_end, mlme_req)) => (scan_end, mlme_req)
        );

        // We don't expect another request to the MLME
        assert!(mlme_req.is_none());

        // Expect a discovery result with both tokens and both SSIDs
        assert_discovery_scan_result(
            scan_end,
            vec![10, 20],
            vec![Ssid::try_from("bar").unwrap(), Ssid::try_from("foo").unwrap()],
        );
    }

    #[test]
    fn test_discovery_scans_dedupe_multiple_groups() {
        let mut sched = create_sched();
        let (_inspector, sme_inspect) = sme_inspect();

        // Post a passive scan command, expect a message to MLME
        let mlme_req = sched
            .enqueue_scan_to_discover(passive_discovery_scan(10))
            .expect("expected a ScanRequest");
        let txn_id = mlme_req.txn_id;

        // Post an active scan command, which should be enqueued until the previous one finishes
        let scan_cmd = DiscoveryScan::new(
            20,
            fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
                ssids: vec![],
                channels: vec![],
            }),
        );
        assert!(sched.enqueue_scan_to_discover(scan_cmd).is_none());

        // Post a passive scan command. It should be merged with the ongoing one and so should not
        // issue another request to MLME
        assert!(sched.enqueue_scan_to_discover(passive_discovery_scan(30)).is_none());

        // Post an active scan command. It should be merged with the active scan command that's
        // still enqueued, and so should not issue another request to MLME
        let scan_cmd = DiscoveryScan::new(
            40,
            fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
                ssids: vec![],
                channels: vec![],
            }),
        );
        assert!(sched.enqueue_scan_to_discover(scan_cmd).is_none());

        // Report scan result and scan end
        sched
            .on_mlme_scan_result(
                fidl_mlme::ScanResult {
                    txn_id,
                    timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                    bss: fidl_internal::BssDescription {
                        bssid: [1; 6],
                        ..fake_fidl_bss_description!(Open, ssid: Ssid::try_from("foo").unwrap())
                    },
                },
                &sme_inspect,
            )
            .expect("expect scan result received");
        let (scan_end, mlme_req) = assert_variant!(
            sched.on_mlme_scan_end(
                fidl_mlme::ScanEnd { txn_id, code: fidl_mlme::ScanResultCode::Success },
                &sme_inspect,
            ),
            Ok((scan_end, mlme_req)) => (scan_end, mlme_req)
        );

        // Expect discovery result with 1st and 3rd tokens
        assert_discovery_scan_result(scan_end, vec![10, 30], vec![Ssid::try_from("foo").unwrap()]);

        // Next mlme_req should be an active scan request
        assert!(mlme_req.is_some());
        let mlme_req = mlme_req.unwrap();
        assert_eq!(mlme_req.scan_type, fidl_mlme::ScanTypes::Active);
        let txn_id = mlme_req.txn_id;

        // Report scan result and scan end
        sched
            .on_mlme_scan_result(
                fidl_mlme::ScanResult {
                    txn_id,
                    timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                    bss: fidl_internal::BssDescription {
                        bssid: [2; 6],
                        ..fake_fidl_bss_description!(Open, ssid: Ssid::try_from("bar").unwrap())
                    },
                },
                &sme_inspect,
            )
            .expect("expect scan result received");
        let (scan_end, mlme_req) = assert_variant!(
            sched.on_mlme_scan_end(
                fidl_mlme::ScanEnd { txn_id, code: fidl_mlme::ScanResultCode::Success },
                &sme_inspect,
            ),
            Ok((scan_end, mlme_req)) => (scan_end, mlme_req)
        );

        // Expect discovery result with 2nd and 4th tokens
        assert_discovery_scan_result(scan_end, vec![20, 40], vec![Ssid::try_from("bar").unwrap()]);

        // We don't expect another request to the MLME
        assert!(mlme_req.is_none());
    }

    #[test]
    fn test_discovery_scan_result_wrong_txn_id() {
        let mut sched = create_sched();
        let (_inspector, sme_inspect) = sme_inspect();

        // Post a passive scan command, expect a message to MLME
        let mlme_req = sched
            .enqueue_scan_to_discover(passive_discovery_scan(10))
            .expect("expected a ScanRequest");
        let txn_id = mlme_req.txn_id;

        // Report scan result with wrong txn id
        assert_variant!(
            sched.on_mlme_scan_result(
                fidl_mlme::ScanResult {
                    txn_id: txn_id + 1,
                    timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                    bss: fidl_internal::BssDescription {
                        bssid: [1; 6],
                        ..fake_fidl_bss_description!(Open, ssid: Ssid::try_from("foo").unwrap())
                    },
                },
                &sme_inspect,
            ),
            Err(Error::ScanResultWrongTxnId)
        );
    }

    #[test]
    fn test_discovery_scan_result_not_scanning() {
        let mut sched = create_sched();
        let (_inspector, sme_inspect) = sme_inspect();
        assert_variant!(
            sched.on_mlme_scan_result(
                fidl_mlme::ScanResult {
                    txn_id: 0,
                    timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                    bss: fidl_internal::BssDescription {
                        bssid: [1; 6],
                        ..fake_fidl_bss_description!(Open, ssid: Ssid::try_from("foo").unwrap())
                    },
                },
                &sme_inspect,
            ),
            Err(Error::ScanResultNotScanning)
        );
    }

    #[test]
    fn test_discovery_scan_end_wrong_txn_id() {
        let mut sched = create_sched();
        let (_inspector, sme_inspect) = sme_inspect();

        // Post a passive scan command, expect a message to MLME
        let mlme_req = sched
            .enqueue_scan_to_discover(passive_discovery_scan(10))
            .expect("expected a ScanRequest");
        let txn_id = mlme_req.txn_id;

        assert_variant!(
            sched.on_mlme_scan_end(
                fidl_mlme::ScanEnd { txn_id: txn_id + 1, code: fidl_mlme::ScanResultCode::Success },
                &sme_inspect,
            ),
            Err(Error::ScanEndWrongTxnId)
        );
    }

    #[test]
    fn test_discovery_scan_end_not_scanning() {
        let mut sched = create_sched();
        let (_inspector, sme_inspect) = sme_inspect();
        assert_variant!(
            sched.on_mlme_scan_end(
                fidl_mlme::ScanEnd { txn_id: 0, code: fidl_mlme::ScanResultCode::Success },
                &sme_inspect,
            ),
            Err(Error::ScanEndNotScanning)
        );
    }

    fn assert_discovery_scan_result(
        scan_end: ScanEnd<i32>,
        expected_tokens: Vec<i32>,
        expected_ssids: Vec<Ssid>,
    ) {
        let (tokens, bss_description_list) = assert_variant!(
            scan_end,
            ScanEnd {
                tokens,
                result_code: fidl_mlme::ScanResultCode::Success,
                bss_description_list
            } => (tokens, bss_description_list),
            "expected discovery scan to be completed successfully"
        );
        assert_eq!(tokens, expected_tokens);
        let mut ssid_list =
            bss_description_list.into_iter().map(|bss| bss.ssid.clone()).collect::<Vec<_>>();
        ssid_list.sort();
        assert_eq!(ssid_list, expected_ssids);
    }

    fn create_sched() -> ScanScheduler<i32> {
        ScanScheduler::new(
            Arc::new(test_utils::fake_device_info(CLIENT_ADDR)),
            fake_spectrum_management_support_empty(),
        )
    }

    fn device_info_with_channel(operating_channels: Vec<u8>) -> fidl_mlme::DeviceInfo {
        fidl_mlme::DeviceInfo {
            bands: vec![fidl_mlme::BandCapability {
                operating_channels,
                ..fake_5ghz_band_capability()
            }],
            ..test_utils::fake_device_info(CLIENT_ADDR)
        }
    }

    fn sme_inspect() -> (Inspector, Arc<inspect::SmeTree>) {
        let inspector = Inspector::default();
        let hasher = WlanHasher::new([88, 77, 66, 55, 44, 33, 22, 11]);
        let sme_inspect = Arc::new(inspect::SmeTree::new(inspector.root(), hasher));
        (inspector, sme_inspect)
    }
}
