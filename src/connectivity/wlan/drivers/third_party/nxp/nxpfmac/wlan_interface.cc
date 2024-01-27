// Copyright (c) 2022 The Fuchsia Authors
//
// Permission to use, copy, modify, and/or distribute this software for any purpose with or without
// fee is hereby granted, provided that the above copyright notice and this permission notice
// appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
// SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
// AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
// NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
// OF THIS SOFTWARE.

#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/wlan_interface.h"

#include <arpa/inet.h>
#include <lib/async/cpp/task.h>
#include <lib/stdcompat/span.h>
#include <stdint.h>
#include <zircon/errors.h>
#include <zircon/status.h>

#include <wlan/common/element.h>
#include <wlan/common/ieee80211.h>
#include <wlan/common/ieee80211_codes.h>
#include <wlan/common/mac_frame.h>

#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/data_plane.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/device.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/device_context.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/ies.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/ioctl_adapter.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/key_ring.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/utils.h"

namespace wlan::nxpfmac {

// This usually completes in about 120 milliseconds, give it plenty of time to complete but not too
// long since connect scanning is synchronous.
constexpr zx_duration_t kConnectScanTimeout = ZX_MSEC(600);

WlanInterface::WlanInterface(zx_device_t* parent, uint32_t iface_index, wlan_mac_role_t role,
                             DeviceContext* context, const uint8_t mac_address[ETH_ALEN],
                             zx::channel&& mlme_channel)
    : WlanInterfaceDeviceType(parent),
      wlan::drivers::components::NetworkPort(context->data_plane_->NetDevIfcProto(), *this,
                                             static_cast<uint8_t>(iface_index)),
      role_(role),
      iface_index_(iface_index),
      mlme_channel_(std::move(mlme_channel)),
      key_ring_(context->ioctl_adapter_, iface_index),
      client_connection_(this, context, &key_ring_, iface_index),
      scanner_(context, iface_index),
      soft_ap_(this, context, iface_index),
      context_(context) {
  memcpy(mac_address_, mac_address, ETH_ALEN);
}

zx_status_t WlanInterface::Create(zx_device_t* parent, const char* name, uint32_t iface_index,
                                  wlan_mac_role_t role, DeviceContext* context,
                                  const uint8_t mac_address[ETH_ALEN], zx::channel&& mlme_channel,
                                  WlanInterface** out_interface) {
  std::unique_ptr<WlanInterface> interface(
      new WlanInterface(parent, iface_index, role, context, mac_address, std::move(mlme_channel)));

  // Set the given mac address in FW.
  zx_status_t status = interface->SetMacAddressInFw();
  if (status != ZX_OK) {
    NXPF_ERR("Unable to set Mac address in FW: %s", zx_status_get_string(status));
    return status;
  }

  status = interface->DdkAdd(name);
  if (status != ZX_OK) {
    NXPF_ERR("Failed to add fullmac device: %s", zx_status_get_string(status));
    return status;
  }

  NetworkPort::Role net_port_role;
  switch (role) {
    case WLAN_MAC_ROLE_CLIENT:
      net_port_role = NetworkPort::Role::Client;
      break;
    case WLAN_MAC_ROLE_AP:
      net_port_role = NetworkPort::Role::Ap;
      break;
    default:
      NXPF_ERR("Unsupported role %u", role);
      return ZX_ERR_INVALID_ARGS;
  }

  interface->NetworkPort::Init(net_port_role);

  *out_interface = interface.release();  // This now has its lifecycle managed by the devhost.
  return ZX_OK;
}

void WlanInterface::Remove(fit::callback<void()>&& on_remove) {
  {
    std::lock_guard lock(mutex_);
    on_remove_ = std::move(on_remove);
  }
  DdkAsyncRemove();
}

void WlanInterface::DdkRelease() {
  fit::callback<void()> on_remove;
  {
    std::lock_guard lock(mutex_);
    if (on_remove_) {
      on_remove = std::move(on_remove_);
    }
  }
  delete this;
  if (on_remove) {
    on_remove();
  }
}

zx_status_t WlanInterface::WlanFullmacImplStart(const wlan_fullmac_impl_ifc_protocol_t* ifc,
                                                zx::channel* out_mlme_channel) {
  std::lock_guard lock(mutex_);
  if (!mlme_channel_.is_valid()) {
    NXPF_ERR("%s IF already bound", __func__);
    return ZX_ERR_ALREADY_BOUND;
  }

  NXPF_INFO("Starting wlan_fullmac interface");
  {
    std::lock_guard lock(fullmac_ifc_mutex_);
    fullmac_ifc_ = ::ddk::WlanFullmacImplIfcProtocolClient(ifc);
  }
  is_up_ = true;

  ZX_DEBUG_ASSERT(out_mlme_channel != nullptr);
  *out_mlme_channel = std::move(mlme_channel_);
  return ZX_OK;
}

void WlanInterface::OnEapolResponse(wlan::drivers::components::Frame&& frame) {
  // This work cannot be run on this thread since it's triggered from the mlan main process. There
  // are locks in fullmac that could block this call if fullmac is calling into this driver and
  // causes something to wait for the mlan main process to run. Unfortunately std::function doesn't
  // allow capturing of move-only objects, it requires them to be copy-constructible. This means we
  // can't move capture `frame`, we have to make a copy of the eapol data instead. Fortunately this
  // is not performance critical.
  std::vector<uint8_t> eapol(frame.Data(), frame.Data() + frame.Size());
  zx_status_t status = async::PostTask(context_->device_->GetDispatcher(), [this, eapol] {
    constexpr size_t kEapolDataOffset = sizeof(ethhdr);
    wlan_fullmac_eapol_indication_t eapol_ind{
        .data_list = eapol.data() + kEapolDataOffset,
        .data_count = eapol.size() - kEapolDataOffset,
    };

    auto eth = reinterpret_cast<const ethhdr*>(eapol.data());
    memcpy(eapol_ind.dst_addr, eth->h_dest, sizeof(eapol_ind.dst_addr));
    memcpy(eapol_ind.src_addr, eth->h_source, sizeof(eapol_ind.src_addr));

    std::lock_guard lock(fullmac_ifc_mutex_);
    if (!fullmac_ifc_.is_valid()) {
      NXPF_WARN("Received EAPOL response when interface is shut down");
      return;
    }
    fullmac_ifc_.EapolInd(&eapol_ind);
  });
  if (status != ZX_OK) {
    NXPF_ERR("Failed to schedule EAPOL response: %s", zx_status_get_string(status));
  }
}

void WlanInterface::OnEapolTransmitted(zx_status_t status, const uint8_t* dst_addr) {
  const wlan_eapol_result_t result =
      status == ZX_OK ? WLAN_EAPOL_RESULT_SUCCESS : WLAN_EAPOL_RESULT_TRANSMISSION_FAILURE;
  wlan_fullmac_eapol_confirm_t response{.result_code = result};
  memcpy(response.dst_addr, dst_addr, sizeof(response.dst_addr));

  // This work cannot be run on this thread since it's triggered from the mlan main process. There
  // are locks in fullmac that could block this call if fullmac is calling into this driver and
  // causes something to wait for the mlan main process to run.
  status = async::PostTask(context_->device_->GetDispatcher(), [this, response] {
    std::lock_guard lock(fullmac_ifc_mutex_);
    if (!fullmac_ifc_.is_valid()) {
      NXPF_WARN("Received EAPOL transmit confirmation when interface is shut down");
      return;
    }
    fullmac_ifc_.EapolConf(&response);
  });
  if (status != ZX_OK) {
    NXPF_ERR("Failed to schedule EAPOL transmit confirmation: %s", zx_status_get_string(status));
  }
}

void WlanInterface::WlanFullmacImplStop() {}

#define WLAN_MAXRATE 108  /* in 500kbps units */
#define WLAN_RATE_1M 2    /* in 500kbps units */
#define WLAN_RATE_2M 4    /* in 500kbps units */
#define WLAN_RATE_5M5 11  /* in 500kbps units */
#define WLAN_RATE_11M 22  /* in 500kbps units */
#define WLAN_RATE_6M 12   /* in 500kbps units */
#define WLAN_RATE_9M 18   /* in 500kbps units */
#define WLAN_RATE_12M 24  /* in 500kbps units */
#define WLAN_RATE_18M 36  /* in 500kbps units */
#define WLAN_RATE_24M 48  /* in 500kbps units */
#define WLAN_RATE_36M 72  /* in 500kbps units */
#define WLAN_RATE_48M 96  /* in 500kbps units */
#define WLAN_RATE_54M 108 /* in 500kbps units */

void WlanInterface::WlanFullmacImplQuery(wlan_fullmac_query_info_t* info) {
  constexpr uint8_t kRates2g[] = {WLAN_RATE_1M,  WLAN_RATE_2M,  WLAN_RATE_5M5, WLAN_RATE_11M,
                                  WLAN_RATE_6M,  WLAN_RATE_9M,  WLAN_RATE_12M, WLAN_RATE_18M,
                                  WLAN_RATE_24M, WLAN_RATE_36M, WLAN_RATE_48M, WLAN_RATE_54M};
  constexpr uint8_t kRates5g[] = {WLAN_RATE_6M,  WLAN_RATE_9M,  WLAN_RATE_12M, WLAN_RATE_18M,
                                  WLAN_RATE_24M, WLAN_RATE_36M, WLAN_RATE_48M, WLAN_RATE_54M};
  static_assert(std::size(kRates2g) <= fuchsia_wlan_internal_MAX_SUPPORTED_BASIC_RATES);
  static_assert(std::size(kRates5g) <= fuchsia_wlan_internal_MAX_SUPPORTED_BASIC_RATES);

  // Retrieve the list of supported channels. This does not call into firmware, it's just mlan
  // computing all the channels and it shouldn't fail as long as the request is properly formatted.
  IoctlRequest<mlan_ds_bss> get_channels(MLAN_IOCTL_BSS, MLAN_ACT_GET, iface_index_,
                                         {.sub_command = MLAN_OID_BSS_CHANNEL_LIST});
  auto& chanlist = get_channels.UserReq().param.chanlist;
  IoctlStatus io_status = context_->ioctl_adapter_->IssueIoctlSync(&get_channels);
  if (io_status != IoctlStatus::Success) {
    NXPF_ERR("Failed to retrieve supported channels: %d", io_status);
    // Clear this to make sure we don't populate any channels.
    chanlist.num_of_chan = 0;
  }

  // Clear info first.
  *info = {};

  std::lock_guard lock(mutex_);

  // Retrieve FW Info (contains HT and VHT capabilities).
  IoctlRequest<mlan_ds_get_info> info_req;
  info_req = IoctlRequest<mlan_ds_get_info>(MLAN_IOCTL_GET_INFO, MLAN_ACT_GET, 0,
                                            {.sub_command = MLAN_OID_GET_FW_INFO});
  auto& fw_info = info_req.UserReq().param.fw_info;
  io_status = context_->ioctl_adapter_->IssueIoctlSync(&info_req);
  if (io_status != IoctlStatus::Success) {
    NXPF_ERR("FW Info get req failed: %d", io_status);
  }

  info->role = role_;
  info->band_cap_count = 2;
  memcpy(info->sta_addr, mac_address_, sizeof(mac_address_));

  memcpy(info->sta_addr, mac_address_, sizeof(mac_address_));

  // 2.4 GHz
  wlan_fullmac_band_capability* band_cap = &info->band_cap_list[0];
  band_cap->band = WLAN_BAND_TWO_GHZ;
  band_cap->basic_rate_count = std::size(kRates2g);
  std::copy(std::begin(kRates2g), std::end(kRates2g), std::begin(band_cap->basic_rate_list));

  for (uint32_t i = 0, chan = 0;
       i < chanlist.num_of_chan && i < std::size(band_cap->operating_channel_list); ++i) {
    if (band_from_channel(chanlist.cf[i].channel) == BAND_2GHZ) {
      band_cap->operating_channel_list[chan++] = static_cast<uint8_t>(chanlist.cf[i].channel);
      ++band_cap->operating_channel_count;
    }
  }
  band_cap->ht_supported = true;
  band_cap->vht_supported = true;
  wlan::HtCapabilities* ht_caps = wlan::HtCapabilities::View(&band_cap->ht_caps);
  wlan::VhtCapabilities* vht_caps = wlan::VhtCapabilities::View(&band_cap->vht_caps);
  ht_caps->ht_cap_info.set_as_uint16(fw_info.hw_dot_11n_dev_cap & 0xFFFF);
  vht_caps->vht_cap_info.set_as_uint32(fw_info.hw_dot_11ac_dev_cap);

  // 5 GHz
  band_cap = &info->band_cap_list[1];
  band_cap->band = WLAN_BAND_FIVE_GHZ;
  band_cap->basic_rate_count = std::size(kRates5g);
  std::copy(std::begin(kRates5g), std::end(kRates5g), std::begin(band_cap->basic_rate_list));

  for (uint32_t i = 0, chan = 0;
       i < chanlist.num_of_chan && i < std::size(band_cap->operating_channel_list); ++i) {
    if (band_from_channel(chanlist.cf[i].channel) == BAND_5GHZ) {
      band_cap->operating_channel_list[chan++] = static_cast<uint8_t>(chanlist.cf[i].channel);
      ++band_cap->operating_channel_count;
    }
  }
  band_cap->ht_supported = true;
  band_cap->vht_supported = true;
  ht_caps = wlan::HtCapabilities::View(&band_cap->ht_caps);
  vht_caps = wlan::VhtCapabilities::View(&band_cap->vht_caps);
  ht_caps->ht_cap_info.set_as_uint16(fw_info.hw_dot_11n_dev_cap & 0xFFFF);
  vht_caps->vht_cap_info.set_as_uint32(fw_info.hw_dot_11ac_dev_cap);
}

void WlanInterface::WlanFullmacImplQueryMacSublayerSupport(mac_sublayer_support_t* resp) {
  std::lock_guard lock(mutex_);
  *resp = mac_sublayer_support_t{
      .data_plane{.data_plane_type = DATA_PLANE_TYPE_GENERIC_NETWORK_DEVICE},
      .device{.mac_implementation_type = MAC_IMPLEMENTATION_TYPE_FULLMAC},
  };
}

void WlanInterface::WlanFullmacImplQuerySecuritySupport(security_support_t* resp) {
  std::lock_guard lock(mutex_);
  resp->sae.sme_handler_supported = true;
  resp->sae.driver_handler_supported = false;
  resp->mfp.supported = true;
}

void WlanInterface::WlanFullmacImplQuerySpectrumManagementSupport(
    spectrum_management_support_t* resp) {
  std::lock_guard lock(mutex_);
  resp->dfs.supported = false;
}

void WlanInterface::WlanFullmacImplStartScan(const wlan_fullmac_scan_req_t* req) {
  std::lock_guard lock(mutex_);

  auto on_scan_result = [this](const wlan_fullmac_scan_result_t& result) {
    std::lock_guard lock(fullmac_ifc_mutex_);
    fullmac_ifc_.OnScanResult(&result);
  };

  auto on_scan_end = [this](uint64_t txn_id, wlan_scan_result_t result) {
    std::lock_guard lock(fullmac_ifc_mutex_);
    const wlan_fullmac_scan_end_t end{.txn_id = txn_id, .code = result};
    fullmac_ifc_.OnScanEnd(&end);
  };

  // TODO(fxbug.dev/108408): Consider calculating a more accurate scan timeout based on the max
  // scan time per channel in the scan request.
  constexpr zx_duration_t kScanTimeout = ZX_MSEC(12000);
  zx_status_t status =
      scanner_.Scan(req, kScanTimeout, std::move(on_scan_result), std::move(on_scan_end));
  if (status != ZX_OK) {
    NXPF_ERR("Scan failed: %s", zx_status_get_string(status));
    const wlan_fullmac_scan_end_t end{.txn_id = req->txn_id,
                                      .code = WLAN_SCAN_RESULT_INTERNAL_ERROR};
    std::lock_guard lock(fullmac_ifc_mutex_);
    fullmac_ifc_.OnScanEnd(&end);
  }
}

void WlanInterface::WlanFullmacImplConnectReq(const wlan_fullmac_connect_req_t* req) {
  if (req == nullptr) {
    NXPF_ERR("Invalid connect request parameter");
    ConfirmConnectReq(ClientConnection::StatusCode::kInvalidParameters);
    return;
  }
  auto ssid = IeView(req->selected_bss.ies_list, req->selected_bss.ies_count).get(SSID);
  if (!ssid) {
    ConfirmConnectReq(ClientConnection::StatusCode::kInvalidParameters);
    return;
  }

  std::lock_guard lock(mutex_);

  // First stop any on-going scans.
  zx_status_t status = scanner_.StopScan();
  if (status == ZX_OK) {
    NXPF_INFO("Connecting, canceled pending scan");
  } else if (status != ZX_ERR_NOT_FOUND) {
    // An error occurred when canceling.
    NXPF_ERR("Failed to cancel on-going scan before connecting");
    ConfirmConnectReq(ClientConnection::StatusCode::kJoinFailure);
    return;
  }

  // Because MLAN expects the BSS to exist in its internal scan table we must perform a targetted
  // scan first. The WLAN stack does NOT guarantee a sequence of a connect scan followed by a
  // connection attempt.
  status = scanner_.ConnectScan(ssid->data(), ssid->size(), req->selected_bss.channel.primary,
                                kConnectScanTimeout);
  if (status != ZX_OK) {
    NXPF_ERR("Connect scan failed: %s", zx_status_get_string(status));
    ConfirmConnectReq(ClientConnection::StatusCode::kJoinFailure);
    return;
  }

  auto on_connect =
      [this](ClientConnection::StatusCode status_code, const uint8_t* ies, size_t ies_size)
          __TA_EXCLUDES(mutex_) { ConfirmConnectReq(status_code, ies, ies_size); };

  status = client_connection_.Connect(req, std::move(on_connect));
  if (status != ZX_OK) {
    ConfirmConnectReq(ClientConnection::StatusCode::kJoinFailure);
    return;
  }
}

void WlanInterface::WlanFullmacImplReconnectReq(const wlan_fullmac_reconnect_req_t* req) {
  NXPF_ERR("%s", __func__);
}

void WlanInterface::WlanFullmacImplAuthResp(const wlan_fullmac_auth_resp_t* resp) {
  NXPF_ERR("%s", __func__);
}

void WlanInterface::WlanFullmacImplDeauthReq(const wlan_fullmac_deauth_req_t* req) {
  std::lock_guard lock(mutex_);

  if (role_ == WLAN_MAC_ROLE_AP) {
    // Deauth the specified client associated to the SoftAP.
    zx_status_t status = soft_ap_.DeauthSta(req->peer_sta_address, req->reason_code);
    if (status != ZX_OK) {
      // Deauth request failed, respond to SME anyway since there is no way to indicate status.
      wlan_fullmac_deauth_confirm_t resp{};
      memcpy(resp.peer_sta_address, req->peer_sta_address, ETH_ALEN);
      std::lock_guard lock(fullmac_ifc_mutex_);
      fullmac_ifc_.DeauthConf(&resp);
    }
    // If the request is successful, the notification should occur via the event handler.
    return;
  }

  auto on_disconnect = [this](IoctlStatus status) __TA_EXCLUDES(mutex_) {
    if (status != IoctlStatus::Success) {
      NXPF_ERR("Deauth failed: %d", status);
    }
    // This doesn't have any way of indicating what went wrong.
    ConfirmDeauth();
  };

  zx_status_t status = client_connection_.Disconnect(req->peer_sta_address, req->reason_code,
                                                     std::move(on_disconnect));
  if (status != ZX_OK) {
    // The request didn't work, send the notification right away.
    ConfirmDeauth();
  }
}

void WlanInterface::WlanFullmacImplAssocResp(const wlan_fullmac_assoc_resp_t* resp) {
  NXPF_ERR("%s", __func__);
}

void WlanInterface::WlanFullmacImplDisassocReq(const wlan_fullmac_disassoc_req_t* req) {
  NXPF_ERR("%s", __func__);
  std::lock_guard lock(mutex_);

  auto on_disconnect = [this](IoctlStatus io_status) __TA_EXCLUDES(mutex_) {
    zx_status_t status = ZX_OK;
    switch (io_status) {
      case IoctlStatus::Success:
        status = ZX_OK;
        break;
      case IoctlStatus::Canceled:
        NXPF_ERR("Deauth canceled");
        status = ZX_ERR_CANCELED;
        break;
      case IoctlStatus::Timeout:
        NXPF_ERR("Deauth timed out");
        status = ZX_ERR_TIMED_OUT;
        break;
      default:
        NXPF_ERR("Deauth failed: %d", status);
        status = ZX_ERR_INTERNAL;
        break;
    }
    ConfirmDisassoc(status);
  };

  zx_status_t status = client_connection_.Disconnect(req->peer_sta_address, req->reason_code,
                                                     std::move(on_disconnect));
  if (status != ZX_OK) {
    // The request didn't work, send the notification right away.
    ConfirmDisassoc(status);
  }
}

void WlanInterface::WlanFullmacImplResetReq(const wlan_fullmac_reset_req_t* req) {
  NXPF_ERR("%s", __func__);
}

void WlanInterface::WlanFullmacImplStartReq(const wlan_fullmac_start_req_t* req) {
  const wlan_start_result_t result = [&]() -> wlan_start_result_t {
    std::lock_guard lock(mutex_);
    if (role_ != WLAN_MAC_ROLE_AP) {
      NXPF_ERR("Start is supported only in AP mode, current mode: %d", role_);
      return WLAN_START_RESULT_NOT_SUPPORTED;
    }
    if (req->bss_type != BSS_TYPE_INFRASTRUCTURE) {
      NXPF_ERR("Attempt to start AP in unsupported mode (%d)", req->bss_type);
      return WLAN_START_RESULT_NOT_SUPPORTED;
    }
    return soft_ap_.Start(req);
  }();

  std::lock_guard lock(fullmac_ifc_mutex_);
  wlan_fullmac_start_confirm_t start_conf = {.result_code = result};
  fullmac_ifc_.StartConf(&start_conf);
}

void WlanInterface::WlanFullmacImplStopReq(const wlan_fullmac_stop_req_t* req) {
  const wlan_stop_result_t result = [&]() -> wlan_stop_result_t {
    std::lock_guard lock(mutex_);

    if (role_ != WLAN_MAC_ROLE_AP) {
      NXPF_ERR("Stop is supported only in AP mode, current mode: %d", role_);
      return WLAN_STOP_RESULT_INTERNAL_ERROR;
    }

    return soft_ap_.Stop(req);
  }();

  std::lock_guard lock(fullmac_ifc_mutex_);
  wlan_fullmac_stop_confirm_t stop_conf = {.result_code = result};
  fullmac_ifc_.StopConf(&stop_conf);
}

void WlanInterface::WlanFullmacImplSetKeysReq(const wlan_fullmac_set_keys_req_t* req,
                                              wlan_fullmac_set_keys_resp_t* resp) {
  for (uint64_t i = 0; i < req->num_keys; ++i) {
    const zx_status_t status = key_ring_.AddKey(req->keylist[i]);
    if (status != ZX_OK) {
      NXPF_WARN("Error adding key %" PRIu64 ": %s", i, zx_status_get_string(status));
    }
    resp->statuslist[i] = status;
  }
  resp->num_keys = req->num_keys;
}

void WlanInterface::WlanFullmacImplDelKeysReq(const wlan_fullmac_del_keys_req_t* req) {
  for (uint64_t i = 0; i < req->num_keys; ++i) {
    const zx_status_t status = key_ring_.RemoveKey(req->keylist[i].key_id, req->keylist[i].address);
    if (status != ZX_OK) {
      NXPF_WARN("Error deleting key %" PRIu64 ": %s", i, zx_status_get_string(status));
    }
  }
}

void WlanInterface::WlanFullmacImplEapolReq(const wlan_fullmac_eapol_req_t* req) {
  std::optional<wlan::drivers::components::Frame> frame = context_->data_plane_->AcquireFrame();
  if (!frame.has_value()) {
    NXPF_ERR("Failed to acquire frame container for EAPOL frame");
    wlan_fullmac_eapol_confirm_t response{
        .result_code = WLAN_EAPOL_RESULT_TRANSMISSION_FAILURE,
    };
    std::lock_guard lock(fullmac_ifc_mutex_);
    fullmac_ifc_.EapolConf(&response);
    return;
  }

  const uint32_t packet_length =
      static_cast<uint32_t>(2ul * ETH_ALEN + sizeof(uint16_t) + req->data_count);

  frame->ShrinkHead(1024);
  frame->SetPortId(PortId());
  frame->SetSize(packet_length);

  memcpy(frame->Data(), req->dst_addr, ETH_ALEN);
  memcpy(frame->Data() + ETH_ALEN, req->src_addr, ETH_ALEN);
  *reinterpret_cast<uint16_t*>(frame->Data() + 2ul * ETH_ALEN) = htons(ETH_P_PAE);
  memcpy(frame->Data() + 2ul * ETH_ALEN + sizeof(uint16_t), req->data_list, req->data_count);

  context_->data_plane_->NetDevQueueTx(cpp20::span<wlan::drivers::components::Frame>(&*frame, 1u));
}

zx_status_t WlanInterface::WlanFullmacImplGetIfaceCounterStats(
    wlan_fullmac_iface_counter_stats_t* out_stats) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t WlanInterface::WlanFullmacImplGetIfaceHistogramStats(
    wlan_fullmac_iface_histogram_stats_t* out_stats) {
  return ZX_ERR_NOT_SUPPORTED;
}

void WlanInterface::WlanFullmacImplStartCaptureFrames(
    const wlan_fullmac_start_capture_frames_req_t* req,
    wlan_fullmac_start_capture_frames_resp_t* resp) {}

void WlanInterface::WlanFullmacImplStopCaptureFrames() {}

zx_status_t WlanInterface::WlanFullmacImplSetMulticastPromisc(bool enable) {
  return ZX_ERR_NOT_SUPPORTED;
}

void WlanInterface::WlanFullmacImplDataQueueTx(uint32_t options, ethernet_netbuf_t* netbuf,
                                               ethernet_impl_queue_tx_callback completion_cb,
                                               void* cookie) {
  ZX_PANIC("DataQueueTx should not ever be called, we're using netdevice");
}

void WlanInterface::WlanFullmacImplSaeHandshakeResp(const wlan_fullmac_sae_handshake_resp_t* resp) {
  if (resp == nullptr) {
    NXPF_ERR("Invalid response parameter");
    ConfirmConnectReq(ClientConnection::StatusCode::kInvalidParameters);
    return;
  }
  std::lock_guard lock_(mutex_);
  const zx_status_t status =
      client_connection_.OnSaeResponse(resp->peer_sta_address, resp->status_code);
  if (status != ZX_OK) {
    NXPF_ERR("Failed to handle SAE handshake response: %s", zx_status_get_string(status));
    if (resp->status_code != STATUS_CODE_SUCCESS) {
      ConfirmConnectReq(static_cast<ClientConnection::StatusCode>(resp->status_code));
    } else {
      ConfirmConnectReq(ClientConnection::StatusCode::kJoinFailure);
    }
    return;
  }
}

void WlanInterface::WlanFullmacImplSaeFrameTx(const wlan_fullmac_sae_frame_t* sae_frame) {
  if (sae_frame == nullptr) {
    NXPF_ERR("Invalid SAE frame parameter");
    ConfirmConnectReq(ClientConnection::StatusCode::kInvalidParameters);
    return;
  }

  std::lock_guard lock(mutex_);

  cpp20::span<const uint8_t> sae_fields(sae_frame->sae_fields_list, sae_frame->sae_fields_count);
  zx_status_t status =
      client_connection_.TransmitAuthFrame(mac_address_, sae_frame->peer_sta_address,
                                           sae_frame->seq_num, sae_frame->status_code, sae_fields);
  if (status != ZX_OK) {
    NXPF_ERR("Failed to transmit SAE auth frame: %s", zx_status_get_string(status));
  }
}

void WlanInterface::WlanFullmacImplWmmStatusReq() {
  // TODO(https://fxbug.dev/110091): Implement support for this.
  std::lock_guard lock(fullmac_ifc_mutex_);

  const wlan_wmm_parameters_t wmm{};
  fullmac_ifc_.OnWmmStatusResp(ZX_OK, &wmm);
}

void WlanInterface::WlanFullmacImplOnLinkStateChanged(bool online) { SetPortOnline(online); }

void WlanInterface::OnDisconnectEvent(uint16_t reason_code) {
  std::lock_guard lock(fullmac_ifc_mutex_);
  wlan_fullmac_deauth_indication_t ind{.reason_code = reason_code};
  memcpy(ind.peer_sta_address, mac_address_, sizeof(mac_address_));
  fullmac_ifc_.DeauthInd(&ind);
}

void WlanInterface::SignalQualityIndication(int8_t rssi, int8_t snr) {
  std::lock_guard lock(fullmac_ifc_mutex_);
  wlan_fullmac_signal_report_indication signal_ind = {.rssi_dbm = rssi, .snr_db = snr};
  fullmac_ifc_.SignalReport(&signal_ind);
}

void WlanInterface::InitiateSaeHandshake(const uint8_t* peer_mac) {
  std::lock_guard lock(fullmac_ifc_mutex_);
  if (!fullmac_ifc_.is_valid()) {
    NXPF_WARN("Received request to initiate SAE handshake when interface is shut down");
    return;
  }

  wlan_fullmac_sae_handshake_ind_t ind{};
  memcpy(ind.peer_sta_address, peer_mac, ETH_ALEN);
  fullmac_ifc_.SaeHandshakeInd(&ind);
}

void WlanInterface::OnAuthFrame(const uint8_t* peer_mac, const wlan::Authentication* frame,
                                cpp20::span<const uint8_t> trailing_data) {
  std::lock_guard lock(fullmac_ifc_mutex_);
  if (!fullmac_ifc_.is_valid()) {
    NXPF_WARN("Received auth frame when interface is shut down");
    return;
  }
  wlan_fullmac_sae_frame_t sae_frame{.status_code = frame->status_code,
                                     .seq_num = frame->auth_txn_seq_number};
  memcpy(sae_frame.peer_sta_address, peer_mac, ETH_ALEN);
  sae_frame.sae_fields_list = trailing_data.data();
  sae_frame.sae_fields_count = trailing_data.size();

  fullmac_ifc_.SaeFrameRx(&sae_frame);
}

// Handle STA connect event for SoftAP.
void WlanInterface::OnStaConnectEvent(uint8_t* sta_mac_addr, uint8_t* ies, uint32_t ie_length) {
  // This is the only event from FW for STA connect. Indicate both auth and assoc to SME.
  std::lock_guard lock(fullmac_ifc_mutex_);
  wlan_fullmac_auth_ind_t auth_ind_params = {};
  memcpy(auth_ind_params.peer_sta_address, sta_mac_addr, ETH_ALEN);
  // We always authenticate as an open system for WPA
  auth_ind_params.auth_type = WLAN_AUTH_TYPE_OPEN_SYSTEM;
  fullmac_ifc_.AuthInd(&auth_ind_params);

  // Indicate assoc.
  wlan_fullmac_assoc_ind_t assoc_ind_params = {};
  memcpy(assoc_ind_params.peer_sta_address, sta_mac_addr, ETH_ALEN);
  if (ie_length && ies && (ie_length <= sizeof(assoc_ind_params.vendor_ie))) {
    memcpy(assoc_ind_params.vendor_ie, ies, ie_length);
    assoc_ind_params.vendor_ie_len = ie_length;
  }
  fullmac_ifc_.AssocInd(&assoc_ind_params);
}

// Handle STA disconnect event for SoftAP.
void WlanInterface::OnStaDisconnectEvent(uint8_t* sta_mac_addr, uint16_t reason_code,
                                         bool locally_initiated) {
  // This is the only event from FW for STA disconnect. Indicate both deauth and disassoc to SME.
  std::lock_guard lock(fullmac_ifc_mutex_);
  if (!locally_initiated) {
    NXPF_INFO("Disconnect Event, not locally initiated, send deauth and disassoc ind");
    wlan_fullmac_deauth_indication_t deauth_ind_params = {};
    // Disconnect event contains the STA's mac address at offset 2.
    memcpy(deauth_ind_params.peer_sta_address, sta_mac_addr, ETH_ALEN);
    deauth_ind_params.reason_code = reason_code;

    // Indicate disassoc.
    fullmac_ifc_.DeauthInd(&deauth_ind_params);
    wlan_fullmac_disassoc_indication_t disassoc_ind_params = {};
    memcpy(disassoc_ind_params.peer_sta_address, sta_mac_addr, ETH_ALEN);
    disassoc_ind_params.reason_code = reason_code;
    fullmac_ifc_.DisassocInd(&disassoc_ind_params);
  } else {
    NXPF_INFO("Disconnect Event, locally initiated, send deauth conf");
    wlan_fullmac_deauth_confirm_t conf{};
    memcpy(conf.peer_sta_address, sta_mac_addr, ETH_ALEN);
    fullmac_ifc_.DeauthConf(&conf);
  }
}
uint32_t WlanInterface::PortGetMtu() { return 1500u; }

void WlanInterface::MacGetAddress(uint8_t out_mac[MAC_SIZE]) {
  memcpy(out_mac, mac_address_, MAC_SIZE);
}

void WlanInterface::MacGetFeatures(features_t* out_features) {
  *out_features = {
      .multicast_filter_count = MLAN_MAX_MULTICAST_LIST_SIZE,
      .supported_modes = SUPPORTED_MAC_FILTER_MODE_MULTICAST_FILTER |
                         SUPPORTED_MAC_FILTER_MODE_MULTICAST_PROMISCUOUS |
                         SUPPORTED_MAC_FILTER_MODE_PROMISCUOUS,
  };
}

void WlanInterface::MacSetMode(mode_t mode, cpp20::span<const uint8_t> multicast_macs) {
  IoctlRequest<mlan_ds_bss> request(MLAN_IOCTL_BSS, MLAN_ACT_SET, PortId(),
                                    {.sub_command = MLAN_OID_BSS_MULTICAST_LIST});

  auto& multicast_list = request.UserReq().param.multicast_list;

  switch (mode) {
    case MODE_MULTICAST_FILTER:
      multicast_list.mode = MLAN_MULTICAST_MODE;
      if (multicast_macs.size() > sizeof(multicast_list.mac_list)) {
        NXPF_ERR("Number of multicast macs %zu exceeds maximum value of %zu",
                 multicast_macs.size() / ETH_ALEN, std::size(multicast_list.mac_list));
        return;
      }
      memcpy(multicast_list.mac_list, multicast_macs.data(), multicast_macs.size());
      multicast_list.num_multicast_addr = static_cast<uint32_t>(multicast_macs.size() / ETH_ALEN);
      break;
    case MODE_MULTICAST_PROMISCUOUS:
      multicast_list.mode = MLAN_ALL_MULTI_MODE;
      break;
    case MODE_PROMISCUOUS:
      multicast_list.mode = MLAN_PROMISC_MODE;
      break;
    default:
      NXPF_ERR("Unsupported MAC mode %u", mode);
      return;
  }

  IoctlStatus io_status = context_->ioctl_adapter_->IssueIoctlSync(&request);
  if (io_status != IoctlStatus::Success) {
    NXPF_ERR("Failed to set mac mode: %d", io_status);
    return;
  }
}

zx_status_t WlanInterface::SetMacAddressInFw() {
  IoctlRequest<mlan_ds_bss> request(MLAN_IOCTL_BSS, MLAN_ACT_SET, iface_index_,
                                    {.sub_command = MLAN_OID_BSS_MAC_ADDR});
  request.IoctlReq().bss_index = iface_index_;
  memcpy(request.UserReq().param.mac_addr, mac_address_, ETH_ALEN);

  IoctlStatus io_status = context_->ioctl_adapter_->IssueIoctlSync(&request);
  if (io_status != IoctlStatus::Success) {
    NXPF_ERR("Failed to perform set MAC ioctl: %d", io_status);
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

void WlanInterface::ConfirmDeauth() {
  wlan_fullmac_deauth_confirm_t resp{};
  memcpy(resp.peer_sta_address, mac_address_, sizeof(mac_address_));
  std::lock_guard lock(fullmac_ifc_mutex_);
  fullmac_ifc_.DeauthConf(&resp);
}

void WlanInterface::ConfirmDisassoc(zx_status_t status) {
  NXPF_ERR("%s", __func__);
  const wlan_fullmac_disassoc_confirm_t resp{.status = status};
  std::lock_guard lock(fullmac_ifc_mutex_);
  fullmac_ifc_.DisassocConf(&resp);
}

void WlanInterface::ConfirmConnectReq(ClientConnection::StatusCode status, const uint8_t* ies,
                                      size_t ies_len) {
  const wlan_fullmac_connect_confirm_t result = {.result_code = static_cast<uint16_t>(status),
                                                 .association_ies_list = ies,
                                                 .association_ies_count = ies_len};
  std::lock_guard lock(fullmac_ifc_mutex_);
  fullmac_ifc_.ConnectConf(&result);
}

}  // namespace wlan::nxpfmac
