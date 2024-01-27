// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/mmio/mmio.h>

#include <fake-mmio-reg/fake-mmio-reg.h>
#include <fbl/algorithm.h>
#include <zxtest/zxtest.h>

namespace ddk_fake_test {

TEST(MockMmioReg, CopyFrom) {
  constexpr size_t kRegArrayLength = 0x100;
  //ddk_fake::FakeMmioReg reg_array_1[kRegArrayLength];
  //ddk_fake::FakeMmioReg reg_array_2[kRegArrayLength];

  ddk_fake::FakeMmioRegRegion reg_region_1(sizeof(uint32_t), kRegArrayLength);
  ddk_fake::FakeMmioRegRegion reg_region_2(sizeof(uint32_t), kRegArrayLength);

  fdf::MmioBuffer dut_1(reg_region_1.GetMmioBuffer());
  fdf::MmioBuffer dut_2(reg_region_2.GetMmioBuffer());

  const uint32_t reg_values[] = {0xdb5a95fd, 0xc1c8f880, 0x733c2bed, 0xf74e857c};
  uint32_t written_values[kRegArrayLength];
  memset(written_values, 0, sizeof(written_values));
  for (size_t i = 0; i < std::size(reg_values); i++) {
    reg_region_1[0x10 + (i * 4)].SetReadCallback([i, reg_values]() { return reg_values[i]; });
    reg_region_2[0x40 + (i * 4)].SetWriteCallback(
        [i, &written_values](uint64_t value) { written_values[i] = static_cast<uint32_t>(value); });
  }

  dut_2.CopyFrom32(dut_1, 0x10, 0x40, 4);
  for (size_t i = 0; i < std::size(written_values); i++) {
    bool valid = i < std::size(reg_values);
    if (valid) {
      ASSERT_EQ(written_values[i], reg_values[i]);
    } else {
      ASSERT_EQ(written_values[i], 0);
    }
  }
}

}  // namespace ddk_fake_test
