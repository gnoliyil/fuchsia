// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_CPU_DRIVERS_AML_CPU_AML_CPU_H_
#define SRC_DEVICES_CPU_DRIVERS_AML_CPU_AML_CPU_H_

#include <fidl/fuchsia.device/cpp/wire.h>
#include <fidl/fuchsia.hardware.cpu.ctrl/cpp/wire.h>
#include <fuchsia/hardware/clock/cpp/banjo.h>
#include <fuchsia/hardware/power/cpp/banjo.h>
#include <lib/inspect/cpp/inspector.h>

#include <vector>

#include <ddktl/device.h>
#include <ddktl/protocol/empty-protocol.h>
#include <fbl/macros.h>
#include <soc/aml-common/aml-cpu-metadata.h>

namespace amlogic_cpu {

namespace fuchsia_cpuctrl = fuchsia_hardware_cpu_ctrl;

class AmlCpu;
using DeviceType = ddk::Device<AmlCpu, ddk::Messageable<fuchsia_cpuctrl::Device>::Mixin,
                               ddk::PerformanceTunable, ddk::AutoSuspendable>;

class AmlCpu : public DeviceType, public ddk::EmptyProtocol<ZX_PROTOCOL_CPU_CTRL> {
 public:
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AmlCpu);
  explicit AmlCpu(zx_device_t* parent, const ddk::ClockProtocolClient& plldiv16,
                  const ddk::ClockProtocolClient& cpudiv16,
                  const ddk::ClockProtocolClient& cpuscaler, const ddk::PowerProtocolClient& pwr,
                  const std::vector<operating_point_t>& operating_points, const uint32_t core_count)
      : DeviceType(parent),
        plldiv16_(plldiv16),
        cpudiv16_(cpudiv16),
        cpuscaler_(cpuscaler),
        pwr_(pwr),
        current_pstate_(operating_points.size() -
                        1)  // Assume the core is running at the slowest clock to begin.
        ,
        operating_points_(operating_points),
        core_count_(core_count) {}

  static zx_status_t Create(void* context, zx_device_t* device);

  static zx_status_t GetPopularVoltageTable(const zx::resource& smc_resource,
                                            uint32_t* metadata_type);

  zx_status_t Init();

  // Implements DDK Device Ops
  void DdkRelease();

  // Implements ddk::PerformanceTunable.
  zx_status_t DdkSetPerformanceState(uint32_t requested_state, uint32_t* out_state);
  zx_status_t DdkConfigureAutoSuspend(bool enable, uint8_t requested_sleep_state);

  // Fidl server interface implementation.
  void GetPerformanceStateInfo(GetPerformanceStateInfoRequestView request,
                               GetPerformanceStateInfoCompleter::Sync& completer) override;
  void GetNumLogicalCores(GetNumLogicalCoresCompleter::Sync& completer) override;
  void GetLogicalCoreId(GetLogicalCoreIdRequestView request,
                        GetLogicalCoreIdCompleter::Sync& completer) override;

  // Set CpuInfo in inspect.
  void SetCpuInfo(uint32_t cpu_version_packed);

 protected:
  inspect::Inspector inspector_;
  inspect::Node cpu_info_ = inspector_.GetRoot().CreateChild("cpu_info_service");

 private:
  const ddk::ClockProtocolClient plldiv16_;
  const ddk::ClockProtocolClient cpudiv16_;
  const ddk::ClockProtocolClient cpuscaler_;
  const ddk::PowerProtocolClient pwr_;

  size_t current_pstate_;
  const std::vector<operating_point_t> operating_points_;

  const uint32_t core_count_;
};

}  // namespace amlogic_cpu

#endif  // SRC_DEVICES_CPU_DRIVERS_AML_CPU_AML_CPU_H_
