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

#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/client_connection.h"

#include <fuchsia/wlan/ieee80211/c/banjo.h>
#include <lib/ddk/debug.h>
#include <netinet/if_ether.h>

#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/debug.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/device.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/device_context.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/event_handler.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/ies.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/ioctl_adapter.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/key_ring.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/utils.h"

namespace wlan::nxpfmac {

using wlan::drivers::timer::Timer;
constexpr zx_duration_t kDestructionStateTimeout = ZX_SEC(5);
constexpr zx_duration_t kConnectionTimeout = ZX_MSEC(6000);
constexpr zx_duration_t kDisconnectTimeout = ZX_MSEC(1000);
constexpr zx_duration_t kLogTimerTimeout = ZX_SEC(30);

ClientConnection::ClientConnection(ClientConnectionIfc* ifc, DeviceContext* context,
                                   KeyRing* key_ring, uint32_t bss_index)
    : ifc_(ifc),
      context_(context),
      key_ring_(key_ring),
      bss_index_(bss_index),
      connect_request_(new IoctlRequest<mlan_ds_bss>()) {
  disconnect_event_ = context_->event_handler_->RegisterForInterfaceEvent(
      MLAN_EVENT_ID_FW_DISCONNECTED, bss_index, [this](pmlan_event event) {
        OnDisconnect(*reinterpret_cast<const uint16_t*>(event->event_buf));
      });
  log_timer_ = std::make_unique<Timer>(context_->device_->GetDispatcher(),
                                       [this]() { IndicateSignalQuality(); });
}

ClientConnection::~ClientConnection() {
  // Stop the periodic log timer unconditionally.
  log_timer_->Stop();
  // Cancel any ongoing connection attempt.
  zx_status_t status = CancelConnect();
  if (status != ZX_OK && status != ZX_ERR_NOT_FOUND) {
    NXPF_ERR("Failed to cancel connection: %s", zx_status_get_string(status));
    // Don't attempt to wait for the correct state here, it might never happen.
    return;
  }

  // Using a MAC address of all zeroes will disconnect from the currently connected BSSID.
  constexpr uint8_t kZeroMac[ETH_ALEN] = {};
  status = Disconnect(kZeroMac, REASON_CODE_LEAVING_NETWORK_DEAUTH, [this](IoctlStatus io_status) {
    if (io_status != IoctlStatus::Success) {
      // Because we're waiting for the disconnect to complete in the destructor we should signal
      // that the state is idle anyway. This allows the destructor to complete instead of
      // permanently lock up.
      NXPF_ERR("Failed to disconnect, destroying connection anyway: %d", io_status);
      std::lock_guard lock(mutex_);
      state_ = State::Idle;
    }
  });
  if (status != ZX_OK && status != ZX_ERR_NOT_CONNECTED && status != ZX_ERR_ALREADY_EXISTS) {
    NXPF_ERR("Failed to disconnect: %s", zx_status_get_string(status));
    // Don't attempt to wait for the disconnected state here, it might never happen.
    return;
  }

  // Wait until any connect or disconnect attempt has completed. A connect or disconnect attempt
  // will cause asynchronous callbacks that could crash if the connection object goes away. By
  // waiting for an attempt to finish we avoid this. The CancelConnect call above should immediately
  // try to stop any ongoing connection attempt. Disconnect attempts should complete fast enough
  // that this shouldn't be an issue.
  std::unique_lock lock(mutex_);
  if (!state_.WaitFor(lock, State::Idle, kDestructionStateTimeout)) {
    NXPF_ERR("Connection failed to reach idle state at destruction");
  }
}

zx_status_t ClientConnection::Connect(const wlan_fullmac_connect_req_t* req,
                                      OnConnectCallback&& on_connect) {
  std::lock_guard lock(mutex_);
  if (state_ == State::Connecting) {
    return ZX_ERR_ALREADY_EXISTS;
  }
  if (state_ == State::Connected) {
    return ZX_ERR_ALREADY_BOUND;
  }
  if (state_ == State::Disconnecting) {
    return ZX_ERR_SHOULD_WAIT;
  }

  auto ssid = IeView(req->selected_bss.ies_list, req->selected_bss.ies_count).get(SSID);
  if (!ssid) {
    NXPF_ERR("Missing SSID in connection request");
    return ZX_ERR_INVALID_ARGS;
  }
  if (ssid->size() > MLAN_MAX_SSID_LENGTH) {
    NXPF_ERR("SSID length of %zu exceeds max length %u", ssid->size(), MLAN_MAX_SSID_LENGTH);
    return ZX_ERR_INVALID_ARGS;
  }

  zx_status_t status = key_ring_->RemoveAllKeys();
  if (status != ZX_OK) {
    NXPF_ERR("Could not remove all keys: %s", zx_status_get_string(status));
    return status;
  }

  if (req->wep_key.key_count > 0) {
    // The WEP key address should be considered a group key, set address to broadcast address.
    set_key_descriptor_t wep_key = req->wep_key;
    memset(wep_key.address, 0xFF, ETH_ALEN);
    status = key_ring_->AddKey(wep_key);
    if (status != ZX_OK) {
      NXPF_ERR("Could not set WEP key: %s", zx_status_get_string(status));
      return status;
    }
    status = key_ring_->EnableWepKey(wep_key.key_id);
    if (status != ZX_OK) {
      NXPF_ERR("Could not enable WEP key: %s", zx_status_get_string(status));
      return status;
    }
  }

  // We need to clear the IEs first to reset the internal MLAN state. This is especially true in
  // case there are no security IEs, otherwise the previous security settings might be applied to
  // the new connection.
  status = ClearIes();
  if (status != ZX_OK) {
    NXPF_ERR("Failed to clear IEs: %s", zx_status_get_string(status));
    return status;
  }
  status = ConfigureIes(req->security_ie_list, req->security_ie_count);
  if (status != ZX_OK) {
    NXPF_ERR("Failed to configure security IEs: %s", zx_status_get_string(status));
    return status;
  }

  status = SetAuthMode(req->auth_type);
  if (status != ZX_OK) {
    NXPF_ERR("Failed to set auth mode %u: %s", req->auth_type, zx_status_get_string(status));
    return status;
  }

  uint8_t pairwise_cipher_suite = 0;
  uint8_t group_cipher_suite = 0;
  status = GetRsnCipherSuites(req->security_ie_list, req->security_ie_count, &pairwise_cipher_suite,
                              &group_cipher_suite);
  // ZX_ERR_NOT_FOUND indicates there was no RSN IE, this could happen for an open network for
  // example. Don't treat this as an error, just use the default values of 0, there doesn't seem to
  // be a useful constant for this.
  if (status != ZX_OK && status != ZX_ERR_NOT_FOUND) {
    NXPF_ERR("Failed to get cipher suite from IEs: %s", zx_status_get_string(status));
    return status;
  }

  status = SetEncryptMode(pairwise_cipher_suite);
  if (status != ZX_OK) {
    NXPF_ERR("Failed to set pairwise cipher suite: %s", zx_status_get_string(status));
    return status;
  }
  if (pairwise_cipher_suite != group_cipher_suite) {
    // Only need to set the group cipher suite if it's different from the pairwise cipher suite.
    status = SetEncryptMode(group_cipher_suite);
    if (status != ZX_OK) {
      NXPF_ERR("Failed to set group cipher suite: %s", zx_status_get_string(status));
      return status;
    }
  }

  if (is_wpa_cipher_suite(pairwise_cipher_suite) || is_wpa_cipher_suite(group_cipher_suite)) {
    status = SetWpaEnabled(true);
    if (status != ZX_OK) {
      NXPF_ERR("Failed to enable WPA: %s", zx_status_get_string(status));
      return status;
    }
  }

  on_connect_ = std::move(on_connect);

  auto on_connect_complete = [this](mlan_ioctl_req* req, IoctlStatus io_status) {
    std::lock_guard lock(mutex_);
    if (state_ != State::Connecting) {
      NXPF_WARN("Connection ioctl completed when no connection was in progress");
      return;
    }

    if (io_status == IoctlStatus::Timeout) {
      NXPF_WARN("Connection attempt timed out");
      CompleteConnection(StatusCode::kRefusedReasonUnspecified);
      return;
    }
    if (io_status == IoctlStatus::Canceled) {
      CompleteConnection(StatusCode::kCanceled);
      return;
    }

    auto& request = reinterpret_cast<IoctlRequest<mlan_ds_bss>*>(req)->UserReq();
    const mlan_ds_misc_assoc_rsp& assoc_rsp = request.param.ssid_bssid.assoc_rsp;

    const uint8_t* ies = nullptr;
    size_t ies_size = 0;

    StatusCode status_code = StatusCode::kSuccess;
    if (assoc_rsp.assoc_resp_len >= sizeof(IEEEtypes_AssocRsp_t)) {
      auto response = reinterpret_cast<const IEEEtypes_AssocRsp_t*>(assoc_rsp.assoc_resp_buf);
      status_code = static_cast<StatusCode>(response->status_code);
      ies = response->ie_buffer;
      ies_size = assoc_rsp.assoc_resp_len - sizeof(IEEEtypes_AssocRsp_t) + 1;
    } else if (io_status != IoctlStatus::Success) {
      status_code = StatusCode::kJoinFailure;
    }

    CompleteConnection(status_code, ies, ies_size);
  };

  *connect_request_ = IoctlRequest<mlan_ds_bss>(
      MLAN_IOCTL_BSS, MLAN_ACT_SET, bss_index_,
      mlan_ds_bss{
          .sub_command = MLAN_OID_BSS_START,
          .param = {.ssid_bssid = {.idx = bss_index_,
                                   .channel = req->selected_bss.channel.primary}},
      });
  mlan_ssid_bssid& bss = connect_request_->UserReq().param.ssid_bssid;
  memcpy(bss.bssid, req->selected_bss.bssid, ETH_ALEN);
  bss.ssid.ssid_len = ssid->size();
  memcpy(bss.ssid.ssid, ssid->data(), bss.ssid.ssid_len);

  IoctlStatus io_status = context_->ioctl_adapter_->IssueIoctl(
      connect_request_.get(), std::move(on_connect_complete), kConnectionTimeout);
  if (io_status != IoctlStatus::Pending) {
    // Even IoctlStatus::Success should  be considered a failure here. Connecting has to be a
    // pending operation, anything else is unreasonable.
    NXPF_ERR("Connect ioctl failed: %d", io_status);
    return ZX_ERR_IO;
  }

  // The connection attempt is now in progress.
  state_ = State::Connecting;

  return ZX_OK;
}

zx_status_t ClientConnection::CancelConnect() {
  std::lock_guard lock(mutex_);

  if (state_ != State::Connecting) {
    // No connection in progress
    return ZX_ERR_NOT_FOUND;
  }

  context_->ioctl_adapter_->CancelIoctl(connect_request_.get());

  return ZX_OK;
}

zx_status_t ClientConnection::Disconnect(
    const uint8_t* addr, uint16_t reason_code,
    std::function<void(IoctlStatus)>&& on_disconnect_complete) {
  std::lock_guard lock(mutex_);
  if (state_ == State::Disconnecting) {
    return ZX_ERR_ALREADY_EXISTS;
  }
  if (state_ != State::Connected) {
    return ZX_ERR_NOT_CONNECTED;
  }

  State previous_state = state_.Load();
  state_ = State::Disconnecting;

  // Stop the log timer unconditionally.
  log_timer_->Stop();

  auto request = std::make_unique<IoctlRequest<mlan_ds_bss>>(
      MLAN_IOCTL_BSS, MLAN_ACT_SET, bss_index_,
      mlan_ds_bss{.sub_command = MLAN_OID_BSS_STOP,
                  .param = {.deauth_param{.reason_code = reason_code}}});
  memcpy(request->UserReq().param.deauth_param.mac_addr, addr, ETH_ALEN);

  auto on_ioctl = [this, previous_state, on_disconnect = std::move(on_disconnect_complete)](
                      pmlan_ioctl_req req, IoctlStatus status) {
    {
      std::lock_guard lock(mutex_);
      state_ = status == IoctlStatus::Success ? State::Idle : previous_state;
    }

    on_disconnect(status);
    delete reinterpret_cast<const IoctlRequest<mlan_ds_bss>*>(req);
  };

  const IoctlStatus io_status =
      context_->ioctl_adapter_->IssueIoctl(request.get(), std::move(on_ioctl), kDisconnectTimeout);
  if (io_status != IoctlStatus::Pending) {
    NXPF_ERR("Failed to disconnect: %d", io_status);
    state_ = previous_state;
    return ZX_ERR_INTERNAL;
  }

  // At this point the request is pending and the allocated memory will be handled by the callback.
  (void)request.release();

  return ZX_OK;
}

void ClientConnection::OnDisconnect(uint16_t reason_code) {
  NXPF_INFO("Client disconnect, reason: %u", reason_code);
  std::lock_guard lock(mutex_);
  if (state_ == State::Disconnecting && reason_code == 0) {
    // If there is a disconnect in progress and the reason code is zero this indicates that this
    // disconnect event is the result of the disconnect call by the driver. Don't handle this case
    // here, it will be handled when the disconnect ioctl completes. The ioctl seems to complete
    // after this event so it should be the safe choice.
    NXPF_INFO("Driver initiated disconnect");
    return;
  }
  if (state_ == State::Connecting) {
    // Attempt to cancel any ongoing connection attempt, if the cancel succeeds the connect callback
    // will be called with an indication that the connection failed.
    if (!context_->ioctl_adapter_->CancelIoctl(connect_request_.get())) {
      // If we can't cancel the ioctl and connect_in_progress_ is still set it means that the ioctl
      // must have been completed but the ioctl callback has yet to run. It seems like mlan should
      // not allow this case to happen, log an error message so it can be caught if it does happen.
      NXPF_ERR("Could not cancel connection attempt during disconnect event");
    }
    return;
  }
  if (state_ != State::Connected) {
    NXPF_ERR("Received disconnect event when not connected, reason: %u", reason_code);
    return;
  }
  state_ = State::Idle;
  // Stop the log timer since the client is disconnected.
  log_timer_->Stop();
  ifc_->OnDisconnectEvent(reason_code);
}

zx_status_t ClientConnection::GetRsnCipherSuites(const uint8_t* ies, size_t ies_count,
                                                 uint8_t* out_pairwise_cipher_suite,
                                                 uint8_t* out_group_cipher_suite) {
  const IeView ie_view(ies, ies_count);

  const IEEEtypes_Rsn_t* rsn = ie_view.get_as<IEEEtypes_Rsn_t>(RSN_IE);
  if (!rsn) {
    return ZX_ERR_NOT_FOUND;
  }

  if (rsn->pairwise_cipher.count != 1) {
    // Not equipped to deal with this at this point.
    NXPF_INFO("Too many cipher counts: %u", rsn->pairwise_cipher.count);
    return ZX_ERR_INVALID_ARGS;
  }
  *out_pairwise_cipher_suite = rsn->pairwise_cipher.list[0].type;
  *out_group_cipher_suite = rsn->group_cipher.type;
  return ZX_OK;
}

zx_status_t ClientConnection::ClearIes() {
  IoctlRequest<mlan_ds_misc_cfg> request(
      MLAN_IOCTL_MISC_CFG, MLAN_ACT_SET, bss_index_,
      mlan_ds_misc_cfg{.sub_command = MLAN_OID_MISC_GEN_IE,
                       .param{.gen_ie{.type = MLAN_IE_TYPE_GEN_IE}}});

  IoctlStatus io_status = context_->ioctl_adapter_->IssueIoctlSync(&request);
  if (io_status != IoctlStatus::Success) {
    NXPF_ERR("Failed to clear IEs: %d", io_status);
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

zx_status_t ClientConnection::ConfigureIes(const uint8_t* ies, size_t ies_count) {
  const IeView ie_view(ies, ies_count);

  for (auto& ie : ie_view) {
    if (ie.type() == MOBILITY_DOMAIN) {
      // Ignore this IE.
      continue;
    }
    if (ie.raw_size() > std::numeric_limits<uint8_t>::max()) {
      NXPF_ERR("IE %u of size %u exceeds maximum size %u", ie.type(), ie.raw_size(),
               std::numeric_limits<uint8_t>::max());
      continue;
    }
    if (ie.is_vendor_specific_oui_type(kOuiMicrosoft, kOuiTypeWmm)) {
      // Do not include WMM IEs, some APs will reject the association.
      continue;
    }

    IoctlRequest<mlan_ds_misc_cfg> request(
        MLAN_IOCTL_MISC_CFG, MLAN_ACT_SET, bss_index_,
        mlan_ds_misc_cfg{.sub_command = MLAN_OID_MISC_GEN_IE,
                         .param{.gen_ie{.type = MLAN_IE_TYPE_GEN_IE, .len = ie.raw_size()}}});
    memcpy(request.UserReq().param.gen_ie.ie_data, ie.raw_data(), ie.raw_size());

    IoctlStatus io_status = context_->ioctl_adapter_->IssueIoctlSync(&request);
    if (io_status != IoctlStatus::Success) {
      NXPF_ERR("Failed to set IE %u: %d", ie.type(), io_status);
      continue;
    }
  }

  return ZX_OK;
}

zx_status_t ClientConnection::SetAuthMode(wlan_auth_type_t auth_type) {
  uint32_t auth_mode = 0;
  switch (auth_type) {
    case WLAN_AUTH_TYPE_OPEN_SYSTEM:
      auth_mode = MLAN_AUTH_MODE_OPEN;
      break;
    case WLAN_AUTH_TYPE_SHARED_KEY:
      // When asked to use a shared key (which should only happen for WEP), we will direct the
      // firmware to use auto-detect, which will fall back on open WEP if shared WEP fails to
      // succeed. This was chosen to allow us to avoid implementing WEP auto-detection at higher
      // levels of the wlan stack.
      auth_mode = MLAN_AUTH_MODE_AUTO;
      break;
    case WLAN_AUTH_TYPE_FAST_BSS_TRANSITION:
      auth_mode = MLAN_AUTH_MODE_FT;
      break;
    case WLAN_AUTH_TYPE_SAE:
      auth_mode = MLAN_AUTH_MODE_SAE;
      break;
    default:
      NXPF_ERR("Invalid auth type %u", auth_type);
      return ZX_ERR_INVALID_ARGS;
  }

  IoctlRequest<mlan_ds_sec_cfg> request(
      MLAN_IOCTL_SEC_CFG, MLAN_ACT_SET, bss_index_,
      mlan_ds_sec_cfg{.sub_command = MLAN_OID_SEC_CFG_AUTH_MODE, .param{.auth_mode = auth_mode}});

  IoctlStatus io_status = context_->ioctl_adapter_->IssueIoctlSync(&request);
  if (io_status != IoctlStatus::Success) {
    NXPF_ERR("Failed to set auth mode: %d", io_status);
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

zx_status_t ClientConnection::SetEncryptMode(uint8_t cipher_suite) {
  uint32_t encrypt_mode = 0;

  switch (cipher_suite) {
    case 0:
      encrypt_mode = MLAN_ENCRYPTION_MODE_NONE;
      break;
    case CIPHER_SUITE_TYPE_WEP_40:
      encrypt_mode = MLAN_ENCRYPTION_MODE_WEP40;
      break;
    case CIPHER_SUITE_TYPE_WEP_104:
      encrypt_mode = MLAN_ENCRYPTION_MODE_WEP104;
      break;
    case CIPHER_SUITE_TYPE_TKIP:
      encrypt_mode = MLAN_ENCRYPTION_MODE_TKIP;
      break;
    case CIPHER_SUITE_TYPE_CCMP_128:
      encrypt_mode = MLAN_ENCRYPTION_MODE_CCMP;
      break;
    case CIPHER_SUITE_TYPE_CCMP_256:
      encrypt_mode = MLAN_ENCRYPTION_MODE_CCMP_256;
      break;
    case CIPHER_SUITE_TYPE_GCMP_128:
      encrypt_mode = MLAN_ENCRYPTION_MODE_GCMP;
      break;
    case CIPHER_SUITE_TYPE_GCMP_256:
      encrypt_mode = MLAN_ENCRYPTION_MODE_GCMP_256;
      break;
    default:
      NXPF_ERR("Unsupported cipher suite: %u", cipher_suite);
      return ZX_ERR_INVALID_ARGS;
  }

  IoctlRequest<mlan_ds_sec_cfg> request(
      MLAN_IOCTL_SEC_CFG, MLAN_ACT_SET, bss_index_,
      mlan_ds_sec_cfg{.sub_command = MLAN_OID_SEC_CFG_ENCRYPT_MODE,
                      .param{.encrypt_mode = encrypt_mode}});

  IoctlStatus io_status = context_->ioctl_adapter_->IssueIoctlSync(&request);
  if (io_status != IoctlStatus::Success) {
    NXPF_ERR("Failed to set encrypt mode: %d", io_status);
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

zx_status_t ClientConnection::SetWpaEnabled(bool enabled) {
  IoctlRequest<mlan_ds_sec_cfg> request(
      MLAN_IOCTL_SEC_CFG, MLAN_ACT_SET, bss_index_,
      mlan_ds_sec_cfg{.sub_command = MLAN_OID_SEC_CFG_WPA_ENABLED, .param{.wpa_enabled = enabled}});

  IoctlStatus io_status = context_->ioctl_adapter_->IssueIoctlSync(&request);
  if (io_status != IoctlStatus::Success) {
    NXPF_ERR("Failed to %s WPA: %d", enabled ? "enable" : "disable", io_status);
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

void ClientConnection::TriggerConnectCallback(StatusCode status_code, const uint8_t* ies,
                                              size_t ies_size) {
  if (on_connect_) {
    on_connect_(status_code, ies, ies_size);
    // Clear out the callback after using it.
    on_connect_ = OnConnectCallback();
  }
}

void ClientConnection::CompleteConnection(StatusCode status_code, const uint8_t* ies,
                                          size_t ies_size) {
  if (state_ != State::Connecting && state_ != State::Authenticating) {
    NXPF_WARN("Received connection completion with no connection attempt in progress, ignoring.");
    return;
  }
  state_ = status_code == StatusCode::kSuccess ? State::Connected : State::Idle;

  TriggerConnectCallback(status_code, ies, ies_size);
  // Start periodic timer to update logs/stats every 30 seconds if the connection was successful.
  if (state_ == State::Connected) {
    log_timer_->StartPeriodic(kLogTimerTimeout);
  }
}

void ClientConnection::IndicateSignalQuality() {
  IoctlRequest<mlan_ds_get_info> signal_req(MLAN_IOCTL_GET_INFO, MLAN_ACT_GET, bss_index_,
                                            {.sub_command = MLAN_OID_GET_SIGNAL});
  auto& signal_info = signal_req.UserReq().param.signal;

  IoctlStatus io_status = context_->ioctl_adapter_->IssueIoctlSync(&signal_req);
  if (io_status != IoctlStatus::Success) {
    NXPF_ERR("Get signal info req failed: %d", io_status);
    return;
  }
  NXPF_INFO("Client connection rssi %d snr %d", signal_info.data_rssi_avg,
            signal_info.data_snr_avg);
  ifc_->SignalQualityIndication((int8_t)signal_info.data_rssi_avg,
                                (int8_t)signal_info.data_snr_avg);
}

}  // namespace wlan::nxpfmac
