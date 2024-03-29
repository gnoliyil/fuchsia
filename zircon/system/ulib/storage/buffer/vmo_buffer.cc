// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "storage/buffer/vmo_buffer.h"

#include <zircon/assert.h>
#include <zircon/status.h>

#include <utility>

#include <safemath/checked_math.h>

namespace storage {

VmoBuffer::VmoBuffer(VmoBuffer&& other)
    : vmoid_registry_(std::move(other.vmoid_registry_)),
      mapper_(std::move(other.mapper_)),
      vmoid_(std::move(other.vmoid_)),
      block_size_(other.block_size_),
      capacity_(other.capacity_) {
  other.Reset();
}

VmoBuffer& VmoBuffer::operator=(VmoBuffer&& other) {
  if (&other != this) {
    vmoid_registry_ = other.vmoid_registry_;
    mapper_ = std::move(other.mapper_);
    vmoid_ = std::move(other.vmoid_);
    block_size_ = other.block_size_;
    capacity_ = other.capacity_;

    other.Reset();
  }
  return *this;
}

VmoBuffer::~VmoBuffer() {
  if (vmoid_.IsAttached()) {
    vmoid_registry_->BlockDetachVmo(std::move(vmoid_));
  }
}

void VmoBuffer::Reset() {
  if (vmoid_.IsAttached()) {
    vmoid_registry_->BlockDetachVmo(std::move(vmoid_));
  }
  vmoid_registry_ = nullptr;
  mapper_.Reset();
  capacity_ = 0;
}

zx_status_t VmoBuffer::Initialize(storage::VmoidRegistry* vmoid_registry, size_t blocks,
                                  uint32_t block_size, const char* label) {
  ZX_DEBUG_ASSERT(!vmoid_.IsAttached());
  fzl::OwnedVmoMapper mapper;
  auto size = safemath::CheckMul(uint64_t{blocks}, block_size);
  if (!size.IsValid())
    return ZX_ERR_INVALID_ARGS;
  zx_status_t status = mapper.CreateAndMap(size.ValueOrDie(), label);
  if (status != ZX_OK) {
    return status;
  }

  status = vmoid_registry->BlockAttachVmo(mapper.vmo(), &vmoid_);
  if (status != ZX_OK) {
    return status;
  }

  vmoid_registry_ = vmoid_registry;
  mapper_ = std::move(mapper);
  block_size_ = block_size;
  capacity_ = mapper_.size() / block_size;
  return ZX_OK;
}

void* VmoBuffer::Data(size_t index) {
  return const_cast<void*>(const_cast<const VmoBuffer*>(this)->Data(index));
}

const void* VmoBuffer::Data(size_t index) const {
  ZX_DEBUG_ASSERT(index < capacity_);
  return reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(mapper_.start()) +
                                       (index * block_size_));
}

zx_status_t VmoBuffer::Zero(size_t index, size_t count) {
  return mapper_.vmo().op_range(ZX_VMO_OP_ZERO, index * BlockSize(), count * BlockSize(), nullptr,
                                0);
}

}  // namespace storage
