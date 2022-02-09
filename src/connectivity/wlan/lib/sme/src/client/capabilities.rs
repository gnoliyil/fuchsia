// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This module tries to check the iface device's capabilities against the BSS it is instructed to
//! join. The capabilities will be tailored based on the band.
//! Next, rates will be joined with the AP and HT Capabilities and VHT Capabilities may be modified
//! based on the user-overridable join channel and bandwidth.
//! If successful, the capabilities will be extracted and saved.

use {
    crate::capabilities::{get_device_band_info, ClientCapabilities, StaCapabilities},
    anyhow::{format_err, Context as _, Error},
    fidl_fuchsia_wlan_internal as fidl_internal, fidl_fuchsia_wlan_mlme as fidl_mlme,
    wlan_common::{
        channel::{Cbw, Channel},
        ie::{
            intersect::*, parse_ht_capabilities, parse_vht_capabilities, HtCapabilities,
            SupportedRate, VhtCapabilities,
        },
        mac::CapabilityInfo,
    },
};

/// Capability Info is defined in IEEE Std 802.11-1026 9.4.1.4.
/// Figure 9-68 indicates BSS and IBSS bits are reserved for client.
/// However, some APs will reject association unless they are set to true and false, respectively.
const OVERRIDE_CAP_INFO_ESS: bool = true;
const OVERRIDE_CAP_INFO_IBSS: bool = false;

/// IEEE Std 802.11-2016 Table 9-43 defines CF-Pollable and CF-Poll Request, Fuchsia does not
/// support them.
const OVERRIDE_CAP_INFO_CF_POLLABLE: bool = false;
const OVERRIDE_CAP_INFO_CF_POLL_REQUEST: bool = false;

/// In an RSNA non-AP STA, privacy bit is set to false. Otherwise it is reserved (has no meaning and
/// is not used).
const OVERRIDE_CAP_INFO_PRIVACY: bool = false;

/// Spectrum Management bit indicates dot11SpectrumManagementRequired. Fuchsia does not support it.
const OVERRIDE_CAP_INFO_SPECTRUM_MGMT: bool = false;

/// Fuchsia does not support tx_stbc with our existing SoftMAC chips.
/// TODO(fxbug.dev/29089): Enable tx_stbc when ath10k supports it.
const OVERRIDE_HT_CAP_INFO_TX_STBC: bool = false;

/// Supported channel bandwidth set can only be non-zero if the associating channel is 160 MHz or
/// 80+80 MHz Channel bandwidth. Otherwise it will be set to 0. 0 is a purely numeric value without
/// a name. See IEEE Std 802.11-2016 Table 9-250 for more details.
/// TODO(fxbug.dev/39546): finer control over CBW if necessary.
const OVERRIDE_VHT_CAP_INFO_SUPPORTED_CBW_SET: u32 = 0;

/// A driver may not properly populate an interface's Capabilities to reflect the selected role.
/// Override the reported capabilities to ensure compatibility with Client role.
fn override_capability_info(capability_info: CapabilityInfo) -> CapabilityInfo {
    capability_info
        .with_ess(OVERRIDE_CAP_INFO_ESS)
        .with_ibss(OVERRIDE_CAP_INFO_IBSS)
        .with_cf_pollable(OVERRIDE_CAP_INFO_CF_POLLABLE)
        .with_cf_poll_req(OVERRIDE_CAP_INFO_CF_POLL_REQUEST)
        .with_privacy(OVERRIDE_CAP_INFO_PRIVACY)
        .with_spectrum_mgmt(OVERRIDE_CAP_INFO_SPECTRUM_MGMT)
}

/// The entry point of this module.
/// 1. Extract the band capabilities from the iface device based on BSS channel.
/// 2. Derive/Override capabilities based on iface capabilities, BSS requirements and
/// user overridable channel bandwidths.
pub(crate) fn derive_join_capabilities(
    bss_channel: Channel,
    bss_rates: &[SupportedRate],
    device_info: &fidl_mlme::DeviceInfo,
) -> Result<ClientCapabilities, Error> {
    // Step 1 - Extract iface capabilities for this particular band we are joining
    let band_info = get_device_band_info(&device_info, bss_channel.primary)
        .ok_or_else(|| format_err!("iface does not support BSS channel {}", bss_channel.primary))?;

    // Step 2.1 - Override CapabilityInfo
    let capability_info = override_capability_info(CapabilityInfo(band_info.capability_info));

    // Step 2.2 - Derive data rates
    // Both are safe to unwrap because SupportedRate is one byte and will not cause alignment issue.
    let client_rates = band_info.rates.iter().map(|&r| SupportedRate(r)).collect::<Vec<_>>();
    let rates = intersect_rates(ApRates(bss_rates), ClientRates(&client_rates))
        .map_err(|error| format_err!("could not intersect rates: {:?}", error))
        .context(format!("deriving rates: {:?} + {:?}", band_info.rates, bss_rates))?;

    // Step 2.3 - Override HT Capabilities and VHT Capabilities
    // Here it is assumed that the channel specified by the BSS will never be invalid.
    let (ht_cap, vht_cap) =
        override_ht_vht(band_info.ht_cap.as_ref(), band_info.vht_cap.as_ref(), bss_channel.cbw)?;

    Ok(ClientCapabilities(StaCapabilities { capability_info, rates, ht_cap, vht_cap }))
}

/// Wrapper function to convert FIDL {HT,VHT}Capabilities into byte arrays, taking into account the
/// limitations imposed by the channel bandwidth.
fn override_ht_vht(
    fidl_ht_cap: Option<&Box<fidl_internal::HtCapabilities>>,
    fidl_vht_cap: Option<&Box<fidl_internal::VhtCapabilities>>,
    cbw: Cbw,
) -> Result<(Option<HtCapabilities>, Option<VhtCapabilities>), Error> {
    if fidl_ht_cap.is_none() && fidl_vht_cap.is_some() {
        return Err(format_err!("VHT Cap without HT Cap is invalid."));
    }

    let ht_cap = match fidl_ht_cap {
        Some(h) => {
            let ht_cap = *parse_ht_capabilities(&h.bytes[..]).context("verifying HT Cap")?;
            Some(override_ht_capabilities(ht_cap, cbw))
        }
        None => None,
    };

    let vht_cap = match fidl_vht_cap {
        Some(v) => {
            let vht_cap = *parse_vht_capabilities(&v.bytes[..]).context("verifying VHT Cap")?;
            Some(override_vht_capabilities(vht_cap, cbw))
        }
        None => None,
    };
    Ok((ht_cap, vht_cap))
}

/// Even though hardware may support higher channel bandwidth, if user specifies a narrower
/// bandwidth, change the channel bandwidth in ht_cap_info to match user's preference.
fn override_ht_capabilities(mut ht_cap: HtCapabilities, cbw: Cbw) -> HtCapabilities {
    let mut ht_cap_info = ht_cap.ht_cap_info.with_tx_stbc(OVERRIDE_HT_CAP_INFO_TX_STBC);
    match cbw {
        Cbw::Cbw20 => ht_cap_info.set_chan_width_set(wlan_common::ie::ChanWidthSet::TWENTY_ONLY),
        _ => (),
    }
    ht_cap.ht_cap_info = ht_cap_info;
    ht_cap
}

/// Even though hardware may support higher channel bandwidth, if user specifies a narrower
/// bandwidth, change the channel bandwidth in vht_cap_info to match user's preference.
fn override_vht_capabilities(mut vht_cap: VhtCapabilities, cbw: Cbw) -> VhtCapabilities {
    let mut vht_cap_info = vht_cap.vht_cap_info;
    if vht_cap_info.supported_cbw_set() != OVERRIDE_VHT_CAP_INFO_SUPPORTED_CBW_SET {
        // Supported channel bandwidth set can only be non-zero if the associating channel is
        // 160 MHz or 80+80 MHz Channel bandwidth. Otherwise it will be set to 0. 0 is a purely
        // numeric value without a name. See IEEE Std 802.11-2016 Table 9-250 for more details.
        // TODO(fxbug.dev/39546): finer control over CBW if necessary.
        match cbw {
            Cbw::Cbw160 | Cbw::Cbw80P80 { secondary80: _ } => (),
            _ => vht_cap_info.set_supported_cbw_set(OVERRIDE_VHT_CAP_INFO_SUPPORTED_CBW_SET),
        }
    }
    vht_cap.vht_cap_info = vht_cap_info;
    vht_cap
}

#[cfg(test)]
mod tests {
    use {super::*, wlan_common::ie};

    #[test]
    fn test_build_cap_info() {
        let capability_info = CapabilityInfo(0)
            .with_ess(!OVERRIDE_CAP_INFO_ESS)
            .with_ibss(!OVERRIDE_CAP_INFO_IBSS)
            .with_cf_pollable(!OVERRIDE_CAP_INFO_CF_POLLABLE)
            .with_cf_poll_req(!OVERRIDE_CAP_INFO_CF_POLL_REQUEST)
            .with_privacy(!OVERRIDE_CAP_INFO_PRIVACY)
            .with_spectrum_mgmt(!OVERRIDE_CAP_INFO_SPECTRUM_MGMT);
        let capability_info = override_capability_info(capability_info);
        assert_eq!(capability_info.ess(), OVERRIDE_CAP_INFO_ESS);
        assert_eq!(capability_info.ibss(), OVERRIDE_CAP_INFO_IBSS);
        assert_eq!(capability_info.cf_pollable(), OVERRIDE_CAP_INFO_CF_POLLABLE);
        assert_eq!(capability_info.cf_poll_req(), OVERRIDE_CAP_INFO_CF_POLL_REQUEST);
        assert_eq!(capability_info.privacy(), OVERRIDE_CAP_INFO_PRIVACY);
        assert_eq!(capability_info.spectrum_mgmt(), OVERRIDE_CAP_INFO_SPECTRUM_MGMT);
    }

    #[test]
    fn test_override_ht_cap() {
        let mut ht_cap = ie::fake_ht_capabilities();
        let ht_cap_info = ht_cap
            .ht_cap_info
            .with_tx_stbc(!OVERRIDE_HT_CAP_INFO_TX_STBC)
            .with_chan_width_set(ie::ChanWidthSet::TWENTY_FORTY);
        ht_cap.ht_cap_info = ht_cap_info;
        let mut channel = Channel { primary: 153, cbw: Cbw::Cbw20 };

        let ht_cap_info = override_ht_capabilities(ht_cap, channel.cbw).ht_cap_info;
        assert_eq!(ht_cap_info.tx_stbc(), OVERRIDE_HT_CAP_INFO_TX_STBC);
        assert_eq!(ht_cap_info.chan_width_set(), ie::ChanWidthSet::TWENTY_ONLY);

        channel.cbw = Cbw::Cbw40;
        let ht_cap_info = override_ht_capabilities(ht_cap, channel.cbw).ht_cap_info;
        assert_eq!(ht_cap_info.chan_width_set(), ie::ChanWidthSet::TWENTY_FORTY);
    }

    #[test]
    fn test_override_vht_cap() {
        let mut vht_cap = ie::fake_vht_capabilities();
        let vht_cap_info = vht_cap.vht_cap_info.with_supported_cbw_set(2);
        vht_cap.vht_cap_info = vht_cap_info;
        let mut channel = Channel { primary: 153, cbw: Cbw::Cbw20 };

        // CBW20, CBW40, CBW80 will set supported_cbw_set to 0

        let vht_cap_info = override_vht_capabilities(vht_cap, channel.cbw).vht_cap_info;
        assert_eq!(vht_cap_info.supported_cbw_set(), OVERRIDE_VHT_CAP_INFO_SUPPORTED_CBW_SET);

        channel.cbw = Cbw::Cbw40;
        let vht_cap_info = override_vht_capabilities(vht_cap, channel.cbw).vht_cap_info;
        assert_eq!(vht_cap_info.supported_cbw_set(), OVERRIDE_VHT_CAP_INFO_SUPPORTED_CBW_SET);

        channel.cbw = Cbw::Cbw80;
        let vht_cap_info = override_vht_capabilities(vht_cap, channel.cbw).vht_cap_info;
        assert_eq!(vht_cap_info.supported_cbw_set(), OVERRIDE_VHT_CAP_INFO_SUPPORTED_CBW_SET);

        // CBW160 and CBW80P80 will preserve existing supported_cbw_set value

        channel.cbw = Cbw::Cbw160;
        let vht_cap_info = override_vht_capabilities(vht_cap, channel.cbw).vht_cap_info;
        assert_eq!(vht_cap_info.supported_cbw_set(), 2);

        channel.cbw = Cbw::Cbw80P80 { secondary80: 42 };
        let vht_cap_info = override_vht_capabilities(vht_cap, channel.cbw).vht_cap_info;
        assert_eq!(vht_cap_info.supported_cbw_set(), 2);
    }
}
