// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_WLANIF_CONVERT_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_WLANIF_CONVERT_H_

#include <fuchsia/hardware/wlan/fullmac/c/banjo.h>
#include <fuchsia/wlan/common/c/banjo.h>
#include <fuchsia/wlan/internal/c/banjo.h>
#include <fuchsia/wlan/internal/cpp/fidl.h>
#include <fuchsia/wlan/mlme/cpp/fidl.h>
#include <net/ethernet.h>

namespace wlanif {

void CloneIntoCSsid(const ::std::vector<uint8_t>& ssid, cssid_t& out_cssid);
uint8_t ConvertScanType(::fuchsia::wlan::mlme::ScanTypes scan_type);
uint8_t ConvertCBW(::fuchsia::wlan::common::ChannelBandwidth cbw);
void ConvertWlanChan(wlan_channel_t* wlan_fullmac_channel,
                     const ::fuchsia::wlan::common::WlanChannel& fidl_channel);
void ConvertWlanChan(::fuchsia::wlan::common::WlanChannel* fidl_channel,
                     const wlan_channel_t& wlan_fullmac_channel);
void CopyRSNE(const ::std::vector<uint8_t>& in_rsne, uint8_t* out_rsne, size_t* out_rsne_len);
void CopyVendorSpecificIE(const ::std::vector<uint8_t>& in_vendor_ie, uint8_t* out_vendor_ie,
                          size_t* out_vendor_ie_len);
void ConvertBssDescription(bss_description_t* wlan_fullmac_bss_desc,
                           const ::fuchsia::wlan::internal::BssDescription& fidl_bss_desc);
void ConvertBssDescription(::fuchsia::wlan::internal::BssDescription* fidl_bss_desc,
                           const bss_description_t& wlan_fullmac_bss_desc);
void ConvertAssocInd(::fuchsia::wlan::mlme::AssociateIndication* fidl_ind,
                     const wlan_fullmac_assoc_ind_t& assoc_ind);
void ConvertEapolConf(::fuchsia::wlan::mlme::EapolConfirm* fidl_resp,
                      const wlan_fullmac_eapol_confirm_t& eapol_conf);
uint8_t ConvertAuthType(::fuchsia::wlan::mlme::AuthenticationTypes auth_type);
uint8_t ConvertKeyType(::fuchsia::wlan::mlme::KeyType key_type);
void ConvertSetKeyDescriptor(set_key_descriptor_t* key_desc,
                             const ::fuchsia::wlan::mlme::SetKeyDescriptor& fidl_key_desc);
void ConvertDeleteKeyDescriptor(delete_key_descriptor_t* key_desc,
                                const ::fuchsia::wlan::mlme::DeleteKeyDescriptor& fidl_key_desc);
::fuchsia::wlan::internal::BssType ConvertBssType(uint8_t bss_type);
::fuchsia::wlan::common::ChannelBandwidth ConvertCBW(channel_bandwidth_t cbw);
::fuchsia::wlan::mlme::AuthenticationTypes ConvertAuthType(uint8_t auth_type);
::fuchsia::wlan::mlme::ScanResultCode ConvertScanResultCode(uint8_t code);
::fuchsia::wlan::mlme::AuthenticateResultCode ConvertAuthResultCode(uint8_t code);
uint8_t ConvertAuthResultCode(::fuchsia::wlan::mlme::AuthenticateResultCode result_code);
::fuchsia::wlan::mlme::AssociateResultCode ConvertAssocResultCode(uint8_t code);
uint8_t ConvertAssocResultCode(::fuchsia::wlan::mlme::AssociateResultCode code);
::fuchsia::wlan::mlme::StartResultCode ConvertStartResultCode(uint8_t code);
::fuchsia::wlan::mlme::StopResultCode ConvertStopResultCode(uint8_t code);
::fuchsia::wlan::mlme::EapolResultCode ConvertEapolResultCode(uint8_t code);
::fuchsia::wlan::common::WlanMacRole ConvertMacRole(wlan_mac_role_t role);
void ConvertBandCapability(::fuchsia::wlan::mlme::BandCapability* fidl_band,
                           const wlan_fullmac_band_capability_t& band);
void ConvertQueryInfoToDeviceInfo(::fuchsia::wlan::mlme::DeviceInfo* fidl_device_info,
                                  const wlan_fullmac_query_info_t& query_info);
// Convert a Banjo noise floor histogram into FIDL.
void ConvertNoiseFloorHistogram(::fuchsia::wlan::stats::NoiseFloorHistogram* fidl_stats,
                                const wlan_fullmac_noise_floor_histogram_t& stats);
// Convert a Banjo received rate index histogram into FIDL.
void ConvertRxRateIndexHistogram(::fuchsia::wlan::stats::RxRateIndexHistogram* fidl_stats,
                                 const wlan_fullmac_rx_rate_index_histogram_t& stats);
// Convert a Banjo received signal strength indicator (RSSI) histogram into FIDL.
void ConvertRssiHistogram(::fuchsia::wlan::stats::RssiHistogram* fidl_stats,
                          const wlan_fullmac_rssi_histogram_t& stats);
// Convert a Banjo signal to noise ratio (SNR) histogram into FIDL.
void ConvertSnrHistogram(::fuchsia::wlan::stats::SnrHistogram* fidl_stats,
                         const wlan_fullmac_snr_histogram_t& stats);
void ConvertPmkInfo(::fuchsia::wlan::mlme::PmkInfo* fidl_ind, const wlan_fullmac_pmk_info_t& ind);

void ConvertIfaceCounterStats(::fuchsia::wlan::stats::IfaceCounterStats* fidl_stats,
                              const wlan_fullmac_iface_counter_stats_t& stats);
void ConvertIfaceHistogramStats(::fuchsia::wlan::stats::IfaceHistogramStats* fidl_stats,
                                const wlan_fullmac_iface_histogram_stats_t& stats);
uint32_t ConvertMgmtCaptureFlags(::fuchsia::wlan::mlme::MgmtFrameCaptureFlags fidl_flags);
::fuchsia::wlan::mlme::MgmtFrameCaptureFlags ConvertMgmtCaptureFlags(uint32_t ddk_flags);
void ConvertRates(::std::vector<uint8_t>* rates, const bss_description_t& banjo_desc);

void ConvertSaeAuthFrame(const ::fuchsia::wlan::mlme::SaeFrame& frame_in,
                         wlan_fullmac_sae_frame_t* frame_out);

void ConvertSaeAuthFrame(const wlan_fullmac_sae_frame_t* frame_in,
                         ::fuchsia::wlan::mlme::SaeFrame& frame_out);
void ConvertWmmStatus(const wlan_wmm_params_t* params_in,
                      ::fuchsia::wlan::internal::WmmStatusResponse* resp);

// The following functions are only exposed for convenience in unit tests.
void ConvertHtCapabilities(::fuchsia::wlan::ieee80211::HtCapabilities* fidl_ht_cap,
                           const ht_capabilities_t& ht_cap);
void ConvertVhtCapabilities(::fuchsia::wlan::ieee80211::VhtCapabilities* fidl_vht_cap,
                            const vht_capabilities_t& vht_cap);

}  // namespace wlanif

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_WLANIF_CONVERT_H_
