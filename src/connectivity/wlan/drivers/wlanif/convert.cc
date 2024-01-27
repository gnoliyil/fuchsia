// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "convert.h"

#include <fuchsia/wlan/common/c/banjo.h>
#include <fuchsia/wlan/ieee80211/cpp/fidl.h>
#include <fuchsia/wlan/internal/c/banjo.h>
#include <fuchsia/wlan/internal/cpp/fidl.h>
#include <fuchsia/wlan/stats/cpp/fidl.h>
#include <zircon/status.h>

#include <algorithm>
#include <bitset>
#include <memory>

#include <wlan/common/band.h>
#include <wlan/common/ieee80211_codes.h>
#include <wlan/drivers/log.h>

#include "debug.h"

namespace wlanif {

namespace wlan_common = ::fuchsia::wlan::common;
namespace wlan_ieee80211 = ::fuchsia::wlan::ieee80211;
namespace wlan_internal = ::fuchsia::wlan::internal;
namespace wlan_mlme = ::fuchsia::wlan::mlme;
namespace wlan_stats = ::fuchsia::wlan::stats;

// This function is only guaranteed to produce a valid fuchsia.wlan.ieee80211/CSsid,
// i.e., an SSID with a valid byte length, if the input argument came from a
// fuchsia.wlan.ieee80211/Ssid.
void CloneIntoCSsid(const ::std::vector<uint8_t>& ssid, cssid_t& out_cssid) {
  // fuchsia.wlan.ieee80211/Ssid is guaranteed to have no more than 32 bytes,
  // so its size will fit in a uint8_t.
  out_cssid.len = static_cast<uint8_t>(ssid.size());
  memcpy(&out_cssid.data, ssid.data(), out_cssid.len);
}

uint8_t ConvertScanType(wlan_mlme::ScanTypes scan_type) {
  switch (scan_type) {
    case wlan_mlme::ScanTypes::ACTIVE:
      return WLAN_SCAN_TYPE_ACTIVE;
    case wlan_mlme::ScanTypes::PASSIVE:
      return WLAN_SCAN_TYPE_PASSIVE;
    default:
      lerror("bad scan type: %u\n", scan_type);
      ZX_ASSERT(0);
  }
}

uint8_t ConvertCBW(wlan_common::ChannelBandwidth cbw) {
  switch (cbw) {
    case wlan_common::ChannelBandwidth::CBW20:
      return CHANNEL_BANDWIDTH_CBW20;
    case wlan_common::ChannelBandwidth::CBW40:
      return CHANNEL_BANDWIDTH_CBW40;
    case wlan_common::ChannelBandwidth::CBW40BELOW:
      return CHANNEL_BANDWIDTH_CBW40BELOW;
    case wlan_common::ChannelBandwidth::CBW80:
      return CHANNEL_BANDWIDTH_CBW80;
    case wlan_common::ChannelBandwidth::CBW160:
      return CHANNEL_BANDWIDTH_CBW160;
    case wlan_common::ChannelBandwidth::CBW80P80:
      return CHANNEL_BANDWIDTH_CBW80P80;
  }
  lerror("bad channel bandwidth: %u\n", cbw);
  ZX_ASSERT(0);
}

void ConvertWlanChan(wlan_channel_t* wlan_fullmac_channel,
                     const wlan_common::WlanChannel& fidl_channel) {
  // primary
  wlan_fullmac_channel->primary = fidl_channel.primary;

  // CBW
  wlan_fullmac_channel->cbw = ConvertCBW(fidl_channel.cbw);

  // secondary80
  wlan_fullmac_channel->secondary80 = fidl_channel.secondary80;
}

void CopyCountry(const ::std::vector<uint8_t>& in_country, uint8_t* out_country,
                 size_t* out_country_len) {
  if (in_country.size() > wlan_ieee80211::WLAN_IE_BODY_MAX_LEN) {
    lwarn("Country length truncated from %lu to %d\n", in_country.size(),
          wlan_ieee80211::WLAN_IE_BODY_MAX_LEN);
    *out_country_len = wlan_ieee80211::WLAN_IE_BODY_MAX_LEN;
  } else {
    *out_country_len = in_country.size();
  }
  std::memcpy(out_country, in_country.data(), *out_country_len);
}

void CopyRSNE(const ::std::vector<uint8_t>& in_rsne, uint8_t* out_rsne, size_t* out_rsne_len) {
  if (in_rsne.size() > wlan_ieee80211::WLAN_IE_BODY_MAX_LEN) {
    lwarn("RSNE length truncated from %lu to %d\n", in_rsne.size(),
          wlan_ieee80211::WLAN_IE_BODY_MAX_LEN);
    *out_rsne_len = wlan_ieee80211::WLAN_IE_BODY_MAX_LEN;
  } else {
    *out_rsne_len = in_rsne.size();
  }
  std::memcpy(out_rsne, in_rsne.data(), *out_rsne_len);
}

void CopyVendorSpecificIE(const ::std::vector<uint8_t>& in_vendor_specific,
                          uint8_t* out_vendor_specific, size_t* vendor_specific_len) {
  if (in_vendor_specific.size() > WLAN_VIE_MAX_LEN) {
    lwarn("Vendor Specific length truncated from %lu to %d\n", in_vendor_specific.size(),
          WLAN_VIE_MAX_LEN);
    *vendor_specific_len = WLAN_VIE_MAX_LEN;
  } else {
    *vendor_specific_len = in_vendor_specific.size();
  }
  std::memcpy(out_vendor_specific, in_vendor_specific.data(), *vendor_specific_len);
}

void ConvertBssDescription(bss_description_t* banjo_desc,
                           const wlan_internal::BssDescription& fidl_desc) {
  // bssid
  std::memcpy(banjo_desc->bssid, fidl_desc.bssid.data(), ETH_ALEN);

  // bss_type
  banjo_desc->bss_type = static_cast<bss_type_t>(fidl_desc.bss_type);

  // beacon_period
  banjo_desc->beacon_period = fidl_desc.beacon_period;

  // capability
  banjo_desc->capability_info = fidl_desc.capability_info;

  // ies
  banjo_desc->ies_list = fidl_desc.ies.data();
  banjo_desc->ies_count = fidl_desc.ies.size();

  // channel
  ConvertWlanChan(&banjo_desc->channel, fidl_desc.channel);

  // rssi_dbm
  banjo_desc->rssi_dbm = fidl_desc.rssi_dbm;

  // snr_db
  banjo_desc->snr_db = fidl_desc.snr_db;
}

wlan_internal::BssType ConvertBssType(uint8_t bss_type) {
  switch (bss_type) {
    case BSS_TYPE_INFRASTRUCTURE:
      return wlan_internal::BssType::INFRASTRUCTURE;
    case BSS_TYPE_PERSONAL:
      return wlan_internal::BssType::PERSONAL;
    case BSS_TYPE_INDEPENDENT:
      return wlan_internal::BssType::INDEPENDENT;
    case BSS_TYPE_MESH:
      return wlan_internal::BssType::MESH;
    default:
      lerror("bad bss type: %u\n", bss_type);
      ZX_ASSERT(0);
  }
}

wlan_common::ChannelBandwidth ConvertCBW(channel_bandwidth_t cbw) {
  switch (cbw) {
    case CHANNEL_BANDWIDTH_CBW20:
      return wlan_common::ChannelBandwidth::CBW20;
    case CHANNEL_BANDWIDTH_CBW40:
      return wlan_common::ChannelBandwidth::CBW40;
    case CHANNEL_BANDWIDTH_CBW40BELOW:
      return wlan_common::ChannelBandwidth::CBW40BELOW;
    case CHANNEL_BANDWIDTH_CBW80:
      return wlan_common::ChannelBandwidth::CBW80;
    case CHANNEL_BANDWIDTH_CBW160:
      return wlan_common::ChannelBandwidth::CBW160;
    case CHANNEL_BANDWIDTH_CBW80P80:
      return wlan_common::ChannelBandwidth::CBW80P80;
    default:
      lerror("bad channel bandwidth: %u\n", cbw);
      ZX_ASSERT(0);
  }
}

void ConvertWlanChan(wlan_common::WlanChannel* fidl_channel,
                     const wlan_channel_t& wlan_fullmac_channel) {
  // primary
  fidl_channel->primary = wlan_fullmac_channel.primary;

  // CBW
  fidl_channel->cbw = ConvertCBW(wlan_fullmac_channel.cbw);

  // secondary80
  fidl_channel->secondary80 = wlan_fullmac_channel.secondary80;
}

void ConvertBssDescription(wlan_internal::BssDescription* fidl_desc,
                           const bss_description_t& banjo_desc) {
  // bssid
  std::memcpy(fidl_desc->bssid.data(), banjo_desc.bssid, ETH_ALEN);

  // bss_type
  fidl_desc->bss_type = ConvertBssType(banjo_desc.bss_type);

  // beacon_period
  fidl_desc->beacon_period = banjo_desc.beacon_period;

  // capability
  fidl_desc->capability_info = banjo_desc.capability_info;

  // ies
  if (banjo_desc.ies_count > 0) {
    fidl_desc->ies =
        std::vector<uint8_t>(banjo_desc.ies_list, banjo_desc.ies_list + banjo_desc.ies_count);
  }

  // channel
  ConvertWlanChan(&fidl_desc->channel, banjo_desc.channel);

  // rssi_dbm
  fidl_desc->rssi_dbm = banjo_desc.rssi_dbm;

  // snr_db
  fidl_desc->snr_db = banjo_desc.snr_db;
}

void ConvertAssocInd(wlan_mlme::AssociateIndication* fidl_ind,
                     const wlan_fullmac_assoc_ind_t& assoc_ind) {
  *fidl_ind = {};
  // peer_sta_address
  std::memcpy(fidl_ind->peer_sta_address.data(), assoc_ind.peer_sta_address, ETH_ALEN);

  // listen_interval
  fidl_ind->listen_interval = assoc_ind.listen_interval;

  // ssid
  if (assoc_ind.ssid.len) {
    fidl_ind->ssid = {
        std::vector<uint8_t>(assoc_ind.ssid.data, assoc_ind.ssid.data + assoc_ind.ssid.len)};
  }
  // rsne
  bool is_protected = assoc_ind.rsne_len != 0;
  if (is_protected) {
    fidl_ind->rsne = {std::vector<uint8_t>(assoc_ind.rsne, assoc_ind.rsne + assoc_ind.rsne_len)};
  }
}

void ConvertEapolConf(::fuchsia::wlan::mlme::EapolConfirm* fidl_resp,
                      const wlan_fullmac_eapol_confirm_t& eapol_conf) {
  // result_code
  fidl_resp->result_code = ConvertEapolResultCode(eapol_conf.result_code);

  std::memcpy(fidl_resp->dst_addr.data(), eapol_conf.dst_addr, ETH_ALEN);
}

uint8_t ConvertAuthType(wlan_mlme::AuthenticationTypes auth_type) {
  switch (auth_type) {
    case wlan_mlme::AuthenticationTypes::OPEN_SYSTEM:
      return WLAN_AUTH_TYPE_OPEN_SYSTEM;
    case wlan_mlme::AuthenticationTypes::SHARED_KEY:
      return WLAN_AUTH_TYPE_SHARED_KEY;
    case wlan_mlme::AuthenticationTypes::FAST_BSS_TRANSITION:
      return WLAN_AUTH_TYPE_FAST_BSS_TRANSITION;
    case wlan_mlme::AuthenticationTypes::SAE:
      return WLAN_AUTH_TYPE_SAE;
    default:
      lerror("bad auth type: %u\n", auth_type);
      ZX_ASSERT(0);
  }
}

uint8_t ConvertKeyType(wlan_mlme::KeyType key_type) {
  switch (key_type) {
    case wlan_mlme::KeyType::GROUP:
      return WLAN_KEY_TYPE_GROUP;
    case wlan_mlme::KeyType::PAIRWISE:
      return WLAN_KEY_TYPE_PAIRWISE;
    case wlan_mlme::KeyType::PEER_KEY:
      return WLAN_KEY_TYPE_PEER;
    case wlan_mlme::KeyType::IGTK:
      return WLAN_KEY_TYPE_IGTK;
    default:
      lerror("bad key type: %u\n", key_type);
      ZX_ASSERT(0);
  }
}

void ConvertSetKeyDescriptor(set_key_descriptor_t* key_desc,
                             const wlan_mlme::SetKeyDescriptor& fidl_key_desc) {
  // key
  key_desc->key_list = const_cast<uint8_t*>(fidl_key_desc.key.data());

  // length
  key_desc->key_count = fidl_key_desc.key.size();

  // key_id
  key_desc->key_id = fidl_key_desc.key_id;

  // key_type
  key_desc->key_type = ConvertKeyType(fidl_key_desc.key_type);

  // address
  std::memcpy(key_desc->address, fidl_key_desc.address.data(), ETH_ALEN);

  // rsc
  key_desc->rsc = fidl_key_desc.rsc;

  // cipher_suite_oui
  std::memcpy(key_desc->cipher_suite_oui, fidl_key_desc.cipher_suite_oui.data(),
              sizeof(key_desc->cipher_suite_oui));

  // cipher_suite_type
  key_desc->cipher_suite_type = fidl_key_desc.cipher_suite_type;
}

void ConvertDeleteKeyDescriptor(delete_key_descriptor_t* key_desc,
                                const wlan_mlme::DeleteKeyDescriptor& fidl_key_desc) {
  // key_id
  key_desc->key_id = fidl_key_desc.key_id;

  // key_type
  key_desc->key_type = ConvertKeyType(fidl_key_desc.key_type);

  // address
  std::memcpy(key_desc->address, fidl_key_desc.address.data(), ETH_ALEN);
}

wlan_mlme::ScanResultCode ConvertScanResultCode(uint8_t code) {
  switch (code) {
    case WLAN_SCAN_RESULT_SUCCESS:
      return wlan_mlme::ScanResultCode::SUCCESS;
    case WLAN_SCAN_RESULT_NOT_SUPPORTED:
      return wlan_mlme::ScanResultCode::NOT_SUPPORTED;
    case WLAN_SCAN_RESULT_INVALID_ARGS:
      return wlan_mlme::ScanResultCode::INVALID_ARGS;
    case WLAN_SCAN_RESULT_INTERNAL_ERROR:
      return wlan_mlme::ScanResultCode::INTERNAL_ERROR;
    case WLAN_SCAN_RESULT_SHOULD_WAIT:
      return wlan_mlme::ScanResultCode::SHOULD_WAIT;
    case WLAN_SCAN_RESULT_CANCELED_BY_DRIVER_OR_FIRMWARE:
      return wlan_mlme::ScanResultCode::CANCELED_BY_DRIVER_OR_FIRMWARE;
    default:
      lerror("bad scan result code: %u\n", code);
      ZX_ASSERT(0);
  }
}

wlan_mlme::AuthenticationTypes ConvertAuthType(uint8_t auth_type) {
  switch (auth_type) {
    case WLAN_AUTH_TYPE_OPEN_SYSTEM:
      return wlan_mlme::AuthenticationTypes::OPEN_SYSTEM;
    case WLAN_AUTH_TYPE_SHARED_KEY:
      return wlan_mlme::AuthenticationTypes::SHARED_KEY;
    case WLAN_AUTH_TYPE_FAST_BSS_TRANSITION:
      return wlan_mlme::AuthenticationTypes::FAST_BSS_TRANSITION;
    case WLAN_AUTH_TYPE_SAE:
      return wlan_mlme::AuthenticationTypes::SAE;
    default:
      lerror("bad auth type: %u\n", auth_type);
      ZX_ASSERT(0);
  }
}

wlan_mlme::AuthenticateResultCode ConvertAuthResultCode(uint8_t code) {
  switch (code) {
    case WLAN_AUTH_RESULT_SUCCESS:
      return wlan_mlme::AuthenticateResultCode::SUCCESS;
    case WLAN_AUTH_RESULT_REFUSED:
      return wlan_mlme::AuthenticateResultCode::REFUSED;
    case WLAN_AUTH_RESULT_ANTI_CLOGGING_TOKEN_REQUIRED:
      return wlan_mlme::AuthenticateResultCode::ANTI_CLOGGING_TOKEN_REQUIRED;
    case WLAN_AUTH_RESULT_FINITE_CYCLIC_GROUP_NOT_SUPPORTED:
      return wlan_mlme::AuthenticateResultCode::FINITE_CYCLIC_GROUP_NOT_SUPPORTED;
    case WLAN_AUTH_RESULT_REJECTED:
      return wlan_mlme::AuthenticateResultCode::AUTHENTICATION_REJECTED;
    case WLAN_AUTH_RESULT_FAILURE_TIMEOUT:
      return wlan_mlme::AuthenticateResultCode::AUTH_FAILURE_TIMEOUT;
    default:
      lerror("bad auth result code: %u\n", code);
      ZX_ASSERT(0);
  }
}

uint8_t ConvertAuthResultCode(wlan_mlme::AuthenticateResultCode code) {
  switch (code) {
    case wlan_mlme::AuthenticateResultCode::SUCCESS:
      return WLAN_AUTH_RESULT_SUCCESS;
    case wlan_mlme::AuthenticateResultCode::REFUSED:
      return WLAN_AUTH_RESULT_REFUSED;
    case wlan_mlme::AuthenticateResultCode::ANTI_CLOGGING_TOKEN_REQUIRED:
      return WLAN_AUTH_RESULT_ANTI_CLOGGING_TOKEN_REQUIRED;
    case wlan_mlme::AuthenticateResultCode::FINITE_CYCLIC_GROUP_NOT_SUPPORTED:
      return WLAN_AUTH_RESULT_FINITE_CYCLIC_GROUP_NOT_SUPPORTED;
    case wlan_mlme::AuthenticateResultCode::AUTHENTICATION_REJECTED:
      return WLAN_AUTH_RESULT_REJECTED;
    case wlan_mlme::AuthenticateResultCode::AUTH_FAILURE_TIMEOUT:
      return WLAN_AUTH_RESULT_FAILURE_TIMEOUT;
    default:
      lerror("bad auth result code: %u\n", code);
      ZX_ASSERT(0);
  }
}

wlan_mlme::AssociateResultCode ConvertAssocResultCode(uint8_t code) {
  switch (code) {
    case WLAN_ASSOC_RESULT_SUCCESS:
      return wlan_mlme::AssociateResultCode::SUCCESS;
    case WLAN_ASSOC_RESULT_REFUSED_REASON_UNSPECIFIED:
      return wlan_mlme::AssociateResultCode::REFUSED_REASON_UNSPECIFIED;
    case WLAN_ASSOC_RESULT_REFUSED_NOT_AUTHENTICATED:
      return wlan_mlme::AssociateResultCode::REFUSED_NOT_AUTHENTICATED;
    case WLAN_ASSOC_RESULT_REFUSED_CAPABILITIES_MISMATCH:
      return wlan_mlme::AssociateResultCode::REFUSED_CAPABILITIES_MISMATCH;
    case WLAN_ASSOC_RESULT_REFUSED_EXTERNAL_REASON:
      return wlan_mlme::AssociateResultCode::REFUSED_EXTERNAL_REASON;
    case WLAN_ASSOC_RESULT_REFUSED_AP_OUT_OF_MEMORY:
      return wlan_mlme::AssociateResultCode::REFUSED_AP_OUT_OF_MEMORY;
    case WLAN_ASSOC_RESULT_REFUSED_BASIC_RATES_MISMATCH:
      return wlan_mlme::AssociateResultCode::REFUSED_BASIC_RATES_MISMATCH;
    case WLAN_ASSOC_RESULT_REJECTED_EMERGENCY_SERVICES_NOT_SUPPORTED:
      return wlan_mlme::AssociateResultCode::REJECTED_EMERGENCY_SERVICES_NOT_SUPPORTED;
    case WLAN_ASSOC_RESULT_REFUSED_TEMPORARILY:
      return wlan_mlme::AssociateResultCode::REFUSED_TEMPORARILY;
    default:
      lerror("bad assoc result code: %u\n", code);
      ZX_ASSERT(0);
  }
}

uint8_t ConvertAssocResultCode(wlan_mlme::AssociateResultCode code) {
  switch (code) {
    case wlan_mlme::AssociateResultCode::SUCCESS:
      return WLAN_ASSOC_RESULT_SUCCESS;
    case wlan_mlme::AssociateResultCode::REFUSED_REASON_UNSPECIFIED:
      return WLAN_ASSOC_RESULT_REFUSED_REASON_UNSPECIFIED;
    case wlan_mlme::AssociateResultCode::REFUSED_NOT_AUTHENTICATED:
      return WLAN_ASSOC_RESULT_REFUSED_NOT_AUTHENTICATED;
    case wlan_mlme::AssociateResultCode::REFUSED_CAPABILITIES_MISMATCH:
      return WLAN_ASSOC_RESULT_REFUSED_CAPABILITIES_MISMATCH;
    case wlan_mlme::AssociateResultCode::REFUSED_EXTERNAL_REASON:
      return WLAN_ASSOC_RESULT_REFUSED_EXTERNAL_REASON;
    case wlan_mlme::AssociateResultCode::REFUSED_AP_OUT_OF_MEMORY:
      return WLAN_ASSOC_RESULT_REFUSED_AP_OUT_OF_MEMORY;
    case wlan_mlme::AssociateResultCode::REFUSED_BASIC_RATES_MISMATCH:
      return WLAN_ASSOC_RESULT_REFUSED_BASIC_RATES_MISMATCH;
    case wlan_mlme::AssociateResultCode::REJECTED_EMERGENCY_SERVICES_NOT_SUPPORTED:
      return WLAN_ASSOC_RESULT_REJECTED_EMERGENCY_SERVICES_NOT_SUPPORTED;
    case wlan_mlme::AssociateResultCode::REFUSED_TEMPORARILY:
      return WLAN_ASSOC_RESULT_REFUSED_TEMPORARILY;
    default:
      lerror("bad assoc result code: %u\n", code);
      ZX_ASSERT(0);
  }
}

wlan_mlme::StartResultCode ConvertStartResultCode(uint8_t code) {
  switch (code) {
    case WLAN_START_RESULT_SUCCESS:
      return wlan_mlme::StartResultCode::SUCCESS;
    case WLAN_START_RESULT_BSS_ALREADY_STARTED_OR_JOINED:
      return wlan_mlme::StartResultCode::BSS_ALREADY_STARTED_OR_JOINED;
    case WLAN_START_RESULT_RESET_REQUIRED_BEFORE_START:
      return wlan_mlme::StartResultCode::RESET_REQUIRED_BEFORE_START;
    case WLAN_START_RESULT_NOT_SUPPORTED:
      return wlan_mlme::StartResultCode::NOT_SUPPORTED;
    default:
      lerror("bad start result code: %u\n", code);
      ZX_ASSERT(0);
  }
}

wlan_mlme::StopResultCode ConvertStopResultCode(uint8_t code) {
  switch (code) {
    case WLAN_STOP_RESULT_SUCCESS:
      return wlan_mlme::StopResultCode::SUCCESS;
    case WLAN_STOP_RESULT_BSS_ALREADY_STOPPED:
      return wlan_mlme::StopResultCode::BSS_ALREADY_STOPPED;
    case WLAN_STOP_RESULT_INTERNAL_ERROR:
      return wlan_mlme::StopResultCode::INTERNAL_ERROR;
    default:
      lerror("bad stop result code: %u\n", code);
      ZX_ASSERT(0);
  }
}

wlan_mlme::EapolResultCode ConvertEapolResultCode(uint8_t code) {
  switch (code) {
    case WLAN_EAPOL_RESULT_SUCCESS:
      return wlan_mlme::EapolResultCode::SUCCESS;
    case WLAN_EAPOL_RESULT_TRANSMISSION_FAILURE:
      return wlan_mlme::EapolResultCode::TRANSMISSION_FAILURE;
    default:
      lerror("bad EAPOL result code: %u\n", code);
      ZX_ASSERT(0);
  }
}

wlan_common::WlanMacRole ConvertMacRole(wlan_mac_role_t role) {
  switch (role) {
    case WLAN_MAC_ROLE_CLIENT:
      return wlan_common::WlanMacRole::CLIENT;
    case WLAN_MAC_ROLE_AP:
      return wlan_common::WlanMacRole::AP;
    case WLAN_MAC_ROLE_MESH:
      return wlan_common::WlanMacRole::MESH;
    default:
      lerror("bad wlan_mac_role_t: %u\n", role);
      ZX_ASSERT(0);
  }
}

void ConvertHtCapabilities(wlan_ieee80211::HtCapabilities* fidl_ht_cap,
                           const ht_capabilities_t& ht_cap) {
  *fidl_ht_cap = {};
  static_assert(sizeof(fidl_ht_cap->bytes) == sizeof(ht_cap));
  // TODO(fxbug.dev/95240): The underlying bytes in
  // fuchsia.hardware.wlan.fullmac/HtCapabilitiesField are @packed so that copying them directly to
  // a byte array works. We may wish to change the FIDL definition in the future so this copy is
  // however more obviously correct.
  memcpy(fidl_ht_cap->bytes.data(), &ht_cap, sizeof(ht_cap));
}

void ConvertVhtCapabilities(wlan_ieee80211::VhtCapabilities* fidl_vht_cap,
                            const vht_capabilities_t& vht_cap) {
  *fidl_vht_cap = {};
  static_assert(sizeof(fidl_vht_cap->bytes) == sizeof(vht_cap));
  // TODO(fxbug.dev/95240): The underlying bytes in
  // fuchsia.hardware.wlan.fullmac/VhtCapabilitiesField are @packed so that copying them directly to
  // a byte array works. We may wish to change the definition in the future so this copy is however
  // more obviously correct.
  memcpy(fidl_vht_cap->bytes.data(), &vht_cap, sizeof(vht_cap));
}

void ConvertBandCapability(wlan_mlme::BandCapability* fidl_band,
                           const wlan_fullmac_band_capability_t& band) {
  zx_status_t status = ::wlan::common::ToFidl(&fidl_band->band, band.band);
  if (status != ZX_OK) {
    lerror("bad wlan_band_t: status %s, band %u\n", zx_status_get_string(status), band.band);
    ZX_ASSERT(0);
  }

  // basic_rates
  fidl_band->basic_rates.assign(band.basic_rate_list, band.basic_rate_list + band.basic_rate_count);

  // channels
  fidl_band->operating_channels.assign(band.operating_channel_list,
                                       band.operating_channel_list + band.operating_channel_count);

  if (band.ht_supported) {
    fidl_band->ht_cap = std::make_unique<wlan_ieee80211::HtCapabilities>();
    ConvertHtCapabilities(fidl_band->ht_cap.get(), band.ht_caps);
  }

  if (band.vht_supported) {
    fidl_band->vht_cap = std::make_unique<wlan_ieee80211::VhtCapabilities>();
    ConvertVhtCapabilities(fidl_band->vht_cap.get(), band.vht_caps);
  }
}

void ConvertQueryInfoToDeviceInfo(wlan_mlme::DeviceInfo* fidl_device_info,
                                  const wlan_fullmac_query_info_t& query_info) {
  *fidl_device_info = {};

  std::memcpy(fidl_device_info->sta_addr.data(), query_info.sta_addr, ETH_ALEN);

  fidl_device_info->role = ConvertMacRole(query_info.role);

  // fuchsia.hardware.wlan.fullmac/WlanFullmacQueryInfo.features is not copied
  // up to fuchsia.wlan.mlme/DeviceInfo.

  fidl_device_info->bands.resize(query_info.band_cap_count);
  for (size_t i = 0; i < query_info.band_cap_count; ++i) {
    ConvertBandCapability(&fidl_device_info->bands[i], query_info.band_cap_list[i]);
  }

  // TODO(fxbug.dev/88315): This field will be replaced in the new driver features
  // framework.
  fidl_device_info->softmac_hardware_capability = 0;
  // TODO(fxbug.dev/43938): This field is stubbed out for future use.
  fidl_device_info->qos_capable = false;
}

namespace {

// Convert a Banjo antenna ID to a FIDL antenna ID (in a unique_ptr).
std::unique_ptr<wlan_stats::AntennaId> ConvertAntennaId(
    const wlan_fullmac_antenna_id_t& impl_antenna_id) {
  auto fidl_antenna_id = std::make_unique<wlan_stats::AntennaId>();
  if (impl_antenna_id.freq == WLAN_FULLMAC_ANTENNA_FREQ_ANTENNA_5_G) {
    fidl_antenna_id->freq = wlan_stats::AntennaFreq::ANTENNA_5_G;
  } else {
    fidl_antenna_id->freq = wlan_stats::AntennaFreq::ANTENNA_2_G;
  }
  fidl_antenna_id->index = impl_antenna_id.index;
  return fidl_antenna_id;
}

}  // namespace

void ConvertNoiseFloorHistogram(wlan_stats::NoiseFloorHistogram* fidl_stats,
                                const wlan_fullmac_noise_floor_histogram_t& stats) {
  if (stats.hist_scope == WLAN_FULLMAC_HIST_SCOPE_PER_ANTENNA) {
    fidl_stats->hist_scope = wlan_stats::HistScope::PER_ANTENNA;
    fidl_stats->antenna_id = ConvertAntennaId(stats.antenna_id);
  } else {
    fidl_stats->hist_scope = wlan_stats::HistScope::STATION;
  }
  for (size_t i = 0; i < stats.noise_floor_samples_count; ++i) {
    const auto& bucket = stats.noise_floor_samples_list[i];
    if (bucket.num_samples == 0) {
      // To keep the FIDL histogram compact, we don't add empty buckets.
      continue;
    }
    wlan_stats::HistBucket fidl_bucket;
    fidl_bucket.bucket_index = bucket.bucket_index;
    fidl_bucket.num_samples = bucket.num_samples;
    fidl_stats->noise_floor_samples.push_back(fidl_bucket);
  }
  fidl_stats->invalid_samples = stats.invalid_samples;
}

void ConvertRxRateIndexHistogram(wlan_stats::RxRateIndexHistogram* fidl_stats,
                                 const wlan_fullmac_rx_rate_index_histogram_t& stats) {
  if (stats.hist_scope == WLAN_FULLMAC_HIST_SCOPE_PER_ANTENNA) {
    fidl_stats->hist_scope = wlan_stats::HistScope::PER_ANTENNA;
    fidl_stats->antenna_id = ConvertAntennaId(stats.antenna_id);
  } else {
    fidl_stats->hist_scope = wlan_stats::HistScope::STATION;
  }
  for (size_t i = 0; i < stats.rx_rate_index_samples_count; ++i) {
    const auto& bucket = stats.rx_rate_index_samples_list[i];
    if (bucket.num_samples == 0) {
      continue;
    }
    wlan_stats::HistBucket fidl_bucket;
    fidl_bucket.bucket_index = bucket.bucket_index;
    fidl_bucket.num_samples = bucket.num_samples;
    fidl_stats->rx_rate_index_samples.push_back(fidl_bucket);
  }
  fidl_stats->invalid_samples = stats.invalid_samples;
}

void ConvertRssiHistogram(wlan_stats::RssiHistogram* fidl_stats,
                          const wlan_fullmac_rssi_histogram_t& stats) {
  if (stats.hist_scope == WLAN_FULLMAC_HIST_SCOPE_PER_ANTENNA) {
    fidl_stats->hist_scope = wlan_stats::HistScope::PER_ANTENNA;
    fidl_stats->antenna_id = ConvertAntennaId(stats.antenna_id);
  } else {
    fidl_stats->hist_scope = wlan_stats::HistScope::STATION;
  }
  for (size_t i = 0; i < stats.rssi_samples_count; ++i) {
    const auto& bucket = stats.rssi_samples_list[i];
    if (bucket.num_samples == 0) {
      continue;
    }
    wlan_stats::HistBucket fidl_bucket;
    fidl_bucket.bucket_index = bucket.bucket_index;
    fidl_bucket.num_samples = bucket.num_samples;
    fidl_stats->rssi_samples.push_back(fidl_bucket);
  }
  fidl_stats->invalid_samples = stats.invalid_samples;
}

void ConvertSnrHistogram(wlan_stats::SnrHistogram* fidl_stats,
                         const wlan_fullmac_snr_histogram_t& stats) {
  if (stats.hist_scope == WLAN_FULLMAC_HIST_SCOPE_PER_ANTENNA) {
    fidl_stats->hist_scope = wlan_stats::HistScope::PER_ANTENNA;
    fidl_stats->antenna_id = ConvertAntennaId(stats.antenna_id);
  } else {
    fidl_stats->hist_scope = wlan_stats::HistScope::STATION;
  }
  for (size_t i = 0; i < stats.snr_samples_count; ++i) {
    const auto& bucket = stats.snr_samples_list[i];
    if (bucket.num_samples == 0) {
      continue;
    }
    wlan_stats::HistBucket fidl_bucket;
    fidl_bucket.bucket_index = bucket.bucket_index;
    fidl_bucket.num_samples = bucket.num_samples;
    fidl_stats->snr_samples.push_back(fidl_bucket);
  }
  fidl_stats->invalid_samples = stats.invalid_samples;
}

void ConvertPmkInfo(wlan_mlme::PmkInfo* fidl_info, const wlan_fullmac_pmk_info_t& info) {
  fidl_info->pmk.resize(info.pmk_count);
  fidl_info->pmk.assign(info.pmk_list, info.pmk_list + info.pmk_count);
  fidl_info->pmkid.resize(info.pmkid_count);
  fidl_info->pmkid.assign(info.pmkid_list, info.pmkid_list + info.pmkid_count);
}

void ConvertIfaceCounterStats(wlan_stats::IfaceCounterStats* fidl_stats,
                              const wlan_fullmac_iface_counter_stats_t& stats) {
  fidl_stats->rx_unicast_total = stats.rx_unicast_total;
  fidl_stats->rx_unicast_drop = stats.rx_unicast_drop;
  fidl_stats->rx_multicast = stats.rx_multicast;
  fidl_stats->tx_total = stats.tx_total;
  fidl_stats->tx_drop = stats.tx_drop;
}

void ConvertIfaceHistogramStats(wlan_stats::IfaceHistogramStats* fidl_stats,
                                const wlan_fullmac_iface_histogram_stats_t& stats) {
  fidl_stats->noise_floor_histograms.resize(stats.noise_floor_histograms_count);
  for (size_t i = 0; i < stats.noise_floor_histograms_count; ++i) {
    ConvertNoiseFloorHistogram(&fidl_stats->noise_floor_histograms[i],
                               stats.noise_floor_histograms_list[i]);
  }
  fidl_stats->rssi_histograms.resize(stats.rssi_histograms_count);
  for (size_t i = 0; i < stats.rssi_histograms_count; ++i) {
    ConvertRssiHistogram(&fidl_stats->rssi_histograms[i], stats.rssi_histograms_list[i]);
  }
  fidl_stats->rx_rate_index_histograms.resize(stats.rx_rate_index_histograms_count);
  for (size_t i = 0; i < stats.rx_rate_index_histograms_count; ++i) {
    ConvertRxRateIndexHistogram(&fidl_stats->rx_rate_index_histograms[i],
                                stats.rx_rate_index_histograms_list[i]);
  }
  fidl_stats->snr_histograms.resize(stats.snr_histograms_count);
  for (size_t i = 0; i < stats.snr_histograms_count; ++i) {
    ConvertSnrHistogram(&fidl_stats->snr_histograms[i], stats.snr_histograms_list[i]);
  }
}

uint32_t ConvertMgmtCaptureFlags(wlan_mlme::MgmtFrameCaptureFlags fidl_flags) {
  uint32_t ret_flags = 0;
  uint32_t flags = static_cast<uint32_t>(fidl_flags);
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::ASSOC_REQ)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_ASSOC_REQ;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::ASSOC_RESP)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_ASSOC_RESP;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::REASSOC_REQ)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_REASSOC_REQ;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::REASSOC_RESP)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_REASSOC_RESP;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::PROBE_REQ)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_PROBE_REQ;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::PROBE_RESP)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_PROBE_RESP;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::TIMING_AD)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_TIMING_AD;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::BEACON)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_BEACON;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::ATIM)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_ATIM;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::DISASSOC)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_DISASSOC;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::AUTH)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_AUTH;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::DEAUTH)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_DEAUTH;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::ACTION)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_ACTION;
  }
  if ((flags & static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::ACTION_NO_ACK)) != 0) {
    ret_flags |= WLAN_MGMT_CAPTURE_FLAG_ACTION_NO_ACK;
  }
  return ret_flags;
}

wlan_mlme::MgmtFrameCaptureFlags ConvertMgmtCaptureFlags(uint32_t ddk_flags) {
  uint32_t ret_flags = 0;
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_ASSOC_REQ) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::ASSOC_REQ);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_ASSOC_RESP) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::ASSOC_RESP);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_REASSOC_REQ) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::REASSOC_REQ);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_REASSOC_RESP) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::REASSOC_RESP);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_PROBE_REQ) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::PROBE_REQ);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_PROBE_RESP) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::PROBE_RESP);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_TIMING_AD) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::TIMING_AD);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_BEACON) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::BEACON);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_ATIM) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::ATIM);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_DISASSOC) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::DISASSOC);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_AUTH) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::AUTH);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_DEAUTH) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::DEAUTH);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_ACTION) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::ACTION);
  }
  if ((ddk_flags & WLAN_MGMT_CAPTURE_FLAG_ACTION_NO_ACK) != 0) {
    ret_flags |= static_cast<uint32_t>(wlan_mlme::MgmtFrameCaptureFlags::ACTION_NO_ACK);
  }
  return static_cast<wlan_mlme::MgmtFrameCaptureFlags>(ret_flags);
}

void ConvertSaeAuthFrame(const ::fuchsia::wlan::mlme::SaeFrame& frame_in,
                         wlan_fullmac_sae_frame_t* frame_out) {
  memcpy(frame_out->peer_sta_address, frame_in.peer_sta_address.data(), ETH_ALEN);
  frame_out->status_code = wlan::common::ConvertStatusCode(frame_in.status_code);
  frame_out->seq_num = frame_in.seq_num;

  frame_out->sae_fields_count = frame_in.sae_fields.size();
  frame_out->sae_fields_list = frame_in.sae_fields.data();
}

void ConvertSaeAuthFrame(const wlan_fullmac_sae_frame_t* frame_in,
                         ::fuchsia::wlan::mlme::SaeFrame& frame_out) {
  memcpy(frame_out.peer_sta_address.data(), frame_in->peer_sta_address, ETH_ALEN);
  frame_out.status_code = wlan::common::ConvertStatusCode(frame_in->status_code);
  frame_out.seq_num = frame_in->seq_num;

  frame_out.sae_fields.resize(frame_in->sae_fields_count);
  frame_out.sae_fields.assign(frame_in->sae_fields_list,
                              frame_in->sae_fields_list + frame_in->sae_fields_count);
}

static void ConvertWmmAcParams(const wlan_wmm_ac_params_t* params_in,
                               ::fuchsia::wlan::internal::WmmAcParams* params_out) {
  params_out->aifsn = params_in->aifsn;
  params_out->ecw_min = params_in->ecw_min;
  params_out->ecw_max = params_in->ecw_max;
  params_out->txop_limit = params_in->txop_limit;
  params_out->acm = params_in->acm;
}

void ConvertWmmStatus(const wlan_wmm_params_t* params_in,
                      ::fuchsia::wlan::internal::WmmStatusResponse* resp) {
  resp->apsd = params_in->apsd, ConvertWmmAcParams(&params_in->ac_be_params, &resp->ac_be_params);
  ConvertWmmAcParams(&params_in->ac_bk_params, &resp->ac_bk_params);
  ConvertWmmAcParams(&params_in->ac_vi_params, &resp->ac_vi_params);
  ConvertWmmAcParams(&params_in->ac_vo_params, &resp->ac_vo_params);
}

}  // namespace wlanif
