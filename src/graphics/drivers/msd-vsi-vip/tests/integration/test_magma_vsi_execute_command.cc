// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/magma/magma.h>
#include <stdio.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <vector>

#include <gtest/gtest.h>
#include <helper/magma_map_cpu.h>

#include "test_magma_vsi.h"

extern "C" {
#include "cmdstream_fuchsia.h"
}

namespace {

constexpr uint64_t kMapFlags = MAGMA_MAP_FLAG_READ | MAGMA_MAP_FLAG_WRITE | MAGMA_MAP_FLAG_EXECUTE;

// Provided by etnaviv_cl_test_gc7000.c
extern "C" uint32_t hello_code[];
extern "C" void gen_cmd_stream(struct etna_cmd_stream* stream, struct etna_bo* code,
                               struct etna_bo* bmp);

class MagmaExecuteMsdVsi : public testing::Test {
 protected:
  void SetUp() override {
    magma_vsi_.DeviceFind();
    magma_vsi_.ConnectionCreate();
    magma_vsi_.ContextCreate();
  }

  void TearDown() override {
    for (auto& buffer : buffers_) {
      buffer->Release(magma_vsi_.GetConnection());
    }
    buffers_.clear();

    magma_vsi_.ContextRelease();
    magma_vsi_.ConnectionRelease();
    magma_vsi_.DeviceClose();
  }

 public:
  class EtnaBuffer : public etna_bo {
    friend MagmaExecuteMsdVsi;

   public:
    ~EtnaBuffer() { magma::UnmapCpuHelper(cpu_address_, size_); }

    uint32_t* GetCpuAddress() const { return reinterpret_cast<uint32_t*>(cpu_address_); }

    void Release(magma_connection_t connection) {
      magma_connection_release_buffer(connection, magma_buffer_);
      magma_buffer_ = 0;
    }

   private:
    magma_buffer_t magma_buffer_;
    uint64_t size_;
    uint32_t gpu_address_;
    magma_exec_resource resource_;
    void* cpu_address_ = nullptr;
  };

  std::shared_ptr<EtnaBuffer> CreateEtnaBuffer(uint32_t size) {
    auto etna_buffer = std::make_shared<EtnaBuffer>();
    uint64_t actual_size = 0;
    magma_buffer_id_t buffer_id;

    if (MAGMA_STATUS_OK !=
        magma_connection_create_buffer(magma_vsi_.GetConnection(), size, &actual_size,
                                       &(etna_buffer->magma_buffer_), &buffer_id))
      return nullptr;

    EXPECT_EQ(actual_size, size);
    EXPECT_NE(etna_buffer->magma_buffer_, 0ul);

    EXPECT_EQ(MAGMA_STATUS_OK, magma_buffer_set_cache_policy(etna_buffer->magma_buffer_,
                                                             MAGMA_CACHE_POLICY_WRITE_COMBINING));

    if (!magma::MapCpuHelper(etna_buffer->magma_buffer_, 0 /*offset*/, actual_size,
                             &etna_buffer->cpu_address_))
      return nullptr;

    etna_buffer->size_ = actual_size;

    etna_buffer->gpu_address_ = next_gpu_addr_;
    next_gpu_addr_ += etna_buffer->size_;

    magma_status_t status = magma_connection_map_buffer(
        magma_vsi_.GetConnection(), etna_buffer->gpu_address_, etna_buffer->magma_buffer_,
        0,  // page offset
        etna_buffer->size_, kMapFlags);
    if (status != MAGMA_STATUS_OK)
      return nullptr;

    etna_buffer->resource_.buffer_id = buffer_id;
    etna_buffer->resource_.offset = 0;
    etna_buffer->resource_.length = etna_buffer->size_;

    buffers_.push_back(etna_buffer);

    return etna_buffer;
  }

  class EtnaCommandStream : public etna_cmd_stream {
    friend MagmaExecuteMsdVsi;

   public:
    void EtnaSetState(uint32_t address, uint32_t value) {
      WriteCommand((1 << 27)           // load state
                   | (1 << 16)         // count
                   | (address >> 2));  // register to be written
      WriteCommand(value);
    }

    void EtnaSetStateFromBuffer(uint32_t address, const EtnaBuffer& buffer, uint32_t reloc_flags) {
      WriteCommand((1 << 27)           // load state
                   | (1 << 16)         // count
                   | (address >> 2));  // register to be written
      WriteCommand(buffer.gpu_address_);
    }

    void EtnaStall(uint32_t from, uint32_t to) {
      EtnaSetState(0x00003808, (from & 0x1f) | ((to << 8) & 0x1f00));

      ASSERT_EQ(from, 1u);

      WriteCommand(0x48000000);
      WriteCommand((from & 0x1f) | ((to << 8) & 0x1f00));
    }

    void EtnaLink(uint16_t prefetch, uint32_t gpu_address) {
      constexpr uint32_t kLinkCommand = 0x40000000;
      WriteCommand(kLinkCommand | prefetch);
      WriteCommand(gpu_address);
    }

   protected:
    std::shared_ptr<EtnaBuffer> etna_buffer = nullptr;
    uint32_t index = 0;

    void WriteCommand(uint32_t command) {
      ASSERT_NE(etna_buffer, nullptr);
      ASSERT_LT(index, etna_buffer->size_);

      etna_buffer->GetCpuAddress()[index++] = command;
      etna_buffer->resource_.length = index * sizeof(uint32_t);
    }
  };

  std::unique_ptr<EtnaCommandStream> CreateEtnaCommandStream(uint32_t size) {
    auto command_stream = std::make_unique<EtnaCommandStream>();

    command_stream->etna_buffer = CreateEtnaBuffer(size);
    if (!command_stream->etna_buffer)
      return nullptr;

    command_stream->etna_buffer->resource_.length = 0;

    return command_stream;
  }

  void ExecuteCommand(std::shared_ptr<EtnaCommandStream> command_stream, uint32_t timeout_ms) {
    uint32_t length = command_stream->index * sizeof(uint32_t);
    magma_semaphore_t semaphore;
    uint64_t semaphore_id;

    ASSERT_NE(length, 0u);
    ASSERT_EQ(
        magma_connection_create_semaphore(magma_vsi_.GetConnection(), &semaphore, &semaphore_id),
        MAGMA_STATUS_OK);

    std::vector<magma_exec_resource> resources;
    resources.push_back(command_stream->etna_buffer->resource_);
    EXPECT_NE(resources[0].length, 0ul);

    magma_exec_command_buffer command_buffer = {.resource_index = 0, .start_offset = 0};

    magma_command_descriptor descriptor = {
        .resource_count = static_cast<uint32_t>(resources.size()),
        .command_buffer_count = 1,
        .wait_semaphore_count = 0,
        .signal_semaphore_count = 1,
        .resources = resources.data(),
        .command_buffers = &command_buffer,
        .semaphore_ids = &semaphore_id,
        .flags = 0};

    auto start = std::chrono::high_resolution_clock::now();

    EXPECT_EQ(MAGMA_STATUS_OK,
              magma_connection_execute_command(magma_vsi_.GetConnection(),
                                               magma_vsi_.GetContextId(), &descriptor));
    magma_poll_item_t item = {.semaphore = semaphore,
                              .type = MAGMA_POLL_TYPE_SEMAPHORE,
                              .condition = MAGMA_POLL_CONDITION_SIGNALED};
    auto status = magma_poll(&item, 1, 1000000ul * timeout_ms);
    auto t = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::high_resolution_clock::now() - start)
                 .count();
    ASSERT_EQ(status, MAGMA_STATUS_OK);
    EXPECT_LT(t, timeout_ms);

    magma_connection_release_semaphore(magma_vsi_.GetConnection(), semaphore);
  }

  void Test() {
    static constexpr size_t kCodeSize = 4096;

    std::shared_ptr<EtnaCommandStream> command_stream = CreateEtnaCommandStream(kCodeSize);
    ASSERT_TRUE(command_stream);

    std::shared_ptr<EtnaBuffer> code = CreateEtnaBuffer(kCodeSize);
    ASSERT_TRUE(code);

    bool found_end_of_code = false;
    for (uint32_t i = 0; i < kCodeSize / sizeof(uint32_t); i++) {
      if ((i % 4 == 0) && hello_code[i] == 0) {
        // End of code is a NOOP line
        found_end_of_code = true;
        break;
      }
      code->GetCpuAddress()[i] = hello_code[i];
    }
    EXPECT_TRUE(found_end_of_code);

    static constexpr size_t kBufferSize = 65536;
    std::shared_ptr<EtnaBuffer> output_buffer = CreateEtnaBuffer(kBufferSize);
    ASSERT_TRUE(output_buffer);

    // Memset doesn't like uncached buffers
    for (uint32_t i = 0; i < kBufferSize / sizeof(uint32_t); i++) {
      output_buffer->GetCpuAddress()[i] = 0;
    }

    gen_cmd_stream(command_stream.get(), code.get(), output_buffer.get());

    static constexpr uint32_t kTimeoutMs = 1000;
    ExecuteCommand(command_stream, kTimeoutMs);

    auto data = reinterpret_cast<const char*>(output_buffer->GetCpuAddress());
    ASSERT_TRUE(data);

    const char kHelloWorld[] = "Hello, World!";
    EXPECT_STREQ(data, kHelloWorld);
  }

  void TestExecuteMmuException() {
    static constexpr size_t kCodeSize = 4096;

    std::shared_ptr<EtnaCommandStream> command_stream = CreateEtnaCommandStream(kCodeSize);
    ASSERT_TRUE(command_stream);

    // Jump to an unmapped address.
    command_stream->EtnaLink(0x8 /* arbitrary prefetch */, next_gpu_addr_);

    static constexpr uint32_t kTimeoutMs = 10000;
    ExecuteCommand(command_stream, kTimeoutMs);

    EXPECT_EQ(MAGMA_STATUS_CONTEXT_KILLED, magma_connection_flush(magma_vsi_.GetConnection()));
  }

  void TestHang() {
    static constexpr size_t kCodeSize = 4096;

    std::shared_ptr<EtnaCommandStream> command_stream = CreateEtnaCommandStream(kCodeSize);
    ASSERT_TRUE(command_stream);

    // Infinite loop by jumping back to the link command.
    command_stream->EtnaLink(0x8 /* prefetch */, command_stream->etna_buffer->gpu_address_);

    static constexpr uint32_t kTimeoutMs = 7000;
    ExecuteCommand(command_stream, kTimeoutMs);

    EXPECT_EQ(MAGMA_STATUS_CONTEXT_KILLED, magma_connection_flush(magma_vsi_.GetConnection()));
  }

 private:
  MagmaVsi magma_vsi_;

  uint32_t next_gpu_addr_ = 0x10000;
  // Since the etnaviv test does not release buffers, we track them here to avoid leaks.
  std::vector<std::shared_ptr<EtnaBuffer>> buffers_;
};

}  // namespace

// Called from etnaviv_cl_test_gc7000.c
void etna_set_state(struct etna_cmd_stream* stream, uint32_t address, uint32_t value) {
  static_cast<MagmaExecuteMsdVsi::EtnaCommandStream*>(stream)->EtnaSetState(address, value);
}

void etna_set_state_from_bo(struct etna_cmd_stream* stream, uint32_t address, struct etna_bo* bo,
                            uint32_t reloc_flags) {
  static_cast<MagmaExecuteMsdVsi::EtnaCommandStream*>(stream)->EtnaSetStateFromBuffer(
      address, *static_cast<MagmaExecuteMsdVsi::EtnaBuffer*>(bo), reloc_flags);
}

void etna_stall(struct etna_cmd_stream* stream, uint32_t from, uint32_t to) {
  static_cast<MagmaExecuteMsdVsi::EtnaCommandStream*>(stream)->EtnaStall(from, to);
}

struct etna_bo* etna_bo_new(void* dev, uint32_t size, uint32_t flags) { return nullptr; }
void* etna_bo_map(struct etna_bo* bo) { return nullptr; }
void etna_cmd_stream_finish(struct etna_cmd_stream* stream) {}
struct drm_test_info* drm_test_setup(int argc, char** argv) { return nullptr; }
void drm_test_teardown(struct drm_test_info* info) {}

TEST_F(MagmaExecuteMsdVsi, ExecuteCommand) { Test(); }

TEST_F(MagmaExecuteMsdVsi, ExecuteMany) {
  for (uint32_t iter = 0; iter < 100; iter++) {
    Test();
    TearDown();
    SetUp();
  }
}

TEST_F(MagmaExecuteMsdVsi, MmuExceptionRecovery) {
  TestExecuteMmuException();
  TearDown();
  // Verify new commands complete successfully.
  SetUp();
  Test();
}

TEST_F(MagmaExecuteMsdVsi, HangRecovery) {
  TestHang();
  TearDown();
  // Verify new commands complete successfully.
  SetUp();
  Test();
}
