// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef STORAGE_OPERATION_UNBUFFERED_OPERATION_H_
#define STORAGE_OPERATION_UNBUFFERED_OPERATION_H_

#include <fuchsia/hardware/block/driver/c/banjo.h>
#include <lib/stdcompat/span.h>
#include <lib/zx/vmo.h>

#include <storage/operation/operation.h>

namespace storage {

// An operation paired with a source vmo.
//
// Used to indicate a request to move in-memory data to an on-disk location,
// or vice versa. To be transmitted to storage, the |vmo| must be later
// converted to a "vmoid_t" object.
//
// Clearly, this is a Fuchsia-only structure.
struct UnbufferedOperation {
  // Either vmo or data must be provided.
  zx::unowned_vmo vmo;

  // This is const void* to avoid having to const cast for the common case where this is used for
  // writes.  At time of writing there is no requirement for reads.  If and when we do need it, a
  // const cast might be required somewhere.
  const void* data = nullptr;

  Operation op;
};

// Sums the |length| of all requests. It will assert if overflow occurs; the caller is responsible
// for making sure this does not happen.
uint64_t BlockCount(cpp20::span<const UnbufferedOperation> operations);

}  // namespace storage

#endif  // STORAGE_OPERATION_UNBUFFERED_OPERATION_H_
