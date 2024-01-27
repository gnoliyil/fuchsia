// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/bus/drivers/pci/device.h"

#include <assert.h>
#include <err.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <inttypes.h>
#include <lib/ddk/binding_driver.h>
#include <lib/fit/defer.h>
#include <lib/inspect/cpp/inspector.h>
#include <lib/zx/interrupt.h>
#include <string.h>
#include <zircon/compiler.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/time.h>
#include <zircon/types.h>

#include <array>
#include <optional>

#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <fbl/ref_ptr.h>
#include <fbl/string_buffer.h>

#include "src/devices/bus/drivers/pci/bus_device_interface.h"
#include "src/devices/bus/drivers/pci/capabilities/msi.h"
#include "src/devices/bus/drivers/pci/capabilities/msix.h"
#include "src/devices/bus/drivers/pci/capabilities/power_management.h"
#include "src/devices/bus/drivers/pci/common.h"
#include "src/devices/bus/drivers/pci/ref_counted.h"
#include "src/devices/bus/drivers/pci/upstream_node.h"

namespace pci {

namespace {  // anon namespace.  Externals do not need to know about DeviceImpl

class DeviceImpl : public Device {
 public:
  static zx_status_t Create(zx_device_t* parent, std::unique_ptr<Config>&& cfg,
                            UpstreamNode* upstream, BusDeviceInterface* bdi, inspect::Node node,
                            bool has_acpi);

  // Implement ref counting, do not let derived classes override.
  PCI_IMPLEMENT_REFCOUNTED;

  // Disallow copying, assigning and moving.
  DISALLOW_COPY_ASSIGN_AND_MOVE(DeviceImpl);

 protected:
  DeviceImpl(zx_device_t* parent, std::unique_ptr<Config>&& cfg, UpstreamNode* upstream,
             BusDeviceInterface* bdi, inspect::Node node, bool has_acpi)
      : Device(parent, std::move(cfg), upstream, bdi, std::move(node), /*is_bridge=*/false,
               has_acpi) {}
};

zx_status_t DeviceImpl::Create(zx_device_t* parent, std::unique_ptr<Config>&& cfg,
                               UpstreamNode* upstream, BusDeviceInterface* bdi, inspect::Node node,
                               bool has_acpi) {
  fbl::AllocChecker ac;
  auto raw_dev =
      new (&ac) DeviceImpl(parent, std::move(cfg), upstream, bdi, std::move(node), has_acpi);
  if (!ac.check()) {
    zxlogf(ERROR, "[%s] Out of memory attemping to create PCIe device.", cfg->addr());
    return ZX_ERR_NO_MEMORY;
  }

  auto dev = fbl::AdoptRef(static_cast<Device*>(raw_dev));
  const zx_status_t status = raw_dev->Init();
  if (status != ZX_OK) {
    zxlogf(ERROR, "[%s] Failed to initialize PCIe device: %s", dev->config()->addr(),
           zx_status_get_string(status));
    return status;
  }

  bdi->LinkDevice(dev);
  return ZX_OK;
}

}  // namespace

Device::Device(zx_device_t* parent, std::unique_ptr<Config>&& config, UpstreamNode* upstream,
               BusDeviceInterface* bdi, inspect::Node node, bool is_bridge, bool has_acpi)
    : cfg_(std::move(config)),
      upstream_(upstream),
      bdi_(bdi),
      bar_count_(is_bridge ? PCI_BAR_REGS_PER_BRIDGE : PCI_BAR_REGS_PER_DEVICE),
      is_bridge_(is_bridge),
      has_acpi_(has_acpi),
      parent_(parent),
      inspect_(std::move(node))

{}

Device::~Device() {
  // We should already be unlinked from the bus's device tree.
  ZX_DEBUG_ASSERT(disabled_);
  ZX_DEBUG_ASSERT(!plugged_in_);

  // Make certain that all bus access (MMIO, PIO, Bus mastering) has been
  // disabled and disable IRQs.
  DisableInterrupts();
  SetBusMastering(false);
  ModifyCmd(/*clr_bits=*/PCI_CONFIG_COMMAND_IO_EN | PCI_CONFIG_COMMAND_MEM_EN, /*set_bits=*/0);
  // TODO(cja/fxbug.dev/32979): Remove this after porting is finished.
  zxlogf(TRACE, "%s [%s] dtor finished", is_bridge() ? "bridge" : "device", cfg_->addr());
}

zx_status_t Device::Create(zx_device_t* parent, std::unique_ptr<Config>&& config,
                           UpstreamNode* upstream, BusDeviceInterface* bdi, inspect::Node node,
                           bool has_acpi) {
  return DeviceImpl::Create(parent, std::move(config), upstream, bdi, std::move(node), has_acpi);
}

zx_status_t Device::Init() {
  const fbl::AutoLock dev_lock(&dev_lock_);

  const zx_status_t status = InitLocked();
  if (status != ZX_OK) {
    zxlogf(ERROR, "failed to initialize device %s: %d", cfg_->addr(), status);
    return status;
  }

  // Things went well and the device is in a good state. Flag the device as
  // plugged in and link ourselves up to the graph. This will keep the device
  // alive as long as the Bus owns it.
  upstream_->LinkDevice(this);
  plugged_in_ = true;

  return status;
}

zx_status_t Device::InitInterrupts() {
  zx_status_t status = zx::interrupt::create(*zx::unowned_resource(ZX_HANDLE_INVALID), 0,
                                             ZX_INTERRUPT_VIRTUAL, &irqs_.legacy);
  if (status != ZX_OK) {
    zxlogf(ERROR, "device %s could not create its legacy interrupt: %s", cfg_->addr(),
           zx_status_get_string(status));
    return status;
  }

  // Disable all interrupt modes until a driver enables the preferred method.
  // The legacy interrupt is disabled by hand because our Enable/Disable methods
  // for doing so need to interact with the Shared IRQ lists in Bus.
  ModifyCmdLocked(/*clr_bits=*/0, /*set_bits=*/PCIE_CFG_COMMAND_INT_DISABLE);
  irqs_.legacy_vector = 0;

  if (caps_.msi) {
    status = DisableMsi();
    if (status != ZX_OK) {
      zxlogf(ERROR, "failed to disable MSI: %s", zx_status_get_string(status));
      return status;
    }
  }

  if (caps_.msix) {
    status = DisableMsix();
    if (status != ZX_OK) {
      zxlogf(ERROR, "failed to disable MSI-X: %s", zx_status_get_string(status));
      return status;
    }
  }

  irqs_.mode = fuchsia_hardware_pci::InterruptMode::kDisabled;
  return ZX_OK;
}

zx_status_t Device::InitLocked() {
  // Cache basic device info
  vendor_id_ = cfg_->Read(Config::kVendorId);
  device_id_ = cfg_->Read(Config::kDeviceId);
  class_id_ = cfg_->Read(Config::kBaseClass);
  subclass_ = cfg_->Read(Config::kSubClass);
  prog_if_ = cfg_->Read(Config::kProgramInterface);
  rev_id_ = cfg_->Read(Config::kRevisionId);

  // Disable the device in event of a failure initializing. TA is disabled
  // because it cannot track the scope of AutoCalls and their associated
  // locking semantics. The lock is grabbed by |Init| and held at this point.
  auto disable = fit::defer([this]() __TA_NO_THREAD_SAFETY_ANALYSIS { DisableLocked(); });

  // Parse and sanity check the capabilities and extended capabilities lists
  // if they exist
  zx_status_t st = ProbeCapabilities();
  if (st != ZX_OK) {
    zxlogf(ERROR, "device %s encountered an error parsing capabilities: %d", cfg_->addr(), st);
    return st;
  }

  ProbeBars();

  // Now that we know what our capabilities are, initialize our internal IRQ
  // bookkeeping and disable all interrupts until a driver requests them.
  st = InitInterrupts();
  if (st != ZX_OK) {
    return st;
  }

  // Power the device on by transitioning to the highest power level if possible
  // and necessary.
  if (caps_.power) {
    if (auto state = caps_.power->GetPowerState(*cfg_);
        state != PowerManagementCapability::PowerState::D0) {
      zxlogf(DEBUG, "[%s] transitioning power state from D%d to D0", cfg_->addr(), state);
      caps_.power->SetPowerState(*cfg_, PowerManagementCapability::PowerState::D0);
    }
  }

  zx::result result = FidlDevice::Create(parent_, this);
  if (result.is_error()) {
    return result.status_value();
  }

  disable.cancel();
  return ZX_OK;
}

zx_status_t Device::ModifyCmd(uint16_t clr_bits, uint16_t set_bits) {
  const fbl::AutoLock dev_lock(&dev_lock_);
  // In order to keep internal bookkeeping coherent, and interactions between
  // MSI/MSI-X and Legacy IRQ mode safe, API users may not directly manipulate
  // the legacy IRQ enable/disable bit.  Just ignore them if they try to
  // manipulate the bit via the modify cmd API.
  // TODO(cja) This only applies to PCI(e)
  clr_bits = static_cast<uint16_t>(clr_bits & ~PCIE_CFG_COMMAND_INT_DISABLE);
  set_bits = static_cast<uint16_t>(set_bits & ~PCIE_CFG_COMMAND_INT_DISABLE);

  if (plugged_in_) {
    ModifyCmdLocked(clr_bits, set_bits);
    return ZX_OK;
  }

  return ZX_ERR_UNAVAILABLE;
}

void Device::ModifyCmdLocked(uint16_t clr_bits, uint16_t set_bits) {
  fbl::AutoLock cmd_reg_lock(&cmd_reg_lock_);
  cfg_->Write(Config::kCommand,
              static_cast<uint16_t>((cfg_->Read(Config::kCommand) & ~clr_bits) | set_bits));
}

void Device::Disable() {
  const fbl::AutoLock dev_lock(&dev_lock_);
  DisableLocked();
}

void Device::DisableLocked() {
  // Disable a device because we cannot allocate space for all of its BARs (or
  // forwarding windows, in the case of a bridge).  Flag the device as
  // disabled from here on out.
  zxlogf(TRACE, "[%s] %s %s", cfg_->addr(), (is_bridge()) ? " (b)" : "", __func__);

  // Flag the device as disabled.  Close the device's MMIO/PIO windows, shut
  // off device initiated accesses to the bus, disable legacy interrupts.
  // Basically, prevent the device from doing anything from here on out.
  disabled_ = true;
  AssignCmdLocked(PCIE_CFG_COMMAND_INT_DISABLE);

  // Release all BAR allocations back into the pool they came from.
  for (auto& bar : bars_) {
    bar.reset();
  }
}

zx_status_t Device::SetBusMastering(bool enabled) {
  // Only allow bus mastering to be turned off if the device is disabled.
  if (enabled && disabled_) {
    return ZX_ERR_BAD_STATE;
  }

  ModifyCmdLocked(enabled ? /*clr_bits=*/0 : /*set_bits=*/PCI_CONFIG_COMMAND_BUS_MASTER_EN,
                  enabled ? /*clr_bits=*/PCI_CONFIG_COMMAND_BUS_MASTER_EN : /*set_bits=*/0);
  return upstream_->SetBusMasteringUpstream(enabled);
}

// Configures the BAR represented by |bar| by writing to its register and configuring
// IO and Memory access accordingly.
zx_status_t Device::WriteBarInformation(const Bar& bar) {
  // Now write the allocated address space to the BAR.
  uint16_t cmd_backup = cfg_->Read(Config::kCommand);
  // Figure out the IO type of the bar and disable that while we adjust the bar address.
  uint16_t mem_io_en_flag = (bar.is_mmio) ? PCI_CONFIG_COMMAND_MEM_EN : PCI_CONFIG_COMMAND_IO_EN;
  ModifyCmdLocked(mem_io_en_flag, cmd_backup);

  cfg_->Write(Config::kBar(bar.bar_id), static_cast<uint32_t>(bar.address));
  if (bar.is_64bit) {
    const uint32_t addr_hi = static_cast<uint32_t>(bar.address >> 32);
    cfg_->Write(Config::kBar(bar.bar_id + 1), addr_hi);
  }
  // Flip the IO bit back on for this type of bar
  AssignCmdLocked(cmd_backup | mem_io_en_flag);
  return ZX_OK;
}

zx::result<> Device::ProbeBar(uint8_t bar_id) {
  if (bar_id >= bar_count_) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  Bar bar{};
  uint32_t bar_val = cfg_->Read(Config::kBar(bar_id));

  bar.bar_id = bar_id;
  bar.is_mmio = (bar_val & PCI_BAR_IO_TYPE_MASK) == PCI_BAR_IO_TYPE_MMIO;
  bar.is_64bit = bar.is_mmio && ((bar_val & PCI_BAR_MMIO_TYPE_MASK) == PCI_BAR_MMIO_TYPE_64BIT);
  bar.is_prefetchable = bar.is_mmio && (bar_val & PCI_BAR_MMIO_PREFETCH_MASK);
  const uint32_t addr_mask = (bar.is_mmio) ? PCI_BAR_MMIO_ADDR_MASK : PCI_BAR_PIO_ADDR_MASK;

  // Check the read-only configuration of the BAR. If it's invalid then don't add it to our BAR
  // list.
  if (bar.is_64bit && (bar.bar_id == bar_count_ - 1)) {
    zxlogf(ERROR, "[%s] has a 64bit bar in invalid position %u!", cfg_->addr(), bar.bar_id);
    return zx::error(ZX_ERR_BAD_STATE);
  }

  if (bar.is_64bit && !bar.is_mmio) {
    zxlogf(ERROR, "[%s] bar %u is 64bit but not mmio!", cfg_->addr(), bar.bar_id);
    return zx::error(ZX_ERR_BAD_STATE);
  }

  // Disable MMIO & PIO access while we perform the probe. We don't want the
  // addresses written during probing to conflict with anything else on the
  // bus. Note: No drivers should have access to this device's registers
  // during the probe process as the device should not have been published
  // yet. That said, there could be other (special case) parts of the system
  // accessing a devices registers at this point in time, like an early init
  // debug console or serial port. Don't make any attempt to print or log
  // until the probe operation has been completed. Hopefully these special
  // systems are quiescent at this point in time, otherwise they might see
  // some minor glitching while access is disabled.
  uint16_t cmd_backup = ReadCmdLocked();
  bool enabled = !!(cmd_backup & (PCI_CONFIG_COMMAND_MEM_EN | PCI_CONFIG_COMMAND_IO_EN));
  if (enabled) {
    ModifyCmdLocked(/*clr_bits=*/PCI_CONFIG_COMMAND_MEM_EN | PCI_CONFIG_COMMAND_IO_EN,
                    /*set_bits=*/cmd_backup);
    // For enabled devices save the original address in the BAR. If the device
    // is enabled then we should assume the bios configured it and we should
    // attempt to retain the BAR allocation.
    bar.address = bar_val & addr_mask;
  }

  // Write ones to figure out the size of the BAR
  cfg_->Write(Config::kBar(bar_id), UINT32_MAX);
  bar_val = cfg_->Read(Config::kBar(bar_id));
  // BARs that are not wired up return all zeroes on read after probing.
  if (bar_val == 0) {
    return zx::ok();
  }

  uint64_t size_mask = ~(bar_val & addr_mask);
  if (bar.is_mmio && bar.is_64bit) {
    // Retain the high 32bits of the 64bit address address if the device was
    // enabled already.
    if (enabled) {
      bar.address |= static_cast<uint64_t>(cfg_->Read(Config::kBar(bar_id + 1))) << 32;
    }

    // Get the high 32 bits of size for the 64 bit BAR by repeating the
    // steps of writing 1s and then reading the value of the next BAR.
    cfg_->Write(Config::kBar(bar_id + 1), UINT32_MAX);
    size_mask |= static_cast<uint64_t>(~cfg_->Read(Config::kBar(bar_id + 1))) << 32;
  } else if (!bar.is_mmio && !(bar_val & (UINT16_MAX << 16))) {
    // Per spec, if the type is IO and the upper 16 bits were zero in the read
    // then they should be removed from the size mask before incrementing it.
    size_mask &= UINT16_MAX;
  }
  InspectRecordBarInitialState(bar_id, bar.address);

  // No matter what configuration we've found, |size_mask| should contain a
  // mask representing all the valid bits that can be set in the address.
  bar.size = size_mask + 1;

  // Write the original address value we had before probing and re-enable its
  // access mode now that probing is complete.
  WriteBarInformation(bar);

  InspectRecordBarProbedState(bar_id, bar);
  bars_[bar_id] = std::move(bar);
  return zx::ok();
}

void Device::ProbeBars() {
  for (uint32_t bar_id = 0; bar_id < bar_count_; bar_id++) {
    auto result = ProbeBar(bar_id);
    if (result.is_error()) {
      zxlogf(ERROR, "[%s] Skipping bar %u due to probing error: %s", cfg_->addr(), bar_id,
             result.status_string());
      continue;
    }

    // If the bar was probed as 64 bit then mark then we can just skip the next bar.
    if (bars_[bar_id] && bars_[bar_id]->is_64bit) {
      bar_id++;
    }
  }
}

// Allocates appropriate address space for BAR |bar| out of any suitable
// upstream allocators, using |base| as the base address if present.
zx::result<std::unique_ptr<PciAllocation>> Device::AllocateFromUpstream(
    const Bar& bar, std::optional<zx_paddr_t> base) {
  ZX_DEBUG_ASSERT(bar.size > 0);
  zx_paddr_t start = base.value_or(0);

  // On all platforms if a BAR is not marked in its register as MMIO then it
  // goes through the Root Host IO/PIO allocator, regardless of whether the
  // platform's IO is actually MMIO backed.
  if (!bar.is_mmio) {
    return upstream_->pio_regions().Allocate(base, bar.size);
  }

  // If a BAR is prefetchable and we're attached to a bridge then the only
  // allocation option available is to use the PF-MMIO window. Otherwise, when
  // attached to a root we can use either MMIO allocator.
  if (upstream_->type() == pci::UpstreamNode::Type::BRIDGE && bar.is_prefetchable) {
    return upstream_->pf_mmio_regions().Allocate(base, bar.size);
  }

  // If the allocation fits within the low MMIO window then attempt to allocate
  // it there. Ensure it can't cross the low to high boundary between 4GB and
  // beyond. Any prefetchable BARs at this point are downstream of a root so it
  // doesn't matter which allocator we use for prefetchability specifically.
  // It's worth noting that if a BAR did not have an existing allocation then
  // its start address will be 0, so we'll always try to allocate from the low
  // MMIO allocator first in that case.
  zx_paddr_t end_offset = 0;
  if (!add_overflow(start, bar.size - 1, &end_offset) &&
      end_offset <= std::numeric_limits<uint32_t>::max()) {
    if (auto result = upstream_->mmio_regions().Allocate(base, bar.size); result.is_ok()) {
      return result.take_value();
    }
  }

  // Otherwise, try to use the high MMIO allocator.
  return upstream_->pf_mmio_regions().Allocate(base, bar.size);
}

// Higher level method to allocate address space a previously probed BAR id
// |bar_id| and handle configuration space setup.
zx::result<> Device::AllocateBar(uint8_t bar_id) {
  ZX_DEBUG_ASSERT(upstream_);
  ZX_DEBUG_ASSERT(bar_id < bar_count_);
  ZX_DEBUG_ASSERT(bars_[bar_id].has_value());

  Bar& bar = *bars_[bar_id];
  // First try to allocate any address that we found during the probe. If it
  // fails then log it because it most likely failed due to an expected address
  // region being in use already. If the address is zero due to being
  // uninitialized when PCI comes up then we can skip this step because we know
  // we will never be able to allocate from address 0 in any address space type.
  zx::result<std::unique_ptr<PciAllocation>> result;
  if (bar.address) {
    result = AllocateFromUpstream(bar, bar.address);
    if (!result.is_ok()) {
      InspectRecordBarFailure(bar_id, {bar.address, bar.size});
    }
  }

  // If the previous allocation failed, or result has been unused, then try to
  // reallocate from any allocator at any location.
  if (!result.is_ok()) {
    result = AllocateFromUpstream(bar, std::nullopt);
  }

  if (result.is_error()) {
    return zx::error(ZX_ERR_NOT_FOUND);
  }

  InspectRecordBarAllocation(bar_id, {result.value()->base(), result.value()->size()});
  bar.allocation = std::move(result.value());
  bar.address = bar.allocation->base();
  WriteBarInformation(bar);
  InspectRecordBarConfiguredState(bar_id, cfg_->Read(Config::kBar(bar_id)));

  return zx::ok();
}

zx::result<> Device::AllocateBars() {
  const fbl::AutoLock dev_lock(&dev_lock_);
  ZX_DEBUG_ASSERT(plugged_in_);
  ZX_DEBUG_ASSERT(bar_count_ <= bars_.max_size());

  // Allocate BARs for the device
  for (uint32_t bar_id = 0; bar_id < bar_count_; bar_id++) {
    if (bars_[bar_id]) {
      if (auto result = AllocateBar(bar_id); result.is_error()) {
        zxlogf(ERROR, "[%s] failed to allocate bar %u: %s", cfg_->addr(), bar_id,
               result.status_string());
        return result.take_error();
      }
    }
  }

  return zx::ok();
}

zx::result<PowerManagementCapability::PowerState> Device::GetPowerState() {
  const fbl::AutoLock dev_lock(&dev_lock_);
  if (!caps_.power) {
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }

  return zx::ok(caps_.power->GetPowerState(*cfg_));
}

void Device::Unplug() {
  zxlogf(TRACE, "[%s] %s %s", cfg_->addr(), (is_bridge()) ? " (b)" : "", __func__);
  const fbl::AutoLock dev_lock(&dev_lock_);
  // Disable should have been called before Unplug and would have disabled
  // everything in the command register
  ZX_DEBUG_ASSERT(disabled_);
  upstream_->UnlinkDevice(this);
  // After unplugging from the Bus there should be no further references to this
  // device and the dtor will be called.
  bdi_->UnlinkDevice(this);
  plugged_in_ = false;
  zxlogf(TRACE, "device [%s] unplugged", cfg_->addr());
}

}  // namespace pci
