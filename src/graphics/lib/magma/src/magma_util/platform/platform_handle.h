// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_HANDLE_H
#define PLATFORM_HANDLE_H

#if defined(__Fuchsia__)
#include <lib/zx/handle.h>
#endif

#include <memory>
#include <string>

#include "platform_port.h"

namespace magma {

class PlatformHandle {
 public:
  PlatformHandle() = default;
  virtual ~PlatformHandle() = default;

  virtual bool GetCount(uint32_t* count_out) = 0;
  virtual uint32_t release() = 0;
#if defined(__Fuchsia__)
  virtual zx::handle release_handle() = 0;
#endif

  // Registers an async wait delivered on the given |port| when the given handle is readable,
  // or if the handle has a peer and the peer is closed.
  // On success returns true.
  virtual bool WaitAsync(PlatformPort* port, uint64_t key) = 0;

  virtual std::string GetName() = 0;

  // Returns a globally-unique ID for this handle.
  virtual uint64_t global_id() = 0;

  static bool duplicate_handle(uint32_t handle_in, uint32_t* handle_out);

  static std::unique_ptr<PlatformHandle> Create(uint32_t handle);

#if defined(__Fuchsia__)
  static std::unique_ptr<PlatformHandle> Create(zx::handle handle);
#endif

  static bool SupportsGetCount();

  static const uint32_t kInvalidHandle;

 private:
  PlatformHandle(const PlatformHandle&) = delete;
  PlatformHandle& operator=(const PlatformHandle&) = delete;
};

}  // namespace magma

#endif  // PLATFORM_HANDLE_H
