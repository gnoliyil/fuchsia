// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zircon_platform_semaphore.h"

#include <lib/zx/time.h>

#include "magma_util/short_macros.h"
#include "platform_object.h"
#include "zircon_platform_port.h"

namespace magma {

bool ZirconPlatformSemaphore::duplicate_handle(uint32_t* handle_out) const {
  zx::handle new_handle;
  if (!duplicate_handle(&new_handle))
    return false;
  *handle_out = new_handle.release();
  return true;
}

bool ZirconPlatformSemaphore::duplicate_handle(zx::handle* handle_out) const {
  zx::event duplicate;
  zx_status_t status = event_.duplicate(ZX_RIGHT_SAME_RIGHTS, &duplicate);
  if (status < 0)
    return DRETF(false, "zx_handle_duplicate failed: %d", status);
  *handle_out = std::move(duplicate);
  return true;
}

magma::Status ZirconPlatformSemaphore::WaitNoReset(uint64_t timeout_ms) {
  TRACE_DURATION("magma:sync", "semaphore wait", "id", koid_);
  zx_status_t status = event_.wait_one(
      zx_signal(), zx::deadline_after(zx::duration(magma::ms_to_signed_ns(timeout_ms))), nullptr);
  switch (status) {
    case ZX_OK:
      return MAGMA_STATUS_OK;
    case ZX_ERR_TIMED_OUT:
      return MAGMA_STATUS_TIMED_OUT;
    case ZX_ERR_CANCELED:
      return MAGMA_STATUS_CONNECTION_LOST;
    default:
      return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "Unexpected wait() status: %d", status);
  }
}

magma::Status ZirconPlatformSemaphore::Wait(uint64_t timeout_ms) {
  magma::Status status = WaitNoReset(timeout_ms);
  if (status.ok()) {
    Reset();
  }
  return status;
}

bool ZirconPlatformSemaphore::WaitAsync(PlatformPort* port, uint64_t key) {
  TRACE_DURATION("magma:sync", "semaphore wait async", "id", koid_);
  TRACE_FLOW_BEGIN("magma:sync", "semaphore wait async", koid_);

  auto zircon_port = static_cast<ZirconPlatformPort*>(port);

  zx_status_t status = event_.wait_async(zircon_port->zx_port(), key, zx_signal(), 0);
  if (status != ZX_OK)
    return DRETF(false, "wait_async failed: %d", status);

  return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<PlatformSemaphore> PlatformSemaphore::Create() {
  zx::event event;
  zx_status_t status = zx::event::create(0, &event);
  if (status != ZX_OK)
    return DRETP(nullptr, "event::create failed: %d", status);

  uint64_t koid;
  if (!PlatformObject::IdFromHandle(event.get(), &koid))
    return DRETP(nullptr, "couldn't get koid from handle");

  return std::make_unique<ZirconPlatformSemaphore>(std::move(event), koid);
}

std::unique_ptr<PlatformSemaphore> PlatformSemaphore::Import(uint32_t handle) {
  return Import(zx::event(handle));
}

std::unique_ptr<PlatformSemaphore> PlatformSemaphore::Import(zx::event event) {
  zx_info_handle_basic_t info;
  zx_status_t status = event.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  if (status != ZX_OK)
    return DRETP(nullptr, "couldn't get handle info %d", status);

  if (info.type != ZX_OBJ_TYPE_EVENT)
    return DRETP(nullptr, "invalid object type: %d", info.type);

  return std::make_unique<ZirconPlatformSemaphore>(std::move(event), info.koid);
}

}  // namespace magma
