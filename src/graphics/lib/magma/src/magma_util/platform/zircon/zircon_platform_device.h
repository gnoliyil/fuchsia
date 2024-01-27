// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_PLATFORM_DEVICE_H
#define ZIRCON_PLATFORM_DEVICE_H

#include <fuchsia/hardware/platform/device/c/banjo.h>
#include <lib/ddk/device.h>
#include <lib/device-protocol/pdev-fidl.h>

#include "magma_util/short_macros.h"
#include "platform_device.h"

namespace magma {

class ZirconPlatformDeviceWithoutProtocol : public PlatformDevice {
 public:
  ZirconPlatformDeviceWithoutProtocol(zx_device_t* zx_device) : zx_device_(zx_device) {}

  void* GetDeviceHandle() override { return zx_device(); }

  bool GetProtocol(uint32_t proto_id, void* proto_out) override;

  Status LoadFirmware(const char* filename, std::unique_ptr<PlatformBuffer>* firmware_out,
                      uint64_t* size_out) const override;

  std::unique_ptr<PlatformHandle> GetBusTransactionInitiator() const override {
    return DRETP(nullptr, "No protocol");
  }

  std::unique_ptr<PlatformMmio> CpuMapMmio(unsigned int index,
                                           PlatformMmio::CachePolicy cache_policy) override {
    return DRETP(nullptr, "No protocol");
  }

  uint32_t GetMmioCount() const override { return 0; }

  std::unique_ptr<PlatformBuffer> GetMmioBuffer(unsigned int index) override {
    return DRETP(nullptr, "No protocol");
  }

  std::unique_ptr<PlatformInterrupt> RegisterInterrupt(unsigned int index) override {
    return DRETP(nullptr, "No protocol");
  }

  zx_device_t* zx_device() const { return zx_device_; }

 private:
  zx_device_t* zx_device_;
};

class ZirconPlatformDevice : public ZirconPlatformDeviceWithoutProtocol {
 public:
  ZirconPlatformDevice(zx_device_t* zx_device, pdev_protocol_t pdev, uint32_t mmio_count)
      : ZirconPlatformDeviceWithoutProtocol(zx_device), pdev_(&pdev), mmio_count_(mmio_count) {}

  std::unique_ptr<PlatformHandle> GetBusTransactionInitiator() const override;

  uint32_t GetMmioCount() const override { return mmio_count_; }

  std::unique_ptr<PlatformMmio> CpuMapMmio(unsigned int index,
                                           PlatformMmio::CachePolicy cache_policy) override;

  std::unique_ptr<PlatformBuffer> GetMmioBuffer(unsigned int index) override;

  std::unique_ptr<PlatformInterrupt> RegisterInterrupt(unsigned int index) override;

 private:
  ddk::PDev pdev_;
  uint32_t mmio_count_;
};

}  // namespace magma

#endif  // ZIRCON_PLATFORM_DEVICE_H
