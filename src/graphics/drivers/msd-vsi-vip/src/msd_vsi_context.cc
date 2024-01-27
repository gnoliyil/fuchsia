// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "msd_vsi_context.h"

#include "address_space_layout.h"
#include "command_buffer.h"
#include "msd_vsi_semaphore.h"

// static
std::shared_ptr<MsdVsiContext> MsdVsiContext::Create(std::weak_ptr<MsdVsiConnection> connection,
                                                     std::shared_ptr<AddressSpace> address_space,
                                                     Ringbuffer* ringbuffer) {
  auto context = std::make_shared<MsdVsiContext>(connection, address_space);
  if (!context->MapRingbuffer(ringbuffer)) {
    MAGMA_LOG(ERROR, "failed to map ringbuffer into new context");
    return nullptr;
  }
  return context;
}

std::unique_ptr<MappedBatch> MsdVsiContext::CreateBatch(std::shared_ptr<MsdVsiContext> context,
                                                        magma_command_buffer* cmd_buf,
                                                        magma_exec_resource* exec_resources,
                                                        msd_buffer_t** msd_buffers,
                                                        msd_semaphore_t** msd_wait_semaphores,
                                                        msd_semaphore_t** msd_signal_semaphores) {
  std::vector<CommandBuffer::ExecResource> resources;
  resources.reserve(cmd_buf->resource_count);
  for (uint32_t i = 0; i < cmd_buf->resource_count; i++) {
    resources.emplace_back(CommandBuffer::ExecResource{MsdVsiAbiBuffer::cast(msd_buffers[i])->ptr(),
                                                       exec_resources[i].offset,
                                                       exec_resources[i].length});
  }
  // Currently wait semaphores are not used.
  std::vector<std::shared_ptr<magma::PlatformSemaphore>> wait_semaphores;
  std::vector<std::shared_ptr<magma::PlatformSemaphore>> signal_semaphores;
  signal_semaphores.reserve(cmd_buf->signal_semaphore_count);
  for (uint32_t i = 0; i < cmd_buf->signal_semaphore_count; i++) {
    signal_semaphores.emplace_back(MsdVsiAbiSemaphore::cast(msd_signal_semaphores[i])->ptr());
  }

  auto connection = context->connection().lock();
  if (!connection) {
    MAGMA_LOG(ERROR, "Connection is already dead");
    return nullptr;
  }

  std::unique_ptr<MappedBatch> batch;

  // The CommandBuffer does not support batches with zero resources.
  if (resources.size() > 0) {
    auto command_buffer = CommandBuffer::Create(context, connection->client_id(),
                                                std::make_unique<magma_command_buffer>(*cmd_buf),
                                                std::move(resources), std::move(signal_semaphores));
    if (!command_buffer) {
      MAGMA_LOG(ERROR, "Failed to create command buffer");
      return nullptr;
    }
    batch = std::move(command_buffer);
  } else {
    batch = std::make_unique<EventBatch>(context, std::move(wait_semaphores),
                                         std::move(signal_semaphores));
  }

  return batch;
}

magma::Status MsdVsiContext::SubmitBatch(std::unique_ptr<MappedBatch> batch) {
  auto connection = connection_.lock();
  if (!connection) {
    DMESSAGE("Can't submit without connection");
    return MAGMA_STATUS_OK;
  }

  std::shared_ptr<MsdVsiContext> context = batch->GetContext().lock();
  DASSERT(context.get() == static_cast<MsdVsiContext*>(this));

  // If there are any mappings pending release, submit them now.
  connection->SubmitPendingReleaseMappings(context);

  // TODO(fxbug.dev/42748): handle wait semaphores.
  return connection->SubmitBatch(std::move(batch));
}

bool MsdVsiContext::MapRingbuffer(Ringbuffer* ringbuffer) {
  uint64_t gpu_addr;
  if (exec_address_space()->GetRingbufferGpuAddress(&gpu_addr)) {
    // Already mapped.
    return true;
  }

  gpu_addr = AddressSpaceLayout::system_gpu_addr_base();
  std::shared_ptr<GpuMapping> gpu_mapping;
  // TODO(fxbug.dev/50307): ringbuffer should be mapped read-only
  bool res = ringbuffer->MultiMap(exec_address_space(), gpu_addr, &gpu_mapping);
  if (res) {
    exec_address_space()->SetRingbufferGpuMapping(gpu_mapping);
  }
  return res;
}

void MsdVsiContext::Kill() {
  if (killed_) {
    return;
  }
  killed_ = true;
  auto connection = connection_.lock();
  if (connection) {
    connection->SendContextKilled();
  }
}

void msd_context_destroy(msd_context_t* abi_context) { delete MsdVsiAbiContext::cast(abi_context); }

magma_status_t msd_context_execute_immediate_commands(msd_context_t* ctx, uint64_t commands_size,
                                                      void* commands, uint64_t semaphore_count,
                                                      msd_semaphore_t** msd_semaphores) {
  return MAGMA_STATUS_UNIMPLEMENTED;
}

magma_status_t msd_context_execute_command_buffer_with_resources(
    struct msd_context_t* ctx, struct magma_command_buffer* cmd_buf,
    struct magma_exec_resource* exec_resources, struct msd_buffer_t** buffers,
    struct msd_semaphore_t** wait_semaphores, struct msd_semaphore_t** signal_semaphores) {
  auto context = MsdVsiAbiContext::cast(ctx)->ptr();

  if (cmd_buf->flags) {
    MAGMA_LOG(ERROR, "Flags not supported");
    return MAGMA_STATUS_INVALID_ARGS;
  }

  std::unique_ptr<MappedBatch> batch = MsdVsiContext::CreateBatch(
      context, cmd_buf, exec_resources, buffers, wait_semaphores, signal_semaphores);
  if (batch->IsCommandBuffer()) {
    auto* command_buffer = static_cast<CommandBuffer*>(batch.get());
    if (!command_buffer->PrepareForExecution()) {
      MAGMA_LOG(ERROR, "Failed to prepare command buffer for execution");
      return MAGMA_STATUS_INTERNAL_ERROR;
    }
    if (!command_buffer->IsValidBatch()) {
      MAGMA_LOG(ERROR, "Command buffer is not a valid batch");
      return MAGMA_STATUS_INTERNAL_ERROR;
    }
  }
  magma::Status status = context->SubmitBatch(std::move(batch));
  return status.get();
}
