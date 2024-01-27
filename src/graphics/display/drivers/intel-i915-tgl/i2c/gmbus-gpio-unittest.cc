// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/intel-i915-tgl/i2c/gmbus-gpio.h"

#include <gtest/gtest.h>

#include "src/graphics/display/drivers/intel-i915-tgl/hardware-common.h"

namespace i915_tgl {

namespace {

TEST(GMBusPinPairTest, GetForDdi_TigerLake) {
  constexpr auto kTigerLake = tgl_registers::Platform::kTigerLake;
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_A, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b0001);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_A);
  }
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_B, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b0010);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_B);
  }
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_C, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b0011);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_C);
  }
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_TC_1, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b1001);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_TC_1);
  }
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_TC_2, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b1010);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_TC_2);
  }
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_TC_3, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b1011);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_TC_3);
  }
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_TC_4, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b1100);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_TC_4);
  }
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_TC_5, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b1101);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_TC_5);
  }
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_TC_6, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b1110);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_TC_6);
  }
}

TEST(GMBusPinPairTest, GetForDdi_Skylake) {
  constexpr auto kSkylake = tgl_registers::Platform::kSkylake;
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_A, kSkylake);
    ASSERT_FALSE(pin_pair.has_value());
  }
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_B, kSkylake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b0101);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_B);
  }
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_C, kSkylake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b0100);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_C);
  }
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_D, kSkylake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b0110);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_D);
  }
  {
    std::optional<GMBusPinPair> pin_pair = GMBusPinPair::GetForDdi(DdiId::DDI_E, kSkylake);
    ASSERT_FALSE(pin_pair.has_value());
  }
  for (const DdiId invalid_ddi :
       {DdiId::DDI_TC_3, DdiId::DDI_TC_4, DdiId::DDI_TC_5, DdiId::DDI_TC_6}) {
    EXPECT_DEATH_IF_SUPPORTED({ GMBusPinPair::GetForDdi(invalid_ddi, kSkylake); }, "Invalid");
  }
}

TEST(GMBusPinPairTest, HasValidPinPair_TigerLake) {
  constexpr auto kTigerLake = tgl_registers::Platform::kTigerLake;
  for (const DdiId ddi_with_valid_pin_pair :
       {DdiId::DDI_A, DdiId::DDI_B, DdiId::DDI_C, DdiId::DDI_TC_1, DdiId::DDI_TC_2, DdiId::DDI_TC_3,
        DdiId::DDI_TC_4, DdiId::DDI_TC_5, DdiId::DDI_TC_6}) {
    EXPECT_TRUE(GMBusPinPair::HasValidPinPair(ddi_with_valid_pin_pair, kTigerLake));
  }
}

TEST(GMBusPinPairTest, HasValidPinPair_Skylake) {
  constexpr auto kSkylake = tgl_registers::Platform::kSkylake;
  for (const DdiId ddi_with_valid_pin_pair : {DdiId::DDI_B, DdiId::DDI_C, DdiId::DDI_D}) {
    EXPECT_TRUE(GMBusPinPair::HasValidPinPair(ddi_with_valid_pin_pair, kSkylake));
  }
  for (const DdiId ddi_without_valid_pin_pair :
       {DdiId::DDI_A, DdiId::DDI_TC_2, DdiId::DDI_TC_3, DdiId::DDI_TC_4, DdiId::DDI_TC_5,
        DdiId::DDI_TC_6}) {
    EXPECT_FALSE(GMBusPinPair::HasValidPinPair(ddi_without_valid_pin_pair, kSkylake));
  }
}

TEST(GpioPortTest, GetForDdi_TigerLake) {
  constexpr auto kTigerLake = tgl_registers::Platform::kTigerLake;
  {
    std::optional<GpioPort> pin_pair = GpioPort::GetForDdi(DdiId::DDI_A, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b0001);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_A);
  }
  {
    std::optional<GpioPort> pin_pair = GpioPort::GetForDdi(DdiId::DDI_B, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b0010);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_B);
  }
  {
    std::optional<GpioPort> pin_pair = GpioPort::GetForDdi(DdiId::DDI_C, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b0011);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_C);
  }
  {
    std::optional<GpioPort> pin_pair = GpioPort::GetForDdi(DdiId::DDI_TC_1, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b1001);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_TC_1);
  }
  {
    std::optional<GpioPort> pin_pair = GpioPort::GetForDdi(DdiId::DDI_TC_2, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b1010);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_TC_2);
  }
  {
    std::optional<GpioPort> pin_pair = GpioPort::GetForDdi(DdiId::DDI_TC_3, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b1011);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_TC_3);
  }
  {
    std::optional<GpioPort> pin_pair = GpioPort::GetForDdi(DdiId::DDI_TC_4, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b1100);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_TC_4);
  }
  {
    std::optional<GpioPort> pin_pair = GpioPort::GetForDdi(DdiId::DDI_TC_5, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b1101);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_TC_5);
  }
  {
    std::optional<GpioPort> pin_pair = GpioPort::GetForDdi(DdiId::DDI_TC_6, kTigerLake);
    ASSERT_TRUE(pin_pair.has_value());
    EXPECT_EQ(pin_pair->number(), 0b1110);
    EXPECT_EQ(pin_pair->ddi_id(), DdiId::DDI_TC_6);
  }
}

TEST(GpioPortTest, GetForDdi_Skylake) {
  constexpr auto kSkylake = tgl_registers::Platform::kSkylake;
  {
    std::optional<GpioPort> gpio_port = GpioPort::GetForDdi(DdiId::DDI_A, kSkylake);
    ASSERT_FALSE(gpio_port.has_value());
  }
  {
    std::optional<GpioPort> gpio_port = GpioPort::GetForDdi(DdiId::DDI_B, kSkylake);
    ASSERT_TRUE(gpio_port.has_value());
    EXPECT_EQ(gpio_port->number(), 0b0100);
    EXPECT_EQ(gpio_port->ddi_id(), DdiId::DDI_B);
  }
  {
    std::optional<GpioPort> gpio_port = GpioPort::GetForDdi(DdiId::DDI_C, kSkylake);
    ASSERT_TRUE(gpio_port.has_value());
    EXPECT_EQ(gpio_port->number(), 0b0011);
    EXPECT_EQ(gpio_port->ddi_id(), DdiId::DDI_C);
  }
  {
    std::optional<GpioPort> gpio_port = GpioPort::GetForDdi(DdiId::DDI_D, kSkylake);
    ASSERT_TRUE(gpio_port.has_value());
    EXPECT_EQ(gpio_port->number(), 0b0101);
    EXPECT_EQ(gpio_port->ddi_id(), DdiId::DDI_D);
  }
  {
    std::optional<GpioPort> gpio_port = GpioPort::GetForDdi(DdiId::DDI_E, kSkylake);
    ASSERT_FALSE(gpio_port.has_value());
  }
  for (const DdiId invalid_ddi :
       {DdiId::DDI_TC_3, DdiId::DDI_TC_4, DdiId::DDI_TC_5, DdiId::DDI_TC_6}) {
    EXPECT_DEATH_IF_SUPPORTED({ GpioPort::GetForDdi(invalid_ddi, kSkylake); }, "Invalid");
  }
}

TEST(GpioPortTest, HasValidPort_TigerLake) {
  constexpr auto kTigerLake = tgl_registers::Platform::kTigerLake;
  for (const DdiId ddi_with_valid_port :
       {DdiId::DDI_A, DdiId::DDI_B, DdiId::DDI_C, DdiId::DDI_TC_1, DdiId::DDI_TC_2, DdiId::DDI_TC_3,
        DdiId::DDI_TC_4, DdiId::DDI_TC_5, DdiId::DDI_TC_6}) {
    EXPECT_TRUE(GpioPort::HasValidPort(ddi_with_valid_port, kTigerLake));
  }
}

TEST(GpioPortTest, HasValidPort_Skylake) {
  constexpr auto kSkylake = tgl_registers::Platform::kSkylake;
  for (const DdiId ddi_with_valid_port : {DdiId::DDI_B, DdiId::DDI_C, DdiId::DDI_D}) {
    EXPECT_TRUE(GpioPort::HasValidPort(ddi_with_valid_port, kSkylake));
  }
  for (const DdiId ddi_without_valid_port : {DdiId::DDI_A, DdiId::DDI_TC_2, DdiId::DDI_TC_3,
                                             DdiId::DDI_TC_4, DdiId::DDI_TC_5, DdiId::DDI_TC_6}) {
    EXPECT_FALSE(GpioPort::HasValidPort(ddi_without_valid_port, kSkylake));
  }
}

}  // namespace

}  // namespace i915_tgl
