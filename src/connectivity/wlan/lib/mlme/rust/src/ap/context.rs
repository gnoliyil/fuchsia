// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        ap::TimedEvent,
        buffer::{BufferProvider, InBuf},
        device::Device,
        disconnect::LocallyInitiated,
        error::Error,
    },
    anyhow::format_err,
    fidl_fuchsia_wlan_ieee80211 as fidl_ieee80211, fidl_fuchsia_wlan_mlme as fidl_mlme,
    fuchsia_zircon as zx,
    ieee80211::{Bssid, MacAddr, Ssid},
    wlan_common::{
        big_endian::BigEndianU16,
        data_writer,
        ie::{self, rsn::rsne},
        mac::{self, Aid, AuthAlgorithmNumber, StatusCode},
        mgmt_writer,
        sequence::SequenceManager,
        timer::{EventId, Timer},
        wmm, TimeUnit,
    },
    wlan_frame_writer::{write_frame, write_frame_with_fixed_buf},
};

/// BeaconParams contains parameters that may be used to offload beaconing to the hardware.
pub struct BeaconOffloadParams {
    /// Offset from the start of the input buffer to the TIM element.
    pub tim_ele_offset: usize,
}

pub struct Context {
    pub device: Device,
    pub buf_provider: BufferProvider,
    pub timer: Timer<TimedEvent>,
    pub seq_mgr: SequenceManager,
    pub bssid: Bssid,
}

impl Context {
    pub fn new(
        device: Device,
        buf_provider: BufferProvider,
        timer: Timer<TimedEvent>,
        bssid: Bssid,
    ) -> Self {
        Self { device, timer, buf_provider, seq_mgr: SequenceManager::new(), bssid }
    }

    pub fn schedule_after(&mut self, duration: zx::Duration, event: TimedEvent) -> EventId {
        self.timer.schedule_after(duration, event)
    }

    // MLME sender functions.

    /// Sends MLME-START.confirm (IEEE Std 802.11-2016, 6.3.11.3) to the SME.
    pub fn send_mlme_start_conf(
        &self,
        result_code: fidl_mlme::StartResultCode,
    ) -> Result<(), Error> {
        self.device
            .send_mlme_event(fidl_mlme::MlmeEvent::StartConf {
                resp: fidl_mlme::StartConfirm { result_code },
            })
            .map_err(|e| e.into())
    }

    /// Sends MLME-STOP.confirm to the SME.
    pub fn send_mlme_stop_conf(&self, result_code: fidl_mlme::StopResultCode) -> Result<(), Error> {
        self.device
            .send_mlme_event(fidl_mlme::MlmeEvent::StopConf {
                resp: fidl_mlme::StopConfirm { result_code },
            })
            .map_err(|e| e.into())
    }

    /// Sends EAPOL.conf (fuchsia.wlan.mlme.EapolConfirm) to the SME.
    pub fn send_mlme_eapol_conf(
        &self,
        result_code: fidl_mlme::EapolResultCode,
        dst_addr: MacAddr,
    ) -> Result<(), Error> {
        self.device
            .send_mlme_event(fidl_mlme::MlmeEvent::EapolConf {
                resp: fidl_mlme::EapolConfirm { result_code, dst_addr },
            })
            .map_err(|e| e.into())
    }

    /// Sends MLME-AUTHENTICATE.indication (IEEE Std 802.11-2016, 6.3.5.4) to the SME.
    pub fn send_mlme_auth_ind(
        &self,
        peer_sta_address: MacAddr,
        auth_type: fidl_mlme::AuthenticationTypes,
    ) -> Result<(), Error> {
        self.device
            .send_mlme_event(fidl_mlme::MlmeEvent::AuthenticateInd {
                ind: fidl_mlme::AuthenticateIndication { peer_sta_address, auth_type },
            })
            .map_err(|e| e.into())
    }

    /// Sends MLME-DEAUTHENTICATE.indication (IEEE Std 802.11-2016, 6.3.6.4) to the SME.
    pub fn send_mlme_deauth_ind(
        &self,
        peer_sta_address: MacAddr,
        reason_code: fidl_ieee80211::ReasonCode,
        locally_initiated: LocallyInitiated,
    ) -> Result<(), Error> {
        self.device
            .send_mlme_event(fidl_mlme::MlmeEvent::DeauthenticateInd {
                ind: fidl_mlme::DeauthenticateIndication {
                    peer_sta_address,
                    reason_code,
                    locally_initiated: locally_initiated.0,
                },
            })
            .map_err(|e| e.into())
    }

    /// Sends MLME-ASSOCIATE.indication (IEEE Std 802.11-2016, 6.3.7.4) to the SME.
    pub fn send_mlme_assoc_ind(
        &self,
        peer_sta_address: MacAddr,
        listen_interval: u16,
        ssid: Option<Ssid>,
        capabilities: mac::CapabilityInfo,
        rates: Vec<ie::SupportedRate>,
        rsne: Option<Vec<u8>>,
    ) -> Result<(), Error> {
        self.device
            .send_mlme_event(fidl_mlme::MlmeEvent::AssociateInd {
                ind: fidl_mlme::AssociateIndication {
                    peer_sta_address,
                    listen_interval,
                    ssid: ssid.map(|s| s.into()),
                    capability_info: capabilities.raw(),
                    rates: rates.iter().map(|r| r.0).collect(),
                    rsne,
                    // TODO(fxbug.dev/37891): Send everything else (e.g. HT capabilities).
                },
            })
            .map_err(|e| e.into())
    }

    /// Sends MLME-DISASSOCIATE.indication (IEEE Std 802.11-2016, 6.3.9.3) to the SME.
    pub fn send_mlme_disassoc_ind(
        &self,
        peer_sta_address: MacAddr,
        reason_code: fidl_ieee80211::ReasonCode,
        locally_initiated: LocallyInitiated,
    ) -> Result<(), Error> {
        self.device
            .send_mlme_event(fidl_mlme::MlmeEvent::DisassociateInd {
                ind: fidl_mlme::DisassociateIndication {
                    peer_sta_address,
                    reason_code,
                    locally_initiated: locally_initiated.0,
                },
            })
            .map_err(|e| e.into())
    }

    /// Sends EAPOL.indication (fuchsia.wlan.mlme.EapolIndication) to the SME.
    pub fn send_mlme_eapol_ind(
        &self,
        dst_addr: MacAddr,
        src_addr: MacAddr,
        data: &[u8],
    ) -> Result<(), Error> {
        self.device
            .send_mlme_event(fidl_mlme::MlmeEvent::EapolInd {
                ind: fidl_mlme::EapolIndication { dst_addr, src_addr, data: data.to_vec() },
            })
            .map_err(|e| e.into())
    }

    // WLAN frame sender functions.

    /// Sends a WLAN authentication frame (IEEE Std 802.11-2016, 9.3.3.12) to the PHY.
    pub fn make_auth_frame(
        &mut self,
        addr: MacAddr,
        auth_alg_num: AuthAlgorithmNumber,
        auth_txn_seq_num: u16,
        status_code: StatusCode,
    ) -> Result<(InBuf, usize), Error> {
        write_frame!(&self.buf_provider, {
            headers: {
                mac::MgmtHdr: &mgmt_writer::mgmt_hdr_from_ap(
                    mac::FrameControl(0)
                        .with_frame_type(mac::FrameType::MGMT)
                        .with_mgmt_subtype(mac::MgmtSubtype::AUTH),
                    addr,
                    self.bssid,
                    mac::SequenceControl(0).with_seq_num(self.seq_mgr.next_sns1(&addr) as u16)
                ),
                mac::AuthHdr: &mac::AuthHdr { auth_alg_num, auth_txn_seq_num, status_code },
            }
        })
    }

    /// Sends a WLAN association response frame (IEEE Std 802.11-2016, 9.3.3.7) to the PHY.
    pub fn make_assoc_resp_frame(
        &mut self,
        addr: MacAddr,
        capabilities: mac::CapabilityInfo,
        aid: Aid,
        rates: &[u8],
        max_idle_period: Option<u16>,
    ) -> Result<(InBuf, usize), Error> {
        write_frame!(&self.buf_provider, {
            headers: {
                mac::MgmtHdr: &mgmt_writer::mgmt_hdr_from_ap(
                    mac::FrameControl(0)
                        .with_frame_type(mac::FrameType::MGMT)
                        .with_mgmt_subtype(mac::MgmtSubtype::ASSOC_RESP),
                    addr,
                    self.bssid,
                    mac::SequenceControl(0).with_seq_num(self.seq_mgr.next_sns1(&addr) as u16)
                ),
                mac::AssocRespHdr: &mac::AssocRespHdr {
                    capabilities,
                    status_code: fidl_ieee80211::StatusCode::Success.into(),
                    aid
                },
            },
            // Order of association response frame body IEs is according to IEEE Std 802.11-2016,
            // Table 9-30, numbered below.
            ies: {
                // 4: Supported Rates and BSS Membership Selectors
                supported_rates: &rates,
                // 5: Extended Supported Rates and BSS Membership Selectors
                extended_supported_rates: {/* continue rates */},
                // 19: BSS Max Idle Period
                bss_max_idle_period?: if let Some(max_idle_period) = max_idle_period {
                    ie::BssMaxIdlePeriod {
                        max_idle_period,
                        idle_options: ie::IdleOptions(0)
                            // TODO(fxbug.dev/37891): Support configuring this.
                            .with_protected_keep_alive_required(false),
                    }
                },
            }
        })
    }

    /// Sends a WLAN association response frame (IEEE Std 802.11-2016, 9.3.3.7) to the PHY, but only
    /// with the status code.
    pub fn make_assoc_resp_frame_error(
        &mut self,
        addr: MacAddr,
        capabilities: mac::CapabilityInfo,
        status_code: StatusCode,
    ) -> Result<(InBuf, usize), Error> {
        write_frame!(&self.buf_provider, {
            headers: {
                mac::MgmtHdr: &mgmt_writer::mgmt_hdr_from_ap(
                    mac::FrameControl(0)
                        .with_frame_type(mac::FrameType::MGMT)
                        .with_mgmt_subtype(mac::MgmtSubtype::ASSOC_RESP),
                    addr,
                    self.bssid,
                    mac::SequenceControl(0).with_seq_num(self.seq_mgr.next_sns1(&addr) as u16)
                ),
                mac::AssocRespHdr: &mac::AssocRespHdr {
                    capabilities,
                    status_code,
                    aid: 0,
                },
            },
        })
    }

    /// Sends a WLAN deauthentication frame (IEEE Std 802.11-2016, 9.3.3.1) to the PHY.
    pub fn make_deauth_frame(
        &mut self,
        addr: MacAddr,
        reason_code: mac::ReasonCode,
    ) -> Result<(InBuf, usize), Error> {
        write_frame!(&self.buf_provider, {
            headers: {
                mac::MgmtHdr: &mgmt_writer::mgmt_hdr_from_ap(
                    mac::FrameControl(0)
                        .with_frame_type(mac::FrameType::MGMT)
                        .with_mgmt_subtype(mac::MgmtSubtype::DEAUTH),
                    addr,
                    self.bssid,
                    mac::SequenceControl(0).with_seq_num(self.seq_mgr.next_sns1(&addr) as u16)
                ),
                mac::DeauthHdr: &mac::DeauthHdr { reason_code },
            },
        })
    }

    /// Sends a WLAN disassociation frame (IEEE Std 802.11-2016, 9.3.3.5) to the PHY.
    pub fn make_disassoc_frame(
        &mut self,
        addr: MacAddr,
        reason_code: mac::ReasonCode,
    ) -> Result<(InBuf, usize), Error> {
        write_frame!(&self.buf_provider, {
            headers: {
                mac::MgmtHdr: &mgmt_writer::mgmt_hdr_from_ap(
                    mac::FrameControl(0)
                        .with_frame_type(mac::FrameType::MGMT)
                        .with_mgmt_subtype(mac::MgmtSubtype::DISASSOC),
                    addr,
                    self.bssid,
                    mac::SequenceControl(0).with_seq_num(self.seq_mgr.next_sns1(&addr) as u16)
                ),
                mac::DisassocHdr: &mac::DisassocHdr { reason_code },
            },
        })
    }

    /// Sends a WLAN probe response frame (IEEE Std 802.11-2016, 9.3.3.11) to the PHY.
    // TODO(fxbug.dev/42088): Use this for devices that don't support probe request offload.
    pub fn make_probe_resp_frame(
        &mut self,
        addr: MacAddr,
        beacon_interval: TimeUnit,
        capabilities: mac::CapabilityInfo,
        ssid: &Ssid,
        rates: &[u8],
        channel: u8,
        rsne: &[u8],
    ) -> Result<(InBuf, usize), Error> {
        write_frame!(&self.buf_provider, {
            headers: {
                mac::MgmtHdr: &mgmt_writer::mgmt_hdr_from_ap(
                    mac::FrameControl(0)
                        .with_frame_type(mac::FrameType::MGMT)
                        .with_mgmt_subtype(mac::MgmtSubtype::PROBE_RESP),
                    addr,
                    self.bssid,
                    mac::SequenceControl(0).with_seq_num(self.seq_mgr.next_sns1(&addr) as u16)
                ),
                mac::ProbeRespHdr: &mac::ProbeRespHdr::new(beacon_interval, capabilities),
            },
            // Order of beacon frame body IEs is according to IEEE Std 802.11-2016, Table 9-27,
            // numbered below.
            ies: {
                // 4. Service Set Identifier (SSID)
                ssid: ssid,
                // 5. Supported Rates and BSS Membership Selectors
                supported_rates: rates,
                // 6. DSSS Parameter Set
                dsss_param_set: &ie::DsssParamSet { current_channel: channel },
                // 16. Extended Supported Rates and BSS Membership Selectors
                extended_supported_rates: {/* continue rates */},
                // 17. RSN
                rsne?: if !rsne.is_empty() {
                    rsne::from_bytes(rsne)
                        .map_err(|e| format_err!("error parsing rsne {:?} : {:?}", rsne, e))?
                        .1
                },

            }
        })
    }

    pub fn make_beacon_frame(
        &self,
        beacon_interval: TimeUnit,
        capabilities: mac::CapabilityInfo,
        ssid: &Ssid,
        rates: &[u8],
        channel: u8,
        tim_header: ie::TimHeader,
        tim_bitmap: &[u8],
        rsne: &[u8],
    ) -> Result<(InBuf, usize, BeaconOffloadParams), Error> {
        let mut tim_ele_offset = 0;
        let (buf, bytes_written) = write_frame!(&self.buf_provider, {
            headers: {
                mac::MgmtHdr: &mgmt_writer::mgmt_hdr_from_ap(
                    mac::FrameControl(0)
                        .with_frame_type(mac::FrameType::MGMT)
                        .with_mgmt_subtype(mac::MgmtSubtype::BEACON),
                    mac::BCAST_ADDR,
                    self.bssid,
                    // The sequence control is 0 because the firmware will set it.
                    mac::SequenceControl(0)
                ),
                mac::BeaconHdr: &mac::BeaconHdr::new(beacon_interval, capabilities),
            },
            // Order of beacon frame body IEs is according to IEEE Std 802.11-2016, Table 9-27,
            // numbered below.
            ies: {
                // 4. Service Set Identifier (SSID)
                ssid: ssid,
                // 5. Supported Rates and BSS Membership Selectors
                supported_rates: rates,
                // 6. DSSS Parameter Set
                dsss_param_set: &ie::DsssParamSet { current_channel: channel },
                // 9. Traffic indication map (TIM)
                // Write a placeholder TIM element, which the firmware will fill in.
                // We only support hardware with hardware offload beaconing for now (e.g. ath10k, before it was removed).
                tim_ele_offset @ tim: ie::TimView {
                    header: tim_header,
                    bitmap: tim_bitmap,
                },
                // 17. Extended Supported Rates and BSS Membership Selectors
                extended_supported_rates: {/* continue rates */},
                // 18. RSN
                rsne?: if !rsne.is_empty() {
                    rsne::from_bytes(rsne)
                        .map_err(|e| format_err!("error parsing rsne {:?} : {:?}", rsne, e))?
                        .1
                },

            }
        })?;
        Ok((buf, bytes_written, BeaconOffloadParams { tim_ele_offset }))
    }
    /// Sends a WLAN data frame (IEEE Std 802.11-2016, 9.3.2) to the PHY.
    pub fn make_data_frame(
        &mut self,
        dst: MacAddr,
        src: MacAddr,
        protected: bool,
        qos_ctrl: bool,
        ether_type: u16,
        payload: &[u8],
    ) -> Result<(InBuf, usize), Error> {
        let qos_ctrl = if qos_ctrl {
            Some(
                wmm::derive_tid(ether_type, payload)
                    .map_or(mac::QosControl(0), |tid| mac::QosControl(0).with_tid(tid as u16)),
            )
        } else {
            None
        };

        write_frame!(&self.buf_provider, {
            headers: {
                mac::FixedDataHdrFields: &mac::FixedDataHdrFields {
                    frame_ctrl: mac::FrameControl(0)
                        .with_frame_type(mac::FrameType::DATA)
                        .with_data_subtype(mac::DataSubtype(0).with_qos(qos_ctrl.is_some()))
                        .with_protected(protected)
                        .with_from_ds(true),
                    duration: 0,
                    addr1: dst,
                    addr2: self.bssid.0,
                    addr3: src,
                    seq_ctrl:  mac::SequenceControl(0).with_seq_num(
                        match qos_ctrl.as_ref() {
                            None => self.seq_mgr.next_sns1(&dst),
                            Some(qos_ctrl) => self.seq_mgr.next_sns2(&dst, qos_ctrl.tid()),
                        } as u16
                    ),
                },
                mac::QosControl?: qos_ctrl,
                mac::LlcHdr: &data_writer::make_snap_llc_hdr(ether_type),
            },
            payload: payload,
        })
    }

    /// Sends an EAPoL data frame (IEEE Std 802.1X, 11.3) to the PHY.
    pub fn make_eapol_frame(
        &mut self,
        dst_addr: MacAddr,
        src_addr: MacAddr,
        is_protected: bool,
        eapol_frame: &[u8],
    ) -> Result<(InBuf, usize), Error> {
        self.make_data_frame(
            dst_addr,
            src_addr,
            is_protected,
            false, // TODO(fxbug.dev/37891): Support QoS.
            mac::ETHER_TYPE_EAPOL,
            eapol_frame,
        )
    }

    // Netstack delivery functions.

    /// Delivers the Ethernet II frame to the netstack.
    pub fn deliver_eth_frame(
        &mut self,
        dst_addr: MacAddr,
        src_addr: MacAddr,
        protocol_id: u16,
        body: &[u8],
    ) -> Result<(), Error> {
        let (buf, bytes_written) = write_frame_with_fixed_buf!([0u8; mac::MAX_ETH_FRAME_LEN], {
            headers: {
                mac::EthernetIIHdr: &mac::EthernetIIHdr {
                    da: dst_addr,
                    sa: src_addr,
                    ether_type: BigEndianU16::from_native(protocol_id),
                },
            },
            payload: body,
        })?;
        let (written, _remaining) = buf.split_at(bytes_written);
        self.device
            .deliver_eth_frame(written)
            .map_err(|s| Error::Status(format!("could not deliver Ethernet II frame"), s))
    }
}

#[cfg(test)]
mod test {
    use {
        super::*,
        crate::{ap::ClientEvent, buffer::FakeBufferProvider, device::FakeDevice},
        fuchsia_async as fasync,
        std::convert::TryFrom,
        wlan_common::{
            assert_variant, mac,
            timer::{create_timer, TimeStream},
        },
    };

    const CLIENT_ADDR: MacAddr = [1u8; 6];
    const BSSID: Bssid = Bssid([2u8; 6]);
    const CLIENT_ADDR2: MacAddr = [3u8; 6];

    fn make_context(device: Device) -> (Context, TimeStream<TimedEvent>) {
        let (timer, time_stream) = create_timer();
        (Context::new(device, FakeBufferProvider::new(), timer, BSSID), time_stream)
    }

    #[test]
    fn send_mlme_auth_ind() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (ctx, _) = make_context(fake_device.as_device());
        ctx.send_mlme_auth_ind(CLIENT_ADDR, fidl_mlme::AuthenticationTypes::OpenSystem)
            .expect("expected OK");
        let msg = fake_device
            .next_mlme_msg::<fidl_mlme::AuthenticateIndication>()
            .expect("expected MLME message");
        assert_eq!(
            msg,
            fidl_mlme::AuthenticateIndication {
                peer_sta_address: CLIENT_ADDR,
                auth_type: fidl_mlme::AuthenticationTypes::OpenSystem,
            },
        );
    }

    #[test]
    fn send_mlme_deauth_ind() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (ctx, _) = make_context(fake_device.as_device());
        ctx.send_mlme_deauth_ind(
            CLIENT_ADDR,
            fidl_ieee80211::ReasonCode::LeavingNetworkDeauth,
            LocallyInitiated(true),
        )
        .expect("expected OK");
        let msg = fake_device
            .next_mlme_msg::<fidl_mlme::DeauthenticateIndication>()
            .expect("expected MLME message");
        assert_eq!(
            msg,
            fidl_mlme::DeauthenticateIndication {
                peer_sta_address: CLIENT_ADDR,
                reason_code: fidl_ieee80211::ReasonCode::LeavingNetworkDeauth,
                locally_initiated: true,
            },
        );
    }

    #[test]
    fn send_mlme_assoc_ind() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (ctx, _) = make_context(fake_device.as_device());
        ctx.send_mlme_assoc_ind(
            CLIENT_ADDR,
            1,
            Some(Ssid::try_from("coolnet").unwrap()),
            mac::CapabilityInfo(0),
            vec![ie::SupportedRate(1), ie::SupportedRate(2), ie::SupportedRate(3)],
            None,
        )
        .expect("expected OK");
        let msg = fake_device
            .next_mlme_msg::<fidl_mlme::AssociateIndication>()
            .expect("expected MLME message");
        assert_eq!(
            msg,
            fidl_mlme::AssociateIndication {
                peer_sta_address: CLIENT_ADDR,
                listen_interval: 1,
                ssid: Some(Ssid::try_from("coolnet").unwrap().into()),
                capability_info: mac::CapabilityInfo(0).raw(),
                rates: vec![1, 2, 3],
                rsne: None,
            },
        );
    }

    #[test]
    fn send_mlme_disassoc_ind() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (ctx, _) = make_context(fake_device.as_device());
        ctx.send_mlme_disassoc_ind(
            CLIENT_ADDR,
            fidl_ieee80211::ReasonCode::LeavingNetworkDisassoc,
            LocallyInitiated(true),
        )
        .expect("expected OK");
        let msg = fake_device
            .next_mlme_msg::<fidl_mlme::DisassociateIndication>()
            .expect("expected MLME message");
        assert_eq!(
            msg,
            fidl_mlme::DisassociateIndication {
                peer_sta_address: CLIENT_ADDR,
                reason_code: fidl_ieee80211::ReasonCode::LeavingNetworkDisassoc,
                locally_initiated: true,
            },
        );
    }

    #[test]
    fn send_mlme_eapol_ind() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (ctx, _) = make_context(fake_device.as_device());
        ctx.send_mlme_eapol_ind(CLIENT_ADDR2, CLIENT_ADDR, &[1, 2, 3, 4, 5][..])
            .expect("expected OK");
        let msg = fake_device
            .next_mlme_msg::<fidl_mlme::EapolIndication>()
            .expect("expected MLME message");
        assert_eq!(
            msg,
            fidl_mlme::EapolIndication {
                dst_addr: CLIENT_ADDR2,
                src_addr: CLIENT_ADDR,
                data: vec![1, 2, 3, 4, 5],
            },
        );
    }

    #[test]
    fn schedule_after() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (mut ctx, mut time_stream) = make_context(fake_device.as_device());
        let event_id = ctx.schedule_after(
            zx::Duration::from_seconds(5),
            TimedEvent::ClientEvent([1, 1, 1, 1, 1, 1], ClientEvent::BssIdleTimeout),
        );
        let (_, timed_event) =
            time_stream.try_next().unwrap().expect("Should have scheduled an event");
        assert_eq!(timed_event.id, event_id);

        assert_variant!(
            timed_event.event,
            TimedEvent::ClientEvent([1, 1, 1, 1, 1, 1], ClientEvent::BssIdleTimeout)
        );
        assert!(time_stream.try_next().is_err());
    }

    #[test]
    fn make_auth_frame() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (mut ctx, _) = make_context(fake_device.as_device());
        let (in_buf, bytes_written) = ctx
            .make_auth_frame(
                CLIENT_ADDR,
                AuthAlgorithmNumber::FAST_BSS_TRANSITION,
                3,
                fidl_ieee80211::StatusCode::TransactionSequenceError.into(),
            )
            .expect("error making auth frame");
        assert_eq!(
            &in_buf.as_slice()[..bytes_written],
            &[
                // Mgmt header
                0b10110000, 0, // Frame Control
                0, 0, // Duration
                1, 1, 1, 1, 1, 1, // addr1
                2, 2, 2, 2, 2, 2, // addr2
                2, 2, 2, 2, 2, 2, // addr3
                0x10, 0, // Sequence Control
                // Auth header:
                2, 0, // auth algorithm
                3, 0, // auth txn seq num
                14, 0, // Status code
            ][..]
        );
    }

    #[test]
    fn make_assoc_resp_frame() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (mut ctx, _) = make_context(fake_device.as_device());
        let (in_buf, bytes_written) = ctx
            .make_assoc_resp_frame(
                CLIENT_ADDR,
                mac::CapabilityInfo(0),
                1,
                &[1, 2, 3, 4, 5, 6, 7, 8, 9, 10][..],
                Some(99),
            )
            .expect("error making assoc resp frame");
        assert_eq!(
            &in_buf.as_slice()[..bytes_written],
            &[
                // Mgmt header
                0b00010000, 0, // Frame Control
                0, 0, // Duration
                1, 1, 1, 1, 1, 1, // addr1
                2, 2, 2, 2, 2, 2, // addr2
                2, 2, 2, 2, 2, 2, // addr3
                0x10, 0, // Sequence Control
                // Association response header:
                0, 0, // Capabilities
                0, 0, // status code
                1, 0, // AID
                // IEs
                1, 8, 1, 2, 3, 4, 5, 6, 7, 8, // Rates
                50, 2, 9, 10, // Extended rates
                90, 3, 99, 0, 0, // BSS max idle period
            ][..]
        );
    }

    #[test]
    fn make_assoc_resp_frame_error() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (mut ctx, _) = make_context(fake_device.as_device());
        let (in_buf, bytes_written) = ctx
            .make_assoc_resp_frame_error(
                CLIENT_ADDR,
                mac::CapabilityInfo(0),
                fidl_ieee80211::StatusCode::RejectedEmergencyServicesNotSupported.into(),
            )
            .expect("error making assoc resp frame error");
        assert_eq!(
            &in_buf.as_slice()[..bytes_written],
            &[
                // Mgmt header
                0b00010000, 0, // Frame Control
                0, 0, // Duration
                1, 1, 1, 1, 1, 1, // addr1
                2, 2, 2, 2, 2, 2, // addr2
                2, 2, 2, 2, 2, 2, // addr3
                0x10, 0, // Sequence Control
                // Association response header:
                0, 0, // Capabilities
                94, 0, // status code
                0, 0, // AID
            ][..]
        );
    }

    #[test]
    fn make_assoc_resp_frame_no_bss_max_idle_period() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (mut ctx, _) = make_context(fake_device.as_device());
        let (in_buf, bytes_written) = ctx
            .make_assoc_resp_frame(
                CLIENT_ADDR,
                mac::CapabilityInfo(0),
                1,
                &[1, 2, 3, 4, 5, 6, 7, 8, 9, 10][..],
                None,
            )
            .expect("error making assoc resp frame");
        assert_eq!(
            &in_buf.as_slice()[..bytes_written],
            &[
                // Mgmt header
                0b00010000, 0, // Frame Control
                0, 0, // Duration
                1, 1, 1, 1, 1, 1, // addr1
                2, 2, 2, 2, 2, 2, // addr2
                2, 2, 2, 2, 2, 2, // addr3
                0x10, 0, // Sequence Control
                // Association response header:
                0, 0, // Capabilities
                0, 0, // status code
                1, 0, // AID
                // IEs
                1, 8, 1, 2, 3, 4, 5, 6, 7, 8, // Rates
                50, 2, 9, 10, // Extended rates
            ][..]
        );
    }

    #[test]
    fn make_disassoc_frame() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (mut ctx, _) = make_context(fake_device.as_device());
        let (in_buf, bytes_written) = ctx
            .make_disassoc_frame(
                CLIENT_ADDR,
                fidl_ieee80211::ReasonCode::LeavingNetworkDisassoc.into(),
            )
            .expect("error making disassoc frame");
        assert_eq!(
            &in_buf.as_slice()[..bytes_written],
            &[
                // Mgmt header
                0b10100000, 0, // Frame Control
                0, 0, // Duration
                1, 1, 1, 1, 1, 1, // addr1
                2, 2, 2, 2, 2, 2, // addr2
                2, 2, 2, 2, 2, 2, // addr3
                0x10, 0, // Sequence Control
                // Disassoc header:
                8, 0, // reason code
            ][..]
        );
    }

    #[test]
    fn make_probe_resp_frame() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (mut ctx, _) = make_context(fake_device.as_device());
        let (in_buf, bytes_written) = ctx
            .make_probe_resp_frame(
                CLIENT_ADDR,
                TimeUnit(10),
                mac::CapabilityInfo(33),
                &Ssid::try_from([1, 2, 3, 4, 5]).unwrap(),
                &[1, 2, 3, 4, 5, 6, 7, 8, 9, 10][..],
                2,
                &[48, 2, 77, 88][..],
            )
            .expect("error making probe resp frame");
        assert_eq!(
            &in_buf.as_slice()[..bytes_written],
            &[
                // Mgmt header
                0b01010000, 0, // Frame Control
                0, 0, // Duration
                1, 1, 1, 1, 1, 1, // addr1
                2, 2, 2, 2, 2, 2, // addr2
                2, 2, 2, 2, 2, 2, // addr3
                0x10, 0, // Sequence Control
                // Beacon header:
                0, 0, 0, 0, 0, 0, 0, 0, // Timestamp zero since TSF Timer not implemented
                10, 0, // Beacon interval
                33, 0, // Capabilities
                // IEs:
                0, 5, 1, 2, 3, 4, 5, // SSID
                1, 8, 1, 2, 3, 4, 5, 6, 7, 8, // Supported rates
                3, 1, 2, // DSSS parameter set
                50, 2, 9, 10, // Extended rates
                48, 2, 77, 88, // RSNE
            ][..]
        );
    }

    #[test]
    fn make_beacon_frame() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (ctx, _) = make_context(fake_device.as_device());

        let (in_buf, bytes_written, params) = ctx
            .make_beacon_frame(
                TimeUnit(10),
                mac::CapabilityInfo(33),
                &Ssid::try_from([1, 2, 3, 4, 5]).unwrap(),
                &[1, 2, 3, 4, 5, 6, 7, 8, 9, 10][..],
                2,
                ie::TimHeader { dtim_count: 1, dtim_period: 2, bmp_ctrl: ie::BitmapControl(0) },
                &[1, 2, 3][..],
                &[48, 2, 77, 88][..],
            )
            .expect("error making probe resp frame");
        assert_eq!(
            &in_buf.as_slice()[..bytes_written],
            &[
                // Mgmt header
                0b10000000, 0, // Frame Control
                0, 0, // Duration
                255, 255, 255, 255, 255, 255, // addr1
                2, 2, 2, 2, 2, 2, // addr2
                2, 2, 2, 2, 2, 2, // addr3
                0, 0, // Sequence Control
                // Beacon header:
                0, 0, 0, 0, 0, 0, 0, 0, // Timestamp zero since TSF Timer not implemented
                10, 0, // Beacon interval
                33, 0, // Capabilities
                // IEs:
                0, 5, 1, 2, 3, 4, 5, // SSID
                1, 8, 1, 2, 3, 4, 5, 6, 7, 8, // Supported rates
                3, 1, 2, // DSSS parameter set
                5, 6, 1, 2, 0, 1, 2, 3, // TIM
                50, 2, 9, 10, // Extended rates
                48, 2, 77, 88, // RSNE
            ][..]
        );
        assert_eq!(params.tim_ele_offset, 56);
    }

    #[test]
    fn make_data_frame() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (mut ctx, _) = make_context(fake_device.as_device());
        let (in_buf, bytes_written) = ctx
            .make_data_frame(CLIENT_ADDR2, CLIENT_ADDR, false, false, 0x1234, &[1, 2, 3, 4, 5])
            .expect("error making data frame");
        assert_eq!(
            &in_buf.as_slice()[..bytes_written],
            &[
                // Mgmt header
                0b00001000, 0b00000010, // Frame Control
                0, 0, // Duration
                3, 3, 3, 3, 3, 3, // addr1
                2, 2, 2, 2, 2, 2, // addr2
                1, 1, 1, 1, 1, 1, // addr3
                0x10, 0, // Sequence Control
                0xAA, 0xAA, 0x03, // DSAP, SSAP, Control, OUI
                0, 0, 0, // OUI
                0x12, 0x34, // Protocol ID
                // Data
                1, 2, 3, 4, 5,
            ][..]
        );
    }

    #[test]
    fn make_data_frame_ipv4_qos() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (mut ctx, _) = make_context(fake_device.as_device());
        let (in_buf, bytes_written) = ctx
            .make_data_frame(
                CLIENT_ADDR2,
                CLIENT_ADDR,
                false,
                true,
                0x0800, // IPv4
                // Not valid IPv4 payload (too short).
                // However, we only care that it includes the DS field.
                &[1, 0xB0, 3, 4, 5], // DSCP = 0b010110 (i.e. AF23)
            )
            .expect("error making data frame");
        assert_eq!(
            &in_buf.as_slice()[..bytes_written],
            &[
                // Mgmt header
                0b10001000, 0b00000010, // Frame Control
                0, 0, // Duration
                3, 3, 3, 3, 3, 3, // addr1
                2, 2, 2, 2, 2, 2, // addr2
                1, 1, 1, 1, 1, 1, // addr3
                0x10, 0, // Sequence Control
                0x06, 0, // QoS Control - TID = 6
                0xAA, 0xAA, 0x03, // DSAP, SSAP, Control, OUI
                0, 0, 0, // OUI
                0x08, 0x00, // Protocol ID
                // Payload
                1, 0xB0, 3, 4, 5,
            ][..]
        );
    }

    #[test]
    fn make_data_frame_ipv6_qos() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (mut ctx, _) = make_context(fake_device.as_device());
        let (in_buf, bytes_written) = ctx
            .make_data_frame(
                CLIENT_ADDR2,
                CLIENT_ADDR,
                false,
                true,
                0x86DD, // IPv6
                // Not valid IPv6 payload (too short).
                // However, we only care that it includes the DS field.
                &[0b0101, 0b10000000, 3, 4, 5], // DSCP = 0b101100 (i.e. VOICE-ADMIT)
            )
            .expect("error making data frame");
        assert_eq!(
            &in_buf.as_slice()[..bytes_written],
            &[
                // Mgmt header
                0b10001000, 0b00000010, // Frame Control
                0, 0, // Duration
                3, 3, 3, 3, 3, 3, // addr1
                2, 2, 2, 2, 2, 2, // addr2
                1, 1, 1, 1, 1, 1, // addr3
                0x10, 0, // Sequence Control
                0x03, 0, // QoS Control - TID = 3
                0xAA, 0xAA, 0x03, // DSAP, SSAP, Control, OUI
                0, 0, 0, // OUI
                0x86, 0xDD, // Protocol ID
                // Payload
                0b0101, 0b10000000, 3, 4, 5,
            ][..]
        );
    }

    #[test]
    fn make_eapol_frame() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (mut ctx, _) = make_context(fake_device.as_device());
        let (in_buf, bytes_written) = ctx
            .make_eapol_frame(CLIENT_ADDR2, CLIENT_ADDR, false, &[1, 2, 3, 4, 5])
            .expect("error making eapol frame");
        assert_eq!(
            &in_buf.as_slice()[..bytes_written],
            &[
                // Mgmt header
                0b00001000, 0b00000010, // Frame Control
                0, 0, // Duration
                3, 3, 3, 3, 3, 3, // addr1
                2, 2, 2, 2, 2, 2, // addr2
                1, 1, 1, 1, 1, 1, // addr3
                0x10, 0, // Sequence Control
                0xAA, 0xAA, 0x03, // DSAP, SSAP, Control, OUI
                0, 0, 0, // OUI
                0x88, 0x8E, // EAPOL protocol ID
                // Data
                1, 2, 3, 4, 5,
            ][..]
        );
    }

    #[test]
    fn deliver_eth_frame() {
        let exec = fasync::TestExecutor::new();
        let mut fake_device = FakeDevice::new(&exec);
        let (mut ctx, _) = make_context(fake_device.as_device());
        ctx.deliver_eth_frame(CLIENT_ADDR2, CLIENT_ADDR, 0x1234, &[1, 2, 3, 4, 5][..])
            .expect("expected OK");
        assert_eq!(fake_device.eth_queue.len(), 1);
        #[rustfmt::skip]
        assert_eq!(&fake_device.eth_queue[0][..], &[
            3, 3, 3, 3, 3, 3,  // dest
            1, 1, 1, 1, 1, 1,  // src
            0x12, 0x34,        // ether_type
            // Data
            1, 2, 3, 4, 5,
        ][..]);
    }
}
