// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MSD_INTEL_CONTEXT_H
#define MSD_INTEL_CONTEXT_H

#include <lib/async/cpp/wait.h>

#include <map>
#include <memory>
#include <queue>
#include <set>

#include "command_buffer.h"
#include "magma_util/status.h"
#include "msd_cc.h"
#include "msd_intel_buffer.h"
#include "platform_logger.h"
#include "ppgtt.h"
#include "ringbuffer.h"
#include "types.h"

class MsdIntelConnection;

// Base context, not tied to a connection.
class MsdIntelContext {
 public:
  // Context for handling a wait semaphore.
  class HandleWaitContext {
   public:
    HandleWaitContext(MsdIntelContext* context, EngineCommandStreamerId id, zx::handle object,
                      std::shared_ptr<magma::PlatformSemaphore> semaphore);

    ~HandleWaitContext() { waiter_.Cancel(); }

    zx_status_t Begin(async_dispatcher_t* dispatcher) { return waiter_.Begin(dispatcher); }

    EngineCommandStreamerId id() const { return id_; }
    const std::shared_ptr<magma::PlatformSemaphore>& semaphore() const { return semaphore_; }

   private:
    void Handler(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                 const zx_packet_signal_t* signal);

    MsdIntelContext* context_;
    EngineCommandStreamerId id_;
    zx::handle object_;
    std::shared_ptr<magma::PlatformSemaphore> semaphore_;
    async::WaitMethod<HandleWaitContext, &HandleWaitContext::Handler> waiter_;
  };

  explicit MsdIntelContext(std::shared_ptr<AddressSpace> address_space)
      : address_space_(std::move(address_space)) {
    DASSERT(address_space_);
  }

  MsdIntelContext(std::shared_ptr<AddressSpace> address_space,
                  std::weak_ptr<MsdIntelConnection> connection)
      : address_space_(std::move(address_space)), connection_(std::move(connection)) {}

  void SetTargetCommandStreamer(EngineCommandStreamerId id) {
    target_command_streamers_.insert(id);
    presubmit_map_.insert({id, PerEnginePresubmit{}});
  }

  std::set<EngineCommandStreamerId> GetTargetCommandStreamers() {
    return target_command_streamers_;
  }

  void SetEngineState(EngineCommandStreamerId id, std::unique_ptr<MsdIntelBuffer> context_buffer,
                      std::unique_ptr<Ringbuffer> ringbuffer);

  bool Map(std::shared_ptr<AddressSpace> address_space, EngineCommandStreamerId id);
  bool Unmap(EngineCommandStreamerId id);

  // The HW context state refers to the indirect context batch, so keep a reference to the batch
  // and its underlying GPU mapping.
  void SetIndirectContextBatch(std::shared_ptr<IndirectContextBatch> batch) {
    indirect_context_batch_ = std::move(batch);
  }

  std::weak_ptr<MsdIntelConnection> connection() { return connection_; }

  bool killed() const { return killed_; }

  void Kill();

  size_t GetQueueSize(EngineCommandStreamerId id) {
    auto iter = presubmit_map_.find(id);
    DASSERT(iter != presubmit_map_.end());

    PerEnginePresubmit& presubmit = iter->second;

    return presubmit.queue.size();
  }

  // Gets the gpu address of the context buffer if mapped.
  bool GetGpuAddress(EngineCommandStreamerId id, gpu_addr_t* addr_out);
  bool GetRingbufferGpuAddress(EngineCommandStreamerId id, gpu_addr_t* addr_out);

  MsdIntelBuffer* get_context_buffer(EngineCommandStreamerId id) {
    auto iter = state_map_.find(id);
    return iter == state_map_.end() ? nullptr : iter->second.context_buffer.get();
  }

  void* GetCachedContextBufferCpuAddr(EngineCommandStreamerId id) {
    auto iter = state_map_.find(id);
    if (iter == state_map_.end())
      return nullptr;
    if (!iter->second.context_buffer_cpu_addr) {
      MsdIntelBuffer* context_buffer = iter->second.context_buffer.get();
      if (!context_buffer)
        return nullptr;
      if (!context_buffer->platform_buffer()->MapCpu(&iter->second.context_buffer_cpu_addr))
        return DRETP(nullptr, "Failed to map context buffer");
    }
    return iter->second.context_buffer_cpu_addr;
  }

  Ringbuffer* get_ringbuffer(EngineCommandStreamerId id) {
    auto iter = state_map_.find(id);
    return iter == state_map_.end() ? nullptr : iter->second.ringbuffer.get();
  }

  bool IsInitializedForEngine(EngineCommandStreamerId id) {
    return state_map_.find(id) != state_map_.end();
  }

  std::queue<std::unique_ptr<MappedBatch>>& pending_batch_queue(EngineCommandStreamerId id) {
    auto iter = state_map_.find(id);
    DASSERT(iter != state_map_.end());
    return iter->second.pending_batch_queue;
  }

  std::shared_ptr<AddressSpace> exec_address_space() { return address_space_; }

  magma::Status SubmitCommandBuffer(std::unique_ptr<CommandBuffer> cmd_buf);
  magma::Status SubmitBatch(std::unique_ptr<MappedBatch> batch);

  std::vector<std::shared_ptr<magma::PlatformSemaphore>> GetWaitSemaphores(
      EngineCommandStreamerId id) const;

  void UpdateWaitSet(EngineCommandStreamerId id);
  void Shutdown();

 private:
  void AddToWaitset(EngineCommandStreamerId id, std::shared_ptr<MsdIntelConnection> connection,
                    std::shared_ptr<magma::PlatformSemaphore> semaphore);
  void WaitComplete(HandleWaitContext* wait_context, zx_status_t status);
  magma::Status ProcessPresubmitQueue(EngineCommandStreamerId id);

  struct PerEngineState {
    std::shared_ptr<MsdIntelBuffer> context_buffer;
    std::unique_ptr<GpuMapping> context_mapping;
    std::unique_ptr<Ringbuffer> ringbuffer;
    std::queue<std::unique_ptr<MappedBatch>> pending_batch_queue;
    gpu_addr_t ringbuffer_gpu_addr = 0;
    void* context_buffer_cpu_addr = nullptr;
  };

  std::shared_ptr<IndirectContextBatch> indirect_context_batch_;
  std::set<EngineCommandStreamerId> target_command_streamers_;
  std::map<EngineCommandStreamerId, PerEngineState> state_map_;
  std::shared_ptr<AddressSpace> address_space_;

  std::weak_ptr<MsdIntelConnection> connection_;

  struct PerEnginePresubmit {
    // The wait set tracks pending semaphores for the head of the presubmit queue
    std::vector<std::unique_ptr<HandleWaitContext>> wait_set;
    std::queue<std::unique_ptr<MappedBatch>> queue;
  };

  std::map<EngineCommandStreamerId, PerEnginePresubmit> presubmit_map_;
  bool killed_ = false;

  friend class TestContext;
};

class MsdIntelAbiContext : public msd::Context {
 public:
  explicit MsdIntelAbiContext(std::shared_ptr<MsdIntelContext> ptr) : ptr_(std::move(ptr)) {}

  ~MsdIntelAbiContext() override;

  std::shared_ptr<MsdIntelContext> ptr() { return ptr_; }

  magma_status_t ExecuteCommandBufferWithResources(magma_command_buffer* command_buffer,
                                                   magma_exec_resource* exec_resources,
                                                   msd::Buffer** buffers,
                                                   msd::Semaphore** wait_semaphores,
                                                   msd::Semaphore** signal_semaphores) override;

 private:
  std::shared_ptr<MsdIntelContext> ptr_;
};

#endif  // MSD_INTEL_CONTEXT_H
