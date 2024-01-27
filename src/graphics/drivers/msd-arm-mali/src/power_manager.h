// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_SRC_POWER_MANAGER_H_
#define SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_SRC_POWER_MANAGER_H_

#include <lib/fit/thread_safety.h>

#include <chrono>
#include <deque>
#include <mutex>

#include <magma_util/macros.h>
#include <magma_util/register_io.h>

#include "mali_register_io.h"
#include "platform_semaphore.h"

// This class generally lives on the device thread.
class PowerManager {
 public:
  PowerManager(mali::RegisterIo* io);

  // Called on the device thread or the initial driver thread.
  void EnableCores(mali::RegisterIo* io, uint64_t shader_bitmask);

  // Called on the GPU interrupt thread.
  void ReceivedPowerInterrupt(mali::RegisterIo* io);

  uint64_t l2_ready_status() const {
    std::lock_guard<std::mutex> lock(ready_status_mutex_);
    return l2_ready_status_;
  }

  // This is called whenever the GPU starts or stops processing work.
  void UpdateGpuActive(bool active);

  // Retrieves information on what fraction of time in the recent past (last
  // 100 ms or so) the GPU was actively processing commands.
  void GetGpuActiveInfo(std::chrono::steady_clock::duration* total_time_out,
                        std::chrono::steady_clock::duration* active_time_out);
  bool GetTotalTime(uint32_t* buffer_out);

  void DisableL2(mali::RegisterIo* io);
  void DisableShaders(mali::RegisterIo* io);
  bool WaitForL2Disable(mali::RegisterIo* io);
  bool WaitForShaderDisable(mali::RegisterIo* io);
  bool WaitForShaderReady(mali::RegisterIo* io);

 private:
  friend class TestMsdArmDevice;
  friend class TestPowerManager;

  struct TimePeriod {
    std::chrono::steady_clock::time_point end_time;
    std::chrono::steady_clock::duration total_time;
    std::chrono::steady_clock::duration active_time;
  };

  void UpdateReadyStatus(mali::RegisterIo* io);
  // Called to update timekeeping and possible update the gpu activity info.
  void UpdateGpuActiveLocked(bool active) FIT_REQUIRES(active_time_mutex_);
  std::deque<TimePeriod>& time_periods() { return time_periods_; }

  mutable std::mutex ready_status_mutex_;
  FIT_GUARDED(ready_status_mutex_) uint64_t tiler_ready_status_ = 0;
  FIT_GUARDED(ready_status_mutex_) uint64_t l2_ready_status_ = 0;

  std::unique_ptr<magma::PlatformSemaphore> power_state_semaphore_;

  std::mutex active_time_mutex_;
  FIT_GUARDED(active_time_mutex_) std::deque<TimePeriod> time_periods_;
  // |gpu_active_| is true if the GPU is currently processing work.
  FIT_GUARDED(active_time_mutex_) bool gpu_active_ = false;
  FIT_GUARDED(active_time_mutex_) std::chrono::steady_clock::time_point last_check_time_;
  FIT_GUARDED(active_time_mutex_) std::chrono::steady_clock::time_point last_trace_time_;

  FIT_GUARDED(active_time_mutex_) uint64_t total_active_time_ = 0u;
};

#endif  // SRC_GRAPHICS_DRIVERS_MSD_ARM_MALI_SRC_POWER_MANAGER_H_
