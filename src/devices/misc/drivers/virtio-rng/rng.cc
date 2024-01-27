// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "rng.h"

#include <inttypes.h>
#include <lib/ddk/debug.h>
#include <limits.h>
#include <zircon/status.h>

#include <memory>
#include <utility>

#include <fbl/auto_lock.h>

#define LOCAL_TRACE 0

namespace virtio {

RngDevice::RngDevice(zx_device_t* bus_device, zx::bti bti, std::unique_ptr<Backend> backend)
    : virtio::Device(bus_device, std::move(bti), std::move(backend)),
      ddk::Device<RngDevice>(bus_device) {}

RngDevice::~RngDevice() {
  // TODO: clean up allocated physical memory
}

zx_status_t RngDevice::Init() {
  // reset the device
  DeviceReset();

  // ack and set the driver status bit
  DriverStatusAck();

  if (!DeviceFeaturesSupported(VIRTIO_F_VERSION_1)) {
    // Declaring non-support until there is a need in the future.
    zxlogf(ERROR, "Legacy virtio interface is not supported by this driver");
    return ZX_ERR_NOT_SUPPORTED;
  }
  DriverFeaturesAck(VIRTIO_F_VERSION_1);
  if (zx_status_t status = DeviceStatusFeaturesOk(); status != ZX_OK) {
    zxlogf(ERROR, "Feature negotiation failed: %s", zx_status_get_string(status));
    return status;
  }

  // allocate the main vring
  zx_status_t status = vring_.Init(kRingIndex, kRingSize);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to allocate vring", tag());
    return status;
  }

  // allocate the entropy buffer
  assert(kBufferSize <= zx_system_get_page_size());
  status = io_buffer_init(&buf_, bti_.get(), kBufferSize, IO_BUFFER_RO | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: cannot allocate entropy buffer: %d", tag(), status);
    return status;
  }

  zxlogf(TRACE, "%s: allocated entropy buffer at %p, physical address %#" PRIxPTR "", tag(),
         io_buffer_virt(&buf_), io_buffer_phys(&buf_));

  // start the interrupt thread
  StartIrqThread();

  // set DRIVER_OK
  DriverStatusOk();

  status = DdkAdd("virtio-rng");
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to add device: %s", tag(), zx_status_get_string(status));
    return status;
  }
  device_ = zxdev();

  // TODO(fxbug.dev/24760): The kernel should trigger entropy requests, instead of relying on this
  // userspace thread to push entropy whenever it wants to. As a temporary hack, this thread
  // pushes entropy to the kernel every 300 seconds instead.
  thrd_create_with_name(&seed_thread_, RngDevice::SeedThreadEntry, this, "virtio-rng-seed-thread");
  thrd_detach(seed_thread_);

  zxlogf(INFO, "%s: initialization succeeded", tag());

  return ZX_OK;
}

void RngDevice::IrqRingUpdate() {
  zxlogf(DEBUG, "%s: Got irq ring update", tag());

  // parse our descriptor chain, add back to the free queue
  auto free_chain = [this](vring_used_elem* used_elem) {
    uint32_t i = static_cast<uint16_t>(used_elem->id);
    struct vring_desc* desc = vring_.DescFromIndex(static_cast<uint16_t>(i));

    if (desc->addr != io_buffer_phys(&buf_) || desc->len != kBufferSize) {
      zxlogf(ERROR, "%s: entropy response with unexpected buffer", tag());
    } else {
      zxlogf(TRACE, "%s: received entropy; adding to kernel pool", tag());
      zx_status_t rc = zx_cprng_add_entropy(io_buffer_virt(&buf_), kBufferSize);
      if (rc != ZX_OK) {
        zxlogf(ERROR, "%s: add_entropy failed (%d)", tag(), rc);
      }
    }

    vring_.FreeDesc(static_cast<uint16_t>(i));
  };

  // tell the ring to find free chains and hand it back to our lambda
  vring_.IrqRingUpdate(free_chain);
}

void RngDevice::IrqConfigChange() { zxlogf(DEBUG, "%s: Got irq config change (ignoring)", tag()); }

int RngDevice::SeedThreadEntry(void* arg) {
  RngDevice* d = static_cast<RngDevice*>(arg);
  for (;;) {
    zx_status_t rc = d->Request();
    zxlogf(TRACE, "virtio-rng-seed-thread: RngDevice::Request() returned %d", rc);
    zx_nanosleep(zx_deadline_after(ZX_SEC(300)));
  }
}

zx_status_t RngDevice::Request() {
  zxlogf(DEBUG, "%s: sending entropy request", tag());
  fbl::AutoLock lock(&lock_);
  uint16_t i;
  vring_desc* desc = vring_.AllocDescChain(1, &i);
  if (!desc) {
    zxlogf(ERROR, "%s: failed to allocate descriptor chain of length 1", tag());
    return ZX_ERR_NO_RESOURCES;
  }

  desc->addr = io_buffer_phys(&buf_);
  desc->len = kBufferSize;
  desc->flags = VRING_DESC_F_WRITE;
  zxlogf(TRACE, "%s: allocated descriptor chain desc %p, i %u", tag(), desc, i);
  if (zxlog_level_enabled(TRACE)) {
    virtio_dump_desc(desc);
  }

  vring_.SubmitChain(i);
  vring_.Kick();

  zxlogf(TRACE, "%s: kicked off entropy request", tag());

  return ZX_OK;
}

}  // namespace virtio
