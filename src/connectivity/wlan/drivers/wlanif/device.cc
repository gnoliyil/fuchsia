// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device.h"

#include <fuchsia/hardware/wlan/fullmac/c/banjo.h>
#include <fuchsia/wlan/ieee80211/cpp/fidl.h>
#include <fuchsia/wlan/internal/c/banjo.h>
#include <fuchsia/wlan/internal/cpp/fidl.h>
#include <fuchsia/wlan/mlme/cpp/fidl.h>
#include <fuchsia/wlan/stats/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/ddk/device.h>
#include <net/ethernet.h>
#include <zircon/status.h>

#include <wlan/common/features.h>
#include <wlan/common/ieee80211_codes.h>
#include <wlan/drivers/log.h>

#include "convert.h"
#include "debug.h"
#include "driver.h"
#include "fuchsia/wlan/common/c/banjo.h"
#include "fuchsia/wlan/common/cpp/fidl.h"
#include "zircon/system/public/zircon/assert.h"

namespace wlanif {

namespace wlan_common = ::fuchsia::wlan::common;
namespace wlan_ieee80211 = ::fuchsia::wlan::ieee80211;
namespace wlan_mlme = ::fuchsia::wlan::mlme;
namespace wlan_stats = ::fuchsia::wlan::stats;

Device::Device(zx_device_t* device, wlan_fullmac_impl_protocol_t wlan_fullmac_impl_proto)
    : parent_(device), wlan_fullmac_impl_(wlan_fullmac_impl_proto) {
  ltrace_fn();
}

Device::~Device() { ltrace_fn(); }

#define DEV(c) static_cast<Device*>(c)
static zx_protocol_device_t device_ops = {
    .version = DEVICE_OPS_VERSION,
    .unbind = [](void* ctx) { DEV(ctx)->Unbind(); },
    .release = [](void* ctx) { DEV(ctx)->Release(); },
};

static wlan_fullmac_impl_ifc_protocol_ops_t wlan_fullmac_impl_ifc_ops = {
    // MLME operations
    .on_scan_result =
        [](void* cookie, const wlan_fullmac_scan_result_t* result) {
          DEV(cookie)->OnScanResult(result);
        },
    .on_scan_end = [](void* cookie,
                      const wlan_fullmac_scan_end_t* end) { DEV(cookie)->OnScanEnd(end); },
    .connect_conf =
        [](void* cookie, const wlan_fullmac_connect_confirm_t* resp) {
          DEV(cookie)->ConnectConf(resp);
        },
    .auth_ind = [](void* cookie,
                   const wlan_fullmac_auth_ind_t* ind) { DEV(cookie)->AuthenticateInd(ind); },
    .deauth_conf =
        [](void* cookie, const wlan_fullmac_deauth_confirm_t* resp) {
          DEV(cookie)->DeauthenticateConf(resp);
        },
    .deauth_ind =
        [](void* cookie, const wlan_fullmac_deauth_indication_t* ind) {
          DEV(cookie)->DeauthenticateInd(ind);
        },
    .assoc_ind = [](void* cookie,
                    const wlan_fullmac_assoc_ind_t* ind) { DEV(cookie)->AssociateInd(ind); },
    .disassoc_conf =
        [](void* cookie, const wlan_fullmac_disassoc_confirm_t* resp) {
          DEV(cookie)->DisassociateConf(resp);
        },
    .disassoc_ind =
        [](void* cookie, const wlan_fullmac_disassoc_indication_t* ind) {
          DEV(cookie)->DisassociateInd(ind);
        },
    .start_conf = [](void* cookie,
                     const wlan_fullmac_start_confirm_t* resp) { DEV(cookie)->StartConf(resp); },
    .stop_conf = [](void* cookie,
                    const wlan_fullmac_stop_confirm_t* resp) { DEV(cookie)->StopConf(resp); },
    .eapol_conf = [](void* cookie,
                     const wlan_fullmac_eapol_confirm_t* resp) { DEV(cookie)->EapolConf(resp); },
    .on_channel_switch =
        [](void* cookie, const wlan_fullmac_channel_switch_info_t* ind) {
          DEV(cookie)->OnChannelSwitched(ind);
        },
    // MLME extension operations
    .signal_report =
        [](void* cookie, const wlan_fullmac_signal_report_indication_t* ind) {
          DEV(cookie)->SignalReport(ind);
        },
    .eapol_ind = [](void* cookie,
                    const wlan_fullmac_eapol_indication_t* ind) { DEV(cookie)->EapolInd(ind); },
    .relay_captured_frame =
        [](void* cookie, const wlan_fullmac_captured_frame_result_t* result) {
          DEV(cookie)->RelayCapturedFrame(result);
        },
    .on_pmk_available =
        [](void* cookie, const wlan_fullmac_pmk_info_t* ind) { DEV(cookie)->OnPmkAvailable(ind); },
    .sae_handshake_ind =
        [](void* cookie, const wlan_fullmac_sae_handshake_ind_t* ind) {
          DEV(cookie)->SaeHandshakeInd(ind);
        },
    .sae_frame_rx = [](void* cookie,
                       const wlan_fullmac_sae_frame_t* ind) { DEV(cookie)->SaeFrameRx(ind); },
    .on_wmm_status_resp =
        [](void* cookie, const zx_status_t status, const wlan_wmm_params_t* params) {
          DEV(cookie)->OnWmmStatusResp(status, params);
        },

    // Ethernet operations
    .data_recv = [](void* cookie, const uint8_t* data, size_t length,
                    uint32_t flags) { DEV(cookie)->EthRecv(data, length, flags); },
};

static ethernet_impl_protocol_ops_t ethernet_impl_ops = {
    .query = [](void* ctx, uint32_t options, ethernet_info_t* info) -> zx_status_t {
      return DEV(ctx)->EthQuery(options, info);
    },
    .stop = [](void* ctx) { DEV(ctx)->EthStop(); },
    .start = [](void* ctx, const ethernet_ifc_protocol_t* ifc) -> zx_status_t {
      return DEV(ctx)->EthStart(ifc);
    },
    .queue_tx =
        [](void* ctx, uint32_t options, ethernet_netbuf_t* netbuf,
           ethernet_impl_queue_tx_callback completion_cb,
           void* cookie) { return DEV(ctx)->EthQueueTx(options, netbuf, completion_cb, cookie); },
    .set_param = [](void* ctx, uint32_t param, int32_t value, const uint8_t* data, size_t data_size)
        -> zx_status_t { return DEV(ctx)->EthSetParam(param, value, data, data_size); },
};
#undef DEV

zx_status_t Device::AddDevice() {
  device_add_args_t args = {};
  args.version = DEVICE_ADD_ARGS_VERSION;
  args.name = "wlanif";
  args.ctx = this;
  args.ops = &device_ops;
  mac_sublayer_support mac_sublayer_support;
  wlan_fullmac_impl_query_mac_sublayer_support(&wlan_fullmac_impl_, &mac_sublayer_support);
  if (mac_sublayer_support.data_plane.data_plane_type == DATA_PLANE_TYPE_ETHERNET_DEVICE) {
    // This is an ethernet driver, add the custom protocol to the device
    args.proto_id = ZX_PROTOCOL_ETHERNET_IMPL;
    args.proto_ops = &ethernet_impl_ops;
  }
  return device_add(parent_, &args, &device_);
}

#define VERIFY_IMPL_PROTO_OP(fn)                                \
  do {                                                          \
    if (wlan_fullmac_impl_.ops->fn == nullptr) {                \
      lerror("required impl proto function %s missing\n", #fn); \
      return ZX_ERR_INVALID_ARGS;                               \
    }                                                           \
  } while (0)

#define VERIFY_IMPL_IFC_PROTO_OP(fn)                                   \
  do {                                                                 \
    if (wlan_fullmac_impl_ifc_ops.fn == nullptr) {                     \
      lerror("required impl ifc protocol function %s missing\n", #fn); \
      return ZX_ERR_INVALID_ARGS;                                      \
    }                                                                  \
  } while (0)

zx_status_t Device::Bind() {
  ltrace_fn();

  // Assert minimum required functionality from the wlan_fullmac_impl driver
  if (wlan_fullmac_impl_.ops == nullptr) {
    lerror("no wlan_fullmac_impl protocol ops provided\n");
    return ZX_ERR_INVALID_ARGS;
  }

  VERIFY_IMPL_PROTO_OP(start);
  VERIFY_IMPL_PROTO_OP(query);
  VERIFY_IMPL_PROTO_OP(query_mac_sublayer_support);
  VERIFY_IMPL_PROTO_OP(query_security_support);
  VERIFY_IMPL_PROTO_OP(query_spectrum_management_support);
  VERIFY_IMPL_PROTO_OP(start_scan);
  VERIFY_IMPL_PROTO_OP(connect_req);
  VERIFY_IMPL_PROTO_OP(reconnect_req);
  VERIFY_IMPL_PROTO_OP(auth_resp);
  VERIFY_IMPL_PROTO_OP(deauth_req);
  VERIFY_IMPL_PROTO_OP(assoc_resp);
  VERIFY_IMPL_PROTO_OP(disassoc_req);
  VERIFY_IMPL_PROTO_OP(reset_req);
  VERIFY_IMPL_PROTO_OP(start_req);
  VERIFY_IMPL_PROTO_OP(stop_req);
  VERIFY_IMPL_PROTO_OP(set_keys_req);
  VERIFY_IMPL_PROTO_OP(del_keys_req);
  VERIFY_IMPL_PROTO_OP(eapol_req);

  VERIFY_IMPL_IFC_PROTO_OP(on_scan_result);
  VERIFY_IMPL_IFC_PROTO_OP(on_scan_end);
  VERIFY_IMPL_IFC_PROTO_OP(connect_conf);
  VERIFY_IMPL_IFC_PROTO_OP(auth_ind);
  VERIFY_IMPL_IFC_PROTO_OP(deauth_conf);
  VERIFY_IMPL_IFC_PROTO_OP(deauth_ind);
  VERIFY_IMPL_IFC_PROTO_OP(assoc_ind);
  VERIFY_IMPL_IFC_PROTO_OP(disassoc_conf);
  VERIFY_IMPL_IFC_PROTO_OP(disassoc_ind);
  VERIFY_IMPL_IFC_PROTO_OP(start_conf);
  VERIFY_IMPL_IFC_PROTO_OP(stop_conf);
  VERIFY_IMPL_IFC_PROTO_OP(eapol_conf);
  VERIFY_IMPL_IFC_PROTO_OP(on_channel_switch);
  VERIFY_IMPL_IFC_PROTO_OP(signal_report);
  VERIFY_IMPL_IFC_PROTO_OP(eapol_ind);
  VERIFY_IMPL_IFC_PROTO_OP(relay_captured_frame);
  VERIFY_IMPL_IFC_PROTO_OP(on_pmk_available);
  VERIFY_IMPL_IFC_PROTO_OP(sae_handshake_ind);
  VERIFY_IMPL_IFC_PROTO_OP(sae_frame_rx);
  VERIFY_IMPL_IFC_PROTO_OP(on_wmm_status_resp);
  VERIFY_IMPL_IFC_PROTO_OP(data_recv);

  // The MLME interface has no start/stop commands, so we will start the wlan_fullmac_impl
  // device immediately
  zx_handle_t mlme_channel = ZX_HANDLE_INVALID;
  zx_status_t status =
      wlan_fullmac_impl_start(&wlan_fullmac_impl_, this, &wlan_fullmac_impl_ifc_ops, &mlme_channel);
  ZX_DEBUG_ASSERT(mlme_channel != ZX_HANDLE_INVALID);
  if (status != ZX_OK) {
    lerror("call to wlan_fullmac-impl start() failed: %s\n", zx_status_get_string(status));
    return status;
  }

  // Query the device.
  wlan_fullmac_impl_query(&wlan_fullmac_impl_, &query_info_);

  mac_sublayer_support_t mac_sublayer_support;
  wlan_fullmac_impl_query_mac_sublayer_support(&wlan_fullmac_impl_, &mac_sublayer_support);
  if (wlan_fullmac_impl_.ops->data_queue_tx &&
      mac_sublayer_support.data_plane.data_plane_type == DATA_PLANE_TYPE_GENERIC_NETWORK_DEVICE) {
    lwarn(
        "driver implements data_queue_tx while indicating a GND data plane, data_queue_tx "
        "will not be called.");
  }

  status = loop_.StartThread("wlanif-loop");
  if (status != ZX_OK) {
    lerror("unable to start async loop: %s\n", zx_status_get_string(status));
    return status;
  }

  ZX_DEBUG_ASSERT(device_ == nullptr);
  status = AddDevice();
  if (status != ZX_OK) {
    lerror("could not add ethernet_impl device: %s\n", zx_status_get_string(status));
  } else {
    status = Connect(zx::channel(mlme_channel));
    if (status != ZX_OK) {
      lerror("unable to wait on SME channel: %s\n", zx_status_get_string(status));
      device_async_remove(device_);
      return status;
    }
  }

  if (status != ZX_OK) {
    loop_.Shutdown();
  }
  return status;
}
#undef VERIFY_IMPL_PROTO_OP
#undef VERIFY_IMPL_IFC_PROTO_OP

void Device::Unbind() {
  ltrace_fn();
  auto dispatcher = loop_.dispatcher();
  {
    // Stop accepting new FIDL requests.
    std::lock_guard<std::mutex> lock(lock_);
    if (binding_ != nullptr) {
      ::async::PostTask(dispatcher, [binding = std::move(binding_)] { binding->Unbind(); });
    }
  }

  // Ensure that all FIDL messages have been processed before removing the device
  ::async::PostTask(dispatcher, [this] { device_unbind_reply(device_); });
}

void Device::Release() {
  ltrace_fn();
  delete this;
}

zx_status_t Device::Connect(zx::channel request) {
  ltrace_fn();
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ != nullptr) {
    return ZX_ERR_ALREADY_BOUND;
  }
  auto binding = std::make_unique<fidl::Binding<::fuchsia::wlan::mlme::MLME>>(this);
  auto status = binding->Bind(std::move(request), loop_.dispatcher());
  if (status == ZX_OK) {
    binding_ = std::move(binding);
  }
  return status;
}

void Device::StartScan(wlan_mlme::ScanRequest req) {
  std::unique_ptr<uint8_t[]> channels_list_begin;
  std::unique_ptr<cssid_t[]> cssids_list_begin;

  wlan_fullmac_scan_req_t impl_req = {
      .txn_id = req.txn_id,
      .scan_type = ConvertScanType(req.scan_type),
      .channels_count = req.channel_list.size(),
      .ssids_count = req.ssid_list.size(),
      .min_channel_time = req.min_channel_time,
      .max_channel_time = req.max_channel_time,
  };

  if (impl_req.channels_count > 0) {
    // Clone list of channels
    channels_list_begin = std::make_unique<uint8_t[]>(impl_req.channels_count);
    if (channels_list_begin == nullptr) {
      wlan_mlme::ScanEnd fidl_end;
      fidl_end.txn_id = req.txn_id;
      fidl_end.code = wlan_mlme::ScanResultCode::INVALID_ARGS;
      SendScanEndUnlocked(std::move(fidl_end));
      return;
    }

    memcpy(channels_list_begin.get(), req.channel_list.data(), impl_req.channels_count);
    impl_req.channels_list = channels_list_begin.get();
  } else {
    wlan_mlme::ScanEnd fidl_end;
    fidl_end.txn_id = req.txn_id;
    fidl_end.code = wlan_mlme::ScanResultCode::INVALID_ARGS;
    SendScanEndUnlocked(std::move(fidl_end));
    return;
  }

  if (impl_req.ssids_count > 0) {
    // Clone list of SSIDs encoded as vector<uint8_t> into a list of cssid_t
    cssids_list_begin = std::make_unique<cssid_t[]>(impl_req.ssids_count);
    if (cssids_list_begin == nullptr) {
      wlan_mlme::ScanEnd fidl_end;
      fidl_end.txn_id = req.txn_id;
      fidl_end.code = wlan_mlme::ScanResultCode::INTERNAL_ERROR;
      SendScanEndUnlocked(std::move(fidl_end));
      return;
    }

    cssid_t* cssids_list_ptr = cssids_list_begin.get();
    for (auto ssid : req.ssid_list) {
      CloneIntoCSsid(ssid, *cssids_list_ptr);
      cssids_list_ptr++;
    }
    impl_req.ssids_list = cssids_list_begin.get();
  }

  wlan_fullmac_impl_start_scan(&wlan_fullmac_impl_, &impl_req);
}

void Device::ConnectReq(wlan_mlme::ConnectRequest req) {
  eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, false);

  wlan_fullmac_connect_req_t impl_req = {};

  // selected_bss
  ConvertBssDescription(&impl_req.selected_bss, req.selected_bss);

  // connect_failure_timeout
  impl_req.connect_failure_timeout = req.connect_failure_timeout;

  // auth_type
  impl_req.auth_type = ConvertAuthType(req.auth_type);

  // sae_password
  impl_req.sae_password_count = req.sae_password.size();
  impl_req.sae_password_list = req.sae_password.data();

  // wep_key
  if (req.wep_key) {
    ConvertSetKeyDescriptor(&impl_req.wep_key, *req.wep_key);
  } else {
    impl_req.wep_key.key_count = 0;
  }

  // security_ie
  {
    std::lock_guard<std::mutex> lock(lock_);
    protected_bss_ = !req.security_ie.empty();
    if (protected_bss_) {
      if (req.security_ie.size() > wlan_ieee80211::WLAN_IE_MAX_LEN) {
        lwarn("Security IE length truncated from %lu to %d\n", req.security_ie.size(),
              wlan_ieee80211::WLAN_IE_MAX_LEN);
        impl_req.security_ie_count = wlan_ieee80211::WLAN_IE_MAX_LEN;
      } else {
        impl_req.security_ie_count = req.security_ie.size();
      }
      impl_req.security_ie_list = req.security_ie.data();
    }
  }

  wlan_fullmac_impl_connect_req(&wlan_fullmac_impl_, &impl_req);
}

void Device::ReconnectReq(wlan_mlme::ReconnectRequest req) {
  wlan_fullmac_reconnect_req_t impl_req = {};

  // peer_sta_address
  std::memcpy(impl_req.peer_sta_address, req.peer_sta_address.data(), ETH_ALEN);

  wlan_fullmac_impl_reconnect_req(&wlan_fullmac_impl_, &impl_req);
}

void Device::AuthenticateResp(wlan_mlme::AuthenticateResponse resp) {
  wlan_fullmac_auth_resp_t impl_resp = {};

  // peer_sta_address
  std::memcpy(impl_resp.peer_sta_address, resp.peer_sta_address.data(), ETH_ALEN);

  // result_code
  impl_resp.result_code = ConvertAuthResultCode(resp.result_code);

  wlan_fullmac_impl_auth_resp(&wlan_fullmac_impl_, &impl_resp);
}

void Device::DeauthenticateReq(wlan_mlme::DeauthenticateRequest req) {
  eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, false);

  wlan_fullmac_deauth_req_t impl_req = {};

  // peer_sta_address
  std::memcpy(impl_req.peer_sta_address, req.peer_sta_address.data(), ETH_ALEN);

  // reason_code
  impl_req.reason_code = wlan::common::ConvertReasonCode(req.reason_code);

  wlan_fullmac_impl_deauth_req(&wlan_fullmac_impl_, &impl_req);
}

void Device::AssociateResp(wlan_mlme::AssociateResponse resp) {
  wlan_fullmac_assoc_resp_t impl_resp = {};

  // peer_sta_address
  std::memcpy(impl_resp.peer_sta_address, resp.peer_sta_address.data(), ETH_ALEN);

  // result_code
  impl_resp.result_code = ConvertAssocResultCode(resp.result_code);

  // association_id
  impl_resp.association_id = resp.association_id;

  wlan_fullmac_impl_assoc_resp(&wlan_fullmac_impl_, &impl_resp);
}

void Device::DisassociateReq(wlan_mlme::DisassociateRequest req) {
  eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, false);

  wlan_fullmac_disassoc_req_t impl_req = {};

  // peer_sta_address
  std::memcpy(impl_req.peer_sta_address, req.peer_sta_address.data(), ETH_ALEN);

  // reason_code
  impl_req.reason_code = static_cast<uint16_t>(req.reason_code);

  wlan_fullmac_impl_disassoc_req(&wlan_fullmac_impl_, &impl_req);
}

void Device::ResetReq(wlan_mlme::ResetRequest req) {
  eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, false);

  wlan_fullmac_reset_req_t impl_req = {};

  // sta_address
  std::memcpy(impl_req.sta_address, req.sta_address.data(), ETH_ALEN);

  // set_default_mib
  impl_req.set_default_mib = req.set_default_mib;

  wlan_fullmac_impl_reset_req(&wlan_fullmac_impl_, &impl_req);
}

void Device::StartReq(wlan_mlme::StartRequest req) {
  {
    std::lock_guard<std::mutex> lock(lock_);
    if (eth_device_.IsEthernetOnline()) {
      SendStartConfLocked(WLAN_START_RESULT_BSS_ALREADY_STARTED_OR_JOINED);
      return;
    }
  }

  wlan_fullmac_start_req_t impl_req = {};

  // ssid
  CloneIntoCSsid(req.ssid, impl_req.ssid);

  // bss_type
  impl_req.bss_type = static_cast<bss_type_t>(req.bss_type);

  // beacon_period
  impl_req.beacon_period = req.beacon_period;

  // dtim_period
  impl_req.dtim_period = req.dtim_period;

  // channel
  impl_req.channel = req.channel;

  // rsne
  CopyRSNE(req.rsne.value_or(std::vector<uint8_t>{}), impl_req.rsne, &impl_req.rsne_len);

  wlan_fullmac_impl_start_req(&wlan_fullmac_impl_, &impl_req);
}

void Device::StopReq(wlan_mlme::StopRequest req) {
  eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, false);

  wlan_fullmac_stop_req_t impl_req = {};
  CloneIntoCSsid(req.ssid, impl_req.ssid);

  wlan_fullmac_impl_stop_req(&wlan_fullmac_impl_, &impl_req);
}

void Device::SetKeysReq(wlan_mlme::SetKeysRequest req) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  wlan_fullmac_set_keys_req_t impl_req = {};
  wlan_fullmac_set_keys_resp_t impl_resp = {};

  // Abort if too many keys sent.
  size_t num_keys = req.keylist.size();
  if (num_keys > WLAN_MAX_KEYLIST_SIZE) {
    lerror("Key list with length %zu exceeds maximum size %d", num_keys, WLAN_MAX_KEYLIST_SIZE);
    std::vector<wlan_mlme::SetKeyResult> results;
    for (size_t result_idx = 0; result_idx < num_keys; result_idx++) {
      results.push_back(wlan_mlme::SetKeyResult{
          .key_id = req.keylist[result_idx].key_id,
          .status = ZX_ERR_INVALID_ARGS,
      });
    }
    wlan_mlme::SetKeysConfirm fidl_err_conf{.results = results};
    binding_->events().SetKeysConf(std::move(fidl_err_conf));
    return;
  }

  impl_req.num_keys = num_keys;
  for (size_t desc_ndx = 0; desc_ndx < num_keys; desc_ndx++) {
    ConvertSetKeyDescriptor(&impl_req.keylist[desc_ndx], req.keylist[desc_ndx]);
  }

  wlan_fullmac_impl_set_keys_req(&wlan_fullmac_impl_, &impl_req, &impl_resp);

  auto num_results = impl_resp.num_keys;
  if (num_keys != num_results) {
    lerror("SetKeyReq count (%zu) and SetKeyResp count (%zu) do not agree", num_keys, num_results);
    return;
  }

  std::vector<wlan_mlme::SetKeyResult> results;
  for (size_t result_idx = 0; result_idx < impl_resp.num_keys; result_idx++) {
    results.push_back(wlan_mlme::SetKeyResult{
        .key_id = impl_req.keylist[result_idx].key_id,
        .status = impl_resp.statuslist[result_idx],
    });
  }

  wlan_mlme::SetKeysConfirm fidl_conf{.results = results};
  binding_->events().SetKeysConf(std::move(fidl_conf));
}

void Device::DeleteKeysReq(wlan_mlme::DeleteKeysRequest req) {
  wlan_fullmac_del_keys_req_t impl_req = {};

  // keylist
  size_t num_keys = req.keylist.size();
  if (num_keys > WLAN_MAX_KEYLIST_SIZE) {
    lwarn("truncating key list from %zu to %d members\n", num_keys, WLAN_MAX_KEYLIST_SIZE);
    impl_req.num_keys = WLAN_MAX_KEYLIST_SIZE;
  } else {
    impl_req.num_keys = num_keys;
  }
  for (size_t desc_ndx = 0; desc_ndx < num_keys; desc_ndx++) {
    ConvertDeleteKeyDescriptor(&impl_req.keylist[desc_ndx], req.keylist[desc_ndx]);
  }

  wlan_fullmac_impl_del_keys_req(&wlan_fullmac_impl_, &impl_req);
}

void Device::EapolReq(wlan_mlme::EapolRequest req) {
  wlan_fullmac_eapol_req_t impl_req = {};

  // src_addr
  std::memcpy(impl_req.src_addr, req.src_addr.data(), ETH_ALEN);

  // dst_addr
  std::memcpy(impl_req.dst_addr, req.dst_addr.data(), ETH_ALEN);

  // data
  impl_req.data_count = req.data.size();
  impl_req.data_list = req.data.data();

  wlan_fullmac_impl_eapol_req(&wlan_fullmac_impl_, &impl_req);
}

void Device::QueryDeviceInfo(QueryDeviceInfoCallback cb) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::DeviceInfo fidl_resp = {};
  ConvertQueryInfoToDeviceInfo(&fidl_resp, query_info_);
  cb(std::move(fidl_resp));
}

void Device::QueryDiscoverySupport(QueryDiscoverySupportCallback cb) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  wlan_common::DiscoverySupport support;
  // Fullmac drivers do not currently support any discovery features.
  // If fullmac drivers come to support discovery features, this method will need
  // to check that the device has been bound (like the other Query functions below.
  cb(support);
}

void Device::QueryMacSublayerSupport(QueryMacSublayerSupportCallback cb) {
  std::lock_guard<std::mutex> lock(lock_);

  if (binding_ == nullptr) {
    return;
  }

  mac_sublayer_support support;
  wlan_fullmac_impl_query_mac_sublayer_support(&wlan_fullmac_impl_, &support);
  wlan_common::MacSublayerSupport fidl_support;
  const auto convert_status = wlan::common::ConvertMacSublayerSupportToFidl(support, &fidl_support);
  if (convert_status != ZX_OK) {
    lwarn("Invalid data plane type: %u", support.data_plane.data_plane_type);
  }
  ZX_DEBUG_ASSERT_MSG(convert_status == ZX_OK, "MAC sublayer support conversion failed: %s",
                      zx_status_get_string(convert_status));
  // All wlanif devices must be fullmac.
  const bool is_fullmac =
      fidl_support.device.mac_implementation_type == wlan_common::MacImplementationType::FULLMAC;
  if (!is_fullmac) {
    lerror("Invalid MAC implementation type: %u", support.device.mac_implementation_type);
  }
  ZX_DEBUG_ASSERT_MSG(is_fullmac, "MAC sublayer support conversion failed: %s",
                      zx_status_get_string(convert_status));

  cb(fidl_support);
}

void Device::QuerySecuritySupport(QuerySecuritySupportCallback cb) {
  std::lock_guard<std::mutex> lock(lock_);

  if (binding_ == nullptr) {
    return;
  }

  security_support support;
  wlan_fullmac_impl_query_security_support(&wlan_fullmac_impl_, &support);
  wlan_common::SecuritySupport fidl_support;
  const auto convert_status = wlan::common::ConvertSecuritySupportToFidl(support, &fidl_support);
  if (convert_status != ZX_OK) {
    lwarn("Security support conversion failed: %s", zx_status_get_string(convert_status));
  }
  ZX_DEBUG_ASSERT_MSG(convert_status == ZX_OK, "Security support conversion failed: %s",
                      zx_status_get_string(convert_status));

  cb(fidl_support);
}

void Device::QuerySpectrumManagementSupport(QuerySpectrumManagementSupportCallback cb) {
  std::lock_guard<std::mutex> lock(lock_);

  if (binding_ == nullptr) {
    return;
  }

  spectrum_management_support support;
  wlan_fullmac_impl_query_spectrum_management_support(&wlan_fullmac_impl_, &support);
  wlan_common::SpectrumManagementSupport fidl_support;
  const auto convert_status =
      wlan::common::ConvertSpectrumManagementSupportToFidl(support, &fidl_support);
  if (convert_status != ZX_OK) {
    lwarn("Spectrum management support conversion failed: %s",
          zx_status_get_string(convert_status));
  }
  ZX_DEBUG_ASSERT_MSG(convert_status == ZX_OK, "Spectrum management support conversion failed:: %s",
                      zx_status_get_string(convert_status));

  cb(fidl_support);
}

void Device::GetIfaceCounterStats(GetIfaceCounterStatsCallback cb) {
  auto status = ZX_ERR_BAD_STATE;
  wlan_fullmac_iface_counter_stats_t out_stats = {};
  if (wlan_fullmac_impl_.ops->get_iface_counter_stats != nullptr) {
    status = wlan_fullmac_impl_get_iface_counter_stats(&wlan_fullmac_impl_, &out_stats);
  }

  wlan_mlme::GetIfaceCounterStatsResponse fidl_resp;
  if (status == ZX_OK) {
    wlan_stats::IfaceCounterStats fidl_out_stats;
    ConvertIfaceCounterStats(&fidl_out_stats, out_stats);
    fidl_resp.set_stats(fidl_out_stats);
  } else {
    fidl_resp.set_error_status(status);
  }

  cb(std::move(fidl_resp));
}

void Device::GetIfaceHistogramStats(GetIfaceHistogramStatsCallback cb) {
  std::lock_guard<std::mutex> lock(get_iface_histogram_stats_lock_);

  auto status = ZX_ERR_BAD_STATE;
  wlan_fullmac_iface_histogram_stats_t out_stats = {};
  if (wlan_fullmac_impl_.ops->get_iface_histogram_stats != nullptr) {
    status = wlan_fullmac_impl_get_iface_histogram_stats(&wlan_fullmac_impl_, &out_stats);
  }

  wlan_mlme::GetIfaceHistogramStatsResponse fidl_resp;
  if (status == ZX_OK) {
    wlan_stats::IfaceHistogramStats fidl_out_stats;
    ConvertIfaceHistogramStats(&fidl_out_stats, out_stats);
    fidl_resp.set_stats(std::move(fidl_out_stats));
  } else {
    fidl_resp.set_error_status(status);
  }

  cb(std::move(fidl_resp));
}

void Device::ListMinstrelPeers(ListMinstrelPeersCallback cb) {
  lerror("Minstrel peer list not available: FullMAC driver not supported.\n");
  ZX_DEBUG_ASSERT(false);

  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  cb(wlan_mlme::MinstrelListResponse{});
}

void Device::GetMinstrelStats(wlan_mlme::MinstrelStatsRequest req, GetMinstrelStatsCallback cb) {
  lerror("Minstrel stats not available: FullMAC driver not supported.\n");
  ZX_DEBUG_ASSERT(false);

  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  cb(wlan_mlme::MinstrelStatsResponse{});
}

void Device::SendMpOpenAction(wlan_mlme::MeshPeeringOpenAction req) {
  lerror("SendMpConfirmAction is not implemented\n");
}

void Device::SendMpConfirmAction(wlan_mlme::MeshPeeringConfirmAction req) {
  lerror("SendMpConfirmAction is not implemented\n");
}

void Device::MeshPeeringEstablished(wlan_mlme::MeshPeeringParams params) {
  lerror("MeshPeeringEstablished is not implemented\n");
}

void Device::GetMeshPathTableReq(::fuchsia::wlan::mlme::GetMeshPathTableRequest req,
                                 GetMeshPathTableReqCallback cb) {
  lerror("GetMeshPathTable is not implemented\n");
}

void Device::SetControlledPort(wlan_mlme::SetControlledPortRequest req) {
  switch (req.state) {
    case wlan_mlme::ControlledPortState::OPEN:
      eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, true);
      break;
    case wlan_mlme::ControlledPortState::CLOSED:
      eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, false);
      break;
  }
}

void Device::StartCaptureFrames(::fuchsia::wlan::mlme::StartCaptureFramesRequest req,
                                StartCaptureFramesCallback cb) {
  wlan_fullmac_start_capture_frames_req_t impl_req = {};
  impl_req.mgmt_frame_flags = ConvertMgmtCaptureFlags(req.mgmt_frame_flags);

  wlan_fullmac_start_capture_frames_resp_t impl_resp = {};

  // forward request to driver
  wlan_fullmac_impl_start_capture_frames(&wlan_fullmac_impl_, &impl_req, &impl_resp);

  wlan_mlme::StartCaptureFramesResponse resp;
  resp.status = impl_resp.status;
  resp.supported_mgmt_frames = ConvertMgmtCaptureFlags(impl_resp.supported_mgmt_frames);
  cb(resp);
}

void Device::StopCaptureFrames() { wlan_fullmac_impl_stop_capture_frames(&wlan_fullmac_impl_); }

void Device::SaeHandshakeResp(::fuchsia::wlan::mlme::SaeHandshakeResponse resp) {
  wlan_fullmac_sae_handshake_resp_t handshake_resp = {};

  memcpy(handshake_resp.peer_sta_address, resp.peer_sta_address.data(), ETH_ALEN);
  handshake_resp.status_code = wlan::common::ConvertStatusCode(resp.status_code);
  wlan_fullmac_impl_sae_handshake_resp(&wlan_fullmac_impl_, &handshake_resp);
}

void Device::SaeFrameTx(::fuchsia::wlan::mlme::SaeFrame frame) {
  wlan_fullmac_sae_frame_t sae_frame = {};
  ConvertSaeAuthFrame(frame, &sae_frame);
  wlan_fullmac_impl_sae_frame_tx(&wlan_fullmac_impl_, &sae_frame);
}

void Device::WmmStatusReq() { wlan_fullmac_impl_wmm_status_req(&wlan_fullmac_impl_); }

void Device::OnScanResult(const wlan_fullmac_scan_result_t* result) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::ScanResult fidl_result;

  // txn_id
  fidl_result.txn_id = result->txn_id;

  // timestamp_nanos
  fidl_result.timestamp_nanos = result->timestamp_nanos;

  // bss
  ConvertBssDescription(&fidl_result.bss, result->bss);

  binding_->events().OnScanResult(std::move(fidl_result));
}

void Device::OnScanEnd(const wlan_fullmac_scan_end_t* end) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::ScanEnd fidl_end;
  fidl_end.txn_id = end->txn_id;
  fidl_end.code = ConvertScanResultCode(end->code);

  SendScanEndLockedBindingChecked(std::move(fidl_end));
}

void Device::SendScanEndLockedBindingChecked(wlan_mlme::ScanEnd scan_end) {
  binding_->events().OnScanEnd(std::move(scan_end));
}

void Device::SendScanEndUnlocked(::fuchsia::wlan::mlme::ScanEnd scan_end) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }
  SendScanEndLockedBindingChecked(std::move(scan_end));
}

void Device::ConnectConf(const wlan_fullmac_connect_confirm_t* resp) {
  std::lock_guard<std::mutex> lock(lock_);

  // For unprotected network, set data state to online immediately. For protected network, do
  // nothing. Later on upper layer would send message to open controlled port.
  if (resp->result_code == WLAN_ASSOC_RESULT_SUCCESS && !protected_bss_) {
    eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, true);
  }

  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::ConnectConfirm fidl_resp;

  // peer_sta_address
  std::memcpy(&fidl_resp.peer_sta_address, resp->peer_sta_address, ETH_ALEN);

  // result_code
  fidl_resp.result_code = static_cast<wlan_ieee80211::StatusCode>(resp->result_code);

  // association_id
  fidl_resp.association_id = resp->association_id;

  // association_ies
  if (resp->association_ies_count > 0) {
    fidl_resp.association_ies = std::vector(
        resp->association_ies_list, resp->association_ies_list + resp->association_ies_count);
  }

  binding_->events().ConnectConf(std::move(fidl_resp));
}

void Device::AuthenticateInd(const wlan_fullmac_auth_ind_t* ind) {
  std::lock_guard<std::mutex> lock(lock_);

  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::AuthenticateIndication fidl_ind;

  // peer_sta_address
  std::memcpy(fidl_ind.peer_sta_address.data(), ind->peer_sta_address, ETH_ALEN);

  // auth_type
  fidl_ind.auth_type = ConvertAuthType(ind->auth_type);

  binding_->events().AuthenticateInd(std::move(fidl_ind));
}

void Device::DeauthenticateConf(const wlan_fullmac_deauth_confirm_t* resp) {
  std::lock_guard<std::mutex> lock(lock_);

  if (query_info_.role == WLAN_MAC_ROLE_CLIENT) {
    eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, false);
  }

  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::DeauthenticateConfirm fidl_resp;

  // peer_sta_address
  std::memcpy(fidl_resp.peer_sta_address.data(), resp->peer_sta_address, ETH_ALEN);

  binding_->events().DeauthenticateConf(std::move(fidl_resp));
}

void Device::DeauthenticateInd(const wlan_fullmac_deauth_indication_t* ind) {
  std::lock_guard<std::mutex> lock(lock_);

  if (query_info_.role == WLAN_MAC_ROLE_CLIENT) {
    eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, false);
  }

  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::DeauthenticateIndication fidl_ind;

  // peer_sta_address
  std::memcpy(fidl_ind.peer_sta_address.data(), ind->peer_sta_address, ETH_ALEN);

  // reason_code
  fidl_ind.reason_code = wlan::common::ConvertReasonCode(ind->reason_code);

  // locally_initiated
  fidl_ind.locally_initiated = ind->locally_initiated;

  binding_->events().DeauthenticateInd(std::move(fidl_ind));
}

void Device::AssociateInd(const wlan_fullmac_assoc_ind_t* ind) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::AssociateIndication fidl_ind;

  ConvertAssocInd(&fidl_ind, *ind);
  binding_->events().AssociateInd(std::move(fidl_ind));
}

void Device::DisassociateConf(const wlan_fullmac_disassoc_confirm_t* resp) {
  std::lock_guard<std::mutex> lock(lock_);

  if (query_info_.role == WLAN_MAC_ROLE_CLIENT) {
    eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, false);
  }

  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::DisassociateConfirm fidl_resp;

  // status
  fidl_resp.status = resp->status;

  binding_->events().DisassociateConf(std::move(fidl_resp));
}

void Device::DisassociateInd(const wlan_fullmac_disassoc_indication_t* ind) {
  std::lock_guard<std::mutex> lock(lock_);

  if (query_info_.role == WLAN_MAC_ROLE_CLIENT) {
    eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, false);
  }

  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::DisassociateIndication fidl_ind;

  // peer_sta_address
  std::memcpy(fidl_ind.peer_sta_address.data(), ind->peer_sta_address, ETH_ALEN);

  // reason_code
  fidl_ind.reason_code = static_cast<wlan_ieee80211::ReasonCode>(ind->reason_code);

  // locally_initiated
  fidl_ind.locally_initiated = ind->locally_initiated;

  binding_->events().DisassociateInd(std::move(fidl_ind));
}

void Device::StartConf(const wlan_fullmac_start_confirm_t* resp) {
  std::lock_guard<std::mutex> lock(lock_);

  if (resp->result_code == WLAN_START_RESULT_SUCCESS) {
    eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, true);
  }

  SendStartConfLocked(resp->result_code);
}

void Device::StopConf(const wlan_fullmac_stop_confirm_t* resp) {
  std::lock_guard<std::mutex> lock(lock_);

  if (resp->result_code == WLAN_STOP_RESULT_SUCCESS) {
    eth_device_.SetEthernetStatus(&wlan_fullmac_impl_, false);
  }

  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::StopConfirm fidl_resp;
  fidl_resp.result_code = ConvertStopResultCode(resp->result_code);

  binding_->events().StopConf(fidl_resp);
}

void Device::EapolConf(const wlan_fullmac_eapol_confirm_t* resp) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::EapolConfirm fidl_resp;

  ConvertEapolConf(&fidl_resp, *resp);

  binding_->events().EapolConf(std::move(fidl_resp));
}

void Device::OnChannelSwitched(const wlan_fullmac_channel_switch_info_t* info) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  ::fuchsia::wlan::internal::ChannelSwitchInfo fidl_info;
  fidl_info.new_channel = info->new_channel;

  binding_->events().OnChannelSwitched(fidl_info);
}

void Device::SaeHandshakeInd(const wlan_fullmac_sae_handshake_ind_t* ind) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::SaeHandshakeIndication fidl_ind;

  memcpy(fidl_ind.peer_sta_address.data(), ind->peer_sta_address, ETH_ALEN);
  binding_->events().OnSaeHandshakeInd(fidl_ind);
}

void Device::SaeFrameRx(const wlan_fullmac_sae_frame_t* frame) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::SaeFrame fidl_frame;
  ConvertSaeAuthFrame(frame, fidl_frame);
  binding_->events().OnSaeFrameRx(std::move(fidl_frame));
}

void Device::OnWmmStatusResp(const zx_status_t status, const wlan_wmm_params_t* params) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  ::fuchsia::wlan::internal::WmmStatusResponse resp;
  ConvertWmmStatus(params, &resp);
  binding_->events().OnWmmStatusResp(status, std::move(resp));
}

void Device::SignalReport(const wlan_fullmac_signal_report_indication_t* ind) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  ::fuchsia::wlan::internal::SignalReportIndication fidl_ind{
      .rssi_dbm = ind->rssi_dbm,
      .snr_db = ind->snr_db,
  };

  binding_->events().SignalReport(std::move(fidl_ind));
}

void Device::EapolInd(const wlan_fullmac_eapol_indication_t* ind) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::EapolIndication fidl_ind;

  // src_addr
  std::memcpy(fidl_ind.src_addr.data(), ind->src_addr, ETH_ALEN);

  // dst_addr
  std::memcpy(fidl_ind.dst_addr.data(), ind->dst_addr, ETH_ALEN);

  // data
  fidl_ind.data.resize(ind->data_count);
  fidl_ind.data.assign(ind->data_list, ind->data_list + ind->data_count);

  binding_->events().EapolInd(std::move(fidl_ind));
}

void Device::RelayCapturedFrame(const wlan_fullmac_captured_frame_result* result) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::CapturedFrameResult fidl_result;
  fidl_result.frame.resize(result->data_count);
  fidl_result.frame.assign(result->data_list, result->data_list + result->data_count);

  binding_->events().RelayCapturedFrame(std::move(fidl_result));
}

void Device::OnPmkAvailable(const wlan_fullmac_pmk_info_t* info) {
  std::lock_guard<std::mutex> lock(lock_);
  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::PmkInfo fidl_info;
  ConvertPmkInfo(&fidl_info, *info);
  binding_->events().OnPmkAvailable(fidl_info);
}

zx_status_t Device::EthStart(const ethernet_ifc_protocol_t* ifc) {
  return eth_device_.EthStart(ifc);
}

void Device::EthStop() { return eth_device_.EthStop(); }

zx_status_t Device::EthQuery(uint32_t options, ethernet_info_t* info) {
  std::lock_guard<std::mutex> lock(lock_);

  std::memset(info, 0, sizeof(*info));

  // features
  info->features = ETHERNET_FEATURE_WLAN;
  if (query_info_.features & WLAN_FULLMAC_FEATURE_DMA) {
    info->features |= ETHERNET_FEATURE_DMA;
  }
  if (query_info_.features & WLAN_FULLMAC_FEATURE_SYNTH) {
    info->features |= ETHERNET_FEATURE_SYNTH;
  }
  if (query_info_.role == WLAN_MAC_ROLE_AP) {
    info->features |= ETHERNET_FEATURE_WLAN_AP;
  }

  // mtu
  info->mtu = 1500;
  info->netbuf_size = sizeof(ethernet_netbuf_t);

  // sta
  std::memcpy(info->mac, query_info_.sta_addr, ETH_ALEN);

  return ZX_OK;
}

void Device::EthQueueTx(uint32_t options, ethernet_netbuf_t* netbuf,
                        ethernet_impl_queue_tx_callback completion_cb, void* cookie) {
  eth_device_.EthQueueTx(&wlan_fullmac_impl_, options, netbuf, completion_cb, cookie);
}

zx_status_t Device::EthSetParam(uint32_t param, int32_t value, const void* data, size_t data_size) {
  return eth_device_.EthSetParam(&wlan_fullmac_impl_, param, value, data, data_size);
}

void Device::EthRecv(const uint8_t* data, size_t length, uint32_t flags) {
  eth_device_.EthRecv(data, length, flags);
}

EthDevice::EthDevice() { ltrace_fn(); }

EthDevice::~EthDevice() { ltrace_fn(); }

zx_status_t EthDevice::EthStart(const ethernet_ifc_protocol_t* ifc) {
  std::lock_guard<std::mutex> lock(lock_);
  ethernet_ifc_ = *ifc;
  eth_started_ = true;
  if (eth_online_) {
    ethernet_ifc_status(&ethernet_ifc_, ETHERNET_STATUS_ONLINE);
  }
  // TODO(fxbug.dev/51009): Inform SME that ethernet has started.
  return ZX_OK;
}

void EthDevice::EthStop() {
  std::lock_guard<std::mutex> lock(lock_);
  eth_started_ = false;
  ethernet_ifc_ = {};
}

void EthDevice::EthQueueTx(wlan_fullmac_impl_protocol_t* wlan_fullmac_impl_proto, uint32_t options,
                           ethernet_netbuf_t* netbuf, ethernet_impl_queue_tx_callback completion_cb,
                           void* cookie) {
  if (wlan_fullmac_impl_proto->ops->data_queue_tx != nullptr) {
    wlan_fullmac_impl_data_queue_tx(wlan_fullmac_impl_proto, options, netbuf, completion_cb,
                                    cookie);
  } else {
    completion_cb(cookie, ZX_ERR_NOT_SUPPORTED, netbuf);
  }
}

zx_status_t EthDevice::EthSetParam(wlan_fullmac_impl_protocol_t* wlan_fullmac_impl_proto,
                                   uint32_t param, int32_t value, const void* data,
                                   size_t data_size) {
  zx_status_t status = ZX_ERR_NOT_SUPPORTED;

  switch (param) {
    case ETHERNET_SETPARAM_PROMISC:
      // See fxbug.dev/28881: In short, the bridge mode doesn't require WLAN promiscuous mode
      // enabled.
      //               So we give a warning and return OK here to continue the bridging.
      // TODO(fxbug.dev/29113): To implement the real promiscuous mode.
      if (value == 1) {  // Only warn when enabling.
        lwarn("WLAN promiscuous not supported yet. see fxbug.dev/29113\n");
      }
      status = ZX_OK;
      break;
    case ETHERNET_SETPARAM_MULTICAST_PROMISC:
      if (wlan_fullmac_impl_proto->ops->set_multicast_promisc != nullptr) {
        return wlan_fullmac_impl_set_multicast_promisc(wlan_fullmac_impl_proto, !!value);
      } else {
        return ZX_ERR_NOT_SUPPORTED;
      }
      break;
  }

  return status;
}

void EthDevice::SetEthernetStatus(wlan_fullmac_impl_protocol_t* wlan_fullmac_impl_proto,
                                  bool online) {
  std::lock_guard<std::mutex> lock(lock_);

  // TODO(fxbug.dev/51009): Let SME handle these changes.
  if (online != eth_online_) {
    eth_online_ = online;
    if (eth_started_) {
      ethernet_ifc_status(&ethernet_ifc_, online ? ETHERNET_STATUS_ONLINE : 0);
    }
    if (wlan_fullmac_impl_proto->ops->on_link_state_changed) {
      wlan_fullmac_impl_on_link_state_changed(wlan_fullmac_impl_proto, online);
    }
  }
}

bool EthDevice::IsEthernetOnline() {
  std::lock_guard<std::mutex> lock(lock_);
  return eth_online_;
}

void EthDevice::EthRecv(const uint8_t* data, size_t length, uint32_t flags) {
  std::lock_guard<std::mutex> lock(lock_);
  if (eth_started_) {
    ethernet_ifc_recv(&ethernet_ifc_, data, length, flags);
  }
}

void Device::SendStartConfLocked(wlan_start_result_t result_code) {
  if (binding_ == nullptr) {
    return;
  }

  wlan_mlme::StartConfirm fidl_resp;

  // result_code
  fidl_resp.result_code = ConvertStartResultCode(result_code);

  binding_->events().StartConf(fidl_resp);
}

}  // namespace wlanif
