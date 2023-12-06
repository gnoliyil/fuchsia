// Copyright (c) 2019 The Fuchsia Authors
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

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/device.h"

#include <lib/fidl/cpp/wire/arena.h>
#include <lib/sync/cpp/completion.h>
#include <zircon/status.h>

#include <ddktl/fidl.h>
#include <ddktl/init-txn.h>
#include <wlan/common/ieee80211.h>

#include "fidl/fuchsia.wlan.phyimpl/cpp/wire_types.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/cfg80211.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/common.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/debug.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/factory_device.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/feature.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/fwil.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/wlan_interface.h"

namespace wlan {
namespace brcmfmac {
namespace {

constexpr char kNetDevDriverName[] = "brcmfmac-netdev";
constexpr char kClientInterfaceName[] = "brcmfmac-wlan-fullmac-client";
constexpr uint8_t kClientInterfaceId = 0;
constexpr char kApInterfaceName[] = "brcmfmac-wlan-fullmac-ap";
constexpr uint8_t kApInterfaceId = 1;
constexpr uint8_t kMaxBufferParts = 1;
constexpr zx::duration kDeviceRemovalTimeout = zx::sec(5);
}  // namespace

namespace wlan_llcpp = fuchsia_factory_wlan;

Device::Device(zx_device_t* parent)
    : DeviceType(parent),
      brcmf_pub_(std::make_unique<brcmf_pub>()),
      client_interface_(nullptr),
      ap_interface_(nullptr),
      network_device_(parent, this),
      parent_(parent),
      outgoing_dir_(fdf::OutgoingDirectory::Create(fdf::Dispatcher::GetCurrent()->get())) {
  brcmf_pub_->device = this;
  for (auto& entry : brcmf_pub_->if2bss) {
    entry = BRCMF_BSSIDX_INVALID;
  }

  // Initialize the recovery trigger for driver, shared by all buses' devices.
  auto recovery_start_callback = std::make_shared<std::function<zx_status_t()>>();
  *recovery_start_callback = std::bind(&brcmf_schedule_recovery_worker, brcmf_pub_.get());
  brcmf_pub_->recovery_trigger =
      std::make_unique<wlan::brcmfmac::RecoveryTrigger>(recovery_start_callback);

  auto dispatcher = fdf::SynchronizedDispatcher::Create(
      {}, "brcmfmac-wlanphy", [&](fdf_dispatcher_t*) { completion_.Signal(); });
  ZX_ASSERT_MSG(!dispatcher.is_error(), "%s(): Dispatcher created failed: %s\n", __func__,
                zx_status_get_string(dispatcher.status_value()));
  dispatcher_ = std::move(*dispatcher);
  driver_async_dispatcher_ = fdf::Dispatcher::GetCurrent()->async_dispatcher();
}

Device::~Device() {
  zx::result res =
      outgoing_dir_.RemoveService<fuchsia_wlan_phyimpl::Service>(fdf::kDefaultInstance);
  if (res.is_error()) {
    BRCMF_ERR("Failed to remove WlanPhyImpl service from outgoing directory: %s\n",
              res.status_string());
  }
  ShutdownDispatcher();
}

zx_status_t Device::Init() {
  zx_status_t status = DeviceInit();
  if (status == ZX_OK) {
    status = network_device_.Init(kNetDevDriverName);
    if (status != ZX_OK) {
      BRCMF_ERR("Failed to initialize network device %s", zx_status_get_string(status));
      return status;
    }
  }

  // This does not have to be freed since its lifecycle is taken care by FDF.
  status = FactoryDevice::Create(parent_, brcmf_pub_.get(), &factory_device_);
  if (status != ZX_OK) {
    BRCMF_ERR("Failed to create factory device %s", zx_status_get_string(status));
    return status;
  }

  return status;
}

void Device::DdkInit(ddk::InitTxn txn) { txn.Reply(Init()); }

void Device::DdkRelease() {
  Shutdown();
  delete this;
}

void Device::DdkSuspend(ddk::SuspendTxn txn) {
  BRCMF_INFO("Shutdown requested");
  Shutdown();
  BRCMF_INFO("Shutdown completed");
  txn.Reply(ZX_OK, txn.requested_state());
}

brcmf_pub* Device::drvr() { return brcmf_pub_.get(); }

const brcmf_pub* Device::drvr() const { return brcmf_pub_.get(); }

void Device::WaitForProtocolConnection() { protocol_connected_.Wait(); }

zx_status_t Device::ServeWlanPhyImplProtocol(fidl::ServerEnd<fuchsia_io::Directory> server_end) {
  // This callback will be invoked when this service is being connected.
  auto protocol = [this](fdf::ServerEnd<fuchsia_wlan_phyimpl::WlanPhyImpl> server_end) mutable {
    // Return immediately if the dispatcher has already been shutdown.
    if (completion_.Wait(zx::sec(0)) == ZX_OK) {
      BRCMF_WARN("Dispatcher has been shutdown, cannot bind fidl server.");
      return;
    }
    fdf::BindServer(dispatcher_.get(), std::move(server_end), this);
    // Notify that the server end is ready to accept request. Waiting for this signal by other
    // entity is not necessary, the current use case of it is in SIM tests.
    protocol_connected_.Signal();
  };

  // Register the callback to handler.
  fuchsia_wlan_phyimpl::Service::InstanceHandler handler({.wlan_phy_impl = std::move(protocol)});

  // Add this service to the outgoing directory so that the child driver can connect to by calling
  // DdkConnectRuntimeProtocol().
  auto status = outgoing_dir_.AddService<fuchsia_wlan_phyimpl::Service>(std::move(handler));
  if (status.is_error()) {
    BRCMF_ERR("%s(): Failed to add service to outgoing directory: %s\n", status.status_string());
    return status.error_value();
  }

  // Serve the outgoing directory to the entity that intends to open it, which is DFv1 in this case.
  auto result = outgoing_dir_.Serve(std::move(server_end));
  if (result.is_error()) {
    BRCMF_ERR("%s(): Failed to serve outgoing directory: %s\n", result.status_string());
    return result.error_value();
  }

  return ZX_OK;
}

void Device::GetSupportedMacRoles(fdf::Arena& arena,
                                  GetSupportedMacRolesCompleter::Sync& completer) {
  BRCMF_DBG(WLANPHY, "Received request for supported MAC roles from SME dfv2");
  fuchsia_wlan_common::wire::WlanMacRole
      supported_mac_roles_list[fuchsia_wlan_common::wire::kMaxSupportedMacRoles] = {};
  uint8_t supported_mac_roles_count = 0;
  zx_status_t status = WlanInterface::GetSupportedMacRoles(
      brcmf_pub_.get(), supported_mac_roles_list, &supported_mac_roles_count);
  if (status != ZX_OK) {
    BRCMF_ERR("Device::GetSupportedMacRoles() failed to get supported mac roles: %s\n",
              zx_status_get_string(status));
    completer.buffer(arena).ReplyError(status);
    return;
  }

  if (supported_mac_roles_count > fuchsia_wlan_common::wire::kMaxSupportedMacRoles) {
    BRCMF_ERR(
        "Device::GetSupportedMacRoles() Too many mac roles returned from brcmfmac driver. Number "
        "of supported max roles got "
        "from driver is %u, but the limitation is: %u\n",
        supported_mac_roles_count, fuchsia_wlan_common::wire::kMaxSupportedMacRoles);
    completer.buffer(arena).ReplyError(ZX_ERR_OUT_OF_RANGE);
    return;
  }

  auto reply_vector = fidl::VectorView<fuchsia_wlan_common::wire::WlanMacRole>::FromExternal(
      supported_mac_roles_list, supported_mac_roles_count);
  fidl::Arena fidl_arena;
  auto builder =
      fuchsia_wlan_phyimpl::wire::WlanPhyImplGetSupportedMacRolesResponse::Builder(fidl_arena);
  builder.supported_mac_roles(reply_vector);
  completer.buffer(arena).ReplySuccess(builder.Build());
}

void Device::CreateIface(CreateIfaceRequestView request, fdf::Arena& arena,
                         CreateIfaceCompleter::Sync& completer) {
  std::lock_guard<std::mutex> lock(lock_);
  BRCMF_INFO("Device::CreateIface() creating interface started dfv2");

  if (!request->has_role() || !request->has_mlme_channel()) {
    BRCMF_ERR("Device::CreateIface() missing information in role(%u), channel(%u)",
              request->has_role(), request->has_mlme_channel());
    completer.buffer(arena).ReplyError(ZX_ERR_INVALID_ARGS);
    return;
  }

  zx_status_t status = ZX_OK;
  wireless_dev* wdev = nullptr;
  uint16_t iface_id = 0;

  switch (request->role()) {
    case fuchsia_wlan_common::wire::WlanMacRole::kClient: {
      if (client_interface_ != nullptr) {
        BRCMF_ERR("Device::CreateIface() client interface already exists");
        completer.buffer(arena).ReplyError(ZX_ERR_NO_RESOURCES);
        return;
      }

      // If we are operating with manufacturing firmware ensure SoftAP IF is also not present
      if (brcmf_feat_is_enabled(brcmf_pub_.get(), BRCMF_FEAT_MFG)) {
        if (ap_interface_ != nullptr) {
          BRCMF_ERR("Simultaneous mode not supported in mfg FW - Ap IF already exists");
          completer.buffer(arena).ReplyError(ZX_ERR_NO_RESOURCES);
          return;
        }
      }

      if ((status = brcmf_cfg80211_add_iface(brcmf_pub_.get(), kClientInterfaceName, nullptr,
                                             request, &wdev)) != ZX_OK) {
        BRCMF_ERR("Device::CreateIface() failed to create Client interface, %s",
                  zx_status_get_string(status));
        completer.buffer(arena).ReplyError(status);
        return;
      }

      WlanInterface* interface = nullptr;

      libsync::Completion created;
      // Create WlanInterface on the default driver dispatcher to ensure its OutgoingDirectory is
      // only accessed on a single thread.
      async::PostTask(driver_async_dispatcher_, [&]() {
        status =
            WlanInterface::Create(this, kClientInterfaceName, wdev, request->role(), &interface);
        created.Signal();
      });
      created.Wait();
      if (status != ZX_OK) {
        BRCMF_ERR("Failed to create WlanInterface: %s", zx_status_get_string(status));
        completer.buffer(arena).ReplyError(status);
        return;
      }

      client_interface_ = interface;  // The lifecycle of `interface` is owned by the devhost.
      iface_id = kClientInterfaceId;

      break;
    }

    case fuchsia_wlan_common::wire::WlanMacRole::kAp: {
      if (ap_interface_ != nullptr) {
        BRCMF_ERR("Device::CreateIface() AP interface already exists");
        completer.buffer(arena).ReplyError(ZX_ERR_NO_RESOURCES);
        return;
      }

      // If we are operating with manufacturing firmware ensure client IF is also not present
      if (brcmf_feat_is_enabled(brcmf_pub_.get(), BRCMF_FEAT_MFG)) {
        if (client_interface_ != nullptr) {
          BRCMF_ERR("Simultaneous mode not supported in mfg FW - Client IF already exists");
          completer.buffer(arena).ReplyError(ZX_ERR_NO_RESOURCES);
          return;
        }
      }

      if ((status = brcmf_cfg80211_add_iface(brcmf_pub_.get(), kApInterfaceName, nullptr, request,
                                             &wdev)) != ZX_OK) {
        BRCMF_ERR("Device::CreateIface() failed to create AP interface, %s",
                  zx_status_get_string(status));
        completer.buffer(arena).ReplyError(status);
        return;
      }

      WlanInterface* interface = nullptr;

      libsync::Completion created;
      // Create WlanInterface on the default driver dispatcher to ensure its OutgoingDirectory is
      // only accessed on a single thread.
      async::PostTask(driver_async_dispatcher_, [&]() {
        status = WlanInterface::Create(this, kApInterfaceName, wdev, request->role(), &interface);
        created.Signal();
      });
      created.Wait();
      if (status != ZX_OK) {
        BRCMF_ERR("Failed to create WlanInterface: %s", zx_status_get_string(status));
        completer.buffer(arena).ReplyError(status);
        return;
      }

      ap_interface_ = interface;  // The lifecycle of `interface` is owned by the devhost.
      iface_id = kApInterfaceId;

      break;
    }

    default: {
      BRCMF_ERR("Device::CreateIface() MAC role %d not supported", request->role());
      completer.buffer(arena).ReplyError(ZX_ERR_NOT_SUPPORTED);
      return;
    }
  }

  // Log the new iface's role, name, and MAC address
  net_device* ndev = wdev->netdev;

  const char* role = request->role() == fuchsia_wlan_common::wire::WlanMacRole::kClient ? "client"
                     : request->role() == fuchsia_wlan_common::wire::WlanMacRole::kAp   ? "ap"
                     : request->role() == fuchsia_wlan_common::wire::WlanMacRole::kMesh
                         ? "mesh"
                         : "unknown type";
  BRCMF_DBG(WLANPHY, "Created %s iface with netdev:%s id:%d", role, ndev->name, iface_id);
#if !defined(NDEBUG)
  const uint8_t* mac_addr = ndev_to_if(ndev)->mac_addr;
  BRCMF_DBG(WLANPHY, "  address: " FMT_MAC, FMT_MAC_ARGS(mac_addr));
#endif /* !defined(NDEBUG) */

  fidl::Arena fidl_arena;
  auto builder = fuchsia_wlan_phyimpl::wire::WlanPhyImplCreateIfaceResponse::Builder(fidl_arena);
  builder.iface_id(iface_id);
  completer.buffer(arena).ReplySuccess(builder.Build());
}

void Device::DestroyIface(DestroyIfaceRequestView request, fdf::Arena& arena,
                          DestroyIfaceCompleter::Sync& completer) {
  std::lock_guard<std::mutex> lock(lock_);

  if (!request->has_iface_id()) {
    BRCMF_ERR("Device::DestroyIface() invoked without valid iface_id");
    completer.buffer(arena).ReplyError(ZX_ERR_INVALID_ARGS);
    return;
  }

  uint16_t iface_id = request->iface_id();
  BRCMF_DBG(WLANPHY, "Destroying interface %d", iface_id);
  switch (iface_id) {
    case kClientInterfaceId: {
      libsync::Completion device_removal;
      DestroyIface(&client_interface_, [&device_removal, completer = completer.ToAsync(),
                                        arena = std::move(arena), iface_id](auto status) mutable {
        if (status != ZX_OK) {
          if (status != ZX_ERR_NOT_FOUND) {
            BRCMF_ERR("Device::DestroyIface() Error destroying Client interface : %s",
                      zx_status_get_string(status));
          }
          completer.buffer(arena).ReplyError(status);
          device_removal.Signal();
          return;
        }
        BRCMF_DBG(WLANPHY, "Interface %d destroyed successfully", iface_id);
        completer.buffer(arena).ReplySuccess();
        device_removal.Signal();
      });
      zx_status_t status = device_removal.Wait(kDeviceRemovalTimeout);
      if (status != ZX_OK) {
        BRCMF_ERR(
            "Timeout: Waiting for brcmfmac-wlan-fullmac-client to be released in driver framework.");
      }
      return;
    }
    case kApInterfaceId: {
      libsync::Completion device_removal;
      DestroyIface(&ap_interface_, [&device_removal, completer = completer.ToAsync(),
                                    arena = std::move(arena), iface_id](auto status) mutable {
        if (status != ZX_OK) {
          BRCMF_ERR("Device::DestroyIface() Error destroying AP interface : %s",
                    zx_status_get_string(status));
          completer.buffer(arena).ReplyError(status);
          device_removal.Signal();
          return;
        }
        BRCMF_DBG(WLANPHY, "Interface %d destroyed successfully", iface_id);
        completer.buffer(arena).ReplySuccess();
        device_removal.Signal();
      });
      zx_status_t status = device_removal.Wait(kDeviceRemovalTimeout);
      if (status != ZX_OK) {
        BRCMF_ERR(
            "Timeout: Waiting for brcmfmac-wlan-fullmac-ap to be released in driver framework.");
      }
      return;
    }
    default: {
      BRCMF_ERR("Device::DestroyIface() Unknown interface id: %d", iface_id);
      completer.buffer(arena).ReplyError(ZX_ERR_NOT_FOUND);
      return;
    }
  }
}

void Device::SetCountry(SetCountryRequestView request, fdf::Arena& arena,
                        SetCountryCompleter::Sync& completer) {
  BRCMF_DBG(WLANPHY, "Setting country code dfv2");
  if (!request->is_alpha2()) {
    completer.buffer(arena).ReplyError(ZX_ERR_INVALID_ARGS);
    BRCMF_ERR("Device::SetCountry() Invalid input format of country code.");
    return;
  }
  const auto country = fuchsia_wlan_phyimpl_wire::WlanPhyCountry::WithAlpha2(request->alpha2());
  zx_status_t status = WlanInterface::SetCountry(brcmf_pub_.get(), &country);
  if (status != ZX_OK) {
    BRCMF_ERR("Device::SetCountry() Failed Set country : %s", zx_status_get_string(status));
    completer.buffer(arena).ReplyError(status);
    return;
  }

  completer.buffer(arena).ReplySuccess();
}

void Device::ClearCountry(fdf::Arena& arena, ClearCountryCompleter::Sync& completer) {
  BRCMF_DBG(WLANPHY, "Clearing country dfv2");
  zx_status_t status = WlanInterface::ClearCountry(brcmf_pub_.get());
  if (status != ZX_OK) {
    BRCMF_ERR("Device::ClearCountry() Failed Clear country : %s", zx_status_get_string(status));
    completer.buffer(arena).ReplyError(status);
    return;
  }

  completer.buffer(arena).ReplySuccess();
}

void Device::GetCountry(fdf::Arena& arena, GetCountryCompleter::Sync& completer) {
  BRCMF_DBG(WLANPHY, "Received request for country from SME dfv2");
  uint8_t cc_code[fuchsia_wlan_phyimpl_wire::kWlanphyAlpha2Len];

  zx_status_t status = WlanInterface::GetCountry(brcmf_pub_.get(), cc_code);
  if (status != ZX_OK) {
    BRCMF_ERR("Device::GetCountry() Failed Get country : %s", zx_status_get_string(status));
    completer.buffer(arena).ReplyError(status);
    return;
  }
  auto country = fuchsia_wlan_phyimpl::wire::WlanPhyCountry::WithAlpha2({cc_code[0], cc_code[1]});
  BRCMF_INFO("Get country code: %c%c", country.alpha2()[0], country.alpha2()[1]);

  completer.buffer(arena).ReplySuccess(country);
}

void Device::SetPowerSaveMode(SetPowerSaveModeRequestView request, fdf::Arena& arena,
                              SetPowerSaveModeCompleter::Sync& completer) {
  BRCMF_DBG(WLANPHY, "Setting power save mode dfv2");
  if (!request->has_ps_mode()) {
    BRCMF_ERR("Device::SetPowerSaveMode() invoked without ps_mode");
    completer.buffer(arena).ReplyError(ZX_ERR_INVALID_ARGS);
    return;
  }

  zx_status_t status = brcmf_set_power_save_mode(brcmf_pub_.get(), request->ps_mode());
  if (status != ZX_OK) {
    BRCMF_ERR("Device::SetPowerSaveMode() failed setting ps mode : %s",
              zx_status_get_string(status));
    completer.buffer(arena).ReplyError(status);
    return;
  }

  completer.buffer(arena).ReplySuccess();
}

void Device::GetPowerSaveMode(fdf::Arena& arena, GetPowerSaveModeCompleter::Sync& completer) {
  BRCMF_DBG(WLANPHY, "Received request for PS mode from SME dfv2");
  fuchsia_wlan_common_wire::PowerSaveType ps_mode;
  zx_status_t status = brcmf_get_power_save_mode(brcmf_pub_.get(), &ps_mode);
  if (status != ZX_OK) {
    BRCMF_ERR("Device::GetPowerSaveMode() Get Power Save Mode failed");
    completer.buffer(arena).ReplyError(ZX_ERR_NOT_FOUND);
    return;
  }
  fidl::Arena fidl_arena;
  auto builder =
      fuchsia_wlan_phyimpl::wire::WlanPhyImplGetPowerSaveModeResponse::Builder(fidl_arena);
  builder.ps_mode(ps_mode);
  completer.buffer(arena).ReplySuccess(builder.Build());
}

void Device::NetDevInit(wlan::drivers::components::NetworkDevice::Callbacks::InitTxn txn) {
  txn.Reply(ZX_OK);
}

void Device::NetDevRelease() {
  // Don't need to do anything here, the release of wlanif should take care of all releasing
}

void Device::NetDevStart(wlan::drivers::components::NetworkDevice::Callbacks::StartTxn txn) {
  txn.Reply(ZX_OK);
}

void Device::NetDevStop(wlan::drivers::components::NetworkDevice::Callbacks::StopTxn txn) {
  // Flush all buffers in response to this call. They are no longer valid for use.
  brcmf_flush_buffers(drvr());
  txn.Reply();
}

void Device::NetDevGetInfo(device_impl_info_t* out_info) {
  std::lock_guard<std::mutex> lock(lock_);

  memset(out_info, 0, sizeof(*out_info));
  zx_status_t err = brcmf_get_tx_depth(drvr(), &out_info->tx_depth);
  ZX_ASSERT(err == ZX_OK);
  err = brcmf_get_rx_depth(drvr(), &out_info->rx_depth);
  ZX_ASSERT(err == ZX_OK);
  out_info->rx_threshold = out_info->rx_depth / 3;
  out_info->max_buffer_parts = kMaxBufferParts;
  out_info->max_buffer_length = ZX_PAGE_SIZE;
  out_info->buffer_alignment = ZX_PAGE_SIZE;
  out_info->min_rx_buffer_length = IEEE80211_MSDU_SIZE_MAX;

  out_info->tx_head_length = drvr()->hdrlen;
  brcmf_get_tail_length(drvr(), &out_info->tx_tail_length);
  // No hardware acceleration supported yet.
  out_info->rx_accel_count = 0;
  out_info->tx_accel_count = 0;
}

void Device::NetDevQueueTx(cpp20::span<wlan::drivers::components::Frame> frames) {
  brcmf_start_xmit(drvr(), frames);
}

void Device::NetDevQueueRxSpace(const rx_space_buffer_t* buffers_list, size_t buffers_count,
                                uint8_t* vmo_addrs[]) {
  brcmf_queue_rx_space(drvr(), buffers_list, buffers_count, vmo_addrs);
}

zx_status_t Device::NetDevPrepareVmo(uint8_t vmo_id, zx::vmo vmo, uint8_t* mapped_address,
                                     size_t mapped_size) {
  return brcmf_prepare_vmo(drvr(), vmo_id, vmo.get(), mapped_address, mapped_size);
}

void Device::NetDevReleaseVmo(uint8_t vmo_id) { brcmf_release_vmo(drvr(), vmo_id); }

void Device::NetDevSetSnoopEnabled(bool snoop) {}

void Device::ShutdownDispatcher() {
  dispatcher_.ShutdownAsync();
  completion_.Wait();
}

void Device::DestroyAllIfaces(void) {
  std::lock_guard<std::mutex> lock(lock_);
  libsync::Completion client_device_removal;
  libsync::Completion ap_device_removal;
  DestroyIface(&client_interface_, [&client_device_removal](auto status) {
    if (status != ZX_OK) {
      BRCMF_ERR("Device::DestroyAllIfaces() : Failed destroying client interface : %s",
                zx_status_get_string(status));
    }
    client_device_removal.Signal();
  });
  DestroyIface(&ap_interface_, [&ap_device_removal](auto status) {
    if (status != ZX_OK) {
      BRCMF_ERR("Device::DestroyAllIfaces() : Failed destroying AP interface : %s",
                zx_status_get_string(status));
    }
    ap_device_removal.Signal();
  });
  client_device_removal.Wait();
  ap_device_removal.Wait();
}

void Device::DestroyIface(WlanInterface** iface_ptr, fit::callback<void(zx_status_t)> respond) {
  WlanInterface* iface = *iface_ptr;
  zx_status_t status = ZX_OK;
  if (iface == nullptr) {
    respond(ZX_ERR_NOT_FOUND);
    return;
  }

  iface->RemovePort();
  wireless_dev* wdev = iface->take_wdev();

  libsync::Completion destroyed;
  // In SIM tests, the Reomve() function below deletes WlanInterface, make sure this operation
  // happens on the default driver dispatcher to ensure the OutgoingDirectory of WlanInterface is
  // only accessed on a single thread.
  async::PostTask(driver_async_dispatcher_, [&]() {
    if ((status = brcmf_cfg80211_del_iface(brcmf_pub_->config, wdev)) != ZX_OK) {
      BRCMF_ERR("Failed to del iface, status: %s", zx_status_get_string(status));
      iface->set_wdev(wdev);
      respond(status);
      destroyed.Signal();
      return;
    }
    iface->Remove([status, respond = std::move(respond)]() mutable { respond(status); });
    *iface_ptr = nullptr;

    destroyed.Signal();
  });
  destroyed.Wait();
}

}  // namespace brcmfmac
}  // namespace wlan
