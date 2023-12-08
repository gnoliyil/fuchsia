// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/display/drivers/virtio-guest/v2/gpu-device.h"

#include <lib/driver/logging/cpp/logger.h>
#include <lib/fit/defer.h>
#include <lib/virtio/backends/backend.h>
#include <lib/virtio/driver_utils.h>

#include <memory>
#include <utility>

#include <fbl/alloc_checker.h>

#include "src/graphics/display/drivers/virtio-guest/v2/virtio-abi.h"

namespace virtio_display {

GpuDevice::GpuDevice(zx::bti bti, std::unique_ptr<virtio::Backend> backend)
    : virtio::Device(std::move(bti), std::move(backend)) {
  sem_init(&request_sem_, 0, 1);
  sem_init(&response_sem_, 0, 0);
}

GpuDevice::~GpuDevice() {
  if (request_virt_addr_) {
    zx::vmar::root_self()->unmap(request_virt_addr_, GetRequestSize(request_vmo_));
  }

  sem_destroy(&request_sem_);
  sem_destroy(&response_sem_);
}

// static
zx::result<std::unique_ptr<GpuDevice>> GpuDevice::Create(
    fidl::ClientEnd<fuchsia_hardware_pci::Device> client_end) {
  zx::result<std::pair<zx::bti, std::unique_ptr<virtio::Backend>>> bti_and_backend_result =
      virtio::GetBtiAndBackend(ddk::Pci(std::move(client_end)));
  if (!bti_and_backend_result.is_ok()) {
    FDF_LOG(ERROR, "GetBtiAndBackend failed: %s", bti_and_backend_result.status_string());
    return bti_and_backend_result.take_error();
  }

  fbl::AllocChecker alloc_checker;
  auto& [bti, backend] = bti_and_backend_result.value();
  auto device =
      fbl::make_unique_checked<GpuDevice>(&alloc_checker, std::move(bti), std::move(backend));
  if (!alloc_checker.check()) {
    FDF_LOG(ERROR, "Failed to allocate memory for GpuDevice");
    return zx::error(ZX_ERR_NO_MEMORY);
  }

  zx_status_t status = device->Init();
  if (status != ZX_OK) {
    FDF_LOG(ERROR, "Failed to initialize device");
    return zx::error(status);
  }

  return zx::ok(std::move(device));
}

zx_status_t GpuDevice::Init() {
  DeviceReset();

  virtio_abi::GpuDeviceConfig config;
  CopyDeviceConfig(&config, sizeof(config));
  FDF_LOG(TRACE, "GpuDeviceConfig - pending events: 0x%08" PRIx32, config.pending_events);
  FDF_LOG(TRACE, "GpuDeviceConfig - scanout limit: %d", config.scanout_limit);
  FDF_LOG(TRACE, "GpuDeviceConfig - capability set limit: %d", config.capability_set_limit);

  // Ack and set the driver status bit
  DriverStatusAck();

  if (!(DeviceFeaturesSupported() & VIRTIO_F_VERSION_1)) {
    // Declaring non-support until there is a need in the future.
    FDF_LOG(ERROR, "Legacy virtio interface is not supported by this driver");
    return ZX_ERR_NOT_SUPPORTED;
  }

  DriverFeaturesAck(VIRTIO_F_VERSION_1);

  zx_status_t status = DeviceStatusFeaturesOk();
  if (status != ZX_OK) {
    FDF_LOG(ERROR, "Feature negotiation failed: %s", zx_status_get_string(status));
    return status;
  }

  // Allocate the main vring
  status = vring_.Init(0, 16);
  if (status != ZX_OK) {
    FDF_LOG(ERROR, "Failed to allocate vring: %s", zx_status_get_string(status));
    return status;
  }

  status = zx::vmo::create(zx_system_get_page_size(), /*options=*/0, &request_vmo_);
  if (status != ZX_OK) {
    FDF_LOG(ERROR, "Failed to allocate request vmo: %s", zx_status_get_string(status));
    return status;
  }

  size_t size = GetRequestSize(request_vmo_);

  // Commit backing store and get the physical address.
  status = bti().pin(ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE, request_vmo_, /*offset=*/0, size,
                     &request_phys_addr_, 1, &request_pmt_);
  if (status != ZX_OK) {
    FDF_LOG(ERROR, "Failed to pin request vmo: %s", zx_status_get_string(status));
    return status;
  }

  status = zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, /*vmar_offset=*/0,
                                      request_vmo_,
                                      /*vmo_offset=*/0, size, &request_virt_addr_);
  if (status != ZX_OK) {
    FDF_LOG(ERROR, "Failed to map request vmo: %s", zx_status_get_string(status));
    return status;
  }

  FDF_LOG(INFO,
          "Allocated command buffer at virtual address 0x%016" PRIx64
          ", physical address 0x%016" PRIx64,
          request_virt_addr_, request_phys_addr_);

  StartIrqThread();
  DriverStatusOk();

  return ZX_OK;
}

// static
uint64_t GpuDevice::GetRequestSize(zx::vmo& vmo) {
  uint64_t size = 0;
  ZX_ASSERT(ZX_OK == vmo.get_size(&size));
  return size;
}

void GpuDevice::IrqRingUpdate() {
  FDF_LOG(TRACE, "IrqRingUpdate()");

  // Parse our descriptor chain, add back to the free queue
  auto free_chain = [this](vring_used_elem* used_elem) {
    uint16_t i = static_cast<uint16_t>(used_elem->id);
    struct vring_desc* desc = vring_.DescFromIndex(i);
    for (;;) {
      int next;

      if (desc->flags & VRING_DESC_F_NEXT) {
        next = desc->next;
      } else {
        // End of chain
        next = -1;
      }

      vring_.FreeDesc(i);

      if (next < 0) {
        break;
      }
      i = static_cast<uint16_t>(next);
      desc = vring_.DescFromIndex(i);
    }
    // Notify the request thread
    sem_post(&response_sem_);
  };

  // Tell the ring to find free chains and hand it back to our lambda
  vring_.IrqRingUpdate(free_chain);
}

void GpuDevice::IrqConfigChange() { FDF_LOG(TRACE, "IrqConfigChange()"); }

}  // namespace virtio_display
