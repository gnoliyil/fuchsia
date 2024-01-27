// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Context as _, Error};
use fidl::endpoints;
use fidl_fuchsia_wlan_common as fidl_common;
use fidl_fuchsia_wlan_common::WlanMacRole;
use fidl_fuchsia_wlan_device_service::DeviceMonitorProxy;
use fidl_fuchsia_wlan_sme as fidl_sme;
use ieee80211::Ssid;

type WlanService = DeviceMonitorProxy;

pub async fn get_sme_proxy(
    wlan_svc: &WlanService,
    iface_id: u16,
) -> Result<fidl_sme::ApSmeProxy, Error> {
    let (sme_proxy, sme_remote) = endpoints::create_proxy()?;
    let result = wlan_svc
        .get_ap_sme(iface_id, sme_remote)
        .await
        .context("error sending GetApSme request")?;
    match result {
        Ok(()) => Ok(sme_proxy),
        Err(e) => Err(format_err!(
            "Failed to get AP sme proxy for interface id {} with error {}",
            iface_id,
            e
        )),
    }
}

pub async fn get_first_sme(wlan_svc: &WlanService) -> Result<fidl_sme::ApSmeProxy, Error> {
    let iface_id =
        super::get_first_iface(wlan_svc, WlanMacRole::Ap).await.context("failed to get iface")?;
    get_sme_proxy(&wlan_svc, iface_id).await
}

pub async fn stop(
    iface_sme_proxy: &fidl_sme::ApSmeProxy,
) -> Result<fidl_sme::StopApResultCode, Error> {
    let stop_ap_result_code = iface_sme_proxy.stop().await;

    match stop_ap_result_code {
        Ok(result_code) => Ok(result_code),
        _ => Err(format_err!("AP stop failure: {:?}", stop_ap_result_code)),
    }
}

pub async fn start(
    iface_sme_proxy: &fidl_sme::ApSmeProxy,
    target_ssid: Ssid,
    target_pwd: Vec<u8>,
    channel: u8,
) -> Result<fidl_sme::StartApResultCode, Error> {
    let config = fidl_sme::ApConfig {
        ssid: target_ssid.into(),
        password: target_pwd,
        radio_cfg: fidl_sme::RadioConfig {
            phy: fidl_common::WlanPhyType::Ht,
            channel: fidl_common::WlanChannel {
                primary: channel,
                cbw: fidl_common::ChannelBandwidth::Cbw20,
                secondary80: 0,
            },
        },
    };
    let start_ap_result_code = iface_sme_proxy.start(&config).await;

    match start_ap_result_code {
        Ok(result_code) => Ok(result_code),
        _ => Err(format_err!("AP start failure: {:?}", start_ap_result_code)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_wlan_device_service::{
        DeviceMonitorMarker, DeviceMonitorRequest, DeviceMonitorRequestStream,
    };
    use fidl_fuchsia_wlan_sme::ApSmeMarker;
    use fidl_fuchsia_wlan_sme::StartApResultCode;
    use fidl_fuchsia_wlan_sme::{ApSmeRequest, ApSmeRequestStream};
    use fuchsia_async as fasync;
    use fuchsia_zircon as zx;
    use futures::stream::{StreamExt, StreamFuture};
    use futures::task::Poll;
    use ieee80211::Ssid;
    use pin_utils::pin_mut;
    use std::convert::TryFrom;
    use wlan_common::assert_variant;

    #[test]
    fn start_ap_success_returns_true() {
        let start_ap_result = test_ap_start("TestAp", "", 6, StartApResultCode::Success);
        assert!(start_ap_result == StartApResultCode::Success);
    }

    #[test]
    fn start_ap_already_started_returns_false() {
        let start_ap_result = test_ap_start("TestAp", "", 6, StartApResultCode::AlreadyStarted);
        assert!(start_ap_result == StartApResultCode::AlreadyStarted);
    }

    #[test]
    fn start_ap_internal_error_returns_false() {
        let start_ap_result = test_ap_start("TestAp", "", 6, StartApResultCode::InternalError);
        assert!(start_ap_result == StartApResultCode::InternalError);
    }

    #[test]
    fn start_ap_canceled_returns_false() {
        let start_ap_result = test_ap_start("TestAp", "", 6, StartApResultCode::Canceled);
        assert!(start_ap_result == StartApResultCode::Canceled);
    }

    #[test]
    fn start_ap_timedout_returns_false() {
        let start_ap_result = test_ap_start("TestAp", "", 6, StartApResultCode::TimedOut);
        assert!(start_ap_result == StartApResultCode::TimedOut);
    }

    #[test]
    fn start_ap_in_progress_returns_false() {
        let start_ap_result =
            test_ap_start("TestAp", "", 6, StartApResultCode::PreviousStartInProgress);
        assert!(start_ap_result == StartApResultCode::PreviousStartInProgress);
    }

    fn test_ap_start(
        ssid: &str,
        password: &str,
        channel: u8,
        result_code: StartApResultCode,
    ) -> StartApResultCode {
        let mut exec = fasync::TestExecutor::new();
        let (ap_sme, server) = create_ap_sme_proxy();
        let mut ap_sme_req = server.into_future();
        let target_ssid = Ssid::try_from(ssid).unwrap();
        let target_password = password.as_bytes().to_vec();

        let config = fidl_sme::ApConfig {
            ssid: target_ssid.to_vec(),
            password: target_password.to_vec(),
            radio_cfg: fidl_sme::RadioConfig {
                phy: fidl_common::WlanPhyType::Ht,
                channel: fidl_common::WlanChannel {
                    primary: channel,
                    cbw: fidl_common::ChannelBandwidth::Cbw20,
                    secondary80: 0,
                },
            },
        };

        let fut = start(&ap_sme, target_ssid, target_password, channel);
        pin_mut!(fut);
        assert!(exec.run_until_stalled(&mut fut).is_pending());

        send_start_ap_response(&mut exec, &mut ap_sme_req, config, result_code);

        let complete = exec.run_until_stalled(&mut fut);

        let ap_start_result = match complete {
            Poll::Ready(result) => result,
            _ => panic!("Expected a start response"),
        };

        let returned_start_ap_code = match ap_start_result {
            Ok(response) => response,
            _ => panic!("Expected a valid start result"),
        };

        returned_start_ap_code
    }

    fn create_ap_sme_proxy() -> (fidl_sme::ApSmeProxy, ApSmeRequestStream) {
        let (proxy, server) =
            endpoints::create_proxy::<ApSmeMarker>().expect("failed to create sme ap channel");
        let server = server.into_stream().expect("failed to create ap sme response stream");
        (proxy, server)
    }

    fn send_start_ap_response(
        exec: &mut fasync::TestExecutor,
        server: &mut StreamFuture<ApSmeRequestStream>,
        expected_config: fidl_sme::ApConfig,
        result_code: StartApResultCode,
    ) {
        let rsp = match poll_ap_sme_request(exec, server) {
            Poll::Ready(ApSmeRequest::Start { config, responder }) => {
                assert_eq!(expected_config, config);
                responder
            }
            Poll::Pending => panic!("Expected AP Start Request"),
            _ => panic!("Expected AP Start Request"),
        };

        rsp.send(result_code).expect("Failed to send AP start response.");
    }

    fn poll_ap_sme_request(
        exec: &mut fasync::TestExecutor,
        next_ap_sme_req: &mut StreamFuture<ApSmeRequestStream>,
    ) -> Poll<ApSmeRequest> {
        exec.run_until_stalled(next_ap_sme_req).map(|(req, stream)| {
            *next_ap_sme_req = stream.into_future();
            req.expect("did not expect the ApSmeRequestStream to end")
                .expect("error polling ap sme request stream")
        })
    }

    fn respond_to_get_ap_sme_request(
        exec: &mut fasync::TestExecutor,
        req_stream: &mut DeviceMonitorRequestStream,
        result: Result<(), zx::Status>,
    ) {
        let req = exec.run_until_stalled(&mut req_stream.next());

        let responder = assert_variant !(
            req,
            Poll::Ready(Some(Ok(DeviceMonitorRequest::GetApSme{ responder, ..})))
            => responder);

        // now send the response back
        responder
            .send(result.map_err(|e| e.into_raw()))
            .expect("fake sme proxy response: send failed")
    }

    fn test_get_first_sme(iface_list: &[WlanMacRole]) -> Result<(), Error> {
        let (mut exec, proxy, mut req_stream) =
            crate::tests::setup_fake_service::<DeviceMonitorMarker>();
        let fut = get_first_sme(&proxy);
        pin_mut!(fut);

        let ifaces = (0..iface_list.len() as u16).collect();

        assert!(exec.run_until_stalled(&mut fut).is_pending());
        crate::tests::respond_to_query_iface_list_request(&mut exec, &mut req_stream, ifaces);

        for mac_role in iface_list {
            // iface query response
            assert!(exec.run_until_stalled(&mut fut).is_pending());
            crate::tests::respond_to_query_iface_request(
                &mut exec,
                &mut req_stream,
                *mac_role,
                Some([1, 2, 3, 4, 5, 6]),
            );

            if *mac_role == WlanMacRole::Ap {
                // ap sme proxy
                assert!(exec.run_until_stalled(&mut fut).is_pending());
                respond_to_get_ap_sme_request(&mut exec, &mut req_stream, Ok(()));
                break;
            }
        }

        let _proxy = exec.run_singlethreaded(&mut fut)?;
        Ok(())
    }
    // iface list contains an AP and a client. Test should pass
    #[test]
    fn check_get_ap_sme_success() {
        let iface_list: Vec<WlanMacRole> = vec![WlanMacRole::Client, WlanMacRole::Ap];
        test_get_first_sme(&iface_list).expect("expect success but failed");
    }

    // iface list is empty. Test should fail
    #[test]
    fn check_get_ap_sme_no_devices() {
        let iface_list: Vec<WlanMacRole> = Vec::new();
        test_get_first_sme(&iface_list).expect_err("expect fail but succeeded");
    }

    // iface list does not contain an ap. Test should fail
    #[test]
    fn check_get_ap_sme_no_aps() {
        let iface_list: Vec<WlanMacRole> = vec![WlanMacRole::Client, WlanMacRole::Client];
        test_get_first_sme(&iface_list).expect_err("expect fail but succeeded");
    }
}
