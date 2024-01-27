// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_OBJECT_H
#define PLATFORM_OBJECT_H

#if defined(__Fuchsia__)
#include <lib/zx/handle.h>
#endif
#include <stdint.h>

namespace magma {

class PlatformObject {
 public:
  enum Type { BUFFER = 11, SEMAPHORE = 12 };

  // Sets an ID that is attached only to this PlatformObject instance, not the underlying
  // object. Can only be set once and must be non-zero.
  virtual void set_local_id(uint64_t id) = 0;

  // Returns the local ID if set; otherwise returns an ID that uniquely identifies the underlying
  // memory object.
  virtual uint64_t id() const = 0;

  // Returns an ID that uniquely identifies the underlying object.
  virtual uint64_t global_id() const = 0;

  // on success, duplicate of the underlying handle which is owned by the caller
  virtual bool duplicate_handle(uint32_t* handle_out) const = 0;

#if defined(__Fuchsia__)
  virtual bool duplicate_handle(zx::handle* handle_out) const = 0;
#endif

  // Returns the id for the given handle
  static bool IdFromHandle(uint32_t handle, uint64_t* id_out);
};

}  // namespace magma

#endif  // PLATFORM_OBJECT_H
