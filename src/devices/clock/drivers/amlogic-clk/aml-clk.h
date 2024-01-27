// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_CLOCK_DRIVERS_AMLOGIC_CLK_AML_CLK_H_
#define SRC_DEVICES_CLOCK_DRIVERS_AMLOGIC_CLK_AML_CLK_H_

#include <fidl/fuchsia.hardware.clock/cpp/wire.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fuchsia/hardware/clockimpl/cpp/banjo.h>
#include <fuchsia/hardware/platform/device/c/banjo.h>
#include <lib/ddk/device.h>
#include <lib/ddk/io-buffer.h>
#include <lib/device-protocol/platform-device.h>
#include <lib/mmio/mmio.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <zircon/syscalls/smc.h>

#include <optional>
#include <vector>

#include <ddktl/device.h>
#include <fbl/array.h>
#include <fbl/mutex.h>
#include <hwreg/mmio.h>
#include <soc/aml-a1/a1-hiu.h>
#include <soc/aml-a5/a5-hiu.h>
#include <soc/aml-s905d2/s905d2-hiu.h>

#include "aml-clk-blocks.h"

namespace ddk {
class PBusProtocolClient;
}

namespace amlogic_clock {

class MesonPllClock;
class MesonCpuClock;
class MesonRateClock;

class AmlClock;
using DeviceType =
    ddk::Device<AmlClock, ddk::Unbindable, ddk::Messageable<fuchsia_hardware_clock::Device>::Mixin>;

class AmlClock : public DeviceType, public ddk::ClockImplProtocol<AmlClock, ddk::base_protocol> {
 public:
  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(AmlClock);
  AmlClock(zx_device_t* device, fdf::MmioBuffer hiu_mmio, fdf::MmioBuffer dosbus_mmio,
           std::optional<fdf::MmioBuffer> msr_mmio, std::optional<fdf::MmioBuffer> cpuctrl_mmio,
           uint32_t device_id);
  ~AmlClock();

  // Performs the object initialization.
  static zx_status_t Create(zx_device_t* device);

  // CLK protocol implementation.
  zx_status_t ClockImplEnable(uint32_t clk);
  zx_status_t ClockImplDisable(uint32_t clk);
  zx_status_t ClockImplIsEnabled(uint32_t id, bool* out_enabled);

  zx_status_t ClockImplSetRate(uint32_t id, uint64_t hz);
  zx_status_t ClockImplQuerySupportedRate(uint32_t id, uint64_t max_rate, uint64_t* out_best_rate);
  zx_status_t ClockImplGetRate(uint32_t id, uint64_t* out_current_rate);

  zx_status_t ClockImplSetInput(uint32_t id, uint32_t idx);
  zx_status_t ClockImplGetNumInputs(uint32_t id, uint32_t* out_num_inputs);
  zx_status_t ClockImplGetInput(uint32_t id, uint32_t* out_input);

  // CLK FIDL implementation.
  void Measure(MeasureRequestView request, MeasureCompleter::Sync& completer) override;
  void GetCount(GetCountCompleter::Sync& completer) override;
  void Enable(EnableRequestView request, EnableCompleter::Sync& completer) override;
  void Disable(DisableRequestView request, DisableCompleter::Sync& completer) override;

  // Device protocol implementation.
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();

  void ShutDown();

  void Register(fdf::WireSyncClient<fuchsia_hardware_platform_bus::PlatformBus> pbus);

 protected:
  // Debug API for forcing a clock to be disabled.
  // This method will reset the clock's vote count to 0 and disable the clock
  // hardware.
  // NOTE: Calling this function may put the driver into an undefined state.
  zx_status_t ClkDebugForceDisable(uint32_t clk);

 private:
  // Toggle clocks enable bit.
  zx_status_t ClkToggle(uint32_t clk, bool enable);
  void ClkToggleHw(const meson_clk_gate_t* gate, bool enable) __TA_REQUIRES(lock_);

  // Clock measure helper API.
  zx_status_t ClkMeasureUtil(uint32_t clk, uint64_t* clk_freq);

  // Toggle enable bit for PLL clocks.
  zx_status_t ClkTogglePll(uint32_t clk, const bool enable);

  // Checks the preconditions for SetInput, GetNumInputs and GetInput and
  // returns ZX_OK if the preconditions are met.
  zx_status_t IsSupportedMux(const uint32_t id, const uint16_t supported_mask);

  // Find the MesonRateClock that corresponds to clk. If ZX_OK is returned
  // `out` is populated with a pointer to the target clock.
  zx_status_t GetMesonRateClock(const uint32_t clk, MesonRateClock** out);

  void InitHiu();

  void InitHiuA5();

  void InitHiuA1();

  // IO MMIO
  fdf::MmioBuffer hiu_mmio_;
  fdf::MmioBuffer dosbus_mmio_;
  std::optional<fdf::MmioBuffer> msr_mmio_;
  std::optional<fdf::MmioBuffer> cpuctrl_mmio_;
  // Protects clock gate registers.
  // Clock gates.
  fbl::Mutex lock_;
  const meson_clk_gate_t* gates_ = nullptr;
  size_t gate_count_ = 0;
  std::vector<uint32_t> meson_gate_enable_count_;

  // Clock muxes.
  const meson_clk_mux_t* muxes_ = nullptr;
  size_t mux_count_ = 0;

  // Cpu Clocks.
  std::vector<MesonCpuClock> cpu_clks_;

  aml_hiu_dev_t hiudev_;
  std::vector<MesonPllClock> pllclk_;
  size_t pll_count_ = HIU_PLL_COUNT;

  // Clock Table
  const char* const* clk_table_ = nullptr;
  size_t clk_table_count_ = 0;
  // MSR_CLK offsets/
  meson_clk_msr_t clk_msr_offsets_;
};

}  // namespace amlogic_clock

#endif  // SRC_DEVICES_CLOCK_DRIVERS_AMLOGIC_CLK_AML_CLK_H_
