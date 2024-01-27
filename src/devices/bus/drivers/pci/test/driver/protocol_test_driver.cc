// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "protocol_test_driver.h"

#include <fidl/fuchsia.device.test/cpp/wire.h>
#include <fuchsia/hardware/pci/cpp/banjo.h>
#include <lib/ddk/device.h>
#include <lib/ddk/platform-defs.h>
#include <lib/zx/clock.h>
#include <lib/zx/object.h>
#include <lib/zx/time.h>
#include <stdio.h>
#include <threads.h>
#include <zircon/errors.h>
#include <zircon/hw/pci.h>
#include <zircon/syscalls/object.h>
#include <zircon/threads.h>

#include <vector>

#include <zxtest/zxtest.h>

#include "src/devices/bus/drivers/pci/capabilities/msi.h"
#include "src/devices/bus/drivers/pci/common.h"
#include "src/devices/bus/drivers/pci/config.h"
#include "src/devices/bus/drivers/pci/test/driver/pci_protocol_test_driver_bind.h"
#include "src/devices/bus/drivers/pci/test/fakes/test_device.h"

// A special LogSink that just redirects all output to zxlogf
class LogSink : public zxtest::LogSink {
 public:
  void Write(const char* format, ...) override {
    std::array<char, 1024> line_buf;
    va_list args;
    va_start(args, format);
    vsnprintf(line_buf.data(), line_buf.size(), format, args);
    va_end(args);
    line_buf[line_buf.size() - 1] = '\0';
    zxlogf(INFO, "%s", line_buf.data());
  }
  void Flush() override {}
};

ProtocolTestDriver* ProtocolTestDriver::instance_;

namespace {
class PciProtocolTests : public zxtest::Test {
 public:
  const ddk::PciProtocolClient& pci() { return drv_->pci(); }

 protected:
  PciProtocolTests() : drv_(ProtocolTestDriver::GetInstance()) {}
  void GetBarTestHelper(uint32_t bar_id);

 private:
  ProtocolTestDriver* drv_;
};

TEST_F(PciProtocolTests, TestResetDeviceUnsupported) {
  EXPECT_EQ(pci().ResetDevice(), ZX_ERR_NOT_SUPPORTED);
}

// Do basic reads work in the config header?
TEST_F(PciProtocolTests, ReadConfigHeader) {
  uint16_t rd_val16 = 0;
  ASSERT_OK(pci().ReadConfig16(PCI_CONFIG_VENDOR_ID, &rd_val16));
  ASSERT_EQ(rd_val16, PCI_TEST_DRIVER_VID);
  ASSERT_OK(pci().ReadConfig16(PCI_CONFIG_DEVICE_ID, &rd_val16));
  ASSERT_EQ(rd_val16, PCI_TEST_DRIVER_DID);
}

TEST_F(PciProtocolTests, ConfigBounds) {
  uint8_t rd_val8 = 0;
  uint16_t rd_val16 = 0;
  uint32_t rd_val32 = 0;

  // Reads/Writes outside of config space should be invalid.
  ASSERT_EQ(pci().ReadConfig8(PCI_EXT_CONFIG_SIZE, &rd_val8), ZX_ERR_OUT_OF_RANGE);
  ASSERT_EQ(pci().ReadConfig16(PCI_EXT_CONFIG_SIZE, &rd_val16), ZX_ERR_OUT_OF_RANGE);
  ASSERT_EQ(pci().ReadConfig32(PCI_EXT_CONFIG_SIZE, &rd_val32), ZX_ERR_OUT_OF_RANGE);
  ASSERT_EQ(pci().WriteConfig8(PCI_EXT_CONFIG_SIZE, UINT8_MAX), ZX_ERR_OUT_OF_RANGE);
  ASSERT_EQ(pci().WriteConfig16(PCI_EXT_CONFIG_SIZE, UINT16_MAX), ZX_ERR_OUT_OF_RANGE);
  ASSERT_EQ(pci().WriteConfig32(PCI_EXT_CONFIG_SIZE, UINT32_MAX), ZX_ERR_OUT_OF_RANGE);

  // Writes within the config header are not allowed.
  for (uint16_t addr = 0; addr < PCI_CONFIG_HDR_SIZE; addr++) {
    ASSERT_EQ(pci().WriteConfig8(addr, UINT8_MAX), ZX_ERR_ACCESS_DENIED);
    ASSERT_EQ(pci().WriteConfig16(addr, UINT16_MAX), ZX_ERR_ACCESS_DENIED);
    ASSERT_EQ(pci().WriteConfig32(addr, UINT32_MAX), ZX_ERR_ACCESS_DENIED);
  }
}

// A simple offset / pattern for confirming reads and writes.
// Ensuring it never returns 0.
constexpr uint16_t kTestPatternStart = 0x800;
constexpr uint16_t kTestPatternEnd = 0x1000;
constexpr uint8_t TestPatternValue(int address) {
  return static_cast<uint8_t>((address % UINT8_MAX) + 1u);
}

// These pattern tests use ReadConfig/WriteConfig of all sizes to read and write
// patterns to the back half of the fake device's config space, using the standard
// Pci Protocol methods and the actual device Config object.
TEST_F(PciProtocolTests, ConfigPattern8) {
  uint8_t rd_val = 0;

  // Clear it out. Important if this test runs out of order.
  for (uint16_t addr = kTestPatternStart; addr < kTestPatternEnd; addr++) {
    ASSERT_OK(pci().WriteConfig8(addr, 0));
  }

  // Verify the clear.
  for (uint16_t addr = kTestPatternStart; addr < kTestPatternEnd; addr++) {
    ASSERT_OK(pci().ReadConfig8(addr, &rd_val));
    ASSERT_EQ(rd_val, 0);
  }

  // Write the pattern out.
  for (uint16_t addr = kTestPatternStart; addr < kTestPatternEnd; addr++) {
    ASSERT_OK(pci().WriteConfig8(addr, TestPatternValue(addr)));
  }

  // Verify the pattern.
  for (uint16_t addr = kTestPatternStart; addr < kTestPatternEnd; addr++) {
    ASSERT_OK(pci().ReadConfig8(addr, &rd_val));
    ASSERT_EQ(rd_val, TestPatternValue(addr));
  }
}

TEST_F(PciProtocolTests, ConfigPattern16) {
  uint16_t rd_val = 0;
  auto PatternValue = [](uint16_t addr) -> uint16_t {
    return static_cast<uint16_t>((TestPatternValue(addr + 1) << 8) | TestPatternValue(addr));
  };

  // Clear it out. Important if this test runs out of order.
  for (uint16_t addr = kTestPatternStart; addr < kTestPatternEnd - 1;
       addr = static_cast<uint16_t>(addr + 2)) {
    ASSERT_OK(pci().WriteConfig16(addr, 0));
  }

  // Verify the clear.
  for (uint16_t addr = kTestPatternStart; addr < kTestPatternEnd - 1;
       addr = static_cast<uint16_t>(addr + 2)) {
    ASSERT_OK(pci().ReadConfig16(addr, &rd_val));
    ASSERT_EQ(rd_val, 0);
  }

  // Write the pattern out.
  for (uint16_t addr = kTestPatternStart; addr < kTestPatternEnd - 1;
       addr = static_cast<uint16_t>(addr + 2)) {
    ASSERT_OK(pci().WriteConfig16(addr, PatternValue(addr)));
  }

  // Verify the pattern.
  for (uint16_t addr = kTestPatternStart; addr < kTestPatternEnd - 1;
       addr = static_cast<uint16_t>(addr + 2)) {
    ASSERT_OK(pci().ReadConfig16(addr, &rd_val));
    ASSERT_EQ(rd_val, PatternValue(addr));
  }
}

TEST_F(PciProtocolTests, ConfigPattern32) {
  uint32_t rd_val = 0;
  auto PatternValue = [](uint16_t addr) -> uint32_t {
    return static_cast<uint32_t>((TestPatternValue(addr + 3) << 24) |
                                 (TestPatternValue(addr + 2) << 16) |
                                 (TestPatternValue(addr + 1) << 8) | TestPatternValue(addr));
  };

  // Clear it out. Important if this test runs out of order.
  for (uint16_t addr = kTestPatternStart; addr < kTestPatternEnd - 3;
       addr = static_cast<uint16_t>(addr + 4)) {
    ASSERT_OK(pci().WriteConfig32(static_cast<uint16_t>(addr), 0));
  }

  // Verify the clear.
  for (uint16_t addr = kTestPatternStart; addr < kTestPatternEnd - 3;
       addr = static_cast<uint16_t>(addr + 4)) {
    ASSERT_OK(pci().ReadConfig32(addr, &rd_val));
    ASSERT_EQ(rd_val, 0);
  }

  // Write the pattern out.
  for (uint16_t addr = kTestPatternStart; addr < kTestPatternEnd - 3;
       addr = static_cast<uint16_t>(addr + 4)) {
    ASSERT_OK(pci().WriteConfig32(addr, PatternValue(addr)));
  }

  // Verify the pattern.
  for (uint16_t addr = kTestPatternStart; addr < kTestPatternEnd - 3;
       addr = static_cast<uint16_t>(addr + 4)) {
    ASSERT_OK(pci().ReadConfig32(addr, &rd_val));
    ASSERT_EQ(rd_val, PatternValue(addr));
  }
}

TEST_F(PciProtocolTests, SetBusMastering) {
  struct pci::config::Command cmd_reg = {};
  uint16_t cached_value = 0;

  // Ensure Bus mastering is already enabled in our test quadro.
  ASSERT_OK(pci().ReadConfig16(PCI_CONFIG_COMMAND, &cmd_reg.value));
  ASSERT_EQ(true, cmd_reg.bus_master());
  cached_value = cmd_reg.value;  // cache so we can test other bits are preserved

  // Ensure we can disable it.
  ASSERT_OK(pci().SetBusMastering(false));
  ASSERT_OK(pci().ReadConfig16(PCI_CONFIG_COMMAND, &cmd_reg.value));
  ASSERT_EQ(false, cmd_reg.bus_master());
  ASSERT_EQ(cached_value & ~PCI_CONFIG_COMMAND_BUS_MASTER_EN, cmd_reg.value);

  // Enable and confirm it.
  ASSERT_OK(pci().SetBusMastering(true));
  ASSERT_OK(pci().ReadConfig16(PCI_CONFIG_COMMAND, &cmd_reg.value));
  ASSERT_EQ(true, cmd_reg.bus_master());
  ASSERT_EQ(cached_value, cmd_reg.value);
}

TEST_F(PciProtocolTests, GetBarArgumentCheck) {
  pci_bar_t info = {};
  // Test that only valid BAR ids are accepted.
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, pci().GetBar(PCI_MAX_BAR_REGS, &info));
}

// These individual BAR tests are coupled closely to the device configuration
// stored in test_device.h. If that configuration is changed in a way that
// affects the expected BAR information then these tests also need to be
// updated.
void PciProtocolTests::GetBarTestHelper(uint32_t bar_id) {
  pci_bar_t info{};
  const test_bar_info_t& test_bar = kTestDeviceBars[bar_id];
  ASSERT_OK(pci().GetBar(bar_id, &info));
  EXPECT_EQ(info.bar_id, bar_id);
  EXPECT_EQ(info.type, test_bar.type);

  pci_address_space_t io_type = PCI_ADDRESS_SPACE_NONE;
#ifdef __x86_64__
  io_type = PCI_ADDRESS_SPACE_IO;
#elif defined(__aarch64__)
  io_type = PCI_ADDRESS_SPACE_MEMORY;
#endif
  ASSERT_NE(io_type, PCI_ADDRESS_SPACE_NONE);
  // ARM uses MMIO for both types of BARs
  if (info.type == PCI_ADDRESS_SPACE_MEMORY ||
      (info.type == PCI_ADDRESS_SPACE_IO && io_type == PCI_ADDRESS_SPACE_MEMORY)) {
    size_t size = 0;
    zx::vmo vmo(info.result.vmo);
    ASSERT_OK(vmo.get_size(&size));
    EXPECT_EQ(size, test_bar.size);
  } else {
    EXPECT_EQ(info.size, test_bar.size);
    EXPECT_EQ(info.result.io.address, test_bar.address);
    EXPECT_NE(info.result.io.resource, ZX_HANDLE_INVALID);
  }
}

// Standard MMIO bars of varying size and 64bit-ness.
TEST_F(PciProtocolTests, GetBar0) { ASSERT_NO_FAILURES(GetBarTestHelper(0)); }
TEST_F(PciProtocolTests, GetBar1) { ASSERT_NO_FAILURES(GetBarTestHelper(1)); }
TEST_F(PciProtocolTests, GetBar3) { ASSERT_NO_FAILURES(GetBarTestHelper(3)); }
// An IO bar which behaves differently according to platform
TEST_F(PciProtocolTests, GetBar5) { ASSERT_NO_FAILURES(GetBarTestHelper(5)); }

TEST_F(PciProtocolTests, GetBar2) {
  pci_bar_t info = {};
  // BAR 2 contains MSI-X registers and should be denied
  ASSERT_EQ(ZX_ERR_ACCESS_DENIED, pci().GetBar(2, &info));
}

TEST_F(PciProtocolTests, GetBar4) {
  pci_bar_t info = {};
  // BAR 4 (Bar 3 second half, should be NOT_FOUND)
  ASSERT_EQ(ZX_ERR_NOT_FOUND, pci().GetBar(4, &info));
}

TEST_F(PciProtocolTests, GetCapabilities) {
  uint8_t offsetA = 0;
  uint8_t offsetB = 0;
  uint8_t val8 = 0;

  // First Power Management Capability is at 0x60.
  ASSERT_OK(pci().GetFirstCapability(PCI_CAPABILITY_ID_PCI_PWR_MGMT, &offsetA));
  ASSERT_EQ(0x60, offsetA);
  ASSERT_OK(pci().ReadConfig8(offsetA, &val8));
  ASSERT_EQ(PCI_CAPABILITY_ID_PCI_PWR_MGMT, val8);

  // There is no second Power Management Capability.
  ASSERT_EQ(ZX_ERR_NOT_FOUND,
            pci().GetNextCapability(PCI_CAPABILITY_ID_PCI_PWR_MGMT, offsetA, &offsetB));

  // First MSI Capability is at 0x68.
  ASSERT_OK(pci().GetFirstCapability(PCI_CAPABILITY_ID_MSI, &offsetA));
  ASSERT_EQ(0x68, offsetA);
  ASSERT_OK(pci().ReadConfig8(offsetA, &val8));
  ASSERT_EQ(PCI_CAPABILITY_ID_MSI, val8);

  // There is no second MSI Capability.
  ASSERT_EQ(ZX_ERR_NOT_FOUND, pci().GetNextCapability(PCI_CAPABILITY_ID_MSI, offsetA, &offsetB));

  // First Pci Express Capability is at 0x78.
  ASSERT_OK(pci().GetFirstCapability(PCI_CAPABILITY_ID_PCI_EXPRESS, &offsetA));
  ASSERT_EQ(0x78, offsetA);
  ASSERT_OK(pci().ReadConfig8(offsetA, &val8));
  ASSERT_EQ(PCI_CAPABILITY_ID_PCI_EXPRESS, val8);

  // There is no second Pci Express Capability.
  ASSERT_EQ(ZX_ERR_NOT_FOUND,
            pci().GetNextCapability(PCI_CAPABILITY_ID_PCI_EXPRESS, offsetA, &offsetB));

  // First Vendor Capability is at 0xC4.
  ASSERT_OK(pci().GetFirstCapability(PCI_CAPABILITY_ID_VENDOR, &offsetA));
  ASSERT_EQ(0xC4, offsetA);
  ASSERT_OK(pci().ReadConfig8(offsetA, &val8));
  ASSERT_EQ(PCI_CAPABILITY_ID_VENDOR, val8);

  // Second Vendor Capability is at 0xC8.
  ASSERT_OK(pci().GetNextCapability(PCI_CAPABILITY_ID_VENDOR, offsetA, &offsetB));
  ASSERT_EQ(0xC8, offsetB);
  ASSERT_OK(pci().ReadConfig8(offsetB, &val8));
  ASSERT_EQ(PCI_CAPABILITY_ID_VENDOR, val8);

  // Third Vendor Capability is at 0xD0.
  ASSERT_OK(pci().GetNextCapability(PCI_CAPABILITY_ID_VENDOR, offsetB, &offsetA));
  ASSERT_EQ(0xD0, offsetA);
  ASSERT_OK(pci().ReadConfig8(offsetA, &val8));
  ASSERT_EQ(PCI_CAPABILITY_ID_VENDOR, val8);

  // Fourth Vendor Capability is at 0xE8.
  ASSERT_OK(pci().GetNextCapability(PCI_CAPABILITY_ID_VENDOR, offsetA, &offsetB));
  ASSERT_EQ(0xE8, offsetB);
  ASSERT_OK(pci().ReadConfig8(offsetB, &val8));
  ASSERT_EQ(PCI_CAPABILITY_ID_VENDOR, val8);

  // There is no fifth Vendor Capability.
  ASSERT_EQ(ZX_ERR_NOT_FOUND, pci().GetNextCapability(PCI_CAPABILITY_ID_VENDOR, offsetB, &offsetA));

  // There is an MSIX capability at 0xF8
  ASSERT_OK(pci().GetFirstCapability(PCI_CAPABILITY_ID_MSIX, &offsetA));
  ASSERT_EQ(0xF0, offsetA);
  ASSERT_OK(pci().ReadConfig8(offsetA, &val8));
  ASSERT_EQ(PCI_CAPABILITY_ID_MSIX, val8);
}

TEST_F(PciProtocolTests, GetExtendedCapabilities) {
  uint16_t offsetA = 0;
  uint16_t offsetB = 0;
  uint16_t val16 = 0;

  // First extneded capability is Virtual Channel @ 0x100
  ASSERT_OK(pci().GetFirstExtendedCapability(PCI_EXTENDED_CAPABILITY_ID_VIRTUAL_CHANNEL_NO_MFVC,
                                             &offsetA));
  ASSERT_EQ(0x100, offsetA);
  ASSERT_OK(pci().ReadConfig16(offsetA, &val16));
  ASSERT_EQ(PCI_EXTENDED_CAPABILITY_ID_VIRTUAL_CHANNEL_NO_MFVC, val16);

  // There is no second Virtual Channel extended capability.
  ASSERT_EQ(ZX_ERR_NOT_FOUND, pci().GetNextExtendedCapability(
                                  PCI_EXTENDED_CAPABILITY_ID_VIRTUAL_CHANNEL, offsetA, &offsetB));

  // Latency Tolerance Reporting @ 0x250.
  ASSERT_OK(pci().GetFirstExtendedCapability(PCI_EXTENDED_CAPABILITY_ID_LATENCY_TOLERANCE_REPORTING,
                                             &offsetA));
  ASSERT_EQ(0x250, offsetA);
  ASSERT_OK(pci().ReadConfig16(offsetA, &val16));
  ASSERT_EQ(PCI_EXTENDED_CAPABILITY_ID_LATENCY_TOLERANCE_REPORTING, val16);

  // There is no second LTR extended capability.
  ASSERT_EQ(ZX_ERR_NOT_FOUND,
            pci().GetNextExtendedCapability(PCI_EXTENDED_CAPABILITY_ID_LATENCY_TOLERANCE_REPORTING,
                                            offsetA, &offsetB));

  // L1 PM Substates @ 0x258.
  ASSERT_OK(pci().GetNextExtendedCapability(PCI_EXTENDED_CAPABILITY_ID_L1PM_SUBSTATES, offsetA,
                                            &offsetA));
  ASSERT_EQ(0x258, offsetA);
  ASSERT_OK(pci().ReadConfig16(offsetA, &val16));
  ASSERT_EQ(PCI_EXTENDED_CAPABILITY_ID_L1PM_SUBSTATES, val16);

  // There is no second L1PM Substates extended capability.
  ASSERT_EQ(ZX_ERR_NOT_FOUND, pci().GetNextExtendedCapability(
                                  PCI_EXTENDED_CAPABILITY_ID_L1PM_SUBSTATES, offsetA, &offsetB));

  // Power Budgeting @ 0x128.
  ASSERT_OK(pci().GetFirstExtendedCapability(PCI_EXTENDED_CAPABILITY_ID_POWER_BUDGETING, &offsetA));
  ASSERT_EQ(0x128, offsetA);
  ASSERT_OK(pci().ReadConfig16(offsetA, &val16));
  ASSERT_EQ(PCI_EXTENDED_CAPABILITY_ID_POWER_BUDGETING, val16);

  // There is no second Power Budgeting extended capability.
  ASSERT_EQ(ZX_ERR_NOT_FOUND, pci().GetNextExtendedCapability(
                                  PCI_EXTENDED_CAPABILITY_ID_POWER_BUDGETING, offsetA, &offsetB));

  // Vendor Specific @ 0x128.
  ASSERT_OK(pci().GetFirstExtendedCapability(PCI_EXTENDED_CAPABILITY_ID_VENDOR, &offsetA));
  ASSERT_EQ(0x600, offsetA);
  ASSERT_OK(pci().ReadConfig16(offsetA, &val16));
  ASSERT_EQ(PCI_EXTENDED_CAPABILITY_ID_VENDOR, val16);

  // There is no second Vendor specific capability.
  ASSERT_EQ(ZX_ERR_NOT_FOUND,
            pci().GetNextExtendedCapability(PCI_EXTENDED_CAPABILITY_ID_VENDOR, offsetA, &offsetB));
}

TEST_F(PciProtocolTests, GetDeviceInfo) {
  uint16_t vendor_id;
  uint16_t device_id;
  uint8_t base_class;
  uint8_t sub_class;

  uint8_t program_interface;
  uint8_t revision_id;
  uint8_t bus_id = PCI_TEST_BUS_ID;
  uint8_t dev_id = PCI_TEST_DEV_ID;
  uint8_t func_id = PCI_TEST_FUNC_ID;

  ASSERT_OK(pci().ReadConfig16(PCI_CONFIG_VENDOR_ID, &vendor_id));
  ASSERT_OK(pci().ReadConfig16(PCI_CONFIG_DEVICE_ID, &device_id));
  ASSERT_EQ(vendor_id, PCI_TEST_DRIVER_VID);
  ASSERT_EQ(device_id, PCI_TEST_DRIVER_DID);
  ASSERT_OK(pci().ReadConfig8(PCI_CONFIG_CLASS_CODE_BASE, &base_class));
  ASSERT_OK(pci().ReadConfig8(PCI_CONFIG_CLASS_CODE_SUB, &sub_class));
  ASSERT_OK(pci().ReadConfig8(PCI_CONFIG_CLASS_CODE_INTR, &program_interface));
  ASSERT_OK(pci().ReadConfig8(PCI_CONFIG_REVISION_ID, &revision_id));

  pci_device_info_t info;
  ASSERT_OK(pci().GetDeviceInfo(&info));
  ASSERT_EQ(vendor_id, info.vendor_id);
  ASSERT_EQ(device_id, info.device_id);
  ASSERT_EQ(base_class, info.base_class);
  ASSERT_EQ(sub_class, info.sub_class);
  ASSERT_EQ(program_interface, info.program_interface);
  ASSERT_EQ(revision_id, info.revision_id);
  ASSERT_EQ(bus_id, info.bus_id);
  ASSERT_EQ(dev_id, info.dev_id);
  ASSERT_EQ(func_id, info.func_id);
}

// MSI-X interrupts should be bound by the platform support.
TEST_F(PciProtocolTests, MsiX) {
  pci_interrupt_modes_t modes{};
  pci().GetInterruptModes(&modes);
  ASSERT_EQ(modes.msix_count, kFakeQuadroMsiXIrqCnt);
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_MSI_X, modes.msix_count));
  {
    std::vector<zx::interrupt> ints;
    for (uint32_t i = 0; i < modes.msix_count; i++) {
      zx::interrupt interrupt = {};
      EXPECT_OK(pci().MapInterrupt(i, &interrupt));
      ints.push_back(std::move(interrupt));
    }
    EXPECT_STATUS(ZX_ERR_BAD_STATE, pci().SetInterruptMode(PCI_INTERRUPT_MODE_DISABLED, 0));
  }
  EXPECT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_DISABLED, 0));
}

// Ensure that bus mastering is enabled when requesting MSI modes.
TEST_F(PciProtocolTests, MsiEnablesBusMastering) {
  pci().SetBusMastering(false);
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_MSI, 1));
  uint16_t value = 0;
  ASSERT_OK(pci().ReadConfig16(PCI_CONFIG_COMMAND, &value));
  ASSERT_EQ(PCI_CONFIG_COMMAND_BUS_MASTER_EN, value & PCI_CONFIG_COMMAND_BUS_MASTER_EN);

  pci().SetBusMastering(false);
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_MSI_X, 1));
  ASSERT_OK(pci().ReadConfig16(PCI_CONFIG_COMMAND, &value));
  ASSERT_EQ(PCI_CONFIG_COMMAND_BUS_MASTER_EN, value & PCI_CONFIG_COMMAND_BUS_MASTER_EN);
}

// The Quadro card supports 4 MSI interrupts.
TEST_F(PciProtocolTests, GetAndSetInterruptMode) {
  pci::MsiControlReg msi_ctrl = {
      .value = *reinterpret_cast<uint16_t*>(
          &kFakeQuadroDeviceConfig[kFakeQuadroMsiCapabilityOffset + 2]),
  };

  pci_interrupt_modes_t modes{};
  pci().GetInterruptModes(&modes);
  EXPECT_EQ(modes.has_legacy, PCI_LEGACY_INT_COUNT);
  ASSERT_EQ(modes.msi_count, pci::MsiCapability::MmcToCount(msi_ctrl.mm_capable()));
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_LEGACY, 1));
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_LEGACY_NOACK, 1));
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_MSI, modes.msi_count));
  // Setting the same mode twice should work if no IRQs have been allocated off of this one.
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_MSI, modes.msi_count));
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_DISABLED, 0));
}

TEST_F(PciProtocolTests, GetInterruptModes) {
  pci::MsiControlReg msi_ctrl = {
      .value = *reinterpret_cast<uint16_t*>(
          &kFakeQuadroDeviceConfig[kFakeQuadroMsiCapabilityOffset + 2]),
  };

  pci_interrupt_modes_t modes;
  pci().GetInterruptModes(&modes);
  EXPECT_EQ(modes.has_legacy, PCI_LEGACY_INT_COUNT);
  EXPECT_EQ(modes.msi_count, pci::MsiCapability::MmcToCount(msi_ctrl.mm_capable()));
  EXPECT_EQ(modes.msix_count, kFakeQuadroMsiXIrqCnt);
}

// TODO(fxbug.dev/61631): Without USERSPACE_PCI defined in proxy it presently
// will always return the kernel implementation which avoids the channel call
// and returns ZX_OK. This needs to be re-enabled after the migration.
TEST_F(PciProtocolTests, DISABLED_AckingIrqModes) {
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_LEGACY, 1));
  ASSERT_OK(pci().AckInterrupt());
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_LEGACY_NOACK, 1));
  ASSERT_STATUS(ZX_ERR_BAD_STATE, pci().AckInterrupt());
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_MSI, 1));
  ASSERT_STATUS(ZX_ERR_BAD_STATE, pci().AckInterrupt());

  // Setting the same mode twice should work if no IRQs have been allocated off of this one.
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_MSI, 1));
  ASSERT_STATUS(ZX_ERR_BAD_STATE, pci().AckInterrupt());
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_DISABLED, 0));
  ASSERT_STATUS(ZX_ERR_BAD_STATE, pci().AckInterrupt());
}

const size_t kWaitDeadlineSecs = 5u;
bool WaitForThreadState(thrd_t thrd, zx_thread_state_t state) {
  zx_status_t status;
  zx_handle_t thread_handle = thrd_get_zx_handle(thrd);
  zx_info_thread_t info = {};
  zx::time deadline = zx::deadline_after(zx::sec(kWaitDeadlineSecs));
  while (zx::clock::get_monotonic() < deadline) {
    status =
        zx_object_get_info(thread_handle, ZX_INFO_THREAD, &info, sizeof(info), nullptr, nullptr);
    if (status == ZX_OK && info.state == state) {
      return true;
    }
    zx::nanosleep(zx::deadline_after(zx::usec(100)));
  }
  return false;
}

TEST_F(PciProtocolTests, MapInterrupt) {
  pci_interrupt_modes_t modes{};
  pci().GetInterruptModes(&modes);
  ASSERT_OK(pci().SetInterruptMode(PCI_INTERRUPT_MODE_MSI, modes.msi_count));
  zx::interrupt interrupt;
  for (uint32_t int_id = 0; int_id < modes.msi_count; int_id++) {
    ASSERT_OK(pci().MapInterrupt(int_id, &interrupt));
    ASSERT_STATUS(ZX_ERR_BAD_STATE,
                  pci().SetInterruptMode(PCI_INTERRUPT_MODE_MSI, modes.msi_count));

    // Verify that we can wait on the provided interrupt and that our thread
    // ends up in the correct state (that it was destroyed out from under it).
    thrd_t waiter_thrd;
    auto waiter_entry = [](void* arg) -> int {
      auto* interrupt = reinterpret_cast<zx::interrupt*>(arg);
      interrupt->wait(nullptr);
      return (interrupt->wait(nullptr) == ZX_ERR_CANCELED);
    };
    ASSERT_EQ(thrd_create(&waiter_thrd, waiter_entry, &interrupt), thrd_success);
    ASSERT_TRUE(WaitForThreadState(waiter_thrd, ZX_THREAD_STATE_BLOCKED_INTERRUPT));
    interrupt.destroy();
    int result;
    thrd_join(waiter_thrd, &result);
    ASSERT_TRUE(result);
    interrupt.reset();
  }

  // Invalid ids
  ASSERT_NOT_OK(pci().MapInterrupt(-1, &interrupt));
  ASSERT_NOT_OK(pci().MapInterrupt(modes.msi_count + 1, &interrupt));
  // Duplicate ids
  {
    zx::interrupt int_0, int_0_dup;
    ASSERT_OK(pci().MapInterrupt(0, &int_0));
    ASSERT_STATUS(pci().MapInterrupt(0, &int_0_dup), ZX_ERR_ALREADY_BOUND);
  }
}

TEST_F(PciProtocolTests, GetBti) {
  zx::bti bti;
  ASSERT_STATUS(pci().GetBti(0, &bti), ZX_ERR_NOT_SUPPORTED);
}
}  // namespace

void ProtocolTestDriver::RunTests(RunTestsCompleter::Sync& completer) {
  auto* driver = ProtocolTestDriver::GetInstance();
  auto* zxt = zxtest::Runner::GetInstance();
  zxt->AddObserver(driver);
  RUN_ALL_TESTS(0, nullptr);
  completer.Reply(ZX_OK, driver->report());
}

static zx_status_t pci_test_driver_bind(void* ctx, zx_device_t* parent) {
  zxtest::Runner::GetInstance()->mutable_reporter()->set_log_sink(std::make_unique<LogSink>());
  return ProtocolTestDriver::Create(parent);
}

static const zx_driver_ops_t protocol_test_driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = pci_test_driver_bind;
  return ops;
}();

// clang-format off
ZIRCON_DRIVER(pci_protocol_test_driver, protocol_test_driver_ops, "zircon", "0.1");
