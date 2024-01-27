// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-pwm-init.h"

#include <fuchsia/hardware/gpio/cpp/banjo-mock.h>
#include <fuchsia/hardware/pwm/cpp/banjo-mock.h>

#include <fbl/alloc_checker.h>

bool operator==(const pwm_config_t& lhs, const pwm_config_t& rhs) {
  return (lhs.polarity == rhs.polarity) && (lhs.period_ns == rhs.period_ns) &&
         (lhs.duty_cycle == rhs.duty_cycle) && (lhs.mode_config_size == rhs.mode_config_size) &&
         (reinterpret_cast<aml_pwm::mode_config*>(lhs.mode_config_buffer)->mode ==
          reinterpret_cast<aml_pwm::mode_config*>(rhs.mode_config_buffer)->mode);
}

namespace pwm_init {

class FakePwmInitDevice : public PwmInitDevice {
 public:
  static std::unique_ptr<FakePwmInitDevice> Create(const pwm_protocol_t* pwm,
                                                   const gpio_protocol_t* wifi_gpio,
                                                   const gpio_protocol_t* bt_gpio) {
    fbl::AllocChecker ac;
    auto device = fbl::make_unique_checked<FakePwmInitDevice>(&ac, ddk::PwmProtocolClient(pwm),
                                                              ddk::GpioProtocolClient(wifi_gpio),
                                                              ddk::GpioProtocolClient(bt_gpio));
    if (!ac.check()) {
      zxlogf(ERROR, "%s: device object alloc failed", __func__);
      return nullptr;
    }
    EXPECT_OK(device->Init());

    return device;
  }

  explicit FakePwmInitDevice(ddk::PwmProtocolClient pwm, ddk::GpioProtocolClient wifi_gpio,
                             ddk::GpioProtocolClient bt_gpio)
      : PwmInitDevice(nullptr, pwm, wifi_gpio, bt_gpio) {}
};

TEST(PwmInitDeviceTest, InitTest) {
  ddk::MockPwm pwm_;
  ddk::MockGpio wifi_gpio_;
  ddk::MockGpio bt_gpio_;

  wifi_gpio_.ExpectSetAltFunction(ZX_OK, 1);
  pwm_.ExpectEnable(ZX_OK);
  aml_pwm::mode_config two_timer = {
      .mode = aml_pwm::Mode::kTwoTimer,
      .two_timer =
          {
              .period_ns2 = 30052,
              .duty_cycle2 = 50.0,
              .timer1 = 0x0a,
              .timer2 = 0x0a,
          },
  };
  pwm_config_t init_cfg = {
      .polarity = false,
      .period_ns = 30053,
      .duty_cycle = static_cast<float>(49.931787176),
      .mode_config_buffer = reinterpret_cast<uint8_t*>(&two_timer),
      .mode_config_size = sizeof(two_timer),
  };
  pwm_.ExpectSetConfig(ZX_OK, init_cfg);
  bt_gpio_.ExpectConfigOut(ZX_OK, 0);
  bt_gpio_.ExpectWrite(ZX_OK, 1);

  std::unique_ptr<FakePwmInitDevice> dev_ =
      FakePwmInitDevice::Create(pwm_.GetProto(), wifi_gpio_.GetProto(), bt_gpio_.GetProto());
  ASSERT_NOT_NULL(dev_);

  pwm_.VerifyAndClear();
  wifi_gpio_.VerifyAndClear();
  bt_gpio_.VerifyAndClear();
}

}  // namespace pwm_init
