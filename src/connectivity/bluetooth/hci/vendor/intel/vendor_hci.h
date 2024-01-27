// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_HCI_VENDOR_INTEL_VENDOR_HCI_H_
#define SRC_CONNECTIVITY_BLUETOOTH_HCI_VENDOR_INTEL_VENDOR_HCI_H_

#include <lib/zx/channel.h>

#include <queue>

#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/device_address.h"
#include "src/connectivity/bluetooth/core/bt-host/common/macros.h"
#include "src/connectivity/bluetooth/core/bt-host/hci-spec/protocol.h"
#include "src/connectivity/bluetooth/core/bt-host/transport/control_packets.h"

namespace btintel {

constexpr bt::hci_spec::OpCode kReadVersion = bt::hci_spec::VendorOpCode(0x0005);

struct ReadVersionReturnParams {
  pw::bluetooth::emboss::StatusCode status;
  uint8_t hw_platform;
  uint8_t hw_variant;
  uint8_t hw_revision;
  uint8_t fw_variant;
  uint8_t fw_revision;
  uint8_t fw_build_num;
  uint8_t fw_build_week;
  uint8_t fw_build_year;
  uint8_t fw_patch_num;
} __PACKED;

constexpr uint8_t kBootloaderFirmwareVariant = 0x06;
constexpr uint8_t kFirmwareFirmwareVariant = 0x23;

constexpr bt::hci_spec::OpCode kLoadPatch = bt::hci_spec::VendorOpCode(0x008e);

constexpr bt::hci_spec::OpCode kSecureSend = bt::hci_spec::VendorOpCode(0x0009);

constexpr bt::hci_spec::OpCode kReadBootParams = bt::hci_spec::VendorOpCode(0x000D);

struct ReadBootParamsReturnParams {
  pw::bluetooth::emboss::StatusCode status;
  uint8_t otp_format;
  uint8_t otp_content;
  uint8_t otp_patch;
  uint16_t dev_revid;
  pw::bluetooth::emboss::GenericEnableParam secure_boot;
  uint8_t key_from_hdr;
  uint8_t key_type;
  pw::bluetooth::emboss::GenericEnableParam otp_lock;
  pw::bluetooth::emboss::GenericEnableParam api_lock;
  pw::bluetooth::emboss::GenericEnableParam debug_lock;
  bt::DeviceAddressBytes otp_bdaddr;
  uint8_t min_fw_build_num;
  uint8_t min_fw_build_week;
  uint8_t min_fw_build_year;
  pw::bluetooth::emboss::GenericEnableParam limited_cce;
  uint8_t unlocked_state;
} __PACKED;

constexpr bt::hci_spec::OpCode kReset = bt::hci_spec::VendorOpCode(0x0001);

struct ResetCommandParams {
  uint8_t reset_type;
  uint8_t patch_enable;
  uint8_t ddc_reload;
  uint8_t boot_option;
  uint32_t boot_address;
} __PACKED;

constexpr bt::hci_spec::OpCode kMfgModeChange = bt::hci_spec::VendorOpCode(0x0011);

enum class MfgDisableMode : uint8_t {
  kNoPatches = 0x00,
  kPatchesDisabled = 0x01,
  kPatchesEnabled = 0x02,
};

struct MfgModeChangeCommandParams {
  pw::bluetooth::emboss::GenericEnableParam enable;
  MfgDisableMode disable_mode;
} __PACKED;

struct SecureSendEventParams {
  uint8_t vendor_event_code;
  uint8_t result;
  uint16_t opcode;
  uint8_t status;
} __PACKED;

struct BootloaderVendorEventParams {
  BootloaderVendorEventParams() = delete;
  BT_DISALLOW_COPY_ASSIGN_AND_MOVE(BootloaderVendorEventParams);

  uint8_t vendor_event_code;
  uint8_t vendor_params[];
} __PACKED;

class VendorHci {
 public:
  explicit VendorHci(zx::channel* ctrl);

  // When |acl| is not nullptr, WaitForEventPacket will wait on both control and
  // ACL channels.
  void enable_events_on_bulk(zx::channel* acl) { acl_ = acl; }

  ReadVersionReturnParams SendReadVersion() const;

  ReadBootParamsReturnParams SendReadBootParams() const;

  pw::bluetooth::emboss::StatusCode SendHciReset() const;

  void SendVendorReset(uint32_t boot_address) const;

  bool SendSecureSend(uint8_t type, const bt::BufferView& bytes) const;

  bool SendAndExpect(const bt::PacketView<bt::hci_spec::CommandHeader>& command,
                     std::deque<bt::BufferView> events) const;

  void EnterManufacturerMode();

  bool ExitManufacturerMode(MfgDisableMode mode);

 private:
  // The control and ACL (interrupt and bulk on USB) endpoints of the
  // controller. Intel controllers that support the "secure send" can send
  // vendor events over the bulk endpoint while in bootloader mode. We listen on
  // incoming events on both channels, if provided.
  zx::channel* ctrl_;
  zx::channel* acl_;

  // True when we are in Manufacturer Mode
  bool manufacturer_;

  void SendCommand(const bt::PacketView<bt::hci_spec::CommandHeader>& command) const;

  std::unique_ptr<bt::hci::EventPacket> WaitForEventPacket(
      zx::duration timeout = zx::sec(5), bt::hci_spec::EventCode expected_event = 0) const;
};

}  // namespace btintel

#endif  // SRC_CONNECTIVITY_BLUETOOTH_HCI_VENDOR_INTEL_VENDOR_HCI_H_
