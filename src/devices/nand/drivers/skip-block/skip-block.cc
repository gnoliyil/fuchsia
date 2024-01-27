// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/nand/drivers/skip-block/skip-block.h"

#include <fuchsia/hardware/badblock/c/banjo.h>
#include <fuchsia/hardware/nand/c/banjo.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/metadata.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/sync/completion.h>
#include <lib/zbi-format/zbi.h>
#include <lib/zx/vmo.h>
#include <string.h>
#include <zircon/status.h>

#include <algorithm>
#include <memory>
#include <utility>

#include <ddktl/fidl.h>
#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>

#include "src/devices/nand/drivers/skip-block/skip-block-bind.h"

namespace nand {

namespace {
struct BlockOperationContext {
  ReadWriteOperation op;
  nand_info_t* nand_info;
  LogicalToPhysicalMap* block_map;
  ddk::NandProtocolClient* nand;
  uint32_t copy;
  uint32_t current_block;
  uint32_t physical_block;
  sync_completion_t* completion_event;
  zx_status_t status;
  bool mark_bad;
  std::optional<PageRange> write_page_range;
};

// Called when all page reads in a block finish. If another block still needs
// to be read, it queues it up as another operation.
void ReadCompletionCallback(void* cookie, zx_status_t status, nand_operation_t* op) {
  auto* ctx = static_cast<BlockOperationContext*>(cookie);
  if (status != ZX_OK || ctx->current_block + 1 == ctx->op.block + ctx->op.block_count) {
    ctx->status = status;
    ctx->mark_bad = false;
    sync_completion_signal(ctx->completion_event);
    return;
  }
  ctx->current_block += 1;

  status = ctx->block_map->GetPhysical(ctx->copy, ctx->current_block, &ctx->physical_block);
  if (status != ZX_OK) {
    ctx->status = status;
    ctx->mark_bad = false;
    sync_completion_signal(ctx->completion_event);
    return;
  }

  op->rw.offset_nand = ctx->physical_block * ctx->nand_info->pages_per_block;
  op->rw.offset_data_vmo += ctx->nand_info->pages_per_block;
  ctx->nand->Queue(op, ReadCompletionCallback, cookie);
  return;
}

void EraseCompletionCallback(void* cookie, zx_status_t status, nand_operation_t* op);

// Called when all page writes in a block finish. If another block still needs
// to be written, it queues up an erase.
void WriteCompletionCallback(void* cookie, zx_status_t status, nand_operation_t* op) {
  auto* ctx = static_cast<BlockOperationContext*>(cookie);

  if (status != ZX_OK || ctx->current_block + 1 == ctx->op.block + ctx->op.block_count) {
    ctx->status = status;
    ctx->mark_bad = (status == ZX_ERR_IO);
    sync_completion_signal(ctx->completion_event);
    return;
  }
  ctx->current_block += 1;

  status = ctx->block_map->GetPhysical(ctx->copy, ctx->current_block, &ctx->physical_block);
  if (status != ZX_OK) {
    ctx->status = status;
    ctx->mark_bad = false;
    sync_completion_signal(ctx->completion_event);
    return;
  }
  op->erase.command = NAND_OP_ERASE;
  op->erase.first_block = ctx->physical_block;
  op->erase.num_blocks = 1;
  ctx->nand->Queue(op, EraseCompletionCallback, cookie);
  return;
}

// Given a block specified in |ctx| and the target page range to write to specified by |page_range|,
// compute the write range within this block and the offset into input vmo.
void ComputeInBlockWriteRangeFromPageRange(const BlockOperationContext* ctx,
                                           const PageRange& page_range, size_t* in_block_offset,
                                           size_t* write_size, size_t* vmo_offset) {
  // All following quantities are logical page indices relative to logical page 0 of the storage.
  // Page index of the current block
  const size_t block_start_page = ctx->current_block * ctx->nand_info->pages_per_block;
  // Page index of the end of current block
  const size_t block_end_page = block_start_page + ctx->nand_info->pages_per_block;
  // Page index where the write within the current block should start.
  const size_t write_start_page = std::max(block_start_page, page_range.page_offset);
  // Page index where the write within the current block should end.
  const size_t write_end_page =
      std::min(block_end_page, page_range.page_offset + page_range.page_count);
  // |write_start_page - page_range.page_offset| is essentially the number of pages we have
  // written so far.
  *vmo_offset = ctx->op.vmo_offset + write_start_page - page_range.page_offset;
  *in_block_offset = write_start_page - block_start_page;
  *write_size = write_end_page - write_start_page;
}

// Called when a block erase operation finishes. Subsequently queues up writes
// to the block.
void EraseCompletionCallback(void* cookie, zx_status_t status, nand_operation_t* op) {
  auto* ctx = static_cast<BlockOperationContext*>(cookie);
  if (status != ZX_OK) {
    ctx->status = status;
    ctx->mark_bad = (status == ZX_ERR_IO);
    sync_completion_signal(ctx->completion_event);
    return;
  }

  size_t in_block_offset = 0;
  size_t write_size = ctx->nand_info->pages_per_block;
  size_t vmo_offset =
      ctx->op.vmo_offset + ((ctx->current_block - ctx->op.block) * ctx->nand_info->pages_per_block);

  if (ctx->write_page_range) {
    ComputeInBlockWriteRangeFromPageRange(ctx, *(ctx->write_page_range), &in_block_offset,
                                          &write_size, &vmo_offset);
  }

  const size_t offset_nand =
      ctx->physical_block * ctx->nand_info->pages_per_block + in_block_offset;

  ZX_ASSERT(write_size <= UINT32_MAX);
  ZX_ASSERT(offset_nand <= UINT32_MAX);

  op->rw.command = NAND_OP_WRITE;
  op->rw.data_vmo = ctx->op.vmo.get();
  op->rw.oob_vmo = ZX_HANDLE_INVALID;
  op->rw.length = static_cast<uint32_t>(write_size);
  op->rw.offset_nand = static_cast<uint32_t>(offset_nand);
  op->rw.offset_data_vmo = vmo_offset;

  ctx->nand->Queue(op, WriteCompletionCallback, cookie);
  return;
}

}  // namespace

zx_status_t SkipBlockDevice::Create(void*, zx_device_t* parent) {
  // Get NAND protocol.
  ddk::NandProtocolClient nand(parent);
  if (!nand.is_valid()) {
    zxlogf(ERROR, "skip-block: parent device: does not support nand protocol");
    return ZX_ERR_NOT_SUPPORTED;
  }

  // Get bad block protocol.
  ddk::BadBlockProtocolClient bad_block(parent);
  if (!bad_block.is_valid()) {
    zxlogf(ERROR, "skip-block: parent device: does not support bad_block protocol");
    return ZX_ERR_NOT_SUPPORTED;
  }

  uint32_t copy_count;
  size_t actual;
  zx_status_t status = device_get_metadata(parent, DEVICE_METADATA_PRIVATE, &copy_count,
                                           sizeof(copy_count), &actual);
  if (status != ZX_OK) {
    zxlogf(ERROR, "skip-block: parent device has no private metadata");
    return status;
  }
  if (actual != sizeof(copy_count)) {
    zxlogf(ERROR, "skip-block: Private metadata is of size %zu, expected to be %zu", actual,
           sizeof(copy_count));
    return ZX_ERR_INTERNAL;
  }

  fbl::AllocChecker ac;
  std::unique_ptr<SkipBlockDevice> device(new (&ac)
                                              SkipBlockDevice(parent, nand, bad_block, copy_count));
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  status = device->Bind();
  if (status != ZX_OK) {
    return status;
  }

  // devmgr is now in charge of the device.
  [[maybe_unused]] auto* dummy = device.release();
  return ZX_OK;
}

zx_status_t SkipBlockDevice::GetBadBlockList(fbl::Array<uint32_t>* bad_blocks) {
  size_t bad_block_count;
  zx_status_t status = bad_block_.GetBadBlockList(nullptr, 0, &bad_block_count);
  if (status != ZX_OK) {
    return status;
  }
  if (bad_block_count == 0) {
    bad_blocks->reset();
    return ZX_OK;
  }
  const size_t bad_block_list_len = bad_block_count;
  std::unique_ptr<uint32_t[]> bad_block_list(new uint32_t[bad_block_count]);
  memset(bad_block_list.get(), 0, sizeof(uint32_t) * bad_block_count);
  status = bad_block_.GetBadBlockList(bad_block_list.get(), bad_block_list_len, &bad_block_count);
  if (status != ZX_OK) {
    return status;
  }
  if (bad_block_list_len != bad_block_count) {
    return ZX_ERR_INTERNAL;
  }
  *bad_blocks = fbl::Array<uint32_t>(bad_block_list.release(), bad_block_count);
  return ZX_OK;
}

zx_status_t SkipBlockDevice::Bind() {
  zxlogf(INFO, "skip-block: Binding");

  fbl::AutoLock al(&lock_);

  if (sizeof(nand_operation_t) > parent_op_size_) {
    zxlogf(ERROR, "skip-block: parent op size, %zu, is smaller than minimum op size: %zu",
           parent_op_size_, sizeof(nand_operation_t));
    return ZX_ERR_INTERNAL;
  }

  nand_op_ = NandOperation::Alloc(parent_op_size_);
  if (!nand_op_) {
    return ZX_ERR_NO_MEMORY;
  }

  // TODO(surajmalhotra): Potentially make this lazy instead of in the bind.
  fbl::Array<uint32_t> bad_blocks;
  const zx_status_t status = GetBadBlockList(&bad_blocks);
  if (status != ZX_OK) {
    zxlogf(ERROR, "skip-block: Failed to get bad block list");
    return status;
  }
  block_map_ = LogicalToPhysicalMap(copy_count_, nand_info_.num_blocks, std::move(bad_blocks));

  return DdkAdd("skip-block");
}

void SkipBlockDevice::GetPartitionInfo(GetPartitionInfoCompleter::Sync& completer) {
  fbl::AutoLock al(&lock_);

  PartitionInfo info;
  info.block_size_bytes = GetBlockSize();
  info.partition_block_count = GetBlockCountLocked();
  memcpy(info.partition_guid.data(), nand_info_.partition_guid, ZBI_PARTITION_GUID_LEN);

  completer.Reply(ZX_OK, info);
}

zx_status_t SkipBlockDevice::ValidateOperationLocked(const ReadWriteOperation& op) const {
  if (op.block_count == 0) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (op.block + op.block_count > GetBlockCountLocked()) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  uint64_t vmo_size;
  zx_status_t status = op.vmo.get_size(&vmo_size);
  if (status != ZX_OK) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (vmo_size < op.vmo_offset + op.block_count * GetBlockSize()) {
    return ZX_ERR_OUT_OF_RANGE;
  }
  return ZX_OK;
}

zx_status_t SkipBlockDevice::ValidateOperationLocked(const WriteBytesOperation& op) const {
  if (op.size == 0) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (op.offset % nand_info_.page_size != 0 || op.size % nand_info_.page_size != 0) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (fbl::round_up(op.offset + op.size, GetBlockSize()) > GetBlockCountLocked() * GetBlockSize()) {
    return ZX_ERR_OUT_OF_RANGE;
  }

  uint64_t vmo_size;
  zx_status_t status = op.vmo.get_size(&vmo_size);
  if (status != ZX_OK) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (vmo_size < op.vmo_offset + op.size) {
    return ZX_ERR_OUT_OF_RANGE;
  }
  return ZX_OK;
}

zx_status_t SkipBlockDevice::ReadLocked(ReadWriteOperation op) {
  for (uint32_t copy = 0; copy < copy_count_; copy++) {
    if (block_map_.AvailableBlockCount(copy) < op.block_count) {
      zxlogf(INFO, "skipblock: copy %u too small, skipping read attempt.", copy);
      continue;
    }

    uint32_t physical_block;
    zx_status_t status = block_map_.GetPhysical(copy, op.block, &physical_block);
    if (status != ZX_OK) {
      return status;
    }
    sync_completion_t completion;
    BlockOperationContext op_context = {
        .op = std::move(op),
        .nand_info = &nand_info_,
        .block_map = &block_map_,
        .nand = &nand_,
        .copy = copy,
        .current_block = op.block,
        .physical_block = physical_block,
        .completion_event = &completion,
        .status = ZX_OK,
        .mark_bad = false,
    };

    nand_operation_t* nand_op = nand_op_->operation();
    nand_op->rw.command = NAND_OP_READ;
    nand_op->rw.data_vmo = op_context.op.vmo.get();
    nand_op->rw.oob_vmo = ZX_HANDLE_INVALID;
    nand_op->rw.length = nand_info_.pages_per_block;
    nand_op->rw.offset_nand = physical_block * nand_info_.pages_per_block;
    nand_op->rw.offset_data_vmo = op.vmo_offset;
    // The read callback will enqueue subsequent reads.
    nand_.Queue(nand_op, ReadCompletionCallback, &op_context);

    // Wait on completion.
    sync_completion_wait(&completion, ZX_TIME_INFINITE);
    op = std::move(op_context.op);
    if (op_context.status == ZX_OK) {
      if (copy != 0) {
        zxlogf(INFO, "skipblock: Successfully read block %d, copy %d", op_context.current_block,
               copy);
      }
      return ZX_OK;
    }
    zxlogf(WARNING, "skipblock: Failed to read block %d, copy %d, with status %s",
           op_context.current_block, copy, zx_status_get_string(op_context.status));
  }
  zxlogf(ERROR, "skipblock: Failed to read any copies of block %d", op.block);
  return ZX_ERR_IO;
}

void SkipBlockDevice::Read(ReadRequestView request, ReadCompleter::Sync& completer) {
  fbl::AutoLock al(&lock_);

  zx_status_t status = ValidateOperationLocked(request->op);
  if (status != ZX_OK) {
    completer.Reply(status);
    return;
  }
  completer.Reply(ReadLocked(std::move(request->op)));
}

zx_status_t SkipBlockDevice::WriteLocked(ReadWriteOperation op, bool* bad_block_grown,
                                         std::optional<PageRange> write_page_range) {
  *bad_block_grown = false;

  bool one_copy_succeeded = false;
  for (uint32_t copy = 0; copy < copy_count_; copy++) {
    for (;;) {
      if (op.block >= block_map_.AvailableBlockCount(copy)) {
        break;
      }
      uint32_t physical_block;
      zx_status_t status = block_map_.GetPhysical(copy, op.block, &physical_block);
      if (status != ZX_OK) {
        return status;
      }

      sync_completion_t completion;
      BlockOperationContext op_context = {
          .op = std::move(op),
          .nand_info = &nand_info_,
          .block_map = &block_map_,
          .nand = &nand_,
          .copy = copy,
          .current_block = op.block,
          .physical_block = physical_block,
          .completion_event = &completion,
          .status = ZX_OK,
          .mark_bad = false,
          .write_page_range = write_page_range,
      };

      nand_operation_t* nand_op = nand_op_->operation();
      nand_op->erase.command = NAND_OP_ERASE;
      nand_op->erase.first_block = physical_block;
      nand_op->erase.num_blocks = 1;
      // The erase callback will enqueue subsequent writes and erases.
      nand_.Queue(nand_op, EraseCompletionCallback, &op_context);

      // Wait on completion.
      sync_completion_wait(&completion, ZX_TIME_INFINITE);
      op = std::move(op_context.op);
      if (op_context.mark_bad) {
        zxlogf(ERROR, "Failed to erase/write block %u, marking bad", op_context.physical_block);
        status = bad_block_.MarkBlockBad(op_context.physical_block);
        if (status != ZX_OK) {
          zxlogf(ERROR, "skip-block: Failed to mark block bad");
          return status;
        }
        // Logical to physical mapping has changed, so we need to re-initialize block_map_.
        fbl::Array<uint32_t> bad_blocks;
        // TODO(surajmalhotra): Make it impossible for this to fail.
        ZX_ASSERT(GetBadBlockList(&bad_blocks) == ZX_OK);
        block_map_ =
            LogicalToPhysicalMap(copy_count_, nand_info_.num_blocks, std::move(bad_blocks));
        *bad_block_grown = true;
        continue;
      }
      if (op_context.status != ZX_OK) {
        zxlogf(ERROR, "Failed to write block %d, copy %d with status %s", op_context.current_block,
               copy, zx_status_get_string(op_context.status));
        break;
      }
      one_copy_succeeded = true;
      break;
    }
  }
  if (!one_copy_succeeded) {
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

void SkipBlockDevice::Write(WriteRequestView request, WriteCompleter::Sync& completer) {
  fbl::AutoLock al(&lock_);

  bool bad_block_grown = false;
  zx_status_t status = ValidateOperationLocked(request->op);
  if (status != ZX_OK) {
    completer.Reply(status, bad_block_grown);
    return;
  }

  status = WriteLocked(std::move(request->op), &bad_block_grown, std::nullopt);
  completer.Reply(status, bad_block_grown);
}

zx_status_t SkipBlockDevice::ReadPartialBlocksLocked(WriteBytesOperation op, uint64_t block_size,
                                                     uint64_t first_block, uint64_t last_block,
                                                     uint64_t op_size, zx::vmo* vmo) {
  zx_status_t status = zx::vmo::create(op_size, 0, vmo);
  if (status != ZX_OK) {
    return status;
  }

  if (op.offset % block_size) {
    // Need to read first block.
    zx::vmo dup;
    status = vmo->duplicate(ZX_RIGHT_SAME_RIGHTS, &dup);
    if (status != ZX_OK) {
      return status;
    }
    ReadWriteOperation rw_op = {
        .vmo = std::move(dup),
        .vmo_offset = 0,
        .block = static_cast<uint32_t>(first_block),
        .block_count = 1,
    };
    status = ReadLocked(std::move(rw_op));
    if (status != ZX_OK) {
      return status;
    }
  }

  if ((first_block != last_block || op.offset % block_size == 0) &&
      (op.offset + op.size) % block_size != 0) {
    // Need to read last block.
    zx::vmo dup;
    status = vmo->duplicate(ZX_RIGHT_SAME_RIGHTS, &dup);
    if (status != ZX_OK) {
      return status;
    }
    ReadWriteOperation rw_op = {
        .vmo = std::move(dup),
        .vmo_offset = last_block * block_size,
        .block = static_cast<uint32_t>(last_block),
        .block_count = 1,
    };
    status = ReadLocked(std::move(rw_op));
    if (status != ZX_OK) {
      return status;
    }
  }

  // Copy from input vmo to newly created one.
  fzl::VmoMapper mapper;
  const size_t vmo_page_offset = op.vmo_offset % ZX_PAGE_SIZE;
  status = mapper.Map(op.vmo, fbl::round_down(op.vmo_offset, ZX_PAGE_SIZE),
                      fbl::round_up(vmo_page_offset + op.size, ZX_PAGE_SIZE), ZX_VM_PERM_READ);
  if (status != ZX_OK) {
    return status;
  }

  status = vmo->write(static_cast<uint8_t*>(mapper.start()) + vmo_page_offset,
                      op.offset % block_size, op.size);
  if (status != ZX_OK) {
    return status;
  }

  return ZX_OK;
}

void SkipBlockDevice::WriteBytes(WriteBytesRequestView request,
                                 WriteBytesCompleter::Sync& completer) {
  fbl::AutoLock al(&lock_);

  bool bad_block_grown = false;
  zx_status_t status = ValidateOperationLocked(request->op);
  if (status != ZX_OK) {
    completer.Reply(status, bad_block_grown);
    return;
  }

  const uint64_t block_size = GetBlockSize();
  const uint64_t first_block = request->op.offset / block_size;
  const uint64_t last_block =
      fbl::round_up(request->op.offset + request->op.size, block_size) / block_size - 1;
  const uint64_t op_size = (last_block - first_block + 1) * block_size;

  if (request->op.mode == WriteBytesMode::kReadModifyEraseWrite) {
    zx::vmo vmo;
    if (op_size == request->op.size) {
      // No copies are necessary as offset and size are block aligned.
      vmo = std::move(request->op.vmo);
    } else {
      status = ReadPartialBlocksLocked(std::move(request->op), block_size, first_block, last_block,
                                       op_size, &vmo);
      if (status != ZX_OK) {
        completer.Reply(status, bad_block_grown);
        return;
      }
    }

    // Now issue normal write.
    ReadWriteOperation rw_op = {
        .vmo = std::move(vmo),
        .vmo_offset = 0,
        .block = static_cast<uint32_t>(first_block),
        .block_count = static_cast<uint32_t>(last_block - first_block + 1),
    };
    status = WriteLocked(std::move(rw_op), &bad_block_grown, {});
  } else if (request->op.mode == WriteBytesMode::kEraseWrite) {
    // No partial read is necessary
    ReadWriteOperation rw_op = {
        .vmo = std::move(request->op.vmo),
        .vmo_offset = 0,
        .block = static_cast<uint32_t>(first_block),
        .block_count = static_cast<uint32_t>(last_block - first_block + 1),
    };
    auto page_size = nand_info_.page_size;
    PageRange page_range{request->op.offset / page_size, request->op.size / page_size};
    status = WriteLocked(std::move(rw_op), &bad_block_grown, page_range);
  }

  completer.Reply(status, bad_block_grown);
}

void SkipBlockDevice::WriteBytesWithoutErase(WriteBytesWithoutEraseRequestView request,
                                             WriteBytesWithoutEraseCompleter::Sync& completer) {
  fbl::AutoLock al(&lock_);

  if (auto status = ValidateOperationLocked(request->op); status != ZX_OK) {
    zxlogf(INFO, "skipblock: Operation param is invalid.\n");
    completer.Reply(status);
    return;
  }

  if (request->op.vmo_offset % nand_info_.page_size) {
    zxlogf(INFO, "skipblock: vmo_offset has to be page aligned for writing without erase");
    completer.Reply(ZX_ERR_INVALID_ARGS);
    return;
  }

  const size_t page_offset = request->op.offset / nand_info_.page_size;
  const size_t page_count = request->op.size / nand_info_.page_size;
  const uint64_t block_size = GetBlockSize();
  const uint64_t first_block = request->op.offset / block_size;
  const uint64_t last_block =
      fbl::round_up(request->op.offset + request->op.size, block_size) / block_size - 1;
  ReadWriteOperation rw_op = {
      .vmo = std::move(request->op.vmo),
      .vmo_offset = request->op.vmo_offset / nand_info_.page_size,
      .block = static_cast<uint32_t>(first_block),
      .block_count = static_cast<uint32_t>(last_block - first_block + 1),
  };

  // Check if write-without-erase goes through
  if (auto res = WriteBytesWithoutEraseLocked(page_offset, page_count, std::move(rw_op));
      res != ZX_OK) {
    zxlogf(INFO, "skipblock: Write without erase failed. %s\n", zx_status_get_string(res));
    completer.Reply(res);
    return;
  }

  completer.Reply(ZX_OK);
}

// Called when a block write-without-erase operation finishes. Subsequently queues up
// write-without-erase to the block.
void WriteBytesWithoutEraseCompletionCallback(void* cookie, zx_status_t status,
                                              nand_operation_t* op) {
  auto* ctx = static_cast<BlockOperationContext*>(cookie);

  if (status != ZX_OK || ctx->current_block == ctx->op.block + ctx->op.block_count ||
      (status = ctx->block_map->GetPhysical(ctx->copy, ctx->current_block, &ctx->physical_block)) !=
          ZX_OK) {
    ctx->status = status;
    sync_completion_signal(ctx->completion_event);
    return;
  }

  size_t in_block_offset, write_size, vmo_offset;
  ComputeInBlockWriteRangeFromPageRange(ctx, *(ctx->write_page_range), &in_block_offset,
                                        &write_size, &vmo_offset);

  const size_t offset_nand =
      ctx->physical_block * ctx->nand_info->pages_per_block + in_block_offset;

  ZX_ASSERT(write_size <= UINT32_MAX);
  ZX_ASSERT(offset_nand <= UINT32_MAX);

  op->rw.command = NAND_OP_WRITE;
  op->rw.data_vmo = ctx->op.vmo.get();
  op->rw.oob_vmo = ZX_HANDLE_INVALID;
  op->rw.length = static_cast<uint32_t>(write_size),
  op->rw.offset_nand = static_cast<uint32_t>(offset_nand);
  op->rw.offset_data_vmo = vmo_offset;
  ctx->current_block += 1;
  ctx->nand->Queue(op, WriteBytesWithoutEraseCompletionCallback, cookie);
}

zx_status_t SkipBlockDevice::WriteBytesWithoutEraseLocked(size_t page_offset, size_t page_count,
                                                          ReadWriteOperation op) {
  uint32_t copy = 0;
  for (; copy < copy_count_ && op.block < block_map_.AvailableBlockCount(copy); copy++) {
    uint32_t physical_block;
    if (auto status = block_map_.GetPhysical(copy, op.block, &physical_block); status != ZX_OK) {
      return status;
    }

    sync_completion_t completion;
    BlockOperationContext op_context{
        .op = std::move(op),
        .nand_info = &nand_info_,
        .block_map = &block_map_,
        .nand = &nand_,
        .copy = copy,
        .current_block = op.block,
        .physical_block = physical_block,
        .completion_event = &completion,
        .status = ZX_OK,
        .mark_bad = false,
        .write_page_range = {{page_offset, page_count}},
    };
    WriteBytesWithoutEraseCompletionCallback(&op_context, ZX_OK, nand_op_->operation());

    // Wait on completion.
    sync_completion_wait(&completion, ZX_TIME_INFINITE);
    op = std::move(op_context.op);
    if (op_context.status != ZX_OK) {
      zxlogf(ERROR, "Failed to write-without-erase block %d, copy %d with status %s\n",
             op_context.current_block, copy, zx_status_get_string(op_context.status));
      return op_context.status;
    }
  }

  return copy == copy_count_ ? ZX_OK : ZX_ERR_IO;
}

uint32_t SkipBlockDevice::GetBlockCountLocked() const {
  uint32_t logical_block_count = 0;
  for (uint32_t copy = 0; copy < copy_count_; copy++) {
    logical_block_count = std::max(logical_block_count, block_map_.AvailableBlockCount(copy));
  }
  return logical_block_count;
}

static constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = SkipBlockDevice::Create;
  return ops;
}();

}  // namespace nand

ZIRCON_DRIVER(skip_block, nand::driver_ops, "zircon", "0.1");
