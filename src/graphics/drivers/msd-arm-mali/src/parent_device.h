// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_SRC_PARENT_DEVICE_H_
#define SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_SRC_PARENT_DEVICE_H_

#include <fuchsia/hardware/platform/device/c/banjo.h>
#include <lib/ddk/device.h>
#include <lib/device-protocol/pdev-fidl.h>
#include <lib/fdf/cpp/channel.h>

#include <chrono>
#include <memory>

#include "magma_util/dlog.h"
#include "magma_util/short_macros.h"
#include "magma_util/status.h"
#include "msd_cc.h"
#include "platform_buffer.h"
#include "platform_handle.h"
#include "platform_interrupt.h"
#include "platform_mmio.h"

// zx_device_t* == msd::DeviceHandle*.
msd::DeviceHandle* ZxDeviceToDeviceHandle(zx_device_t* device);

class ParentDevice {
 public:
  explicit ParentDevice(zx_device_t* parent, pdev_protocol_t pdev)
      : parent_(parent), pdev_(&pdev) {}

  virtual ~ParentDevice() { DLOG("ParentDevice dtor"); }

  virtual zx_device_t* GetDeviceHandle();

  zx::bti GetBusTransactionInitiator() const;

  // Get a driver-specific protocol implementation. |proto_id| identifies which
  // protocol to retrieve.
  virtual bool GetProtocol(uint32_t proto_id, void* proto_out);

  // Map an MMIO listed at |index| in the platform device
  virtual std::unique_ptr<magma::PlatformMmio> CpuMapMmio(
      unsigned int index, magma::PlatformMmio::CachePolicy cache_policy);

  // Register an interrupt listed at |index| in the platform device.
  virtual std::unique_ptr<magma::PlatformInterrupt> RegisterInterrupt(unsigned int index);

  virtual zx_status_t ConnectRuntimeProtocol(const char* service_name, const char* name,
                                             fdf::Channel server_end);

  // Ownership of |device_handle| is *not* transferred to the ParentDevice.
  static std::unique_ptr<ParentDevice> Create(msd::DeviceHandle* device_handle);

 private:
  zx_device_t* parent_;
  ddk::PDev pdev_;
};

#endif  // SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_SRC_PARENT_DEVICE_H_
