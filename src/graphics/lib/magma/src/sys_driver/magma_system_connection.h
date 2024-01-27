// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_LIB_MAGMA_SRC_SYS_DRIVER_MAGMA_SYSTEM_CONNECTION_H_
#define SRC_GRAPHICS_LIB_MAGMA_SRC_SYS_DRIVER_MAGMA_SYSTEM_CONNECTION_H_

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "magma_system_buffer.h"
#include "magma_system_context.h"
#include "magma_util/macros.h"
#include "magma_util/platform/platform_connection.h"
#include "msd.h"

using msd_connection_unique_ptr_t =
    std::unique_ptr<msd_connection_t, decltype(&msd_connection_close)>;

static inline msd_connection_unique_ptr_t MsdConnectionUniquePtr(msd_connection_t* conn) {
  return msd_connection_unique_ptr_t(conn, &msd_connection_close);
}

class MagmaSystemDevice;

class MagmaSystemConnection : private MagmaSystemContext::Owner,
                              public magma::PlatformConnection::Delegate {
 public:
  MagmaSystemConnection(std::weak_ptr<MagmaSystemDevice> device,
                        msd_connection_unique_ptr_t msd_connection_t);

  ~MagmaSystemConnection() override;

  magma::Status ImportObject(uint32_t handle, magma::PlatformObject::Type object_type,
                             uint64_t client_id) override;
  magma::Status ReleaseObject(uint64_t object_id, magma::PlatformObject::Type object_type) override;
  magma::Status CreateContext(uint32_t context_id) override;
  magma::Status DestroyContext(uint32_t context_id) override;
  magma::Status ExecuteCommandBufferWithResources(
      uint32_t context_id, std::unique_ptr<magma_command_buffer> command_buffer,
      std::vector<magma_exec_resource> resources, std::vector<uint64_t> semaphores) override;
  magma::Status MapBuffer(uint64_t buffer_id, uint64_t hw_va, uint64_t offset, uint64_t length,
                          uint64_t flags) override;
  magma::Status UnmapBuffer(uint64_t buffer_id, uint64_t hw_va) override;
  magma::Status BufferRangeOp(uint64_t buffer_id, uint32_t op, uint64_t start,
                              uint64_t length) override;
  magma::Status ExecuteImmediateCommands(uint32_t context_id, uint64_t commands_size,
                                         void* commands, uint64_t semaphore_count,
                                         uint64_t* semaphore_ids) override;
  MagmaSystemContext* LookupContext(uint32_t context_id);
  void SetNotificationCallback(msd_connection_notification_callback_t callback,
                               void* token) override;
  magma::Status EnablePerformanceCounterAccess(
      std::unique_ptr<magma::PlatformHandle> access_token) override;
  bool IsPerformanceCounterAccessAllowed() override { return can_access_performance_counters_; }
  magma::Status EnablePerformanceCounters(const uint64_t* counters,
                                          uint64_t counter_count) override;
  magma::Status CreatePerformanceCounterBufferPool(
      std::unique_ptr<magma::PlatformPerfCountPool> pool) override;
  magma::Status ReleasePerformanceCounterBufferPool(uint64_t pool_id) override;
  magma::Status AddPerformanceCounterBufferOffsetToPool(uint64_t pool_id, uint64_t buffer_id,
                                                        uint64_t buffer_offset,
                                                        uint64_t buffer_size) override;
  magma::Status RemovePerformanceCounterBufferFromPool(uint64_t pool_id,
                                                       uint64_t buffer_id) override;
  magma::Status DumpPerformanceCounters(uint64_t pool_id, uint32_t trigger_id) override;
  magma::Status ClearPerformanceCounters(const uint64_t* counters, uint64_t counter_count) override;

  // Create a buffer from the handle and add it to the map,
  // on success |id_out| contains the id to be used to query the map
  magma::Status ImportBuffer(uint32_t handle, uint64_t id);
  // This removes the reference to the shared_ptr in the map
  // other instances remain valid until deleted
  // Returns false if no buffer with the given |id| exists in the map
  magma::Status ReleaseBuffer(uint64_t id);

  // Attempts to locate a buffer by |id| in the buffer map and return it.
  // Returns nullptr if the buffer is not found
  std::shared_ptr<MagmaSystemBuffer> LookupBuffer(uint64_t id);

  // Returns the msd_semaphore for the given |id| if present in the semaphore map.
  std::shared_ptr<MagmaSystemSemaphore> LookupSemaphore(uint64_t id);

  uint32_t GetDeviceId();

  msd_connection_t* msd_connection() { return msd_connection_.get(); }

  void set_can_access_performance_counters(bool can_access) {
    can_access_performance_counters_ = can_access;
  }

 private:
  // TODO(fxbug.dev/100552) - disallow importing an ID multiple times.
  struct BufferReference {
    uint64_t refcount = 1;
    std::shared_ptr<MagmaSystemBuffer> buffer;
  };
  struct PoolReference {
    msd_perf_count_pool* msd_pool;
    std::unique_ptr<magma::PlatformPerfCountPool> platform_pool;
  };

  // MagmaSystemContext::Owner
  std::shared_ptr<MagmaSystemBuffer> LookupBufferForContext(uint64_t id) override {
    return LookupBuffer(id);
  }
  std::shared_ptr<MagmaSystemSemaphore> LookupSemaphoreForContext(uint64_t id) override {
    return LookupSemaphore(id);
  }

  // The returned value is valid until ReleasePerformanceCounterBufferPool is called on it.
  // Otherwise it will always be valid within the lifetime of a call into MagmaSystemConnection on
  // the connection thread.
  msd_perf_count_pool* LookupPerfCountPool(uint64_t id);

  static void NotificationCallback(void* token, struct msd_notification_t* notification);

  std::weak_ptr<MagmaSystemDevice> device_;
  msd_connection_unique_ptr_t msd_connection_;
  std::unordered_map<uint32_t, std::unique_ptr<MagmaSystemContext>> context_map_;
  std::unordered_map<uint64_t, BufferReference> buffer_map_;
  std::unordered_map<uint64_t, std::shared_ptr<MagmaSystemSemaphore>> semaphore_map_;

  msd_connection_notification_callback_t platform_callback_{};
  void* platform_token_{};

  // |pool_map_mutex_| should not be held while calling into the driver. It must be held for
  // modifications to pool_map_ and accesses to pool_map_ from a thread that's not the connection
  // thread.
  std::mutex pool_map_mutex_;
  std::unordered_map<uint64_t, PoolReference> pool_map_;
  bool can_access_performance_counters_ = false;
};

#endif  // SRC_GRAPHICS_LIB_MAGMA_SRC_SYS_DRIVER_MAGMA_SYSTEM_CONNECTION_H_
