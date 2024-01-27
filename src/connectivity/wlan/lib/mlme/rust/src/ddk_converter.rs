// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Error},
    banjo_fuchsia_hardware_wlan_associnfo as banjo_wlan_associnfo,
    banjo_fuchsia_wlan_common as banjo_common, banjo_fuchsia_wlan_ieee80211 as banjo_ieee80211,
    banjo_fuchsia_wlan_softmac as banjo_wlan_softmac, fidl_fuchsia_wlan_common as fidl_common,
    fidl_fuchsia_wlan_ieee80211 as fidl_ieee80211, fidl_fuchsia_wlan_mlme as fidl_mlme,
    ieee80211::Bssid,
    wlan_common::{capabilities::StaCapabilities, ie, mac::Aid},
    zerocopy::AsBytes,
};

pub fn ddk_channel_from_fidl(fc: fidl_common::WlanChannel) -> banjo_common::WlanChannel {
    let cbw = match fc.cbw {
        fidl_common::ChannelBandwidth::Cbw20 => banjo_common::ChannelBandwidth::CBW20,
        fidl_common::ChannelBandwidth::Cbw40 => banjo_common::ChannelBandwidth::CBW40,
        fidl_common::ChannelBandwidth::Cbw40Below => banjo_common::ChannelBandwidth::CBW40BELOW,
        fidl_common::ChannelBandwidth::Cbw80 => banjo_common::ChannelBandwidth::CBW80,
        fidl_common::ChannelBandwidth::Cbw160 => banjo_common::ChannelBandwidth::CBW160,
        fidl_common::ChannelBandwidth::Cbw80P80 => banjo_common::ChannelBandwidth::CBW80P80,
    };
    banjo_common::WlanChannel { primary: fc.primary, cbw, secondary80: fc.secondary80 }
}

pub fn build_ddk_assoc_ctx(
    bssid: Bssid,
    aid: Aid,
    channel: banjo_common::WlanChannel,
    negotiated_capabilities: StaCapabilities,
    ht_op: Option<[u8; fidl_ieee80211::HT_OP_LEN as usize]>,
    vht_op: Option<[u8; fidl_ieee80211::VHT_OP_LEN as usize]>,
) -> banjo_wlan_associnfo::WlanAssocCtx {
    let mut rates = [0; banjo_wlan_associnfo::WLAN_MAC_MAX_RATES as usize];
    rates[..negotiated_capabilities.rates.len()]
        .clone_from_slice(negotiated_capabilities.rates.as_bytes());
    let has_ht_cap = negotiated_capabilities.ht_cap.is_some();
    let has_vht_cap = negotiated_capabilities.vht_cap.is_some();
    let mut ht_cap =
        banjo_ieee80211::HtCapabilities { bytes: [0u8; fidl_ieee80211::HT_CAP_LEN as usize] };
    let mut vht_cap =
        banjo_ieee80211::VhtCapabilities { bytes: [0u8; fidl_ieee80211::VHT_CAP_LEN as usize] };
    negotiated_capabilities
        .ht_cap
        .map(|negotiated_ht_cap| ht_cap.bytes.copy_from_slice(&negotiated_ht_cap.as_bytes()[..]));
    negotiated_capabilities.vht_cap.map(|negotiated_vht_cap| {
        vht_cap.bytes.copy_from_slice(&negotiated_vht_cap.as_bytes()[..])
    });
    let ht_op_bytes = ht_op.unwrap_or([0; fidl_ieee80211::HT_OP_LEN as usize]);
    let vht_op_bytes = vht_op.unwrap_or([0; fidl_ieee80211::VHT_OP_LEN as usize]);
    banjo_wlan_associnfo::WlanAssocCtx {
        bssid: bssid.0,
        aid,
        // In the association request we sent out earlier, listen_interval is always set to 0,
        // indicating the client never enters power save mode.
        // TODO(fxbug.dev/42217): ath10k disregard this value and hard code it to 1.
        // It is working now but we may need to revisit.
        listen_interval: 0,
        channel,
        // TODO(fxbug.dev/29325): QoS works with Aruba/Ubiquiti for BlockAck session but it may need to be
        // dynamically determined for each outgoing data frame.
        // TODO(fxbug.dev/43938): Derive QoS flag and WMM parameters from device info
        qos: has_ht_cap,
        wmm_params: blank_wmm_params(),

        rates_cnt: negotiated_capabilities.rates.len() as u16, // will not overflow as MAX_RATES_LEN is u8
        rates,
        capability_info: negotiated_capabilities.capability_info.raw(),
        // All the unwrap are safe because the size of the byte array follow wire format.
        has_ht_cap,
        ht_cap,
        has_ht_op: ht_op.is_some(),
        ht_op: { *ie::parse_ht_operation(&ht_op_bytes[..]).unwrap() }.into(),
        has_vht_cap,
        vht_cap,
        has_vht_op: vht_op.is_some(),
        vht_op: { *ie::parse_vht_operation(&vht_op_bytes[..]).unwrap() }.into(),
    }
}

pub fn get_rssi_dbm(rx_info: banjo_wlan_softmac::WlanRxInfo) -> Option<i8> {
    match rx_info.valid_fields & banjo_wlan_associnfo::WlanRxInfoValid::RSSI.0 != 0
        && rx_info.rssi_dbm != 0
    {
        true => Some(rx_info.rssi_dbm),
        false => None,
    }
}

pub fn blank_wmm_params() -> banjo_wlan_associnfo::WlanWmmParameters {
    banjo_wlan_associnfo::WlanWmmParameters {
        apsd: false,
        ac_be_params: blank_wmm_ac_params(),
        ac_bk_params: blank_wmm_ac_params(),
        ac_vi_params: blank_wmm_ac_params(),
        ac_vo_params: blank_wmm_ac_params(),
    }
}

fn blank_wmm_ac_params() -> banjo_wlan_associnfo::WlanWmmAcParams {
    banjo_wlan_associnfo::WlanWmmAcParams {
        ecw_min: 0,
        ecw_max: 0,
        aifsn: 0,
        txop_limit: 0,
        acm: false,
    }
}

pub fn device_info_from_wlan_softmac_query_response(
    info: banjo_wlan_softmac::WlanSoftmacQueryResponse,
) -> Result<fidl_mlme::DeviceInfo, Error> {
    let sta_addr = info.sta_addr;
    let role = fidl_common::WlanMacRole::from_primitive(info.mac_role.0)
        .ok_or(format_err!("Unknown WlanWlanMacRole: {}", info.mac_role.0))?;
    let softmac_hardware_capability = info.hardware_capability;
    let mut bands = vec![];
    // SAFETY: softmac.fidl API guarantees these values represent a valid memory region.
    let band_caps_list =
        unsafe { std::slice::from_raw_parts(info.band_caps_list, info.band_caps_count) };
    for band_cap in band_caps_list {
        bands.push(convert_ddk_band_cap(band_cap)?);
    }
    Ok(fidl_mlme::DeviceInfo {
        sta_addr,
        role,
        bands,
        qos_capable: false,
        softmac_hardware_capability,
    })
}

pub fn convert_ddk_discovery_support(
    support: banjo_common::DiscoverySupport,
) -> Result<fidl_common::DiscoverySupport, Error> {
    let scan_offload = fidl_common::ScanOffloadExtension {
        supported: support.scan_offload.supported,
        scan_cancel_supported: support.scan_offload.scan_cancel_supported,
    };
    let probe_response_offload = fidl_common::ProbeResponseOffloadExtension {
        supported: support.probe_response_offload.supported,
    };
    Ok(fidl_common::DiscoverySupport { scan_offload, probe_response_offload })
}

pub fn convert_ddk_mac_sublayer_support(
    support: banjo_common::MacSublayerSupport,
) -> Result<fidl_common::MacSublayerSupport, Error> {
    let rate_selection_offload = fidl_common::RateSelectionOffloadExtension {
        supported: support.rate_selection_offload.supported,
    };
    let mac_implementation_type = match support.device.mac_implementation_type {
        banjo_common::MacImplementationType::SOFTMAC => fidl_common::MacImplementationType::Softmac,
        banjo_common::MacImplementationType::FULLMAC => fidl_common::MacImplementationType::Fullmac,
        _ => {
            return Err(format_err!(
                "Unexpected banjo_comon::MacImplementationType value {}",
                support.device.mac_implementation_type.0
            ))
        }
    };
    let device = fidl_common::DeviceExtension {
        is_synthetic: support.device.is_synthetic,
        tx_status_report_supported: support.device.tx_status_report_supported,
        mac_implementation_type: mac_implementation_type,
    };

    let data_plane_type = match support.data_plane.data_plane_type {
        banjo_common::DataPlaneType::ETHERNET_DEVICE => fidl_common::DataPlaneType::EthernetDevice,
        banjo_common::DataPlaneType::GENERIC_NETWORK_DEVICE => {
            fidl_common::DataPlaneType::GenericNetworkDevice
        }
        _ => {
            return Err(format_err!(
                "Unexpected banjo_comon::DataPlaneType value {}",
                support.data_plane.data_plane_type.0
            ))
        }
    };
    let data_plane = fidl_common::DataPlaneExtension { data_plane_type: data_plane_type };
    Ok(fidl_common::MacSublayerSupport { rate_selection_offload, data_plane, device })
}

pub fn convert_ddk_security_support(
    support: banjo_common::SecuritySupport,
) -> Result<fidl_common::SecuritySupport, Error> {
    let mfp = fidl_common::MfpFeature { supported: support.mfp.supported };
    let sae = fidl_common::SaeFeature {
        driver_handler_supported: support.sae.driver_handler_supported,
        sme_handler_supported: support.sae.sme_handler_supported,
    };
    Ok(fidl_common::SecuritySupport { sae, mfp })
}

pub fn convert_ddk_spectrum_management_support(
    support: banjo_common::SpectrumManagementSupport,
) -> Result<fidl_common::SpectrumManagementSupport, Error> {
    Ok(fidl_common::SpectrumManagementSupport {
        dfs: fidl_common::DfsFeature { supported: support.dfs.supported },
    })
}

fn convert_ddk_band_cap(
    band_cap: &banjo_wlan_softmac::WlanSoftmacBandCapability,
) -> Result<fidl_mlme::BandCapability, Error> {
    let band = match band_cap.band {
        banjo_common::WlanBand::TWO_GHZ => fidl_common::WlanBand::TwoGhz,
        banjo_common::WlanBand::FIVE_GHZ => fidl_common::WlanBand::FiveGhz,
        _ => return Err(format_err!("Unexpected banjo_comon::WlanBand value {}", band_cap.band.0)),
    };
    let basic_rates = band_cap.basic_rate_list[..band_cap.basic_rate_count as usize].to_vec();
    let operating_channels =
        band_cap.operating_channel_list[..band_cap.operating_channel_count as usize].to_vec();
    let ht_cap = if band_cap.ht_supported {
        Some(Box::new(fidl_ieee80211::HtCapabilities { bytes: band_cap.ht_caps.bytes }))
    } else {
        None
    };
    let vht_cap = if band_cap.vht_supported {
        Some(Box::new(fidl_ieee80211::VhtCapabilities { bytes: band_cap.vht_caps.bytes }))
    } else {
        None
    };
    Ok(fidl_mlme::BandCapability { band, basic_rates, operating_channels, ht_cap, vht_cap })
}

pub fn cssid_from_ssid_unchecked(ssid: &Vec<u8>) -> banjo_ieee80211::CSsid {
    let mut cssid = banjo_ieee80211::CSsid {
        len: ssid.len() as u8,
        data: [0; fidl_ieee80211::MAX_SSID_BYTE_LEN as usize],
    };
    // Ssid never exceeds fidl_ieee80211::MAX_SSID_BYTE_LEN bytes, so this assignment will never panic
    cssid.data[..ssid.len()].copy_from_slice(&ssid[..]);
    cssid
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::device::{
            fake_discovery_support, fake_mac_sublayer_support, fake_security_support,
            fake_spectrum_management_support, fake_wlan_softmac_query_response,
        },
        banjo_fuchsia_wlan_ieee80211 as banjo_80211,
        std::convert::TryInto,
        wlan_common::{ie, mac},
        zerocopy::AsBytes,
    };

    #[test]
    fn assoc_ctx_construction_successful() {
        let ddk = build_ddk_assoc_ctx(
            Bssid([1, 2, 3, 4, 5, 6]),
            42,
            banjo_common::WlanChannel {
                primary: 149,
                cbw: banjo_common::ChannelBandwidth::CBW40,
                secondary80: 42,
            },
            StaCapabilities {
                capability_info: mac::CapabilityInfo(0x1234),
                rates: vec![111, 112, 113, 114, 115, 116, 117, 118, 119, 120]
                    .into_iter()
                    .map(ie::SupportedRate)
                    .collect(),
                ht_cap: Some(ie::fake_ht_capabilities()),
                vht_cap: Some(ie::fake_vht_capabilities()),
            },
            Some(ie::fake_ht_operation().as_bytes().try_into().unwrap()),
            Some(ie::fake_vht_operation().as_bytes().try_into().unwrap()),
        );
        assert_eq!([1, 2, 3, 4, 5, 6], ddk.bssid);
        assert_eq!(42, ddk.aid);
        assert_eq!(0, ddk.listen_interval);
        assert_eq!(
            banjo_common::WlanChannel {
                primary: 149,
                cbw: banjo_common::ChannelBandwidth::CBW40,
                secondary80: 42
            },
            ddk.channel
        );
        assert_eq!(true, ddk.qos);

        assert_eq!(10, ddk.rates_cnt);
        assert_eq!([111, 112, 113, 114, 115, 116, 117, 118, 119, 120], ddk.rates[0..10]);
        assert_eq!(&[0; 253][..], &ddk.rates[10..]);

        assert_eq!(0x1234, ddk.capability_info);

        assert_eq!(true, ddk.has_ht_cap);
        assert_eq!(&ie::fake_ht_capabilities().as_bytes()[..], &ddk.ht_cap.bytes[..]);

        assert_eq!(true, ddk.has_ht_op);
        let expected_ht_op: banjo_wlan_associnfo::WlanHtOp = ie::fake_ht_operation().into();
        assert_eq!(expected_ht_op, ddk.ht_op);

        assert_eq!(true, ddk.has_vht_cap);
        let expected_vht_cap: banjo_80211::VhtCapabilities = ie::fake_vht_capabilities().into();
        assert_eq!(expected_vht_cap, ddk.vht_cap);

        assert_eq!(true, ddk.has_vht_op);
        let expected_vht_op: banjo_wlan_associnfo::WlanVhtOp = ie::fake_vht_operation().into();
        assert_eq!(expected_vht_op, ddk.vht_op);
    }

    fn empty_rx_info() -> banjo_wlan_softmac::WlanRxInfo {
        banjo_wlan_softmac::WlanRxInfo {
            rx_flags: banjo_fuchsia_wlan_softmac::WlanRxInfoFlags(0),
            valid_fields: 0,
            phy: banjo_common::WlanPhyType::DSSS,
            data_rate: 0,
            channel: banjo_common::WlanChannel {
                primary: 0,
                cbw: banjo_common::ChannelBandwidth::CBW20,
                secondary80: 0,
            },
            mcs: 0,
            rssi_dbm: 0,
            snr_dbh: 0,
        }
    }

    #[test]
    fn test_get_rssi_dbm_field_not_valid() {
        let rx_info =
            banjo_wlan_softmac::WlanRxInfo { valid_fields: 0, rssi_dbm: 20, ..empty_rx_info() };
        assert_eq!(get_rssi_dbm(rx_info), None);
    }

    #[test]
    fn test_get_rssi_dbm_zero_dbm() {
        let rx_info = banjo_wlan_softmac::WlanRxInfo {
            valid_fields: banjo_wlan_associnfo::WlanRxInfoValid::RSSI.0,
            rssi_dbm: 0,
            ..empty_rx_info()
        };
        assert_eq!(get_rssi_dbm(rx_info), None);
    }

    #[test]
    fn test_get_rssi_dbm_all_good() {
        let rx_info = banjo_wlan_softmac::WlanRxInfo {
            valid_fields: banjo_wlan_associnfo::WlanRxInfoValid::RSSI.0,
            rssi_dbm: 20,
            ..empty_rx_info()
        };
        assert_eq!(get_rssi_dbm(rx_info), Some(20));
    }

    #[test]
    fn test_convert_band_cap() {
        let mut supported_phys: Vec<banjo_common::WlanPhyType> = Vec::new();
        let mut band_caps: Vec<banjo_wlan_softmac::WlanSoftmacBandCapability> = Vec::new();
        let _wlan_softmac_query_response =
            fake_wlan_softmac_query_response(&mut supported_phys, &mut band_caps);
        let band0 = convert_ddk_band_cap(&band_caps[0]).expect("failed to convert band capability");
        assert_eq!(band0.band, fidl_common::WlanBand::TwoGhz);
        assert_eq!(
            band0.basic_rates,
            vec![0x02, 0x04, 0x0b, 0x16, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c]
        );
        assert_eq!(band0.operating_channels, vec![1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14]);
        assert!(band0.ht_cap.is_some());
        assert!(band0.vht_cap.is_none());
    }

    #[test]
    fn test_convert_device_info() {
        let mut supported_phys: Vec<banjo_common::WlanPhyType> = Vec::new();
        let mut band_caps: Vec<banjo_wlan_softmac::WlanSoftmacBandCapability> = Vec::new();
        let wlan_softmac_query_response =
            fake_wlan_softmac_query_response(&mut supported_phys, &mut band_caps);
        let device_info = device_info_from_wlan_softmac_query_response(wlan_softmac_query_response)
            .expect("Failed to conver wlan-softmac info");
        assert_eq!(device_info.sta_addr, wlan_softmac_query_response.sta_addr);
        assert_eq!(device_info.role, fidl_common::WlanMacRole::Client);
        assert_eq!(device_info.bands.len(), 2);
    }

    #[test]
    fn test_convert_device_info_unknown_role() {
        let mut supported_phys: Vec<banjo_common::WlanPhyType> = Vec::new();
        let mut band_caps: Vec<banjo_wlan_softmac::WlanSoftmacBandCapability> = Vec::new();
        let mut wlan_softmac_query_response =
            fake_wlan_softmac_query_response(&mut supported_phys, &mut band_caps);
        wlan_softmac_query_response.mac_role.0 = 10;
        device_info_from_wlan_softmac_query_response(wlan_softmac_query_response)
            .expect_err("Shouldn't convert invalid mac role");
    }

    #[test]
    fn test_convert_ddk_discovery_support() {
        let support_ddk = fake_discovery_support();
        let support_fidl = convert_ddk_discovery_support(support_ddk)
            .expect("Failed to convert discovery support");
        assert_eq!(support_fidl.scan_offload.supported, support_ddk.scan_offload.supported);
        assert_eq!(
            support_fidl.probe_response_offload.supported,
            support_ddk.probe_response_offload.supported
        );
    }

    #[test]
    fn test_convert_ddk_mac_sublayer_support() {
        let support_ddk = fake_mac_sublayer_support();
        let support_fidl = convert_ddk_mac_sublayer_support(support_ddk)
            .expect("Failed to convert MAC sublayer support");
        assert_eq!(
            support_fidl.rate_selection_offload.supported,
            support_ddk.rate_selection_offload.supported
        );
        assert_eq!(
            support_fidl.data_plane.data_plane_type,
            fidl_common::DataPlaneType::EthernetDevice
        );
        assert_eq!(support_fidl.device.is_synthetic, support_ddk.device.is_synthetic);
        assert_eq!(
            support_fidl.device.mac_implementation_type,
            fidl_common::MacImplementationType::Softmac
        );
        assert_eq!(
            support_fidl.device.tx_status_report_supported,
            support_ddk.device.tx_status_report_supported
        );
    }

    #[test]
    fn test_convert_ddk_security_support() {
        let support_ddk = fake_security_support();
        let support_fidl =
            convert_ddk_security_support(support_ddk).expect("Failed to convert security support");
        assert_eq!(
            support_fidl.sae.driver_handler_supported,
            support_ddk.sae.driver_handler_supported
        );
        assert_eq!(support_fidl.sae.sme_handler_supported, support_ddk.sae.sme_handler_supported);
        assert_eq!(support_fidl.mfp.supported, support_ddk.mfp.supported);
    }

    #[test]
    fn test_convert_ddk_spectrum_management_support() {
        let support_ddk = fake_spectrum_management_support();
        let support_fidl = convert_ddk_spectrum_management_support(support_ddk)
            .expect("Failed to convert spectrum management support");
        assert_eq!(support_fidl.dfs.supported, support_ddk.dfs.supported);
    }
}
