// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STORAGE_BUFFER_BLOCK_BUFFER_H_
#define STORAGE_BUFFER_BLOCK_BUFFER_H_

#include <fuchsia/hardware/block/driver/c/banjo.h>

#include <cstdint>

namespace storage {

// Interface for a block-aligned buffer.
//
// This class should be thread-compatible.
class BlockBuffer {
 public:
  virtual ~BlockBuffer() = default;

  // Returns the total amount of blocks which the buffer handles.
  virtual size_t capacity() const = 0;

  // Returns the size of each data block handled by this buffer.
  virtual uint32_t BlockSize() const = 0;

  // Returns the vmoid of the underlying BlockBuffer, if one exists.
  virtual vmoid_t vmoid() const = 0;

  // Returns a handle to the underlying VMO, if one exists. Ownership of the VMO
  // is not being transferred to the caller.
  virtual zx_handle_t Vmo() const = 0;

  // Returns data starting at block |index| in the buffer.
  virtual void* Data(size_t index) = 0;

  // Returns data starting at block |index| in the buffer.
  virtual const void* Data(size_t index) const = 0;

  // Zero |count| blocks from |index|. Subclasses might override to provide a more efficient
  // implementation.
  virtual zx_status_t Zero(size_t index, size_t count);
};

}  // namespace storage

#endif  // STORAGE_BUFFER_BLOCK_BUFFER_H_
