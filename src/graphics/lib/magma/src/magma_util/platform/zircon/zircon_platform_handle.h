// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_PLATFORM_HANDLE_H
#define ZIRCON_PLATFORM_HANDLE_H

#include <lib/zx/handle.h>

#include "magma_util/short_macros.h"
#include "platform_handle.h"

namespace magma {

class ZirconPlatformHandle : public PlatformHandle {
 public:
  ZirconPlatformHandle(zx::handle handle) : handle_(std::move(handle)) {
    DASSERT(handle_ != ZX_HANDLE_INVALID);
  }

  bool GetCount(uint32_t* count_out) override;

  uint64_t global_id() override;

  bool WaitAsync(PlatformPort* port, uint64_t key) override;
  std::string GetName() override;

  uint32_t release() override { return handle_.release(); }
  zx::handle release_handle() override { return std::move(handle_); }

  zx_handle_t get() const { return handle_.get(); }

 private:
  zx::handle handle_;
  static_assert(sizeof(handle_) == sizeof(uint32_t), "zx handle is not 32 bits");
};

}  // namespace magma

#endif  // ZIRCON_PLATFORM_HANDLE_H
