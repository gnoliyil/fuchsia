// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ti-lp8556.h"

#include <fidl/fuchsia.hardware.adhoc.lp8556/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async_patterns/testing/cpp/dispatcher_bound.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/inspect/cpp/reader.h>
#include <lib/mock-i2c/mock-i2c.h>
#include <lib/stdcompat/span.h>
#include <math.h>

#include <map>

#include <mock-mmio-reg/mock-mmio-reg.h>
#include <zxtest/zxtest.h>

#include "sdk/lib/inspect/testing/cpp/zxtest/inspect.h"
#include "src/devices/bus/testing/fake-pdev/fake-pdev.h"
#include "src/devices/testing/mock-ddk/mock-device.h"

namespace {

bool FloatNear(double a, double b) { return std::abs(a - b) < 0.001; }

}  // namespace

namespace ti {

constexpr uint32_t kMmioRegSize = sizeof(uint32_t);
constexpr uint32_t kMmioRegCount = (kAOBrightnessStickyReg + kMmioRegSize) / kMmioRegSize;

class Lp8556DeviceTest : public zxtest::Test, public inspect::InspectTestHelper {
 public:
  Lp8556DeviceTest()
      : mock_regs_(ddk_mock::MockMmioRegRegion(mock_reg_array_, kMmioRegSize, kMmioRegCount)),
        fake_parent_(MockDevice::FakeRootParent()),
        loop_(&kAsyncLoopConfigNeverAttachToThread),
        i2c_loop_(&kAsyncLoopConfigNeverAttachToThread) {}

  void SetUp() override {
    fdf::MmioBuffer mmio(mock_regs_.GetMmioBuffer());

    auto i2c_endpoints = fidl::CreateEndpoints<fuchsia_hardware_i2c::Device>();
    fidl::BindServer(i2c_loop_.dispatcher(), std::move(i2c_endpoints->server), &mock_i2c_);

    fbl::AllocChecker ac;
    dev_ = fbl::make_unique_checked<Lp8556Device>(
        &ac, fake_parent_.get(), std::move(i2c_endpoints->client), std::move(mmio));
    ASSERT_TRUE(ac.check());

    zx::result server = fidl::CreateEndpoints(&client_);
    ASSERT_OK(server);
    fidl::BindServer(loop_.dispatcher(), std::move(server.value()), dev_.get());

    ASSERT_OK(loop_.StartThread("lp8556-client-thread"));
    ASSERT_OK(i2c_loop_.StartThread("mock-i2c-driver-thread"));
  }

  void TestLifecycle() {
    EXPECT_OK(dev_->DdkAdd("ti-lp8556"));
    EXPECT_EQ(fake_parent_->child_count(), 1);
    dev_->DdkAsyncRemove();
    EXPECT_OK(mock_ddk::ReleaseFlaggedDevices(fake_parent_.get()));  // Calls DdkRelease() on dev_.
    [[maybe_unused]] auto ptr = dev_.release();
    EXPECT_EQ(fake_parent_->child_count(), 0);
  }

  void VerifyGetBrightness(bool power, double brightness) {
    bool pwr;
    double brt;
    EXPECT_OK(dev_->GetBacklightState(&pwr, &brt));
    EXPECT_EQ(pwr, power);
    EXPECT_EQ(brt, brightness);
  }

  void VerifySetBrightness(bool power, double brightness) {
    if (brightness != dev_->GetDeviceBrightness()) {
      uint16_t brightness_reg_value =
          static_cast<uint16_t>(ceil(brightness * kBrightnessRegMaxValue));
      mock_i2c_.ExpectWriteStop({kBacklightBrightnessLsbReg,
                                 static_cast<uint8_t>(brightness_reg_value & kBrightnessLsbMask)});
      // An I2C bus read is a write of the address followed by a read of the data.
      mock_i2c_.ExpectWrite({kBacklightBrightnessMsbReg}).ExpectReadStop({0});
      mock_i2c_.ExpectWriteStop(
          {kBacklightBrightnessMsbReg,
           static_cast<uint8_t>(
               ((brightness_reg_value & kBrightnessMsbMask) >> kBrightnessMsbShift) &
               kBrightnessMsbByteMask)});

      auto sticky_reg = BrightnessStickyReg::Get().FromValue(0);
      sticky_reg.set_brightness(brightness_reg_value & kBrightnessRegMask);
      sticky_reg.set_is_valid(1);

      mock_regs_[BrightnessStickyReg::Get().addr()].ExpectWrite(sticky_reg.reg_value());
    }

    if (power != dev_->GetDevicePower()) {
      const uint8_t control_value = kDeviceControlDefaultValue | (power ? kBacklightOn : 0);
      mock_i2c_.ExpectWriteStop({kDeviceControlReg, control_value});
      if (power) {
        mock_i2c_.ExpectWriteStop({kCfg2Reg, dev_->GetCfg2()});
      }
    }
    EXPECT_OK(dev_->SetBacklightState(power, brightness));

    ASSERT_NO_FATAL_FAILURE(mock_regs_[BrightnessStickyReg::Get().addr()].VerifyAndClear());
    ASSERT_NO_FATAL_FAILURE(mock_i2c_.VerifyAndClear());
  }

 protected:
  const fidl::ClientEnd<fuchsia_hardware_adhoc_lp8556::Device>& client() const { return client_; }

  mock_i2c::MockI2c mock_i2c_;
  std::unique_ptr<Lp8556Device> dev_;
  ddk_mock::MockMmioRegRegion mock_regs_;
  std::shared_ptr<MockDevice> fake_parent_;

 private:
  ddk_mock::MockMmioReg mock_reg_array_[kMmioRegCount];
  fidl::ClientEnd<fuchsia_hardware_adhoc_lp8556::Device> client_;
  async::Loop loop_;
  async::Loop i2c_loop_;
};

TEST_F(Lp8556DeviceTest, DdkLifecycle) { TestLifecycle(); }

TEST_F(Lp8556DeviceTest, Brightness) {
  VerifySetBrightness(false, 0.0);
  VerifyGetBrightness(false, 0.0);

  VerifySetBrightness(true, 0.5);
  VerifyGetBrightness(true, 0.5);

  VerifySetBrightness(true, 1.0);
  VerifyGetBrightness(true, 1.0);

  VerifySetBrightness(true, 0.0);
  VerifyGetBrightness(true, 0.0);
}

TEST_F(Lp8556DeviceTest, InitRegisters) {
  TiLp8556Metadata kDeviceMetadata = {
      .panel_id = 0,
      .registers =
          {
              // Registers
              0x01, 0x85,  // Device Control
                           // EPROM
              0xa2, 0x30,  // CFG2
              0xa3, 0x32,  // CFG3
              0xa5, 0x54,  // CFG5
              0xa7, 0xf4,  // CFG7
              0xa9, 0x60,  // CFG9
              0xae, 0x09,  // CFGE
          },
      .register_count = 14,
  };
  // constexpr uint8_t kInitialRegisterValues[] = {
  //     0x01, 0x85, 0xa2, 0x30, 0xa3, 0x32, 0xa5, 0x54, 0xa7, 0xf4, 0xa9, 0x60, 0xae, 0x09,
  // };

  fake_parent_->SetMetadata(DEVICE_METADATA_PRIVATE, &kDeviceMetadata, sizeof(kDeviceMetadata));

  mock_i2c_.ExpectWriteStop({0x01, 0x85})
      .ExpectWriteStop({0xa2, 0x30})
      .ExpectWriteStop({0xa3, 0x32})
      .ExpectWriteStop({0xa5, 0x54})
      .ExpectWriteStop({0xa7, 0xf4})
      .ExpectWriteStop({0xa9, 0x60})
      .ExpectWriteStop({0xae, 0x09})
      .ExpectWrite({kCfg2Reg})
      .ExpectReadStop({kCfg2Default})
      .ExpectWrite({kCurrentLsbReg})
      .ExpectReadStop({0x05, 0x4e})
      .ExpectWrite({kBacklightBrightnessLsbReg})
      .ExpectReadStop({0xab, 0x05})
      .ExpectWrite({kDeviceControlReg})
      .ExpectReadStop({0x85})
      .ExpectWrite({kCfgReg})
      .ExpectReadStop({0x01});
  mock_regs_[BrightnessStickyReg::Get().addr()].ExpectRead();

  EXPECT_OK(dev_->Init());

  ASSERT_NO_FATAL_FAILURE(mock_regs_[BrightnessStickyReg::Get().addr()].VerifyAndClear());
  ASSERT_NO_FATAL_FAILURE(mock_i2c_.VerifyAndClear());
}

TEST_F(Lp8556DeviceTest, InitNoRegisters) {
  mock_i2c_.ExpectWrite({kCfg2Reg})
      .ExpectReadStop({kCfg2Default})
      .ExpectWrite({kCurrentLsbReg})
      .ExpectReadStop({0x05, 0x4e})
      .ExpectWrite({kBacklightBrightnessLsbReg})
      .ExpectReadStop({0xab, 0x05})
      .ExpectWrite({kDeviceControlReg})
      .ExpectReadStop({0x85})
      .ExpectWrite({kCfgReg})
      .ExpectReadStop({0x01});
  mock_regs_[BrightnessStickyReg::Get().addr()].ExpectRead();

  EXPECT_OK(dev_->Init());

  ASSERT_NO_FATAL_FAILURE(mock_regs_[BrightnessStickyReg::Get().addr()].VerifyAndClear());
  ASSERT_NO_FATAL_FAILURE(mock_i2c_.VerifyAndClear());
}

TEST_F(Lp8556DeviceTest, InitInvalidRegisters) {
  constexpr uint8_t kInitialRegisterValues[] = {
      0x01, 0x85, 0xa2, 0x30, 0xa3, 0x32, 0xa5, 0x54, 0xa7, 0xf4, 0xa9, 0x60, 0xae,
  };

  fake_parent_->AddProtocol(ZX_PROTOCOL_PDEV, nullptr, nullptr, "pdev");
  fake_parent_->SetMetadata(DEVICE_METADATA_PRIVATE, kInitialRegisterValues,
                            sizeof(kInitialRegisterValues));

  EXPECT_NOT_OK(dev_->Init());

  ASSERT_NO_FATAL_FAILURE(mock_regs_[BrightnessStickyReg::Get().addr()].VerifyAndClear());
  ASSERT_NO_FATAL_FAILURE(mock_i2c_.VerifyAndClear());
}

TEST_F(Lp8556DeviceTest, InitTooManyRegisters) {
  constexpr uint8_t kInitialRegisterValues[514] = {};

  fake_parent_->AddProtocol(ZX_PROTOCOL_PDEV, nullptr, nullptr, "pdev");
  fake_parent_->SetMetadata(DEVICE_METADATA_PRIVATE, kInitialRegisterValues,
                            sizeof(kInitialRegisterValues));

  EXPECT_NOT_OK(dev_->Init());

  ASSERT_NO_FATAL_FAILURE(mock_regs_[BrightnessStickyReg::Get().addr()].VerifyAndClear());
  ASSERT_NO_FATAL_FAILURE(mock_i2c_.VerifyAndClear());
}

TEST_F(Lp8556DeviceTest, OverwriteStickyRegister) {
  // constexpr uint8_t kInitialRegisterValues[] = {
  //     kBacklightBrightnessLsbReg,
  //     0xab,
  //     kBacklightBrightnessMsbReg,
  //     0xcd,
  // };

  TiLp8556Metadata kDeviceMetadata = {
      .panel_id = 0,
      .registers =
          {// Registers
           kBacklightBrightnessLsbReg, 0xab, kBacklightBrightnessMsbReg, 0xcd},
      .register_count = 4,
  };

  fake_parent_->AddProtocol(ZX_PROTOCOL_PDEV, nullptr, nullptr, "pdev");
  fake_parent_->SetMetadata(DEVICE_METADATA_PRIVATE, &kDeviceMetadata, sizeof(kDeviceMetadata));

  mock_i2c_.ExpectWriteStop({kBacklightBrightnessLsbReg, 0xab})
      .ExpectWriteStop({kBacklightBrightnessMsbReg, 0xcd})
      .ExpectWrite({kCfg2Reg})
      .ExpectReadStop({kCfg2Default})
      .ExpectWrite({kCurrentLsbReg})
      .ExpectReadStop({0x05, 0x4e})
      .ExpectWrite({kBacklightBrightnessLsbReg})
      .ExpectReadStop({0xab, 0xcd})
      .ExpectWrite({kDeviceControlReg})
      .ExpectReadStop({0x85})
      .ExpectWrite({kCfgReg})
      .ExpectReadStop({0x01});
  mock_regs_[BrightnessStickyReg::Get().addr()].ExpectRead();

  EXPECT_OK(dev_->Init());

  const uint32_t kStickyRegValue =
      BrightnessStickyReg::Get().FromValue(0).set_is_valid(1).set_brightness(0x400).reg_value();
  mock_regs_[BrightnessStickyReg::Get().addr()].ExpectWrite(kStickyRegValue);

  // The DUT should set the brightness to 0.25 by writing 0x0400, starting with the LSB. The MSB
  // register needs to be RMW, so check that the upper four bits are preserved (0xab -> 0xa4).
  mock_i2c_.ExpectWriteStop({kBacklightBrightnessLsbReg, 0x00})
      .ExpectWrite({kBacklightBrightnessMsbReg})
      .ExpectReadStop({0xab})
      .ExpectWriteStop({kBacklightBrightnessMsbReg, 0xa4});

  auto result = fidl::WireCall(client())->SetStateNormalized({true, 0.25});
  EXPECT_TRUE(result.ok());
  EXPECT_FALSE(result->is_error());

  ASSERT_NO_FATAL_FAILURE(mock_regs_[BrightnessStickyReg::Get().addr()].VerifyAndClear());
  ASSERT_NO_FATAL_FAILURE(mock_i2c_.VerifyAndClear());
}

TEST_F(Lp8556DeviceTest, ReadDefaultCurrentScale) {
  TiLp8556Metadata kDeviceMetadata = {
      .panel_id = 0,
      .allow_set_current_scale = true,
      .register_count = 0,
  };

  fake_parent_->AddProtocol(ZX_PROTOCOL_PDEV, nullptr, nullptr, "pdev");
  fake_parent_->SetMetadata(DEVICE_METADATA_PRIVATE, &kDeviceMetadata, sizeof(kDeviceMetadata));

  mock_i2c_.ExpectWrite({kCfg2Reg})
      .ExpectReadStop({kCfg2Default})
      .ExpectWrite({kCurrentLsbReg})
      .ExpectReadStop({0x05, 0x4e})
      .ExpectWrite({kBacklightBrightnessLsbReg})
      .ExpectReadStop({0xab, 0x05})
      .ExpectWrite({kDeviceControlReg})
      .ExpectReadStop({0x85})
      .ExpectWrite({kCfgReg})
      .ExpectReadStop({0x01});
  mock_regs_[BrightnessStickyReg::Get().addr()].ExpectRead();

  EXPECT_OK(dev_->Init());

  auto result = fidl::WireCall(client())->GetNormalizedBrightnessScale();
  ASSERT_TRUE(result.ok());
  ASSERT_TRUE(result->is_ok());
  EXPECT_TRUE(FloatNear(result->value()->scale, static_cast<double>(0xe05) / 0xfff));

  ASSERT_NO_FATAL_FAILURE(mock_regs_[BrightnessStickyReg::Get().addr()].VerifyAndClear());
  ASSERT_NO_FATAL_FAILURE(mock_i2c_.VerifyAndClear());
}

TEST_F(Lp8556DeviceTest, SetCurrentScale) {
  TiLp8556Metadata kDeviceMetadata = {
      .panel_id = 0,
      .allow_set_current_scale = true,
      .register_count = 0,
  };

  fake_parent_->AddProtocol(ZX_PROTOCOL_PDEV, nullptr, nullptr, "pdev");
  fake_parent_->SetMetadata(DEVICE_METADATA_PRIVATE, &kDeviceMetadata, sizeof(kDeviceMetadata));

  mock_i2c_.ExpectWrite({kCfg2Reg})
      .ExpectReadStop({kCfg2Default})
      .ExpectWrite({kCurrentLsbReg})
      .ExpectReadStop({0x05, 0x4e})
      .ExpectWrite({kBacklightBrightnessLsbReg})
      .ExpectReadStop({0xab, 0x05})
      .ExpectWrite({kDeviceControlReg})
      .ExpectReadStop({0x85})
      .ExpectWrite({kCfgReg})
      .ExpectReadStop({0x01});
  mock_regs_[BrightnessStickyReg::Get().addr()].ExpectRead();

  EXPECT_OK(dev_->Init());

  mock_i2c_.ExpectWrite({kCfgReg}).ExpectReadStop({0x7e}).ExpectWriteStop(
      {kCurrentLsbReg, 0xab, 0x72});

  auto set_result =
      fidl::WireCall(client())->SetNormalizedBrightnessScale(static_cast<double>(0x2ab) / 0xfff);
  ASSERT_TRUE(set_result.ok());
  EXPECT_TRUE(set_result->is_ok());

  auto get_result = fidl::WireCall(client())->GetNormalizedBrightnessScale();
  ASSERT_TRUE(get_result.ok());
  ASSERT_TRUE(get_result->is_ok());
  EXPECT_TRUE(FloatNear(get_result->value()->scale, static_cast<double>(0x2ab) / 0xfff));

  ASSERT_NO_FATAL_FAILURE(mock_regs_[BrightnessStickyReg::Get().addr()].VerifyAndClear());
  ASSERT_NO_FATAL_FAILURE(mock_i2c_.VerifyAndClear());
}

TEST_F(Lp8556DeviceTest, SetAbsoluteBrightnessScaleReset) {
  TiLp8556Metadata kDeviceMetadata = {
      .panel_id = 0,
      .allow_set_current_scale = true,
      .register_count = 0,
  };

  fake_parent_->AddProtocol(ZX_PROTOCOL_PDEV, nullptr, nullptr, "pdev");
  fake_parent_->SetMetadata(DEVICE_METADATA_PRIVATE, &kDeviceMetadata, sizeof(kDeviceMetadata));

  constexpr double kMaxBrightnessInNits = 350.0;
  fake_parent_->SetMetadata(DEVICE_METADATA_BACKLIGHT_MAX_BRIGHTNESS_NITS, &kMaxBrightnessInNits,
                            sizeof(kMaxBrightnessInNits));

  mock_i2c_.ExpectWrite({kCfg2Reg})
      .ExpectReadStop({kCfg2Default})
      .ExpectWrite({kCurrentLsbReg})
      .ExpectReadStop({0x05, 0x4e})
      .ExpectWrite({kBacklightBrightnessLsbReg})
      .ExpectReadStop({0xab, 0x05})
      .ExpectWrite({kDeviceControlReg})
      .ExpectReadStop({0x85})
      .ExpectWrite({kCfgReg})
      .ExpectReadStop({0x01});
  mock_regs_[BrightnessStickyReg::Get().addr()].ExpectRead();

  EXPECT_OK(dev_->Init());

  mock_i2c_.ExpectWrite({kCfgReg}).ExpectReadStop({0x7e}).ExpectWriteStop(
      {kCurrentLsbReg, 0xab, 0x72});

  auto set_result =
      fidl::WireCall(client())->SetNormalizedBrightnessScale(static_cast<double>(0x2ab) / 0xfff);
  EXPECT_TRUE(set_result.ok());
  EXPECT_FALSE(set_result->is_error());

  mock_i2c_.ExpectWrite({kCfgReg})
      .ExpectReadStop({0x6e})
      .ExpectWriteStop({kCurrentLsbReg, 0x05, 0x6e})
      .ExpectWriteStop({kBacklightBrightnessLsbReg, 0x00})
      .ExpectWrite({kBacklightBrightnessMsbReg})
      .ExpectReadStop({0xab})
      .ExpectWriteStop({kBacklightBrightnessMsbReg, 0xa8});

  auto absolute_result_1 = fidl::WireCall(client())->SetStateAbsolute({true, 175.0});
  EXPECT_TRUE(absolute_result_1.ok());
  EXPECT_FALSE(absolute_result_1->is_error());

  // The scale is already set to the default, so the register should not be written again.
  mock_i2c_.ExpectWriteStop({kBacklightBrightnessLsbReg, 0x00})
      .ExpectWrite({kBacklightBrightnessMsbReg})
      .ExpectReadStop({0x1b})
      .ExpectWriteStop({kBacklightBrightnessMsbReg, 0x14});

  auto absolute_result_2 = fidl::WireCall(client())->SetStateAbsolute({true, 87.5});
  EXPECT_TRUE(absolute_result_2.ok());
  EXPECT_FALSE(absolute_result_2->is_error());

  ASSERT_NO_FATAL_FAILURE(mock_regs_[BrightnessStickyReg::Get().addr()].VerifyAndClear());
  ASSERT_NO_FATAL_FAILURE(mock_i2c_.VerifyAndClear());
}

TEST_F(Lp8556DeviceTest, Inspect) {
  mock_i2c_.ExpectWrite({kCfg2Reg})
      .ExpectReadStop({kCfg2Default})
      .ExpectWrite({kCurrentLsbReg})
      .ExpectReadStop({0x05, 0x4e})
      .ExpectWrite({kBacklightBrightnessLsbReg})
      .ExpectReadStop({0xff, 0x0f})
      .ExpectWrite({kDeviceControlReg})
      .ExpectReadStop({0x85})
      .ExpectWrite({kCfgReg})
      .ExpectReadStop({0x01});
  mock_regs_[BrightnessStickyReg::Get().addr()].ExpectRead();

  EXPECT_OK(dev_->Init());

  ReadInspect(dev_->InspectVmo());
  auto& root_node = hierarchy().GetByPath({"ti-lp8556"})->node();
  CheckProperty(root_node, "brightness", inspect::DoublePropertyValue(1.0));

  EXPECT_FALSE(root_node.get_property<inspect::UintPropertyValue>("persistent_brightness"));
  CheckProperty(root_node, "scale", inspect::UintPropertyValue(3589u));
  CheckProperty(root_node, "calibrated_scale", inspect::UintPropertyValue(3589u));
  CheckProperty(root_node, "power", inspect::BoolPropertyValue(true));
  EXPECT_FALSE(
      root_node.get_property<inspect::DoublePropertyValue>("max_absolute_brightness_nits"));
}
struct IncomingNamespace {
  fake_pdev::FakePDevFidl pdev_server;
  component::OutgoingDirectory outgoing{async_get_default_dispatcher()};
};

TEST_F(Lp8556DeviceTest, GetBackLightPower) {
  TiLp8556Metadata kDeviceMetadata = {
      .panel_id = 2,
      .registers = {},
      .register_count = 0,
  };

  constexpr uint32_t kPanelId = 2;

  async::Loop incoming_loop{&kAsyncLoopConfigNoAttachToCurrentThread};
  async_patterns::TestDispatcherBound<IncomingNamespace> incoming{incoming_loop.dispatcher(),
                                                                  std::in_place};
  fake_pdev::FakePDevFidl::Config config;
  config.board_info = pdev_board_info_t{
      .pid = PDEV_PID_NELSON,
  };

  zx::result outgoing_endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  ASSERT_OK(outgoing_endpoints);
  ASSERT_OK(incoming_loop.StartThread("incoming-ns-thread"));
  incoming.SyncCall([config = std::move(config), server = std::move(outgoing_endpoints->server)](
                        IncomingNamespace* infra) mutable {
    infra->pdev_server.SetConfig(std::move(config));
    ASSERT_OK(infra->outgoing.AddService<fuchsia_hardware_platform_device::Service>(
        infra->pdev_server.GetInstanceHandler()));

    ASSERT_OK(infra->outgoing.Serve(std::move(server)));
  });
  ASSERT_NO_FATAL_FAILURE();
  fake_parent_->AddFidlService(fuchsia_hardware_platform_device::Service::Name,
                               std::move(outgoing_endpoints->client), "pdev");

  fake_parent_->SetMetadata(DEVICE_METADATA_PRIVATE, &kDeviceMetadata, sizeof(kDeviceMetadata));
  fake_parent_->SetMetadata(DEVICE_METADATA_BOARD_PRIVATE, &kPanelId, sizeof(kPanelId));

  mock_i2c_.ExpectWrite({kCfg2Reg})
      .ExpectReadStop({kCfg2Default})
      .ExpectWrite({kCurrentLsbReg})
      .ExpectReadStop({0x42, 0x36})
      .ExpectWrite({kBacklightBrightnessLsbReg})
      .ExpectReadStop({0xab, 0x05})
      .ExpectWrite({kDeviceControlReg})
      .ExpectReadStop({0x85})
      .ExpectWrite({kCfgReg})
      .ExpectReadStop({0x36});
  mock_regs_[BrightnessStickyReg::Get().addr()].ExpectRead();

  EXPECT_OK(dev_->Init());

  VerifySetBrightness(false, 0.0);
  EXPECT_LT(abs(dev_->GetBacklightPower(0) - 0.0141694967), 0.000001f);

  VerifySetBrightness(true, 0.5);
  EXPECT_LT(abs(dev_->GetBacklightPower(2048) - 0.5352831254), 0.000001f);

  VerifySetBrightness(true, 1.0);
  EXPECT_LT(abs(dev_->GetBacklightPower(4095) - 1.0637770353), 0.000001f);
}

TEST_F(Lp8556DeviceTest, GetPowerWatts) {
  TiLp8556Metadata kDeviceMetadata = {
      .panel_id = 2,
      .registers = {},
      .register_count = 0,
  };

  constexpr uint32_t kPanelId = 2;

  async::Loop incoming_loop{&kAsyncLoopConfigNoAttachToCurrentThread};
  async_patterns::TestDispatcherBound<IncomingNamespace> incoming{incoming_loop.dispatcher(),
                                                                  std::in_place};
  fake_pdev::FakePDevFidl::Config config;
  config.board_info = pdev_board_info_t{
      .pid = PDEV_PID_NELSON,
  };

  zx::result outgoing_endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
  ASSERT_OK(outgoing_endpoints);
  ASSERT_OK(incoming_loop.StartThread("incoming-ns-thread"));
  incoming.SyncCall([config = std::move(config), server = std::move(outgoing_endpoints->server)](
                        IncomingNamespace* infra) mutable {
    infra->pdev_server.SetConfig(std::move(config));
    ASSERT_OK(infra->outgoing.AddService<fuchsia_hardware_platform_device::Service>(
        infra->pdev_server.GetInstanceHandler()));

    ASSERT_OK(infra->outgoing.Serve(std::move(server)));
  });
  ASSERT_NO_FATAL_FAILURE();
  fake_parent_->AddFidlService(fuchsia_hardware_platform_device::Service::Name,
                               std::move(outgoing_endpoints->client), "pdev");

  fake_parent_->SetMetadata(DEVICE_METADATA_PRIVATE, &kDeviceMetadata, sizeof(kDeviceMetadata));
  fake_parent_->SetMetadata(DEVICE_METADATA_BOARD_PRIVATE, &kPanelId, sizeof(kPanelId));

  mock_i2c_.ExpectWrite({kCfg2Reg})
      .ExpectReadStop({kCfg2Default})
      .ExpectWrite({kCurrentLsbReg})
      .ExpectReadStop({0x42, 0x36})
      .ExpectWrite({kBacklightBrightnessLsbReg})
      .ExpectReadStop({0xab, 0x05})
      .ExpectWrite({kDeviceControlReg})
      .ExpectReadStop({0x85})
      .ExpectWrite({kCfgReg})
      .ExpectReadStop({0x36});
  mock_regs_[BrightnessStickyReg::Get().addr()].ExpectRead();

  EXPECT_OK(dev_->Init());

  VerifySetBrightness(true, 1.0);
  EXPECT_LT(abs(dev_->GetBacklightPower(4095) - 1.0637770353), 0.000001f);

  auto result = fidl::WireCall(client())->GetPowerWatts();
  EXPECT_TRUE(result.ok());
  EXPECT_FALSE(result->is_error());
}

}  // namespace ti
