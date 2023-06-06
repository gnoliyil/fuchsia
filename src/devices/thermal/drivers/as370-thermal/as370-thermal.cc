// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "as370-thermal.h"

#include <lib/ddk/binding_driver.h>
#include <lib/ddk/device.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/device-protocol/pdev-fidl.h>
#include <lib/zx/time.h>

#include <ddktl/fidl.h>
#include <fbl/alloc_checker.h>

#include "as370-thermal-reg.h"

namespace {

constexpr uint32_t kEocLoopTimeout = 20000;
constexpr zx::duration kEocLoopSleepTime = zx::usec(100);

constexpr float SensorReadingToTemperature(int32_t reading) {
  reading = reading * 251802 / 4096 - 85525;
  return static_cast<float>(reading) / 1000.0f;
}

}  // namespace

namespace thermal {

using fuchsia_hardware_thermal::wire::OperatingPoint;

zx_status_t As370Thermal::Create(void* ctx, zx_device_t* parent) {
  auto pdev = ddk::PDevFidl::FromFragment(parent);
  if (!pdev.is_valid()) {
    zxlogf(ERROR, "%s: Failed to get platform device protocol", __func__);
    return ZX_ERR_NO_RESOURCES;
  }

  zx::result clock_result =
      ddk::Device<void>::DdkConnectFragmentFidlProtocol<fuchsia_hardware_clock::Service::Clock>(
          parent, "clock");
  if (clock_result.is_error()) {
    zxlogf(WARNING, "Failed to get clock protocol from fragment: %s", clock_result.status_string());
    return clock_result.status_value();
  }
  fidl::ClientEnd<fuchsia_hardware_clock::Clock> cpu_clock = std::move(*clock_result);

  zx::result cpu_power_result =
      ddk::Device<void>::DdkConnectFragmentFidlProtocol<fuchsia_hardware_power::Service::Device>(
          parent, "power");
  if (cpu_power_result.is_error()) {
    zxlogf(WARNING, "Failed to get cpu power protocol from fragment: %s",
           cpu_power_result.status_string());
    return ZX_ERR_NO_RESOURCES;
  }
  fidl::ClientEnd<fuchsia_hardware_power::Device> cpu_power = std::move(*cpu_power_result);

  std::optional<ddk::MmioBuffer> mmio;
  zx_status_t status = pdev.MapMmio(0, &mmio);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Failed to map MMIO: %d", __func__, status);
    return status;
  }

  size_t actual_size = 0;
  ThermalDeviceInfo device_info = {};
  if ((status = device_get_metadata(parent, DEVICE_METADATA_THERMAL_CONFIG, &device_info,
                                    sizeof(device_info), &actual_size)) != ZX_OK) {
    zxlogf(ERROR, "%s: Failed to get metadata: %d", __func__, status);
    return status;
  }
  if (actual_size != sizeof(device_info)) {
    zxlogf(ERROR, "%s: Metadata size mismatch", __func__);
    return ZX_ERR_BAD_STATE;
  }

  fbl::AllocChecker ac;
  auto device = fbl::make_unique_checked<As370Thermal>(&ac, parent, *std::move(mmio), device_info,
                                                       std::move(cpu_clock), std::move(cpu_power));
  if (!ac.check()) {
    zxlogf(ERROR, "%s: Failed to allocate device memory", __func__);
    return ZX_ERR_NO_MEMORY;
  }

  if ((status = device->Init()) != ZX_OK) {
    return status;
  }

  if ((status = device->DdkAdd("as370-thermal")) != ZX_OK) {
    zxlogf(ERROR, "%s: DdkAdd failed: %d", __func__, status);
    return status;
  }

  [[maybe_unused]] auto* dummy = device.release();
  return ZX_OK;
}

void As370Thermal::GetInfo(GetInfoCompleter::Sync& completer) {
  completer.Reply(ZX_ERR_NOT_SUPPORTED, nullptr);
}

void As370Thermal::GetDeviceInfo(GetDeviceInfoCompleter::Sync& completer) {
  ThermalDeviceInfo device_info_copy = device_info_;
  completer.Reply(ZX_OK, fidl::ObjectView<ThermalDeviceInfo>::FromExternal(&device_info_copy));
}

void As370Thermal::GetDvfsInfo(GetDvfsInfoRequestView request,
                               GetDvfsInfoCompleter::Sync& completer) {
  if (request->power_domain != PowerDomain::kBigClusterPowerDomain) {
    completer.Reply(ZX_ERR_NOT_SUPPORTED, nullptr);
  } else {
    OperatingPoint dvfs_info_copy = device_info_.opps[static_cast<uint32_t>(request->power_domain)];
    completer.Reply(ZX_OK, fidl::ObjectView<OperatingPoint>::FromExternal(&dvfs_info_copy));
  }
}

void As370Thermal::GetTemperatureCelsius(GetTemperatureCelsiusCompleter::Sync& completer) {
  PvtCtrl::Get()
      .ReadFrom(&mmio_)
      .set_pmos_sel(0)
      .set_nmos_sel(0)
      .set_voltage_sel(0)
      .set_temperature_sel(1)
      .WriteTo(&mmio_)
      .set_enable(1)
      .WriteTo(&mmio_)
      .set_power_down(0)
      .WriteTo(&mmio_);

  auto pvt_status = PvtStatus::Get().FromValue(0);
  for (uint32_t i = 0; i < kEocLoopTimeout && pvt_status.ReadFrom(&mmio_).eoc() == 0; i++) {
    zx::nanosleep(zx::deadline_after(kEocLoopSleepTime));
  }

  PvtCtrl::Get().FromValue(0).set_power_down(1).WriteTo(&mmio_);
  if (pvt_status.eoc() == 0) {
    zxlogf(ERROR, "%s: Timed out waiting for temperature reading", __func__);
    completer.Reply(ZX_ERR_TIMED_OUT, 0.0f);
  } else {
    completer.Reply(ZX_OK, SensorReadingToTemperature(pvt_status.data()));
  }
}

void As370Thermal::GetStateChangeEvent(GetStateChangeEventCompleter::Sync& completer) {
  completer.Reply(ZX_ERR_NOT_SUPPORTED, {});
}

void As370Thermal::GetStateChangePort(GetStateChangePortCompleter::Sync& completer) {
  completer.Reply(ZX_ERR_NOT_SUPPORTED, {});
}

void As370Thermal::SetTripCelsius(SetTripCelsiusRequestView request,
                                  SetTripCelsiusCompleter::Sync& completer) {
  completer.Reply(ZX_ERR_NOT_SUPPORTED);
}

void As370Thermal::GetDvfsOperatingPoint(GetDvfsOperatingPointRequestView request,
                                         GetDvfsOperatingPointCompleter::Sync& completer) {
  if (request->power_domain != PowerDomain::kBigClusterPowerDomain) {
    completer.Reply(ZX_ERR_NOT_SUPPORTED, 0);
  } else {
    completer.Reply(ZX_OK, operating_point_);
  }
}

void As370Thermal::SetDvfsOperatingPoint(SetDvfsOperatingPointRequestView request,
                                         SetDvfsOperatingPointCompleter::Sync& completer) {
  if (request->power_domain != PowerDomain::kBigClusterPowerDomain) {
    completer.Reply(ZX_ERR_NOT_SUPPORTED);
  } else if (request->op_idx >=
             device_info_.opps[static_cast<uint32_t>(request->power_domain)].count) {
    completer.Reply(ZX_ERR_INVALID_ARGS);
  } else {
    completer.Reply(SetOperatingPoint(request->op_idx));
  }
}

void As370Thermal::GetFanLevel(GetFanLevelCompleter::Sync& completer) {
  completer.Reply(ZX_ERR_NOT_SUPPORTED, 0);
}

void As370Thermal::SetFanLevel(SetFanLevelRequestView request,
                               SetFanLevelCompleter::Sync& completer) {
  completer.Reply(ZX_ERR_NOT_SUPPORTED);
}

zx_status_t As370Thermal::Init() {
  PvtCtrl::Get().FromValue(0).set_power_down(1).WriteTo(&mmio_);

  const OperatingPoint& operating_points =
      device_info_.opps[static_cast<uint32_t>(PowerDomain::kBigClusterPowerDomain)];
  const auto max_operating_point = static_cast<uint16_t>(operating_points.count - 1);

  fidl::WireResult result = cpu_power_->RegisterPowerDomain(
      operating_points.opp[0].volt_uv, operating_points.opp[max_operating_point].volt_uv);

  if (!result.ok()) {
    zxlogf(ERROR, "%s: Failed to send request for register power domain: %s", __func__,
           result.status_string());
    return result.status();
  }

  if (result->is_error()) {
    zxlogf(ERROR, "%s: Failed to register power domain: %s", __func__,
           zx_status_get_string(result.value().error_value()));
    return result.value().error_value();
  }

  return SetOperatingPoint(max_operating_point);
}

zx_status_t As370Thermal::SetOperatingPoint(uint16_t op_idx) {
  const auto& opps =
      device_info_.opps[static_cast<uint32_t>(PowerDomain::kBigClusterPowerDomain)].opp;

  if (opps[op_idx].freq_hz > opps[operating_point_].freq_hz) {
    fidl::WireResult result = cpu_power_->RequestVoltage(opps[op_idx].volt_uv);
    if (!result.ok()) {
      zxlogf(ERROR, "%s: Failed to send request for set voltage: %s", __func__,
             result.status_string());
      return result.status();
    }

    if (result->is_error()) {
      zxlogf(ERROR, "%s:Failed to set voltage: %s", __func__,
             zx_status_get_string(result.value().error_value()));
      return result.value().error_value();
    }

    uint32_t actual_voltage = result.value()->actual_voltage;

    if (actual_voltage != opps[op_idx].volt_uv) {
      zxlogf(ERROR, "%s: Failed to set exact voltage: set %u, wanted %u", __func__, actual_voltage,
             opps[op_idx].volt_uv);
      return ZX_ERR_BAD_STATE;
    }

    fidl::WireResult set_rate_result = cpu_clock_->SetRate(opps[op_idx].freq_hz);
    if (!set_rate_result.ok()) {
      zxlogf(ERROR, "Failed to send SetRate request to clock: %s\n",
             set_rate_result.status_string());
      return set_rate_result.status();
    }
    if (set_rate_result->is_error()) {
      zxlogf(ERROR, "Failed to set rate for clock: %s\n",
             zx_status_get_string(set_rate_result->error_value()));
      return set_rate_result->error_value();
    }
  } else {
    fidl::WireResult set_rate_result = cpu_clock_->SetRate(opps[op_idx].freq_hz);
    if (!set_rate_result.ok()) {
      zxlogf(ERROR, "Failed to send SetRate request to clock: %s\n",
             set_rate_result.status_string());
      return set_rate_result.status();
    }
    if (set_rate_result->is_error()) {
      zxlogf(ERROR, "Failed to set rate for clock: %s\n",
             zx_status_get_string(set_rate_result->error_value()));
      return set_rate_result->error_value();
    }

    fidl::WireResult result = cpu_power_->RequestVoltage(opps[op_idx].volt_uv);
    if (!result.ok()) {
      zxlogf(ERROR, "%s: Failed to send request for set voltage: %s", __func__,
             result.status_string());
      return result.status();
    }

    if (result->is_error()) {
      zxlogf(ERROR, "%s:Failed to set voltage: %s", __func__,
             zx_status_get_string(result.value().error_value()));
      return result.value().error_value();
    }

    uint32_t actual_voltage = result.value()->actual_voltage;
    if (actual_voltage != opps[op_idx].volt_uv) {
      zxlogf(ERROR, "%s: Failed to set exact voltage: set %u, wanted %u", __func__, actual_voltage,
             opps[op_idx].volt_uv);
      return ZX_ERR_BAD_STATE;
    }
  }

  operating_point_ = op_idx;
  return ZX_OK;
}

}  // namespace thermal

static constexpr zx_driver_ops_t as370_thermal_driver_ops = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = thermal::As370Thermal::Create;
  return ops;
}();

ZIRCON_DRIVER(as370_thermal, as370_thermal_driver_ops, "zircon", "0.1");
