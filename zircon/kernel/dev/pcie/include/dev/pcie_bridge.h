// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2016, Google, Inc. All rights reserved
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_DEV_PCIE_INCLUDE_DEV_PCIE_BRIDGE_H_
#define ZIRCON_KERNEL_DEV_PCIE_INCLUDE_DEV_PCIE_BRIDGE_H_

#include <sys/types.h>
#include <zircon/compiler.h>
#include <zircon/errors.h>

#include <dev/pcie_bus_driver.h>
#include <dev/pcie_device.h>
#include <dev/pcie_ref_counted.h>
#include <dev/pcie_upstream_node.h>
#include <fbl/macros.h>
#include <fbl/ref_ptr.h>
#include <region-alloc/region-alloc.h>

class PciConfig;
class PcieBridge : public PcieDevice, public PcieUpstreamNode {
 public:
  static fbl::RefPtr<PcieDevice> Create(PcieUpstreamNode& upstream, uint dev_id, uint func_id,
                                        uint managed_bus_id);

  // Disallow copying, assigning and moving.
  DISALLOW_COPY_ASSIGN_AND_MOVE(PcieBridge);

  // Implement ref counting, do not let derived classes override.
  PCIE_IMPLEMENT_REFCOUNTED;

  // Device overrides
  void Unplug() __TA_EXCLUDES(bridge_lock_) final;

  zx_status_t EnableBusMasterUpstream(bool enabled) __TA_EXCLUDES(bridge_lock_) final;

  // UpstreamNode overrides
  RegionAllocator& pf_mmio_regions() final { return pf_mmio_regions_; }
  RegionAllocator& mmio_lo_regions() final { return mmio_lo_regions_; }
  RegionAllocator& mmio_hi_regions() final { return mmio_hi_regions_; }
  RegionAllocator& pio_regions() final { return pio_regions_; }

  // Properties
  PcieBusDriver& driver() { return PcieDevice::driver(); }

  uint64_t pf_mem_base() const { return pf_mem_base_; }
  uint64_t pf_mem_limit() const { return pf_mem_limit_; }
  uint32_t mem_base() const { return mem_base_; }
  uint32_t mem_limit() const { return mem_limit_; }
  uint32_t io_base() const { return io_base_; }
  uint32_t io_limit() const { return io_limit_; }
  bool supports_32bit_pio() const { return supports_32bit_pio_; }

  // print some info about the bridge
  void Dump() const override;

 protected:
  zx_status_t AllocateBars() __TA_EXCLUDES(bridge_lock_) final;
  zx_status_t AllocateBridgeWindowsLocked() __TA_REQUIRES(bridge_lock_);
  void Disable() __TA_EXCLUDES(bridge_lock_) __TA_EXCLUDES(dev_lock_) override;

 private:
  friend class PcieBusDriver;

  PcieBridge(PcieBusDriver& bus_drv, uint bus_id, uint dev_id, uint func_id, uint mbus_id);

  zx_status_t ParseBusWindowsLocked() __TA_REQUIRES(bridge_lock_);
  zx_status_t Init(PcieUpstreamNode& upstream) __TA_EXCLUDES(bridge_lock_);

  mutable DECLARE_MUTEX(PcieBridge) bridge_lock_;
  RegionAllocator pf_mmio_regions_ __TA_GUARDED(bridge_lock_);
  RegionAllocator mmio_lo_regions_ __TA_GUARDED(bridge_lock_);
  RegionAllocator mmio_hi_regions_ __TA_GUARDED(bridge_lock_);
  RegionAllocator pio_regions_ __TA_GUARDED(bridge_lock_);

  RegionAllocator::Region::UPtr pf_mmio_window_ __TA_GUARDED(bridge_lock_);
  RegionAllocator::Region::UPtr mmio_window_ __TA_GUARDED(bridge_lock_);
  RegionAllocator::Region::UPtr pio_window_ __TA_GUARDED(bridge_lock_);

  size_t downstream_bus_mastering_cnt_ = 0;
  uint64_t pf_mem_base_ = 0;
  uint64_t pf_mem_limit_ = 0;
  uint32_t mem_base_ = 0;
  uint32_t mem_limit_ = 0;
  uint32_t io_base_ = 0;
  uint32_t io_limit_ = 0;
  bool supports_32bit_pio_ = false;
  fbl::RefPtr<PcieDevice> ScanDevice(const PciConfig* cfg, uint dev_id, uint func_id);
};

#endif  // ZIRCON_KERNEL_DEV_PCIE_INCLUDE_DEV_PCIE_BRIDGE_H_
