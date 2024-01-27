// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_DEVICES_PCI_TESTING_PCI_PROTOCOL_FAKE_H_
#define SRC_DEVICES_PCI_TESTING_PCI_PROTOCOL_FAKE_H_

#include <fidl/fuchsia.hardware.pci/cpp/wire_test_base.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async-loop/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/device-protocol/pci.h>
#include <lib/sync/completion.h>
#include <lib/zx/bti.h>
#include <lib/zx/interrupt.h>
#include <zircon/errors.h>
#include <zircon/status.h>

#include <array>
#include <optional>
#include <vector>

#include <fbl/algorithm.h>

#include "src/devices/pci/testing/protocol/internal.h"

namespace pci {

// FakePciProtocol provides a PciProtocol implementation that can be configured
// to match the layout of a given PCI device. It implements a PCI FIDL server
// and can be trivially constructed for tests. All public methods are safe to
// use and it has been written in mind to validate correctness of the
// configuration space whenever possible as well as to behave similarly to the
// actual PciProtocol driver proved by the userspace PCI Bus Driver.
class FakePciProtocol : public FakePciProtocolInternal,
                        public fidl::testing::WireTestBase<fuchsia_hardware_pci::Device> {
 public:
  // Add an interrupt for the specified PCI interrupt mode. A reference to the
  // interrupt object created is returned.
  zx::interrupt& AddLegacyInterrupt() {
    return AddInterrupt(fuchsia_hardware_pci::InterruptMode::kLegacy);
  }
  zx::interrupt& AddMsiInterrupt() {
    return AddInterrupt(fuchsia_hardware_pci::InterruptMode::kMsi);
  }
  zx::interrupt& AddMsixInterrupt() {
    return AddInterrupt(fuchsia_hardware_pci::InterruptMode::kMsiX);
  }

  // Sets the structure returned by |PciGetDeviceInfo|.
  fuchsia_hardware_pci::wire::DeviceInfo SetDeviceInfo(
      fuchsia_hardware_pci::wire::DeviceInfo info) {
    return SetDeviceInfoInternal(info);
  }

  // Adds a vendor capability of size |size| to the device at |position| in PCI Configuration Space.
  void AddVendorCapability(uint8_t position, uint8_t size) {
    ZX_ASSERT_MSG(
        size > 2,
        "FakePciProtocol Error: a vendor capability must be at least size 0x3 (size = %#x).", size);
    AddCapabilityInternal(fuchsia_hardware_pci::CapabilityId::kVendor, position, size);
    // Vendor capabilities store a size at the byte following the next pointer.
    config().write(&size, position + 2, sizeof(size));
  }

  // Adds a PCI Express capability at |position|.
  // No registers are configured, but most devices that check for this
  // capability do so just to understand the configuration space they have
  // available, not to actually attempt to modify this capability.
  void AddPciExpressCapability(uint8_t position) {
    AddCapabilityInternal(fuchsia_hardware_pci::CapabilityId::kPciExpress, position,
                          FakePciProtocolInternal::kPciExpressCapabilitySize);
  }

  // Adds a capability of a given type corresponding to |capability_id| at the
  // specified position. This is only recommended for drivers that check for
  // existence of a capability rather than those that expect to read and write
  // from one. For MSI and MSI-X capabilities you should use the interrupt
  // methods to add interrupts. For a PCI Express capability you should use
  // AddPciExpressCapability instead.
  void AddCapability(uint8_t capability_id, uint8_t position, uint8_t size) {
    AddCapabilityInternal(capability_id, position, size);
  }

  // Creates a BAR corresponding to the provided |bar_id| of the requested |size| and returns
  // a reference to the VMO backing its mapped region. |is_mmio| determines whether the bar
  // is MMIO or IO backed. The caller is responsible for mocking/faking the IO access in their
  // driver.
  zx::vmo& CreateBar(uint32_t bar_id, size_t size, bool is_mmio) {
    ZX_ASSERT_MSG(bar_id < PCI_DEVICE_BAR_COUNT,
                  "FakePciProtocol Error: valid BAR ids are [0, 5] (bar_id = %u)", bar_id);
    zx::vmo vmo;
    zx_status_t status = zx::vmo::create(size, /*options=*/0, &vmo);
    ZX_ASSERT_MSG(status == ZX_OK,
                  "FakePciProtocol Error: failed to create VMO for bar (bar_id = %u, size = %#zx, "
                  "status = %s)",
                  bar_id, size, zx_status_get_string(status));
    status = vmo.set_cache_policy(ZX_CACHE_POLICY_UNCACHED_DEVICE);
    ZX_ASSERT_MSG(
        status == ZX_OK,
        "FakePciProtocol Error: failed to set cache policy for BAR (bar_id = %u, status = %s)",
        status, zx_status_get_string(status));
    bars()[bar_id].type = (is_mmio) ? PCI_BAR_TYPE_MMIO : PCI_BAR_TYPE_IO;
    bars()[bar_id].size = size;
    bars()[bar_id].vmo = std::move(vmo);
    return GetBar(bar_id);
  }

  // Returns a reference to the VMO backing a given BAR id |bar_id|.
  zx::vmo& GetBar(uint32_t bar_id) {
    ZX_ASSERT_MSG(bar_id < PCI_DEVICE_BAR_COUNT,
                  "FakePciProtocol Error: valid BAR ids are [0, 5] (bar_id = %u)", bar_id);
    ZX_ASSERT_MSG(bars()[bar_id].vmo.has_value(), "FakePciProtocol Error: BAR %u has not been set.",
                  bar_id);
    return *bars()[bar_id].vmo;
  }

  // Returns a VMO corresponding to the device's configuration space.
  zx::unowned_vmo GetConfigVmo() { return config().borrow(); }
  // Returns the presently configured interrupt mode.
  fuchsia_hardware_pci::InterruptMode GetIrqMode() const { return irq_mode(); }
  // Returns the present number of interrupts configured by |PciSetInterruptMode|.
  uint32_t GetIrqCount() const { return irq_cnt(); }
  // Returns how many times |PciResetDevice| has been called.
  uint32_t GetResetCount() const { return reset_cnt(); }

  // Returns the state of the device's Bus Mastering setting. If std::nullopt is returned
  // then |PciSetBusMastering| was never called. Returned as an optional
  // so that the caller can differentiate between off and "never set" states in driver
  // testing.
  std::optional<bool> GetBusMasterEnabled() const { return bus_master_en(); }

  // Reset all internal state of the fake PciProtocol.
  void Reset() { reset(); }

  // FIDL methods
  void GetBar(GetBarRequestView request, GetBarCompleter::Sync& completer) override;
  void GetDeviceInfo(GetDeviceInfoCompleter::Sync& completer) override;
  void GetBti(GetBtiRequestView request, GetBtiCompleter::Sync& completer) override;
  void WriteConfig8(WriteConfig8RequestView request,
                    WriteConfig8Completer::Sync& completer) override;
  void WriteConfig16(WriteConfig16RequestView request,
                     WriteConfig16Completer::Sync& completer) override;
  void WriteConfig32(WriteConfig32RequestView request,
                     WriteConfig32Completer::Sync& completer) override;
  void ReadConfig8(ReadConfig8RequestView request, ReadConfig8Completer::Sync& completer) override;
  void ReadConfig16(ReadConfig16RequestView request,
                    ReadConfig16Completer::Sync& completer) override;
  void ReadConfig32(ReadConfig32RequestView request,
                    ReadConfig32Completer::Sync& completer) override;
  void GetInterruptModes(GetInterruptModesCompleter::Sync& completer) override;
  void SetInterruptMode(SetInterruptModeRequestView request,
                        SetInterruptModeCompleter::Sync& completer) override;
  void MapInterrupt(MapInterruptRequestView request,
                    MapInterruptCompleter::Sync& completer) override;
  void GetCapabilities(GetCapabilitiesRequestView request,
                       GetCapabilitiesCompleter::Sync& completer) override;
  void SetBusMastering(SetBusMasteringRequestView request,
                       SetBusMasteringCompleter::Sync& completer) override;
  void ResetDevice(ResetDeviceCompleter::Sync& completer) override;
  void AckInterrupt(AckInterruptCompleter::Sync& completer) override;

  void NotImplemented_(const std::string& name, fidl::CompleterBase& completer) final {
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  // Convenience method to set up a FIDL server in tests.
  ddk::Pci SetUpFidlServer(async::Loop& loop);
};

// Any test code which calls a helper method on FakePciProtocol must be executed
// an async task, because FakePciProtocol is a FIDL server running on another
// thread.
void RunAsync(async::Loop& loop, fit::closure f);

}  // namespace pci

#endif  // SRC_DEVICES_PCI_TESTING_PCI_PROTOCOL_FAKE_H_
