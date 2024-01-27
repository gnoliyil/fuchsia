// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_DEVICES_BUS_DRIVERS_PCI_DEVICE_H_
#define SRC_DEVICES_BUS_DRIVERS_PCI_DEVICE_H_

#include <assert.h>
#include <fidl/fuchsia.hardware.pci/cpp/wire.h>
#include <fidl/fuchsia.hardware.pci/cpp/wire_types.h>
#include <fuchsia/hardware/pci/c/banjo.h>
#include <fuchsia/hardware/pci/cpp/banjo.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/fdf/cpp/dispatcher.h>
#include <lib/inspect/cpp/inspector.h>
#include <lib/zx/channel.h>
#include <lib/zx/result.h>
#include <sys/types.h>
#include <zircon/compiler.h>
#include <zircon/errors.h>

#include <limits>

#include <ddktl/device.h>
#include <ddktl/unbind-txn.h>
#include <fbl/algorithm.h>
#include <fbl/intrusive_container_utils.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/intrusive_wavl_tree.h>
#include <fbl/macros.h>
#include <fbl/mutex.h>
#include <fbl/ref_ptr.h>
#include <region-alloc/region-alloc.h>

#include "src/devices/bus/drivers/pci/allocation.h"
#include "src/devices/bus/drivers/pci/bar_info.h"
#include "src/devices/bus/drivers/pci/bus_device_interface.h"
#include "src/devices/bus/drivers/pci/capabilities.h"
#include "src/devices/bus/drivers/pci/capabilities/msi.h"
#include "src/devices/bus/drivers/pci/capabilities/msix.h"
#include "src/devices/bus/drivers/pci/capabilities/pci_express.h"
#include "src/devices/bus/drivers/pci/capabilities/power_management.h"
#include "src/devices/bus/drivers/pci/config.h"
#include "src/devices/bus/drivers/pci/proxy_rpc.h"
#include "src/devices/bus/drivers/pci/ref_counted.h"

namespace pci {

// UpstreamNode includes device.h, so only forward declare it here.
class UpstreamNode;
class BusDeviceInterface;

struct DownstreamListTag {};
struct SharedIrqListTag {};

// A pci::Device represents a given PCI(e) device on a bus. It can be used
// standalone for a regular PCI(e) device on the bus, or as the base class for a
// Bridge. Most work a pci::Device does is limited to its own registers in
// configuration space and are managed through their Config object handled to it
// during creation. One of the biggest responsibilities of the pci::Device class
// is fulfill the PCI protocol for the driver downstream operating the PCI
// device this corresponds to.
class Device;
class Device : public fbl::WAVLTreeContainable<fbl::RefPtr<pci::Device>>,
               public fbl::ContainableBaseClasses<
                   fbl::TaggedDoublyLinkedListable<Device*, DownstreamListTag>,
                   fbl::TaggedDoublyLinkedListable<Device*, SharedIrqListTag>> {
 public:
  // This structure contains all bookkeeping and state for a device's
  // configured IRQ mode. It is initialized to PCI_INTERRUPT_MODE_DISABLED.
  struct Irqs {
    pci_interrupt_mode_t mode;          // The mode currently configured.
    zx::interrupt legacy;               // Virtual interrupt for legacy signaling.
    uint32_t legacy_vector;             // Vector for the legacy interrupt, mirrors kInterruptLine
                                        // in Config.
    uint8_t legacy_pin;                 // Pin for the legacy interrupt, mirrors kInterruptPin.
    zx_time_t legacy_irq_period_start;  // Timestamp of the current second we're monitoring for
                                        // IRQ floods.
    bool legacy_disabled;               // Whether  the legacy vector has been disabled
    zx::msi msi_allocation;             // The MSI allocation object for MSI & MSI-X
    uint64_t
        irqs_in_period;  // Current count of interrupts per PCI_INTERRUPT_MODE_LEGACY_NOACK period.
  };

  struct Capabilities {
    CapabilityList list;
    ExtCapabilityList ext_list;
    PowerManagementCapability* power;
    MsiCapability* msi;
    MsixCapability* msix;
    PciExpressCapability* pcie;
  };

  // These traits are used for the WAVL tree implementation. They allow device objects
  // to be sorted and found in trees by composite bdf address in the Bus.
  struct KeyTraitsSortByBdf {
    static const pci_bdf_t& GetKey(pci::Device& dev) { return dev.cfg_->bdf(); }

    static bool LessThan(const pci_bdf_t& bdf1, const pci_bdf_t& bdf2) {
      return (bdf1.bus_id < bdf2.bus_id) ||
             ((bdf1.bus_id == bdf2.bus_id) && (bdf1.device_id < bdf2.device_id)) ||
             ((bdf1.bus_id == bdf2.bus_id) && (bdf1.device_id == bdf2.device_id) &&
              (bdf1.function_id < bdf2.function_id));
    }

    static bool EqualTo(const pci_bdf_t& bdf1, const pci_bdf_t& bdf2) {
      return (bdf1.bus_id == bdf2.bus_id) && (bdf1.device_id == bdf2.device_id) &&
             (bdf1.function_id == bdf2.function_id);
    }
  };

  struct Inspect {
    explicit Inspect(inspect::Node node) : device(std::move(node)) {}

    // All the strings used for inspect PropertyValue and Node names.
    static constexpr char kInspectHeaderBars[] = "BARs";
    static constexpr char kInspectHeaderBarsInitial[] = "0. Initial";
    static constexpr char kInspectHeaderBarsProbed[] = "1. Probed";
    static constexpr char kInspectHeaderBarsConfigured[] = "2. Configured";
    static constexpr char kInspectHeaderBarsFailed[] = "Failed Range";
    static constexpr char kInspectHeaderBarsReallocated[] = "Reallocated Range";
    static constexpr char kInspectHeaderInterrupts[] = "Interrupts";
    static constexpr char kInspectIrqMode[] = "Mode";
    static constexpr char kInspectLegacyInterrupt[] = "Legacy Interrupt";
    static constexpr char kInspectMsi[] = "Message Signaled Interrupts";
    static constexpr char kInspectLegacyDisabled[] = "Disabled";
    static const constexpr char* kInspectIrqModes[] = {"Disabled", "Legacy", "Legacy (No Ack)",
                                                       "MSI", "MSI-X"};
    static constexpr char kInspectLegacyInterruptLine[] = "InterruptLine";
    static constexpr char kInspectLegacyInterruptPin[] = "InterruptPin";
    static constexpr char kInspectLegacySignalCount[] = "Signal Count";
    static constexpr char kInspectLegacyAckCount[] = "Ack Count";
    static constexpr char kInspectMsiBaseVector[] = "Base Vector";
    static constexpr char kInspectMsiAllocated[] = "Allocated";
    static constexpr char kInspectMsiMapped[] = "Mapped";

    inspect::Node device;
    // These hang off of |device|
    inspect::Node interrupts;
    inspect::UintProperty legacy_signal_cnt;
    inspect::UintProperty legacy_ack_cnt;
    // Individual BARs
    inspect::Node bar;  // The top level 'BARs' header
    std::array<std::optional<inspect::Node>, fuchsia_hardware_pci::wire::kMaxBarCount> bars;
  };

  // Templated helpers to assist with differently sized protocol reads and writes.
  // TODO(91513): Move these back to a .cc after we no longer have both Banjo and FIDL callers for
  // them.
  template <typename V, typename R>
  zx::result<V> ReadConfig(uint16_t offset) {
    if (offset + sizeof(V) > PCI_EXT_CONFIG_SIZE) {
      return zx::error(ZX_ERR_OUT_OF_RANGE);
    }

    return zx::ok(cfg_->Read(R(offset)));
  }

  template <typename V, typename R>
  zx_status_t WriteConfig(uint16_t offset, V value) {
    // Don't permit writes inside the config header.
    if (offset < PCI_CONFIG_HDR_SIZE) {
      return ZX_ERR_ACCESS_DENIED;
    }

    if (offset + sizeof(V) > PCI_EXT_CONFIG_SIZE) {
      return ZX_ERR_OUT_OF_RANGE;
    }

    cfg_->Write(R(offset), value);
    return ZX_OK;
  }

  // Create, but do not initialize, a device.
  static zx_status_t Create(zx_device_t* parent, std::unique_ptr<Config>&& config,
                            UpstreamNode* upstream, BusDeviceInterface* bdi, inspect::Node node,
                            bool has_acpi);
  virtual ~Device();

  // Bridge or DeviceImpl will need to implement refcounting
  PCI_REQUIRE_REFCOUNTED;

  // Disallow copying, assigning and moving.
  DISALLOW_COPY_ASSIGN_AND_MOVE(Device);

  // Trigger a function level reset (if possible)
  // TODO(cja): port zx_status_t DoFunctionLevelReset() when we have a way to test it

  // The methods here are locking versions that are used primarily by the PCI
  // protocol implementation in device_protocol.cc.

  // Modify bits in the device's command register (in the device config space),
  // clearing the bits specified by clr_bits and setting the bits specified by set
  // bits.  Specifically, the operation will be applied as...
  //
  // WR(cmd, (RD(cmd) & ~clr) | set)
  //
  // @param clr_bits The mask of bits to be cleared.
  // @param clr_bits The mask of bits to be set.
  // @return A zx_status_t indicating success or failure of the operation.
  zx_status_t ModifyCmd(uint16_t clr_bits, uint16_t set_bits) __TA_EXCLUDES(dev_lock_);

  // Enable or disable bus mastering in a device's configuration.
  //
  // @param enable If true, allow the device to access main system memory as a bus
  // master.
  // @return A zx_status_t indicating success or failure of the operation.
  zx_status_t SetBusMastering(bool enabled) __TA_REQUIRES(dev_lock_);

  // Enable or disable PIO access in a device's configuration.
  //
  // @param enable If true, allow the device to access its PIO mapped registers.
  // @return A zx_status_t indicating success or failure of the operation.
  zx_status_t EnablePio(bool enabled) __TA_EXCLUDES(dev_lock_);

  // Enable or disable MMIO access in a device's configuration.
  //
  // @param enable If true, allow the device to access its MMIO mapped registers.
  // @return A zx_status_t indicating success or failure of the operation.
  zx_status_t EnableMmio(bool enabled) __TA_EXCLUDES(dev_lock_);

  // Requests a device unplug itself from its UpstreamNode and the Bus list.
  virtual void Unplug() __TA_EXCLUDES(dev_lock_);
  // TODO(cja): port void SetQuirksDone() __TA_REQUIRES(dev_lock_) { quirks_done_ = true; }
  const std::unique_ptr<Config>& config() const { return cfg_; }

  fbl::Mutex* dev_lock() __TA_RETURN_CAPABILITY(dev_lock_) { return &dev_lock_; }
  UpstreamNode* upstream() { return upstream_; }

  bool plugged_in() const __TA_REQUIRES(dev_lock_) { return plugged_in_; }
  bool disabled() const __TA_REQUIRES(dev_lock_) { return disabled_; }
  bool quirks_done() const __TA_REQUIRES(dev_lock_) { return quirks_done_; }
  bool has_acpi() const { return has_acpi_; }
  bool is_bridge() const { return is_bridge_; }
  bool is_pcie() const { return is_pcie_; }
  uint16_t vendor_id() const { return vendor_id_; }
  uint16_t device_id() const { return device_id_; }
  uint8_t class_id() const { return class_id_; }
  uint8_t subclass() const { return subclass_; }
  uint8_t prog_if() const { return prog_if_; }
  uint8_t rev_id() const { return rev_id_; }
  uint8_t bus_id() const { return cfg_->bdf().bus_id; }
  uint8_t dev_id() const { return cfg_->bdf().device_id; }
  uint8_t func_id() const { return cfg_->bdf().function_id; }
  uint32_t bar_count() const { return bar_count_; }
  const Capabilities& capabilities() const { return caps_; }

  uint32_t legacy_vector() const __TA_EXCLUDES(dev_lock_) {
    const fbl::AutoLock dev_lock(&dev_lock_);
    return irqs_.legacy_vector;
  }
  const zx::msi& msi_allocation() const __TA_REQUIRES(dev_lock_) { return irqs_.msi_allocation; }

  // A packed version of the BDF addr used for BTI identifiers by the IOMMU implementation.
  uint32_t packed_addr() const {
    auto bdf = cfg_->bdf();
    return static_cast<uint32_t>((bdf.bus_id << 8) | (bdf.device_id << 3) | bdf.function_id);
  }

  // These methods handle IRQ configuration and are generally called by the
  // PciProtocol methods, though they may be used to disable IRQs on
  // initialization as well.
  zx::result<uint32_t> QueryIrqMode(pci_interrupt_mode_t mode) __TA_EXCLUDES(dev_lock_);
  pci_interrupt_modes_t GetInterruptModes() __TA_EXCLUDES(dev_lock_);
  zx_status_t SetIrqMode(pci_interrupt_mode_t mode, uint32_t irq_cnt) __TA_EXCLUDES(dev_lock_);
  zx::result<zx::interrupt> MapInterrupt(uint32_t which_irq) __TA_EXCLUDES(dev_lock_);
  zx_status_t DisableInterrupts() __TA_REQUIRES(dev_lock_);
  zx_status_t EnableLegacy(bool needs_ack) __TA_REQUIRES(dev_lock_);
  zx_status_t EnableMsi(uint32_t irq_cnt) __TA_REQUIRES(dev_lock_);
  zx_status_t EnableMsix(uint32_t irq_cnt) __TA_REQUIRES(dev_lock_);
  zx_status_t DisableLegacy() __TA_REQUIRES(dev_lock_);
  void DisableMsiCommon() __TA_REQUIRES(dev_lock_);
  zx_status_t DisableMsi() __TA_REQUIRES(dev_lock_);
  zx_status_t DisableMsix() __TA_REQUIRES(dev_lock_);
  // Signals the device's zx::interrupt, effectively triggering an interrupt for the device
  // driver.
  zx_status_t SignalLegacyIrq(zx_time_t timestamp) __TA_REQUIRES(dev_lock_);
  zx_status_t AckLegacyIrq() __TA_REQUIRES(dev_lock_);
  void EnableLegacyIrq() __TA_REQUIRES(dev_lock_);
  void DisableLegacyIrq() __TA_REQUIRES(dev_lock_);

  zx::result<PowerManagementCapability::PowerState> GetPowerState() __TA_EXCLUDES(dev_lock_);

  // Provide Irq information to the Bus for handling situations with no ack.
  Irqs& irqs() __TA_REQUIRES(dev_lock_) { return irqs_; }
  // Info about the BARs computed and cached during the initial setup/probe,
  // indexed by starting BAR register index.
  std::array<std::optional<Bar>, PCI_MAX_BAR_REGS>& bars() __TA_REQUIRES(dev_lock_) {
    return bars_;
  }
  BusDeviceInterface* bdi() __TA_REQUIRES(dev_lock_) { return bdi_; }

  // Devices need to exist in both the top level bus driver class, as well
  // as in a list for roots/bridges to track their downstream children. These
  // traits facilitate that for us.
 protected:
  Device(zx_device_t* parent, std::unique_ptr<Config>&& config, UpstreamNode* upstream,
         BusDeviceInterface* bdi, inspect::Node node, bool is_bridge, bool has_acpi);

  zx_status_t Init() __TA_EXCLUDES(dev_lock_);
  zx_status_t InitLocked() __TA_REQUIRES(dev_lock_);
  zx_status_t InitInterrupts() __TA_REQUIRES(dev_lock_);

  // Read the value of the Command register, requires the dev_lock.
  uint16_t ReadCmdLocked() __TA_REQUIRES(dev_lock_) __TA_EXCLUDES(cmd_reg_lock_) {
    fbl::AutoLock cmd_lock(&cmd_reg_lock_);
    return cfg_->Read(Config::kCommand);
  }
  void ModifyCmdLocked(uint16_t clr_bits, uint16_t set_bits) __TA_REQUIRES(dev_lock_)
      __TA_EXCLUDES(cmd_reg_lock_);
  void AssignCmdLocked(uint16_t value) __TA_REQUIRES(dev_lock_) __TA_EXCLUDES(cmd_reg_lock_) {
    ModifyCmdLocked(UINT16_MAX, value);
  }

  bool IoEnabled() __TA_REQUIRES(dev_lock_) { return ReadCmdLocked() & PCI_CONFIG_COMMAND_IO_EN; }
  bool MmioEnabled() __TA_REQUIRES(dev_lock_) {
    return ReadCmdLocked() & PCI_CONFIG_COMMAND_MEM_EN;
  }

  zx_status_t ProbeCapabilities() __TA_REQUIRES(dev_lock_);
  zx_status_t ParseCapabilities() __TA_REQUIRES(dev_lock_);
  zx_status_t ParseExtendedCapabilities() __TA_REQUIRES(dev_lock_);

 private:
  // Allow UpstreamNode implementations to Probe/Allocate/Configure/Disable.
  friend class UpstreamNode;
  friend class Bridge;
  friend class Root;
  // Probes a BAR's configuration. If it is already allocated it will try to
  // reserve the existing address window for it so that devices configured by system
  // firmware can be maintained as much as possible.
  zx::result<> ProbeBar(uint8_t bar_id) __TA_REQUIRES(dev_lock_);
  void ProbeBars() __TA_REQUIRES(dev_lock_);
  // Allocates address space for a BAR out of any suitable allocators.
  zx::result<std::unique_ptr<PciAllocation>> AllocateFromUpstream(const Bar& bar,
                                                                  std::optional<zx_paddr_t> base)
      __TA_REQUIRES(dev_lock_);
  zx_status_t WriteBarInformation(const Bar& bar) __TA_REQUIRES(dev_lock_);
  // Allocates address space for a BAR if it does not already exist.
  zx::result<> AllocateBar(uint8_t bar_id) __TA_REQUIRES(dev_lock_);
  // Called a device to configure (probe/allocate) its BARs
  zx::result<> AllocateBarsLocked() __TA_REQUIRES(dev_lock_);
  // Called by an UpstreamNode to configure the BARs of a device downstream.
  // Bridge implements it so it can allocate its bridge windows and own BARs before
  // configuring downstream BARs..
  virtual zx::result<> AllocateBars() __TA_EXCLUDES(dev_lock_);
  zx::result<> ConfigureCapabilities() __TA_EXCLUDES(dev_lock_);
  zx::result<std::pair<zx::msi, zx_info_msi_t>> AllocateMsi(uint32_t irq_cnt)
      __TA_REQUIRES(dev_lock_);
  zx_status_t VerifyAllMsisFreed() __TA_REQUIRES(dev_lock_);

  // Disable a device, and anything downstream of it.  The device will
  // continue to enumerate, but users will only be able to access config (and
  // only in a read only fashion).  BAR windows, bus mastering, and interrupts
  // will all be disabled.
  virtual void Disable() __TA_EXCLUDES(dev_lock_);
  void DisableLocked() __TA_REQUIRES(dev_lock_);

  // Inspect methods
  void InspectUpdateInterrupts() __TA_REQUIRES(dev_lock_);
  void InspectIncrementLegacySignalCount();
  void InspectIncrementLegacyAckCount();
  inspect::Node& InspectGetOrCreateBarNode(uint8_t bar_id);
  void InspectRecordBarState(const char* name, uint8_t bar_id, uint64_t bar_val);
  void InspectRecordBarInitialState(uint8_t bar_id, uint64_t bar_val);
  void InspectRecordBarConfiguredState(uint8_t bar_id, uint64_t bar_val);
  void InspectRecordBarRange(const char* name, uint8_t bar_id, ralloc_region_t region);
  void InspectRecordBarFailure(uint8_t bar_id, ralloc_region_t region);
  void InspectRecordBarReallocation(uint8_t bar_id, ralloc_region_t region);
  void InspectRecordBarProbedState(uint8_t bar_id, const Bar& bar);

  mutable fbl::Mutex dev_lock_;
  mutable fbl::Mutex cmd_reg_lock_;    // Protection for access to the command register.
  const std::unique_ptr<Config> cfg_;  // Pointer to the device's config interface.
  UpstreamNode* upstream_;             // The upstream node in the device graph.
  BusDeviceInterface* bdi_ __TA_GUARDED(dev_lock_);
  std::array<std::optional<Bar>, PCI_MAX_BAR_REGS> bars_ __TA_GUARDED(dev_lock_) = {};
  const uint32_t bar_count_;

  const bool is_bridge_;  // True if this device is also a bridge
  const bool has_acpi_;   // True if this device has an acpi fragment for its composite.
  uint16_t vendor_id_;    // The device's vendor ID, as read from config
  uint16_t device_id_;    // The device's device ID, as read from config
  uint8_t class_id_;      // The device's class ID, as read from config.
  uint8_t subclass_;      // The device's subclass, as read from config.
  uint8_t prog_if_;       // The device's programming interface (from cfg)
  uint8_t rev_id_;        // The device's revision ID (from cfg)

  // State related to lifetime management.
  bool plugged_in_ __TA_GUARDED(dev_lock_) = false;
  bool disabled_ __TA_GUARDED(dev_lock_) = false;
  bool quirks_done_ __TA_GUARDED(dev_lock_) = false;
  bool is_pcie_ = false;

  Capabilities caps_ __TA_GUARDED(dev_lock_){};
  Irqs irqs_ __TA_GUARDED(dev_lock_){.mode = PCI_INTERRUPT_MODE_DISABLED};

  zx_device_t* parent_;
  Inspect inspect_;
};

class BanjoDevice;
using BanjoDeviceType = ddk::Device<pci::BanjoDevice, ddk::GetProtocolable, ddk::Unbindable>;
class BanjoDevice : public BanjoDeviceType, public ddk::PciProtocol<pci::BanjoDevice> {
 public:
  BanjoDevice(zx_device_t* parent, pci::Device* device)
      : BanjoDeviceType(parent), device_(device) {}

  // ddk::PciProtocol implementations.
  zx_status_t PciGetBar(uint32_t bar_id, pci_bar_t* out_bar);
  zx_status_t PciSetBusMastering(bool enable);
  zx_status_t PciResetDevice();
  zx_status_t PciAckInterrupt();
  zx_status_t PciMapInterrupt(uint32_t which_irq, zx::interrupt* out_handle);
  void PciGetInterruptModes(pci_interrupt_modes_t* out_modes);
  zx_status_t PciSetInterruptMode(pci_interrupt_mode_t mode, uint32_t requested_irq_count);
  zx_status_t PciGetDeviceInfo(pci_device_info_t* out_info);
  zx_status_t PciReadConfig8(uint16_t offset, uint8_t* out_value);
  zx_status_t PciReadConfig16(uint16_t offset, uint16_t* out_value);
  zx_status_t PciReadConfig32(uint16_t offset, uint32_t* out_value);
  zx_status_t PciWriteConfig8(uint16_t offset, uint8_t value);
  zx_status_t PciWriteConfig16(uint16_t offset, uint16_t value);
  zx_status_t PciWriteConfig32(uint16_t offset, uint32_t value);
  zx_status_t PciGetFirstCapability(uint8_t cap_id, uint8_t* out_offset);
  zx_status_t PciGetNextCapability(uint8_t cap_id, uint8_t offset, uint8_t* out_offset);
  zx_status_t PciGetFirstExtendedCapability(uint16_t cap_id, uint16_t* out_offset);
  zx_status_t PciGetNextExtendedCapability(uint16_t cap_id, uint16_t offset, uint16_t* out_offset);
  zx_status_t PciGetBti(uint32_t index, zx::bti* out_bti);

  // Does the work necessary to create a ddk Composite device representing the
  // pci::Device.
  static zx::result<> Create(zx_device_t* parent, pci::Device* device);

  // DDK mix-in impls
  zx_status_t DdkGetProtocol(uint32_t proto_id, void* out);
  void DdkRelease() { delete this; }
  void DdkUnbind(ddk::UnbindTxn txn) { txn.Reply(); }
  pci::Device* device() { return device_; }

 private:
  pci::Device* device_;
};

class FidlDevice;
using FidlDeviceType = ddk::Device<pci::FidlDevice, ddk::Unbindable>;
class FidlDevice : public FidlDeviceType, public fidl::WireServer<fuchsia_hardware_pci::Device> {
 public:
  void Bind(fidl::ServerEnd<fuchsia_hardware_pci::Device> request);
  static zx::result<> Create(zx_device_t* parent, pci::Device* device);

  // fidl::WireServer<fuchsia_hardware_pci::Pci> implementations.
  void GetDeviceInfo(GetDeviceInfoCompleter::Sync& completer) override;
  void GetBar(GetBarRequestView request, GetBarCompleter::Sync& completer) override;
  void SetBusMastering(SetBusMasteringRequestView request,
                       SetBusMasteringCompleter::Sync& completer) override;
  void ResetDevice(ResetDeviceCompleter::Sync& completer) override;
  void AckInterrupt(AckInterruptCompleter::Sync& completer) override;
  void MapInterrupt(MapInterruptRequestView request,
                    MapInterruptCompleter::Sync& completer) override;
  void SetInterruptMode(SetInterruptModeRequestView request,
                        SetInterruptModeCompleter::Sync& completer) override;
  void GetInterruptModes(GetInterruptModesCompleter::Sync& completer) override;
  void ReadConfig8(ReadConfig8RequestView request, ReadConfig8Completer::Sync& completer) override;
  void ReadConfig16(ReadConfig16RequestView request,
                    ReadConfig16Completer::Sync& completer) override;
  void ReadConfig32(ReadConfig32RequestView request,
                    ReadConfig32Completer::Sync& completer) override;
  void WriteConfig8(WriteConfig8RequestView request,
                    WriteConfig8Completer::Sync& completer) override;
  void WriteConfig16(WriteConfig16RequestView request,
                     WriteConfig16Completer::Sync& completer) override;
  void WriteConfig32(WriteConfig32RequestView request,
                     WriteConfig32Completer::Sync& completer) override;
  void GetCapabilities(GetCapabilitiesRequestView request,
                       GetCapabilitiesCompleter::Sync& completer) override;
  void GetExtendedCapabilities(GetExtendedCapabilitiesRequestView request,
                               GetExtendedCapabilitiesCompleter::Sync& completer) override;
  void GetBti(GetBtiRequestView request, GetBtiCompleter::Sync& completer) override;
  pci::Device* device() { return device_; }
  component::OutgoingDirectory& outgoing_dir() { return outgoing_dir_; }

  void DdkRelease() { delete this; }
  void DdkUnbind(ddk::UnbindTxn txn) { txn.Reply(); }

 private:
  FidlDevice(zx_device_t* parent, pci::Device* device)
      : FidlDeviceType(parent),
        device_(device),
        outgoing_dir_(fdf::Dispatcher::GetCurrent()->async_dispatcher()) {}
  pci::Device* device_;
  fidl::ServerBindingGroup<fuchsia_hardware_pci::Device> bindings_;
  component::OutgoingDirectory outgoing_dir_;
};

}  // namespace pci

#endif  // SRC_DEVICES_BUS_DRIVERS_PCI_DEVICE_H_
