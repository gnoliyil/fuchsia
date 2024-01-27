// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::client::{rsn::Supplicant, EstablishRsnaFailureReason, ServingApInfo},
    fidl_fuchsia_wlan_ieee80211 as fidl_ieee80211, fidl_fuchsia_wlan_internal as fidl_internal,
    fidl_fuchsia_wlan_mlme as fidl_mlme, fuchsia_zircon as zx,
    futures::channel::mpsc,
    ieee80211::{Bssid, Ssid},
    lazy_static::lazy_static,
    std::{
        convert::TryFrom,
        sync::{
            atomic::{AtomicBool, Ordering},
            Arc, Mutex,
        },
    },
    wlan_common::{
        assert_variant,
        bss::Protection,
        channel,
        ie::fake_ies::{fake_ht_cap_bytes, fake_vht_cap_bytes},
    },
    wlan_rsn::{auth, format_rsn_err, psk, rsna::UpdateSink, Error},
};

pub fn fake_serving_ap_info() -> ServingApInfo {
    ServingApInfo {
        bssid: Bssid([55, 11, 22, 3, 9, 70]),
        ssid: Ssid::try_from("foo").unwrap(),
        rssi_dbm: 0,
        snr_db: 0,
        signal_report_time: zx::Time::ZERO,
        channel: channel::Channel { primary: 1, cbw: channel::Cbw::Cbw20 },
        protection: Protection::Wpa2Personal,
        ht_cap: Some(fidl_ieee80211::HtCapabilities { bytes: fake_ht_cap_bytes() }),
        vht_cap: Some(fidl_ieee80211::VhtCapabilities { bytes: fake_vht_cap_bytes() }),
        probe_resp_wsc: None,
        wmm_param: None,
    }
}

pub fn fake_scan_request() -> fidl_mlme::ScanRequest {
    fidl_mlme::ScanRequest {
        txn_id: 1,
        scan_type: fidl_mlme::ScanTypes::Active,
        channel_list: vec![11],
        ssid_list: vec![],
        probe_delay: 5,
        min_channel_time: 50,
        max_channel_time: 50,
    }
}

pub fn fake_wmm_param() -> fidl_mlme::WmmParameter {
    #[rustfmt::skip]
    let wmm_param = fidl_mlme::WmmParameter {
        bytes: [
            0x80, // Qos Info - U-ASPD enabled
            0x00, // reserved
            0x03, 0xa4, 0x00, 0x00, // Best effort AC params
            0x27, 0xa4, 0x00, 0x00, // Background AC params
            0x42, 0x43, 0x5e, 0x00, // Video AC params
            0x62, 0x32, 0x2f, 0x00, // Voice AC params
        ]
    };
    wmm_param
}

pub fn create_connect_conf(
    bssid: Bssid,
    result_code: fidl_fuchsia_wlan_ieee80211::StatusCode,
) -> fidl_mlme::MlmeEvent {
    fidl_mlme::MlmeEvent::ConnectConf {
        resp: fidl_mlme::ConnectConfirm {
            peer_sta_address: bssid.0,
            result_code,
            association_id: 42,
            association_ies: vec![],
        },
    }
}

pub fn create_on_wmm_status_resp(status: zx::zx_status_t) -> fidl_mlme::MlmeEvent {
    fidl_mlme::MlmeEvent::OnWmmStatusResp { status, resp: fake_wmm_status_resp() }
}

pub fn fake_wmm_status_resp() -> fidl_internal::WmmStatusResponse {
    fidl_internal::WmmStatusResponse {
        apsd: true,
        ac_be_params: fidl_internal::WmmAcParams {
            aifsn: 1,
            acm: false,
            ecw_min: 2,
            ecw_max: 3,
            txop_limit: 4,
        },
        ac_bk_params: fidl_internal::WmmAcParams {
            aifsn: 5,
            acm: false,
            ecw_min: 6,
            ecw_max: 7,
            txop_limit: 8,
        },
        ac_vi_params: fidl_internal::WmmAcParams {
            aifsn: 9,
            acm: true,
            ecw_min: 10,
            ecw_max: 11,
            txop_limit: 12,
        },
        ac_vo_params: fidl_internal::WmmAcParams {
            aifsn: 13,
            acm: true,
            ecw_min: 14,
            ecw_max: 15,
            txop_limit: 16,
        },
    }
}

pub fn expect_stream_empty<T>(stream: &mut mpsc::UnboundedReceiver<T>, error_msg: &str) {
    assert_variant!(
        stream.try_next(),
        Ok(None) | Err(..),
        "error, receiver not empty: {}",
        error_msg
    );
}

fn mock_supplicant(auth_cfg: auth::Config) -> (MockSupplicant, MockSupplicantController) {
    let started = Arc::new(AtomicBool::new(false));
    let start_failure = Arc::new(Mutex::new(None));
    let on_eapol_frame_sink = Arc::new(Mutex::new(Ok(UpdateSink::default())));
    let on_rsna_retransmission_timeout = Arc::new(Mutex::new(Ok(UpdateSink::default())));
    let on_rsna_response_timeout = Arc::new(Mutex::new(None));
    let on_rsna_completion_timeout = Arc::new(Mutex::new(None));
    let on_sae_handshake_ind_sink = Arc::new(Mutex::new(Ok(UpdateSink::default())));
    let on_sae_frame_rx_sink = Arc::new(Mutex::new(Ok(UpdateSink::default())));
    let on_sae_timeout_sink = Arc::new(Mutex::new(Ok(UpdateSink::default())));
    let on_eapol_frame_cb = Arc::new(Mutex::new(None));
    let supplicant = MockSupplicant {
        started: started.clone(),
        start_failure: start_failure.clone(),
        on_eapol_frame: on_eapol_frame_sink.clone(),
        on_rsna_retransmission_timeout: on_rsna_retransmission_timeout.clone(),
        on_rsna_response_timeout: on_rsna_response_timeout.clone(),
        on_rsna_completion_timeout: on_rsna_completion_timeout.clone(),
        on_eapol_frame_cb: on_eapol_frame_cb.clone(),
        on_sae_handshake_ind: on_sae_handshake_ind_sink.clone(),
        on_sae_frame_rx: on_sae_frame_rx_sink.clone(),
        on_sae_timeout: on_sae_timeout_sink.clone(),
        auth_cfg,
    };
    let mock = MockSupplicantController {
        started,
        start_failure,
        mock_on_eapol_frame: on_eapol_frame_sink,
        mock_on_rsna_retransmission_timeout: on_rsna_retransmission_timeout,
        mock_on_rsna_response_timeout: on_rsna_response_timeout,
        mock_on_rsna_completion_timeout: on_rsna_completion_timeout,
        mock_on_sae_handshake_ind: on_sae_handshake_ind_sink,
        mock_on_sae_frame_rx: on_sae_frame_rx_sink,
        mock_on_sae_timeout: on_sae_timeout_sink,
        on_eapol_frame_cb,
    };
    (supplicant, mock)
}

const MOCK_PASS: &str = "dummy_password";
lazy_static! {
    static ref MOCK_SSID: Ssid = Ssid::try_from("network_ssid").unwrap();
}

pub fn mock_psk_supplicant() -> (MockSupplicant, MockSupplicantController) {
    let config = auth::Config::ComputedPsk(
        psk::compute(MOCK_PASS.as_bytes(), &MOCK_SSID).expect("Failed to create mock psk"),
    );
    mock_supplicant(config)
}

pub fn mock_sae_supplicant() -> (MockSupplicant, MockSupplicantController) {
    let config = auth::Config::Sae {
        ssid: MOCK_SSID.clone(),
        password: MOCK_PASS.as_bytes().to_vec(),
        mac: [0xaa; 6],
        peer_mac: [0xbb; 6],
    };
    mock_supplicant(config)
}

type Cb = dyn Fn() + Send + 'static;

pub struct MockSupplicant {
    started: Arc<AtomicBool>,
    start_failure: Arc<Mutex<Option<anyhow::Error>>>,
    on_eapol_frame: Arc<Mutex<Result<UpdateSink, anyhow::Error>>>,
    on_eapol_frame_cb: Arc<Mutex<Option<Box<Cb>>>>,
    on_rsna_retransmission_timeout: Arc<Mutex<Result<UpdateSink, anyhow::Error>>>,
    on_rsna_response_timeout: Arc<Mutex<Option<EstablishRsnaFailureReason>>>,
    on_rsna_completion_timeout: Arc<Mutex<Option<EstablishRsnaFailureReason>>>,
    on_sae_handshake_ind: Arc<Mutex<Result<UpdateSink, anyhow::Error>>>,
    on_sae_frame_rx: Arc<Mutex<Result<UpdateSink, anyhow::Error>>>,
    on_sae_timeout: Arc<Mutex<Result<UpdateSink, anyhow::Error>>>,
    auth_cfg: auth::Config,
}

fn populate_update_sink(
    update_sink: &mut UpdateSink,
    results: &Arc<Mutex<Result<UpdateSink, anyhow::Error>>>,
) -> Result<(), Error> {
    results
        .lock()
        .unwrap()
        .as_mut()
        .map(|updates| {
            update_sink.extend(updates.drain(..));
        })
        .map_err(|e| format_rsn_err!("{:?}", e))
}

impl Supplicant for MockSupplicant {
    fn start(&mut self) -> Result<(), Error> {
        match &*self.start_failure.lock().unwrap() {
            Some(error) => return Err(format_rsn_err!("{:?}", error)),
            None => {
                self.started.store(true, Ordering::SeqCst);
                Ok(())
            }
        }
    }

    fn reset(&mut self) {
        let _ = self.on_eapol_frame.lock().unwrap().as_mut().map(|updates| updates.clear());
    }

    fn on_eapol_frame(
        &mut self,
        update_sink: &mut UpdateSink,
        _frame: eapol::Frame<&[u8]>,
    ) -> Result<(), Error> {
        if let Some(cb) = self.on_eapol_frame_cb.lock().unwrap().as_mut() {
            cb();
        }
        populate_update_sink(update_sink, &self.on_eapol_frame)
    }

    fn on_eapol_conf(
        &mut self,
        _update_sink: &mut UpdateSink,
        _result: fidl_mlme::EapolResultCode,
    ) -> Result<(), Error> {
        // TODO(fxbug.dev/68454): Implement once this does something for a real supplicant.
        Ok(())
    }

    fn on_rsna_retransmission_timeout(
        &mut self,
        update_sink: &mut UpdateSink,
    ) -> Result<(), Error> {
        populate_update_sink(update_sink, &self.on_rsna_retransmission_timeout)
    }

    fn on_rsna_response_timeout(&self) -> EstablishRsnaFailureReason {
        self.on_rsna_response_timeout.lock().unwrap().take().expect("No establish RSNA reason")
    }

    fn on_rsna_completion_timeout(&self) -> EstablishRsnaFailureReason {
        self.on_rsna_completion_timeout.lock().unwrap().take().expect("No establish RSNA reason")
    }

    fn on_pmk_available(
        &mut self,
        _update_sink: &mut UpdateSink,
        _pmk: &[u8],
        _pmkid: &[u8],
    ) -> Result<(), Error> {
        unimplemented!()
    }

    fn on_sae_handshake_ind(&mut self, update_sink: &mut UpdateSink) -> Result<(), Error> {
        populate_update_sink(update_sink, &self.on_sae_handshake_ind)
    }
    fn on_sae_frame_rx(
        &mut self,
        update_sink: &mut UpdateSink,
        _frame: fidl_mlme::SaeFrame,
    ) -> Result<(), Error> {
        populate_update_sink(update_sink, &self.on_sae_frame_rx)
    }
    fn on_sae_timeout(
        &mut self,
        update_sink: &mut UpdateSink,
        _event_id: u64,
    ) -> Result<(), Error> {
        populate_update_sink(update_sink, &self.on_sae_timeout)
    }
    fn get_auth_cfg(&self) -> &auth::Config {
        &self.auth_cfg
    }
    fn get_auth_method(&self) -> auth::MethodName {
        self.auth_cfg.method_name()
    }
}

impl std::fmt::Debug for MockSupplicant {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "MockSupplicant cannot be formatted")
    }
}

pub struct MockSupplicantController {
    started: Arc<AtomicBool>,
    start_failure: Arc<Mutex<Option<anyhow::Error>>>,
    mock_on_eapol_frame: Arc<Mutex<Result<UpdateSink, anyhow::Error>>>,
    mock_on_rsna_retransmission_timeout: Arc<Mutex<Result<UpdateSink, anyhow::Error>>>,
    mock_on_rsna_response_timeout: Arc<Mutex<Option<EstablishRsnaFailureReason>>>,
    mock_on_rsna_completion_timeout: Arc<Mutex<Option<EstablishRsnaFailureReason>>>,
    mock_on_sae_handshake_ind: Arc<Mutex<Result<UpdateSink, anyhow::Error>>>,
    mock_on_sae_frame_rx: Arc<Mutex<Result<UpdateSink, anyhow::Error>>>,
    mock_on_sae_timeout: Arc<Mutex<Result<UpdateSink, anyhow::Error>>>,
    on_eapol_frame_cb: Arc<Mutex<Option<Box<Cb>>>>,
}

impl MockSupplicantController {
    pub fn set_start_failure(&self, error: anyhow::Error) {
        *self.start_failure.lock().unwrap() = Some(error);
    }

    pub fn is_supplicant_started(&self) -> bool {
        self.started.load(Ordering::SeqCst)
    }

    pub fn set_on_eapol_frame_updates(&self, updates: UpdateSink) {
        *self.mock_on_eapol_frame.lock().unwrap() = Ok(updates);
    }

    pub fn set_on_rsna_retransmission_timeout_updates(&self, updates: UpdateSink) {
        *self.mock_on_rsna_retransmission_timeout.lock().unwrap() = Ok(updates);
    }

    pub fn set_on_rsna_retransmission_timeout_failure(&self, error: anyhow::Error) {
        *self.mock_on_rsna_retransmission_timeout.lock().unwrap() = Err(error);
    }

    pub fn set_on_rsna_response_timeout(&self, error: EstablishRsnaFailureReason) {
        *self.mock_on_rsna_response_timeout.lock().unwrap() = Some(error);
    }

    pub fn set_on_rsna_completion_timeout(&self, error: EstablishRsnaFailureReason) {
        *self.mock_on_rsna_completion_timeout.lock().unwrap() = Some(error);
    }

    pub fn set_on_sae_handshake_ind_updates(&self, updates: UpdateSink) {
        *self.mock_on_sae_handshake_ind.lock().unwrap() = Ok(updates);
    }

    pub fn set_on_sae_frame_rx_updates(&self, updates: UpdateSink) {
        *self.mock_on_sae_frame_rx.lock().unwrap() = Ok(updates);
    }

    pub fn set_on_sae_timeout_updates(&self, updates: UpdateSink) {
        *self.mock_on_sae_timeout.lock().unwrap() = Ok(updates);
    }

    pub fn set_on_sae_timeout_failure(&self, error: anyhow::Error) {
        *self.mock_on_sae_timeout.lock().unwrap() = Err(error);
    }

    pub fn set_on_eapol_frame_callback<F>(&self, cb: F)
    where
        F: Fn() + Send + 'static,
    {
        *self.on_eapol_frame_cb.lock().unwrap() = Some(Box::new(cb));
    }

    pub fn set_on_eapol_frame_failure(&self, error: anyhow::Error) {
        *self.mock_on_eapol_frame.lock().unwrap() = Err(error);
    }
}
