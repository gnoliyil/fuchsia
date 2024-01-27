// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-clk.h"

#include <fidl/fuchsia.hardware.clock/cpp/wire.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/device-protocol/pdev-fidl.h>
#include <string.h>

#include <fbl/auto_lock.h>
#include <hwreg/bitfields.h>
#include <soc/aml-meson/aml-clk-common.h>

#include "aml-a1-blocks.h"
#include "aml-a5-blocks.h"
#include "aml-axg-blocks.h"
#include "aml-fclk.h"
#include "aml-g12a-blocks.h"
#include "aml-g12b-blocks.h"
#include "aml-gxl-blocks.h"
#include "aml-sm1-blocks.h"
#include "src/devices/clock/drivers/amlogic-clk/aml-clk-bind.h"

namespace amlogic_clock {

// MMIO Indexes
static constexpr uint32_t kHiuMmio = 0;
static constexpr uint32_t kDosbusMmio = 1;
static constexpr uint32_t kMsrMmio = 2;
static constexpr uint32_t kCpuCtrlMmio = 3;

#define MSR_WAIT_BUSY_RETRIES 5
#define MSR_WAIT_BUSY_TIMEOUT_US 10000

class SysCpuClkControl : public hwreg::RegisterBase<SysCpuClkControl, uint32_t> {
 public:
  DEF_BIT(29, busy_cnt);
  DEF_BIT(28, busy);
  DEF_BIT(26, dyn_enable);
  DEF_FIELD(25, 20, mux1_divn_tcnt);
  DEF_BIT(18, postmux1);
  DEF_FIELD(17, 16, premux1);
  DEF_BIT(15, manual_mux_mode);
  DEF_BIT(14, manual_mode_post);
  DEF_BIT(13, manual_mode_pre);
  DEF_BIT(12, force_update_t);
  DEF_BIT(11, final_mux_sel);
  DEF_BIT(10, final_dyn_mux_sel);
  DEF_FIELD(9, 4, mux0_divn_tcnt);
  DEF_BIT(3, rev);
  DEF_BIT(2, postmux0);
  DEF_FIELD(1, 0, premux0);

  static auto Get(uint32_t offset) { return hwreg::RegisterAddr<SysCpuClkControl>(offset); }
};

class MesonRateClock {
 public:
  virtual zx_status_t SetRate(const uint32_t hz) = 0;
  virtual zx_status_t QuerySupportedRate(const uint64_t max_rate, uint64_t* result) = 0;
  virtual zx_status_t GetRate(uint64_t* result) = 0;
  virtual ~MesonRateClock() {}
};

class MesonPllClock : public MesonRateClock {
 public:
  explicit MesonPllClock(const hhi_plls_t pll_num, aml_hiu_dev_t* hiudev)
      : pll_num_(pll_num), hiudev_(hiudev) {}
  explicit MesonPllClock(std::unique_ptr<AmlMesonPllDevice> meson_hiudev)
      : pll_num_(HIU_PLL_COUNT),  // A5 doesn't use it.
        meson_hiudev_(std::move(meson_hiudev)) {}
  explicit MesonPllClock(MesonPllClock&& other)
      : pll_num_(other.pll_num_),  // A5 doesn't use it.
        pll_(other.pll_),
        hiudev_(other.hiudev_),
        meson_hiudev_(std::move(other.meson_hiudev_)) {}
  ~MesonPllClock() = default;

  void Init();

  // Implement MesonRateClock
  zx_status_t SetRate(const uint32_t hz) final override;
  zx_status_t QuerySupportedRate(const uint64_t max_rate, uint64_t* result) final override;
  zx_status_t GetRate(uint64_t* result) final override;

  zx_status_t Toggle(const bool enable);

 private:
  const hhi_plls_t pll_num_;
  aml_pll_dev_t pll_;
  aml_hiu_dev_t* hiudev_;
  std::unique_ptr<AmlMesonPllDevice> meson_hiudev_;
};

void MesonPllClock::Init() {
  const hhi_pll_rate_t* rate_table = nullptr;
  size_t rate_table_size = 0;

  if (meson_hiudev_) {
    rate_table = meson_hiudev_->GetRateTable();
    rate_table_size = meson_hiudev_->GetRateTableSize();
  } else {
    s905d2_pll_init_etc(hiudev_, &pll_, pll_num_);

    rate_table = s905d2_pll_get_rate_table(pll_num_);
    rate_table_size = s905d2_get_rate_table_count(pll_num_);
  }

  // Make sure that the rate table is sorted in strictly ascending order.
  for (size_t i = 0; i < rate_table_size - 1; i++) {
    ZX_ASSERT(rate_table[i].rate < rate_table[i + 1].rate);
  }
}

zx_status_t MesonPllClock::SetRate(const uint32_t hz) {
  if (meson_hiudev_) {
    return meson_hiudev_->SetRate(hz);
  }

  return s905d2_pll_set_rate(&pll_, hz);
}

zx_status_t MesonPllClock::QuerySupportedRate(const uint64_t max_rate, uint64_t* result) {
  // Find the largest rate that does not exceed `max_rate`

  // Start by getting the rate tables.
  const hhi_pll_rate_t* rate_table = nullptr;
  size_t rate_table_size = 0;
  const hhi_pll_rate_t* best_rate = nullptr;

  if (meson_hiudev_) {
    rate_table = meson_hiudev_->GetRateTable();
    rate_table_size = meson_hiudev_->GetRateTableSize();
  } else {
    rate_table = s905d2_pll_get_rate_table(pll_num_);
    rate_table_size = s905d2_get_rate_table_count(pll_num_);
  }

  // The rate table is already sorted in ascending order so pick the largest
  // element that does not exceed max_rate.
  for (size_t i = 0; i < rate_table_size; i++) {
    if (rate_table[i].rate <= max_rate) {
      best_rate = &rate_table[i];
    } else {
      break;
    }
  }

  if (best_rate == nullptr) {
    return ZX_ERR_NOT_FOUND;
  }

  *result = best_rate->rate;
  return ZX_OK;
}

zx_status_t MesonPllClock::GetRate(uint64_t* result) { return ZX_ERR_NOT_SUPPORTED; }

zx_status_t MesonPllClock::Toggle(const bool enable) {
  if (enable) {
    if (meson_hiudev_) {
      return meson_hiudev_->Enable();
    } else {
      return s905d2_pll_ena(&pll_);
    }
  } else {
    if (meson_hiudev_) {
      meson_hiudev_->Disable();
    } else {
      s905d2_pll_disable(&pll_);
    }
    return ZX_OK;
  }
}

class MesonCpuClock : public MesonRateClock {
 public:
  explicit MesonCpuClock(const fdf::MmioBuffer* hiu, const uint32_t offset, MesonPllClock* sys_pll,
                         const uint32_t initial_rate)
      : hiu_(hiu), offset_(offset), sys_pll_(sys_pll), current_rate_hz_(initial_rate) {}
  explicit MesonCpuClock(const fdf::MmioBuffer* hiu, const uint32_t offset, MesonPllClock* sys_pll,
                         const uint32_t initial_rate, const uint32_t chip_id)
      : hiu_(hiu),
        offset_(offset),
        sys_pll_(sys_pll),
        current_rate_hz_(initial_rate),
        chip_id_(chip_id) {}
  explicit MesonCpuClock(const fdf::MmioBuffer* hiu, const uint32_t offset, MesonPllClock* sys_pll,
                         const uint32_t initial_rate, const uint32_t chip_id,
                         zx::resource smc_resource)
      : hiu_(hiu),
        offset_(offset),
        sys_pll_(sys_pll),
        current_rate_hz_(initial_rate),
        chip_id_(chip_id),
        smc_(std::move(smc_resource)) {}
  explicit MesonCpuClock(MesonCpuClock&& other)
      : hiu_(other.hiu_),
        offset_(other.offset_),
        sys_pll_(other.sys_pll_),
        current_rate_hz_(other.current_rate_hz_),
        chip_id_(other.chip_id_),
        smc_(std::move(other.smc_)) {}
  ~MesonCpuClock() = default;

  // Implement MesonRateClock
  zx_status_t SetRate(const uint32_t hz) final override;
  zx_status_t QuerySupportedRate(const uint64_t max_rate, uint64_t* result) final override;
  zx_status_t GetRate(uint64_t* result) final override;

 private:
  zx_status_t ConfigCpuFixedPll(const uint32_t new_rate);
  zx_status_t ConfigureSysPLL(uint32_t new_rate);
  zx_status_t WaitForBusyCpu();
  zx_status_t SecSetClk(uint32_t func_id, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                        uint64_t arg4, uint64_t arg5, uint64_t arg6);
  zx_status_t SetRateA5(const uint32_t hz);
  zx_status_t SecSetCpuClkMux(uint64_t clock_source);
  zx_status_t SecSetSys0DcoPll(const pll_params_table& pll_params);
  zx_status_t SecSetCpuClkDyn(const cpu_dyn_table& dyn_params);
  zx_status_t SetRateA1(const uint32_t hz);

  static constexpr uint32_t kFrequencyThresholdHz = 1'000'000'000;
  // Final Mux for selecting clock source.
  static constexpr uint32_t kFixedPll = 0;
  static constexpr uint32_t kSysPll = 1;

  static constexpr uint32_t kSysCpuWaitBusyRetries = 5;
  static constexpr uint32_t kSysCpuWaitBusyTimeoutUs = 10'000;

  const fdf::MmioBuffer* hiu_;
  const uint32_t offset_;

  MesonPllClock* sys_pll_;

  uint32_t current_rate_hz_;
  uint32_t chip_id_ = 0;
  zx::resource smc_;
};

zx_status_t MesonCpuClock::SecSetClk(uint32_t func_id, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                     uint64_t arg4, uint64_t arg5, uint64_t arg6) {
  zx_status_t status;

  zx_smc_parameters_t smc_params = {
      .func_id = func_id,
      .arg1 = arg1,
      .arg2 = arg2,
      .arg3 = arg3,
      .arg4 = arg4,
      .arg5 = arg5,
      .arg6 = arg6,
  };

  zx_smc_result_t smc_result;
  status = zx_smc_call(smc_.get(), &smc_params, &smc_result);
  if (status != ZX_OK) {
    zxlogf(ERROR, "zx_smc_call failed: %s", zx_status_get_string(status));
  }

  return status;
}

zx_status_t MesonCpuClock::SecSetCpuClkMux(uint64_t clock_source) {
  zx_status_t status = ZX_OK;

  status = SecSetClk(kSecureCpuClk, static_cast<uint64_t>(SecPll::kSecidCpuClkSel),
                     kFinalMuxSelMask, clock_source, 0, 0, 0);
  if (status != ZX_OK) {
    zxlogf(ERROR, "kSecidCpuClkSel failed: %s", zx_status_get_string(status));
  }
  return status;
}

zx_status_t MesonCpuClock::SecSetSys0DcoPll(const pll_params_table& pll_params) {
  zx_status_t status = ZX_OK;

  status = SecSetClk(kSecurePllClk, static_cast<uint64_t>(SecPll::kSecidSys0DcoPll), pll_params.m,
                     pll_params.n, pll_params.od, 0, 0);
  if (status != ZX_OK) {
    zxlogf(ERROR, "kSecidSys0DcoPll failed: %s", zx_status_get_string(status));
  }
  return status;
}

zx_status_t MesonCpuClock::SecSetCpuClkDyn(const cpu_dyn_table& dyn_params) {
  zx_status_t status = ZX_OK;

  status = SecSetClk(kSecureCpuClk, static_cast<uint64_t>(SecPll::kSecidCpuClkDyn),
                     dyn_params.dyn_pre_mux, dyn_params.dyn_post_mux, dyn_params.dyn_div, 0, 0);
  if (status != ZX_OK) {
    zxlogf(ERROR, "kSecidCpuClkDyn failed: %s", zx_status_get_string(status));
  }
  return status;
}

zx_status_t MesonCpuClock::SetRateA5(const uint32_t hz) {
  zx_status_t status;

  // CPU clock tree: sys_pll(high clock source), final_dyn_mux(low clock source)
  //
  // cts_osc_clk ->|->premux0->|->mux0_divn->|->postmux0->|
  // fclk_div2   ->|           |  -------->  |            |->final_dyn_mux->|
  // fclk_div3   ->|->premux1->|->mux1_divn->|->postmux1->|                 |
  // fclk_div2p5 ->|           |  -------->  |            |                 |->final_mux->cpu_clk
  //                                                                        |
  // sys_pll     ->|            -------------------->                       |
  //
  if (hz > kFrequencyThresholdHz) {
    auto rate = std::find_if(std::begin(a5_sys_pll_params_table), std::end(a5_sys_pll_params_table),
                             [hz](const pll_params_table& a) { return a.rate == hz; });
    if (rate == std::end(a5_sys_pll_params_table)) {
      zxlogf(ERROR, "Invalid cpu freq");
      return ZX_ERR_NOT_SUPPORTED;
    }

    // Switch to low freq source(cpu_dyn)
    status = SecSetCpuClkMux(kFinalMuxSelCpuDyn);
    if (status != ZX_OK) {
      zxlogf(ERROR, "SecSetCpuClkMux failed: %s", zx_status_get_string(status));
      return status;
    }

    // Set clock by sys_pll
    status = SecSetSys0DcoPll(*rate);
    if (status != ZX_OK) {
      zxlogf(ERROR, "SecSetSys0DcoPll failed: %s", zx_status_get_string(status));
      return status;
    }

    // Switch to high freq source(sys_pll)
    status = SecSetCpuClkMux(kFinalMuxSelSysPll);
    if (status != ZX_OK) {
      zxlogf(ERROR, "SecSetCpuClkMux failed: %s", zx_status_get_string(status));
    }
  } else {
    auto rate = std::find_if(std::begin(a5_cpu_dyn_table), std::end(a5_cpu_dyn_table),
                             [hz](const cpu_dyn_table& a) { return a.rate == hz; });
    if (rate == std::end(a5_cpu_dyn_table)) {
      zxlogf(ERROR, "Invalid cpu freq");
      return ZX_ERR_NOT_SUPPORTED;
    }

    // Set clock by cpu_dyn
    status = SecSetCpuClkDyn(*rate);
    if (status != ZX_OK) {
      zxlogf(ERROR, "SecSetCpuClkDyn failed: %s", zx_status_get_string(status));
      return status;
    }

    // Switch to low freq source(cpu_dyn)
    status = SecSetCpuClkMux(kFinalMuxSelCpuDyn);
    if (status != ZX_OK) {
      zxlogf(ERROR, "SecSetCpuClkMux failed: %s", zx_status_get_string(status));
    }
  }

  return status;
}

zx_status_t MesonCpuClock::SetRateA1(const uint32_t hz) {
  zx_status_t status;

  if (hz > kFrequencyThresholdHz) {
    // switch to low freq source
    auto sys_cpu_ctrl0 = SysCpuClkControl::Get(offset_).ReadFrom(&*hiu_);
    sys_cpu_ctrl0.set_final_mux_sel(kFixedPll).WriteTo(&*hiu_);

    status = ConfigureSysPLL(hz);
  } else {
    status = ConfigCpuFixedPll(hz);
  }

  return status;
}

zx_status_t MesonCpuClock::SetRate(const uint32_t hz) {
  zx_status_t status;

  if (chip_id_ == PDEV_PID_AMLOGIC_A5) {
    status = SetRateA5(hz);
  } else if (chip_id_ == PDEV_PID_AMLOGIC_A1) {
    status = SetRateA1(hz);
  } else {
    if (hz > kFrequencyThresholdHz && current_rate_hz_ > kFrequencyThresholdHz) {
      // Switching between two frequencies both higher than 1GHz.
      // In this case, as per the datasheet it is recommended to change
      // to a frequency lower than 1GHz first and then switch to higher
      // frequency to avoid glitches.

      // Let's first switch to 1GHz
      status = SetRate(kFrequencyThresholdHz);
      if (status != ZX_OK) {
        zxlogf(ERROR, "%s: failed to set CPU freq to intermediate freq, status = %d", __func__,
               status);
        return status;
      }

      // Now let's set SYS_PLL rate to hz.
      status = ConfigureSysPLL(hz);

    } else if (hz > kFrequencyThresholdHz && current_rate_hz_ <= kFrequencyThresholdHz) {
      // Switching from a frequency lower than 1GHz to one greater than 1GHz.
      // In this case we just need to set the SYS_PLL to required rate and
      // then set the final mux to 1 (to select SYS_PLL as the source.)

      // Now let's set SYS_PLL rate to hz.
      status = ConfigureSysPLL(hz);

    } else {
      // Switching between two frequencies below 1GHz.
      // In this case we change the source and dividers accordingly
      // to get the required rate from MPLL and do not touch the
      // final mux.
      status = ConfigCpuFixedPll(hz);
    }
  }

  if (status == ZX_OK) {
    current_rate_hz_ = hz;
  } else {
    zxlogf(ERROR, "%s: Failed to set rate, st = %d", __func__, status);
  }

  return status;
}

zx_status_t MesonCpuClock::ConfigureSysPLL(uint32_t new_rate) {
  // This API also validates if the new_rate is valid.
  // So no need to validate it here.
  zx_status_t status = sys_pll_->SetRate(new_rate);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to set SYS_PLL rate, status = %d", __func__, status);
    return status;
  }

  // Now we need to change the final mux to select input as SYS_PLL.
  status = WaitForBusyCpu();
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to wait for busy, status = %d", __func__, status);
    return status;
  }

  // Select the final mux.
  auto sys_cpu_ctrl0 = SysCpuClkControl::Get(offset_).ReadFrom(&*hiu_);
  sys_cpu_ctrl0.set_final_mux_sel(kSysPll).WriteTo(&*hiu_);

  return status;
}

zx_status_t MesonCpuClock::QuerySupportedRate(const uint64_t max_rate, uint64_t* result) {
  // Cpu Clock supported rates fall into two categories based on whether they're below
  // or above the 1GHz threshold. This method scans both the syspll and the fclk to
  // determine the maximum rate that does not exceed `max_rate`.
  uint64_t syspll_rate = 0;
  uint64_t fclk_rate = 0;
  zx_status_t syspll_status = ZX_ERR_NOT_FOUND;
  zx_status_t fclk_status = ZX_ERR_NOT_FOUND;

  if (chip_id_ == PDEV_PID_AMLOGIC_A5) {
    for (size_t i = 0; i < std::size(a5_cpu_dyn_table); i++) {
      if (a5_cpu_dyn_table[i].rate > fclk_rate && a5_cpu_dyn_table[i].rate <= max_rate) {
        fclk_rate = a5_cpu_dyn_table[i].rate;
        fclk_status = ZX_OK;
      }
    }
    for (size_t i = 0; i < std::size(a5_sys_pll_params_table); i++) {
      if (a5_sys_pll_params_table[i].rate > fclk_rate &&
          a5_sys_pll_params_table[i].rate <= max_rate) {
        syspll_rate = a5_sys_pll_params_table[i].rate;
        syspll_status = ZX_OK;
      }
    }
  } else {
    syspll_status = sys_pll_->QuerySupportedRate(max_rate, &syspll_rate);

    const aml_fclk_rate_table_t* fclk_rate_table = s905d2_fclk_get_rate_table();
    size_t rate_count = s905d2_fclk_get_rate_table_count();

    for (size_t i = 0; i < rate_count; i++) {
      if (fclk_rate_table[i].rate > fclk_rate && fclk_rate_table[i].rate <= max_rate) {
        fclk_rate = fclk_rate_table[i].rate;
        fclk_status = ZX_OK;
      }
    }
  }

  // 4 cases: rate supported by syspll only, rate supported by fclk only
  //          rate supported by neither or rate supported by both.
  if (syspll_status == ZX_OK && fclk_status != ZX_OK) {
    // Case 1
    *result = syspll_rate;
    return ZX_OK;
  } else if (syspll_status != ZX_OK && fclk_status == ZX_OK) {
    // Case 2
    *result = fclk_rate;
    return ZX_OK;
  } else if (syspll_status != ZX_OK && fclk_status != ZX_OK) {
    // Case 3
    return ZX_ERR_NOT_FOUND;
  }

  // Case 4
  if (syspll_rate > kFrequencyThresholdHz) {
    *result = syspll_rate;
  } else {
    *result = fclk_rate;
  }
  return ZX_OK;
}

zx_status_t MesonCpuClock::GetRate(uint64_t* result) {
  if (result == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }

  *result = current_rate_hz_;
  return ZX_OK;
}

// NOTE: This block doesn't modify the MPLL, it just programs the muxes &
// dividers to get the new_rate in the sys_pll_div block. Refer fig. 6.6 Multi
// Phase PLLS for A53 & A73 in the datasheet.
zx_status_t MesonCpuClock::ConfigCpuFixedPll(const uint32_t new_rate) {
  const aml_fclk_rate_table_t* fclk_rate_table;
  size_t rate_count;
  size_t i;

  if (chip_id_ == PDEV_PID_AMLOGIC_A1) {
    fclk_rate_table = a1_fclk_get_rate_table();
    rate_count = a1_fclk_get_rate_table_count();
  } else {
    fclk_rate_table = s905d2_fclk_get_rate_table();
    rate_count = s905d2_fclk_get_rate_table_count();
  }
  // Validate if the new_rate is available
  for (i = 0; i < rate_count; i++) {
    if (new_rate == fclk_rate_table[i].rate) {
      break;
    }
  }
  if (i == rate_count) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t status = WaitForBusyCpu();
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to wait for busy, status = %d", __func__, status);
    return status;
  }

  auto sys_cpu_ctrl0 = SysCpuClkControl::Get(offset_).ReadFrom(&*hiu_);

  if (sys_cpu_ctrl0.final_dyn_mux_sel()) {
    // Dynamic mux 1 is in use, we setup dynamic mux 0
    sys_cpu_ctrl0.set_final_dyn_mux_sel(0)
        .set_mux0_divn_tcnt(fclk_rate_table[i].mux_div)
        .set_postmux0(fclk_rate_table[i].postmux)
        .set_premux0(fclk_rate_table[i].premux);
  } else {
    // Dynamic mux 0 is in use, we setup dynamic mux 1
    sys_cpu_ctrl0.set_final_dyn_mux_sel(1)
        .set_mux1_divn_tcnt(fclk_rate_table[i].mux_div)
        .set_postmux1(fclk_rate_table[i].postmux)
        .set_premux1(fclk_rate_table[i].premux);
  }

  // Select the final mux.
  sys_cpu_ctrl0.set_final_mux_sel(kFixedPll).WriteTo(&*hiu_);

  return ZX_OK;
}

zx_status_t MesonCpuClock::WaitForBusyCpu() {
  auto sys_cpu_ctrl0 = SysCpuClkControl::Get(offset_).ReadFrom(&*hiu_);

  // Wait till we are not busy.
  for (uint32_t i = 0; i < kSysCpuWaitBusyRetries; i++) {
    sys_cpu_ctrl0 = SysCpuClkControl::Get(offset_).ReadFrom(&*hiu_);

    if (sys_cpu_ctrl0.busy()) {
      // Wait a little bit before trying again.
      zx_nanosleep(zx_deadline_after(ZX_USEC(kSysCpuWaitBusyTimeoutUs)));
      continue;
    } else {
      return ZX_OK;
    }
  }
  return ZX_ERR_TIMED_OUT;
}

AmlClock::~AmlClock() = default;

AmlClock::AmlClock(zx_device_t* device, fdf::MmioBuffer hiu_mmio, fdf::MmioBuffer dosbus_mmio,
                   std::optional<fdf::MmioBuffer> msr_mmio,
                   std::optional<fdf::MmioBuffer> cpuctrl_mmio, uint32_t device_id)
    : DeviceType(device),
      hiu_mmio_(std::move(hiu_mmio)),
      dosbus_mmio_(std::move(dosbus_mmio)),
      msr_mmio_(std::move(msr_mmio)),
      cpuctrl_mmio_(std::move(cpuctrl_mmio)) {
  // Populate the correct register blocks.
  switch (device_id) {
    case PDEV_DID_AMLOGIC_AXG_CLK: {
      // Gauss
      gates_ = axg_clk_gates;
      gate_count_ = std::size(axg_clk_gates);
      meson_gate_enable_count_.resize(gate_count_);
      break;
    }
    case PDEV_DID_AMLOGIC_GXL_CLK: {
      gates_ = gxl_clk_gates;
      gate_count_ = std::size(gxl_clk_gates);
      meson_gate_enable_count_.resize(gate_count_);
      break;
    }
    case PDEV_DID_AMLOGIC_G12A_CLK: {
      // Astro
      clk_msr_offsets_ = g12a_clk_msr;

      clk_table_ = static_cast<const char* const*>(g12a_clk_table);
      clk_table_count_ = std::size(g12a_clk_table);

      gates_ = g12a_clk_gates;
      gate_count_ = std::size(g12a_clk_gates);
      meson_gate_enable_count_.resize(gate_count_);

      InitHiu();

      constexpr size_t cpu_clk_count = std::size(g12a_cpu_clks);
      cpu_clks_.reserve(cpu_clk_count);
      for (size_t i = 0; i < cpu_clk_count; i++) {
        cpu_clks_.emplace_back(&hiu_mmio_, g12a_cpu_clks[i].reg, &pllclk_[g12a_cpu_clks[i].pll],
                               g12a_cpu_clks[i].initial_hz);
      }

      break;
    }
    case PDEV_DID_AMLOGIC_G12B_CLK: {
      // Sherlock
      clk_msr_offsets_ = g12b_clk_msr;

      clk_table_ = static_cast<const char* const*>(g12b_clk_table);
      clk_table_count_ = std::size(g12b_clk_table);

      gates_ = g12b_clk_gates;
      gate_count_ = std::size(g12b_clk_gates);
      meson_gate_enable_count_.resize(gate_count_);

      InitHiu();

      constexpr size_t cpu_clk_count = std::size(g12b_cpu_clks);
      cpu_clks_.reserve(cpu_clk_count);
      for (size_t i = 0; i < cpu_clk_count; i++) {
        cpu_clks_.emplace_back(&hiu_mmio_, g12b_cpu_clks[i].reg, &pllclk_[g12b_cpu_clks[i].pll],
                               g12b_cpu_clks[i].initial_hz);
      }

      break;
    }
    case PDEV_DID_AMLOGIC_SM1_CLK: {
      // Nelson
      clk_msr_offsets_ = sm1_clk_msr;

      clk_table_ = static_cast<const char* const*>(sm1_clk_table);
      clk_table_count_ = std::size(sm1_clk_table);

      gates_ = sm1_clk_gates;
      gate_count_ = std::size(sm1_clk_gates);
      meson_gate_enable_count_.resize(gate_count_);

      muxes_ = sm1_muxes;
      mux_count_ = std::size(sm1_muxes);

      InitHiu();

      break;
    }
    case PDEV_DID_AMLOGIC_A5_CLK: {
      // AV400
      uint32_t chip_id = PDEV_PID_AMLOGIC_A5;
      zx::resource smc_resource;

      ddk::PDev pdev(device);
      if ((pdev.GetSmc(0, &smc_resource)) != ZX_OK) {
        zxlogf(ERROR, "pdev.GetSmc failed");
        return;
      }

      clk_msr_offsets_ = a5_clk_msr;

      clk_table_ = static_cast<const char* const*>(a5_clk_table);
      clk_table_count_ = std::size(a5_clk_table);

      gates_ = a5_clk_gates;
      gate_count_ = std::size(a5_clk_gates);
      meson_gate_enable_count_.resize(gate_count_);

      muxes_ = a5_muxes;
      mux_count_ = std::size(a5_muxes);

      pll_count_ = a5::PLL_COUNT;
      InitHiuA5();

      constexpr size_t cpu_clk_count = std::size(a5_cpu_clks);
      cpu_clks_.reserve(cpu_clk_count);
      // For A5, there is only 1 CPU clock
      cpu_clks_.emplace_back(&hiu_mmio_, a5_cpu_clks[0].reg, &pllclk_[a5_cpu_clks[0].pll],
                             a5_cpu_clks[0].initial_hz, chip_id, std::move(smc_resource));

      break;
    }
    case PDEV_DID_AMLOGIC_A1_CLK: {
      // clover
      uint32_t chip_id = PDEV_PID_AMLOGIC_A1;
      clk_msr_offsets_ = a1_clk_msr;

      clk_table_ = static_cast<const char* const*>(a1_clk_table);
      clk_table_count_ = std::size(a1_clk_table);

      gates_ = a1_clk_gates;
      gate_count_ = std::size(a1_clk_gates);
      meson_gate_enable_count_.resize(gate_count_);

      muxes_ = a1_muxes;
      mux_count_ = std::size(a1_muxes);

      pll_count_ = a1::PLL_COUNT;
      InitHiuA1();

      constexpr size_t cpu_clk_count = std::size(a1_cpu_clks);
      cpu_clks_.reserve(cpu_clk_count);
      // For A1, there is only 1 CPU clock
      cpu_clks_.emplace_back(&cpuctrl_mmio_.value(), a1_cpu_clks[0].reg,
                             &pllclk_[a1_cpu_clks[0].pll], a1_cpu_clks[0].initial_hz, chip_id);

      break;
    }
    default:
      ZX_PANIC("aml-clk: Unsupported SOC DID %u", device_id);
  }
}

zx_status_t AmlClock::Create(zx_device_t* parent) {
  zx_status_t status;

  // Get the platform device protocol and try to map all the MMIO regions.
  ddk::PDev pdev(parent);
  if (!pdev.is_valid()) {
    zxlogf(ERROR, "aml-clk: failed to get pdev protocol");
    return ZX_ERR_NO_RESOURCES;
  }

  std::optional<fdf::MmioBuffer> hiu_mmio = std::nullopt;
  std::optional<fdf::MmioBuffer> dosbus_mmio = std::nullopt;
  std::optional<fdf::MmioBuffer> msr_mmio = std::nullopt;
  std::optional<fdf::MmioBuffer> cpuctrl_mmio = std::nullopt;

  // All AML clocks have HIU and dosbus regs but only some support MSR regs.
  // Figure out which of the varieties we're dealing with.
  status = pdev.MapMmio(kHiuMmio, &hiu_mmio);
  if (status != ZX_OK || !hiu_mmio) {
    zxlogf(ERROR, "aml-clk: failed to map HIU regs, status = %d", status);
    return status;
  }

  status = pdev.MapMmio(kDosbusMmio, &dosbus_mmio);
  if (status != ZX_OK || !dosbus_mmio) {
    zxlogf(ERROR, "aml-clk: failed to map DOS regs: %s", zx_status_get_string(status));
    return status;
  }

  // Use the Pdev Device Info to determine if we've been provided with two
  // MMIO regions.
  pdev_device_info_t info;
  status = pdev.GetDeviceInfo(&info);
  if (status != ZX_OK) {
    zxlogf(ERROR, "aml-clk: failed to get pdev device info, status = %d", status);
    return status;
  }

  if (info.mmio_count > kMsrMmio) {
    status = pdev.MapMmio(kMsrMmio, &msr_mmio);
    if (status != ZX_OK) {
      zxlogf(ERROR, "aml-clk: failed to map MSR regs, status = %d", status);
      return status;
    }
  }

  // For A1, this register is within cpuctrl mmio
  if (info.pid == PDEV_PID_AMLOGIC_A1 && info.mmio_count > kCpuCtrlMmio) {
    status = pdev.MapMmio(kCpuCtrlMmio, &cpuctrl_mmio);
    if (status != ZX_OK) {
      zxlogf(ERROR, "aml-clk: failed to map cpuctrl regs, status = %d", status);
      return status;
    }
  }

  auto endpoints = fdf::CreateEndpoints<fuchsia_hardware_platform_bus::PlatformBus>();
  if (endpoints.is_error()) {
    zxlogf(ERROR, "Create endpoints failed: %s", endpoints.status_string());
    return endpoints.error_value();
  }

  bool has_pbus = true;

  status = device_connect_runtime_protocol(
      parent, fuchsia_hardware_platform_bus::Service::PlatformBus::ServiceName,
      fuchsia_hardware_platform_bus::Service::PlatformBus::Name,
      endpoints->server.TakeHandle().release());
  if (status != ZX_OK) {
    zxlogf(WARNING, "Failed to connect to platform bus: %s", zx_status_get_string(status));
    has_pbus = false;
  }

  auto clock_device = std::make_unique<amlogic_clock::AmlClock>(
      parent, std::move(*hiu_mmio), *std::move(dosbus_mmio), *std::move(msr_mmio),
      *std::move(cpuctrl_mmio), info.did);

  status = clock_device->DdkAdd(
      ddk::DeviceAddArgs("clocks").forward_metadata(parent, DEVICE_METADATA_CLOCK_IDS));
  if (status != ZX_OK) {
    zxlogf(ERROR, "aml-clk: Could not create clock device: %d", status);
    return status;
  }

  if (has_pbus) {
    clock_device->Register(fdf::WireSyncClient<fuchsia_hardware_platform_bus::PlatformBus>(
        std::move(endpoints->client)));
  }

  // devmgr is now in charge of the memory for dev.
  [[maybe_unused]] auto ptr = clock_device.release();
  return ZX_OK;
}

zx_status_t AmlClock::ClkTogglePll(uint32_t clk, const bool enable) {
  if (clk >= pll_count_) {
    return ZX_ERR_INVALID_ARGS;
  }

  return pllclk_[clk].Toggle(enable);
}

zx_status_t AmlClock::ClkToggle(uint32_t clk, bool enable) {
  if (clk >= gate_count_) {
    return ZX_ERR_INVALID_ARGS;
  }

  const meson_clk_gate_t* gate = &(gates_[clk]);

  fbl::AutoLock al(&lock_);

  uint32_t enable_count = meson_gate_enable_count_[clk];

  // For the sake of catching bugs, disabling a clock that has never
  // been enabled is a bug.
  ZX_ASSERT_MSG((enable == true || enable_count > 0),
                "Cannot disable already disabled clock. clk = %u", clk);

  // Update the refcounts.
  if (enable) {
    meson_gate_enable_count_[clk]++;
  } else {
    ZX_ASSERT(enable_count > 0);
    meson_gate_enable_count_[clk]--;
  }

  if (enable && meson_gate_enable_count_[clk] == 1) {
    // Transition from 0 refs to 1.
    ClkToggleHw(gate, true);
  }

  if (enable == false && meson_gate_enable_count_[clk] == 0) {
    // Transition from 1 ref to 0.
    ClkToggleHw(gate, false);
  }

  return ZX_OK;
}

void AmlClock::ClkToggleHw(const meson_clk_gate_t* gate, bool enable) {
  uint32_t mask = gate->mask ? gate->mask : (1 << gate->bit);
  fdf::MmioBuffer* mmio;
  switch (gate->register_set) {
    case kMesonRegisterSetHiu:
      mmio = &hiu_mmio_;
      break;
    case kMesonRegisterSetDos:
      mmio = &dosbus_mmio_;
      break;
    default:
      ZX_ASSERT(false);
  }

  if (enable) {
    mmio->SetBits32(mask, gate->reg);
  } else {
    mmio->ClearBits32(mask, gate->reg);
  }
}

zx_status_t AmlClock::ClkDebugForceDisable(uint32_t clk) {
  zxlogf(WARNING, "Force Disable Clock %u, note this may put the driver into an undefined state!",
         clk);

  if (clk >= gate_count_) {
    return ZX_ERR_INVALID_ARGS;
  }

  const meson_clk_gate_t* gate = &(gates_[clk]);

  fbl::AutoLock al(&lock_);

  meson_gate_enable_count_[clk] = 0;  // Set clock vote count to 0.

  ClkToggleHw(gate, false);  // Disable the clock hardware.

  return ZX_OK;
}

zx_status_t AmlClock::ClockImplEnable(uint32_t clk) {
  // Determine which clock type we're trying to control.
  aml_clk_common::aml_clk_type type = aml_clk_common::AmlClkType(clk);
  const uint16_t clkid = aml_clk_common::AmlClkIndex(clk);

  switch (type) {
    case aml_clk_common::aml_clk_type::kMesonGate:
      return ClkToggle(clkid, true);
    case aml_clk_common::aml_clk_type::kMesonPll:
      return ClkTogglePll(clkid, true);
    default:
      // Not a supported clock type?
      return ZX_ERR_NOT_SUPPORTED;
  }
}

zx_status_t AmlClock::ClockImplDisable(uint32_t clk) {
  // Determine which clock type we're trying to control.
  aml_clk_common::aml_clk_type type = aml_clk_common::AmlClkType(clk);
  const uint16_t clkid = aml_clk_common::AmlClkIndex(clk);

  switch (type) {
    case aml_clk_common::aml_clk_type::kMesonGate:
      return ClkToggle(clkid, false);
    case aml_clk_common::aml_clk_type::kMesonPll:
      return ClkTogglePll(clkid, false);
    default:
      // Not a supported clock type?
      return ZX_ERR_NOT_SUPPORTED;
  };
}

zx_status_t AmlClock::ClockImplIsEnabled(uint32_t id, bool* out_enabled) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t AmlClock::ClockImplSetRate(uint32_t clk, uint64_t hz) {
  zxlogf(TRACE, "%s: clk = %u, hz = %lu", __func__, clk, hz);

  if (hz >= UINT32_MAX) {
    zxlogf(ERROR, "%s: requested rate exceeds uint32_max, clk = %u, rate = %lu", __func__, clk, hz);
    return ZX_ERR_INVALID_ARGS;
  }

  MesonRateClock* target_clock;
  zx_status_t st = GetMesonRateClock(clk, &target_clock);
  if (st != ZX_OK) {
    return st;
  }

  return target_clock->SetRate(static_cast<uint32_t>(hz));
}

zx_status_t AmlClock::ClockImplQuerySupportedRate(uint32_t clk, uint64_t max_rate,
                                                  uint64_t* out_best_rate) {
  zxlogf(TRACE, "%s: clk = %u, max_rate = %lu", __func__, clk, max_rate);

  if (out_best_rate == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }

  MesonRateClock* target_clock;
  zx_status_t st = GetMesonRateClock(clk, &target_clock);
  if (st != ZX_OK) {
    return st;
  }

  return target_clock->QuerySupportedRate(max_rate, out_best_rate);
}

zx_status_t AmlClock::ClockImplGetRate(uint32_t clk, uint64_t* out_current_rate) {
  zxlogf(TRACE, "%s: clk = %u", __func__, clk);

  if (out_current_rate == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }

  MesonRateClock* target_clock;
  zx_status_t st = GetMesonRateClock(clk, &target_clock);
  if (st != ZX_OK) {
    return st;
  }

  return target_clock->GetRate(out_current_rate);
}

zx_status_t AmlClock::IsSupportedMux(const uint32_t id, const uint16_t kSupportedMask) {
  const uint16_t index = aml_clk_common::AmlClkIndex(id);
  const uint16_t type = static_cast<uint16_t>(aml_clk_common::AmlClkType(id));

  if ((type & kSupportedMask) == 0) {
    zxlogf(ERROR, "%s: Unsupported mux type for operation, clkid = %u", __func__, id);
    return ZX_ERR_NOT_SUPPORTED;
  }

  if (!muxes_ || mux_count_ == 0) {
    zxlogf(ERROR, "%s: Platform does not have mux support.", __func__);
    return ZX_ERR_NOT_SUPPORTED;
  }

  if (index >= mux_count_) {
    zxlogf(ERROR, "%s: Mux index out of bounds, count = %lu, idx = %u", __func__, mux_count_,
           index);
    return ZX_ERR_OUT_OF_RANGE;
  }

  return ZX_OK;
}

zx_status_t AmlClock::ClockImplSetInput(uint32_t id, uint32_t idx) {
  constexpr uint16_t kSupported = static_cast<uint16_t>(aml_clk_common::aml_clk_type::kMesonMux);
  zx_status_t st = IsSupportedMux(id, kSupported);
  if (st != ZX_OK) {
    return st;
  }

  const uint16_t index = aml_clk_common::AmlClkIndex(id);

  fbl::AutoLock al(&lock_);

  const meson_clk_mux_t& mux = muxes_[index];

  if (idx >= mux.n_inputs) {
    zxlogf(ERROR, "%s: mux input index out of bounds, max = %u, idx = %u.", __func__, mux.n_inputs,
           idx);
    return ZX_ERR_OUT_OF_RANGE;
  }

  uint32_t clkidx;
  if (mux.inputs) {
    clkidx = mux.inputs[idx];
  } else {
    clkidx = idx;
  }

  uint32_t val = hiu_mmio_.Read32(mux.reg);
  val &= ~(mux.mask << mux.shift);
  val |= (clkidx & mux.mask) << mux.shift;
  hiu_mmio_.Write32(val, mux.reg);

  return ZX_OK;
}

zx_status_t AmlClock::ClockImplGetNumInputs(uint32_t id, uint32_t* out_num_inputs) {
  constexpr uint16_t kSupported =
      (static_cast<uint16_t>(aml_clk_common::aml_clk_type::kMesonMux) |
       static_cast<uint16_t>(aml_clk_common::aml_clk_type::kMesonMuxRo));

  zx_status_t st = IsSupportedMux(id, kSupported);
  if (st != ZX_OK) {
    return st;
  }

  const uint16_t index = aml_clk_common::AmlClkIndex(id);

  const meson_clk_mux_t& mux = muxes_[index];

  *out_num_inputs = mux.n_inputs;

  return ZX_OK;
}

zx_status_t AmlClock::ClockImplGetInput(uint32_t id, uint32_t* out_input) {
  // Bitmask representing clock types that support this operation.
  constexpr uint16_t kSupported =
      (static_cast<uint16_t>(aml_clk_common::aml_clk_type::kMesonMux) |
       static_cast<uint16_t>(aml_clk_common::aml_clk_type::kMesonMuxRo));

  zx_status_t st = IsSupportedMux(id, kSupported);
  if (st != ZX_OK) {
    return st;
  }

  const uint16_t index = aml_clk_common::AmlClkIndex(id);

  const meson_clk_mux_t& mux = muxes_[index];

  const uint32_t result = (hiu_mmio_.Read32(mux.reg) >> mux.shift) & mux.mask;

  if (mux.inputs) {
    for (uint32_t i = 0; i < mux.n_inputs; i++) {
      if (result == mux.inputs[i]) {
        *out_input = i;
        return ZX_OK;
      }
    }
  }

  *out_input = result;
  return ZX_OK;
}

// Note: The clock index taken here are the index of clock
// from the clock table and not the clock_gates index.
// This API measures the clk frequency for clk.
// Following implementation is adopted from Amlogic SDK,
// there is absolutely no documentation.
zx_status_t AmlClock::ClkMeasureUtil(uint32_t clk, uint64_t* clk_freq) {
  if (!msr_mmio_) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  // Set the measurement gate to 64uS.
  uint32_t value = 64 - 1;
  msr_mmio_->Write32(value, clk_msr_offsets_.reg0_offset);
  // Disable continuous measurement.
  // Disable interrupts.
  value = MSR_CONT | MSR_INTR;
  // Clear the clock source.
  value |= MSR_CLK_SRC_MASK << MSR_CLK_SRC_SHIFT;
  msr_mmio_->ClearBits32(value, clk_msr_offsets_.reg0_offset);

  value = ((clk << MSR_CLK_SRC_SHIFT) |  // Select the MUX.
           MSR_RUN |                     // Enable the clock.
           MSR_ENABLE);                  // Enable measuring.
  msr_mmio_->SetBits32(value, clk_msr_offsets_.reg0_offset);

  // Wait for the measurement to be done.
  for (uint32_t i = 0; i < MSR_WAIT_BUSY_RETRIES; i++) {
    value = msr_mmio_->Read32(clk_msr_offsets_.reg0_offset);
    if (value & MSR_BUSY) {
      // Wait a little bit before trying again.
      zx_nanosleep(zx_deadline_after(ZX_USEC(MSR_WAIT_BUSY_TIMEOUT_US)));
      continue;
    } else {
      // Disable measuring.
      msr_mmio_->ClearBits32(MSR_ENABLE, clk_msr_offsets_.reg0_offset);
      // Get the clock value.
      value = msr_mmio_->Read32(clk_msr_offsets_.reg2_offset);
      // Magic numbers, since lack of documentation.
      *clk_freq = (((value + 31) & MSR_VAL_MASK) / 64);
      return ZX_OK;
    }
  }
  return ZX_ERR_TIMED_OUT;
}

void AmlClock::Measure(MeasureRequestView request, MeasureCompleter::Sync& completer) {
  fuchsia_hardware_clock::wire::FrequencyInfo info;
  if (request->clock >= clk_table_count_) {
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  size_t max_len = info.name.size();
  size_t len = strnlen(clk_table_[request->clock], max_len);
  if (len == max_len) {
    completer.Close(ZX_ERR_INVALID_ARGS);
    return;
  }

  memcpy(info.name.data(), clk_table_[request->clock], len + 1);
  zx_status_t status = ClkMeasureUtil(request->clock, &info.frequency);
  if (status != ZX_OK) {
    completer.Close(status);
    return;
  }

  completer.Reply(info);
}

void AmlClock::GetCount(GetCountCompleter::Sync& completer) {
  completer.Reply(static_cast<uint32_t>(clk_table_count_));
}

void AmlClock::Enable(EnableRequestView request, EnableCompleter::Sync& completer) {
  zx_status_t result = ClkToggle(request->clock, true);

  if (result == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.ReplyError(result);
  }
}

void AmlClock::Disable(DisableRequestView request, DisableCompleter::Sync& completer) {
  zx_status_t result = ClkDebugForceDisable(request->clock);

  if (result == ZX_OK) {
    completer.ReplySuccess();
  } else {
    completer.ReplyError(result);
  };
}

void AmlClock::ShutDown() {
  hiu_mmio_.reset();

  if (msr_mmio_) {
    msr_mmio_->reset();
  }
}

void AmlClock::Register(fdf::WireSyncClient<fuchsia_hardware_platform_bus::PlatformBus> pbus) {
  clock_impl_protocol_t clk_proto = {
      .ops = &clock_impl_protocol_ops_,
      .ctx = this,
  };

  fdf::Arena arena('CLK_');
  [[maybe_unused]] auto unused = pbus.buffer(arena)->RegisterProtocol(
      ZX_PROTOCOL_CLOCK_IMPL, fidl::VectorView<uint8_t>::FromExternal(
                                  reinterpret_cast<uint8_t*>(&clk_proto), sizeof(clk_proto)));
}

zx_status_t AmlClock::GetMesonRateClock(const uint32_t clk, MesonRateClock** out) {
  aml_clk_common::aml_clk_type type = aml_clk_common::AmlClkType(clk);
  const uint16_t clkid = aml_clk_common::AmlClkIndex(clk);

  switch (type) {
    case aml_clk_common::aml_clk_type::kMesonPll:
      if (clkid >= pll_count_) {
        zxlogf(ERROR, "%s: HIU PLL out of range, clkid = %hu.", __func__, clkid);
        return ZX_ERR_INVALID_ARGS;
      }

      *out = &pllclk_[clkid];
      return ZX_OK;
    case aml_clk_common::aml_clk_type::kMesonCpuClk:
      if (clkid >= cpu_clks_.size()) {
        zxlogf(ERROR, "%s: cpu clk out of range, clkid = %hu.", __func__, clkid);
        return ZX_ERR_INVALID_ARGS;
      }

      *out = &cpu_clks_[clkid];
      return ZX_OK;
    default:
      zxlogf(ERROR, "%s: Unsupported clock type, type = 0x%hx\n", __func__, type);
      return ZX_ERR_NOT_SUPPORTED;
  }

  __UNREACHABLE;
}

void AmlClock::InitHiu() {
  pllclk_.reserve(pll_count_);
  s905d2_hiu_init_etc(&hiudev_, static_cast<MMIO_PTR uint8_t*>(hiu_mmio_.get()));
  for (unsigned int pllnum = 0; pllnum < pll_count_; pllnum++) {
    const hhi_plls_t pll = static_cast<hhi_plls_t>(pllnum);
    pllclk_.emplace_back(pll, &hiudev_);
    pllclk_[pllnum].Init();
  }
}

void AmlClock::InitHiuA5() {
  pllclk_.reserve(pll_count_);
  for (unsigned int pllnum = 0; pllnum < pll_count_; pllnum++) {
    auto plldev = a5::CreatePllDevice(&dosbus_mmio_, pllnum);
    pllclk_.emplace_back(std::move(plldev));
    pllclk_[pllnum].Init();
  }
}

void AmlClock::InitHiuA1() {
  pllclk_.reserve(pll_count_);
  for (unsigned int pllnum = 0; pllnum < pll_count_; pllnum++) {
    auto plldev = a1::CreatePllDevice(&dosbus_mmio_, pllnum);
    pllclk_.emplace_back(std::move(plldev));
    pllclk_[pllnum].Init();
  }
}

void AmlClock::DdkUnbind(ddk::UnbindTxn txn) {
  ShutDown();
  txn.Reply();
}

void AmlClock::DdkRelease() { delete this; }

}  // namespace amlogic_clock

zx_status_t aml_clk_bind(void* ctx, zx_device_t* parent) {
  return amlogic_clock::AmlClock::Create(parent);
}

static constexpr zx_driver_ops_t aml_clk_driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = aml_clk_bind;
  return ops;
}();

// clang-format off
ZIRCON_DRIVER(aml_clk, aml_clk_driver_ops, "zircon", "0.1");
