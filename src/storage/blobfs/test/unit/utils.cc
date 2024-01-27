// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/blobfs/test/unit/utils.h"

#include <memory>
#include <vector>

#include <gtest/gtest.h>
#include <safemath/checked_math.h>
#include <storage/buffer/owned_vmoid.h>

using id_allocator::IdAllocator;

namespace blobfs {

namespace {

storage::OwnedVmoid AttachVmo(BlockDevice* device, zx::vmo* vmo) {
  storage::Vmoid vmoid;
  EXPECT_EQ(device->BlockAttachVmo(*vmo, &vmoid), ZX_OK);
  return storage::OwnedVmoid(std::move(vmoid), device);
}

// Verify that the |size| and |offset| are |device| block size aligned.
// Returns block_size in |out_block_size|.
void VerifySizeBlockAligned(BlockDevice* device, size_t size, uint64_t offset,
                            uint32_t* out_block_size) {
  fuchsia_hardware_block::wire::BlockInfo info = {};
  ASSERT_EQ(device->BlockGetInfo(&info), ZX_OK);
  ASSERT_EQ(size % info.block_size, 0ul);
  ASSERT_EQ(offset % info.block_size, 0ul);
  *out_block_size = info.block_size;
}

}  // namespace

zx_status_t MockTransactionManager::FifoTransaction(block_fifo_request_t* requests, size_t count) {
  fbl::AutoLock lock(&lock_);

  if (transaction_callback_) {
    for (size_t i = 0; i < count; i++) {
      if (attached_vmos_.size() < requests[i].vmoid) {
        return ZX_ERR_INVALID_ARGS;
      }

      std::optional<zx::vmo>* optional_vmo = &attached_vmos_[requests[i].vmoid - 1];

      if (!optional_vmo->has_value()) {
        return ZX_ERR_BAD_STATE;
      }

      const zx::vmo& dest_vmo = optional_vmo->value();

      if (dest_vmo.get() == ZX_HANDLE_INVALID) {
        return ZX_ERR_INVALID_ARGS;
      }

      zx_status_t status = transaction_callback_(requests[i], dest_vmo);
      if (status != ZX_OK) {
        return status;
      }
    }
  }

  return ZX_OK;
}

zx_status_t MockTransactionManager::BlockAttachVmo(const zx::vmo& vmo, storage::Vmoid* out) {
  fbl::AutoLock lock(&lock_);
  zx::vmo duplicate_vmo;
  zx_status_t status = vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &duplicate_vmo);
  if (status != ZX_OK) {
    return status;
  }
  attached_vmos_.push_back(std::move(duplicate_vmo));
  *out = storage::Vmoid(safemath::checked_cast<uint16_t>(attached_vmos_.size()));
  if (out->get() == 0) {
    return ZX_ERR_OUT_OF_RANGE;
  }
  return ZX_OK;
}

zx_status_t MockTransactionManager::BlockDetachVmo(storage::Vmoid vmoid) {
  fbl::AutoLock lock(&lock_);
  vmoid_t id = vmoid.TakeId();
  if (attached_vmos_.size() < id) {
    return ZX_ERR_INVALID_ARGS;
  }

  attached_vmos_[id - 1].reset();
  return ZX_OK;
}

// Create a block and node map of the requested size, update the superblock of
// the |space_manager|, and create an allocator from this provided info.
void InitializeAllocator(size_t blocks, size_t nodes, MockSpaceManager* space_manager,
                         std::unique_ptr<Allocator>* out) {
  RawBitmap block_map;
  ASSERT_EQ(block_map.Reset(blocks), ZX_OK);
  fzl::ResizeableVmoMapper node_map;
  ASSERT_EQ(node_map.CreateAndMap(nodes * kBlobfsBlockSize, "node map"), ZX_OK);

  space_manager->MutableInfo().inode_count = nodes;
  space_manager->MutableInfo().data_block_count = blocks;
  std::unique_ptr<IdAllocator> nodes_bitmap = {};
  ASSERT_EQ(IdAllocator::Create(nodes, &nodes_bitmap), ZX_OK);
  *out = std::make_unique<Allocator>(space_manager, std::move(block_map), std::move(node_map),
                                     std::move(nodes_bitmap));
  (*out)->SetLogging(false);
}

// Force the allocator to become maximally fragmented by allocating
// every-other block within up to |blocks|.
void ForceFragmentation(Allocator* allocator, size_t blocks) {
  std::vector<std::vector<ReservedExtent>> extents(blocks);
  for (size_t i = 0; i < blocks; i++) {
    ASSERT_EQ(allocator->ReserveBlocks(1, &extents[i]), ZX_OK);
    ASSERT_EQ(1ul, extents[i].size());
  }

  for (size_t i = 0; i < blocks; i += 2) {
    allocator->MarkBlocksAllocated(extents[i][0]);
  }
}

// Save the extents within |in| in a non-reserved vector |out|.
void CopyExtents(const std::vector<ReservedExtent>& in, std::vector<Extent>* out) {
  out->reserve(in.size());
  for (const ReservedExtent& extent : in) {
    out->push_back(extent.extent());
  }
}

// Save the nodes within |in| in a non-reserved vector |out|.
void CopyNodes(const std::vector<ReservedNode>& in, std::vector<uint32_t>* out) {
  out->reserve(in.size());
  for (const ReservedNode& node : in) {
    out->push_back(node.index());
  }
}

void DeviceBlockRead(BlockDevice* device, void* buf, size_t size, uint64_t dev_offset) {
  uint32_t block_size;
  VerifySizeBlockAligned(device, size, dev_offset, &block_size);

  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(size, 0, &vmo), ZX_OK);

  storage::OwnedVmoid vmoid = AttachVmo(device, &vmo);

  block_fifo_request_t request;
  request.command = {.opcode = BLOCK_OPCODE_READ, .flags = 0};
  request.vmoid = vmoid.get();
  request.length = safemath::checked_cast<uint32_t>(size / block_size);
  request.vmo_offset = 0;
  request.dev_offset = safemath::checked_cast<uint32_t>(dev_offset / block_size);
  ASSERT_EQ(device->FifoTransaction(&request, 1), ZX_OK);
  ASSERT_EQ(vmo.read(buf, 0, size), ZX_OK);
}

void DeviceBlockWrite(BlockDevice* device, const void* buf, size_t size, uint64_t dev_offset) {
  uint32_t block_size;
  VerifySizeBlockAligned(device, size, dev_offset, &block_size);

  zx::vmo vmo;
  ASSERT_EQ(zx::vmo::create(size, 0, &vmo), ZX_OK);
  ASSERT_EQ(vmo.write(buf, 0, size), ZX_OK);

  storage::OwnedVmoid vmoid = AttachVmo(device, &vmo);

  block_fifo_request_t request;
  request.command = {.opcode = BLOCK_OPCODE_WRITE, .flags = 0};
  request.vmoid = vmoid.get();
  request.length = safemath::checked_cast<uint32_t>(size / block_size);
  request.vmo_offset = 0;
  request.dev_offset = safemath::checked_cast<uint32_t>(dev_offset / block_size);
  ASSERT_EQ(device->FifoTransaction(&request, 1), ZX_OK);
}

std::string GetCompressionAlgorithmName(CompressionAlgorithm compression_algorithm) {
  // CompressionAlgorithmToString can't be used because it contains underscores which aren't
  // allowed in test names.
  switch (compression_algorithm) {
    case CompressionAlgorithm::kUncompressed:
      return "Uncompressed";
    case CompressionAlgorithm::kChunked:
      return "Chunked";
  }

  ZX_DEBUG_ASSERT(false);
  return "INVALID";
}

}  // namespace blobfs
