// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-cpu.h"

#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/platform-defs.h>
#include <lib/device-protocol/pdev-fidl.h>
#include <lib/inspect/cpp/inspector.h>
#include <lib/mmio/mmio.h>
#include <zircon/syscalls/smc.h>

#include <memory>
#include <vector>

#include <ddktl/fidl.h>
#include <fbl/string_buffer.h>

namespace amlogic_cpu {

namespace {
// Fragments are provided to this driver in groups of 4. Fragments are provided as follows:
// [4 fragments for cluster 0]
// [4 fragments for cluster 1]
// [...]
// [4 fragments for cluster n]
// Each fragment is a combination of the fixed string + id.
constexpr size_t kFragmentsPerPfDomain = 4;
constexpr size_t kFragmentsPerPfDomainA5 = 2;
constexpr size_t kFragmentsPerPfDomainA1 = 1;

constexpr zx_off_t kCpuVersionOffset = 0x220;
constexpr zx_off_t kCpuVersionOffsetA5 = 0x300;
constexpr zx_off_t kCpuVersionOffsetA1 = 0x220;

constexpr uint32_t kCpuGetDvfsTableIndexFuncId = 0x82000088;
constexpr uint64_t kDefaultClusterId = 0;

}  // namespace

zx_status_t AmlCpu::GetPopularVoltageTable(const zx::resource& smc_resource,
                                           uint32_t* metadata_type) {
  if (smc_resource.is_valid()) {
    zx_smc_parameters_t smc_params = {};
    smc_params.func_id = kCpuGetDvfsTableIndexFuncId;
    smc_params.arg1 = kDefaultClusterId;

    zx_smc_result_t smc_result;
    zx_status_t status = zx_smc_call(smc_resource.get(), &smc_params, &smc_result);
    if (status != ZX_OK) {
      zxlogf(ERROR, "zx_smc_call failed: %s", zx_status_get_string(status));
      return status;
    }

    switch (smc_result.arg0) {
      case amlogic_cpu::OppTable1:
        *metadata_type = DEVICE_METADATA_AML_OP_1_POINTS;
        break;
      case amlogic_cpu::OppTable2:
        *metadata_type = DEVICE_METADATA_AML_OP_2_POINTS;
        break;
      case amlogic_cpu::OppTable3:
        *metadata_type = DEVICE_METADATA_AML_OP_3_POINTS;
        break;
      default:
        *metadata_type = DEVICE_METADATA_AML_OP_POINTS;
        break;
    }
    zxlogf(INFO, "Dvfs using table%ld.\n", smc_result.arg0);
  }

  return ZX_OK;
}

zx_status_t AmlCpu::Create(void* context, zx_device_t* parent) {
  zx_status_t st;

  // Get the metadata for the performance domains.
  auto perf_doms = ddk::GetMetadataArray<perf_domain_t>(parent, DEVICE_METADATA_AML_PERF_DOMAINS);
  if (!perf_doms.is_ok()) {
    zxlogf(ERROR, "%s: Failed to get performance domains from board driver, st = %d", __func__,
           perf_doms.error_value());
    return perf_doms.error_value();
  }

  // Map AOBUS registers
  auto pdev = ddk::PDevFidl::FromFragment(parent);
  if (!pdev.is_valid()) {
    zxlogf(ERROR, "Failed to get platform device fragment");
    return ZX_ERR_NO_RESOURCES;
  }
  std::optional<fdf::MmioBuffer> mmio_buffer;
  if ((st = pdev.MapMmio(0, &mmio_buffer)) != ZX_OK) {
    zxlogf(ERROR, "aml-cpu: Failed to map mmio, st = %d", st);
    return st;
  }

  pdev_device_info_t info = {};
  st = pdev.GetDeviceInfo(&info);
  if (st != ZX_OK) {
    zxlogf(ERROR, "Failed to get DeviceInfo: %s", zx_status_get_string(st));
    return st;
  }

  zx::resource smc_resource = {};
  uint32_t metadata_type = DEVICE_METADATA_AML_OP_POINTS;
  size_t fragments_per_pf_domain = kFragmentsPerPfDomain;
  zx_off_t cpu_version_offset = kCpuVersionOffset;
  if (info.pid == PDEV_PID_AMLOGIC_A5) {
    st = pdev.GetSmc(0, &smc_resource);
    if (st != ZX_OK) {
      zxlogf(ERROR, "Failed to get smc: %s", zx_status_get_string(st));
      return st;
    }
    st = GetPopularVoltageTable(smc_resource, &metadata_type);
    if (st != ZX_OK) {
      zxlogf(ERROR, "Failed to get popular voltage table: %s", zx_status_get_string(st));
      return st;
    }
    fragments_per_pf_domain = kFragmentsPerPfDomainA5;
    cpu_version_offset = kCpuVersionOffsetA5;
  } else if (info.pid == PDEV_PID_AMLOGIC_A1) {
    fragments_per_pf_domain = kFragmentsPerPfDomainA1;
    cpu_version_offset = kCpuVersionOffsetA1;
  }

  auto op_points = ddk::GetMetadataArray<operating_point_t>(parent, metadata_type);
  if (!op_points.is_ok()) {
    zxlogf(ERROR, "Failed to get operating point from board driver: %s", op_points.status_string());
    return op_points.error_value();
  }

  const uint32_t cpu_version_packed = mmio_buffer->Read32(cpu_version_offset);

  // Build and publish each performance domain.
  for (const perf_domain_t& perf_domain : perf_doms.value()) {
    fbl::StringBuffer<32> fragment_name;
    ddk::ClockProtocolClient pll_div16_client;
    ddk::ClockProtocolClient cpu_div16_client;
    if (fragments_per_pf_domain == kFragmentsPerPfDomain) {
      fragment_name.AppendPrintf("clock-pll-div16-%02d", perf_domain.id);
      if ((st = ddk::ClockProtocolClient::CreateFromDevice(parent, fragment_name.c_str(),
                                                           &pll_div16_client)) != ZX_OK) {
        zxlogf(ERROR, "%s: Failed to create pll_div_16 clock client, st = %d", __func__, st);
        return st;
      }

      fragment_name.Resize(0);
      fragment_name.AppendPrintf("clock-cpu-div16-%02d", perf_domain.id);
      if ((st = ddk::ClockProtocolClient::CreateFromDevice(parent, fragment_name.c_str(),
                                                           &cpu_div16_client)) != ZX_OK) {
        zxlogf(ERROR, "%s: Failed to create cpu_div_16 clock client, st = %d", __func__, st);
        return st;
      }
    }

    fragment_name.Resize(0);
    fragment_name.AppendPrintf("clock-cpu-scaler-%02d", perf_domain.id);
    ddk::ClockProtocolClient cpu_scaler_client;
    if ((st = ddk::ClockProtocolClient::CreateFromDevice(parent, fragment_name.c_str(),
                                                         &cpu_scaler_client)) != ZX_OK) {
      zxlogf(ERROR, "%s: Failed to create cpu_scaler clock client, st = %d", __func__, st);
      return st;
    }

    // For A1, the CPU power is VDD_CORE, which share with other module.
    // The fixed voltage is 0.8v, we can't adjust it dynamically.
    fidl::ClientEnd<fuchsia_hardware_power::Device> power_client;
    if (info.pid != PDEV_PID_AMLOGIC_A1) {
      fragment_name.Resize(0);
      fragment_name.AppendPrintf("power-%02d", perf_domain.id);
      zx::result client_end_result =
          DdkConnectFragmentFidlProtocol<fuchsia_hardware_power::Service::Device>(
              parent, fragment_name.c_str());
      if (client_end_result.is_error()) {
        zxlogf(ERROR, "%s: Failed to create power client, st = %s", __func__,
               client_end_result.status_string());
        return client_end_result.error_value();
      }

      power_client = std::move(client_end_result.value());
    }

    // Vector of operating points that belong to this power domain.
    std::vector<operating_point_t> pd_op_points;
    std::copy_if(
        op_points->begin(), op_points->end(), std::back_inserter(pd_op_points),
        [&perf_domain](const operating_point_t& op) { return op.pd_id == perf_domain.id; });

    // Order operating points from highest frequency to lowest because Operating Point 0 is the
    // fastest.
    std::sort(pd_op_points.begin(), pd_op_points.end(),
              [](const operating_point_t& a, const operating_point_t& b) {
                // Use voltage as a secondary sorting key.
                if (a.freq_hz == b.freq_hz) {
                  return a.volt_uv > b.volt_uv;
                }
                return a.freq_hz > b.freq_hz;
              });

    const size_t perf_state_count = pd_op_points.size();
    auto perf_states = std::make_unique<device_performance_state_info_t[]>(perf_state_count);
    for (size_t j = 0; j < perf_state_count; j++) {
      perf_states[j].state_id = static_cast<uint8_t>(j);
      perf_states[j].restore_latency = 0;
    }

    auto device =
        std::make_unique<AmlCpu>(parent, std::move(pll_div16_client), std::move(cpu_div16_client),
                                 std::move(cpu_scaler_client), std::move(power_client),
                                 std::move(pd_op_points), perf_domain.core_count);

    st = device->Init();
    if (st != ZX_OK) {
      zxlogf(ERROR, "%s: Failed to initialize device, st = %d", __func__, st);
      return st;
    }

    device->SetCpuInfo(cpu_version_packed);

    st = device->DdkAdd(ddk::DeviceAddArgs(perf_domain.name)
                            .set_flags(DEVICE_ADD_NON_BINDABLE)
                            .set_proto_id(ZX_PROTOCOL_CPU_CTRL)
                            .set_performance_states({perf_states.get(), perf_state_count})
                            .set_inspect_vmo(device->inspector_.DuplicateVmo()));

    if (st != ZX_OK) {
      zxlogf(ERROR, "%s: DdkAdd failed, st = %d", __func__, st);
      return st;
    }

    [[maybe_unused]] auto ptr = device.release();
  }

  return ZX_OK;
}

void AmlCpu::DdkRelease() { delete this; }

zx_status_t AmlCpu::DdkSetPerformanceState(uint32_t requested_state, uint32_t* out_state) {
  if (requested_state >= operating_points_.size()) {
    zxlogf(ERROR, "%s: Requested performance state is out of bounds, state = %u\n", __func__,
           requested_state);
    return ZX_ERR_OUT_OF_RANGE;
  }

  if (!out_state) {
    zxlogf(ERROR, "%s: out_state may not be null", __func__);
    return ZX_ERR_INVALID_ARGS;
  }

  // There is no condition under which this function will return ZX_OK but out_state will not
  // be requested_state so we're going to go ahead and set that up front.
  *out_state = requested_state;

  const operating_point_t& target_state = operating_points_[requested_state];
  const operating_point_t& initial_state = operating_points_[current_pstate_];

  zxlogf(INFO, "%s: Scaling from %u MHz %u mV to %u MHz %u mV", __func__,
         initial_state.freq_hz / 1000000, initial_state.volt_uv / 1000,
         target_state.freq_hz / 1000000, target_state.volt_uv / 1000);

  if (initial_state.freq_hz == target_state.freq_hz &&
      initial_state.volt_uv == target_state.volt_uv) {
    // Nothing to be done.
    return ZX_OK;
  }

  if (target_state.volt_uv > initial_state.volt_uv) {
    // If we're increasing the voltage we need to do it before setting the
    // frequency.
    ZX_ASSERT(pwr_.is_valid());
    fidl::WireResult result = pwr_->RequestVoltage(target_state.volt_uv);
    if (!result.ok()) {
      zxlogf(ERROR, "%s: Failed to send RequestVoltage request: %s", __func__,
             result.status_string());
      return result.error().status();
    }

    if (result->is_error()) {
      zxlogf(ERROR, "%s: RequestVoltage call returned error: %s", __func__,
             zx_status_get_string(result->error_value()));
      return result->error_value();
    }

    uint32_t actual_voltage = result->value()->actual_voltage;
    if (actual_voltage != target_state.volt_uv) {
      zxlogf(ERROR, "%s: Actual voltage does not match, requested = %u, got = %u", __func__,
             target_state.volt_uv, actual_voltage);
      return ZX_OK;
    }
  }

  // Set the frequency next.
  zx_status_t st = cpuscaler_.SetRate(target_state.freq_hz);
  if (st != ZX_OK) {
    zxlogf(ERROR, "%s: Could not set CPU frequency, st = %d\n", __func__, st);

    // Put the voltage back if frequency scaling fails.
    if (pwr_.is_valid()) {
      fidl::WireResult result = pwr_->RequestVoltage(initial_state.volt_uv);
      if (!result.ok()) {
        zxlogf(ERROR, "%s: Failed to send RequestVoltage request: %s", __func__,
               result.status_string());
        return result.error().status();
      }

      if (result->is_error()) {
        zxlogf(ERROR, "%s: Failed to reset CPU voltage, st = %s, Voltage and frequency mismatch!",
               __func__, zx_status_get_string(result->error_value()));
        return result->error_value();
      }
    }
    return st;
  }

  // If we're decreasing the voltage, then we do it after the frequency has been
  // reduced to avoid undervolt conditions.
  if (target_state.volt_uv < initial_state.volt_uv) {
    ZX_ASSERT(pwr_.is_valid());
    fidl::WireResult result = pwr_->RequestVoltage(target_state.volt_uv);
    if (!result.ok()) {
      zxlogf(ERROR, "%s: Failed to send RequestVoltage request: %s", __func__,
             result.status_string());
      return result.error().status();
    }

    if (result->is_error()) {
      zxlogf(ERROR, "%s: RequestVoltage call returned error: %s", __func__,
             zx_status_get_string(result->error_value()));
      return result->error_value();
    }

    uint32_t actual_voltage = result->value()->actual_voltage;
    if (actual_voltage != target_state.volt_uv) {
      zxlogf(ERROR,
             "%s: Failed to set cpu voltage, requested = %u, got = %u. "
             "Voltage and frequency mismatch!",
             __func__, target_state.volt_uv, actual_voltage);
      return ZX_OK;
    }
  }

  zxlogf(INFO, "%s: Success\n", __func__);

  current_pstate_ = requested_state;

  return ZX_OK;
}

zx_status_t AmlCpu::Init() {
  zx_status_t result;
  constexpr uint32_t kInitialPstate = 0;

  if (plldiv16_.is_valid()) {
    result = plldiv16_.Enable();
    if (result != ZX_OK) {
      zxlogf(ERROR, "%s: Failed to enable plldiv16, st = %d", __func__, result);
      return result;
    }
  }

  if (cpudiv16_.is_valid()) {
    result = cpudiv16_.Enable();
    if (result != ZX_OK) {
      zxlogf(ERROR, "%s: Failed to enable cpudiv16_, st = %d", __func__, result);
      return result;
    }
  }

  if (pwr_.is_valid()) {
    fidl::WireResult voltage_range_result = pwr_->GetSupportedVoltageRange();
    if (!voltage_range_result.ok()) {
      zxlogf(ERROR, "Failed to send GetSupportedVoltageRange request: %s",
             voltage_range_result.status_string());
      return voltage_range_result.status();
    }

    if (voltage_range_result->is_error()) {
      zxlogf(ERROR, "GetSupportedVoltageRange returned error: %s",
             zx_status_get_string(voltage_range_result->error_value()));
      return voltage_range_result->error_value();
    }

    uint32_t max_voltage = voltage_range_result->value()->max;
    uint32_t min_voltage = voltage_range_result->value()->min;

    fidl::WireResult register_result = pwr_->RegisterPowerDomain(min_voltage, max_voltage);
    if (!register_result.ok()) {
      zxlogf(ERROR, "Failed to send RegisterPowerDomain request: %s",
             register_result.status_string());
      return voltage_range_result.status();
    }

    if (register_result->is_error()) {
      zxlogf(ERROR, "RegisterPowerDomain returned error: %s",
             zx_status_get_string(register_result->error_value()));
      return register_result->error_value();
    }
  }

  uint32_t actual;
  result = DdkSetPerformanceState(kInitialPstate, &actual);

  if (result != ZX_OK) {
    zxlogf(ERROR, "%s: Failed to set initial performance state, st = %d", __func__, result);
    return result;
  }

  if (actual != kInitialPstate) {
    zxlogf(ERROR, "%s: Failed to set initial performance state, requested = %u, actual = %u",
           __func__, kInitialPstate, actual);
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

zx_status_t AmlCpu::DdkConfigureAutoSuspend(bool enable, uint8_t requested_sleep_state) {
  return ZX_ERR_NOT_SUPPORTED;
}

void AmlCpu::GetPerformanceStateInfo(GetPerformanceStateInfoRequestView request,
                                     GetPerformanceStateInfoCompleter::Sync& completer) {
  if (request->state >= operating_points_.size()) {
    zxlogf(INFO, "%s: Requested an operating point that's out of bounds, %u\n", __func__,
           request->state);
    completer.ReplyError(ZX_ERR_OUT_OF_RANGE);
    return;
  }

  fuchsia_hardware_cpu_ctrl::wire::CpuPerformanceStateInfo result;
  result.frequency_hz = operating_points_[request->state].freq_hz;
  result.voltage_uv = operating_points_[request->state].volt_uv;

  completer.ReplySuccess(result);
}

void AmlCpu::GetNumLogicalCores(GetNumLogicalCoresCompleter::Sync& completer) {
  completer.Reply(core_count_);
}

void AmlCpu::GetLogicalCoreId(GetLogicalCoreIdRequestView request,
                              GetLogicalCoreIdCompleter::Sync& completer) {
  // Placeholder.
  completer.Reply(0);
}

void AmlCpu::SetCpuInfo(uint32_t cpu_version_packed) {
  const uint8_t major_revision = (cpu_version_packed >> 24) & 0xff;
  const uint8_t minor_revision = (cpu_version_packed >> 8) & 0xff;
  const uint8_t cpu_package_id = (cpu_version_packed >> 20) & 0x0f;
  zxlogf(INFO, "major revision number: 0x%x", major_revision);
  zxlogf(INFO, "minor revision number: 0x%x", minor_revision);
  zxlogf(INFO, "cpu package id number: 0x%x", cpu_package_id);

  cpu_info_.CreateUint("cpu_major_revision", major_revision, &inspector_);
  cpu_info_.CreateUint("cpu_minor_revision", minor_revision, &inspector_);
  cpu_info_.CreateUint("cpu_package_id", cpu_package_id, &inspector_);
}

}  // namespace amlogic_cpu

static constexpr zx_driver_ops_t aml_cpu_driver_ops = []() {
  zx_driver_ops_t result = {};
  result.version = DRIVER_OPS_VERSION;
  result.bind = amlogic_cpu::AmlCpu::Create;
  return result;
}();

// clang-format off
ZIRCON_DRIVER(aml_cpu, aml_cpu_driver_ops, "zircon", "0.1");

