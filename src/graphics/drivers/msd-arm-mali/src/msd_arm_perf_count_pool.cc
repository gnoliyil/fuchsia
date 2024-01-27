// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/graphics/drivers/msd-arm-mali/src/msd_arm_perf_count_pool.h"

#include <lib/zx/clock.h>

void MsdArmPerfCountPool::OnPerfCountDump(const std::vector<uint32_t>& dumped) {
  std::lock_guard lock(device_thread_checker_);
  OnPerfCountDumpLocked(dumped);
}

void MsdArmPerfCountPool::OnPerfCountDumpLocked(const std::vector<uint32_t>& dumped) {
  if (!valid_)
    return;

  std::vector<uint32_t> triggers = std::move(triggers_);
  triggers_.clear();

  for (uint32_t i = 0; i < triggers.size(); ++i) {
    if (buffers_.empty()) {
      DLOG("No available perf count buffers, dropping write");
      discontinuous_ = true;
      return;
    }
    BufferOffset buffer = std::move(buffers_.front());
    buffers_.pop_front();

    void* data;
    if (!buffer.buffer->platform_buffer()->MapCpu(&data)) {
      DLOG("Failed to map performance counter buffer");
      return;
    }
    uint8_t* out_addr = reinterpret_cast<uint8_t*>(data) + buffer.offset;
    uint32_t size_to_write = magma::to_uint32(dumped.size() * sizeof(uint32_t));
    if (size_to_write > buffer.size) {
      DLOG("Truncating write to perf count buffer");
      size_to_write = magma::to_uint32(buffer.size);
    }
    if (i == 0) {
      memcpy(out_addr, dumped.data(), size_to_write);
    } else {
      // For later events, we just set all the counters to 0 to pretend that they've been cleared.
      memset(out_addr, 0, size_to_write);
    }
    buffer.buffer->platform_buffer()->UnmapCpu();

    msd::PerfCounterResult result{};
    result.pool_id = pool_id_;
    result.buffer_id = buffer.buffer_id;
    result.buffer_offset = magma::to_uint32(buffer.offset);
    result.result_flags = discontinuous_ ? MAGMA_PERF_COUNTER_RESULT_DISCONTINUITY : 0;
    discontinuous_ = false;
    result.trigger_id = triggers[i];
    result.timestamp = zx::clock::get_monotonic().get();
    // MagmaSystemConnection should destroy the pool before destroying connection.
    auto connection = connection_.lock();
    DASSERT(connection);
    connection->SendPerfCounterNotification(result);
  }
}

void MsdArmPerfCountPool::OnPerfCountersCanceled(size_t perf_count_size) {
  std::lock_guard lock(device_thread_checker_);
  discontinuous_ = true;
  OnPerfCountDumpLocked(std::vector<uint32_t>(perf_count_size / sizeof(uint32_t)));
}

void MsdArmPerfCountPool::AddBuffer(std::shared_ptr<MsdArmBuffer> buffer, uint64_t buffer_id,
                                    uint64_t offset, uint64_t size) {
  std::lock_guard lock(device_thread_checker_);
  buffers_.push_back({buffer, buffer_id, offset, size});
}

void MsdArmPerfCountPool::RemoveBuffer(std::shared_ptr<MsdArmBuffer> buffer) {
  std::lock_guard lock(device_thread_checker_);
  for (auto it = buffers_.begin(); it != buffers_.end();) {
    if (it->buffer == buffer) {
      it = buffers_.erase(it);
    } else {
      ++it;
    }
  }
}

void MsdArmPerfCountPool::AddTriggerId(uint32_t trigger_id) {
  std::lock_guard lock(device_thread_checker_);
  triggers_.push_back(trigger_id);
}
