// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/block/drivers/zxcrypt/extra.h"

#include <fuchsia/hardware/block/driver/c/banjo.h>
#include <inttypes.h>
#include <lib/ddk/debug.h>
#include <stddef.h>
#include <stdint.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/listnode.h>
#include <zircon/types.h>

#include "src/devices/block/drivers/zxcrypt/debug.h"

namespace zxcrypt {

zx_status_t extra_op_t::Init(block_op_t* block, block_impl_queue_callback cb, void* _cookie,
                             size_t reserved_blocks) {
  LOG_ENTRY_ARGS("block=%p, reserved_blocks=%zu", block, reserved_blocks);

  list_initialize(&node);
  data = nullptr;
  completion_cb = cb;
  cookie = _cookie;

  switch (block->command.opcode) {
    case BLOCK_OPCODE_READ:
    case BLOCK_OPCODE_WRITE:
      if (add_overflow(block->rw.offset_dev, reserved_blocks, &block->rw.offset_dev)) {
        zxlogf(ERROR, "adjusted offset overflow: block->rw.offset_dev=%" PRIu64 "",
               block->rw.offset_dev);
        return ZX_ERR_OUT_OF_RANGE;
      }
      vmo = block->rw.vmo;
      length = block->rw.length;
      offset_dev = block->rw.offset_dev;
      offset_vmo = block->rw.offset_vmo;
      break;

    default:
      length = 0;
      offset_dev = 0;
      offset_vmo = 0;
      break;
  }
  return ZX_OK;
}

extra_op_t* BlockToExtra(block_op_t* block, size_t op_size) {
  LOG_ENTRY_ARGS("block=%p, op_size=%zu\n", block, op_size);
  ZX_DEBUG_ASSERT(block);

  uint8_t* ptr = reinterpret_cast<uint8_t*>(block);

  return reinterpret_cast<extra_op_t*>(ptr + op_size) - 1;
}

block_op_t* ExtraToBlock(extra_op_t* extra, size_t op_size) {
  LOG_ENTRY_ARGS("extra=%p, op_size=%zu\n", extra, op_size);
  ZX_DEBUG_ASSERT(extra);

  uint8_t* ptr = reinterpret_cast<uint8_t*>(extra + 1);

  return reinterpret_cast<block_op_t*>(ptr - op_size);
}

}  // namespace zxcrypt
