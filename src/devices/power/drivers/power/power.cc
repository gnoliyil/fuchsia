// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "power.h"

#include <fuchsia/hardware/power/c/banjo.h>
#include <fuchsia/hardware/power/cpp/banjo.h>
#include <fuchsia/hardware/powerimpl/cpp/banjo.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <memory>

#include <ddk/metadata/power.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>

void GetUniqueId(uint64_t* id) {
  static std::atomic<size_t> unique_id = 0;
  *id = unique_id.fetch_add(1);
}

namespace power {

zx_status_t PowerDeviceFragmentChild::PowerRegisterPowerDomain(uint32_t min_needed_voltage_uV,
                                                               uint32_t max_supported_voltage_uV) {
  return power_device_->RegisterPowerDomain(fragment_device_id_, min_needed_voltage_uV,
                                            max_supported_voltage_uV);
}

zx_status_t PowerDeviceFragmentChild::PowerUnregisterPowerDomain() {
  return power_device_->UnregisterPowerDomain(fragment_device_id_);
}

zx_status_t PowerDeviceFragmentChild::PowerGetPowerDomainStatus(power_domain_status_t* out_status) {
  return power_device_->GetPowerDomainStatus(fragment_device_id_, out_status);
}

zx_status_t PowerDeviceFragmentChild::PowerGetSupportedVoltageRange(uint32_t* min_voltage,
                                                                    uint32_t* max_voltage) {
  return power_device_->GetSupportedVoltageRange(fragment_device_id_, min_voltage, max_voltage);
}

zx_status_t PowerDeviceFragmentChild::PowerRequestVoltage(uint32_t voltage,
                                                          uint32_t* actual_voltage) {
  return power_device_->RequestVoltage(fragment_device_id_, voltage, actual_voltage);
}

zx_status_t PowerDeviceFragmentChild::PowerGetCurrentVoltage(uint32_t index,
                                                             uint32_t* current_voltage) {
  return power_device_->GetCurrentVoltage(fragment_device_id_, index, current_voltage);
}

zx_status_t PowerDeviceFragmentChild::PowerWritePmicCtrlReg(uint32_t reg_addr, uint32_t value) {
  return power_device_->WritePmicCtrlReg(fragment_device_id_, reg_addr, value);
}

zx_status_t PowerDeviceFragmentChild::PowerReadPmicCtrlReg(uint32_t reg_addr, uint32_t* out_value) {
  return power_device_->ReadPmicCtrlReg(fragment_device_id_, reg_addr, out_value);
}

PowerDeviceFragmentChild* PowerDevice::GetFragmentChildLocked(uint64_t fragment_device_id) {
  for (auto& child : children_) {
    if (child->fragment_device_id() == fragment_device_id) {
      return child.get();
    }
  }
  return nullptr;
}

uint32_t PowerDevice::GetDependentCount() {
  fbl::AutoLock al(&power_device_lock_);
  return GetDependentCountLocked();
}

uint32_t PowerDevice::GetDependentCountLocked() {
  uint32_t count = 0;
  for (const auto& child : children_) {
    if (child->registered()) {
      count++;
    }
  }
  return count;
}

zx_status_t PowerDevice::GetSuitableVoltageLocked(uint32_t voltage, uint32_t* suitable_voltage) {
  uint32_t min_voltage_all_children = min_voltage_uV_;
  uint32_t max_voltage_all_children = max_voltage_uV_;
  for (auto& child : children_) {
    if (child->registered()) {
      min_voltage_all_children = std::max(min_voltage_all_children, child->min_needed_voltage_uV());
      max_voltage_all_children =
          std::min(max_voltage_all_children, child->max_supported_voltage_uV());
    }
  }
  if (min_voltage_all_children > max_voltage_all_children) {
    zxlogf(ERROR, "Supported voltage ranges of all the dependents do not intersect.");
    return ZX_ERR_NOT_FOUND;
  }
  *suitable_voltage = voltage;
  if (voltage < min_voltage_all_children) {
    *suitable_voltage = min_voltage_all_children;
  }
  if (voltage > max_voltage_all_children) {
    *suitable_voltage = max_voltage_all_children;
  }
  return ZX_OK;
}

zx_status_t PowerDevice::RegisterPowerDomain(uint64_t fragment_device_id,
                                             uint32_t min_needed_voltage_uV,
                                             uint32_t max_supported_voltage_uV) {
  zx_status_t status = ZX_OK;
  fbl::AutoLock al(&power_device_lock_);
  PowerDeviceFragmentChild* child = GetFragmentChildLocked(fragment_device_id);
  ZX_DEBUG_ASSERT(child != nullptr);
  child->set_min_needed_voltage_uV(std::max(min_needed_voltage_uV, min_voltage_uV_));
  child->set_max_supported_voltage_uV(std::min(max_supported_voltage_uV, max_voltage_uV_));
  if (child->registered()) {
    return ZX_OK;
  }
  child->set_registered(true);
  if (GetDependentCountLocked() == 1) {
    // First dependent. Make sure parent is enabled by registering for it.
    if (parent_power_.is_valid()) {
      status = parent_power_.RegisterPowerDomain(min_voltage_uV_, max_voltage_uV_);
      if (status != ZX_OK) {
        zxlogf(ERROR, "Failed to register with parent power domain");
        return status;
      }
    }
    status = power_impl_.EnablePowerDomain(index_);
    if (status != ZX_OK) {
      zxlogf(ERROR, "Failed to enabled this power domain");
      return status;
    }
  }

  return ZX_OK;
}

zx_status_t PowerDevice::UnregisterPowerDomain(uint64_t fragment_device_id) {
  zx_status_t status = ZX_OK;
  fbl::AutoLock al(&power_device_lock_);
  PowerDeviceFragmentChild* child = GetFragmentChildLocked(fragment_device_id);
  ZX_DEBUG_ASSERT(child != nullptr);
  if (!child->registered()) {
    return ZX_ERR_UNAVAILABLE;
  }
  child->set_registered(false);
  if (GetDependentCountLocked() == 0) {
    status = power_impl_.DisablePowerDomain(index_);
    if (status != ZX_OK) {
      zxlogf(ERROR, "Failed to disable power domain");
      return status;
    }
    if (parent_power_.is_valid() && status == ZX_OK) {
      status = parent_power_.UnregisterPowerDomain();
      if (status != ZX_OK) {
        zxlogf(ERROR, "Failed to unregister with parent power domain");
        return status;
      }
    }
  }

  return ZX_OK;
}

zx_status_t PowerDevice::GetPowerDomainStatus(uint64_t fragment_device_id,
                                              power_domain_status_t* out_status) {
  return power_impl_.GetPowerDomainStatus(index_, out_status);
}

zx_status_t PowerDevice::GetSupportedVoltageRange(uint64_t fragment_device_id,
                                                  uint32_t* min_voltage, uint32_t* max_voltage) {
  if (fixed_) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  *min_voltage = min_voltage_uV_;
  *max_voltage = max_voltage_uV_;
  return ZX_OK;
}

zx_status_t PowerDevice::RequestVoltage(uint64_t fragment_device_id, uint32_t voltage,
                                        uint32_t* actual_voltage) {
  fbl::AutoLock al(&power_device_lock_);
  if (fixed_) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  if (voltage < min_voltage_uV_ || voltage > max_voltage_uV_) {
    zxlogf(ERROR, "The voltage is not within supported voltage range of the power domain");
    return ZX_ERR_INVALID_ARGS;
  }
  PowerDeviceFragmentChild* child = GetFragmentChildLocked(fragment_device_id);
  ZX_DEBUG_ASSERT(child != nullptr);
  if (!child->registered()) {
    zxlogf(ERROR, "The device is not registered for the power domain");
    return ZX_ERR_UNAVAILABLE;
  }

  uint32_t suitable_voltage = voltage;
  zx_status_t status = GetSuitableVoltageLocked(voltage, &suitable_voltage);
  if (status != ZX_OK) {
    zxlogf(ERROR,
           "Unable to find a suitable voltage that matches all dependents of power domain\n");
    return status;
  }
  return power_impl_.RequestVoltage(index_, suitable_voltage, actual_voltage);
}

zx_status_t PowerDevice::GetCurrentVoltage(uint64_t fragment_device_id, uint32_t index,
                                           uint32_t* current_voltage) {
  fbl::AutoLock al(&power_device_lock_);
  return power_impl_.GetCurrentVoltage(index_, current_voltage);
}

zx_status_t PowerDevice::WritePmicCtrlReg(uint64_t fragment_device_id, uint32_t reg_addr,
                                          uint32_t value) {
  fbl::AutoLock al(&power_device_lock_);
  return power_impl_.WritePmicCtrlReg(index_, reg_addr, value);
}

zx_status_t PowerDevice::ReadPmicCtrlReg(uint64_t fragment_device_id, uint32_t reg_addr,
                                         uint32_t* out_value) {
  fbl::AutoLock al(&power_device_lock_);
  return power_impl_.ReadPmicCtrlReg(index_, reg_addr, out_value);
}

zx_status_t PowerDevice::DdkOpenProtocolSessionMultibindable(uint32_t proto_id, void* out) {
  if (proto_id != ZX_PROTOCOL_POWER) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  fbl::AutoLock al(&power_device_lock_);
  auto* proto = static_cast<ddk::AnyProtocol*>(out);
  fbl::AllocChecker ac;
  uint64_t id = 0;
  GetUniqueId(&id);
  std::unique_ptr<PowerDeviceFragmentChild> child(new (&ac) PowerDeviceFragmentChild(id, this));
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  children_.push_back(std::move(child));
  PowerDeviceFragmentChild* child_ptr = children_.back().get();

  proto->ctx = child_ptr;
  proto->ops = child_ptr->ops();
  return ZX_OK;
}

zx_status_t PowerDevice::DdkCloseProtocolSessionMultibindable(void* child_ctx) {
  fbl::AutoLock al(&power_device_lock_);
  auto child = reinterpret_cast<PowerDeviceFragmentChild*>(child_ctx);

  auto iter = children_.begin();
  for (; iter != children_.end(); iter++) {
    if (iter->get()->fragment_device_id() == child->fragment_device_id()) {
      break;
    }
  }

  if (iter != children_.end()) {
    children_.erase(iter);
  } else {
    zxlogf(ERROR, "%s: Unable to find the child with the given child_ctx", __FUNCTION__);
    return ZX_ERR_NOT_FOUND;
  }
  return ZX_OK;
}

void PowerDevice::DdkRelease() { delete this; }

zx_status_t PowerDevice::Create(void* ctx, zx_device_t* parent) {
  auto power_domain = ddk::GetMetadata<power_domain_t>(parent, DEVICE_METADATA_POWER_DOMAINS);
  if (!power_domain.is_ok()) {
    return power_domain.error_value();
  }

  auto index = power_domain->index;
  char name[20];
  snprintf(name, sizeof(name), "power-%u", index);
  ddk::PowerImplProtocolClient power_impl(parent, "power-impl");
  if (!power_impl.is_valid()) {
    zxlogf(ERROR, "%s: ZX_PROTOCOL_POWER_IMPL not available", __func__);
    return ZX_ERR_NO_RESOURCES;
  }

  // This is optional.
  ddk::PowerProtocolClient parent_power(parent, "power-parent");

  uint32_t min_voltage = 0, max_voltage = 0;
  bool fixed = false;
  zx_status_t status = power_impl.GetSupportedVoltageRange(index, &min_voltage, &max_voltage);
  if (status != ZX_OK && status != ZX_ERR_NOT_SUPPORTED) {
    return status;
  }
  if (status == ZX_ERR_NOT_SUPPORTED) {
    fixed = true;
  }
  fbl::AllocChecker ac;
  std::unique_ptr<PowerDevice> dev(new (&ac) PowerDevice(parent, index, power_impl, parent_power,
                                                         min_voltage, max_voltage, fixed));
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  zx_device_prop_t props[] = {
      {BIND_POWER_DOMAIN, 0, index},
  };

  status = dev->DdkAdd(
      ddk::DeviceAddArgs(name).set_flags(DEVICE_ADD_ALLOW_MULTI_COMPOSITE).set_props(props));
  if (status != ZX_OK) {
    return status;
  }

  // dev is now owned by devmgr.
  [[maybe_unused]] auto ptr = dev.release();

  return ZX_OK;
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = PowerDevice::Create;
  return ops;
}();

}  // namespace power

// clang-format off
ZIRCON_DRIVER(generic-power, power::driver_ops, "zircon", "0.1");
//clang-format on
