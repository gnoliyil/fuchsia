// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/intel-i915/interrupts.h"

#include <lib/device-protocol/pci.h>
#include <zircon/assert.h>
#include <zircon/syscalls.h>
#include <zircon/threads.h>

#include <bitset>

#include "src/graphics/display/drivers/intel-i915/ddi.h"
#include "src/graphics/display/drivers/intel-i915/intel-i915.h"
#include "src/graphics/display/drivers/intel-i915/pci-ids.h"
#include "src/graphics/display/drivers/intel-i915/registers-ddi.h"
#include "src/graphics/display/drivers/intel-i915/registers-pipe.h"
#include "src/graphics/display/drivers/intel-i915/registers.h"

namespace i915 {

namespace {

struct HotplugDetectionResult {
  constexpr static size_t kMaxAllowedDdis = 32;
  std::bitset<kMaxAllowedDdis> detected;
  std::bitset<kMaxAllowedDdis> long_pulse;
};

HotplugDetectionResult DetectHotplugSkylake(fdf::MmioBuffer* mmio_space) {
  HotplugDetectionResult result;

  auto sde_int_identity =
      registers::SdeInterruptBase::Get(registers ::SdeInterruptBase::kSdeIntIdentity)
          .ReadFrom(mmio_space);
  auto hp_ctrl1 = registers::SouthHotplugCtrl ::Get(DdiId::DDI_A).ReadFrom(mmio_space);
  auto hp_ctrl2 = registers::SouthHotplugCtrl ::Get(DdiId::DDI_E).ReadFrom(mmio_space);
  for (auto ddi : DdiIds<registers::Platform::kKabyLake>()) {
    auto hp_ctrl = ddi < DdiId::DDI_E ? hp_ctrl1 : hp_ctrl2;
    result.detected[ddi] =
        sde_int_identity.skl_ddi_bit(ddi).get() &
        (hp_ctrl.hpd_long_pulse(ddi).get() || hp_ctrl.hpd_short_pulse(ddi).get());
    result.long_pulse[ddi] = hp_ctrl.hpd_long_pulse(ddi).get();
  }
  // Write back the register to clear the bits
  hp_ctrl1.WriteTo(mmio_space);
  hp_ctrl2.WriteTo(mmio_space);
  sde_int_identity.WriteTo(mmio_space);

  return result;
}

HotplugDetectionResult DetectHotplugTigerLake(fdf::MmioBuffer* mmio_space) {
  HotplugDetectionResult result;

  auto sde_int_identity =
      registers::SdeInterruptBase::Get(registers ::SdeInterruptBase::kSdeIntIdentity)
          .ReadFrom(mmio_space);
  auto hpd_int_identity =
      registers::HpdInterruptBase::Get(registers::HpdInterruptBase::kHpdIntIdentity)
          .ReadFrom(mmio_space);

  auto pch_ddi_ctrl = registers::IclSouthHotplugCtrl::Get(DdiId::DDI_A).ReadFrom(mmio_space);
  auto pch_tc_ctrl = registers::IclSouthHotplugCtrl::Get(DdiId::DDI_TC_1).ReadFrom(mmio_space);

  auto tbt_ctrl = registers::TbtHotplugCtrl::Get().ReadFrom(mmio_space);
  auto tc_ctrl = registers::TcHotplugCtrl::Get().ReadFrom(mmio_space);

  for (auto ddi : DdiIds<registers::Platform::kTigerLake>()) {
    switch (ddi) {
      case DdiId::DDI_A:
      case DdiId::DDI_B:
      case DdiId::DDI_C: {
        result.detected[ddi] =
            sde_int_identity.icl_ddi_bit(ddi).get() &
            (pch_ddi_ctrl.hpd_long_pulse(ddi).get() || pch_ddi_ctrl.hpd_short_pulse(ddi).get());
        result.long_pulse[ddi] = pch_ddi_ctrl.hpd_long_pulse(ddi).get();
      } break;
      case DdiId::DDI_TC_1:
      case DdiId::DDI_TC_2:
      case DdiId::DDI_TC_3:
      case DdiId::DDI_TC_4:
      case DdiId::DDI_TC_5:
      case DdiId::DDI_TC_6: {
        bool sde_detected = sde_int_identity.icl_ddi_bit(ddi).get();
        bool tbt_detected = hpd_int_identity.tbt_hotplug(ddi).get();
        bool tc_detected = hpd_int_identity.tc_hotplug(ddi).get();
        result.detected[ddi] = tbt_detected || tc_detected || sde_detected;
        result.long_pulse[ddi] = (tbt_detected && tbt_ctrl.hpd_long_pulse(ddi).get()) ||
                                 (tc_detected && tc_ctrl.hpd_long_pulse(ddi).get()) ||
                                 (sde_detected && pch_tc_ctrl.hpd_long_pulse(ddi).get());
      } break;
    }
  }

  // Write back the register to clear the bits
  pch_ddi_ctrl.WriteTo(mmio_space);
  pch_tc_ctrl.WriteTo(mmio_space);
  tbt_ctrl.WriteTo(mmio_space);
  tc_ctrl.WriteTo(mmio_space);
  sde_int_identity.WriteTo(mmio_space);
  hpd_int_identity.WriteTo(mmio_space);

  return result;
}

void EnableHotplugInterruptsSkylake(fdf::MmioBuffer* mmio_space) {
  auto pch_fuses = registers::PchDisplayFuses::Get().ReadFrom(mmio_space);

  for (const auto ddi : DdiIds<registers::Platform::kKabyLake>()) {
    bool enabled = false;
    switch (ddi) {
      case DdiId::DDI_A:
      case DdiId::DDI_E:
        enabled = true;
        break;
      case DdiId::DDI_B:
        enabled = pch_fuses.port_b_present();
        break;
      case DdiId::DDI_C:
        enabled = pch_fuses.port_c_present();
        break;
      case DdiId::DDI_D:
        enabled = pch_fuses.port_d_present();
        break;
      case DdiId::DDI_TC_3:
      case DdiId::DDI_TC_4:
      case DdiId::DDI_TC_5:
      case DdiId::DDI_TC_6:
        ZX_DEBUG_ASSERT_MSG(false, "Unsupported DDI (%d)", ddi);
        break;
    }

    auto hp_ctrl = registers::SouthHotplugCtrl::Get(ddi).ReadFrom(mmio_space);
    hp_ctrl.hpd_enable(ddi).set(enabled);
    hp_ctrl.WriteTo(mmio_space);

    auto mask = registers::SdeInterruptBase::Get(registers::SdeInterruptBase::kSdeIntMask)
                    .ReadFrom(mmio_space);
    mask.skl_ddi_bit(ddi).set(!enabled);
    mask.WriteTo(mmio_space);

    auto enable = registers::SdeInterruptBase::Get(registers::SdeInterruptBase::kSdeIntEnable)
                      .ReadFrom(mmio_space);
    enable.skl_ddi_bit(ddi).set(enabled);
    enable.WriteTo(mmio_space);
  }
}

void EnableHotplugInterruptsTigerLake(fdf::MmioBuffer* mmio_space) {
  constexpr zx_off_t kSHPD_FILTER_CNT = 0xc4038;
  constexpr uint32_t kSHPD_FILTER_CNT_500_ADJ = 0x001d9;
  mmio_space->Write32(kSHPD_FILTER_CNT_500_ADJ, kSHPD_FILTER_CNT);

  for (const auto ddi : DdiIds<registers::Platform::kTigerLake>()) {
    switch (ddi) {
      case DdiId::DDI_TC_1:
      case DdiId::DDI_TC_2:
      case DdiId::DDI_TC_3:
      case DdiId::DDI_TC_4:
      case DdiId::DDI_TC_5:
      case DdiId::DDI_TC_6: {
        auto hp_ctrl = registers::TcHotplugCtrl::Get().ReadFrom(mmio_space);
        hp_ctrl.hpd_enable(ddi).set(1);
        hp_ctrl.WriteTo(mmio_space);

        auto mask = registers::HpdInterruptBase::Get(registers::HpdInterruptBase::kHpdIntMask)
                        .ReadFrom(mmio_space);
        mask.set_reg_value(0);
        mask.WriteTo(mmio_space);

        auto enable = registers::HpdInterruptBase::Get(registers::HpdInterruptBase::kHpdIntEnable)
                          .ReadFrom(mmio_space);
        enable.tc_hotplug(ddi).set(1);
        enable.tbt_hotplug(ddi).set(1);
        enable.WriteTo(mmio_space);
      }
        __FALLTHROUGH;
      case DdiId::DDI_A:
      case DdiId::DDI_B:
      case DdiId::DDI_C: {
        auto hp_ctrl = registers::IclSouthHotplugCtrl::Get(ddi).ReadFrom(mmio_space);
        hp_ctrl.hpd_enable(ddi).set(1);
        hp_ctrl.WriteTo(mmio_space);

        auto mask = registers::SdeInterruptBase::Get(registers::SdeInterruptBase::kSdeIntMask)
                        .ReadFrom(mmio_space);
        mask.set_reg_value(0);
        mask.WriteTo(mmio_space);
        mask.ReadFrom(mmio_space);

        auto enable = registers::SdeInterruptBase::Get(registers::SdeInterruptBase::kSdeIntEnable)
                          .ReadFrom(mmio_space);
        enable.icl_ddi_bit(ddi).set(1);
        enable.WriteTo(mmio_space);
      } break;
    }
  }
}

}  // namespace

Interrupts::Interrupts() { mtx_init(&lock_, mtx_plain); }

Interrupts::~Interrupts() { Destroy(); }

void Interrupts::Destroy() {
  irq_.destroy();
  if (irq_thread_) {
    thrd_join(irq_thread_.value(), nullptr);
    irq_thread_ = std::nullopt;
  }
  irq_.reset();
}

int Interrupts::IrqLoop() {
  // We implement the steps in the section "Shared Functions" > "Interrupts" >
  // "Interrupt Service Routine" section of Intel's display engine docs.
  //
  // Tiger Lake: IHD-OS-TGL-Vol 12-1.22-Rev2.0 pages 199-200
  // Kaby Lake: IHD-OS-KBL-Vol 12-1.17 pages 142-143
  // Skylake: IHD-OS-SKL-Vol 12-05.16 pages 139-140
  for (;;) {
    zx_time_t timestamp;
    if (zx_interrupt_wait(irq_.get(), &timestamp) != ZX_OK) {
      zxlogf(INFO, "interrupt wait failed");
      return -1;
    }

    auto graphics_primary_interrupts = registers::GraphicsPrimaryInterrupt::Get().FromValue(0);
    if (is_tgl(device_id_)) {
      graphics_primary_interrupts.ReadFrom(mmio_space_)
          .set_interrupts_enabled(false)
          .WriteTo(mmio_space_);
    }

    auto display_interrupts = registers::DisplayInterruptControl::Get().ReadFrom(mmio_space_);
    display_interrupts.set_interrupts_enabled(false);
    display_interrupts.WriteTo(mmio_space_);

    const bool pch_display_hotplug_pending = display_interrupts.pch_engine_pending();
    const bool display_hotplug_pending =
        is_tgl(device_id_) && display_interrupts.display_hot_plug_pending_tiger_lake();

    if (pch_display_hotplug_pending || display_hotplug_pending) {
      auto detect_result = is_tgl(device_id_) ? DetectHotplugTigerLake(mmio_space_)
                                              : DetectHotplugSkylake(mmio_space_);
      for (auto ddi : GetDdiIds(device_id_)) {
        if (detect_result.detected[ddi]) {
          zxlogf(TRACE, "Detected hot plug interrupt on ddi %d", ddi);
          hotplug_callback_(ddi, detect_result.long_pulse[ddi]);
        }
      }
    }

    // TODO(fxbug.dev/109278): Check for Pipe D interrupts here when we support
    //                         pipe and transcoder D.

    if (display_interrupts.pipe_c_pending()) {
      HandlePipeInterrupt(PipeId::PIPE_C, timestamp);
    }
    if (display_interrupts.pipe_b_pending()) {
      HandlePipeInterrupt(PipeId::PIPE_B, timestamp);
    }
    if (display_interrupts.pipe_a_pending()) {
      HandlePipeInterrupt(PipeId::PIPE_A, timestamp);
    }

    {
      // Dispatch GT interrupts to the GPU driver.
      fbl::AutoLock lock(&lock_);
      if (gpu_interrupt_callback_.callback) {
        if (is_tgl(device_id_)) {
          if (graphics_primary_interrupts.gt1_interrupt_pending() ||
              graphics_primary_interrupts.gt0_interrupt_pending()) {
            // Mask isn't used
            gpu_interrupt_callback_.callback(gpu_interrupt_callback_.ctx, 0, timestamp);
          }
        } else {
          if (display_interrupts.reg_value() & gpu_interrupt_mask_) {
            gpu_interrupt_callback_.callback(gpu_interrupt_callback_.ctx,
                                             display_interrupts.reg_value(), timestamp);
          }
        }
      }
    }

    display_interrupts.set_interrupts_enabled(true).WriteTo(mmio_space_);

    if (is_tgl(device_id_)) {
      graphics_primary_interrupts.set_interrupts_enabled(true).WriteTo(mmio_space_);
    }
  }
}

void Interrupts::HandlePipeInterrupt(PipeId pipe_id, zx_time_t timestamp) {
  registers::PipeRegs regs(pipe_id);
  auto interrupt_identity =
      regs.PipeInterrupt(registers::PipeRegs::InterruptRegister::kIdentity).ReadFrom(mmio_space_);

  // Interrupt Identity Registers (IIR) are R/WC (Read/Write Clear), meaning
  // that indicator bits are cleared by writing 1s to them. Writing the value we
  // just read declares that we've handled all the interrupts reported there.
  interrupt_identity.WriteTo(mmio_space_);

  if (interrupt_identity.underrun()) {
    zxlogf(WARNING, "Transcoder underrun on pipe %d", pipe_id);
  }
  if (interrupt_identity.vsync()) {
    pipe_vsync_callback_(pipe_id, timestamp);
  }
}

void Interrupts::EnablePipeInterrupts(PipeId pipe_id, bool enable) {
  registers::PipeRegs regs(pipe_id);
  auto interrupt_mask =
      regs.PipeInterrupt(registers::PipeRegs::InterruptRegister::kMask).FromValue(0);
  interrupt_mask.set_underrun(!enable).set_vsync(!enable).WriteTo(mmio_space_);

  auto interrupt_enable =
      regs.PipeInterrupt(registers::PipeRegs::InterruptRegister::kEnable).FromValue(0);
  interrupt_enable.set_underrun(enable).set_vsync(enable).WriteTo(mmio_space_);
}

zx_status_t Interrupts::SetGpuInterruptCallback(
    const intel_gpu_core_interrupt_t& gpu_interrupt_callback, uint32_t gpu_interrupt_mask) {
  fbl::AutoLock lock(&lock_);
  if (gpu_interrupt_callback.callback != nullptr && gpu_interrupt_callback_.callback != nullptr) {
    return ZX_ERR_ALREADY_BOUND;
  }
  gpu_interrupt_callback_ = gpu_interrupt_callback;
  gpu_interrupt_mask_ = gpu_interrupt_mask;
  return ZX_OK;
}

zx_status_t Interrupts::Init(PipeVsyncCallback pipe_vsync_callback,
                             HotplugCallback hotplug_callback, zx_device_t* dev,
                             const ddk::Pci& pci, fdf::MmioBuffer* mmio_space, uint16_t device_id) {
  ZX_DEBUG_ASSERT(pipe_vsync_callback);
  ZX_DEBUG_ASSERT(hotplug_callback);
  ZX_DEBUG_ASSERT(dev);
  ZX_DEBUG_ASSERT(mmio_space);

  // TODO(fxbug.dev/86038): Looks like calling Init multiple times is allowed for unit tests but it
  // would make the state of instances of this class more predictable to disallow this.
  if (irq_) {
    Destroy();
  }

  pipe_vsync_callback_ = std::move(pipe_vsync_callback);
  hotplug_callback_ = std::move(hotplug_callback);
  mmio_space_ = mmio_space;
  device_id_ = device_id;

  // Interrupt propagation will be re-enabled in ::FinishInit()
  zxlogf(TRACE, "Disabling graphics and display interrupt propagation");

  if (is_tgl(device_id_)) {
    auto graphics_primary_interrupts =
        registers::GraphicsPrimaryInterrupt::Get().ReadFrom(mmio_space);
    graphics_primary_interrupts.set_interrupts_enabled(false).WriteTo(mmio_space_);
  }

  auto interrupt_ctrl = registers::DisplayInterruptControl::Get().ReadFrom(mmio_space);
  interrupt_ctrl.set_interrupts_enabled(false).WriteTo(mmio_space);

  // Assume that PCI will enable bus mastering as required for MSI interrupts.
  zx_status_t status = pci.ConfigureInterruptMode(1, &irq_mode_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to configure irq mode (%d)", status);
    return ZX_ERR_INTERNAL;
  }

  status = pci.MapInterrupt(0, &irq_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Failed to map interrupt (%d)", status);
    return status;
  }

  {
    thrd_t thread;
    int thrd_status = thrd_create_with_name(
        &thread, [](void* ctx) { return static_cast<Interrupts*>(ctx)->IrqLoop(); }, this,
        "i915-irq-thread");
    if (thrd_status != thrd_success) {
      status = thrd_status_to_zx_status(thrd_status);
      zxlogf(ERROR, "Failed to create irq thread (%d)", status);
      irq_.reset();
      return status;
    }
    irq_thread_ = thread;
  }

  const char* role_name = "fuchsia.graphics.display.drivers.intel-i915.interrupt";
  status = device_set_profile_by_role(dev, thrd_get_zx_handle(*irq_thread_), role_name,
                                      strlen(role_name));
  if (status != ZX_OK) {
    zxlogf(WARNING, "Failed to apply role: %s", zx_status_get_string(status));
  }

  Resume();
  return ZX_OK;
}

void Interrupts::FinishInit() {
  zxlogf(TRACE, "Interrupts re-enabled");

  auto display_interrupts = registers::DisplayInterruptControl::Get().ReadFrom(mmio_space_);
  display_interrupts.set_interrupts_enabled(true).WriteTo(mmio_space_);

  if (is_tgl(device_id_)) {
    auto graphics_primary_interrupts =
        registers::GraphicsPrimaryInterrupt::Get().ReadFrom(mmio_space_);
    graphics_primary_interrupts.set_interrupts_enabled(true).WriteTo(mmio_space_);

    graphics_primary_interrupts.ReadFrom(mmio_space_);  // posting read
  }
}

void Interrupts::Resume() {
  if (is_tgl(device_id_)) {
    EnableHotplugInterruptsTigerLake(mmio_space_);
  } else {
    EnableHotplugInterruptsSkylake(mmio_space_);
  }
}

}  // namespace i915
