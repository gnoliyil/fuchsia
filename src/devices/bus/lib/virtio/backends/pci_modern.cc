// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <inttypes.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/hw/reg.h>
#include <lib/mmio/mmio-buffer.h>

#include <cstdint>

#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>

#include "../include/lib/virtio/backends/pci.h"

namespace {

// MMIO reads and writes are abstracted out into template methods that
// ensure fields are only accessed with the right size.
template <typename T>
void MmioWrite(volatile T* addr, T value) {
  T::bad_instantiation();
}

template <typename T>
void MmioRead(const volatile T* addr, T* value) {
  T::bad_instantiation();
}

template <>
void MmioWrite<uint32_t>(volatile uint32_t* addr, uint32_t value) {
  writel(value, addr);
}

template <>
void MmioRead<uint32_t>(const volatile uint32_t* addr, uint32_t* value) {
  *value = readl(addr);
}

template <>
void MmioWrite<uint16_t>(volatile uint16_t* addr, uint16_t value) {
  writew(value, addr);
}

template <>
void MmioRead<uint16_t>(const volatile uint16_t* addr, uint16_t* value) {
  *value = readw(addr);
}

template <>
void MmioWrite<uint8_t>(volatile uint8_t* addr, uint8_t value) {
  writeb(value, addr);
}

template <>
void MmioRead<uint8_t>(const volatile uint8_t* addr, uint8_t* value) {
  *value = readb(addr);
}

// Virtio 1.0 Section 4.1.3:
// 64-bit fields are to be treated as two 32-bit fields, with low 32 bit
// part followed by the high 32 bit part.
template <>
void MmioWrite<uint64_t>(volatile uint64_t* addr, uint64_t value) {
  auto words = reinterpret_cast<volatile uint32_t*>(addr);
  MmioWrite(&words[0], static_cast<uint32_t>(value));
  MmioWrite(&words[1], static_cast<uint32_t>(value >> 32));
}

template <>
void MmioRead<uint64_t>(const volatile uint64_t* addr, uint64_t* value) {
  auto words = reinterpret_cast<const volatile uint32_t*>(addr);
  uint32_t lo, hi;
  MmioRead(&words[0], &lo);
  MmioRead(&words[1], &hi);
  *value = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
}

}  // anonymous namespace

namespace virtio {

// For reading the virtio specific vendor capabilities that can be PIO or MMIO space
#define cap_field(offset, field) static_cast<uint8_t>((offset) + offsetof(virtio_pci_cap_t, field))
zx_status_t PciModernBackend::ReadVirtioCap(uint8_t offset, virtio_pci_cap* cap) {
  zx_status_t status;
  uint8_t value8;
  status = pci().ReadConfig8(cap_field(offset, cap_vndr), &value8);
  if (status != ZX_OK) {
    return status;
  }
  cap->cap_vndr = value8;
  status = pci().ReadConfig8(cap_field(offset, cap_next), &value8);
  if (status != ZX_OK) {
    return status;
  }
  cap->cap_next = value8;
  status = pci().ReadConfig8(cap_field(offset, cap_len), &value8);
  if (status != ZX_OK) {
    return status;
  }
  cap->cap_len = value8;
  status = pci().ReadConfig8(cap_field(offset, cfg_type), &value8);
  if (status != ZX_OK) {
    return status;
  }
  cap->cfg_type = value8;
  status = pci().ReadConfig8(cap_field(offset, bar), &value8);
  if (status != ZX_OK) {
    return status;
  }
  cap->bar = value8;

  uint32_t value32;
  status = pci().ReadConfig32(cap_field(offset, offset), &value32);
  if (status != ZX_OK) {
    return status;
  }
  cap->offset = value32;
  status = pci().ReadConfig32(cap_field(offset, length), &value32);
  if (status != ZX_OK) {
    return status;
  }
  cap->length = value32;
  return ZX_OK;
}
#undef cap_field

zx_status_t PciModernBackend::Init() {
  fbl::AutoLock guard(&lock());

  // try to parse capabilities
  uint8_t off = 0;
  zx_status_t st;
  for (st = pci().GetFirstCapability(fuchsia_hardware_pci::CapabilityId::kVendor, &off);
       st == ZX_OK;
       st = pci().GetNextCapability(fuchsia_hardware_pci::CapabilityId::kVendor, off, &off)) {
    virtio_pci_cap_t cap;

    st = ReadVirtioCap(off, &cap);
    if (st != ZX_OK) {
      zxlogf(ERROR, "Failed to read PCI capabilities");
      return st;
    }
    switch (cap.cfg_type) {
      case VIRTIO_PCI_CAP_COMMON_CFG:
        CommonCfgCallbackLocked(cap);
        break;
      case VIRTIO_PCI_CAP_NOTIFY_CFG:
        // Virtio 1.0 section 4.1.4.4
        // notify_off_multiplier is a 32bit field following this capability
        pci().ReadConfig32(static_cast<uint8_t>(off + sizeof(virtio_pci_cap_t)), &notify_off_mul_);
        NotifyCfgCallbackLocked(cap);
        break;
      case VIRTIO_PCI_CAP_ISR_CFG:
        IsrCfgCallbackLocked(cap);
        break;
      case VIRTIO_PCI_CAP_DEVICE_CFG:
        DeviceCfgCallbackLocked(cap);
        break;
      case VIRTIO_PCI_CAP_PCI_CFG:
        PciCfgCallbackLocked(cap);
        break;
    }
  }

  // Ensure we found needed capabilities during parsing
  if (common_cfg_ == nullptr || isr_status_ == nullptr || device_cfg_ == 0 || notify_base_ == 0) {
    zxlogf(ERROR, "%s: failed to bind, missing capabilities", tag());
    return ZX_ERR_BAD_STATE;
  }

  zxlogf(TRACE, "virtio: modern pci backend successfully initialized");
  return ZX_OK;
}

// value pointers are used to maintain type safety with field width
void PciModernBackend::ReadDeviceConfig(uint16_t offset, uint8_t* value) {
  fbl::AutoLock guard(&lock());
  MmioRead(reinterpret_cast<volatile uint8_t*>(device_cfg_ + offset), value);
}

void PciModernBackend::ReadDeviceConfig(uint16_t offset, uint16_t* value) {
  fbl::AutoLock guard(&lock());
  MmioRead(reinterpret_cast<volatile uint16_t*>(device_cfg_ + offset), value);
}

void PciModernBackend::ReadDeviceConfig(uint16_t offset, uint32_t* value) {
  fbl::AutoLock guard(&lock());
  MmioRead(reinterpret_cast<volatile uint32_t*>(device_cfg_ + offset), value);
}

void PciModernBackend::ReadDeviceConfig(uint16_t offset, uint64_t* value) {
  fbl::AutoLock guard(&lock());
  MmioRead(reinterpret_cast<volatile uint64_t*>(device_cfg_ + offset), value);
}

void PciModernBackend::WriteDeviceConfig(uint16_t offset, uint8_t value) {
  fbl::AutoLock guard(&lock());
  MmioWrite(reinterpret_cast<volatile uint8_t*>(device_cfg_ + offset), value);
}

void PciModernBackend::WriteDeviceConfig(uint16_t offset, uint16_t value) {
  fbl::AutoLock guard(&lock());
  MmioWrite(reinterpret_cast<volatile uint16_t*>(device_cfg_ + offset), value);
}

void PciModernBackend::WriteDeviceConfig(uint16_t offset, uint32_t value) {
  fbl::AutoLock guard(&lock());
  MmioWrite(reinterpret_cast<volatile uint32_t*>(device_cfg_ + offset), value);
}

void PciModernBackend::WriteDeviceConfig(uint16_t offset, uint64_t value) {
  fbl::AutoLock guard(&lock());
  MmioWrite(reinterpret_cast<volatile uint64_t*>(device_cfg_ + offset), value);
}

// Attempt to map a bar found in a capability structure. If it has already been
// mapped and we have stored a valid handle in the structure then just return
// ZX_OK.
zx_status_t PciModernBackend::MapBar(uint8_t bar) {
  if (bar >= std::size(bar_)) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (bar_[bar]) {
    return ZX_OK;
  }

  std::optional<fdf::MmioBuffer> mmio;
  zx_status_t s = pci().MapMmio(bar, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
  if (s != ZX_OK) {
    zxlogf(ERROR, "%s: Failed to map bar %u: %d", tag(), bar, s);
    return s;
  }

  bar_[bar] = std::move(mmio);
  zxlogf(DEBUG, "%s: bar %u mapped to %p", tag(), bar, bar_[bar]->get());
  return ZX_OK;
}

void PciModernBackend::CommonCfgCallbackLocked(const virtio_pci_cap_t& cap) {
  zxlogf(DEBUG, "%s: common cfg found in bar %u offset %#x", tag(), cap.bar, cap.offset);
  if (MapBar(cap.bar) != ZX_OK) {
    return;
  }

  // Common config is a structure of type virtio_pci()common_cfg_t located at an
  // the bar and offset specified by the capability.
  auto addr = reinterpret_cast<uintptr_t>(bar_[cap.bar]->get()) + cap.offset;
  common_cfg_ = reinterpret_cast<volatile virtio_pci_common_cfg_t*>(addr);

  // Cache this when we find the config for kicking the queues later
}

void PciModernBackend::NotifyCfgCallbackLocked(const virtio_pci_cap_t& cap) {
  zxlogf(DEBUG, "%s: notify cfg found in bar %u offset %#x", tag(), cap.bar, cap.offset);
  if (MapBar(cap.bar) != ZX_OK) {
    return;
  }

  notify_base_ = reinterpret_cast<uintptr_t>(bar_[cap.bar]->get()) + cap.offset;
}

void PciModernBackend::IsrCfgCallbackLocked(const virtio_pci_cap_t& cap) {
  zxlogf(DEBUG, "%s: isr cfg found in bar %u offset %#x", tag(), cap.bar, cap.offset);
  if (MapBar(cap.bar) != ZX_OK) {
    return;
  }

  // interrupt status is directly read from the register at this address
  isr_status_ = reinterpret_cast<volatile uint32_t*>(
      reinterpret_cast<uintptr_t>(bar_[cap.bar]->get()) + cap.offset);
}

void PciModernBackend::DeviceCfgCallbackLocked(const virtio_pci_cap_t& cap) {
  zxlogf(DEBUG, "%s: device cfg found in bar %u offset %#x", tag(), cap.bar, cap.offset);
  if (MapBar(cap.bar) != ZX_OK) {
    return;
  }

  device_cfg_ = reinterpret_cast<uintptr_t>(bar_[cap.bar]->get()) + cap.offset;
}

void PciModernBackend::PciCfgCallbackLocked(const virtio_pci_cap_t& cap) {
  // We are not using this capability presently since we can map the
  // bars for direct memory access.
}

// Get the ring size of a specific index
uint16_t PciModernBackend::GetRingSize(uint16_t index) {
  fbl::AutoLock guard(&lock());

  uint16_t queue_size = 0;
  MmioWrite(&common_cfg_->queue_select, index);
  MmioRead(&common_cfg_->queue_size, &queue_size);
  zxlogf(TRACE, "QueueSize: %#x", queue_size);
  return queue_size;
}

// Set up ring descriptors with the backend.
zx_status_t PciModernBackend::SetRing(uint16_t index, uint16_t count, zx_paddr_t pa_desc,
                                      zx_paddr_t pa_avail, zx_paddr_t pa_used) {
  fbl::AutoLock guard(&lock());

  // These offsets are wrong and this should be changed
  MmioWrite(&common_cfg_->queue_select, index);
  MmioWrite(&common_cfg_->queue_size, count);
  MmioWrite(&common_cfg_->queue_desc, pa_desc);
  MmioWrite(&common_cfg_->queue_avail, pa_avail);
  MmioWrite(&common_cfg_->queue_used, pa_used);

  if (irq_mode() == fuchsia_hardware_pci::InterruptMode::kMsiX) {
    uint16_t vector = 0;
    MmioWrite(&common_cfg_->config_msix_vector, PciBackend::kMsiConfigVector);
    MmioRead(&common_cfg_->config_msix_vector, &vector);
    if (vector != PciBackend::kMsiConfigVector) {
      zxlogf(ERROR, "MSI-X config vector in invalid state after write: %#x", vector);
      return ZX_ERR_BAD_STATE;
    }

    MmioWrite(&common_cfg_->queue_msix_vector, PciBackend::kMsiQueueVector);
    MmioRead(&common_cfg_->queue_msix_vector, &vector);
    if (vector != PciBackend::kMsiQueueVector) {
      zxlogf(ERROR, "MSI-X queue vector in invalid state after write: %#x", vector);
      return ZX_ERR_BAD_STATE;
    }
  }

  MmioWrite<uint16_t>(&common_cfg_->queue_enable, 1);
  // Assert that queue_notify_off is equal to the ring index.
  uint16_t queue_notify_off;
  MmioRead(&common_cfg_->queue_notify_off, &queue_notify_off);
  if (queue_notify_off != index) {
    zxlogf(ERROR, "Virtio queue notify setup failed");
    return ZX_ERR_BAD_STATE;
  }

  return ZX_OK;
}

void PciModernBackend::RingKick(uint16_t ring_index) {
  fbl::AutoLock guard(&lock());

  // Virtio 1.0 Section 4.1.4.4
  // The address to notify for a queue is calculated using information from
  // the notify_off_multiplier, the capability's base + offset, and the
  // selected queue's offset.
  //
  // For performance reasons, we assume that the selected queue's offset is
  // equal to the ring index.
  auto addr = notify_base_ + ring_index * notify_off_mul_;
  auto ptr = reinterpret_cast<volatile uint16_t*>(addr);
  zxlogf(TRACE, "%s: kick %u addr %p", tag(), ring_index, ptr);
  *ptr = ring_index;
}

uint64_t PciModernBackend::ReadFeatures() {
  auto read_subset_features = [this](uint32_t select) {
    uint32_t val;
    {
      fbl::AutoLock guard(&lock());
      MmioWrite(&common_cfg_->device_feature_select, select);
      MmioRead(&common_cfg_->device_feature, &val);
    }
    return val;
  };

  uint64_t bitmap = read_subset_features(1);
  bitmap = bitmap << 32 | read_subset_features(0);
  return bitmap;
}

void PciModernBackend::SetFeatures(uint64_t bitmap) {
  auto write_subset_features = [this](uint32_t select, uint32_t sub_bitmap) {
    fbl::AutoLock guard(&lock());
    MmioWrite(&common_cfg_->driver_feature_select, select);
    uint32_t val;
    MmioRead(&common_cfg_->driver_feature, &val);
    MmioWrite(&common_cfg_->driver_feature, val | sub_bitmap);
    zxlogf(DEBUG, "%s: feature bits %08uh now set at offset %u", tag(), sub_bitmap, 32 * select);
  };

  uint32_t sub_bitmap = bitmap & UINT32_MAX;
  if (sub_bitmap) {
    write_subset_features(0, sub_bitmap);
  }
  sub_bitmap = bitmap >> 32;
  if (sub_bitmap) {
    write_subset_features(1, sub_bitmap);
  }
}

zx_status_t PciModernBackend::ConfirmFeatures() {
  fbl::AutoLock guard(&lock());
  uint8_t val;

  MmioRead(&common_cfg_->device_status, &val);
  val |= VIRTIO_STATUS_FEATURES_OK;
  MmioWrite(&common_cfg_->device_status, val);

  // Check that the device confirmed our feature choices were valid
  MmioRead(&common_cfg_->device_status, &val);
  if ((val & VIRTIO_STATUS_FEATURES_OK) == 0) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  return ZX_OK;
}

void PciModernBackend::DeviceReset() {
  fbl::AutoLock guard(&lock());

  MmioWrite<uint8_t>(&common_cfg_->device_status, 0u);
}

void PciModernBackend::WaitForDeviceReset() {
  fbl::AutoLock guard(&lock());

  uint8_t device_status = 0xFF;
  while (device_status != 0) {
    MmioRead(&common_cfg_->device_status, &device_status);
  }
}

void PciModernBackend::DriverStatusOk() {
  fbl::AutoLock guard(&lock());

  uint8_t device_status;
  MmioRead(&common_cfg_->device_status, &device_status);
  device_status |= VIRTIO_STATUS_DRIVER_OK;
  MmioWrite(&common_cfg_->device_status, device_status);
}

void PciModernBackend::DriverStatusAck() {
  fbl::AutoLock guard(&lock());

  uint8_t device_status;
  MmioRead(&common_cfg_->device_status, &device_status);
  device_status |= VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
  MmioWrite(&common_cfg_->device_status, device_status);
}

uint32_t PciModernBackend::IsrStatus() {
  return (*isr_status_ & (VIRTIO_ISR_QUEUE_INT | VIRTIO_ISR_DEV_CFG_INT));
}

}  // namespace virtio
