// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ft_device.h"

#include <fuchsia/hardware/gpio/cpp/banjo-mock.h>
#include <lib/ddk/metadata.h>
#include <lib/fake-i2c/fake-i2c.h>
#include <lib/fake_ddk/fake_ddk.h>
#include <lib/focaltech/focaltech.h>

#include <hid/ft3x27.h>
#include <hid/ft5726.h>
#include <hid/ft6336.h>
#include <zxtest/zxtest.h>

#include "ft_firmware.h"

namespace {

constexpr uint8_t kFirmware0[] = {0x00, 0xd2, 0xc8, 0x53, 0xd5, 0x6b, 0xc0, 0x60, 0x73, 0xd8};
constexpr uint8_t kFirmware1[] = {0x10, 0x58, 0xb2, 0x12, 0xc8, 0xea, 0x5a, 0xca};
constexpr uint8_t kFirmware2[] = {0xb7, 0xf9, 0xd1, 0x12, 0xb0, 0x10, 0x2c, 0xef, 0x98, 0x65};
constexpr uint8_t kFirmware3[] = {0x02, 0x69, 0x96, 0x71, 0x61};

}  // namespace

namespace ft {

const FirmwareEntry kFirmwareEntries[] = {
    {
        .display_vendor = 0,
        .ddic_version = 0,
        .firmware_data = kFirmware0,
        .firmware_size = sizeof(kFirmware0),
    },
    {
        .display_vendor = 1,
        .ddic_version = 0,
        .firmware_data = kFirmware1,
        .firmware_size = sizeof(kFirmware1),
    },
    {
        .display_vendor = 0,
        .ddic_version = 1,
        .firmware_data = kFirmware2,
        .firmware_size = sizeof(kFirmware2),
    },
    {
        .display_vendor = 1,
        .ddic_version = 1,
        .firmware_data = kFirmware3,
        .firmware_size = sizeof(kFirmware3),
    },
};

const size_t kNumFirmwareEntries = countof(kFirmwareEntries);

class FakeFtDevice : public fake_i2c::FakeI2c {
 protected:
  zx_status_t Transact(const uint8_t* write_buffer, size_t write_buffer_size, uint8_t* read_buffer,
                       size_t* read_buffer_size) override {
    return ZX_ERR_NOT_SUPPORTED;
  }
};

class FocaltechTest : public zxtest::Test {
 public:
  void SetUp() override {
    fbl::Array<fake_ddk::FragmentEntry> fragments(new fake_ddk::FragmentEntry[3], 3);
    fragments[0].name = "i2c";
    fragments[0].protocols.push_back(fake_ddk::ProtocolEntry{
        .id = ZX_PROTOCOL_I2C,
        .proto =
            {
                .ops = i2c_.GetProto()->ops,
                .ctx = i2c_.GetProto()->ctx,
            },
    });

    fragments[1].name = "gpio-int";
    fragments[1].protocols.push_back(fake_ddk::ProtocolEntry{
        .id = ZX_PROTOCOL_GPIO,
        .proto =
            {
                .ops = interrupt_gpio_.GetProto()->ops,
                .ctx = interrupt_gpio_.GetProto()->ctx,
            },
    });

    fragments[2].name = "gpio-reset";
    fragments[2].protocols.push_back(fake_ddk::ProtocolEntry{
        .id = ZX_PROTOCOL_GPIO,
        .proto =
            {
                .ops = reset_gpio_.GetProto()->ops,
                .ctx = reset_gpio_.GetProto()->ctx,
            },
    });

    ddk_.SetFragments(std::move(fragments));

    zx::interrupt interrupt;
    ASSERT_OK(zx::interrupt::create(zx::resource(), 0, ZX_INTERRUPT_VIRTUAL, &interrupt));

    interrupt_gpio_.ExpectConfigIn(ZX_OK, GPIO_NO_PULL)
        .ExpectGetInterrupt(ZX_OK, ZX_INTERRUPT_MODE_EDGE_LOW, std::move(interrupt));
    reset_gpio_.ExpectConfigOut(ZX_OK, 0).ExpectWrite(ZX_OK, 1);
  }

 protected:
  fake_ddk::Bind ddk_;

 private:
  FakeFtDevice i2c_;
  ddk::MockGpio interrupt_gpio_;
  ddk::MockGpio reset_gpio_;
};

TEST_F(FocaltechTest, Metadata3x27) {
  constexpr FocaltechMetadata kFt3x27Metadata = {
    .device_id = FOCALTECH_DEVICE_FT3X27,
    .needs_firmware = false,
  };
  ddk_.SetMetadata(DEVICE_METADATA_PRIVATE, &kFt3x27Metadata, sizeof(kFt3x27Metadata));

  FtDevice dut(nullptr);
  EXPECT_OK(dut.Init());

  uint8_t actual_descriptor[1024];
  size_t actual_size = 0;
  EXPECT_OK(dut.HidbusGetDescriptor(0, actual_descriptor, sizeof(actual_descriptor), &actual_size));

  const uint8_t* expected_descriptor;
  const size_t expected_size = get_ft3x27_report_desc(&expected_descriptor);
  ASSERT_EQ(actual_size, expected_size);
  EXPECT_BYTES_EQ(actual_descriptor, expected_descriptor, expected_size);
}

TEST_F(FocaltechTest, Metadata5726) {
  constexpr FocaltechMetadata kFt5726Metadata = {
    .device_id = FOCALTECH_DEVICE_FT5726,
    .needs_firmware = false,
  };
  ddk_.SetMetadata(DEVICE_METADATA_PRIVATE, &kFt5726Metadata, sizeof(kFt5726Metadata));

  FtDevice dut(nullptr);
  EXPECT_OK(dut.Init());

  uint8_t actual_descriptor[1024];
  size_t actual_size = 0;
  EXPECT_OK(dut.HidbusGetDescriptor(0, actual_descriptor, sizeof(actual_descriptor), &actual_size));

  const uint8_t* expected_descriptor;
  const size_t expected_size = get_ft5726_report_desc(&expected_descriptor);
  ASSERT_EQ(actual_size, expected_size);
  EXPECT_BYTES_EQ(actual_descriptor, expected_descriptor, expected_size);
}

TEST_F(FocaltechTest, Metadata6336) {
  constexpr FocaltechMetadata kFt6336Metadata = {
    .device_id = FOCALTECH_DEVICE_FT6336,
    .needs_firmware = false,
  };
  ddk_.SetMetadata(DEVICE_METADATA_PRIVATE, &kFt6336Metadata, sizeof(kFt6336Metadata));

  FtDevice dut(nullptr);
  EXPECT_OK(dut.Init());

  uint8_t actual_descriptor[1024];
  size_t actual_size = 0;
  EXPECT_OK(dut.HidbusGetDescriptor(0, actual_descriptor, sizeof(actual_descriptor), &actual_size));

  const uint8_t* expected_descriptor;
  const size_t expected_size = get_ft6336_report_desc(&expected_descriptor);
  ASSERT_EQ(actual_size, expected_size);
  EXPECT_BYTES_EQ(actual_descriptor, expected_descriptor, expected_size);
}

TEST_F(FocaltechTest, Firmware5726) {
  constexpr FocaltechMetadata kFt5726Metadata = {
    .device_id = FOCALTECH_DEVICE_FT5726,
    .needs_firmware = true,
    .display_vendor = 1,
    .ddic_version = 1,
  };
  ddk_.SetMetadata(DEVICE_METADATA_PRIVATE, &kFt5726Metadata, sizeof(kFt5726Metadata));

  FtDevice dut(nullptr);
  EXPECT_OK(dut.Init());
}

}  // namespace ft
