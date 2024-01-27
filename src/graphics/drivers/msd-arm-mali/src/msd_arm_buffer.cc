// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/drivers/msd-arm-mali/src/msd_arm_buffer.h"

#include "msd_defs.h"
#include "src/graphics/drivers/msd-arm-mali/src/gpu_mapping.h"
#include "src/graphics/drivers/msd-arm-mali/src/msd_arm_connection.h"

MsdArmBuffer::MsdArmBuffer(std::unique_ptr<magma::PlatformBuffer> platform_buf)
    : platform_buf_(std::move(platform_buf)) {}

MsdArmBuffer::~MsdArmBuffer() {
  size_t mapping_count = gpu_mappings_.size();
  for (auto mapping : gpu_mappings_)
    mapping->Remove();
  // The weak pointer to this should already have been invalidated, so
  // Remove() shouldn't be able to modify gpu_mappings_.
  DASSERT(gpu_mappings_.size() == mapping_count);
}

std::unique_ptr<MsdArmBuffer> MsdArmBuffer::Import(zx::vmo handle, uint64_t client_id) {
  auto platform_buf = magma::PlatformBuffer::Import(std::move(handle));
  if (!platform_buf)
    return DRETP(nullptr, "MsdArmBuffer::Create: Could not create platform buffer from token");

  platform_buf->set_local_id(client_id);

  return std::unique_ptr<MsdArmBuffer>(new MsdArmBuffer(std::move(platform_buf)));
}

std::unique_ptr<MsdArmBuffer> MsdArmBuffer::Create(uint64_t size, const char* name) {
  auto platform_buf = magma::PlatformBuffer::Create(size, name);
  if (!platform_buf)
    return DRETP(nullptr, "MsdArmBuffer::Create: Could not create platform buffer from size");

  return std::unique_ptr<MsdArmBuffer>(new MsdArmBuffer(std::move(platform_buf)));
}

void MsdArmBuffer::AddMapping(GpuMapping* mapping) {
  DASSERT(!gpu_mappings_.count(mapping));
  gpu_mappings_.insert(mapping);
}

void MsdArmBuffer::RemoveMapping(GpuMapping* mapping) {
  DASSERT(gpu_mappings_.count(mapping));
  gpu_mappings_.erase(mapping);
}

bool MsdArmBuffer::SetCommittedPages(uint64_t start_page, uint64_t page_count) {
  if ((start_page + page_count) * PAGE_SIZE > platform_buffer()->size())
    return DRETF(false, "invalid parameters start_page %lu page_count %lu", start_page, page_count);

  committed_region_ = Region::FromStartAndLength(start_page, page_count);

  bool success = true;
  for (auto& mapping : gpu_mappings_) {
    if (!mapping->UpdateCommittedMemory())
      success = false;
  }
  return success;
}

bool MsdArmBuffer::CommitPageRange(uint64_t start_page, uint64_t page_count) {
  if ((start_page + page_count) * PAGE_SIZE > platform_buffer()->size())
    return DRETF(false, "invalid parameters start_page %lu page_count %lu", start_page, page_count);

  committed_region_.Union(Region::FromStartAndLength(start_page, page_count));

  bool success = true;
  for (auto& mapping : gpu_mappings_) {
    if (!mapping->UpdateCommittedMemory())
      success = false;
  }
  return success;
}

bool MsdArmBuffer::DecommitPageRange(uint64_t start_page, uint64_t page_count) {
  DASSERT((start_page + page_count) * PAGE_SIZE <= platform_buffer()->size());

  auto decommit_region = Region::FromStartAndLength(start_page, page_count);
  Region new_region(committed_region_);
  if (!new_region.Subtract(decommit_region)) {
    DLOG("Trying to decommit region in middle, ignoring");
  }
  committed_region_ = new_region;
  bool success = true;
  for (auto& mapping : gpu_mappings_) {
    if (!mapping->UpdateCommittedMemory())
      success = false;
  }
  return success;
}

bool MsdArmBuffer::EnsureRegionFlushed(uint64_t start_bytes, uint64_t end_bytes) {
  DASSERT(end_bytes >= start_bytes);

  Region new_flushed_region = flushed_region_;
  new_flushed_region.Union(Region::FromStartAndEnd(start_bytes, end_bytes));
  auto [left_to_flush, right_to_flush] = new_flushed_region.SubtractWithSplit(flushed_region_);
  if (!left_to_flush.empty()) {
    if (!platform_buf_->CleanCache(left_to_flush.start(), left_to_flush.length(), false))
      return DRETF(false, "CleanCache of start failed");
  }
  if (!right_to_flush.empty()) {
    if (!platform_buf_->CleanCache(right_to_flush.start(), right_to_flush.length(), false))
      return DRETF(false, "CleanCache of end failed");
  }
  flushed_region_ = new_flushed_region;
  return true;
}
