// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    banjo_fuchsia_wlan_softmac as banjo_wlan_softmac, fidl_fuchsia_wlan_common as fidl_common,
    fidl_fuchsia_wlan_internal as fidl_internal,
    ieee80211::Bssid,
    wlan_common::{channel::derive_channel, ie, mac::CapabilityInfo, TimeUnit},
};

/// Given information from beacon or probe response, convert to BssDescription.
pub fn construct_bss_description(
    bssid: Bssid,
    beacon_interval: TimeUnit,
    capability_info: CapabilityInfo,
    ies: &[u8],
    rx_info: banjo_wlan_softmac::WlanRxInfo,
) -> Result<fidl_internal::BssDescription, Error> {
    let mut dsss_channel = None;
    let mut parsed_ht_op = None;
    let mut parsed_vht_op = None;

    for (id, body) in ie::Reader::new(ies) {
        match id {
            ie::Id::DSSS_PARAM_SET => {
                dsss_channel = Some(ie::parse_dsss_param_set(body)?.current_channel)
            }
            ie::Id::HT_OPERATION => {
                let ht_op = ie::parse_ht_operation(body)?;
                parsed_ht_op = Some(*ht_op);
            }
            ie::Id::VHT_OPERATION => {
                let ht_op = ie::parse_vht_operation(body)?;
                parsed_vht_op = Some(*ht_op);
            }
            _ => (),
        }
    }

    let bss_type = get_bss_type(capability_info);
    let channel =
        derive_channel(rx_info.channel.primary, dsss_channel, parsed_ht_op, parsed_vht_op);

    Ok(fidl_internal::BssDescription {
        bssid: bssid.0,
        bss_type,
        beacon_period: beacon_interval.0,
        capability_info: capability_info.raw(),
        ies: ies.to_vec(),
        channel,
        rssi_dbm: rx_info.rssi_dbm,
        snr_db: 0,
    })
}

/// Note: This is in Beacon / Probe Response frames context.
/// IEEE Std 802.11-2016, 9.4.1.4
fn get_bss_type(capability_info: CapabilityInfo) -> fidl_common::BssType {
    match (capability_info.ess(), capability_info.ibss()) {
        (true, false) => fidl_common::BssType::Infrastructure,
        (false, true) => fidl_common::BssType::Independent,
        (false, false) => fidl_common::BssType::Mesh,
        _ => fidl_common::BssType::Unknown,
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*, banjo_fuchsia_wlan_common as banjo_common,
        fidl_fuchsia_wlan_common as fidl_common,
    };

    const BSSID: Bssid = Bssid([0x33; 6]);
    const BEACON_INTERVAL: u16 = 100;
    // Capability information: ESS, privacy, spectrum mgmt, radio msmt
    const CAPABILITY_INFO: CapabilityInfo = CapabilityInfo(0x1111);
    const RX_INFO: banjo_wlan_softmac::WlanRxInfo = banjo_wlan_softmac::WlanRxInfo {
        channel: banjo_common::WlanChannel {
            primary: 11,
            cbw: banjo_common::ChannelBandwidth::CBW20,
            secondary80: 0,
        },
        rssi_dbm: -40,
        snr_dbh: 35,

        // Unused fields
        rx_flags: banjo_wlan_softmac::WlanRxInfoFlags(0),
        valid_fields: banjo_wlan_softmac::WlanRxInfoValid(0),
        phy: banjo_common::WlanPhyType::DSSS,
        data_rate: 0,
        mcs: 0,
    };

    fn beacon_frame_ies() -> Vec<u8> {
        #[rustfmt::skip]
        let ies = vec![
            // SSID: "foo-ssid"
            0x00, 0x08, 0x66, 0x6f, 0x6f, 0x2d, 0x73, 0x73, 0x69, 0x64,
            // Supported rates: 24(B), 36, 48, 54
            0x01, 0x04, 0xb0, 0x48, 0x60, 0x6c,
            // DS parameter set: channel 140
            0x03, 0x01, 0x8c,
            // TIM - DTIM count: 0, DTIM period: 1, PVB: 2
            0x05, 0x04, 0x00, 0x01, 0x00, 0x02,
            // Country info
            0x07, 0x10,
            0x55, 0x53, 0x20, // US, Any environment
            0x24, 0x04, 0x24, // 1st channel: 36, # channels: 4, maximum tx power: 36 dBm
            0x34, 0x04, 0x1e, // 1st channel: 52, # channels: 4, maximum tx power: 30 dBm
            0x64, 0x0c, 0x1e, // 1st channel: 100, # channels: 12, maximum tx power: 30 dBm
            0x95, 0x05, 0x24, // 1st channel: 149, # channels: 5, maximum tx power: 36 dBm
            0x00, // padding
            // Power constraint: 0
            0x20, 0x01, 0x00,
            // TPC Report Transmit Power: 9, Link Margin: 0
            0x23, 0x02, 0x09, 0x00,
            // RSN Information
            0x30, 0x14, 0x01, 0x00,
            0x00, 0x0f, 0xac, 0x04, // Group cipher: AES (CCM)
            0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, // Pairwise cipher: AES (CCM)
            0x01, 0x00, 0x00, 0x0f, 0xac, 0x01, // AKM: EAP
            0x28, 0x00, // RSN capabilities
            // HT Capabilities
            0x2d, 0x1a,
            0xef, 0x09, // HT capabilities info
            0x17, // A-MPDU parameters
            0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // MCS set
            0x00, 0x00, // HT extended capabilities
            0x00, 0x00, 0x00, 0x00, // Transmit beamforming
            0x00, // Antenna selection capabilities
            // HT Operation
            0x3d, 0x16,
            0x8c, // Primary channel: 140
            0x0d, // HT info subset - secondary channel above, any channel width, RIFS permitted
            0x16, 0x00, 0x00, 0x00, // HT info subsets
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Basic MCS set
            // Extended Capabilities: extended channel switching, BSS transition, operating mode notification
            0x7f, 0x08, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x40,
            // VHT Capabilities
            0xbf, 0x0c,
            0x91, 0x59, 0x82, 0x0f, // VHT capabilities info
            0xea, 0xff, 0x00, 0x00, 0xea, 0xff, 0x00, 0x00, // VHT supported MCS set
            // VHT Operation
            0xc0, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
            // VHT Tx Power Envelope
            0xc3, 0x03, 0x01, 0x24, 0x24,
            // Aruba, Hewlett Packard vendor-specific IE
            0xdd, 0x07, 0x00, 0x0b, 0x86, 0x01, 0x04, 0x08, 0x09,
            // WMM parameters
            0xdd, 0x18, 0x00, 0x50, 0xf2, 0x02, 0x01, 0x01,
            0x80, // U-APSD enabled
            0x00, // reserved
            0x03, 0xa4, 0x00, 0x00, // AC_BE parameters
            0x27, 0xa4, 0x00, 0x00, // AC_BK parameters
            0x42, 0x43, 0x5e, 0x00, // AC_VI parameters
            0x62, 0x32, 0x2f, 0x00, // AC_VO parameters
        ];
        ies
    }

    #[test]
    fn test_construct_bss_description() {
        let ies = beacon_frame_ies();
        let bss_description = construct_bss_description(
            BSSID,
            TimeUnit(BEACON_INTERVAL),
            CAPABILITY_INFO,
            &ies[..],
            RX_INFO,
        )
        .expect("expect convert_beacon to succeed");

        assert_eq!(
            bss_description,
            fidl_internal::BssDescription {
                bssid: BSSID.0,
                bss_type: fidl_common::BssType::Infrastructure,
                beacon_period: BEACON_INTERVAL,
                capability_info: CAPABILITY_INFO.0,
                ies,
                rssi_dbm: RX_INFO.rssi_dbm,
                channel: fidl_common::WlanChannel {
                    primary: 140,
                    cbw: fidl_common::ChannelBandwidth::Cbw40,
                    secondary80: 0,
                },
                snr_db: 0,
            }
        );
    }
}
