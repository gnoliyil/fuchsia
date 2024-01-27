// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "as370-thermal.h"

#include <fidl/fuchsia.hardware.thermal/cpp/wire.h>
#include <fuchsia/hardware/clock/cpp/banjo-mock.h>
#include <fuchsia/hardware/power/cpp/banjo-mock.h>
#include <lib/async-loop/cpp/loop.h>

#include <fbl/algorithm.h>
#include <mock-mmio-reg/mock-mmio-reg.h>
#include <zxtest/zxtest.h>

#include "src/devices/testing/mock-ddk/mock-device.h"

namespace {

bool FloatNear(float a, float b) { return std::abs(a - b) < 0.001f; }

}  // namespace

namespace thermal {

using ThermalClient = fidl::WireSyncClient<fuchsia_hardware_thermal::Device>;
using fuchsia_hardware_thermal::wire::OperatingPoint;
using fuchsia_hardware_thermal::wire::OperatingPointEntry;
using fuchsia_hardware_thermal::wire::PowerDomain;
using fuchsia_hardware_thermal::wire::ThermalDeviceInfo;

class As370ThermalTest : public zxtest::Test {
 public:
  As370ThermalTest()
      : reg_region_(sizeof(uint32_t), 8),
        dut_(nullptr, ddk::MmioBuffer(reg_region_.GetMmioBuffer()), kThermalDeviceInfo,
             ddk::ClockProtocolClient(clock_.GetProto()),
             ddk::PowerProtocolClient(power_.GetProto())) {}

  void SetUp() override {
    loop_.StartThread();
    auto endpoints = fidl::CreateEndpoints<fuchsia_hardware_thermal::Device>();
    ASSERT_OK(endpoints.status_value());
    fidl::BindServer(loop_.dispatcher(), std::move(endpoints->server), &dut_);
    client_.Bind(std::move(endpoints->client));
  }

  void TearDown() override { loop_.Shutdown(); }

 protected:
  void VerifyAll() {
    ASSERT_NO_FATAL_FAILURE(clock_.VerifyAndClear());
    ASSERT_NO_FATAL_FAILURE(power_.VerifyAndClear());
  }

  std::shared_ptr<MockDevice> root_ = MockDevice::FakeRootParent();
  ddk_mock::MockMmioRegRegion reg_region_;
  ddk::MockClock clock_;
  ddk::MockPower power_;
  As370Thermal dut_;
  ThermalClient client_;

 private:
  static constexpr ThermalDeviceInfo kThermalDeviceInfo = {
      .active_cooling = false,
      .passive_cooling = true,
      .gpu_throttling = false,
      .num_trip_points = 0,
      .big_little = false,
      .critical_temp_celsius = 0.0f,
      .trip_point_info = {},
      .opps =
          fidl::Array<OperatingPoint, 2>{
              OperatingPoint{
                  .opp =
                      fidl::Array<OperatingPointEntry, 16>{
                          // clang-format off
                          OperatingPointEntry{.freq_hz =   400'000'000, .volt_uv = 825'000},
                          OperatingPointEntry{.freq_hz =   800'000'000, .volt_uv = 825'000},
                          OperatingPointEntry{.freq_hz = 1'200'000'000, .volt_uv = 825'000},
                          OperatingPointEntry{.freq_hz = 1'400'000'000, .volt_uv = 825'000},
                          OperatingPointEntry{.freq_hz = 1'500'000'000, .volt_uv = 900'000},
                          OperatingPointEntry{.freq_hz = 1'800'000'000, .volt_uv = 900'000},
                          // clang-format on
                      },
                  .latency = 0,
                  .count = 6,
              },
              {
                  .opp = {},
                  .latency = 0,
                  .count = 0,
              },
          },
  };

  async::Loop loop_{&kAsyncLoopConfigNeverAttachToThread};
};

TEST_F(As370ThermalTest, GetTemperature) {
  {
    reg_region_[0x14].ReadReturns(0x17ff);
    auto result = client_->GetTemperatureCelsius();
    EXPECT_OK(result->status);
    EXPECT_TRUE(FloatNear(result->temp, 40.314f));
  }

  {
    reg_region_[0x14].ReadReturns(0x182b);
    auto result = client_->GetTemperatureCelsius();
    EXPECT_OK(result->status);
    EXPECT_TRUE(FloatNear(result->temp, 43.019f));
  }
}

TEST_F(As370ThermalTest, DvfsOperatingPoint) {
  {
    // Success, sets operating point 0.
    power_.ExpectRequestVoltage(ZX_OK, 825'000, 825'000);
    clock_.ExpectSetRate(ZX_OK, 400'000'000);
    auto set_result = client_->SetDvfsOperatingPoint(0, PowerDomain::kBigClusterPowerDomain);
    EXPECT_OK(set_result->status);
    ASSERT_NO_FATAL_FAILURE(VerifyAll());
  }

  {
    auto get_result = client_->GetDvfsOperatingPoint(PowerDomain::kBigClusterPowerDomain);
    EXPECT_OK(get_result->status);
    EXPECT_EQ(get_result->op_idx, 0);
  }

  {
    // Failure, unable to set exact voltage.
    power_.ExpectRequestVoltage(ZX_OK, 825'000, 900'000);
    auto set_result = client_->SetDvfsOperatingPoint(2, PowerDomain::kBigClusterPowerDomain);
    EXPECT_NOT_OK(set_result->status);
    ASSERT_NO_FATAL_FAILURE(VerifyAll());
  }

  {
    auto get_result = client_->GetDvfsOperatingPoint(PowerDomain::kBigClusterPowerDomain);
    EXPECT_OK(get_result->status);
    EXPECT_EQ(get_result->op_idx, 0);
  }

  {
    // Failure, unable to set frequency.
    power_.ExpectRequestVoltage(ZX_OK, 825'000, 825'000);
    clock_.ExpectSetRate(ZX_ERR_IO, 1'200'000'000);
    auto set_result = client_->SetDvfsOperatingPoint(2, PowerDomain::kBigClusterPowerDomain);
    EXPECT_NOT_OK(set_result->status);
    ASSERT_NO_FATAL_FAILURE(VerifyAll());
  }

  {
    auto get_result = client_->GetDvfsOperatingPoint(PowerDomain::kBigClusterPowerDomain);
    EXPECT_OK(get_result->status);
    EXPECT_EQ(get_result->op_idx, 0);
  }

  {
    // Success, sets operating point 4.
    power_.ExpectRequestVoltage(ZX_OK, 900'000, 900'000);
    clock_.ExpectSetRate(ZX_OK, 1'500'000'000);
    auto set_result = client_->SetDvfsOperatingPoint(4, PowerDomain::kBigClusterPowerDomain);
    EXPECT_OK(set_result->status);
    ASSERT_NO_FATAL_FAILURE(VerifyAll());
  }

  {
    auto get_result = client_->GetDvfsOperatingPoint(PowerDomain::kBigClusterPowerDomain);
    EXPECT_OK(get_result->status);
    EXPECT_EQ(get_result->op_idx, 4);
  }

  {
    // Failure, unable to set frequency.
    clock_.ExpectSetRate(ZX_ERR_IO, 800'000'000);
    auto set_result = client_->SetDvfsOperatingPoint(1, PowerDomain::kBigClusterPowerDomain);
    EXPECT_NOT_OK(set_result->status);
    ASSERT_NO_FATAL_FAILURE(VerifyAll());
  }

  {
    auto get_result = client_->GetDvfsOperatingPoint(PowerDomain::kBigClusterPowerDomain);
    EXPECT_OK(get_result->status);
    EXPECT_EQ(get_result->op_idx, 4);
  }

  {
    // Failure, unable to set voltage.
    clock_.ExpectSetRate(ZX_OK, 800'000'000);
    power_.ExpectRequestVoltage(ZX_ERR_IO, 825'000, 0);
    auto set_result = client_->SetDvfsOperatingPoint(1, PowerDomain::kBigClusterPowerDomain);
    EXPECT_NOT_OK(set_result->status);
    ASSERT_NO_FATAL_FAILURE(VerifyAll());
  }

  {
    auto get_result = client_->GetDvfsOperatingPoint(PowerDomain::kBigClusterPowerDomain);
    EXPECT_OK(get_result->status);
    EXPECT_EQ(get_result->op_idx, 4);
  }

  {
    // Success, sets operating point 1.
    clock_.ExpectSetRate(ZX_OK, 800'000'000);
    power_.ExpectRequestVoltage(ZX_OK, 825'000, 825'000);
    auto set_result = client_->SetDvfsOperatingPoint(1, PowerDomain::kBigClusterPowerDomain);
    EXPECT_OK(set_result->status);
    ASSERT_NO_FATAL_FAILURE(VerifyAll());
  }

  {
    auto get_result = client_->GetDvfsOperatingPoint(PowerDomain::kBigClusterPowerDomain);
    EXPECT_OK(get_result->status);
    EXPECT_EQ(get_result->op_idx, 1);
  }
}

TEST_F(As370ThermalTest, Init) {
  power_.ExpectRegisterPowerDomain(ZX_OK, 825'000, 900'000);
  power_.ExpectRequestVoltage(ZX_OK, 900'000, 900'000);
  clock_.ExpectSetRate(ZX_OK, 1'800'000'000);
  EXPECT_OK(dut_.Init());
  ASSERT_NO_FATAL_FAILURE(VerifyAll());
}

}  // namespace thermal
