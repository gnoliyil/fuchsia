// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_PWM_DRIVERS_AML_PWM_AML_PWM_H_
#define SRC_DEVICES_PWM_DRIVERS_AML_PWM_AML_PWM_H_

#include <fuchsia/hardware/pwm/cpp/banjo.h>
#include <lib/ddk/platform-defs.h>
#include <lib/mmio/mmio.h>
#include <zircon/types.h>

#include <array>
#include <vector>

#include <ddk/metadata/pwm.h>
#include <ddktl/device.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>

#include "aml-pwm-regs.h"

namespace pwm {

class AmlPwm;
class AmlPwmDevice;
using AmlPwmDeviceType = ddk::Device<AmlPwmDevice>;
constexpr size_t kPwmPairCount = 2;

class AmlPwm {
 public:
  explicit AmlPwm(fdf::MmioBuffer mmio, pwm_id_t id1, pwm_id_t id2)
      : ids_{id1, id2}, enabled_{false, false}, mmio_(std::move(mmio)) {}

  void Init() {
    for (size_t i = 0; i < kPwmPairCount; i++) {
      mode_configs_[i].mode = Mode::kOff;
      mode_configs_[i].regular = {};
      configs_[i] = {false, 0, 0.0, reinterpret_cast<uint8_t*>(&mode_configs_[i]),
                     sizeof(mode_config)};
      if (ids_[i].init) {
        SetMode(i, Mode::kOff);
      }
    }
  }

  zx_status_t PwmImplGetConfig(uint32_t idx, pwm_config_t* out_config);
  zx_status_t PwmImplSetConfig(uint32_t idx, const pwm_config_t* config);
  zx_status_t PwmImplEnable(uint32_t idx);
  zx_status_t PwmImplDisable(uint32_t idx);

 private:
  friend class AmlPwmDevice;

  // Register fine control.

  // Sets the PWM controller working mode. `mode` must be a valid PWM mode.
  void SetMode(uint32_t idx, Mode mode);
  // Sets the duty cycle for the default timer. `duty_cycle` must be a float
  // value in the range [0.0, 100.0].
  void SetDutyCycle(uint32_t idx, uint32_t period, float duty_cycle);
  // Sets the duty cycle for the second timer in two-timer mode. `duty_cycle`
  // must be a float value in the range [0.0, 100.0].
  void SetDutyCycle2(uint32_t idx, uint32_t period, float duty_cycle);
  void Invert(uint32_t idx, bool on);
  void EnableHiZ(uint32_t idx, bool on);
  void EnableClock(uint32_t idx, bool on);
  void EnableConst(uint32_t idx, bool on);
  void SetClock(uint32_t idx, uint8_t sel);
  void SetClockDivider(uint32_t idx, uint8_t div);
  void EnableBlink(uint32_t idx, bool on);
  void SetBlinkTimes(uint32_t idx, uint8_t times);
  void SetDSSetting(uint32_t idx, uint16_t val);
  void SetTimers(uint32_t idx, uint8_t timer1, uint8_t timer2);

  std::array<pwm_id_t, kPwmPairCount> ids_;
  std::array<bool, kPwmPairCount> enabled_;
  std::array<pwm_config_t, kPwmPairCount> configs_;
  std::array<mode_config, kPwmPairCount> mode_configs_;
  std::array<fbl::Mutex, REG_COUNT> locks_;
  fdf::MmioBuffer mmio_;
};

class AmlPwmDevice : public AmlPwmDeviceType,
                     public ddk::PwmImplProtocol<AmlPwmDevice, ddk::base_protocol> {
 public:
  static zx_status_t Create(void* ctx, zx_device_t* parent);

  void DdkRelease() { delete this; }

  zx_status_t PwmImplGetConfig(uint32_t idx, pwm_config_t* out_config);
  zx_status_t PwmImplSetConfig(uint32_t idx, const pwm_config_t* config);
  zx_status_t PwmImplEnable(uint32_t idx);
  zx_status_t PwmImplDisable(uint32_t idx);

 protected:
  // For unit testing
  explicit AmlPwmDevice() : AmlPwmDeviceType(nullptr) {}
  zx_status_t Init(fdf::MmioBuffer mmio0, fdf::MmioBuffer mmio1, fdf::MmioBuffer mmio2,
                   fdf::MmioBuffer mmio3, fdf::MmioBuffer mmio4, std::vector<pwm_id_t> ids) {
    pwms_.push_back(std::make_unique<AmlPwm>(std::move(mmio0), ids.at(0), ids.at(1)));
    pwms_.back()->Init();
    pwms_.push_back(std::make_unique<AmlPwm>(std::move(mmio1), ids.at(2), ids.at(3)));
    pwms_.back()->Init();
    pwms_.push_back(std::make_unique<AmlPwm>(std::move(mmio2), ids.at(4), ids.at(5)));
    pwms_.back()->Init();
    pwms_.push_back(std::make_unique<AmlPwm>(std::move(mmio3), ids.at(6), ids.at(7)));
    pwms_.back()->Init();
    pwms_.push_back(std::make_unique<AmlPwm>(std::move(mmio4), ids.at(8), ids.at(9)));
    pwms_.back()->Init();

    return ZX_OK;
  }

 private:
  explicit AmlPwmDevice(zx_device_t* parent) : AmlPwmDeviceType(parent) {}

  zx_status_t Init(zx_device_t* parent);

  std::vector<std::unique_ptr<AmlPwm>> pwms_;
};

}  // namespace pwm

#endif  // SRC_DEVICES_PWM_DRIVERS_AML_PWM_AML_PWM_H_
