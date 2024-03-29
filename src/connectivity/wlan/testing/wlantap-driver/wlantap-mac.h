// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_TESTING_WLANTAP_DRIVER_WLANTAP_MAC_H_
#define SRC_CONNECTIVITY_WLAN_TESTING_WLANTAP_DRIVER_WLANTAP_MAC_H_

#include <fidl/fuchsia.wlan.softmac/cpp/driver/wire.h>
#include <fidl/fuchsia.wlan.tap/cpp/driver/wire.h>
#include <fidl/fuchsia.wlan.tap/cpp/wire.h>
#include <lib/driver/logging/cpp/logger.h>

#include "fidl/fuchsia.wlan.softmac/cpp/markers.h"

namespace wlan_tap = fuchsia_wlan_tap::wire;
namespace wlan_common = fuchsia_wlan_common::wire;
namespace wlan_softmac = fuchsia_wlan_softmac::wire;

namespace wlan {

// Serves the WlanSoftmac protocol.
// This class either responds to calls based on the given phy_config, or forwards calls to the
// Listener.
class WlantapMac : public fdf::WireServer<fuchsia_wlan_softmac::WlanSoftmac> {
 public:
  // An interface to allow another class to intercept WlanSoftmac calls.
  // Many of the functions in WlantapMac simply forward calls along to the Listener.
  class Listener {
   public:
    virtual void WlantapMacStart(
        fdf::ClientEnd<::fuchsia_wlan_softmac::WlanSoftmacIfc> ifc_client) = 0;
    virtual void WlantapMacStop() = 0;
    virtual void WlantapMacQueueTx(const wlan_softmac::WlanTxPacket& pkt) = 0;
    virtual void WlantapMacSetChannel(const wlan_common::WlanChannel& channel) = 0;
    virtual void WlantapMacJoinBss(const wlan_common::JoinBssRequest& join_request) = 0;
    virtual void WlantapMacStartScan(uint64_t scan_id) = 0;
    virtual void WlantapMacSetKey(const wlan_softmac::WlanKeyConfiguration& key_config) = 0;
  };

  WlantapMac(Listener* listener, wlan_common::WlanMacRole,
             const std::shared_ptr<const wlan_tap::WlantapPhyConfig>& config,
             zx::channel sme_channel);

  fidl::ProtocolHandler<fuchsia_wlan_softmac::WlanSoftmac> ProtocolHandler();

  // WlanSoftmac protocol implementation.
  void Query(fdf::Arena& arena, QueryCompleter::Sync& completer) override;
  void QueryDiscoverySupport(fdf::Arena& arena,
                             QueryDiscoverySupportCompleter::Sync& completer) override;
  void QueryMacSublayerSupport(fdf::Arena& arena,
                               QueryMacSublayerSupportCompleter::Sync& completer) override;
  void QuerySecuritySupport(fdf::Arena& arena,
                            QuerySecuritySupportCompleter::Sync& completer) override;
  void QuerySpectrumManagementSupport(
      fdf::Arena& arena, QuerySpectrumManagementSupportCompleter::Sync& completer) override;
  void Start(StartRequestView request, fdf::Arena& arena, StartCompleter::Sync& completer) override;
  void Stop(fdf::Arena& arena, StopCompleter::Sync& completer) override;
  void QueueTx(QueueTxRequestView request, fdf::Arena& arena,
               QueueTxCompleter::Sync& completer) override;
  void SetChannel(SetChannelRequestView request, fdf::Arena& arena,
                  SetChannelCompleter::Sync& completer) override;
  void JoinBss(JoinBssRequestView request, fdf::Arena& arena,
               JoinBssCompleter::Sync& completer) override;
  void EnableBeaconing(EnableBeaconingRequestView request, fdf::Arena& arena,
                       EnableBeaconingCompleter::Sync& completer) override;
  void DisableBeaconing(fdf::Arena& arena, DisableBeaconingCompleter::Sync& completer) override;
  void InstallKey(InstallKeyRequestView request, fdf::Arena& arena,
                  InstallKeyCompleter::Sync& completer) override;
  void NotifyAssociationComplete(NotifyAssociationCompleteRequestView request, fdf::Arena& arena,
                                 NotifyAssociationCompleteCompleter::Sync& completer) override;
  void ClearAssociation(ClearAssociationRequestView request, fdf::Arena& arena,
                        ClearAssociationCompleter::Sync& completer) override;
  void StartPassiveScan(StartPassiveScanRequestView request, fdf::Arena& arena,
                        StartPassiveScanCompleter::Sync& completer) override;
  void StartActiveScan(StartActiveScanRequestView request, fdf::Arena& arena,
                       StartActiveScanCompleter::Sync& completer) override;
  void CancelScan(CancelScanRequestView request, fdf::Arena& arena,
                  CancelScanCompleter::Sync& completer) override;
  void UpdateWmmParameters(UpdateWmmParametersRequestView request, fdf::Arena& arena,
                           UpdateWmmParametersCompleter::Sync& completer) override;

 private:
  Listener* listener_;
  wlan_common::WlanMacRole role_;

  const std::shared_ptr<const wlan_tap::WlantapPhyConfig> phy_config_;

  zx::channel sme_channel_;

  fdf::ServerBindingGroup<fuchsia_wlan_softmac::WlanSoftmac> bindings_;
};

}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_TESTING_WLANTAP_DRIVER_WLANTAP_MAC_H_
