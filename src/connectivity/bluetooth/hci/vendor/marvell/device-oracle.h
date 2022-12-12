// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_HCI_VENDOR_MARVELL_DEVICE_ORACLE_H_
#define SRC_CONNECTIVITY_BLUETOOTH_HCI_VENDOR_MARVELL_DEVICE_ORACLE_H_

#include <lib/zx/result.h>
#include <stdint.h>

#include <memory>

namespace bt_hci_marvell {

enum class DeviceType {
  k88W8987,
};

// Value that indicates that firmware has been successfully loaded
constexpr uint16_t kFirmwareStatusReady = 0xfedc;

// Value to set in RSR register for interrupts to clear-on-read
constexpr uint8_t kRsrClearOnReadMask = 0x3f;
constexpr uint8_t kRsrClearOnReadValue = 0x3f;

// Value to set in MiscCfg register for interrupts to automatically re-enable
constexpr uint8_t kMiscCfgAutoReenableMask = 0x10;
constexpr uint8_t kMiscCfgAutoReenableValue = 0x10;

// A DeviceOracle provides functions to retrieve values that are specific to a product.
class DeviceOracle {
 public:
  // Factory method
  static zx::result<std::unique_ptr<DeviceOracle>> Create(uint32_t pid);

  DeviceOracle() = delete;

  uint16_t GetSdioBlockSize() const;
  uint32_t GetRegAddrFirmwareStatus() const;
  uint32_t GetRegAddrInterruptRsr() const;
  uint32_t GetRegAddrIoportAddr() const;
  uint32_t GetRegAddrMiscCfg() const;

 private:
  explicit DeviceOracle(enum DeviceType device_type) : device_type_(device_type) {}

  enum DeviceType device_type_;
};

}  // namespace bt_hci_marvell

#endif  // SRC_CONNECTIVITY_BLUETOOTH_HCI_VENDOR_MARVELL_DEVICE_ORACLE_H_
