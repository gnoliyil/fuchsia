// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-light.h"

#include <fuchsia/hardware/gpio/cpp/banjo-mock.h>
#include <fuchsia/hardware/pwm/cpp/banjo-mock.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl-async/cpp/bind.h>

#include <cmath>

#include <fbl/alloc_checker.h>

bool operator==(const pwm_config_t& lhs, const pwm_config_t& rhs) {
  return (lhs.polarity == rhs.polarity) && (lhs.period_ns == rhs.period_ns) &&
         (lhs.duty_cycle == rhs.duty_cycle) && (lhs.mode_config_size == rhs.mode_config_size) &&
         (reinterpret_cast<aml_pwm::mode_config*>(lhs.mode_config_buffer)->mode ==
          reinterpret_cast<aml_pwm::mode_config*>(rhs.mode_config_buffer)->mode);
}

namespace aml_light {

class FakeAmlLight : public AmlLight {
 public:
  static std::unique_ptr<FakeAmlLight> Create(const gpio_protocol_t* gpio,
                                              std::optional<const pwm_protocol_t*> pwm,
                                              zx_duration_t pwm_period = 170'625) {
    fbl::AllocChecker ac;
    auto device = fbl::make_unique_checked<FakeAmlLight>(&ac);
    if (!ac.check()) {
      zxlogf(ERROR, "%s: device object alloc failed", __func__);
      return nullptr;
    }
    device->lights_.emplace_back(
        "test", ddk::GpioProtocolClient(gpio),
        pwm.has_value() ? std::optional<ddk::PwmProtocolClient>(*pwm) : std::nullopt,
        zx::duration(pwm_period));
    EXPECT_OK(device->lights_.back().Init(true));
    return device;
  }

  explicit FakeAmlLight() : AmlLight(nullptr) {}

  zx_status_t Connect(async_dispatcher_t* dispatcher,
                      fidl::ServerEnd<fuchsia_hardware_light::Light> request) {
    return fidl::BindSingleInFlightOnly(dispatcher, std::move(request), this);
  }
};

namespace {

class AmlLightTest : public zxtest::Test {
 public:
  void Init() {
    loop_ = std::make_unique<async::Loop>(&kAsyncLoopConfigAttachToCurrentThread);

    zx::result server = fidl::CreateEndpoints(&client_);
    ASSERT_OK(server);
    ASSERT_OK(loop_->StartThread("aml-light-test-loop"));
    ASSERT_OK(light_->Connect(loop_->dispatcher(), std::move(server.value())));
  }

  void TearDown() override {
    gpio_.VerifyAndClear();
    pwm_.VerifyAndClear();

    loop_->Quit();
    loop_->JoinThreads();
  }

 protected:
  friend class FakeAmlLight;

  std::unique_ptr<FakeAmlLight> light_;

  ddk::MockGpio gpio_;
  ddk::MockPwm pwm_;

  fidl::ClientEnd<fuchsia_hardware_light::Light> client_;

 private:
  std::unique_ptr<async::Loop> loop_;
};

TEST_F(AmlLightTest, GetInfoTest1) {
  pwm_.ExpectEnable(ZX_OK);
  aml_pwm::mode_config regular = {aml_pwm::ON, {}};
  pwm_config_t init_config = {false, 170625, 100.0, reinterpret_cast<uint8_t*>(&regular),
                              sizeof(regular)};
  pwm_.ExpectSetConfig(ZX_OK, init_config);

  auto gpio = gpio_.GetProto();
  auto pwm = pwm_.GetProto();
  light_ = FakeAmlLight::Create(gpio, pwm);
  ASSERT_NOT_NULL(light_);
  Init();

  fidl::WireSyncClient<fuchsia_hardware_light::Light> client(std::move(client_));
  auto result = client->GetInfo(0);
  EXPECT_OK(result.status());
  EXPECT_FALSE(result->is_error());
  EXPECT_EQ(strcmp(result->value()->info.name.begin(), "test"), 0);
  EXPECT_EQ(result->value()->info.capability, Capability::kBrightness);
}

TEST_F(AmlLightTest, GetInfoTest2) {
  gpio_.ExpectWrite(ZX_OK, true);

  auto gpio = gpio_.GetProto();
  light_ = FakeAmlLight::Create(gpio, std::nullopt);
  ASSERT_NOT_NULL(light_);
  Init();

  fidl::WireSyncClient<fuchsia_hardware_light::Light> client(std::move(client_));
  auto result = client->GetInfo(0);
  EXPECT_OK(result.status());
  EXPECT_FALSE(result->is_error());
  EXPECT_EQ(strcmp(result->value()->info.name.begin(), "test"), 0);
  EXPECT_EQ(result->value()->info.capability, Capability::kSimple);
}

TEST_F(AmlLightTest, SetValueTest1) {
  gpio_.ExpectWrite(ZX_OK, true);

  auto gpio = gpio_.GetProto();
  light_ = FakeAmlLight::Create(gpio, std::nullopt);
  ASSERT_NOT_NULL(light_);
  Init();

  fidl::WireSyncClient<fuchsia_hardware_light::Light> client(std::move(client_));
  {
    auto get_result = client->GetCurrentSimpleValue(0);
    EXPECT_OK(get_result.status());
    EXPECT_FALSE(get_result->is_error());
    EXPECT_EQ(get_result->value()->value, true);
  }
  {
    auto get_result = client->GetCurrentSimpleValue(0);
    EXPECT_OK(get_result.status());
    EXPECT_FALSE(get_result->is_error());
    EXPECT_EQ(get_result->value()->value, true);
  }
  {
    gpio_.ExpectWrite(ZX_OK, false);
    auto set_result = client->SetSimpleValue(0, false);
    EXPECT_OK(set_result.status());
    EXPECT_FALSE(set_result->is_error());
  }
  {
    auto get_result = client->GetCurrentSimpleValue(0);
    EXPECT_OK(get_result.status());
    EXPECT_FALSE(get_result->is_error());
    EXPECT_EQ(get_result->value()->value, false);
  }
  {
    gpio_.ExpectWrite(ZX_OK, true);
    auto set_result = client->SetSimpleValue(0, true);
    EXPECT_OK(set_result.status());
    EXPECT_FALSE(set_result->is_error());
  }
  {
    gpio_.ExpectWrite(ZX_OK, true);
    auto set_result = client->SetSimpleValue(0, true);
    EXPECT_OK(set_result.status());
    EXPECT_FALSE(set_result->is_error());
  }
  {
    auto get_result = client->GetCurrentSimpleValue(0);
    EXPECT_OK(get_result.status());
    EXPECT_FALSE(get_result->is_error());
    EXPECT_EQ(get_result->value()->value, true);
  }
}

TEST_F(AmlLightTest, SetValueTest2) {
  pwm_.ExpectEnable(ZX_OK);
  aml_pwm::mode_config regular = {aml_pwm::ON, {}};
  pwm_config_t config = {false, 170625, 100.0, reinterpret_cast<uint8_t*>(&regular),
                         sizeof(regular)};
  pwm_.ExpectSetConfig(ZX_OK, config);

  auto gpio = gpio_.GetProto();
  auto pwm = pwm_.GetProto();
  light_ = FakeAmlLight::Create(gpio, pwm);
  ASSERT_NOT_NULL(light_);
  Init();

  fidl::WireSyncClient<fuchsia_hardware_light::Light> client(std::move(client_));
  {
    auto get_result = client->GetCurrentBrightnessValue(0);
    EXPECT_OK(get_result.status());
    EXPECT_FALSE(get_result->is_error());
    EXPECT_EQ(get_result->value()->value, 1.0);
  }
  {
    auto get_result = client->GetCurrentBrightnessValue(0);
    EXPECT_OK(get_result.status());
    EXPECT_FALSE(get_result->is_error());
    EXPECT_EQ(get_result->value()->value, 1.0);
  }
  {
    config.duty_cycle = 0;
    pwm_.ExpectSetConfig(ZX_OK, config);
    auto set_result = client->SetBrightnessValue(0, 0.0);
    EXPECT_OK(set_result.status());
    EXPECT_FALSE(set_result->is_error());
  }
  {
    auto get_result = client->GetCurrentBrightnessValue(0);
    EXPECT_OK(get_result.status());
    EXPECT_FALSE(get_result->is_error());
    EXPECT_EQ(get_result->value()->value, 0.0);
  }
  {
    config.duty_cycle = 20.0;
    pwm_.ExpectSetConfig(ZX_OK, config);
    auto set_result = client->SetBrightnessValue(0, 0.2);
    EXPECT_OK(set_result.status());
    EXPECT_FALSE(set_result->is_error());
  }
  {
    pwm_.ExpectSetConfig(ZX_OK, config);
    auto set_result = client->SetBrightnessValue(0, 0.2);
    EXPECT_OK(set_result.status());
    EXPECT_FALSE(set_result->is_error());
  }
  {
    auto get_result = client->GetCurrentBrightnessValue(0);
    EXPECT_OK(get_result.status());
    EXPECT_FALSE(get_result->is_error());
    EXPECT_EQ(get_result->value()->value, 0.2);
  }
}

TEST_F(AmlLightTest, SetInvalidValueTest) {
  pwm_.ExpectEnable(ZX_OK);
  aml_pwm::mode_config regular = {aml_pwm::ON, {}};
  pwm_config_t config = {false, 170625, 100.0, reinterpret_cast<uint8_t*>(&regular),
                         sizeof(regular)};
  pwm_.ExpectSetConfig(ZX_OK, config);

  auto gpio = gpio_.GetProto();
  auto pwm = pwm_.GetProto();
  light_ = FakeAmlLight::Create(gpio, pwm);
  ASSERT_NOT_NULL(light_);
  Init();

  fidl::WireSyncClient<fuchsia_hardware_light::Light> client(std::move(client_));
  {
    auto get_result = client->GetCurrentBrightnessValue(0);
    EXPECT_OK(get_result.status());
    EXPECT_FALSE(get_result->is_error());
    EXPECT_EQ(get_result->value()->value, 1.0);
  }
  {
    auto set_result = client->SetBrightnessValue(0, 3.2);
    EXPECT_OK(set_result.status());
    EXPECT_TRUE(set_result->is_error());
  }
  {
    auto get_result = client->GetCurrentBrightnessValue(0);
    EXPECT_OK(get_result.status());
    EXPECT_FALSE(get_result->is_error());
    EXPECT_EQ(get_result->value()->value, 1.0);
  }
  {
    auto set_result = client->SetBrightnessValue(0, -0.225);
    EXPECT_OK(set_result.status());
    EXPECT_TRUE(set_result->is_error());
  }
  {
    auto set_result = client->SetBrightnessValue(0, NAN);
    EXPECT_OK(set_result.status());
    EXPECT_TRUE(set_result->is_error());
  }
  {
    auto get_result = client->GetCurrentBrightnessValue(0);
    EXPECT_OK(get_result.status());
    EXPECT_FALSE(get_result->is_error());
    EXPECT_EQ(get_result->value()->value, 1.0);
  }
}

TEST_F(AmlLightTest, SetValueTestNelson) {
  pwm_.ExpectEnable(ZX_OK);
  aml_pwm::mode_config regular = {aml_pwm::ON, {}};
  pwm_config_t config = {false, 500'000, 100.0, reinterpret_cast<uint8_t*>(&regular),
                         sizeof(regular)};
  pwm_.ExpectSetConfig(ZX_OK, config);

  auto gpio = gpio_.GetProto();
  auto pwm = pwm_.GetProto();
  light_ = FakeAmlLight::Create(gpio, pwm, 500'000);
  ASSERT_NOT_NULL(light_);
  Init();

  fidl::WireSyncClient<fuchsia_hardware_light::Light> client(std::move(client_));

  {
    config.duty_cycle = 0;
    pwm_.ExpectSetConfig(ZX_OK, config);
    auto set_result = client->SetBrightnessValue(0, 0.0);
    EXPECT_OK(set_result.status());
    EXPECT_FALSE(set_result->is_error());
  }
  {
    config.duty_cycle = 20.0;
    pwm_.ExpectSetConfig(ZX_OK, config);
    auto set_result = client->SetBrightnessValue(0, 0.2);
    EXPECT_OK(set_result.status());
    EXPECT_FALSE(set_result->is_error());
  }
  {
    config.duty_cycle = 100.0;
    pwm_.ExpectSetConfig(ZX_OK, config);
    auto set_result = client->SetBrightnessValue(0, 1.0);
    EXPECT_OK(set_result.status());
    EXPECT_FALSE(set_result->is_error());
  }
}

}  // namespace

}  // namespace aml_light
