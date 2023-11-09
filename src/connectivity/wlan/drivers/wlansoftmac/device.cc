// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device.h"

#include <fidl/fuchsia.wlan.softmac/cpp/fidl.h>
#include <fuchsia/hardware/ethernet/cpp/banjo.h>
#include <fuchsia/wlan/common/c/banjo.h>
#include <fuchsia/wlan/ieee80211/c/banjo.h>
#include <fuchsia/wlan/internal/c/banjo.h>
#include <fuchsia/wlan/softmac/c/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/ddk/device.h>
#include <lib/fit/result.h>
#include <lib/zx/result.h>
#include <lib/zx/thread.h>
#include <lib/zx/time.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/port.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#include <wlan/common/channel.h>
#include <wlan/drivers/log.h>
#include <wlan/mlme/validate_frame.h>
#include <wlan/mlme/wlan.h>

#include "convert.h"
#include "device_interface.h"

namespace wlan {

wlansoftmac_in_buf_t IntoRustInBuf(std::unique_ptr<Packet> packet) {
  auto* pkt = packet.release();
  return wlansoftmac_in_buf_t{
      .free_buffer = [](void* raw) { std::unique_ptr<Packet>(static_cast<Packet*>(raw)).reset(); },
      .raw = pkt,
      .data = pkt->data(),
      .len = pkt->len(),
  };
}

std::unique_ptr<Packet> FromRustOutBuf(wlansoftmac_out_buf_t buf) {
  if (!buf.raw) {
    return {};
  }
  auto pkt = std::unique_ptr<Packet>(static_cast<Packet*>(buf.raw));
  pkt->set_len(buf.written_bytes);
  return pkt;
}

wlansoftmac_buffer_provider_ops_t rust_buffer_provider{
    .get_buffer = [](size_t min_len) -> wlansoftmac_in_buf_t {
      // Note: Once Rust MLME supports more than sending WLAN frames this needs
      // to change.
      auto pkt = GetWlanPacket(min_len);
      ZX_DEBUG_ASSERT(pkt != nullptr);
      return IntoRustInBuf(std::move(pkt));
    },
};

namespace {

constexpr inline DeviceInterface* DEVICE(void* c) { return static_cast<DeviceInterface*>(c); }
constexpr inline Device* DEV(void* ctx) { return static_cast<Device*>(ctx); }

zx_protocol_device_t eth_device_ops = {
    .version = DEVICE_OPS_VERSION,
    .unbind = [](void* ctx) { DEV(ctx)->EthUnbind(); },
    .release = [](void* ctx) { DEV(ctx)->EthReleaseAndDeleteThis(); },
};

ethernet_impl_protocol_ops_t ethernet_impl_ops = {
    .query = [](void* ctx, uint32_t options, ethernet_info_t* info) -> zx_status_t {
      return DEV(ctx)->EthernetImplQuery(options, info);
    },
    .stop = [](void* ctx) { DEV(ctx)->EthernetImplStop(); },
    .start = [](void* ctx, const ethernet_ifc_protocol_t* ifc) -> zx_status_t {
      return DEV(ctx)->EthernetImplStart(ifc);
    },
    .queue_tx =
        [](void* ctx, uint32_t options, ethernet_netbuf_t* netbuf,
           ethernet_impl_queue_tx_callback completion_cb,
           void* cookie) { DEV(ctx)->EthernetImplQueueTx(options, netbuf, completion_cb, cookie); },
    .set_param = [](void* ctx, uint32_t param, int32_t value, const uint8_t* data, size_t data_size)
        -> zx_status_t { return Device::EthernetImplSetParam(param, value, data, data_size); },
};

}  // namespace

WlanSoftmacHandle::WlanSoftmacHandle(DeviceInterface* device)
    : device_(device),
      inner_handle_(nullptr),
      wlan_softmac_bridge_server_loop_(&kAsyncLoopConfigNeverAttachToThread) {
  ldebug(0, nullptr, "Entering.");
}

WlanSoftmacHandle::~WlanSoftmacHandle() {
  ldebug(0, nullptr, "Entering.");
  if (inner_handle_ != nullptr) {
    delete_sta(inner_handle_);
  }
}

// WlanSoftmacBridgeImpl hosts methods migrated from Banjo data structures
// to FIDL data structures. This server implementation was modeled after
// https://fuchsia.dev/fuchsia-src/development/languages/fidl/tutorials/cpp/basics/server.
class WlanSoftmacBridgeImpl : public fidl::WireServer<fuchsia_wlan_softmac::WlanSoftmacBridge> {
 public:
  explicit WlanSoftmacBridgeImpl(fdf::WireSharedClient<fuchsia_wlan_softmac::WlanSoftmac> client)
      : client_(std::move(client)) {}

  void Query(QueryCompleter::Sync& completer) override {
    Dispatcher<fuchsia_wlan_softmac::WlanSoftmac::Query> dispatcher =
        [](const auto& arena, const auto& client) { return client.sync().buffer(arena)->Query(); };
    DispatchAndComplete(__func__, dispatcher, completer);
  }

  void SetChannel(SetChannelRequestView request, SetChannelCompleter::Sync& completer) override {
    Dispatcher<fuchsia_wlan_softmac::WlanSoftmac::SetChannel> dispatcher =
        [request](const auto& arena, const auto& client) {
          return client.sync().buffer(arena)->SetChannel(*request);
        };
    DispatchAndComplete(__func__, dispatcher, completer);
  }

  void NotifyAssociationComplete(NotifyAssociationCompleteRequestView request,
                                 NotifyAssociationCompleteCompleter::Sync& completer) override {
    Dispatcher<fuchsia_wlan_softmac::WlanSoftmac::NotifyAssociationComplete> dispatcher =
        [request](const auto& arena, const auto& client) {
          return client.sync().buffer(arena)->NotifyAssociationComplete(request->assoc_cfg);
        };
    DispatchAndComplete(__func__, dispatcher, completer);
  }

  void ClearAssociation(ClearAssociationRequestView request,
                        ClearAssociationCompleter::Sync& completer) override {
    Dispatcher<fuchsia_wlan_softmac::WlanSoftmac::ClearAssociation> dispatcher =
        [request](const auto& arena, const auto& client) {
          return client.sync().buffer(arena)->ClearAssociation(*request);
        };
    DispatchAndComplete(__func__, dispatcher, completer);
  }

  void StartPassiveScan(StartPassiveScanRequestView request,
                        StartPassiveScanCompleter::Sync& completer) override {
    Dispatcher<fuchsia_wlan_softmac::WlanSoftmac::StartPassiveScan> dispatcher =
        [request](const auto& arena, const auto& client) {
          return client.sync().buffer(arena)->StartPassiveScan(*request);
        };
    DispatchAndComplete(__func__, dispatcher, completer);
  }

  void StartActiveScan(StartActiveScanRequestView request,
                       StartActiveScanCompleter::Sync& completer) override {
    Dispatcher<fuchsia_wlan_softmac::WlanSoftmac::StartActiveScan> dispatcher =
        [request](const auto& arena, const auto& client) {
          return client.sync().buffer(arena)->StartActiveScan(*request);
        };
    DispatchAndComplete(__func__, dispatcher, completer);
  }

  void CancelScan(CancelScanRequestView request, CancelScanCompleter::Sync& completer) override {
    Dispatcher<fuchsia_wlan_softmac::WlanSoftmac::CancelScan> dispatcher =
        [request](const auto& arena, const auto& client) {
          return client.sync().buffer(arena)->CancelScan(*request);
        };
    DispatchAndComplete(__func__, dispatcher, completer);
  }

  static void BindSelfManagedServer(
      async_dispatcher_t* dispatcher,
      fdf::WireSharedClient<fuchsia_wlan_softmac::WlanSoftmac> client,
      fidl::ServerEnd<fuchsia_wlan_softmac::WlanSoftmacBridge> server_end) {
    std::unique_ptr impl = std::make_unique<WlanSoftmacBridgeImpl>(std::move(client));
    fidl::BindServer(dispatcher, std::move(server_end), std::move(impl),
                     &WlanSoftmacBridgeImpl::OnUnbound);
  }

  // TODO(issues.fuchsia.dev/306181180): This method should probably trigger
  // shutdown of the device if called outside of a normal shutdown sequence.
  static void OnUnbound([[maybe_unused]] WlanSoftmacBridgeImpl* impl_ptr, fidl::UnbindInfo info,
                        fidl::ServerEnd<fuchsia_wlan_softmac::WlanSoftmacBridge> server_end) {
    if (info.is_user_initiated()) {
      // This is part of the normal shutdown sequence.
    } else if (info.is_peer_closed()) {
      linfo("WlanSoftmacBridge client disconnected");
    } else {
      lerror("WlanSoftmacBridge server error: %s", info.status_string());
    }
  }

 private:
  template <typename, typename = void>
  static constexpr bool has_value_type = false;
  template <typename T>
  static constexpr bool has_value_type<T, std::void_t<typename T::value_type>> = true;

  template <typename FidlMethod>
  static fidl::WireResultUnwrapType<FidlMethod> LogAndFoldResult(
      const std::string& method_name, fdf::WireUnownedResult<FidlMethod> result) {
    if (!result.ok()) {
      lerror("%s failed (FIDL error %s)", method_name.c_str(), result.status_string());
      return fit::error(result.status());
    }
    if (result->is_error()) {
      lerror("%s failed (status %s)", method_name.c_str(),
             zx_status_get_string(result->error_value()));
      return fit::error(result->error_value());
    }

    if constexpr (has_value_type<fidl::WireResultUnwrapType<FidlMethod>>) {
      return fit::success(std::move(result->value()));
    } else
      return fit::ok();
  }

  template <typename FidlMethod>
  using Dispatcher = std::function<fdf::WireUnownedResult<FidlMethod>(
      const fdf::Arena&, const fdf::WireSharedClient<fuchsia_wlan_softmac::WlanSoftmac>&)>;

  template <typename Completer, typename FidlMethod>
  void DispatchAndComplete(const std::string& method_name, Dispatcher<FidlMethod> dispatcher,
                           Completer& completer) {
    auto arena = fdf::Arena::Create(0, 0);
    if (arena.is_error()) {
      lerror("Arena creation failed: %s", arena.status_string());
      completer.ReplyError(ZX_ERR_INTERNAL);
      return;
    }

    auto result = LogAndFoldResult(method_name, dispatcher(*std::move(arena), client_));

    if (result.is_error()) {
      completer.ReplyError(result.error_value());
      return;
    }
    if constexpr (has_value_type<fidl::WireResultUnwrapType<FidlMethod>>) {
      completer.ReplySuccess(*std::move(result.value()));
    } else {
      completer.ReplySuccess();
    }
  }

  fdf::WireSharedClient<fuchsia_wlan_softmac::WlanSoftmac> client_;
};

zx_status_t WlanSoftmacHandle::Init(
    fdf::WireSharedClient<fuchsia_wlan_softmac::WlanSoftmac> client) {
  if (inner_handle_ != nullptr) {
    return ZX_ERR_BAD_STATE;
  }

  rust_device_interface_t wlansoftmac_rust_ops = {
      .device = static_cast<void*>(this->device_),
      .start = [](void* device, const rust_wlan_softmac_ifc_protocol_copy_t* ifc,
                  zx_handle_t* out_sme_channel) -> zx_status_t {
        zx::channel channel;
        zx_status_t result = DEVICE(device)->Start(ifc, &channel);
        *out_sme_channel = channel.release();
        return result;
      },
      .deliver_eth_frame = [](void* device, const uint8_t* data, size_t len) -> zx_status_t {
        return DEVICE(device)->DeliverEthernet({data, len});
      },
      .queue_tx = [](void* device, uint32_t options, wlansoftmac_out_buf_t buf,
                     wlan_tx_info_t tx_info) -> zx_status_t {
        auto pkt = FromRustOutBuf(buf);
        return DEVICE(device)->QueueTx(std::move(pkt), tx_info);
      },
      .set_ethernet_status = [](void* device, uint32_t status) -> zx_status_t {
        return DEVICE(device)->SetEthernetStatus(status);
      },
      .set_key = [](void* device, wlan_key_configuration_t* key) -> zx_status_t {
        return DEVICE(device)->InstallKey(key);
      },
      .get_discovery_support = [](void* device) -> discovery_support_t {
        return DEVICE(device)->GetDiscoverySupport();
      },
      .get_mac_sublayer_support = [](void* device) -> mac_sublayer_support_t {
        return DEVICE(device)->GetMacSublayerSupport();
      },
      .get_security_support = [](void* device) -> security_support_t {
        return DEVICE(device)->GetSecuritySupport();
      },
      .get_spectrum_management_support = [](void* device) -> spectrum_management_support_t {
        return DEVICE(device)->GetSpectrumManagementSupport();
      },
      .join_bss = [](void* device, join_bss_request_t* cfg) -> zx_status_t {
        return DEVICE(device)->JoinBss(cfg);
      },
      .enable_beaconing = [](void* device, wlansoftmac_out_buf_t buf, size_t tim_ele_offset,
                             uint16_t beacon_interval) -> zx_status_t {
        auto pkt = FromRustOutBuf(buf);
        wlan_softmac_enable_beaconing_request_t request = {
            .packet_template =
                {
                    .mac_frame_buffer = pkt->data(),
                    .mac_frame_size = pkt->size(),
                },
            .tim_ele_offset = tim_ele_offset,
            .beacon_interval = beacon_interval,
        };
        return DEVICE(device)->EnableBeaconing(&request);
      },
      .disable_beaconing = [](void* device) -> zx_status_t {
        return DEVICE(device)->DisableBeaconing();
      },
  };

  auto endpoints = fidl::CreateEndpoints<fuchsia_wlan_softmac::WlanSoftmacBridge>();
  WlanSoftmacBridgeImpl::BindSelfManagedServer(wlan_softmac_bridge_server_loop_.dispatcher(),
                                               std::move(client), std::move(endpoints->server));
  wlan_softmac_bridge_server_loop_.StartThread("wlansoftmac-migration-fidl-server");

  inner_handle_ = start_sta(wlansoftmac_rust_ops, rust_buffer_provider,
                            endpoints->client.TakeHandle().release());

  return ZX_OK;
}

zx_status_t WlanSoftmacHandle::StopMainLoop() {
  if (inner_handle_ == nullptr) {
    return ZX_ERR_BAD_STATE;
  }
  stop_sta(inner_handle_);
  return ZX_OK;
}

zx_status_t WlanSoftmacHandle::QueueEthFrameTx(std::unique_ptr<Packet> pkt) {
  if (inner_handle_ == nullptr) {
    return ZX_ERR_BAD_STATE;
  }
  wlan_span_t span{.data = pkt->data(), .size = pkt->len()};
  sta_queue_eth_frame_tx(inner_handle_, span);
  return ZX_OK;
}

Device::Device(zx_device_t* device) : parent_(device) {
  ldebug(0, nullptr, "Entering.");
  linfo("Creating a new WLAN device.");
  state_ = fbl::AdoptRef(new DeviceState);
  // Create a dispatcher to wait on the runtime channel.
  auto dispatcher = fdf::SynchronizedDispatcher::Create(
      fdf::SynchronizedDispatcher::Options::kAllowSyncCalls, "wlansoftmacifc_server",
      [&](fdf_dispatcher_t*) { device_unbind_reply(ethdev_); });

  if (dispatcher.is_error()) {
    ZX_ASSERT_MSG(false, "Creating server dispatcher error: %s",
                  zx_status_get_string(dispatcher.status_value()));
  }

  server_dispatcher_ = *std::move(dispatcher);

  // Create a dispatcher for Wlansoftmac device as a FIDL client.
  dispatcher = fdf::SynchronizedDispatcher::Create(
      fdf::SynchronizedDispatcher::Options::kAllowSyncCalls, "wlansoftmac_client",
      [&](fdf_dispatcher_t*) { server_dispatcher_.ShutdownAsync(); });

  if (dispatcher.is_error()) {
    ZX_ASSERT_MSG(false, "Creating client dispatcher error: %s",
                  zx_status_get_string(dispatcher.status_value()));
  }

  client_dispatcher_ = *std::move(dispatcher);
}

Device::~Device() { ldebug(0, nullptr, "Entering."); }

// Disable thread safety analysis, as this is a part of device initialization.
// All thread-unsafe work should occur before multiple threads are possible
// (e.g., before MainLoop is started and before DdkAdd() is called), or locks
// should be held.
zx_status_t Device::Bind() __TA_NO_THREAD_SAFETY_ANALYSIS {
  ldebug(0, nullptr, "Entering.");
  linfo("Binding our new WLAN softmac device.");

  auto endpoints = fdf::CreateEndpoints<fuchsia_wlan_softmac::Service::WlanSoftmac::ProtocolType>();
  if (endpoints.is_error()) {
    lerror("Failed to create FDF endpoints: %s", zx_status_get_string(endpoints.status_value()));
    return endpoints.status_value();
  }

  auto status = device_connect_runtime_protocol(
      parent_, fuchsia_wlan_softmac::Service::WlanSoftmac::ServiceName,
      fuchsia_wlan_softmac::Service::WlanSoftmac::Name, endpoints->server.TakeChannel().release());
  if (status != ZX_OK) {
    lerror("Failed to connect to WlanSoftmac service: %s", zx_status_get_string(status));
    return status;
  }

  client_ = fdf::WireSharedClient(std::move(endpoints->client), client_dispatcher_.get());

  auto arena = fdf::Arena::Create(0, 0);
  if (arena.is_error()) {
    lerror("Arena creation failed: %s", arena.status_string());
    return ZX_ERR_INTERNAL;
  }

  auto query_result = client_.sync().buffer(*std::move(arena))->Query();
  if (!query_result.ok()) {
    lerror("Failed getting query result (FIDL error %s)", query_result.status_string());
    return query_result.status();
  }
  if (query_result->is_error()) {
    lerror("Failed getting query result (status %s)",
           zx_status_get_string(query_result->error_value()));
    return query_result->error_value();
  }
  // Take ownership of the data in the wire representation's arena.
  if (!query_result->value()->has_sta_addr()) {
    lerror("Query result missing sta_addr.");
    return ZX_ERR_INTERNAL;
  }

  auto discovery_arena = fdf::Arena::Create(0, 0);
  if (discovery_arena.is_error()) {
    lerror("Arena creation failed: %s", discovery_arena.status_string());
    return ZX_ERR_INTERNAL;
  }

  auto discovery_result =
      client_.sync().buffer(*std::move(discovery_arena))->QueryDiscoverySupport();
  if (!discovery_result.ok()) {
    lerror("Failed getting discovery result (FIDL error %s)", discovery_result.status_string());
    return discovery_result.status();
  }

  ConvertDiscoverySuppport(discovery_result->value()->resp, &discovery_support_);

  auto mac_sublayer_arena = fdf::Arena::Create(0, 0);
  if (mac_sublayer_arena.is_error()) {
    lerror("Arena creation failed: %s", mac_sublayer_arena.status_string());
    return ZX_ERR_INTERNAL;
  }

  auto mac_sublayer_result =
      client_.sync().buffer(*std::move(mac_sublayer_arena))->QueryMacSublayerSupport();
  if (!mac_sublayer_result.ok()) {
    lerror("Failed getting mac sublayer result (FIDL error %s)",
           mac_sublayer_result.status_string());
    return mac_sublayer_result.status();
  }

  status = ConvertMacSublayerSupport(mac_sublayer_result->value()->resp, &mac_sublayer_support_);
  if (status != ZX_OK) {
    lerror("MacSublayerSupport conversion failed (%s)", zx_status_get_string(status));
    return status;
  }

  auto security_arena = fdf::Arena::Create(0, 0);
  if (security_arena.is_error()) {
    lerror("Arena creation failed: %s", security_arena.status_string());
    return ZX_ERR_INTERNAL;
  }

  auto security_result = client_.sync().buffer(*std::move(security_arena))->QuerySecuritySupport();
  if (!security_result.ok()) {
    lerror("Failed getting security result (FIDL error %s)", security_result.status_string());
    return security_result.status();
  }

  ConvertSecuritySupport(security_result->value()->resp, &security_support_);

  auto spectrum_management_arena = fdf::Arena::Create(0, 0);
  if (spectrum_management_arena.is_error()) {
    lerror("Arena creation failed: %s", spectrum_management_arena.status_string());
    return ZX_ERR_INTERNAL;
  }

  auto spectrum_management_result = client_.sync()
                                        .buffer(*std::move(spectrum_management_arena))
                                        ->QuerySpectrumManagementSupport();

  if (!spectrum_management_result.ok()) {
    lerror("Failed getting spectrum management result (FIDL error %s)",
           spectrum_management_result.status_string());
    return spectrum_management_result.status();
  }

  ConvertSpectrumManagementSupport(spectrum_management_result->value()->resp,
                                   &spectrum_management_support_);

  /* End of data type conversion. */

  softmac_handle_ = std::make_unique<WlanSoftmacHandle>(this);
  status = softmac_handle_->Init(client_.Clone());
  if (status != ZX_OK) {
    lerror("could not initialize Rust WlanSoftmac: %d", status);
    return status;
  }

  status = AddEthDevice();
  if (status != ZX_OK) {
    lerror("could not add eth device: %s", zx_status_get_string(status));
    softmac_handle_->StopMainLoop();
    return status;
  }

  debugf("device added");
  return ZX_OK;
}

zx_status_t Device::AddEthDevice() {
  device_add_args_t args = {};
  args.version = DEVICE_ADD_ARGS_VERSION;
  args.name = "wlansoftmac-ethernet";
  args.ctx = this;
  args.ops = &eth_device_ops;
  args.proto_id = ZX_PROTOCOL_ETHERNET_IMPL;
  args.proto_ops = &ethernet_impl_ops;
  return device_add(parent_, &args, &ethdev_);
}

std::unique_ptr<Packet> Device::PreparePacket(const void* data, size_t length, Packet::Peer peer) {
  std::unique_ptr<Buffer> buffer = GetBuffer(length);
  if (buffer == nullptr) {
    lerror("could not get buffer for packet of length %zu", length);
    return nullptr;
  }

  auto packet = std::make_unique<Packet>(std::move(buffer), length);
  packet->set_peer(peer);
  zx_status_t status = packet->CopyFrom(data, length, 0);
  if (status != ZX_OK) {
    lerror("could not copy to packet: %d", status);
    return nullptr;
  }
  return packet;
}

void Device::ShutdownMainLoop() {
  if (main_loop_dead_) {
    lerror("ShutdownMainLoop called while main loop was not running");
    return;
  }
  softmac_handle_->StopMainLoop();
  main_loop_dead_ = true;
}

// ddk ethernet_impl_protocol_ops methods

void Device::EthUnbind() {
  ldebug(0, nullptr, "Entering.");
  ShutdownMainLoop();
  client_dispatcher_.ShutdownAsync();
}

void Device::EthReleaseAndDeleteThis() {
  ldebug(0, nullptr, "Entering.");
  // The lifetime of this device is managed by the parent ethernet device, but we don't
  // have a mechanism to make this explicit. EthUnbind is already called at this point,
  // so it's safe to clean up our memory usage.
  delete this;
}

zx_status_t Device::EthernetImplQuery(uint32_t options, ethernet_info_t* info) {
  ldebug(0, nullptr, "Entering.");
  if (info == nullptr)
    return ZX_ERR_INVALID_ARGS;

  auto arena = fdf::Arena::Create(0, 0);
  if (arena.is_error()) {
    lerror("Arena creation failed: %s", arena.status_string());
    return ZX_ERR_INTERNAL;
  }

  auto query_result = client_.sync().buffer(*std::move(arena))->Query();
  if (!query_result.ok()) {
    lerror("Failed getting query result (FIDL error %s)", query_result.status_string());
    return query_result.status();
  }
  if (query_result->is_error()) {
    lerror("Failed getting query result (status %s)",
           zx_status_get_string(query_result->error_value()));
    return query_result->error_value();
  }

  memset(info, 0, sizeof(*info));
  common::MacAddr(query_result->value()->sta_addr().data()).CopyTo(info->mac);
  info->features = ETHERNET_FEATURE_WLAN;

  auto mac_sublayer_result = client_.sync().buffer(*std::move(arena))->QueryMacSublayerSupport();
  if (!mac_sublayer_result.ok()) {
    lerror("Failed getting mac sublayer result (FIDL error %s)",
           mac_sublayer_result.status_string());
    return mac_sublayer_result.status();
  }

  mac_sublayer_support_t mac_sublayer;
  zx_status_t status = ConvertMacSublayerSupport(mac_sublayer_result->value()->resp, &mac_sublayer);
  if (status != ZX_OK) {
    lerror("MacSublayerSupport conversion failed (%s)", zx_status_get_string(status));
    return status;
  }

  if (mac_sublayer.device.is_synthetic) {
    info->features |= ETHERNET_FEATURE_SYNTH;
  }
  info->mtu = 1500;
  info->netbuf_size = eth::BorrowedOperation<>::OperationSize(sizeof(ethernet_netbuf_t));

  return ZX_OK;
}

zx_status_t Device::EthernetImplStart(const ethernet_ifc_protocol_t* ifc) {
  ldebug(0, nullptr, "Entering.");
  ZX_DEBUG_ASSERT(ifc != nullptr);

  std::lock_guard<std::mutex> lock(ethernet_proxy_lock_);
  if (ethernet_proxy_.is_valid()) {
    return ZX_ERR_ALREADY_BOUND;
  }
  ethernet_proxy_ = ddk::EthernetIfcProtocolClient(ifc);
  return ZX_OK;
}

void Device::EthernetImplStop() {
  ldebug(0, nullptr, "Entering.");

  std::lock_guard<std::mutex> lock(ethernet_proxy_lock_);
  if (!ethernet_proxy_.is_valid()) {
    lwarn("ethmac not started");
  }
  ethernet_proxy_.clear();
}

void Device::EthernetImplQueueTx(uint32_t options, ethernet_netbuf_t* netbuf,
                                 ethernet_impl_queue_tx_callback completion_cb, void* cookie) {
  eth::BorrowedOperation<> op(netbuf, completion_cb, cookie, sizeof(ethernet_netbuf_t));
  // Do not log "Entering" because it's too noisy.
  auto packet = PreparePacket(op.operation()->data_buffer, op.operation()->data_size,
                              Packet::Peer::kEthernet);
  if (packet == nullptr) {
    lwarn("could not prepare Ethernet packet with len %zu", netbuf->data_size);
    op.Complete(ZX_ERR_NO_RESOURCES);
    return;
  }

  // Forward the packet straight into Rust MLME.
  auto status = softmac_handle_->QueueEthFrameTx(std::move(packet));
  if (status != ZX_OK) {
    lwarn("could not queue Ethernet packet err=%s", zx_status_get_string(status));
    ZX_DEBUG_ASSERT(status != ZX_ERR_SHOULD_WAIT);
  }
  op.Complete(status);
}

zx_status_t Device::EthernetImplSetParam(uint32_t param, int32_t value, const void* data,
                                         size_t data_size) {
  ldebug(0, nullptr, "Entering.");
  if (param == ETHERNET_SETPARAM_PROMISC) {
    // See fxbug.dev/28881: In short, the bridge mode doesn't require WLAN
    // promiscuous mode enabled.
    //               So we give a warning and return OK here to continue the
    //               bridging.
    // TODO(fxbug.dev/29113): To implement the real promiscuous mode.
    if (value == 1) {  // Only warn when enabling.
      lwarn("WLAN promiscuous not supported yet. see fxbug.dev/29113");
    }
    return ZX_OK;
  }
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t Device::Start(const rust_wlan_softmac_ifc_protocol_copy_t* ifc,
                          zx::channel* out_sme_channel) {
  debugf("Start");

  auto arena = fdf::Arena::Create(0, 0);
  if (arena.is_error()) {
    lerror("Arena creation failed: %s", arena.status_string());
    return ZX_ERR_INTERNAL;
  }

  auto endpoints = fdf::CreateEndpoints<fuchsia_wlan_softmac::WlanSoftmacIfc>();
  if (endpoints.is_error()) {
    lerror("Creating end point error: %s", zx_status_get_string(endpoints.status_value()));
    return endpoints.status_value();
  }

  fdf::BindServer(server_dispatcher_.get(), std::move(endpoints->server), this);

  // The protocol functions are stored in this class, which will act as
  // the server end of WlanSoftmacifc FIDL protocol, and this set of function pointers will be
  // called in the handler functions of FIDL server end.
  wlan_softmac_ifc_protocol_ops_ =
      std::make_unique<wlan_softmac_ifc_protocol_ops_t>(wlan_softmac_ifc_protocol_ops_t{
          .recv = ifc->ops->recv,
          .report_tx_result = ifc->ops->report_tx_result,
          .notify_scan_complete = ifc->ops->scan_complete,
      });

  wlan_softmac_ifc_protocol_ = std::make_unique<wlan_softmac_ifc_protocol_t>();
  wlan_softmac_ifc_protocol_->ops = wlan_softmac_ifc_protocol_ops_.get();
  wlan_softmac_ifc_protocol_->ctx = ifc->ctx;

  auto result = client_.sync().buffer(*std::move(arena))->Start(std::move(endpoints->client));
  if (!result.ok()) {
    lerror("change channel failed (FIDL error %s)", result.status_string());
    return result.status();
  }
  if (result->is_error()) {
    lerror("change channel failed (status %d)", result->error_value());
    return result->error_value();
  }
  *out_sme_channel = std::move(result->value()->sme_channel);
  return ZX_OK;
}

zx_status_t Device::DeliverEthernet(cpp20::span<const uint8_t> eth_frame) {
  if (eth_frame.size() > ETH_FRAME_MAX_SIZE) {
    lerror("Attempted to deliver an ethernet frame of invalid length: %zu", eth_frame.size());
    return ZX_ERR_INVALID_ARGS;
  }

  std::lock_guard<std::mutex> lock(ethernet_proxy_lock_);
  if (ethernet_proxy_.is_valid()) {
    ethernet_proxy_.Recv(eth_frame.data(), eth_frame.size(), 0u);
  }
  return ZX_OK;
}

zx_status_t Device::QueueTx(std::unique_ptr<Packet> packet, wlan_tx_info_t tx_info) {
  ZX_DEBUG_ASSERT(packet->len() <= std::numeric_limits<uint16_t>::max());

  auto arena = fdf::Arena::Create(0, 0);
  if (arena.is_error()) {
    lerror("Arena creation failed: %s", arena.status_string());
    return ZX_ERR_INTERNAL;
  }

  fuchsia_wlan_softmac::wire::WlanTxPacket fidl_tx_packet;
  zx_status_t status = status =
      ConvertTxPacket(packet->data(), packet->len(), tx_info, &fidl_tx_packet);
  if (status != ZX_OK) {
    lerror("WlanTxPacket conversion failed: %s", zx_status_get_string(status));
    return status;
  }

  auto result = client_.sync().buffer(*arena)->QueueTx(fidl_tx_packet);

  if (!result.ok()) {
    lerror("QueueTx failed (FIDL error %s)", result.status_string());
    return result.status();
  }
  if (result->is_error()) {
    lerror("QueueTx failed (status %s)", zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

zx_status_t Device::SetEthernetStatus(uint32_t status) {
  std::lock_guard<std::mutex> lock(ethernet_proxy_lock_);
  if (ethernet_proxy_.is_valid()) {
    ethernet_proxy_.Status(status);
  }
  return ZX_OK;
}

zx_status_t Device::JoinBss(join_bss_request_t* cfg) {
  auto arena = fdf::Arena::Create(0, 0);
  if (arena.is_error()) {
    lerror("Arena creation failed: %s", arena.status_string());
    return ZX_ERR_INTERNAL;
  }

  fuchsia_wlan_common::wire::JoinBssRequest fidl_bss_config;
  zx_status_t status = ConvertJoinBssRequest(*cfg, &fidl_bss_config, *arena);
  if (status != ZX_OK) {
    lerror("JoinBssRequest conversion failed: %s", zx_status_get_string(status));
    return status;
  }

  auto result = client_.sync().buffer(*std::move(arena))->JoinBss(fidl_bss_config);
  if (!result.ok()) {
    lerror("Config bss failed (FIDL error %s)", result.status_string());
    return result.status();
  }
  if (result->is_error()) {
    lerror("Config bss failed (status %s)", zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

// Max size of WlanSoftmacEnableBeaconingRequest.
static constexpr size_t kWlanSoftmacEnableBeaconingRequestBufferSize =
    fidl::MaxSizeInChannel<fuchsia_wlan_softmac::wire::WlanSoftmacEnableBeaconingRequest,
                           fidl::MessageDirection::kSending>();

zx_status_t Device::EnableBeaconing(wlan_softmac_enable_beaconing_request_t* request) {
  ZX_DEBUG_ASSERT(
      ValidateFrame("Malformed beacon template",
                    {reinterpret_cast<const uint8_t*>(request->packet_template.mac_frame_buffer),
                     request->packet_template.mac_frame_size}));

  auto arena = fdf::Arena::Create(0, 0);
  if (arena.is_error()) {
    lerror("Arena creation failed: %s", arena.status_string());
    return ZX_ERR_INTERNAL;
  }

  fidl::Arena<kWlanSoftmacEnableBeaconingRequestBufferSize> fidl_arena;
  fuchsia_wlan_softmac::wire::WlanSoftmacEnableBeaconingRequest fidl_request;
  ConvertEnableBeaconing(*request, &fidl_request, fidl_arena);

  auto result = client_.sync().buffer(*std::move(arena))->EnableBeaconing(fidl_request);
  if (!result.ok()) {
    lerror("EnableBeaconing failed (FIDL error %s)", result.status_string());
    return result.status();
  }
  if (result->is_error()) {
    lerror("EnableBeaconing failed (status %s)", zx_status_get_string(result->error_value()));
    return result->error_value();
  }
  return ZX_OK;
}

zx_status_t Device::DisableBeaconing() {
  auto arena = fdf::Arena::Create(0, 0);
  if (arena.is_error()) {
    lerror("Arena creation failed: %s", arena.status_string());
    return ZX_ERR_INTERNAL;
  }

  auto result = client_.sync().buffer(*std::move(arena))->DisableBeaconing();

  if (!result.ok()) {
    lerror("DisableBeaconing failed (FIDL error %s)", result.status_string());
    return result.status();
  }
  if (result->is_error()) {
    lerror("DisableBeaconing failed (status %s)", zx_status_get_string(result->error_value()));
    return result->error_value();
  }
  return ZX_OK;
}

zx_status_t Device::InstallKey(wlan_key_configuration_t* key_config) {
  auto arena = fdf::Arena::Create(0, 0);
  if (arena.is_error()) {
    lerror("Arena creation failed: %s", arena.status_string());
    return ZX_ERR_INTERNAL;
  }

  fidl::Arena fidl_arena;
  fuchsia_wlan_softmac::wire::WlanKeyConfiguration fidl_key_config;
  zx_status_t status = ConvertKeyConfig(*key_config, &fidl_key_config, fidl_arena);
  if (status != ZX_OK) {
    lerror("WlanKeyConfiguration conversion failed: %s", zx_status_get_string(status));
    return status;
  }

  auto result = client_.sync().buffer(*std::move(arena))->InstallKey(fidl_key_config);
  if (!result.ok()) {
    lerror("InstallKey failed (FIDL error %s)", result.status_string());
    return result.status();
  }
  if (result->is_error()) {
    lerror("InstallKey failed (status %s)", zx_status_get_string(result->error_value()));
    return result->error_value();
  }
  return ZX_OK;
}

void Device::Recv(RecvRequestView request, fdf::Arena& arena, RecvCompleter::Sync& completer) {
  wlan_rx_packet_t rx_packet;

  {
    // Lock the buffer operations to prevent data corruption when multiple thread are calling into
    // this function.
    std::lock_guard lock(rx_lock_);
    bool use_prealloc_recv_buffer =
        unlikely(request->packet.mac_frame.count() > PRE_ALLOC_RECV_BUFFER_SIZE);
    uint8_t* rx_packet_buffer;
    if (use_prealloc_recv_buffer) {
      rx_packet_buffer = static_cast<uint8_t*>(malloc(request->packet.mac_frame.count()));
    } else {
      rx_packet_buffer = pre_alloc_recv_buffer_;
    }

    zx_status_t status = ConvertRxPacket(request->packet, &rx_packet, rx_packet_buffer);
    if (status != ZX_OK) {
      lerror("RxPacket conversion failed: %s", zx_status_get_string(status));
    }

    wlan_softmac_ifc_protocol_->ops->recv(wlan_softmac_ifc_protocol_->ctx, &rx_packet);
    if (use_prealloc_recv_buffer) {
      // Freeing the frame buffer allocated in ConvertRxPacket() above.
      memset(const_cast<uint8_t*>(rx_packet.mac_frame_buffer), 0, rx_packet.mac_frame_size);
      free(const_cast<uint8_t*>(rx_packet.mac_frame_buffer));
    } else {
      memset(pre_alloc_recv_buffer_, 0, PRE_ALLOC_RECV_BUFFER_SIZE);
    }
  }

  completer.buffer(arena).Reply();
}

void Device::ReportTxResult(ReportTxResultRequestView request, fdf::Arena& arena,
                            ReportTxResultCompleter::Sync& completer) {
  wlan_tx_result_t tx_result;
  zx_status_t status = ConvertTxStatus(request->tx_result, &tx_result);
  if (status != ZX_OK) {
    lerror("TxStatus conversion failed: %s", zx_status_get_string(status));
  }

  wlan_softmac_ifc_protocol_->ops->report_tx_result(wlan_softmac_ifc_protocol_->ctx, &tx_result);

  completer.buffer(arena).Reply();
}
void Device::NotifyScanComplete(NotifyScanCompleteRequestView request, fdf::Arena& arena,
                                NotifyScanCompleteCompleter::Sync& completer) {
  wlan_softmac_ifc_protocol_->ops->notify_scan_complete(wlan_softmac_ifc_protocol_->ctx,
                                                        request->status(), request->scan_id());
  completer.buffer(arena).Reply();
}

fbl::RefPtr<DeviceState> Device::GetState() { return state_; }

const discovery_support_t& Device::GetDiscoverySupport() const { return discovery_support_; }

const mac_sublayer_support_t& Device::GetMacSublayerSupport() const {
  return mac_sublayer_support_;
}

const security_support_t& Device::GetSecuritySupport() const { return security_support_; }

const spectrum_management_support_t& Device::GetSpectrumManagementSupport() const {
  return spectrum_management_support_;
}
}  // namespace wlan
