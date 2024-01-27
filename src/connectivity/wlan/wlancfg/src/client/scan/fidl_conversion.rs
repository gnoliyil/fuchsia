// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::client::types,
    anyhow::{format_err, Error},
    fidl::prelude::*,
    fidl_fuchsia_wlan_policy as fidl_policy, fidl_fuchsia_wlan_sme as fidl_sme,
    fuchsia_zircon as zx,
    futures::stream::TryStreamExt,
    measure_tape_for_scan_result::Measurable as _,
    tracing::{debug, info},
};

// TODO(fxbug.dev/80422): Remove this.
// Size of FIDL message header and FIDL error-wrapped vector header
const FIDL_HEADER_AND_ERR_WRAPPED_VEC_HEADER_SIZE: usize = 56;

/// Convert the protection type we receive from the SME in scan results to the Policy layer
/// security type. This function should only be used when converting to results for the public
/// FIDL API, and not for internal use within Policy, where we should prefer the detailed SME
/// security types.
fn fidl_security_from_sme_protection(
    protection: fidl_sme::Protection,
    wpa3_supported: bool,
) -> Option<fidl_policy::SecurityType> {
    use fidl_policy::SecurityType;
    use fidl_sme::Protection::*;
    match protection {
        Wpa3Enterprise | Wpa3Personal | Wpa2Wpa3Personal => {
            Some(if wpa3_supported { SecurityType::Wpa3 } else { SecurityType::Wpa2 })
        }
        Wpa2Enterprise
        | Wpa2Personal
        | Wpa1Wpa2Personal
        | Wpa2PersonalTkipOnly
        | Wpa1Wpa2PersonalTkipOnly => Some(SecurityType::Wpa2),
        Wpa1 => Some(SecurityType::Wpa),
        Wep => Some(SecurityType::Wep),
        Open => Some(SecurityType::None),
        Unknown => None,
    }
}

pub fn scan_result_to_policy_scan_result(
    internal_results: &Vec<types::ScanResult>,
    wpa3_supported: bool,
) -> Vec<fidl_policy::ScanResult> {
    let scan_results: Vec<fidl_policy::ScanResult> = internal_results
        .iter()
        .filter_map(|internal| {
            if let Some(security) =
                fidl_security_from_sme_protection(internal.security_type_detailed, wpa3_supported)
            {
                Some(fidl_policy::ScanResult {
                    id: Some(fidl_policy::NetworkIdentifier {
                        ssid: internal.ssid.to_vec(),
                        type_: security,
                    }),
                    entries: Some(
                        internal
                            .entries
                            .iter()
                            .map(|input| {
                                // Get the frequency. On error, default to Some(0) rather than None
                                // to protect against consumer code that expects this field to
                                // always be set.
                                let frequency = input.channel.get_center_freq().unwrap_or(0);
                                fidl_policy::Bss {
                                    bssid: Some(input.bssid.0),
                                    rssi: Some(input.rssi),
                                    frequency: Some(frequency.into()), // u16.into() -> u32
                                    timestamp_nanos: Some(input.timestamp.into_nanos()),
                                    ..Default::default()
                                }
                            })
                            .collect(),
                    ),
                    compatibility: Some(internal.compatibility),
                    ..Default::default()
                })
            } else {
                debug!(
                    "Unknown security type present in scan results ({} BSSs)",
                    internal.entries.len()
                );
                None
            }
        })
        .collect();

    return scan_results;
}

/// Send batches of results to the output iterator when getNext() is called on it.
/// Send empty batch and close the channel when no results are remaining.
pub async fn send_scan_results_over_fidl(
    output_iterator: fidl::endpoints::ServerEnd<fidl_policy::ScanResultIteratorMarker>,
    scan_results: &Vec<fidl_policy::ScanResult>,
) -> Result<(), Error> {
    // Wait to get a request for a chunk of scan results
    let (mut stream, ctrl) = output_iterator.into_stream_and_control_handle()?;
    let max_batch_size = zx::sys::ZX_CHANNEL_MAX_MSG_BYTES as usize;
    let mut sent_some_results = false;
    let mut remaining_results = scan_results.iter().peekable();
    let mut batch_size: usize;

    // Verify consumer is expecting results before each batch
    loop {
        if let Some(fidl_policy::ScanResultIteratorRequest::GetNext { responder }) =
            stream.try_next().await?
        {
            sent_some_results = true;
            let mut batch = vec![];
            batch_size = FIDL_HEADER_AND_ERR_WRAPPED_VEC_HEADER_SIZE;
            // Peek if next element exists and will fit in current batch
            while let Some(peeked_result) = remaining_results.peek() {
                let result_size = peeked_result.measure().num_bytes;
                if result_size + FIDL_HEADER_AND_ERR_WRAPPED_VEC_HEADER_SIZE > max_batch_size {
                    return Err(format_err!("Single scan result too large to send via FIDL"));
                }
                // Peeked result will not fit. Send batch and continue.
                if result_size + batch_size > max_batch_size {
                    break;
                }
                // Actually remove result and push to batch
                if let Some(result) = remaining_results.next() {
                    batch.push(result.clone());
                    batch_size += result_size;
                }
            }
            let close_channel = batch.is_empty();
            responder.send(&mut Ok(batch))?;

            // Guarantees empty batch is sent before channel is closed.
            if close_channel {
                ctrl.shutdown();
                return Ok(());
            }
        } else {
            // This will happen if the iterator request stream was closed and we expected to send
            // another response.
            if sent_some_results {
                // Some consumers may not care about all scan results, e.g. if they find the
                // particular network they were looking for. This is not an error.
                debug!("Scan result consumer closed channel before consuming all scan results");
                return Ok(());
            } else {
                return Err(format_err!("Peer closed channel before receiving any scan results"));
            }
        }
    }
}

/// On the next request for results, send an error to the output iterator and
/// shut it down.
pub async fn send_scan_error_over_fidl(
    output_iterator: fidl::endpoints::ServerEnd<fidl_policy::ScanResultIteratorMarker>,
    error_code: types::ScanError,
) -> Result<(), fidl::Error> {
    // Wait to get a request for a chunk of scan results
    let (mut stream, ctrl) = output_iterator.into_stream_and_control_handle()?;
    if let Some(req) = stream.try_next().await? {
        let fidl_policy::ScanResultIteratorRequest::GetNext { responder } = req;
        let mut err: fidl_policy::ScanResultIteratorGetNextResult = Err(error_code);
        responder.send(&mut err)?;
        ctrl.shutdown();
    } else {
        // This will happen if the iterator request stream was closed and we expected to send
        // another response.
        info!("Peer closed channel for getting scan results unexpectedly");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fuchsia_async as fasync, fuchsia_zircon as zx,
        futures::task::Poll,
        pin_utils::pin_mut,
        wlan_common::assert_variant,
        wlan_common::{
            random_fidl_bss_description, scan::Compatibility, security::SecurityDescriptor,
        },
    };

    fn generate_test_fidl_data() -> Vec<fidl_policy::ScanResult> {
        const CENTER_FREQ_CHAN_1: u32 = 2412;
        const CENTER_FREQ_CHAN_8: u32 = 2447;
        const CENTER_FREQ_CHAN_11: u32 = 2462;
        vec![
            fidl_policy::ScanResult {
                id: Some(fidl_policy::NetworkIdentifier {
                    ssid: types::Ssid::try_from("duplicated ssid").unwrap().into(),
                    type_: fidl_policy::SecurityType::Wpa2,
                }),
                entries: Some(vec![
                    fidl_policy::Bss {
                        bssid: Some([0, 0, 0, 0, 0, 0]),
                        rssi: Some(0),
                        frequency: Some(CENTER_FREQ_CHAN_1),
                        timestamp_nanos: Some(zx::Time::get_monotonic().into_nanos()),
                        ..Default::default()
                    },
                    fidl_policy::Bss {
                        bssid: Some([7, 8, 9, 10, 11, 12]),
                        rssi: Some(13),
                        frequency: Some(CENTER_FREQ_CHAN_11),
                        timestamp_nanos: Some(zx::Time::get_monotonic().into_nanos()),
                        ..Default::default()
                    },
                ]),
                compatibility: Some(fidl_policy::Compatibility::Supported),
                ..Default::default()
            },
            fidl_policy::ScanResult {
                id: Some(fidl_policy::NetworkIdentifier {
                    ssid: types::Ssid::try_from("unique ssid").unwrap().into(),
                    type_: fidl_policy::SecurityType::Wpa2,
                }),
                entries: Some(vec![fidl_policy::Bss {
                    bssid: Some([1, 2, 3, 4, 5, 6]),
                    rssi: Some(7),
                    frequency: Some(CENTER_FREQ_CHAN_8),
                    timestamp_nanos: Some(zx::Time::get_monotonic().into_nanos()),
                    ..Default::default()
                }]),
                compatibility: Some(fidl_policy::Compatibility::Supported),
                ..Default::default()
            },
        ]
    }

    /// Generate a vector of FIDL scan results, each sized based on the input
    /// vector parameter. Size, in bytes, must be greater than the baseline scan
    /// result's size, measure below, and divisible into octets (by 8).
    fn create_fidl_scan_results_from_size(
        result_sizes: Vec<usize>,
    ) -> Vec<fidl_policy::ScanResult> {
        // Create a baseline result
        let minimal_scan_result = fidl_policy::ScanResult {
            id: Some(fidl_policy::NetworkIdentifier {
                ssid: types::Ssid::empty().into(),
                type_: fidl_policy::SecurityType::None,
            }),
            entries: Some(vec![]),
            ..Default::default()
        };
        let minimal_result_size: usize = minimal_scan_result.measure().num_bytes;

        // Create result with single entry
        let mut scan_result_with_one_bss = minimal_scan_result.clone();
        scan_result_with_one_bss.entries = Some(vec![fidl_policy::Bss::default()]);

        // Size of each additional BSS entry to FIDL ScanResult
        let empty_bss_entry_size: usize =
            scan_result_with_one_bss.measure().num_bytes - minimal_result_size;

        // Validate size is possible
        if result_sizes.iter().any(|size| size < &minimal_result_size || size % 8 != 0) {
            panic!("Invalid size. Requested size must be larger than {} minimum bytes and divisible into octets (by 8)", minimal_result_size);
        }

        let mut fidl_scan_results = vec![];
        for size in result_sizes {
            let mut scan_result = minimal_scan_result.clone();

            let num_bss_for_ap = (size - minimal_result_size) / empty_bss_entry_size;
            // Every 8 characters for SSID adds 8 bytes (1 octet).
            let ssid_length =
                (size - minimal_result_size) - (num_bss_for_ap * empty_bss_entry_size);

            scan_result.id = Some(fidl_policy::NetworkIdentifier {
                ssid: (0..ssid_length).map(|_| rand::random::<u8>()).collect(),
                type_: fidl_policy::SecurityType::None,
            });
            scan_result.entries = Some(vec![fidl_policy::Bss::default(); num_bss_for_ap]);

            // Validate result measures to expected size.
            assert_eq!(scan_result.measure().num_bytes, size);

            fidl_scan_results.push(scan_result);
        }
        fidl_scan_results
    }

    #[fuchsia::test]
    fn scan_result_generate_from_size() {
        let scan_results = create_fidl_scan_results_from_size(vec![112; 4]);
        assert_eq!(scan_results.len(), 4);
        assert!(scan_results.iter().all(|scan_result| scan_result.measure().num_bytes == 112));
    }

    #[fuchsia::test]
    fn sme_protection_converts_to_policy_security() {
        use {super::fidl_policy::SecurityType, super::fidl_sme::Protection};
        let wpa3_supported = true;
        let wpa3_not_supported = false;
        let test_pairs = vec![
            // Below are pairs when WPA3 is supported.
            (Protection::Wpa3Enterprise, wpa3_supported, Some(SecurityType::Wpa3)),
            (Protection::Wpa3Personal, wpa3_supported, Some(SecurityType::Wpa3)),
            (Protection::Wpa2Wpa3Personal, wpa3_supported, Some(SecurityType::Wpa3)),
            (Protection::Wpa2Enterprise, wpa3_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa2Personal, wpa3_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa1Wpa2Personal, wpa3_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa2PersonalTkipOnly, wpa3_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa1Wpa2PersonalTkipOnly, wpa3_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa1, wpa3_supported, Some(SecurityType::Wpa)),
            (Protection::Wep, wpa3_supported, Some(SecurityType::Wep)),
            (Protection::Open, wpa3_supported, Some(SecurityType::None)),
            (Protection::Unknown, wpa3_supported, None),
            // Below are pairs when WPA3 is not supported.
            (Protection::Wpa3Enterprise, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa3Personal, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa2Wpa3Personal, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa2Enterprise, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa2Personal, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa1Wpa2Personal, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa2PersonalTkipOnly, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa1Wpa2PersonalTkipOnly, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa1, wpa3_not_supported, Some(SecurityType::Wpa)),
            (Protection::Wep, wpa3_not_supported, Some(SecurityType::Wep)),
            (Protection::Open, wpa3_not_supported, Some(SecurityType::None)),
            (Protection::Unknown, wpa3_not_supported, None),
        ];
        for (input, wpa3_capable, output) in test_pairs {
            assert_eq!(fidl_security_from_sme_protection(input, wpa3_capable), output);
        }
    }

    #[fuchsia::test]
    fn scan_results_converted_correctly() {
        let fidl_aps = generate_test_fidl_data();
        let internal_aps = vec![
            types::ScanResult {
                ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                security_type_detailed: types::SecurityTypeDetailed::Wpa3Personal,
                entries: vec![
                    types::Bss {
                        bssid: types::Bssid([0, 0, 0, 0, 0, 0]),
                        rssi: 0,
                        timestamp: zx::Time::from_nanos(
                            fidl_aps[0].entries.as_ref().unwrap()[0].timestamp_nanos.unwrap(),
                        ),
                        snr_db: 1,
                        channel: types::WlanChan::new(1, types::Cbw::Cbw20),
                        observation: types::ScanObservation::Passive,
                        compatibility: Compatibility::expect_some([
                            SecurityDescriptor::WPA3_PERSONAL,
                        ]),
                        bss_description: random_fidl_bss_description!(
                            Wpa3,
                            bssid: [0, 0, 0, 0, 0, 0],
                            ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                            rssi_dbm: 0,
                            snr_db: 1,
                            channel: types::WlanChan::new(1, types::Cbw::Cbw20),
                        )
                        .into(),
                    },
                    types::Bss {
                        bssid: types::Bssid([7, 8, 9, 10, 11, 12]),
                        rssi: 13,
                        timestamp: zx::Time::from_nanos(
                            fidl_aps[0].entries.as_ref().unwrap()[1].timestamp_nanos.unwrap(),
                        ),
                        snr_db: 3,
                        channel: types::WlanChan::new(11, types::Cbw::Cbw20),
                        observation: types::ScanObservation::Passive,
                        compatibility: None,
                        bss_description: random_fidl_bss_description!(
                            Wpa3,
                            bssid: [7, 8, 9, 10, 11, 12],
                            ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                            rssi_dbm: 13,
                            snr_db: 3,
                            channel: types::WlanChan::new(11, types::Cbw::Cbw20),
                        )
                        .into(),
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
                    timestamp: zx::Time::from_nanos(
                        fidl_aps[1].entries.as_ref().unwrap()[0].timestamp_nanos.unwrap(),
                    ),
                    snr_db: 2,
                    channel: types::WlanChan::new(8, types::Cbw::Cbw20),
                    observation: types::ScanObservation::Passive,
                    compatibility: Compatibility::expect_some([SecurityDescriptor::WPA2_PERSONAL]),
                    bss_description: random_fidl_bss_description!(
                        Wpa2,
                        bssid: [1, 2, 3, 4, 5, 6],
                        ssid: types::Ssid::try_from("unique ssid").unwrap(),
                        rssi_dbm: 7,
                        snr_db: 2,
                        channel: types::WlanChan::new(8, types::Cbw::Cbw20),
                    )
                    .into(),
                }],
                compatibility: types::Compatibility::Supported,
            },
        ];
        assert_eq!(fidl_aps, scan_result_to_policy_scan_result(&internal_aps, false));
    }

    // TODO(fxbug.dev/54255): Separate test case for "empty final vector not consumed" vs "partial ap list"
    // consumed.
    #[fuchsia::test]
    fn partial_scan_result_consumption_has_no_error() {
        let mut exec = fasync::TestExecutor::new();
        let scan_results = generate_test_fidl_data();

        // Create an iterator and send scan results
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let send_fut = send_scan_results_over_fidl(iter_server, &scan_results);
        pin_mut!(send_fut);

        // Request a chunk of scan results.
        let mut output_iter_fut = iter.get_next();

        // Send first chunk of scan results
        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Pending);

        // Make sure the first chunk of results were delivered
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results, scan_results);
        });

        // Close the channel without getting remaining results
        // Note: as of the writing of this test, the "remaining results" are just the final message
        // with an empty vector of networks that signify the end of results. That final empty vector
        // is still considered part of the results, so this test successfully exercises the
        // "partial results read" path.
        drop(output_iter_fut);
        drop(iter);

        // This should not result in error, since some results were consumed
        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Ready(Ok(())));
    }

    #[fuchsia::test]
    fn no_scan_result_consumption_has_error() {
        let mut exec = fasync::TestExecutor::new();
        let scan_results = generate_test_fidl_data();

        // Create an iterator and send scan results
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let send_fut = send_scan_results_over_fidl(iter_server, &scan_results);
        pin_mut!(send_fut);

        // Close the channel without getting results
        drop(iter);

        // This should result in error, since no results were consumed
        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Ready(Err(_)));
    }

    #[fuchsia::test]
    fn scan_result_sends_max_message_size() {
        let mut exec = fasync::TestExecutor::new();
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");

        // Create a single scan result at the max allowed size to send in single
        // FIDL message.
        let fidl_scan_results = create_fidl_scan_results_from_size(vec![
            zx::sys::ZX_CHANNEL_MAX_MSG_BYTES as usize
                - FIDL_HEADER_AND_ERR_WRAPPED_VEC_HEADER_SIZE,
        ]);

        let send_fut = send_scan_results_over_fidl(iter_server, &fidl_scan_results);
        pin_mut!(send_fut);

        let mut output_iter_fut = iter.get_next();

        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Pending);

        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results, fidl_scan_results);
        })
    }

    #[fuchsia::test]
    fn scan_result_exceeding_max_size_throws_error() {
        let mut exec = fasync::TestExecutor::new();
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");

        // Create a single scan result exceeding the  max allowed size to send in single
        // FIDL message.
        let fidl_scan_results = create_fidl_scan_results_from_size(vec![
            (zx::sys::ZX_CHANNEL_MAX_MSG_BYTES as usize
                - FIDL_HEADER_AND_ERR_WRAPPED_VEC_HEADER_SIZE)
                + 8,
        ]);

        let send_fut = send_scan_results_over_fidl(iter_server, &fidl_scan_results);
        pin_mut!(send_fut);

        let mut output_iter_fut = iter.get_next();

        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Ready(Err(_)));

        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(Err(_)));
    }

    #[fuchsia::test]
    fn scan_result_sends_single_batch() {
        let mut exec = fasync::TestExecutor::new();
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");

        // Create a set of scan results that does not exceed the the max message
        // size, so it should be sent in a single batch.
        let fidl_scan_results =
            create_fidl_scan_results_from_size(vec![
                zx::sys::ZX_CHANNEL_MAX_MSG_BYTES as usize / 4;
                3
            ]);

        let send_fut = send_scan_results_over_fidl(iter_server, &fidl_scan_results);
        pin_mut!(send_fut);

        let mut output_iter_fut = iter.get_next();

        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Pending);

        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results, fidl_scan_results);
        });
    }

    #[fuchsia::test]
    fn scan_result_sends_multiple_batches() {
        let mut exec = fasync::TestExecutor::new();
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");

        // Create a set of scan results that exceed the max FIDL message size, so
        // they should be split into batches.
        let fidl_scan_results =
            create_fidl_scan_results_from_size(vec![
                zx::sys::ZX_CHANNEL_MAX_MSG_BYTES as usize / 8;
                8
            ]);

        let send_fut = send_scan_results_over_fidl(iter_server, &fidl_scan_results);
        pin_mut!(send_fut);

        let mut output_iter_fut = iter.get_next();

        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Pending);

        let mut aggregate_results = vec![];
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results.len(), 7);
            aggregate_results.extend(results);
        });

        let mut output_iter_fut = iter.get_next();
        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Pending);
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results.len(), 1);
            aggregate_results.extend(results);
        });
        assert_eq!(aggregate_results, fidl_scan_results);
    }
}
