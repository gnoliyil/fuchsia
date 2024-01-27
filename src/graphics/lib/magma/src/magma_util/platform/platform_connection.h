// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_LIB_MAGMA_SRC_MAGMA_UTIL_PLATFORM_PLATFORM_CONNECTION_H_
#define SRC_GRAPHICS_LIB_MAGMA_SRC_MAGMA_UTIL_PLATFORM_PLATFORM_CONNECTION_H_

#include <atomic>
#include <memory>
#include <vector>

#include "magma/magma.h"
#include "magma_util/macros.h"
#include "magma_util/status.h"
#include "msd_defs.h"
#include "platform_buffer.h"
#include "platform_event.h"
#include "platform_handle.h"
#include "platform_object.h"
#include "platform_semaphore.h"
#include "platform_thread.h"

namespace magma {

class PlatformPerfCountPool {
 public:
  virtual ~PlatformPerfCountPool() = default;
  virtual uint64_t pool_id() = 0;
  // Sends a OnPerformanceCounterReadCompleted. May be called from any thread.
  virtual magma::Status SendPerformanceCounterCompletion(uint32_t trigger_id, uint64_t buffer_id,
                                                         uint32_t buffer_offset, uint64_t time,
                                                         uint32_t result_flags) = 0;
};

class PlatformConnection {
 public:
  // Allows for the temporary limit of 3500 messages in the channel,
  // and also constrains the maximum amount of IPC memory.
  static constexpr uint32_t kMaxInflightMessages = 1000;
  static constexpr uint32_t kMaxInflightMemoryMB = 100;
  static constexpr uint32_t kMaxInflightBytes = kMaxInflightMemoryMB * 1024 * 1024;

  class Delegate {
   public:
    virtual ~Delegate() {}
    virtual magma::Status ImportObject(uint32_t handle, PlatformObject::Type object_type,
                                       uint64_t client_id) = 0;
    virtual magma::Status ReleaseObject(uint64_t object_id, PlatformObject::Type object_type) = 0;

    virtual magma::Status CreateContext(uint32_t context_id) = 0;
    virtual magma::Status DestroyContext(uint32_t context_id) = 0;

    virtual magma::Status ExecuteCommandBufferWithResources(
        uint32_t context_id, std::unique_ptr<magma_command_buffer> command_buffer,
        std::vector<magma_exec_resource> resources, std::vector<uint64_t> semaphores) = 0;
    virtual magma::Status MapBuffer(uint64_t buffer_id, uint64_t gpu_va, uint64_t offset,
                                    uint64_t length, uint64_t flags) = 0;
    virtual magma::Status UnmapBuffer(uint64_t buffer_id, uint64_t gpu_va) = 0;
    virtual magma::Status BufferRangeOp(uint64_t buffer_id, uint32_t op, uint64_t start,
                                        uint64_t length) = 0;

    virtual void SetNotificationCallback(msd_connection_notification_callback_t callback,
                                         void* token) = 0;
    virtual magma::Status ExecuteImmediateCommands(uint32_t context_id, uint64_t commands_size,
                                                   void* commands, uint64_t semaphore_count,
                                                   uint64_t* semaphore_ids) = 0;
    virtual magma::Status EnablePerformanceCounterAccess(
        std::unique_ptr<magma::PlatformHandle> access_token) = 0;
    virtual bool IsPerformanceCounterAccessAllowed() = 0;
    virtual magma::Status EnablePerformanceCounters(const uint64_t* counters,
                                                    uint64_t counter_count) = 0;
    virtual magma::Status CreatePerformanceCounterBufferPool(
        std::unique_ptr<PlatformPerfCountPool> pool) = 0;
    virtual magma::Status ReleasePerformanceCounterBufferPool(uint64_t pool_id) = 0;
    virtual magma::Status AddPerformanceCounterBufferOffsetToPool(uint64_t pool_id,
                                                                  uint64_t buffer_id,
                                                                  uint64_t buffer_offset,
                                                                  uint64_t buffer_size) = 0;
    virtual magma::Status RemovePerformanceCounterBufferFromPool(uint64_t pool_id,
                                                                 uint64_t buffer_id) = 0;
    virtual magma::Status DumpPerformanceCounters(uint64_t pool_id, uint32_t trigger_id) = 0;
    virtual magma::Status ClearPerformanceCounters(const uint64_t* counters,
                                                   uint64_t counter_count) = 0;
  };

  PlatformConnection(std::shared_ptr<magma::PlatformEvent> shutdown_event,
                     msd_client_id_t client_id)
      : client_id_(client_id), shutdown_event_(std::move(shutdown_event)) {}

  virtual ~PlatformConnection() {}

  // Creates a PlatformConnection.
  static std::shared_ptr<PlatformConnection> Create(
      std::unique_ptr<Delegate> Delegate, msd_client_id_t client_id,
      std::unique_ptr<magma::PlatformHandle> server_endpoint,
      std::unique_ptr<magma::PlatformHandle> server_notification_endpoint);

  // handles a single request, returns false if anything has put it into an illegal state
  // or if the remote has closed
  virtual bool HandleRequest() = 0;

  // Returns: messages consumed, bytes imported
  virtual std::pair<uint64_t, uint64_t> GetFlowControlCounts() = 0;

  std::shared_ptr<magma::PlatformEvent> ShutdownEvent() { return shutdown_event_; }

  static void RunLoop(std::shared_ptr<magma::PlatformConnection> connection, void* device_handle) {
    magma::PlatformThreadHelper::SetCurrentThreadName("ConnectionThread " +
                                                      std::to_string(connection->client_id_));

    // Apply the thread role before entering the handler loop.
    magma::PlatformThreadHelper::SetRole(device_handle, "fuchsia.graphics.magma.connection");

    while (connection->HandleRequest()) {
      connection->request_count_ += 1;
    }
    // the runloop terminates when the remote closes, or an error is experienced
    // so this is the appropriate time to let the connection go out of scope and be destroyed
  }

  uint64_t get_request_count() { return request_count_; }

 private:
  msd_client_id_t client_id_;
  std::shared_ptr<magma::PlatformEvent> shutdown_event_;
  std::atomic_uint request_count_{};
};

}  // namespace magma

#endif  // SRC_GRAPHICS_LIB_MAGMA_SRC_MAGMA_UTIL_PLATFORM_PLATFORM_CONNECTION_H_
