// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "msd_vsi_buffer.h"

std::unique_ptr<MsdVsiBuffer> MsdVsiBuffer::Import(uint32_t handle, uint64_t client_id) {
  auto platform_buf = magma::PlatformBuffer::Import(handle);
  if (!platform_buf) {
    MAGMA_LOG(ERROR, "failed to import buffer handle 0x%x", handle);
    return nullptr;
  }

  platform_buf->set_local_id(client_id);

  return std::make_unique<MsdVsiBuffer>(std::move(platform_buf));
}

std::unique_ptr<MsdVsiBuffer> MsdVsiBuffer::Create(uint64_t size, const char* name) {
  auto platform_buf = magma::PlatformBuffer::Create(size, name);
  if (!platform_buf) {
    MAGMA_LOG(ERROR, "failed to create buffer size %lu", size);
    return nullptr;
  }
  return std::make_unique<MsdVsiBuffer>(std::move(platform_buf));
}

//////////////////////////////////////////////////////////////////////////////

msd_buffer_t* msd_buffer_import(uint32_t handle, uint64_t client_id) {
  auto buffer = MsdVsiBuffer::Import(handle, client_id);
  if (!buffer) {
    MAGMA_LOG(ERROR, "failed to import buffer handle 0x%x", handle);
    return nullptr;
  }

  return new MsdVsiAbiBuffer(std::move(buffer));
}

void msd_buffer_destroy(msd_buffer_t* buf) { delete MsdVsiAbiBuffer::cast(buf); }
