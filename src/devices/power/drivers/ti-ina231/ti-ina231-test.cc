// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ti-ina231.h"

#include <lib/fake-i2c/fake-i2c.h>

#include <memory>

#include <zxtest/zxtest.h>

#include "src/devices/testing/mock-ddk/mock-device.h"

namespace {

bool FloatNear(float a, float b) { return std::abs(a - b) < 0.001f; }

}  // namespace

namespace power_sensor {

class FakeIna231Device : public fake_i2c::FakeI2c {
 public:
  FakeIna231Device() {
    // Set bits 15 and 14. Bit 15 (reset) should be masked off, while 14 should be preserved.
    registers_[0] = 0xc000;
  }

  uint16_t configuration() const { return registers_[0]; }
  uint16_t calibration() const { return registers_[5]; }
  uint16_t mask_enable() const { return registers_[6]; }
  uint16_t alert_limit() const { return registers_[7]; }

  void set_bus_voltage(uint16_t voltage) { registers_[2] = voltage; }
  void set_power(uint16_t power) { registers_[3] = power; }

 protected:
  zx_status_t Transact(const uint8_t* write_buffer, size_t write_buffer_size, uint8_t* read_buffer,
                       size_t* read_buffer_size) override {
    if (write_buffer_size < 1 || write_buffer[0] >= std::size(registers_)) {
      return ZX_ERR_IO;
    }

    if (write_buffer_size == 1) {
      read_buffer[0] = registers_[write_buffer[0]] >> 8;
      read_buffer[1] = registers_[write_buffer[0]] & 0xff;
      *read_buffer_size = 2;
    } else if (write_buffer_size == 3) {
      if (write_buffer[0] >= 1 && write_buffer[0] <= 4) {
        // Write-only registers.
        return ZX_ERR_IO;
      }

      registers_[write_buffer[0]] = (write_buffer[1] << 8) | write_buffer[2];
    }

    return ZX_OK;
  }

 private:
  uint16_t registers_[8] = {};
};

class TiIna231Test : public zxtest::Test {
 public:
  TiIna231Test() : loop_(&kAsyncLoopConfigNeverAttachToThread) {}

  void SetUp() override {
    root_ = MockDevice::FakeRootParent();
    auto endpoints = fidl::CreateEndpoints<fuchsia_hardware_i2c::Device>();
    EXPECT_TRUE(endpoints.is_ok());

    fidl::BindServer(loop_.dispatcher(), std::move(endpoints->server), &fake_i2c_);
    i2c_client_ = std::move(endpoints->client);

    EXPECT_OK(loop_.StartThread());
  }

 protected:
  FakeIna231Device& fake_i2c() { return fake_i2c_; }
  MockDevice* root() { return root_.get(); }
  fidl::ClientEnd<fuchsia_hardware_i2c::Device> TakeI2cClient() { return std::move(i2c_client_); }
  async::Loop loop_;

 private:
  std::shared_ptr<MockDevice> root_;
  FakeIna231Device fake_i2c_;
  fidl::ClientEnd<fuchsia_hardware_i2c::Device> i2c_client_;
};

TEST_F(TiIna231Test, GetPowerWatts) {
  auto dut = std::make_unique<Ina231Device>(root(), 10'000, TakeI2cClient());

  constexpr Ina231Metadata kMetadata = {
      .mode = Ina231Metadata::kModeShuntAndBusContinuous,
      .shunt_voltage_conversion_time = Ina231Metadata::kConversionTime332us,
      .bus_voltage_conversion_time = Ina231Metadata::kConversionTime332us,
      .averages = Ina231Metadata::kAverages1024,
      .shunt_resistance_microohm = 10'000,
      .alert = Ina231Metadata::kAlertNone,
  };

  EXPECT_OK(dut->Init(kMetadata));

  EXPECT_EQ(fake_i2c().configuration(), 0x4e97);
  EXPECT_EQ(fake_i2c().calibration(), 2048);
  EXPECT_EQ(fake_i2c().mask_enable(), 0);

  EXPECT_OK(dut->DdkAdd("ti-ina231"));
  EXPECT_EQ(root()->child_count(), 1);

  auto endpoints = fidl::CreateEndpoints<power_sensor_fidl::Device>();
  ASSERT_OK(endpoints.status_value());
  ASSERT_OK(dut->PowerSensorConnectServer(endpoints->server.TakeChannel()));
  fidl::WireSyncClient client{
      fidl::ClientEnd<power_sensor_fidl::Device>{std::move(endpoints->client)}};

  {
    fake_i2c().set_power(4792);
    auto response = client->GetPowerWatts();
    ASSERT_TRUE(response.ok());
    ASSERT_FALSE(response->is_error());
    EXPECT_TRUE(FloatNear(response->value()->power, 29.95f));
  }

  {
    fake_i2c().set_power(0);
    auto response = client->GetPowerWatts();
    ASSERT_TRUE(response.ok());
    ASSERT_FALSE(response->is_error());
    EXPECT_TRUE(FloatNear(response->value()->power, 0.0f));
  }

  {
    fake_i2c().set_power(65535);
    auto response = client->GetPowerWatts();
    ASSERT_TRUE(response.ok());
    ASSERT_FALSE(response->is_error());
    EXPECT_TRUE(FloatNear(response->value()->power, 409.59375f));
  }

  dut.release();  // Owned by mock-ddk.
}

TEST_F(TiIna231Test, SetAlertLimit) {
  Ina231Device dut(root(), 10'000, TakeI2cClient());

  constexpr Ina231Metadata kMetadata = {
      .mode = Ina231Metadata::kModeShuntAndBusContinuous,
      .shunt_voltage_conversion_time = Ina231Metadata::kConversionTime332us,
      .bus_voltage_conversion_time = Ina231Metadata::kConversionTime332us,
      .averages = Ina231Metadata::kAverages1024,
      .shunt_resistance_microohm = 10'000,
      .bus_voltage_limit_microvolt = 11'000'000,
      .alert = Ina231Metadata::kAlertBusUnderVoltage,
  };

  EXPECT_OK(dut.Init(kMetadata));
  EXPECT_EQ(fake_i2c().configuration(), 0x4e97);
  EXPECT_EQ(fake_i2c().calibration(), 2048);
  EXPECT_EQ(fake_i2c().mask_enable(), 0x1000);
  EXPECT_EQ(fake_i2c().alert_limit(), 0x2260);
}

TEST_F(TiIna231Test, BanjoClients) {
  Ina231Device dut(root(), 10'000, TakeI2cClient());

  constexpr Ina231Metadata kMetadata = {
      .mode = Ina231Metadata::kModeShuntAndBusContinuous,
      .shunt_voltage_conversion_time = Ina231Metadata::kConversionTime332us,
      .bus_voltage_conversion_time = Ina231Metadata::kConversionTime332us,
      .averages = Ina231Metadata::kAverages1024,
      .shunt_resistance_microohm = 10'000,
      .bus_voltage_limit_microvolt = 11'000'000,
      .alert = Ina231Metadata::kAlertBusUnderVoltage,
  };

  EXPECT_OK(dut.Init(kMetadata));

  zx::result<fidl::ClientEnd<fuchsia_hardware_power_sensor::Device>> client_end;
  fidl::ServerEnd<fuchsia_hardware_power_sensor::Device> server;

  ASSERT_OK((client_end = fidl::CreateEndpoints(&server)).status_value());
  fidl::WireSyncClient client1(std::move(*client_end));
  ASSERT_OK(dut.PowerSensorConnectServer(server.TakeChannel()));

  ASSERT_OK((client_end = fidl::CreateEndpoints(&server)).status_value());
  fidl::WireSyncClient client2(std::move(*client_end));
  ASSERT_OK(dut.PowerSensorConnectServer(server.TakeChannel()));

  fake_i2c().set_power(4792);

  {
    auto response = client1->GetPowerWatts();
    ASSERT_TRUE(response.ok());
    ASSERT_FALSE(response->is_error());
    EXPECT_TRUE(FloatNear(response->value()->power, 29.95f));
  }

  {
    auto response = client2->GetPowerWatts();
    ASSERT_TRUE(response.ok());
    ASSERT_FALSE(response->is_error());
    EXPECT_TRUE(FloatNear(response->value()->power, 29.95f));
  }
}

TEST_F(TiIna231Test, GetVoltageVolts) {
  auto dut = std::make_unique<Ina231Device>(root(), 10'000, TakeI2cClient());

  constexpr Ina231Metadata kMetadata = {
      .mode = Ina231Metadata::kModeShuntAndBusContinuous,
      .shunt_voltage_conversion_time = Ina231Metadata::kConversionTime332us,
      .bus_voltage_conversion_time = Ina231Metadata::kConversionTime332us,
      .averages = Ina231Metadata::kAverages1024,
      .shunt_resistance_microohm = 10'000,
      .alert = Ina231Metadata::kAlertNone,
  };

  EXPECT_OK(dut->Init(kMetadata));
  EXPECT_EQ(fake_i2c().configuration(), 0x4e97);
  EXPECT_EQ(fake_i2c().calibration(), 2048);
  EXPECT_EQ(fake_i2c().mask_enable(), 0);

  EXPECT_OK(dut->DdkAdd("ti-ina231"));

  auto endpoints = fidl::CreateEndpoints<power_sensor_fidl::Device>();
  ASSERT_OK(endpoints.status_value());
  ASSERT_OK(dut->PowerSensorConnectServer(endpoints->server.TakeChannel()));
  fidl::WireSyncClient client{
      fidl::ClientEnd<power_sensor_fidl::Device>{std::move(endpoints->client)}};

  {
    fake_i2c().set_bus_voltage(9200);
    auto response = client->GetVoltageVolts();
    ASSERT_TRUE(response.ok());
    ASSERT_FALSE(response->is_error());
    EXPECT_TRUE(FloatNear(response->value()->voltage, 11.5f));
  }

  {
    fake_i2c().set_bus_voltage(0);
    auto response = client->GetVoltageVolts();
    ASSERT_TRUE(response.ok());
    ASSERT_FALSE(response->is_error());
    EXPECT_TRUE(FloatNear(response->value()->voltage, 0.0f));
  }

  {
    fake_i2c().set_bus_voltage(65535);
    auto response = client->GetVoltageVolts();
    ASSERT_TRUE(response.ok());
    ASSERT_FALSE(response->is_error());
    EXPECT_TRUE(FloatNear(response->value()->voltage, 81.91875f));
  }
  dut.release();  // Owned by mock-ddk.
}

}  // namespace power_sensor
