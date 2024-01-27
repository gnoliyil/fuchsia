// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util.h"

#include <gtest/gtest.h>

namespace bt::hci_spec {
namespace {

TEST(UtilTest, EncodeLegacyAdvertisingInterval) {
  uint16_t input = 0x00ff;
  uint8_t actual[3] = {0};
  EncodeLegacyAdvertisingInterval(input, actual);

  uint8_t expected[3] = {0};
  if (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) {
    expected[1] = 0xff;
  } else {
    expected[0] = 0xff;
  }

  EXPECT_EQ(expected[0], actual[0]);
  EXPECT_EQ(expected[1], actual[1]);
  EXPECT_EQ(expected[2], actual[2]);
}

TEST(UtilTest, DecodeExtendedAdvertisingInterval) {
  uint8_t input[3];
  input[0] = 0xaa;
  input[1] = 0xbb;
  input[2] = 0xcc;

  uint32_t actual = DecodeExtendedAdvertisingInterval(input);

  uint32_t expected = 0;
  if (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) {
    expected = (0xaa << 24) | (0xbb << 16) | (0xcc << 8) | (0x00 << 0);
  } else {
    expected = (0x00 << 24) | (0xcc << 16) | (0xbb << 8) | (0xaa << 0);
  }

  EXPECT_EQ(expected, actual);
}

// Bit values used in this test are given in a table in Core Spec Volume 4, Part E, Section 7.8.53.
TEST(UtilTest, AdvertisingTypeToEventBits) {
  std::optional<hci_spec::AdvertisingEventBits> bits =
      AdvertisingTypeToEventBits(LEAdvertisingType::kAdvInd);
  ASSERT_TRUE(bits);
  EXPECT_EQ(0b00010011, bits.value());

  bits = AdvertisingTypeToEventBits(LEAdvertisingType::kAdvDirectIndLowDutyCycle);
  ASSERT_TRUE(bits);
  EXPECT_EQ(0b00010101, bits.value());

  bits = AdvertisingTypeToEventBits(LEAdvertisingType::kAdvDirectIndHighDutyCycle);
  ASSERT_TRUE(bits);
  EXPECT_EQ(0b00011101, bits.value());

  bits = AdvertisingTypeToEventBits(LEAdvertisingType::kAdvScanInd);
  ASSERT_TRUE(bits);
  EXPECT_EQ(0b00010010, bits.value());

  bits = AdvertisingTypeToEventBits(LEAdvertisingType::kAdvNonConnInd);
  ASSERT_TRUE(bits);
  EXPECT_EQ(0b00010000, bits.value());
}

}  // namespace
}  // namespace bt::hci_spec
