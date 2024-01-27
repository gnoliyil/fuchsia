// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/board/lib/acpi/device.h"

#include <fuchsia/hardware/sysmem/c/banjo.h>
#include <lib/async/cpp/executor.h>
#include <lib/component/outgoing/cpp/handlers.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/metadata.h>
#include <lib/fit/defer.h>
#include <lib/fpromise/promise.h>
#include <zircon/errors.h>
#include <zircon/syscalls/resource.h>
#include <zircon/types.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#include <fbl/auto_lock.h>
#include <fbl/string_printf.h>

#include "lib/ddk/device.h"
#include "lib/zx/result.h"
#include "src/devices/board/lib/acpi/event.h"
#include "src/devices/board/lib/acpi/fidl.h"
#include "src/devices/board/lib/acpi/global-lock.h"
#include "src/devices/board/lib/acpi/manager.h"
#include "src/devices/board/lib/acpi/power-resource.h"
#include "src/devices/lib/iommu/iommu.h"
#include "third_party/acpica/source/include/actypes.h"

namespace acpi {
namespace {
// Maximum number of pending Device Object Notifications before we stop sending them to a device.
constexpr size_t kMaxPendingNotifications = 1000;
}  // namespace

ACPI_STATUS Device::AddResource(ACPI_RESOURCE* res) {
  if (resource_is_memory(res)) {
    resource_memory_t mem;
    zx_status_t st = resource_parse_memory(res, &mem);
    // only expect fixed memory resource. resource_parse_memory sets minimum == maximum
    // for this memory resource type.
    if ((st != ZX_OK) || (mem.minimum != mem.maximum)) {
      return AE_ERROR;
    }
    mmio_resources_.emplace_back(mem);

  } else if (resource_is_address(res)) {
    resource_address_t addr;
    zx_status_t st = resource_parse_address(res, &addr);
    if (st != ZX_OK) {
      return AE_ERROR;
    }
    if ((addr.resource_type == RESOURCE_ADDRESS_MEMORY) && addr.min_address_fixed &&
        addr.max_address_fixed && (addr.maximum < addr.minimum)) {
      mmio_resources_.emplace_back(/* writeable= */ true, addr.min_address_fixed,
                                   /* alignment= */ 0, static_cast<uint32_t>(addr.address_length));
    }

  } else if (resource_is_io(res)) {
    resource_io_t io;
    zx_status_t st = resource_parse_io(res, &io);
    if (st != ZX_OK) {
      return AE_ERROR;
    }

    pio_resources_.emplace_back(io);

  } else if (resource_is_irq(res)) {
    resource_irq_t irq;
    zx_status_t st = resource_parse_irq(res, &irq);
    if (st != ZX_OK) {
      return AE_ERROR;
    }
    for (auto i = 0; i < irq.pin_count; i++) {
      irqs_.emplace_back(irq, i);
    }
  }

  return AE_OK;
}

zx_status_t Device::ReportCurrentResources() {
  if (got_resources_) {
    return ZX_OK;
  }

  // Check device state.
  auto state = acpi_->EvaluateObject(acpi_handle_, "_STA", std::nullopt);
  uint64_t sta;
  if (state.is_error() || state->Type != ACPI_TYPE_INTEGER) {
    sta = 0xf;
  } else {
    sta = state->Integer.Value;
  }

  if ((sta & ACPI_STA_DEVICE_ENABLED) == 0) {
    // We're not allowed to enumerate resources if the device is not enabled.
    // see ACPI 6.4 section 6.3.7.
    return ZX_OK;
  }

  // call _CRS to fill in resources
  ACPI_STATUS acpi_status = AcpiWalkResources(
      acpi_handle_, const_cast<char*>("_CRS"),
      [](ACPI_RESOURCE* res, void* ctx) __TA_REQUIRES(reinterpret_cast<Device*>(ctx)->lock_) {
        return reinterpret_cast<Device*>(ctx)->AddResource(res);
      },
      this);
  if ((acpi_status != AE_NOT_FOUND) && (acpi_status != AE_OK)) {
    return acpi_to_zx_status(acpi_status);
  }

  zxlogf(DEBUG, "acpi-bus: found %zd port resources %zd memory resources %zx irqs",
         pio_resources_.size(), mmio_resources_.size(), irqs_.size());
  if (zxlog_level_enabled(TRACE)) {
    zxlogf(TRACE, "port resources:");
    for (size_t i = 0; i < pio_resources_.size(); i++) {
      zxlogf(TRACE, "  %02zd: addr=0x%x length=0x%x align=0x%x", i, pio_resources_[i].base_address,
             pio_resources_[i].address_length, pio_resources_[i].alignment);
    }
    zxlogf(TRACE, "memory resources:");
    for (size_t i = 0; i < mmio_resources_.size(); i++) {
      zxlogf(TRACE, "  %02zd: addr=0x%x length=0x%x align=0x%x writeable=%d", i,
             mmio_resources_[i].base_address, mmio_resources_[i].address_length,
             mmio_resources_[i].alignment, mmio_resources_[i].writeable);
    }
    zxlogf(TRACE, "irqs:");
    for (size_t i = 0; i < irqs_.size(); i++) {
      const char* trigger;
      switch (irqs_[i].trigger) {
        case ACPI_IRQ_TRIGGER_EDGE:
          trigger = "edge";
          break;
        case ACPI_IRQ_TRIGGER_LEVEL:
          trigger = "level";
          break;
        default:
          trigger = "bad_trigger";
          break;
      }
      const char* polarity;
      switch (irqs_[i].polarity) {
        case ACPI_IRQ_ACTIVE_BOTH:
          polarity = "both";
          break;
        case ACPI_IRQ_ACTIVE_LOW:
          polarity = "low";
          break;
        case ACPI_IRQ_ACTIVE_HIGH:
          polarity = "high";
          break;
        default:
          polarity = "bad_polarity";
          break;
      }
      zxlogf(TRACE, "  %02zd: pin=%u %s %s %s %s", i, irqs_[i].pin, trigger, polarity,
             (irqs_[i].sharable == ACPI_IRQ_SHARED) ? "shared" : "exclusive",
             irqs_[i].wake_capable ? "wake" : "nowake");
    }
  }

  got_resources_ = true;

  return ZX_OK;
}

void Device::DdkInit(ddk::InitTxn txn) {
  auto use_global_lock = acpi_->EvaluateObject(acpi_handle_, "_GLK", std::nullopt);
  if (use_global_lock.is_ok()) {
    if (use_global_lock->Type == ACPI_TYPE_INTEGER && use_global_lock->Integer.Value == 1) {
      can_use_global_lock_ = true;
    }
  }

  zx_status_t result = InitializePowerManagement();
  if (result != ZX_OK) {
    zxlogf(ERROR, "Error initializing power management for ACPI device: %s",
           zx_status_get_string(result));
    txn.Reply(result);
    return;
  }

#ifdef ENABLE_ATLAS_CAMERA
  bool atlas_camera_enabled = true;
#else
  bool atlas_camera_enabled = false;
#endif

  // Initial transition to D state 0.
  // Skip turning on Atlas camera unless enabled.
  if ((name_ != "acpi-CAM0" && name_ != "acpi-NVM0") || atlas_camera_enabled) {
    if (GetPowerStateInfo(DEV_POWER_STATE_D0)) {
      PowerStateTransitionResponse result = TransitionToPowerState(DEV_POWER_STATE_D0);
      if (result.status != ZX_OK) {
        zxlogf(ERROR, "Error transitioning ACPI device to D0 in Init: %s",
               zx_status_get_string(result.status));
        txn.Reply(result.status);
        return;
      }
    }
  }

  txn.Reply(ZX_OK);
}

void Device::DdkUnbind(ddk::UnbindTxn txn) {
  if (notify_handler_.has_value()) {
    RemoveNotifyHandler();
  }

  std::optional<fpromise::promise<void>> address_handler_finished;
  {
    std::scoped_lock lock(address_handler_lock_);
    for (auto& entry : address_handlers_) {
      entry.second.AsyncTeardown();
    }

    address_handler_finished.emplace(
        fpromise::join_promise_vector(std::move(address_handler_teardown_finished_))
            .discard_result());
  }

  std::optional<fpromise::promise<void>> teardown_finished;
  notify_teardown_finished_.swap(teardown_finished);
  auto promise = fpromise::join_promises(
                     std::move(teardown_finished).value_or(fpromise::make_ok_promise()),
                     std::move(address_handler_finished).value_or(fpromise::make_ok_promise()))
                     .discard_result()
                     .and_then([txn = std::move(txn)]() mutable { txn.Reply(); });
  executor_.schedule_task(std::move(promise));
}

void Device::GetMmio(GetMmioRequestView request, GetMmioCompleter::Sync& completer) {
  std::scoped_lock guard{lock_};
  zx_status_t st = ReportCurrentResources();
  if (st != ZX_OK) {
    zxlogf(ERROR, "Internal error evaluating resources: %s", zx_status_get_string(st));
    completer.ReplyError(ZX_ERR_INTERNAL);
    return;
  }

  if (request->index >= mmio_resources_.size()) {
    completer.ReplyError(ZX_ERR_OUT_OF_RANGE);
    return;
  }

  const DeviceMmioResource& res = mmio_resources_[request->index];
  // TODO(fxbug.dev/67899): This check becomes overly pessimistic at larger page sizes.
  if (((res.base_address & (zx_system_get_page_size() - 1)) != 0) ||
      ((res.address_length & (zx_system_get_page_size() - 1)) != 0)) {
    zxlogf(ERROR, "acpi-bus: memory id=%d addr=0x%08x len=0x%x is not page aligned", request->index,
           res.base_address, res.address_length);
    completer.ReplyError(ZX_ERR_INVALID_ARGS);
    return;
  }

  zx_handle_t vmo;
  size_t size{res.address_length};
  // Please do not use get_root_resource() in new code. See fxbug.dev/31358.
  st = zx_vmo_create_physical(get_root_resource(), res.base_address, size, &vmo);
  if (st != ZX_OK) {
    zxlogf(ERROR, "Internal error creating VMO: %s", zx_status_get_string(st));
    completer.ReplyError(ZX_ERR_INTERNAL);
    return;
  }

  completer.ReplySuccess(fuchsia_mem::wire::Range{
      .vmo = zx::vmo(vmo),
      .offset = 0,
      .size = size,
  });
}

void Device::GetBti(GetBtiRequestView request, GetBtiCompleter::Sync& completer) {
  // We only support getting BTIs for devices with no bus.
  if (bus_type_ != BusType::kUnknown) {
    completer.ReplyError(ZX_ERR_NOT_SUPPORTED);
    return;
  }
  if (request->index != 0) {
    completer.ReplyError(ZX_ERR_OUT_OF_RANGE);
    return;
  }

  // For dummy IOMMUs, the bti_id just needs to be unique.
  // We assume that the device will never get an actual BTI
  // because it is a pure ACPI device.
  //
  // TODO(fxbug.dev/92140): check the DMAR for ACPI entries.
  auto path = acpi_->GetPath(acpi_handle_);
  if (path.is_error()) {
    completer.ReplyError(path.zx_status_value());
    return;
  }
  auto iommu_handle = manager_->iommu_manager()->IommuForAcpiDevice(path.value());
  zx::bti bti;
  zx::bti::create(*iommu_handle, 0, bti_id_, &bti);

  completer.ReplySuccess(std::move(bti));
}

zx::result<zx::channel> Device::PrepareOutgoing() {
  auto result = outgoing_.AddService<fuchsia_hardware_acpi::Service>(
      fuchsia_hardware_acpi::Service::InstanceHandler({.device = bind_handler(dispatcher_)}));
  if (result.is_error()) {
    return result.take_error();
  }

  auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (endpoints.is_error()) {
    return endpoints.take_error();
  }

  result = outgoing_.Serve(std::move(endpoints->server));
  if (result.is_error()) {
    zxlogf(ERROR, "Failed to serve the outgoing directory: %s", result.status_string());
    return result.take_error();
  }

  return zx::ok(endpoints->client.TakeChannel());
}

zx_status_t Device::CallPsxMethod(const PowerStateInfo& state) {
  if (!state.defines_psx_method) {
    return ZX_OK;
  }

  std::string method_name = "_PS" + std::to_string(state.d_state);
  auto psx = acpi_->EvaluateObject(acpi_handle_, method_name.c_str(), std::nullopt);
  return psx.zx_status_value();
}

zx::result<Device::PowerStateInfo> Device::GetInfoForState(uint8_t d_state) {
  PowerStateInfo power_state_info{.d_state = d_state};
  std::vector<const PowerResource*> power_resources;

  // Gather information about what power resources are needed in this D state.
  std::string method_name = "_PR" + std::to_string(d_state);
  auto prx = acpi_->EvaluateObject(acpi_handle_, method_name.c_str(), std::nullopt);
  if (prx.is_ok()) {
    // Whether the status of power resources implies that the device is in this state.
    bool all_resources_on = true;

    for (size_t i = 0; i < prx->Package.Count; i++) {
      ACPI_OBJECT power_resource_reference = prx->Package.Elements[i];
      const PowerResource* power_resource =
          manager_->AddPowerResource(power_resource_reference.Reference.Handle);

      if (power_resource == nullptr) {
        zxlogf(ERROR, "Failed to add power resource");
        return zx::error(ZX_ERR_INTERNAL);
      }

      if (power_resource) {
        power_resources.push_back(power_resource);
        if (!power_resource->is_on()) {
          all_resources_on = false;
        }
      }
    }

    // Save the shallowest power state that power resources imply to be on.
    if (all_resources_on && current_power_state_ > d_state) {
      current_power_state_ = d_state;
    }
  }

  // Map from D states to supported S states based on power resource system_levels.
  uint8_t shallowest_system_level = 4;
  for (const PowerResource* power_resource : power_resources) {
    shallowest_system_level = std::min(shallowest_system_level, power_resource->system_level());
  }

  for (uint8_t s_state = 0; s_state <= shallowest_system_level; ++s_state) {
    power_state_info.supported_s_states.insert(s_state);
  }

  // Sort power resources by ascending resource_order.
  std::sort(power_resources.begin(), power_resources.end(),
            [](const PowerResource* lhs, const PowerResource* rhs) {
              return lhs->resource_order() < rhs->resource_order();
            });

  for (auto power_resource : power_resources) {
    power_state_info.power_resources.push_back(power_resource->handle());
  }

  // Check whether this D state has a _PSx method defined.
  method_name = "_PS" + std::to_string(d_state);
  auto psx = acpi_->GetHandle(acpi_handle_, method_name.c_str());
  if (psx.is_ok()) {
    power_state_info.defines_psx_method = true;
  }

  return zx::ok(power_state_info);
}

zx_status_t Device::ConfigureInitialPowerState() {
  if (supported_power_states_.empty()) {
    return ZX_OK;
  }

  auto psc = acpi_->EvaluateObject(acpi_handle_, "_PSC", std::nullopt);
  if (psc.is_ok()) {
    // This overrides any power state earlier implied by power resource status.
    current_power_state_ = static_cast<uint8_t>(psc->Integer.Value);
  }

  if (current_power_state_ == DEV_POWER_STATE_D3COLD &&
      !GetPowerStateInfo(DEV_POWER_STATE_D3COLD)) {
    current_power_state_ = DEV_POWER_STATE_D3HOT;
  }

  PowerStateInfo* current_power_state_info = GetPowerStateInfo(current_power_state_);
  ZX_ASSERT_MSG(current_power_state_info, "ACPI device initial state is not a supported state");

  zx_status_t result = manager_->ReferencePowerResources(current_power_state_info->power_resources);
  if (result != ZX_OK) {
    zxlogf(ERROR, "Failed to reference initial power resources for ACPI device: %s",
           zx_status_get_string(result));
    return result;
  }

  if (psc.is_error() && current_power_state_ == DEV_POWER_STATE_D0) {
    // We inferred the power state to be D0 from power resources so we may still need to call _PS0.
    result = CallPsxMethod(*current_power_state_info);
    if (result != ZX_OK) {
      zxlogf(ERROR, "Failed initial call to _PS0 for ACPI device: %s",
             zx_status_get_string(result));
      return result;
    }
  }

  return ZX_OK;
}

zx_status_t Device::InitializePowerManagement() {
  for (uint8_t d_state = DEV_POWER_STATE_D0; d_state <= DEV_POWER_STATE_D3HOT; ++d_state) {
    zx::result<PowerStateInfo> power_state_info = GetInfoForState(d_state);

    if (power_state_info.is_error()) {
      zxlogf(ERROR, "Failed to get info for D state %d", d_state);
      return power_state_info.error_value();
    }

    if (!power_state_info->power_resources.empty() || power_state_info->defines_psx_method) {
      supported_power_states_.insert({d_state, *power_state_info});
    }
  }

  // If power resources are provided for D3hot, D3cold is supported.
  if (PowerStateInfo* d3hot_state = GetPowerStateInfo(DEV_POWER_STATE_D3HOT)) {
    if (!d3hot_state->power_resources.empty()) {
      PowerStateInfo d3cold_state{.d_state = DEV_POWER_STATE_D3COLD,
                                  .supported_s_states{0, 1, 2, 3, 4}};
      supported_power_states_.insert({DEV_POWER_STATE_D3COLD, d3cold_state});
    }
  }

  // If D0 is supported, D3hot must be supported.
  if (GetPowerStateInfo(DEV_POWER_STATE_D0) && !GetPowerStateInfo(DEV_POWER_STATE_D3HOT)) {
    PowerStateInfo d3hot_state{.d_state = DEV_POWER_STATE_D3HOT,
                               .supported_s_states{0, 1, 2, 3, 4}};
    supported_power_states_.insert({DEV_POWER_STATE_D3HOT, d3hot_state});
  }

  // Call _SxD methods to figure out valid D state to S state mapping.
  // This removes any mappings which were valid according to power resource system_levels but are
  // invalid according to the _SxD methods.
  for (uint8_t s_state = 1; s_state <= 4; ++s_state) {
    fbl::String method_name = fbl::StringPrintf("_S%dD", s_state);
    auto sxd = acpi_->EvaluateObject(acpi_handle_, method_name.c_str(), std::nullopt);
    if (sxd.is_ok()) {
      for (uint8_t d_state = DEV_POWER_STATE_D0; d_state < static_cast<uint8_t>(sxd->Integer.Value);
           ++d_state) {
        if (PowerStateInfo* power_state = GetPowerStateInfo(d_state)) {
          power_state->supported_s_states.erase(s_state);
        }
      }
    }
  }

  zx_status_t result = ConfigureInitialPowerState();
  if (result != ZX_OK) {
    return result;
  }

  return ZX_OK;
}

std::unordered_map<uint8_t, DevicePowerState> Device::GetSupportedPowerStates() {
  std::unordered_map<uint8_t, DevicePowerState> states;

  for (const auto& power_state : supported_power_states_) {
    states.insert({power_state.first,
                   DevicePowerState(power_state.first, power_state.second.supported_s_states)});
  }

  return states;
}

zx_status_t Device::Resume(const PowerStateInfo& requested_state_info) {
  PowerStateInfo* current_state_info = GetPowerStateInfo(current_power_state_);

  zx_status_t status = manager_->ReferencePowerResources(requested_state_info.power_resources);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to reference power resources for ACPI device: %s",
           zx_status_get_string(status));
    return status;
  }

  status = manager_->DereferencePowerResources(current_state_info->power_resources);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to dereference power resources for ACPI device: %s",
           zx_status_get_string(status));
    goto undo2;
  }

  status = CallPsxMethod(requested_state_info);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to call PSx method for ACPI device: %s", zx_status_get_string(status));
    goto undo1;
  }

  return ZX_OK;

undo1:
  manager_->ReferencePowerResources(current_state_info->power_resources);
undo2:
  manager_->DereferencePowerResources(requested_state_info.power_resources);
  return status;
}

zx_status_t Device::Suspend(const PowerStateInfo& requested_state_info) {
  PowerStateInfo* current_state_info = GetPowerStateInfo(current_power_state_);
  zx_status_t status;
  bool called_psx_method = false;

  // When transitioning from D3hot to D3cold, we've already called _PS3 so skip it.
  if (current_power_state_ != DEV_POWER_STATE_D3HOT ||
      requested_state_info.d_state != DEV_POWER_STATE_D3COLD) {
    called_psx_method = true;
    // When transitioning from D0 to D3cold, we need to call _PS3.
    if (current_power_state_ == DEV_POWER_STATE_D0 &&
        requested_state_info.d_state == DEV_POWER_STATE_D3COLD) {
      status = CallPsxMethod(*GetPowerStateInfo(DEV_POWER_STATE_D3HOT));
    } else {
      status = CallPsxMethod(requested_state_info);
    }

    if (status != ZX_OK) {
      zxlogf(ERROR, "Failed to call PSx method for ACPI device: %s", zx_status_get_string(status));
      return status;
    }
  }

  status = manager_->ReferencePowerResources(requested_state_info.power_resources);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to reference power resources for ACPI device: %s",
           zx_status_get_string(status));
    goto undo2;
  }

  status = manager_->DereferencePowerResources(current_state_info->power_resources);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to dereference power resources for ACPI device: %s",
           zx_status_get_string(status));
    goto undo1;
  }

  return ZX_OK;

undo1:
  manager_->DereferencePowerResources(requested_state_info.power_resources);
undo2:
  if (called_psx_method) {
    CallPsxMethod(*current_state_info);
  }
  return status;
}

PowerStateTransitionResponse Device::TransitionToPowerState(uint8_t requested_state) {
  if (current_power_state_ == requested_state) {
    return PowerStateTransitionResponse(ZX_OK, current_power_state_);
  }

  PowerStateInfo* requested_state_info = GetPowerStateInfo(requested_state);
  if (requested_state_info == nullptr) {
    zxlogf(ERROR, "Tried to transition an ACPI device to an unsupported power state.");
    return PowerStateTransitionResponse(ZX_ERR_NOT_SUPPORTED, current_power_state_);
  }

  // Cannot transition between non-D0 states.
  if (current_power_state_ != DEV_POWER_STATE_D0 && requested_state != DEV_POWER_STATE_D0) {
    // Unless transitioning from D3hot to D3cold.
    if (current_power_state_ != DEV_POWER_STATE_D3HOT ||
        requested_state != DEV_POWER_STATE_D3COLD) {
      zxlogf(ERROR, "Cannot transition an ACPI device from state %d to %d.", current_power_state_,
             requested_state);
      return PowerStateTransitionResponse(ZX_ERR_NOT_SUPPORTED, current_power_state_);
    }
  }

  zx_status_t status;
  if (requested_state == DEV_POWER_STATE_D0) {
    status = Resume(*requested_state_info);
  } else {
    status = Suspend(*requested_state_info);
  }

  if (status != ZX_OK) {
    return PowerStateTransitionResponse(status, current_power_state_);
  }

  current_power_state_ = requested_state;
  return PowerStateTransitionResponse(ZX_OK, current_power_state_);
}

zx::result<> Device::AddDevice(const char* name, cpp20::span<zx_device_prop_t> props,
                               cpp20::span<zx_device_str_prop_t> str_props, uint32_t flags) {
  auto outgoing = PrepareOutgoing();
  if (outgoing.is_error()) {
    zxlogf(ERROR, "failed to add acpi device '%s' - while setting up outgoing: %s", name,
           outgoing.status_string());
    return outgoing.take_error();
  }

  // A node can either have children manually added to it, or have drivers bound to it. To make this
  // work and preserve the tree topology of ACPI we create a passthrough node called
  // 'passthrough-device' which is what drivers bind to.
  bool needs_passthrough = false;
  if (!(flags & DEVICE_ADD_NON_BINDABLE)) {
    needs_passthrough = true;
  }

  zx_status_t status = DdkAdd(ddk::DeviceAddArgs(name).set_flags(DEVICE_ADD_NON_BINDABLE));
  if (status != ZX_OK) {
    return zx::error(status);
  }
  if (!needs_passthrough) {
    return zx::ok();
  }

  static const zx_protocol_device_t passthrough_proto = {
      .version = DEVICE_OPS_VERSION,
      .init =
          [](void* ctx) {
            Device* dev = static_cast<Device*>(ctx);
            zx_status_t result = ZX_OK;
            switch (dev->bus_type_) {
              case BusType::kSpi:
                result = device_add_metadata(dev->passthrough_dev_, DEVICE_METADATA_SPI_CHANNELS,
                                             dev->metadata_.data(), dev->metadata_.size());
                break;
              case BusType::kI2c:
                result = device_add_metadata(dev->passthrough_dev_, DEVICE_METADATA_I2C_CHANNELS,
                                             dev->metadata_.data(), dev->metadata_.size());
                break;
              default:
                break;
            }

            device_init_reply_args_t args{};
            device_init_reply(dev->passthrough_dev_, result, &args);
          },
      .release = [](void* dev) {},
  };

  std::array offers = {
      fuchsia_hardware_acpi::Service::Name,
  };

  device_add_args_t passthrough_args{
      .version = DEVICE_ADD_ARGS_VERSION,
      .name = "pt",
      .ctx = this,
      .ops = &passthrough_proto,
      .props = props.data(),
      .prop_count = static_cast<uint32_t>(props.size()),
      .str_props = str_props.data(),
      .str_prop_count = static_cast<uint32_t>(str_props.size()),
      .proto_id = ZX_PROTOCOL_ACPI,
      .fidl_service_offers = offers.data(),
      .fidl_service_offer_count = offers.size(),
      .flags = flags | DEVICE_ADD_MUST_ISOLATE | DEVICE_ADD_ALLOW_MULTI_COMPOSITE,
      .outgoing_dir_channel = outgoing->release(),
  };

  status = device_add(zxdev(), &passthrough_args, &passthrough_dev_);
  if (status != ZX_OK) {
    zxlogf(WARNING, "Failed to add passthrough device for '%s': %s", name,
           zx_status_get_string(status));
    // Do not fail here so that child devices can still get added.
  }

  return zx::ok();
}

void Device::GetBusId(GetBusIdCompleter::Sync& completer) {
  if (bus_id_ == UINT32_MAX) {
    completer.ReplyError(ZX_ERR_BAD_STATE);
  } else {
    completer.ReplySuccess(bus_id_);
  }
}

void Device::EvaluateObject(EvaluateObjectRequestView request,
                            EvaluateObjectCompleter::Sync& completer) {
  auto helper = EvaluateObjectFidlHelper::FromRequest(acpi_, acpi_handle_, request);
  fidl::Arena<> alloc;
  auto result = helper.Evaluate(alloc);
  if (result.is_error()) {
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status(result.error_value()));
  } else {
    completer.ReplySuccess(std::move(result.value().response().result));
  }
}

zx::result<zx::interrupt> Device::GetInterrupt(size_t index) {
  std::scoped_lock guard{lock_};
  zx_status_t st = ReportCurrentResources();
  if (st != ZX_OK) {
    zxlogf(ERROR, "Internal error evaluating resources: %s", zx_status_get_string(st));
    return zx::error(ZX_ERR_INTERNAL);
  }

  if (index >= irqs_.size()) {
    return zx::error(ZX_ERR_OUT_OF_RANGE);
  }

  const DeviceIrqResource& irq = irqs_[index];
  uint32_t mode;
  mode = ZX_INTERRUPT_MODE_DEFAULT;
  st = ZX_OK;
  switch (irq.trigger) {
    case ACPI_IRQ_TRIGGER_EDGE:
      switch (irq.polarity) {
        case ACPI_IRQ_ACTIVE_BOTH:
          mode = ZX_INTERRUPT_MODE_EDGE_BOTH;
          break;
        case ACPI_IRQ_ACTIVE_LOW:
          mode = ZX_INTERRUPT_MODE_EDGE_LOW;
          break;
        case ACPI_IRQ_ACTIVE_HIGH:
          mode = ZX_INTERRUPT_MODE_EDGE_HIGH;
          break;
        default:
          st = ZX_ERR_INVALID_ARGS;
          break;
      }
      break;
    case ACPI_IRQ_TRIGGER_LEVEL:
      switch (irq.polarity) {
        case ACPI_IRQ_ACTIVE_LOW:
          mode = ZX_INTERRUPT_MODE_LEVEL_LOW;
          break;
        case ACPI_IRQ_ACTIVE_HIGH:
          mode = ZX_INTERRUPT_MODE_LEVEL_HIGH;
          break;
        default:
          st = ZX_ERR_INVALID_ARGS;
          break;
      }
      break;
    default:
      st = ZX_ERR_INVALID_ARGS;
      break;
  }
  if (st != ZX_OK) {
    return zx::error(st);
  }
  // Please do not use get_root_resource() in new code. See fxbug.dev/31358.
  zx::interrupt out_irq;
  st = zx::interrupt::create(*zx::unowned_resource{get_root_resource()}, irq.pin,
                             ZX_INTERRUPT_REMAP_IRQ | mode, &out_irq);
  if (st != ZX_OK) {
    zxlogf(ERROR, "Internal error creating interrupt: %s", zx_status_get_string(st));
    return zx::error(ZX_ERR_INTERNAL);
  }

  return zx::ok(std::move(out_irq));
}

void Device::MapInterrupt(MapInterruptRequestView request, MapInterruptCompleter::Sync& completer) {
  auto result = GetInterrupt(request->index);
  if (result.is_error()) {
    completer.ReplyError(result.error_value());
  } else {
    completer.ReplySuccess(std::move(*result));
  }
}

void Device::GetPio(GetPioRequestView request, GetPioCompleter::Sync& completer) {
  std::scoped_lock guard{lock_};
  zx_status_t st = ReportCurrentResources();
  if (st != ZX_OK) {
    zxlogf(ERROR, "Internal error evaluating resources: %s", zx_status_get_string(st));
    completer.ReplyError(ZX_ERR_INTERNAL);
    return;
  }

  if (request->index >= pio_resources_.size()) {
    completer.ReplyError(ZX_ERR_OUT_OF_RANGE);
    return;
  }

  const DevicePioResource& res = pio_resources_[request->index];

  char name[ZX_MAX_NAME_LEN];
  snprintf(name, ZX_MAX_NAME_LEN, "ioport-%u", request->index);

  zx::resource out_pio;
  // Please do not use get_root_resource() in new code. See fxbug.dev/31358.
  zx_status_t status =
      zx::resource::create(*zx::unowned_resource{get_root_resource()}, ZX_RSRC_KIND_IOPORT,
                           res.base_address, res.address_length, name, 0, &out_pio);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Internal error creating resource: %s", zx_status_get_string(status));
    completer.ReplyError(ZX_ERR_INTERNAL);
  } else {
    completer.ReplySuccess(std::move(out_pio));
  }
}

void Device::InstallNotifyHandler(InstallNotifyHandlerRequestView request,
                                  InstallNotifyHandlerCompleter::Sync& completer) {
  // Try and take the notification handler.
  // Will set is_active to true if is_active is already true.
  bool is_active = false;
  notify_handler_active_.compare_exchange_strong(is_active, true, std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
  if (notify_handler_ && notify_handler_->is_valid() && is_active) {
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status::kAlreadyExists);
    return;
  }
  notify_handler_type_ = static_cast<uint32_t>(request->mode);

  if (!request->handler.is_valid()) {
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status::kBadParameter);
    return;
  }

  if (request->mode.has_unknown_bits()) {
    zxlogf(WARNING, "Unknown mode bits for notify handler ignored: 0x%x",
           uint32_t(request->mode.unknown_bits()));
  }

  uint32_t mode(request->mode & fuchsia_hardware_acpi::wire::NotificationMode::kMask);

  auto async_completer = completer.ToAsync();
  std::optional<fpromise::promise<void>> teardown_finished;
  notify_teardown_finished_.swap(teardown_finished);
  auto promise =
      std::move(teardown_finished)
          .value_or(fpromise::make_ok_promise())
          .and_then([this, mode, async_completer = std::move(async_completer),
                     handler = std::move(request->handler)]() mutable {
            pending_notify_count_.store(0, std::memory_order_release);
            // Reset the "teardown finished" promise.
            fpromise::bridge<void> bridge;
            notify_teardown_finished_ = bridge.consumer.promise();
            auto notify_event_handler =
                std::make_unique<NotifyEventHandler>(this, std::move(bridge.completer));

            fidl::WireSharedClient<fuchsia_hardware_acpi::NotifyHandler> client(
                std::move(handler), dispatcher_, std::move(notify_event_handler));
            notify_handler_ = std::move(client);
            auto status = acpi_->InstallNotifyHandler(
                acpi_handle_, mode, Device::DeviceObjectNotificationHandler, this);
            if (status.is_error()) {
              notify_handler_.reset();
              async_completer.ReplyError(fuchsia_hardware_acpi::wire::Status(status.error_value()));
              return;
            }

            async_completer.ReplySuccess();
          })
          .box();
  executor_.schedule_task(std::move(promise));
}

void Device::DeviceObjectNotificationHandler(ACPI_HANDLE object, uint32_t value, void* context) {
  Device* device = static_cast<Device*>(context);
  if (device->pending_notify_count_.load(std::memory_order_acquire) >= kMaxPendingNotifications) {
    if (!device->notify_count_warned_) {
      zxlogf(ERROR, "%s: too many un-handled pending notifications. Will drop notifications.",
             device->name());
      device->notify_count_warned_ = true;
    }
    return;
  }

  device->pending_notify_count_.fetch_add(1, std::memory_order_acq_rel);
  if (device->notify_handler_ && device->notify_handler_->is_valid()) {
    device->notify_handler_.value()->Handle(value).ThenExactlyOnce(
        [device](fidl::WireUnownedResult<fuchsia_hardware_acpi::NotifyHandler::Handle>& result) {
          if (!result.ok()) {
            return;
          }
          device->pending_notify_count_.fetch_sub(1, std::memory_order_acq_rel);
        });
  }
}

void Device::RemoveNotifyHandler(RemoveNotifyHandlerCompleter::Sync& completer) {
  auto status = RemoveNotifyHandler();
  if (status != AE_OK) {
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status(status));
    return;
  }
  completer.ReplySuccess();
}

ACPI_STATUS Device::RemoveNotifyHandler() {
  // Try and mark the notify handler as inactive. If this fails, then someone else marked it as
  // inactive.
  // If this succeeds, then we're going to tear down the notify handler.
  bool is_active = true;
  notify_handler_active_.compare_exchange_strong(is_active, false, std::memory_order_acq_rel,
                                                 std::memory_order_acquire);
  if (!is_active) {
    return AE_OK;
  }
  auto status = acpi_->RemoveNotifyHandler(acpi_handle_, notify_handler_type_,
                                           Device::DeviceObjectNotificationHandler);
  if (status.is_error()) {
    zxlogf(ERROR, "Failed to remove notification handler from '%s': %d", name(),
           status.error_value());
    return status.error_value();
  }
  notify_handler_->AsyncTeardown();
  return AE_OK;
}

void Device::AcquireGlobalLock(AcquireGlobalLockCompleter::Sync& completer) {
  if (!can_use_global_lock_) {
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status::kAccess);
    return;
  }

  GlobalLockHandle::Create(acpi_, dispatcher_, completer.ToAsync());
}

ACPI_STATUS Device::AddressSpaceHandler(uint32_t function, ACPI_PHYSICAL_ADDRESS physical_address,
                                        uint32_t bit_width, UINT64* value, void* handler_ctx,
                                        void* region_ctx) {
  HandlerCtx* ctx = static_cast<HandlerCtx*>(handler_ctx);
  std::scoped_lock lock(ctx->device->address_handler_lock_);
  auto client = ctx->device->address_handlers_.find(ctx->space_type);
  if (client == ctx->device->address_handlers_.end()) {
    zxlogf(ERROR, "No handler found for space %u", ctx->space_type);
    return AE_NOT_FOUND;
  }

  switch (function) {
    case ACPI_READ: {
      auto result = client->second.sync()->Read(physical_address, bit_width);
      if (!result.ok()) {
        zxlogf(ERROR, "FIDL Read failed: %s", result.FormatDescription().data());
        return AE_ERROR;
      }
      if (result->is_error()) {
        return static_cast<ACPI_STATUS>(result->error_value());
      }
      *value = result->value()->value;
      break;
    }
    case ACPI_WRITE: {
      auto result = client->second.sync()->Write(physical_address, bit_width, *value);
      if (!result.ok()) {
        zxlogf(ERROR, "FIDL Write failed: %s", result.FormatDescription().data());
        return AE_ERROR;
      }
      if (result->is_error()) {
        return static_cast<ACPI_STATUS>(result->error_value());
      }
      break;
    }
  }
  return AE_OK;
}

void Device::InstallAddressSpaceHandler(InstallAddressSpaceHandlerRequestView request,
                                        InstallAddressSpaceHandlerCompleter::Sync& completer) {
  if (request->space.IsUnknown()) {
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status::kNotSupported);
    return;
  }

  std::scoped_lock lock(address_handler_lock_);
  uint32_t space(request->space);
  if (address_handlers_.find(space) != address_handlers_.end()) {
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status::kAlreadyExists);
    return;
  }

  // Allocated using new, and then destroyed by the FIDL teardown handler.
  auto ctx = std::make_unique<HandlerCtx>();
  ctx->device = this;
  ctx->space_type = space;

  // It's safe to do this now, because any address space requests will try and acquire the
  // address_handler_lock_. As a result, nothing will happen until we've finished setting up the
  // FIDL client and our bookkeeping below.
  auto status = acpi_->InstallAddressSpaceHandler(acpi_handle_, static_cast<uint8_t>(space),
                                                  AddressSpaceHandler, nullptr, ctx.get());
  if (status.is_error()) {
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status(status.error_value()));
    return;
  }

  fpromise::bridge<void> bridge;
  fidl::WireSharedClient<fuchsia_hardware_acpi::AddressSpaceHandler> client(
      std::move(request->handler), dispatcher_,
      fidl::AnyTeardownObserver::ByCallback(
          [this, ctx = std::move(ctx), space, completer = std::move(bridge.completer)]() mutable {
            std::scoped_lock lock(address_handler_lock_);
            // Remove the address space handler from ACPICA.
            auto result = acpi_->RemoveAddressSpaceHandler(
                acpi_handle_, static_cast<uint8_t>(space), AddressSpaceHandler);
            if (result.is_error()) {
              zxlogf(ERROR, "Failed to remove address space handler: %d", result.status_value());
              // We're in a strange state now. Claim that we've torn down, but avoid freeing
              // things to minimise the chance of a UAF in the address space handler.
              ZX_DEBUG_ASSERT_MSG(false, "Failed to remove address space handler: %d",
                                  result.status_value());
              completer.complete_ok();
              return;
            }
            // Clean up other things.
            address_handlers_.erase(space);
            completer.complete_ok();
          }));

  // Everything worked, so insert our book-keeping.
  address_handler_teardown_finished_.emplace_back(bridge.consumer.promise());
  address_handlers_.emplace(space, std::move(client));

  completer.ReplySuccess();
}

void Device::SetWakeDevice(SetWakeDeviceRequestView request,
                           SetWakeDeviceCompleter::Sync& completer) {
  // Get the GPE device and GPE number associated with the device's Power Resource for Wake
  auto prw_result = acpi_->EvaluateObject(acpi_handle_, "_PRW", std::nullopt);
  if (prw_result.is_error()) {
    zxlogf(ERROR, "EvaluateObject failed: %d", int(prw_result.error_value()));
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status(prw_result.error_value()));
    return;
  }

  if (prw_result->Type != ACPI_TYPE_PACKAGE || prw_result->Package.Count < 2) {
    zxlogf(ERROR, "Unexpected response from EvaluateObject");
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status::kBadData);
    return;
  }

  if (request->requested_state > prw_result->Package.Elements[1].Integer.Value) {
    zxlogf(ERROR,
           "Requested sleep state (%u) is deeper than the deepest sleep state that the device can "
           "wake the system from (%llu)",
           request->requested_state, prw_result->Package.Elements[1].Integer.Value);
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status::kNotSupported);
    return;
  }

  ACPI_HANDLE gpe_dev = nullptr;
  uint32_t gpe_num;
  // See ACPI v6.3 Section 7.3.13
  // The first object within the _PRW object is the information about the _GPE object
  // associated with the device. This evaluates to either an integer or a package.
  // The integer specifies the bit in the FADT GPEx_STS blocks to use.
  // The package contains the reference to the device and the index in that device where the
  // event is.
  auto& gpe_info = prw_result->Package.Elements[0];
  if (gpe_info.Type == ACPI_TYPE_INTEGER) {
    gpe_num = static_cast<uint32_t>(gpe_info.Integer.Value);
  } else if (gpe_info.Type == ACPI_TYPE_PACKAGE) {
    if (gpe_info.Package.Count != 2 ||
        gpe_info.Package.Elements[0].Type != ACPI_TYPE_LOCAL_REFERENCE ||
        gpe_info.Package.Elements[1].Type != ACPI_TYPE_INTEGER) {
      zxlogf(ERROR, "Unexpected response from EvaluateObject");
      completer.ReplyError(fuchsia_hardware_acpi::wire::Status::kBadData);
      return;
    }
    gpe_dev = gpe_info.Package.Elements[0].Reference.Handle;
    gpe_num = static_cast<uint32_t>(gpe_info.Package.Elements[1].Integer.Value);
  } else {
    zxlogf(ERROR, "Unexpected response from EvaluateObject");
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status::kBadData);
    return;
  }

  auto status = acpi_->SetGpeWakeMask(gpe_dev, gpe_num, true);
  if (status.is_error()) {
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status(status.error_value()));
    return;
  }

  zxlogf(INFO, "Deepest sleep state that device can wake system from: %llu",
         prw_result->Package.Elements[1].Integer.Value);

  // Get the power resources associated with the _PRW object and turn them all on.
  std::vector<ACPI_HANDLE> power_resources;
  // The first two elements of the _PRW object are the event info, and the lowest sleep state the
  // device can wake from. The rest of the elements are power resources.
  uint64_t pwr_res_count = prw_result->Package.Count - 2;
  for (uint64_t i = 0; i < pwr_res_count; i++) {
    ACPI_OBJECT power_resource_reference = prw_result->Package.Elements[i + 2];
    const PowerResource* power_resource =
        manager_->AddPowerResource(power_resource_reference.Reference.Handle);

    if (power_resource == nullptr) {
      zxlogf(ERROR, "Failed to add power resource");
    }

    if (power_resource && !power_resource->is_on()) {
      power_resources.push_back(power_resource->handle());
    }
  }

  zx_status_t zx_status = manager_->ReferencePowerResources(power_resources);
  if (zx_status != ZX_OK) {
    zxlogf(ERROR, "Failed to reference power resources for ACPI device: %s",
           zx_status_get_string(zx_status));
    completer.ReplyError(fuchsia_hardware_acpi::wire::Status::kError);
  }
  completer.ReplySuccess();
}
}  // namespace acpi
