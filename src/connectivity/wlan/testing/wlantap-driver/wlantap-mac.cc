// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "wlantap-mac.h"

#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/fdf/cpp/dispatcher.h>

#include <mutex>

#include <ddktl/device.h>
#include <wlan/common/channel.h>
#include <wlan/common/features.h>
#include <wlan/common/phy.h>

#include "utils.h"

namespace wlan {

namespace wlan_common = fuchsia_wlan_common::wire;
namespace wlan_softmac = fuchsia_wlan_softmac::wire;

namespace {

// TODO(fxbug.dev/93459) Prune unnecessary fields from phy_config
struct WlantapMacImpl : WlantapMac,
                        public ddk::Device<WlantapMacImpl, ddk::Initializable, ddk::Unbindable,
                                           ddk::ServiceConnectable>,
                        public fdf::WireServer<fuchsia_wlan_softmac::WlanSoftmac> {
  WlantapMacImpl(zx_device_t* phy_device, wlan_common::WlanMacRole role,
                 const std::shared_ptr<const wlan_tap::WlantapPhyConfig> phy_config,
                 Listener* listener, zx::channel sme_channel)
      : ddk::Device<WlantapMacImpl, ddk::Initializable, ddk::Unbindable, ddk::ServiceConnectable>(
            phy_device),
        role_(role),
        phy_config_(phy_config),
        listener_(listener),
        sme_channel_(std::move(sme_channel)) {}

  zx_status_t InitWlanSoftmacIfcClient() {
    // Create dispatcher for FIDL client of WlanSoftmacIfc protocol.
    auto dispatcher = fdf::SynchronizedDispatcher::Create(
        {}, WLAN_SOFTMAC_IFC_DISPATCHER_NAME, [&](fdf_dispatcher_t*) {
          if (unbind_txn_) {
            unbind_txn_->Reply();
            return;
          }
          zxlogf(ERROR, "%s shutdown for reason other than MAC device unbind.",
                 WLAN_SOFTMAC_IFC_DISPATCHER_NAME);
        });

    if (dispatcher.is_error()) {
      return dispatcher.status_value();
    }

    wlan_softmac_ifc_dispatcher_ = *std::move(dispatcher);

    return ZX_OK;
  }

  zx_status_t InitWlanSoftmacServer() {
    // Create dispatcher for FIDL server of WlanSoftmac protocol.
    auto dispatcher = fdf::SynchronizedDispatcher::Create(
        {}, WLAN_SOFTMAC_DISPATCHER_NAME, [&](fdf_dispatcher_t*) {
          if (unbind_txn_) {
            wlan_softmac_ifc_dispatcher_.ShutdownAsync();
            return;
          }
          zxlogf(ERROR, "%s shutdown for reason other than MAC device unbind.",
                 WLAN_SOFTMAC_DISPATCHER_NAME);
        });
    if (dispatcher.is_error()) {
      return dispatcher.status_value();
    }
    wlan_softmac_dispatcher_ = *std::move(dispatcher);

    return ZX_OK;
  }

  void DdkInit(ddk::InitTxn txn) {
    zx_status_t ret = InitWlanSoftmacServer();
    ZX_ASSERT_MSG(ret == ZX_OK, "%s(): %s create failed%s\n", __func__,
                  WLAN_SOFTMAC_DISPATCHER_NAME, zx_status_get_string(ret));

    ret = InitWlanSoftmacIfcClient();
    ZX_ASSERT_MSG(ret == ZX_OK, "%s(): %s create failed%s\n", __func__,
                  WLAN_SOFTMAC_IFC_DISPATCHER_NAME, zx_status_get_string(ret));

    txn.Reply(ZX_OK);
  }

  void DdkUnbind(ddk::UnbindTxn txn) {
    // ddk::UnbindTxn::Reply() will be called when the WlanSoftmacIfc dispatcher is shutdown. This
    // DdkUnbind triggers the following sequence.
    //
    //   1. WlanSoftmac dispatcher ShutdownAsync() called.
    //   2. WlanSoftmac dispatcher shutdown handler calls WlanSoftmacIfc dispatcher ShutdownAsync().
    //   3. WlanSoftmacIfc dispatcher shutdown handler calls ddk::UnbindTxn::Reply().
    unbind_txn_ = std::move(txn);
    wlan_softmac_dispatcher_.ShutdownAsync();
  }

  void DdkRelease() { delete this; }

  zx_status_t DdkServiceConnect(const char* service_name, fdf::Channel channel) {
    fdf::ServerEnd<fuchsia_wlan_softmac::WlanSoftmac> server_end(std::move(channel));
    fdf::BindServer(wlan_softmac_dispatcher_.get(), std::move(server_end), this);
    return ZX_OK;
  }

  // WlanSoftmac protocol impl

  // Large enough to back a full WlanSoftmacInfo FIDL struct.
  static constexpr size_t kWlanSoftmacInfoBufferSize = 5120;

  void Query(fdf::Arena& arena, QueryCompleter::Sync& completer) override {
    fidl::Arena<kWlanSoftmacInfoBufferSize> table_arena;
    wlan_softmac::WlanSoftmacInfo softmac_info;
    ConvertTapPhyConfig(&softmac_info, *phy_config_, table_arena);
    completer.buffer(arena).ReplySuccess(softmac_info);
  }

  void QueryDiscoverySupport(fdf::Arena& arena,
                             QueryDiscoverySupportCompleter::Sync& completer) override {
    completer.buffer(arena).ReplySuccess(phy_config_->discovery_support);
  }

  void QueryMacSublayerSupport(fdf::Arena& arena,
                               QueryMacSublayerSupportCompleter::Sync& completer) override {
    completer.buffer(arena).ReplySuccess(phy_config_->mac_sublayer_support);
  }

  void QuerySecuritySupport(fdf::Arena& arena,
                            QuerySecuritySupportCompleter::Sync& completer) override {
    completer.buffer(arena).ReplySuccess(phy_config_->security_support);
  }

  void QuerySpectrumManagementSupport(
      fdf::Arena& arena, QuerySpectrumManagementSupportCompleter::Sync& completer) override {
    completer.buffer(arena).ReplySuccess(phy_config_->spectrum_management_support);
  }

  void Start(StartRequestView request, fdf::Arena& arena,
             StartCompleter::Sync& completer) override {
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (!sme_channel_.is_valid()) {
        completer.buffer(arena).ReplyError(ZX_ERR_ALREADY_BOUND);
        return;
      }
      wlan_softmac_ifc_client_ = fdf::WireSharedClient<fuchsia_wlan_softmac::WlanSoftmacIfc>(
          std::move(request->ifc), wlan_softmac_ifc_dispatcher_.get());
    }
    listener_->WlantapMacStart();
    completer.buffer(arena).ReplySuccess(std::move(sme_channel_));
  }

  void Stop(fdf::Arena& arena, StopCompleter::Sync& completer) override {
    listener_->WlantapMacStop();
    completer.buffer(arena).Reply();
  }

  void QueueTx(QueueTxRequestView request, fdf::Arena& arena,
               QueueTxCompleter::Sync& completer) override {
    listener_->WlantapMacQueueTx(request->packet);
    completer.buffer(arena).ReplySuccess(false);
  }

  void SetChannel(SetChannelRequestView request, fdf::Arena& arena,
                  SetChannelCompleter::Sync& completer) override {
    if (!wlan::common::IsValidChan(request->chan)) {
      completer.buffer(arena).ReplyError(ZX_ERR_INVALID_ARGS);
      return;
    }
    listener_->WlantapMacSetChannel(request->chan);
    completer.buffer(arena).ReplySuccess();
  }

  void ConfigureBss(ConfigureBssRequestView request, fdf::Arena& arena,
                    ConfigureBssCompleter::Sync& completer) override {
    bool expected_remote = role_ == wlan_common::WlanMacRole::kClient;
    if (request->config.remote != expected_remote) {
      completer.buffer(arena).ReplyError(ZX_ERR_INVALID_ARGS);
      return;
    }
    listener_->WlantapMacConfigureBss(request->config);
    completer.buffer(arena).ReplySuccess();
  }

  void EnableBeaconing(EnableBeaconingRequestView request, fdf::Arena& arena,
                       EnableBeaconingCompleter::Sync& completer) override {
    // This is the test driver, so we can just pretend beaconing was enabled.
    completer.buffer(arena).ReplySuccess();
  }

  void ConfigureBeacon(ConfigureBeaconRequestView request, fdf::Arena& arena,
                       ConfigureBeaconCompleter::Sync& completer) override {
    // This is the test driver, so we can just pretend the beacon was configured.
    completer.buffer(arena).ReplySuccess();
  }

  void StartPassiveScan(StartPassiveScanRequestView request, fdf::Arena& arena,
                        StartPassiveScanCompleter::Sync& completer) override {
    uint64_t scan_id = 111;
    listener_->WlantapMacStartScan(scan_id);
    completer.buffer(arena).ReplySuccess(scan_id);
  }

  void StartActiveScan(StartActiveScanRequestView request, fdf::Arena& arena,
                       StartActiveScanCompleter::Sync& completer) override {
    uint64_t scan_id = 222;
    listener_->WlantapMacStartScan(scan_id);
    completer.buffer(arena).ReplySuccess(scan_id);
  }

  void SetKey(SetKeyRequestView request, fdf::Arena& arena,
              SetKeyCompleter::Sync& completer) override {
    listener_->WlantapMacSetKey(request->key_config);
    completer.buffer(arena).ReplySuccess();
  }

  void ConfigureAssoc(ConfigureAssocRequestView request, fdf::Arena& arena,
                      ConfigureAssocCompleter::Sync& completer) override {
    // This is the test driver, so we can just pretend the association was configured.
    // TODO(fxbug.dev/28907): Evaluate the use and implement
    completer.buffer(arena).ReplySuccess();
  }

  void ClearAssoc(ClearAssocRequestView request, fdf::Arena& arena,
                  ClearAssocCompleter::Sync& completer) override {
    // TODO(fxbug.dev/28907): Evaluate the use and implement. Association is never
    // configured, so there is nothing to clear.
    completer.buffer(arena).ReplySuccess();
  }

  void CancelScan(CancelScanRequestView request, fdf::Arena& arena,
                  CancelScanCompleter::Sync& completer) override {
    ZX_PANIC("CancelScan is not supported.");
  }

  void UpdateWmmParams(UpdateWmmParamsRequestView request, fdf::Arena& arena,
                       UpdateWmmParamsCompleter::Sync& completer) override {
    ZX_PANIC("UpdateWmmParams is not supported.");
  }

  // WlantapMac impl

  virtual void Rx(const fidl::VectorView<uint8_t>& data,
                  const wlan_tap::WlanRxInfo& rx_info) override {
    std::lock_guard<std::mutex> guard(lock_);

    wlan_softmac::WlanRxInfoFlags rx_flags =
        wlan_softmac::WlanRxInfoFlags::TruncatingUnknown(rx_info.rx_flags);
    wlan_softmac::WlanRxInfo converted_info = {.rx_flags = rx_flags,
                                               .valid_fields = rx_info.valid_fields,
                                               .phy = rx_info.phy,
                                               .data_rate = rx_info.data_rate,
                                               .channel = rx_info.channel,
                                               .mcs = rx_info.mcs,
                                               .rssi_dbm = rx_info.rssi_dbm,
                                               .snr_dbh = rx_info.snr_dbh};
    wlan_softmac::WlanRxPacket rx_packet = {.mac_frame = data, .info = converted_info};
    auto arena = fdf::Arena::Create(0, 0);
    auto result = wlan_softmac_ifc_client_.sync().buffer(*arena)->Recv(rx_packet);
    if (!result.ok()) {
      zxlogf(ERROR, "Failed to send rx frames up. Status: %d\n", result.status());
    }
  }

  virtual void Status(uint32_t status) override {
    std::lock_guard<std::mutex> guard(lock_);
    auto arena = fdf::Arena::Create(0, 0);
    auto result = wlan_softmac_ifc_client_.sync().buffer(*arena)->Status(status);
    if (!result.ok()) {
      zxlogf(ERROR, "Failed to send status up. Status: %d\n", result.status());
    }
  }

  virtual void ReportTxStatus(const wlan_common::WlanTxStatus& ts) override {
    std::lock_guard<std::mutex> guard(lock_);
    auto arena = fdf::Arena::Create(0, 0);
    auto result = wlan_softmac_ifc_client_.sync().buffer(*arena)->ReportTxStatus(ts);
    if (!result.ok()) {
      zxlogf(ERROR, "Failed to report tx status up. Status: %d\n", result.status());
    }
  }

  virtual void ScanComplete(uint64_t scan_id, int32_t status) override {
    std::lock_guard<std::mutex> guard(lock_);
    auto arena = fdf::Arena::Create(0, 0);
    auto result = wlan_softmac_ifc_client_.sync().buffer(*arena)->ScanComplete(status, scan_id);
    if (!result.ok()) {
      zxlogf(ERROR, "Failed to send scan complete notification up. Status: %d\n", result.status());
    }
  }

  virtual void RemoveDevice() override { DdkAsyncRemove(); }

  uint16_t id_;
  wlan_common::WlanMacRole role_;
  std::mutex lock_;
  // The FIDL client to communicate with Wlan device.
  fdf::WireSharedClient<fuchsia_wlan_softmac::WlanSoftmacIfc> wlan_softmac_ifc_client_;

  const std::shared_ptr<const wlan_tap::WlantapPhyConfig> phy_config_;
  Listener* listener_;
  zx::channel sme_channel_;

  // Dispatcher for FIDL client of WlanSoftmacIfc protocol.
  const char* WLAN_SOFTMAC_IFC_DISPATCHER_NAME = "wlan-softmac-ifc-client";
  fdf::Dispatcher wlan_softmac_ifc_dispatcher_;

  // Dispatcher for FIDL server of WlanSoftmac protocol.
  const char* WLAN_SOFTMAC_DISPATCHER_NAME = "wlan-softmac-server";
  fdf::Dispatcher wlan_softmac_dispatcher_;

  // Store unbind txn for async reply.
  std::optional<::ddk::UnbindTxn> unbind_txn_;
};

}  // namespace

zx_status_t CreateWlantapMac(zx_device_t* parent_phy, const wlan_common::WlanMacRole role,
                             const std::shared_ptr<const wlan_tap::WlantapPhyConfig> phy_config,
                             WlantapMac::Listener* listener, zx::channel sme_channel,
                             WlantapMac** ret) {
  static uint16_t n = 0;
  char name[ZX_MAX_NAME_LEN + 1];
  snprintf(name, sizeof(name), "wlansoftmac-%u", n++);
  std::unique_ptr<WlantapMacImpl> wlan_softmac(
      new WlantapMacImpl(parent_phy, role, phy_config, listener, std::move(sme_channel)));

  zx_status_t status =
      wlan_softmac->DdkAdd(::ddk::DeviceAddArgs(name).set_proto_id(ZX_PROTOCOL_WLAN_SOFTMAC));
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: could not add device: %d", __func__, status);
    return status;
  }
  // Transfer ownership to devmgr
  *ret = wlan_softmac.release();
  return ZX_OK;
}

}  // namespace wlan
