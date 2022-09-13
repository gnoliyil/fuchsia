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
#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_NXP_NXPFMAC_CLIENT_CONNECTION_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_NXP_NXPFMAC_CLIENT_CONNECTION_H_

#include <fidl/fuchsia.wlan.ieee80211/cpp/common_types.h>
#include <netinet/if_ether.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

#include <mutex>

#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/event_handler.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/ioctl_adapter.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/mlan.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/mlan/mlan_ieee.h"
#include "src/connectivity/wlan/drivers/third_party/nxp/nxpfmac/waitable_state.h"

namespace wlan::nxpfmac {

class ClientConnection {
 public:
  using StatusCode = fuchsia_wlan_ieee80211::StatusCode;
  using OnConnectCallback = std::function<void(StatusCode)>;

  ClientConnection(IoctlAdapter* ioctl_adapter, uint32_t bss_index);
  ~ClientConnection();
  // Attempt to connect to given bssid on the given channel. Returns ZX_ERR_ALREADY_EXISTS if a
  // connection attempt is already in progress. Returns ZX_OK if the request is successfully
  // initiated, on_connect will be called asynchronously with the result of the connection attempt.
  zx_status_t Connect(const uint8_t (&bssid)[ETH_ALEN], uint8_t channel,
                      OnConnectCallback&& on_connect);
  // Cancel a connection attempt. This will call the on_connect callback passed to Connect if a
  // connection attempt was found. Returns ZX_ERR_NOT_FOUND if no connection attempt is in progress.
  zx_status_t CancelConnect();

  // Returns ZX_ERR_NOT_CONNECTED if not connected. Otherwise attempt to disconnect from an
  // established connection.
  zx_status_t Disconnect();

 private:
  void TriggerConnectCallback(StatusCode status_code) __TA_REQUIRES(mutex_);
  void CompleteConnection(StatusCode status_code) __TA_REQUIRES(mutex_);

  IoctlAdapter* ioctl_adapter_;
  const uint32_t bss_index_;
  bool connected_ __TA_GUARDED(mutex_) = false;
  WaitableState<bool> connect_in_progress_{false};
  OnConnectCallback on_connect_;
  // Something inside mlan_ds_bss makes this a variable size struct so we need to have a pointer.
  // Otherwise it has to be at the end of this class and that makes this class variable size which
  // means that all instances of this class would have to be at the end of any classes containing it
  // and so on.
  std::unique_ptr<IoctlRequest<mlan_ds_bss>> connect_request_ __TA_GUARDED(mutex_);
  std::mutex mutex_;

  EventRegistration disconnect_event_;
};

}  // namespace wlan::nxpfmac

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_NXP_NXPFMAC_CLIENT_CONNECTION_H_
