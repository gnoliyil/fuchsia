// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpio.h"

#include <fuchsia/hardware/gpioimpl/cpp/banjo-mock.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/metadata.h>
#include <lib/fidl-async/cpp/bind.h>

#include <ddk/metadata/gpio.h>
#include <fbl/alloc_checker.h>

#include "src/devices/testing/mock-ddk/mock-device.h"

namespace gpio {

class GpioDeviceWrapper {
 public:
  zx::result<fidl::ClientEnd<fuchsia_hardware_gpio::Gpio>> Connect(async_dispatcher_t* dispatcher) {
    zx::result endpoints = fidl::CreateEndpoints<fuchsia_hardware_gpio::Gpio>();
    if (endpoints.is_error()) {
      return endpoints.take_error();
    }
    zx_status_t status =
        fidl::BindSingleInFlightOnly<fidl::WireServer<fuchsia_hardware_gpio::Gpio>>(
            dispatcher, std::move(endpoints->server), &device_);
    if (status != ZX_OK) {
      return zx::error(status);
    }
    return zx::ok(std::move(endpoints->client));
  }

  explicit GpioDeviceWrapper()
      : device_(nullptr, const_cast<gpio_impl_protocol_t*>(gpio_impl_.GetProto()), 0, "GPIO_0") {}

  ddk::MockGpioImpl gpio_impl_;
  GpioDevice device_;
};

class GpioTest : public zxtest::Test {
 public:
  void SetUp() override {
    zx::result gpio_client = gpio_.Connect(loop.dispatcher());
    ASSERT_TRUE(gpio_client.is_ok());
    gpio_client_.Bind(std::move(*gpio_client), loop.dispatcher());
  }

  void TearDown() override { gpio_.gpio_impl_.VerifyAndClear(); }

 protected:
  async::Loop loop{&kAsyncLoopConfigAttachToCurrentThread};
  GpioDeviceWrapper gpio_;
  fidl::WireClient<fuchsia_hardware_gpio::Gpio> gpio_client_;
};

TEST_F(GpioTest, TestFidlAll) {
  gpio_.gpio_impl_.ExpectRead(ZX_OK, 0, 20);
  gpio_client_->Read().ThenExactlyOnce(
      [](fidl::WireUnownedResult<fuchsia_hardware_gpio::Gpio::Read>& result) {
        EXPECT_OK(result.status());
        EXPECT_EQ(result->value()->value, 20);
      });
  ASSERT_OK(loop.RunUntilIdle());

  gpio_.gpio_impl_.ExpectWrite(ZX_OK, 0, 11);
  gpio_client_->Write(11).ThenExactlyOnce(
      [](fidl::WireUnownedResult<fuchsia_hardware_gpio::Gpio::Write>& result) {
        EXPECT_OK(result.status());
      });
  ASSERT_OK(loop.RunUntilIdle());

  gpio_.gpio_impl_.ExpectConfigIn(ZX_OK, 0, 0);
  gpio_client_->ConfigIn(fuchsia_hardware_gpio::wire::GpioFlags::kPullDown)
      .ThenExactlyOnce([](fidl::WireUnownedResult<fuchsia_hardware_gpio::Gpio::ConfigIn>& result) {
        EXPECT_OK(result.status());
      });
  ASSERT_OK(loop.RunUntilIdle());

  gpio_.gpio_impl_.ExpectConfigOut(ZX_OK, 0, 5);
  gpio_client_->ConfigOut(5).ThenExactlyOnce(
      [](fidl::WireUnownedResult<fuchsia_hardware_gpio::Gpio::ConfigOut>& result) {
        EXPECT_OK(result.status());
      });
  ASSERT_OK(loop.RunUntilIdle());

  gpio_.gpio_impl_.ExpectSetDriveStrength(ZX_OK, 0, 2000, 2000);
  gpio_client_->SetDriveStrength(2000).ThenExactlyOnce(
      [](fidl::WireUnownedResult<fuchsia_hardware_gpio::Gpio::SetDriveStrength>& result) {
        EXPECT_OK(result.status());
        EXPECT_EQ(result->value()->actual_ds_ua, 2000);
      });
  ASSERT_OK(loop.RunUntilIdle());

  gpio_.gpio_impl_.ExpectGetDriveStrength(ZX_OK, 0, 2000);
  gpio_client_->GetDriveStrength().ThenExactlyOnce(
      [](fidl::WireUnownedResult<fuchsia_hardware_gpio::Gpio::GetDriveStrength>& result) {
        EXPECT_OK(result.status());
        EXPECT_EQ(result->value()->result_ua, 2000);
      });
  ASSERT_OK(loop.RunUntilIdle());
}

TEST_F(GpioTest, TestBanjoSetDriveStrength) {
  uint64_t actual = 0;
  gpio_.gpio_impl_.ExpectSetDriveStrength(ZX_OK, 0, 3000, 3000);
  EXPECT_OK(gpio_.device_.GpioSetDriveStrength(3000, &actual));
  EXPECT_EQ(actual, 3000);
}

TEST_F(GpioTest, TestBanjoGetDriveStrength) {
  uint64_t result = 0;
  gpio_.gpio_impl_.ExpectGetDriveStrength(ZX_OK, 0, 3000);
  EXPECT_OK(gpio_.device_.GpioGetDriveStrength(&result));
  EXPECT_EQ(result, 3000);
}

TEST_F(GpioTest, ValidateMetadataOk) {
  constexpr gpio_pin_t pins[] = {
      DECL_GPIO_PIN(0),
      DECL_GPIO_PIN(1),
      DECL_GPIO_PIN(2),
  };

  auto parent = MockDevice::FakeRootParent();

  ddk::MockGpioImpl gpio_impl;
  parent->AddProtocol(ZX_PROTOCOL_GPIO_IMPL, gpio_impl.GetProto()->ops, gpio_impl.GetProto()->ctx);
  parent->SetMetadata(DEVICE_METADATA_GPIO_PINS, pins, std::size(pins) * sizeof(gpio_pin_t));

  ASSERT_OK(GpioDevice::Create(nullptr, parent.get()));
}

TEST_F(GpioTest, ValidateMetadataRejectDuplicates) {
  constexpr gpio_pin_t pins[] = {
      DECL_GPIO_PIN(2),
      DECL_GPIO_PIN(1),
      DECL_GPIO_PIN(2),
      DECL_GPIO_PIN(0),
  };

  ddk::MockGpioImpl gpio_impl;
  auto parent = MockDevice::FakeRootParent();

  parent->AddProtocol(ZX_PROTOCOL_GPIO_IMPL, gpio_impl.GetProto()->ops, gpio_impl.GetProto()->ctx);
  parent->SetMetadata(DEVICE_METADATA_GPIO_PINS, pins, std::size(pins) * sizeof(gpio_pin_t));

  ASSERT_NOT_OK(GpioDevice::Create(nullptr, parent.get()));
}

TEST_F(GpioTest, ValidateGpioNameGeneration) {
  constexpr gpio_pin_t pins_digit[] = {
      DECL_GPIO_PIN(2),
      DECL_GPIO_PIN(5),
      DECL_GPIO_PIN((11)),
  };
  EXPECT_EQ(pins_digit[0].pin, 2);
  EXPECT_STREQ(pins_digit[0].name, "2");
  EXPECT_EQ(pins_digit[1].pin, 5);
  EXPECT_STREQ(pins_digit[1].name, "5");
  EXPECT_EQ(pins_digit[2].pin, 11);
  EXPECT_STREQ(pins_digit[2].name, "(11)");

#define GPIO_TEST_NAME1 5
#define GPIO_TEST_NAME2 (6)
#define GPIO_TEST_NAME3_OF_63_CHRS_ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890 7
  constexpr uint32_t GPIO_TEST_NAME4 = 8;  // constexpr should work too
#define GEN_GPIO0(x) ((x) + 1)
#define GEN_GPIO1(x) ((x) + 2)
  constexpr gpio_pin_t pins[] = {
      DECL_GPIO_PIN(GPIO_TEST_NAME1),
      DECL_GPIO_PIN(GPIO_TEST_NAME2),
      DECL_GPIO_PIN(GPIO_TEST_NAME3_OF_63_CHRS_ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890),
      DECL_GPIO_PIN(GPIO_TEST_NAME4),
      DECL_GPIO_PIN(GEN_GPIO0(9)),
      DECL_GPIO_PIN(GEN_GPIO1(18)),
  };
  EXPECT_EQ(pins[0].pin, 5);
  EXPECT_STREQ(pins[0].name, "GPIO_TEST_NAME1");
  EXPECT_EQ(pins[1].pin, 6);
  EXPECT_STREQ(pins[1].name, "GPIO_TEST_NAME2");
  EXPECT_EQ(pins[2].pin, 7);
  EXPECT_STREQ(pins[2].name, "GPIO_TEST_NAME3_OF_63_CHRS_ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890");
  EXPECT_EQ(strlen(pins[2].name), GPIO_NAME_MAX_LENGTH - 1);
  EXPECT_EQ(pins[3].pin, 8);
  EXPECT_STREQ(pins[3].name, "GPIO_TEST_NAME4");
  EXPECT_EQ(pins[4].pin, 10);
  EXPECT_STREQ(pins[4].name, "GEN_GPIO0(9)");
  EXPECT_EQ(pins[5].pin, 20);
  EXPECT_STREQ(pins[5].name, "GEN_GPIO1(18)");
#undef GPIO_TEST_NAME1
#undef GPIO_TEST_NAME2
#undef GPIO_TEST_NAME3_OF_63_CHRS_ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890
#undef GEN_GPIO0
#undef GEN_GPIO1
}

TEST_F(GpioTest, Init) {
  constexpr gpio_pin_t kGpioPins[] = {
      DECL_GPIO_PIN(1),
      DECL_GPIO_PIN(2),
      DECL_GPIO_PIN(3),
  };

  std::shared_ptr<zx_device> fake_root = MockDevice::FakeRootParent();

  ddk::MockGpioImpl gpio;

  fake_root->AddProtocol(ZX_PROTOCOL_GPIO_IMPL, gpio.GetProto()->ops, gpio.GetProto()->ctx);

  fidl::Arena arena;

  fuchsia_hardware_gpio_init::wire::GpioInitMetadata metadata;
  metadata.steps = fidl::VectorView<fuchsia_hardware_gpio_init::wire::GpioInitStep>(arena, 10);

  metadata.steps[0].index = 1;
  metadata.steps[0].options = fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena)
                                  .input_flags(fuchsia_hardware_gpio::GpioFlags::kPullDown)
                                  .output_value(1)
                                  .drive_strength_ua(4000)
                                  .Build();
  gpio.ExpectConfigIn(ZX_OK, 1, GPIO_PULL_DOWN)
      .ExpectConfigOut(ZX_OK, 1, 1)
      .ExpectSetDriveStrength(ZX_OK, 1, 4000, 4000);

  metadata.steps[1].index = 2;
  metadata.steps[1].options = fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena)
                                  .input_flags(fuchsia_hardware_gpio::GpioFlags::kNoPull)
                                  .alt_function(5)
                                  .drive_strength_ua(2000)
                                  .Build();
  gpio.ExpectConfigIn(ZX_OK, 2, GPIO_NO_PULL)
      .ExpectSetAltFunction(ZX_OK, 2, 5)
      .ExpectSetDriveStrength(ZX_OK, 2, 2000, 2000);

  metadata.steps[2].index = 3;
  metadata.steps[2].options =
      fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena).output_value(0).Build();
  gpio.ExpectConfigOut(ZX_OK, 3, 0);

  metadata.steps[3].index = 3;
  metadata.steps[3].options =
      fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena).output_value(1).Build();
  gpio.ExpectConfigOut(ZX_OK, 3, 1);

  metadata.steps[4].index = 3;
  metadata.steps[4].options = fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena)
                                  .input_flags(fuchsia_hardware_gpio::GpioFlags::kPullUp)
                                  .Build();
  gpio.ExpectConfigIn(ZX_OK, 3, GPIO_PULL_UP);

  metadata.steps[5].index = 2;
  metadata.steps[5].options = fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena)
                                  .alt_function(0)
                                  .drive_strength_ua(1000)
                                  .Build();
  gpio.ExpectSetAltFunction(ZX_OK, 2, 0).ExpectSetDriveStrength(ZX_OK, 2, 1000, 1000);

  metadata.steps[6].index = 2;
  metadata.steps[6].options =
      fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena).output_value(1).Build();
  gpio.ExpectConfigOut(ZX_OK, 2, 1);

  metadata.steps[7].index = 1;
  metadata.steps[7].options = fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena)
                                  .input_flags(fuchsia_hardware_gpio::GpioFlags::kPullUp)
                                  .alt_function(0)
                                  .drive_strength_ua(4000)
                                  .Build();
  gpio.ExpectConfigIn(ZX_OK, 1, GPIO_PULL_UP)
      .ExpectSetAltFunction(ZX_OK, 1, 0)
      .ExpectSetDriveStrength(ZX_OK, 1, 4000, 4000);

  metadata.steps[8].index = 1;
  metadata.steps[8].options =
      fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena).output_value(1).Build();
  gpio.ExpectConfigOut(ZX_OK, 1, 1);

  metadata.steps[9].index = 3;
  metadata.steps[9].options = fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena)
                                  .alt_function(3)
                                  .drive_strength_ua(2000)
                                  .Build();
  gpio.ExpectSetAltFunction(ZX_OK, 3, 3).ExpectSetDriveStrength(ZX_OK, 3, 2000, 2000);

  fit::result encoded = fidl::Persist(metadata);
  ASSERT_TRUE(encoded.is_ok(), "%s", encoded.error_value().FormatDescription().c_str());

  std::vector<uint8_t>& message = encoded.value();
  fake_root->SetMetadata(DEVICE_METADATA_GPIO_INIT_STEPS, message.data(), message.size());
  fake_root->SetMetadata(DEVICE_METADATA_GPIO_PINS, kGpioPins, sizeof(kGpioPins));

  EXPECT_OK(GpioDevice::Create(nullptr, fake_root.get()));

  // GPIO init device and three GPIO pin devices.
  EXPECT_EQ(fake_root->child_count(), 4);
  device_async_remove(fake_root.get());
  mock_ddk::ReleaseFlaggedDevices(fake_root.get());

  EXPECT_NO_FAILURES(gpio.VerifyAndClear());
}

TEST_F(GpioTest, InitErrorHandling) {
  constexpr gpio_pin_t kGpioPins[] = {
      DECL_GPIO_PIN(1),
      DECL_GPIO_PIN(2),
      DECL_GPIO_PIN(3),
  };

  std::shared_ptr<zx_device> fake_root = MockDevice::FakeRootParent();

  ddk::MockGpioImpl gpio;

  fake_root->AddProtocol(ZX_PROTOCOL_GPIO_IMPL, gpio.GetProto()->ops, gpio.GetProto()->ctx);

  fidl::Arena arena;

  fuchsia_hardware_gpio_init::wire::GpioInitMetadata metadata;
  metadata.steps = fidl::VectorView<fuchsia_hardware_gpio_init::wire::GpioInitStep>(arena, 4);

  metadata.steps[0].index = 4;
  metadata.steps[0].options = fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena)
                                  .input_flags(fuchsia_hardware_gpio::GpioFlags::kPullDown)
                                  .output_value(1)
                                  .drive_strength_ua(4000)
                                  .Build();
  gpio.ExpectConfigIn(ZX_OK, 4, GPIO_PULL_DOWN)
      .ExpectConfigOut(ZX_OK, 4, 1)
      .ExpectSetDriveStrength(ZX_OK, 4, 4000, 4000);

  metadata.steps[1].index = 2;
  metadata.steps[1].options = fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena)
                                  .input_flags(fuchsia_hardware_gpio::GpioFlags::kNoPull)
                                  .alt_function(5)
                                  .drive_strength_ua(2000)
                                  .Build();
  gpio.ExpectConfigIn(ZX_OK, 2, GPIO_NO_PULL)
      .ExpectSetAltFunction(ZX_OK, 2, 5)
      .ExpectSetDriveStrength(ZX_OK, 2, 2000, 2000);

  metadata.steps[2].index = 3;
  metadata.steps[2].options =
      fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena).output_value(0).Build();
  gpio.ExpectConfigOut(ZX_ERR_NOT_FOUND, 3, 0);

  // Processing should continue after the above error.

  metadata.steps[3].index = 2;
  metadata.steps[3].options = fuchsia_hardware_gpio_init::wire::GpioInitOptions::Builder(arena)
                                  .alt_function(0)
                                  .drive_strength_ua(1000)
                                  .Build();
  gpio.ExpectSetAltFunction(ZX_OK, 2, 0).ExpectSetDriveStrength(ZX_OK, 2, 1000, 1000);

  fit::result encoded = fidl::Persist(metadata);
  ASSERT_TRUE(encoded.is_ok(), "%s", encoded.error_value().FormatDescription().c_str());

  std::vector<uint8_t>& message = encoded.value();
  fake_root->SetMetadata(DEVICE_METADATA_GPIO_INIT_STEPS, message.data(), message.size());
  fake_root->SetMetadata(DEVICE_METADATA_GPIO_PINS, kGpioPins, sizeof(kGpioPins));

  EXPECT_OK(GpioDevice::Create(nullptr, fake_root.get()));

  // Three GPIO pin devices (GPIO init device should not be added due to errors).
  EXPECT_EQ(fake_root->child_count(), 3);
  device_async_remove(fake_root.get());
  mock_ddk::ReleaseFlaggedDevices(fake_root.get());

  EXPECT_NO_FAILURES(gpio.VerifyAndClear());
}

}  // namespace gpio
