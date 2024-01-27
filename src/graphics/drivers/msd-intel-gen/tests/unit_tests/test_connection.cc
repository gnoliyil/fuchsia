// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <type_traits>

#include <gtest/gtest.h>

#include "mock/mock_bus_mapper.h"
#include "msd_intel_connection.h"
#include "msd_intel_context.h"

namespace {
struct HandleWaitNotification {
  msd_connection_handle_wait_start_t starter;
  msd_connection_handle_wait_complete_t completer;
  void* wait_context;
  magma_handle_t handle;
};
}  // namespace

class TestMsdIntelConnection : public ::testing::Test,
                               public MsdIntelConnection::Owner,
                               public msd::NotificationHandler {
 public:
  ~TestMsdIntelConnection() {
    // Call back completions just to prevent memory leaks
    for (auto& n : notifications_) {
      static int cancel_token;
      n.starter(n.wait_context, &cancel_token);
      n.completer(n.wait_context, MAGMA_STATUS_OK, n.handle);
    }
  }

  void SubmitBatch(std::unique_ptr<MappedBatch> batch) override {
    if (submit_batch_handler_)
      submit_batch_handler_(std::move(batch));
  }

  void DestroyContext(std::shared_ptr<MsdIntelContext> client_context) override {}

  magma::PlatformBusMapper* GetBusMapper() override { return &mock_bus_mapper_; }

  // msd::NotificationHandler impl
  void NotificationChannelSend(cpp20::span<uint8_t> data) override {
    channel_send_count_ += 1;
    callback_count_ += 1;

    // Checks below are specific to BufferCompletions test
    if (test_buffer_ids_.empty())
      return;

    switch (channel_send_count_) {
      case 1:
        EXPECT_EQ(MsdIntelConnection::kMaxUint64PerChannelSend, data.size() / sizeof(uint64_t));

        for (uint32_t i = 0; i < data.size() / sizeof(uint64_t); i++) {
          EXPECT_EQ(test_buffer_ids_[i], reinterpret_cast<uint64_t*>(&data[0])[i]);
        }
        break;

      case 2:
        EXPECT_EQ(test_buffer_ids_.size() - MsdIntelConnection::kMaxUint64PerChannelSend,
                  data.size() / sizeof(uint64_t));

        for (uint32_t i = 0; i < data.size() / sizeof(uint64_t); i++) {
          EXPECT_EQ(test_buffer_ids_[MsdIntelConnection::kMaxUint64PerChannelSend + i],
                    reinterpret_cast<uint64_t*>(&data[0])[i]);
        }
        break;

      default:
        EXPECT_TRUE(false);
    }
  }

  void ContextKilled() override {
    context_killed_count_ += 1;
    callback_count_ += 1;
  }

  void PerformanceCounterReadCompleted(const msd::PerfCounterResult& result) override {
    callback_count_ += 1;
  }

  void HandleWait(msd_connection_handle_wait_start_t starter,
                  msd_connection_handle_wait_complete_t completer, void* wait_context,
                  zx::unowned_handle handle) override {
    handle_wait_count_ += 1;
    callback_count_ += 1;

    HandleWaitNotification notification = {
        .starter = starter,
        .completer = completer,
        .wait_context = wait_context,
        .handle = handle->get(),
    };
    notifications_.push_back(notification);
  }

  void HandleWaitCancel(void* cancel_token) override { callback_count_ += 1; }

  async_dispatcher_t* GetAsyncDispatcher() override { return nullptr; }

  void BufferCompletions() {
    auto connection = MsdIntelConnection::Create(this, 0);
    ASSERT_TRUE(connection);

    connection->SetNotificationCallback(this);

    // +2 so we force multiple notification callbacks
    for (uint32_t i = 0; i < MsdIntelConnection::kMaxUint64PerChannelSend + 2; i++) {
      test_buffer_ids_.push_back(i);
    }

    connection->SendNotification(test_buffer_ids_);
  }

  void ReleaseBuffer() {
    auto connection = MsdIntelConnection::Create(this, 0);
    ASSERT_TRUE(connection);

    connection->SetNotificationCallback(this);

    std::shared_ptr<MsdIntelBuffer> buffer = MsdIntelBuffer::Create(PAGE_SIZE, "test");
    std::shared_ptr<GpuMapping> mapping;
    EXPECT_TRUE(
        AddressSpace::MapBufferGpu(connection->per_process_gtt(), buffer, 0x10000, 0, 1, &mapping));
    ASSERT_TRUE(mapping);
    EXPECT_TRUE(connection->per_process_gtt()->AddMapping(mapping));

    size_t batch_count = 0;
    submit_batch_handler_ = [&](std::unique_ptr<MappedBatch> batch) {
      batch_count += 1;
      EXPECT_EQ(
          1u, reinterpret_cast<MappingReleaseBatch*>(batch.get())->wrapper()->bus_mappings.size());
    };

    mapping.reset();
    connection->ReleaseBuffer(buffer->platform_buffer());

    EXPECT_EQ(0u, callback_count_);

    EXPECT_EQ(2u, batch_count);
  }

  void ReleaseBufferWhileMapped() {
    auto connection = std::shared_ptr<MsdIntelConnection>(MsdIntelConnection::Create(this, 0));
    ASSERT_TRUE(connection);

    // At least one context needed for callback to be invoked
    auto context = MsdIntelConnection::CreateContext(connection);

    context->SetTargetCommandStreamer(RENDER_COMMAND_STREAMER);

    size_t expected_flush_batches = context->GetTargetCommandStreamers().size();
    EXPECT_GE(expected_flush_batches, 1u);

    connection->SetNotificationCallback(this);

    std::shared_ptr<MsdIntelBuffer> buffer = MsdIntelBuffer::Create(PAGE_SIZE, "test");
    std::shared_ptr<GpuMapping> mapping;

    constexpr uint64_t kGpuAddr = 0x10000;
    EXPECT_TRUE(AddressSpace::MapBufferGpu(connection->per_process_gtt(), buffer, kGpuAddr, 0, 1,
                                           &mapping));
    ASSERT_TRUE(mapping);
    EXPECT_TRUE(connection->per_process_gtt()->AddMapping(mapping));

    uint32_t wait_callback_count = 0;
    auto wait_callback = [&](std::vector<std::shared_ptr<magma::PlatformSemaphore>>& semaphores,
                             uint32_t timeout_ms) {
      EXPECT_EQ(semaphores.size(), 1u);

      wait_callback_count += 1;

      if (wait_callback_count == 1) {
        // First time through, say the event hasn't fired to check the wait callback
        // gets called again.
        return magma::Status(MAGMA_STATUS_TIMED_OUT);
      }

      EXPECT_EQ(2u, wait_callback_count);

      // The pipeline flush batch is submitted, then destroyed by the submit handler
      // in the test harness.
      EXPECT_EQ(MAGMA_STATUS_OK, semaphores[0]->WaitNoReset(timeout_ms).get());
      mapping.reset();
      return magma::Status(MAGMA_STATUS_OK);
    };

    size_t batch_count = 0;
    submit_batch_handler_ = [&](std::unique_ptr<MappedBatch> batch) {
      batch_count += 1;
      if (batch_count > expected_flush_batches)
        EXPECT_EQ(
            1u,
            reinterpret_cast<MappingReleaseBatch*>(batch.get())->wrapper()->bus_mappings.size());
    };

    connection->ReleaseBuffer(buffer->platform_buffer(), wait_callback);

    EXPECT_EQ(0u, callback_count_);

    EXPECT_FALSE(connection->sent_context_killed());

    EXPECT_EQ(expected_flush_batches + 2, batch_count);

    connection->DestroyContext(context);
  }

  void ReleaseBufferWhileMappedMultiContext() {
    auto connection = std::shared_ptr<MsdIntelConnection>(MsdIntelConnection::Create(this, 0));
    ASSERT_TRUE(connection);

    connection->SetNotificationCallback(this);

    std::vector<std::shared_ptr<MsdIntelContext>> contexts;
    contexts.push_back(MsdIntelConnection::CreateContext(connection));
    contexts.push_back(MsdIntelConnection::CreateContext(connection));

    size_t expected_flush_batches = 0;
    for (auto& context : contexts) {
      context->SetTargetCommandStreamer(RENDER_COMMAND_STREAMER);
      expected_flush_batches += context->GetTargetCommandStreamers().size();
    }

    std::shared_ptr<MsdIntelBuffer> buffer = MsdIntelBuffer::Create(PAGE_SIZE, "test");
    std::shared_ptr<GpuMapping> mapping;
    EXPECT_TRUE(
        AddressSpace::MapBufferGpu(connection->per_process_gtt(), buffer, 0x10000, 0, 1, &mapping));
    ASSERT_TRUE(mapping);
    EXPECT_TRUE(connection->per_process_gtt()->AddMapping(mapping));

    uint32_t wait_callback_count = 0;
    auto wait_callback = [&](std::vector<std::shared_ptr<magma::PlatformSemaphore>>& semaphores,
                             uint32_t timeout_ms) {
      EXPECT_EQ(semaphores.size(), 1u);
      EXPECT_EQ(MAGMA_STATUS_OK, semaphores[0]->WaitNoReset(timeout_ms).get());

      wait_callback_count += 1;
      if (wait_callback_count == contexts.size()) {
        mapping.reset();
      }
      return magma::Status(MAGMA_STATUS_OK);
    };

    size_t batch_count = 0;
    submit_batch_handler_ = [&](std::unique_ptr<MappedBatch> batch) {
      batch_count += 1;
      if (batch_count > expected_flush_batches)
        EXPECT_EQ(
            1u, reinterpret_cast<MappingReleaseBatch*>(batch.get())->wrapper()->bus_mappings.size())
            << "batch_count: " << batch_count;
    };

    connection->ReleaseBuffer(buffer->platform_buffer(), wait_callback);

    EXPECT_EQ(wait_callback_count, contexts.size());

    EXPECT_EQ(0u, callback_count_);

    EXPECT_FALSE(connection->sent_context_killed());

    EXPECT_EQ(expected_flush_batches + 2, batch_count);

    for (auto& context : contexts) {
      connection->DestroyContext(context);
    }
  }

  void ReleaseBufferCommandBuffer(bool command_buffer_stuck) {
    auto connection = std::shared_ptr<MsdIntelConnection>(MsdIntelConnection::Create(this, 0));
    ASSERT_TRUE(connection);

    connection->SetNotificationCallback(this);

    auto context = MsdIntelConnection::CreateContext(connection);
    context->SetTargetCommandStreamer(RENDER_COMMAND_STREAMER);

    std::shared_ptr<MsdIntelBuffer> buffer = MsdIntelBuffer::Create(PAGE_SIZE, "test");
    {
      std::shared_ptr<GpuMapping> mapping;
      EXPECT_TRUE(AddressSpace::MapBufferGpu(connection->per_process_gtt(), buffer, 0x10000, 0, 1,
                                             &mapping));
      ASSERT_TRUE(mapping);
      EXPECT_TRUE(connection->per_process_gtt()->AddMapping(std::move(mapping)));
    }

    // Send a command buffer
    auto command = std::make_unique<magma_command_buffer>();
    command->resource_count = 1;
    command->batch_buffer_resource_index = 0;
    command->batch_start_offset = 0;
    command->wait_semaphore_count = 1;
    command->signal_semaphore_count = 0;

    auto command_buffer = std::make_unique<CommandBuffer>(context, std::move(command));

    auto resource = CommandBuffer::ExecResource{.buffer = buffer};

    auto wait_semaphore =
        std::shared_ptr<magma::PlatformSemaphore>(magma::PlatformSemaphore::Create());

    ASSERT_TRUE(command_buffer->InitializeResources({resource}, {wait_semaphore}, {}));
    ASSERT_TRUE(command_buffer->PrepareForExecution());
    ASSERT_EQ(MAGMA_STATUS_OK, context->SubmitCommandBuffer(std::move(command_buffer)).get());

    uint32_t wait_callback_count = 0;
    auto wait_callback = [&](std::vector<std::shared_ptr<magma::PlatformSemaphore>>& semaphores,
                             uint32_t timeout_ms) {
      wait_callback_count += 1;

      if (command_buffer_stuck)
        return MAGMA_STATUS_TIMED_OUT;

      if (semaphores.size() > 1) {
        // By signalling the wait semaphore, the command buffer should be submitted, and the
        // pipeline fences after it.
        EXPECT_EQ(semaphores.size(), 2u);
        wait_semaphore->Signal();
        return MAGMA_STATUS_OK;
      }

      return MAGMA_STATUS_TIMED_OUT;
    };

    std::map<MappedBatch::BatchType, int> batch_count;
    submit_batch_handler_ = [&](std::unique_ptr<MappedBatch> batch) {
      batch_count[batch->GetType()] += 1;
    };

    connection->ReleaseBuffer(buffer->platform_buffer(), wait_callback);

    EXPECT_EQ(wait_callback_count, 1u);

    EXPECT_EQ(1u, handle_wait_count_);
    EXPECT_EQ(command_buffer_stuck ? 1u : 0u, context_killed_count_);
    EXPECT_EQ(command_buffer_stuck ? 0u : 1u, channel_send_count_);

    if (command_buffer_stuck) {
      EXPECT_TRUE(connection->sent_context_killed());
      EXPECT_EQ(0u, batch_count.size());
    } else {
      EXPECT_FALSE(connection->sent_context_killed());
      EXPECT_EQ(batch_count[MappedBatch::COMMAND_BUFFER], 1);
      EXPECT_EQ(batch_count[MappedBatch::PIPELINE_FENCE_BATCH], 1);
      EXPECT_EQ(batch_count[MappedBatch::MAPPING_RELEASE_BATCH], 2);
    }

    // Ensure wait semaphore is reset
    EXPECT_FALSE(wait_semaphore->WaitNoReset(0));

    connection->SetNotificationCallback(nullptr);

    connection->DestroyContext(context);
  }

  // This can happen when a connection is shutting down.
  void ReleaseBufferWhileMappedNoContext() {
    auto connection = std::shared_ptr<MsdIntelConnection>(MsdIntelConnection::Create(this, 0));
    ASSERT_TRUE(connection);

    std::shared_ptr<MsdIntelBuffer> buffer = MsdIntelBuffer::Create(PAGE_SIZE, "test");
    std::shared_ptr<GpuMapping> mapping;

    constexpr uint64_t kGpuAddr = 0x10000;
    EXPECT_TRUE(AddressSpace::MapBufferGpu(connection->per_process_gtt(), buffer, kGpuAddr, 0, 1,
                                           &mapping));
    ASSERT_TRUE(mapping);
    EXPECT_TRUE(connection->per_process_gtt()->AddMapping(mapping));

    auto wait_callback = [&](std::vector<std::shared_ptr<magma::PlatformSemaphore>>& semaphores,
                             uint32_t timeout_ms) {
      // Should never be called.
      EXPECT_TRUE(false);
      return MAGMA_STATUS_OK;
    };

    size_t batch_count = 0;
    submit_batch_handler_ = [&](std::unique_ptr<MappedBatch> batch) { batch_count += 1; };

    connection->ReleaseBuffer(buffer->platform_buffer(), wait_callback);

    EXPECT_FALSE(connection->sent_context_killed());

    EXPECT_EQ(0u, batch_count);
  }

  void ReuseGpuAddrWithoutRelease() {
    auto connection = MsdIntelConnection::Create(this, 0);
    ASSERT_TRUE(connection);

    constexpr uint64_t kBufferSizeInPages = 1;
    constexpr uint64_t kGpuAddr = 0x10000;

    for (uint32_t i = 0; i < 2; i++) {
      std::shared_ptr<MsdIntelBuffer> buffer =
          MsdIntelBuffer::Create(kBufferSizeInPages * magma::page_size(), "test");
      EXPECT_EQ(MAGMA_STATUS_OK,
                connection->MapBufferGpu(buffer, kGpuAddr, 0, kBufferSizeInPages).get());

      std::shared_ptr<GpuMapping> mapping = connection->per_process_gtt()->FindGpuMapping(kGpuAddr);
      ASSERT_TRUE(mapping);
      EXPECT_EQ(mapping->BufferId(), buffer->platform_buffer()->id());
    }
  }

 private:
  MockBusMapper mock_bus_mapper_;
  std::vector<uint64_t> test_buffer_ids_;
  uint64_t callback_count_ = 0;
  uint64_t channel_send_count_ = 0;
  uint64_t context_killed_count_ = 0;
  uint64_t handle_wait_count_ = 0;
  std::function<void(std::unique_ptr<MappedBatch> batch)> submit_batch_handler_;
  // Notifications to be cleaned up to avoid leaks
  std::vector<HandleWaitNotification> notifications_;
};

TEST_F(TestMsdIntelConnection, BufferCompletions) { BufferCompletions(); }

TEST_F(TestMsdIntelConnection, ReleaseBuffer) { ReleaseBuffer(); }

TEST_F(TestMsdIntelConnection, ReleaseBufferWhileMapped) { ReleaseBufferWhileMapped(); }

TEST_F(TestMsdIntelConnection, ReleaseBufferWhileMappedMultiContext) {
  ReleaseBufferWhileMappedMultiContext();
}

TEST_F(TestMsdIntelConnection, ReleaseBufferWhileMappedNoContext) {
  ReleaseBufferWhileMappedNoContext();
}

TEST_F(TestMsdIntelConnection, ReleaseBufferCommandBuffer) { ReleaseBufferCommandBuffer(false); }

TEST_F(TestMsdIntelConnection, ReleaseBufferCommandBufferStuck) {
  ReleaseBufferCommandBuffer(true);
}

TEST_F(TestMsdIntelConnection, ReuseGpuAddrWithoutRelease) { ReuseGpuAddrWithoutRelease(); }

TEST_F(TestMsdIntelConnection, InheritanceCheck) {
  EXPECT_FALSE(static_cast<bool>(std::is_base_of<PerProcessGtt::Owner, MsdIntelConnection>()));
}
