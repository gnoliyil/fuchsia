// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "as370-power.h"

#include <lib/async-loop/cpp/loop.h>
#include <lib/mock-i2c/mock-i2c.h>

#include <soc/as370/as370-power.h>
#include <zxtest/zxtest.h>

namespace power {

class As370PowerTest : public As370Power {
 public:
  As370PowerTest() : As370Power(nullptr), loop_(&kAsyncLoopConfigNeverAttachToThread) {}

  zx_status_t InitializeProtocols(fidl::ClientEnd<fuchsia_hardware_i2c::Device>* i2c) override {
    auto endpoints = fidl::CreateEndpoints<fuchsia_hardware_i2c::Device>();
    EXPECT_TRUE(endpoints.is_ok());

    fidl::BindServer(loop_.dispatcher(), std::move(endpoints->server), &mock_i2c);

    *i2c = std::move(endpoints->client);

    return loop_.StartThread();
  }

  void Verify() { mock_i2c.VerifyAndClear(); }

  mock_i2c::MockI2c mock_i2c;

 private:
  async::Loop loop_;
};

TEST(As370PowerTest, InitTest) {
  As370PowerTest dut;

  // Initial read to get the regulator state.
  dut.mock_i2c.ExpectWrite({0x00}).ExpectReadStop({0x85});

  EXPECT_OK(dut.Init());
  dut.Verify();
}

TEST(As370PowerTest, BuckRegulatorEnableDisable) {
  As370PowerTest dut;
  dut.mock_i2c.ExpectWrite({0x00}).ExpectReadStop({0x85});
  EXPECT_OK(dut.Init());

  // Initial status enabled
  power_domain_status_t domain_status = POWER_DOMAIN_STATUS_DISABLED;
  EXPECT_OK(dut.PowerImplGetPowerDomainStatus(kBuckSoC, &domain_status));
  EXPECT_EQ(domain_status, POWER_DOMAIN_STATUS_ENABLED);

  // Disable
  dut.mock_i2c.ExpectWrite({0x00}).ExpectReadStop({0x85});
  dut.mock_i2c.ExpectWriteStop({0x00, 0x05});
  EXPECT_OK(dut.PowerImplDisablePowerDomain(kBuckSoC));
  EXPECT_OK(dut.PowerImplGetPowerDomainStatus(kBuckSoC, &domain_status));
  EXPECT_EQ(domain_status, POWER_DOMAIN_STATUS_DISABLED);

  // Enable
  dut.mock_i2c.ExpectWrite({0x00}).ExpectReadStop({0x05});
  dut.mock_i2c.ExpectWriteStop({0x00, 0x85});
  EXPECT_OK(dut.PowerImplEnablePowerDomain(kBuckSoC));
  EXPECT_OK(dut.PowerImplGetPowerDomainStatus(kBuckSoC, &domain_status));
  EXPECT_EQ(domain_status, POWER_DOMAIN_STATUS_ENABLED);

  dut.Verify();
}

TEST(As370PowerTest, BuckRegulatorSetVoltage) {
  As370PowerTest dut;
  dut.mock_i2c.ExpectWrite({0x00}).ExpectReadStop({0x8B});
  EXPECT_OK(dut.Init());

  // Get default voltage
  uint32_t voltage = 0;
  EXPECT_OK(dut.PowerImplGetCurrentVoltage(kBuckSoC, &voltage));
  EXPECT_EQ(voltage, 900000);

  // Set new voltage
  dut.mock_i2c.ExpectWrite({0x00}).ExpectReadStop({0x8B});
  dut.mock_i2c.ExpectWriteStop({0x00, 0x8D});
  voltage += 25000;
  uint32_t actual_voltage = 0;
  EXPECT_OK(dut.PowerImplRequestVoltage(kBuckSoC, voltage, &actual_voltage));
  EXPECT_EQ(voltage, actual_voltage);

  // Check current voltage
  EXPECT_OK(dut.PowerImplGetCurrentVoltage(kBuckSoC, &voltage));
  EXPECT_EQ(voltage, actual_voltage);

  dut.Verify();
}

}  // namespace power
