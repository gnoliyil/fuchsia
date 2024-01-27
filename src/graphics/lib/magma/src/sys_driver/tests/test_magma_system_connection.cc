// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/magma/magma.h>

#include <utility>

#include <gtest/gtest.h>

#include "magma_util/short_macros.h"
#include "mock/mock_msd_cc.h"
#include "sys_driver/magma_system_connection.h"
#include "sys_driver/magma_system_device.h"
#ifdef __Fuchsia__
#include <src/graphics/lib/magma/src/magma_util/platform/zircon/zircon_platform_buffer.h>
#endif

namespace msd {
namespace {

inline uint64_t page_size() { return sysconf(_SC_PAGESIZE); }

class MsdMockConnection_ContextManagement : public MsdMockConnection {
 public:
  MsdMockConnection_ContextManagement() {}

  std::unique_ptr<msd::Context> CreateContext() override {
    active_context_count_++;
    return MsdMockConnection::CreateContext();
  }

  void DestroyContext(MsdMockContext* ctx) override {
    active_context_count_--;
    MsdMockConnection::DestroyContext(ctx);
  }

  uint32_t NumActiveContexts() { return active_context_count_; }

 private:
  uint32_t active_context_count_ = 0;
};

class MockPerfCountPool : public msd::PerfCountPoolServer {
 public:
  MockPerfCountPool(uint64_t pool_id) : pool_id_(pool_id) {}

  ~MockPerfCountPool() override {}
  uint64_t pool_id() override { return pool_id_; }

  magma::Status SendPerformanceCounterCompletion(uint32_t trigger_id, uint64_t buffer_id,
                                                 uint32_t buffer_offset, uint64_t time,
                                                 uint32_t result_flags) override {
    return MAGMA_STATUS_OK;
  }

 private:
  uint64_t pool_id_;
};

TEST(MagmaSystemConnection, ContextManagement) {
  auto msd_connection = std::make_unique<MsdMockConnection_ContextManagement>();
  auto msd_connection_ptr = msd_connection.get();

  auto msd_drv = std::make_unique<MsdMockDriver>();
  auto dev = std::shared_ptr<MagmaSystemDevice>(
      MagmaSystemDevice::Create(msd_drv.get(), std::make_unique<MsdMockDevice>()));
  MagmaSystemConnection connection(dev, std::move(msd_connection));

  EXPECT_EQ(msd_connection_ptr->NumActiveContexts(), 0u);

  uint32_t context_id_0 = 0;
  uint32_t context_id_1 = 1;

  EXPECT_TRUE(connection.CreateContext(context_id_0));
  EXPECT_EQ(msd_connection_ptr->NumActiveContexts(), 1u);

  EXPECT_TRUE(connection.CreateContext(context_id_1));
  EXPECT_EQ(msd_connection_ptr->NumActiveContexts(), 2u);

  EXPECT_TRUE(connection.DestroyContext(context_id_0));
  EXPECT_EQ(msd_connection_ptr->NumActiveContexts(), 1u);
  EXPECT_FALSE(connection.DestroyContext(context_id_0));

  EXPECT_TRUE(connection.DestroyContext(context_id_1));
  EXPECT_EQ(msd_connection_ptr->NumActiveContexts(), 0u);
  EXPECT_FALSE(connection.DestroyContext(context_id_1));
}

TEST(MagmaSystemConnection, BufferManagement) {
  auto msd_drv = std::make_unique<MsdMockDriver>();
  auto msd_dev = std::make_unique<MsdMockDevice>();
  auto msd_connection = msd_dev->Open(0);
  auto dev = std::shared_ptr<MagmaSystemDevice>(
      MagmaSystemDevice::Create(msd_drv.get(), std::move(msd_dev)));
  ASSERT_NE(msd_connection, nullptr);
  MagmaSystemConnection connection(dev, std::move(msd_connection));

  uint64_t test_size = 4096;

  auto buf = magma::PlatformBuffer::Create(test_size, "test");

  // assert because if this fails the rest of this is gonna be bogus anyway
  ASSERT_NE(buf, nullptr);
  EXPECT_GE(buf->size(), test_size);

  zx::handle duplicate_handle1;
  ASSERT_TRUE(buf->duplicate_handle(&duplicate_handle1));

  uint64_t id = buf->id();
  EXPECT_TRUE(connection.ImportBuffer(std::move(duplicate_handle1), id));

  // should be able to get the buffer by handle
  auto get_buf = connection.LookupBuffer(id);
  EXPECT_NE(get_buf, nullptr);
  EXPECT_EQ(get_buf->id(), id);  // they are shared ptrs after all

  zx::handle duplicate_handle2;
  ASSERT_TRUE(buf->duplicate_handle(&duplicate_handle2));

  EXPECT_TRUE(connection.ImportBuffer(std::move(duplicate_handle2), id));

  // freeing the allocated buffer should cause refcount to drop to 1
  EXPECT_TRUE(connection.ReleaseBuffer(id));
  EXPECT_NE(connection.LookupBuffer(id), nullptr);

  // freeing the allocated buffer should work
  EXPECT_TRUE(connection.ReleaseBuffer(id));

  // should no longer be able to get it from the map
  EXPECT_EQ(connection.LookupBuffer(id), nullptr);

  // should not be able to double free it
  EXPECT_FALSE(connection.ReleaseBuffer(id));
}

TEST(MagmaSystemConnection, Semaphores) {
  auto msd_drv = std::make_unique<MsdMockDriver>();
  auto msd_dev = std::make_unique<MsdMockDevice>();
  auto msd_connection = msd_dev->Open(0);
  auto dev = std::shared_ptr<MagmaSystemDevice>(
      MagmaSystemDevice::Create(msd_drv.get(), std::move(msd_dev)));
  ASSERT_TRUE(msd_connection);
  MagmaSystemConnection connection(dev, std::move(msd_connection));

  auto semaphore = magma::PlatformSemaphore::Create();
  ASSERT_TRUE(semaphore);

  zx::handle duplicate_handle1;
  ASSERT_TRUE(semaphore->duplicate_handle(&duplicate_handle1));

  EXPECT_TRUE(connection.ImportObject(
      std::move(duplicate_handle1), fuchsia_gpu_magma::wire::ObjectType::kEvent, semaphore->id()));

  auto system_semaphore = connection.LookupSemaphore(semaphore->id());
  EXPECT_TRUE(system_semaphore);
  EXPECT_EQ(static_cast<MsdMockSemaphore*>(system_semaphore->msd_semaphore())->GetKoid(),
            semaphore->id());

  zx::handle duplicate_handle2;
  ASSERT_TRUE(semaphore->duplicate_handle(&duplicate_handle2));

  // Can't import the same id twice
  EXPECT_FALSE(connection.ImportObject(
      std::move(duplicate_handle2), fuchsia_gpu_magma::wire::ObjectType::kEvent, semaphore->id()));

  EXPECT_TRUE(
      connection.ReleaseObject(semaphore->id(), fuchsia_gpu_magma::wire::ObjectType::kEvent));

  // should no longer be able to get it from the map
  EXPECT_EQ(connection.LookupSemaphore(semaphore->id()), nullptr);

  // should not be able to double free it
  EXPECT_FALSE(
      connection.ReleaseObject(semaphore->id(), fuchsia_gpu_magma::wire::ObjectType::kEvent));
}

TEST(MagmaSystemConnection, BadSemaphoreImport) {
  auto msd_drv = std::make_unique<MsdMockDriver>();
  auto msd_dev = std::make_unique<MsdMockDevice>();
  auto msd_connection = msd_dev->Open(0);
  auto dev = std::shared_ptr<MagmaSystemDevice>(
      MagmaSystemDevice::Create(msd_drv.get(), std::move(msd_dev)));
  ASSERT_NE(msd_connection, nullptr);
  MagmaSystemConnection connection(dev, std::move(msd_connection));

  constexpr uint32_t kBogusHandle = 0xabcd1234;
  EXPECT_FALSE(connection.ImportObject(zx::handle(kBogusHandle),
                                       fuchsia_gpu_magma::wire::ObjectType::kEvent, 0));

  auto buffer = magma::PlatformBuffer::Create(4096, "test");
  ASSERT_TRUE(buffer);

  zx::handle buffer_handle;
  ASSERT_TRUE(buffer->duplicate_handle(&buffer_handle));

  EXPECT_FALSE(connection.ImportObject(std::move(buffer_handle),
                                       fuchsia_gpu_magma::wire::ObjectType::kEvent, buffer->id()));
}

TEST(MagmaSystemConnection, BufferSharing) {
  auto msd_drv = std::make_unique<MsdMockDriver>();
  auto msd_dev = std::make_unique<MsdMockDevice>();
  auto msd_connection_0 = msd_dev->Open(0);
  auto msd_connection_1 = msd_dev->Open(1);
  auto dev = std::shared_ptr<MagmaSystemDevice>(
      MagmaSystemDevice::Create(msd_drv.get(), std::move(msd_dev)));
  ASSERT_NE(msd_connection_0, nullptr);
  MagmaSystemConnection connection_0(dev, std::move(msd_connection_0));
  ASSERT_NE(msd_connection_1, nullptr);
  MagmaSystemConnection connection_1(dev, std::move(msd_connection_1));

  auto platform_buf = magma::PlatformBuffer::Create(4096, "test");

  uint64_t buf_id_0 = 1;

  {
    zx::handle duplicate_handle;
    ASSERT_TRUE(platform_buf->duplicate_handle(&duplicate_handle));
    EXPECT_TRUE(connection_0.ImportBuffer(std::move(duplicate_handle), buf_id_0));
  }

  uint64_t buf_id_1 = 2;

  {
    zx::handle duplicate_handle;
    ASSERT_TRUE(platform_buf->duplicate_handle(&duplicate_handle));
    EXPECT_TRUE(connection_1.ImportBuffer(std::move(duplicate_handle), buf_id_1));
  }

  auto buf_0 = connection_0.LookupBuffer(buf_id_0);
  ASSERT_TRUE(buf_0);
  EXPECT_EQ(buf_0->id(), buf_id_0);

  auto buf_1 = connection_1.LookupBuffer(buf_id_1);
  ASSERT_TRUE(buf_1);
  EXPECT_EQ(buf_1->id(), buf_id_1);

#ifdef __Fuchsia__
  EXPECT_EQ(static_cast<magma::ZirconPlatformBuffer*>(buf_0->platform_buffer())->koid(),
            platform_buf->id());
  EXPECT_EQ(static_cast<magma::ZirconPlatformBuffer*>(buf_1->platform_buffer())->koid(),
            platform_buf->id());
#endif
}

TEST(MagmaSystemConnection, BadBufferImport) {
  auto msd_drv = std::make_unique<MsdMockDriver>();
  auto msd_dev = std::make_unique<MsdMockDevice>();
  auto msd_connection = msd_dev->Open(0);
  auto dev = std::shared_ptr<MagmaSystemDevice>(
      MagmaSystemDevice::Create(msd_drv.get(), std::move(msd_dev)));
  ASSERT_NE(msd_connection, nullptr);
  MagmaSystemConnection connection(dev, std::move(msd_connection));

  constexpr uint32_t kBogusHandle = 0xabcd1234;
  uint64_t id = 1;
  EXPECT_FALSE(connection.ImportBuffer(zx::handle(kBogusHandle), id));

  auto semaphore = magma::PlatformSemaphore::Create();
  ASSERT_TRUE(semaphore);

  zx::handle semaphore_handle;
  ASSERT_TRUE(semaphore->duplicate_handle(&semaphore_handle));

  EXPECT_FALSE(connection.ImportBuffer(std::move(semaphore_handle), id));

  zx::vmo vmo;
  ASSERT_EQ(ZX_OK, zx::vmo::create(4096, ZX_VMO_RESIZABLE, &vmo));
  EXPECT_FALSE(connection.ImportBuffer(std::move(vmo), id));
}

TEST(MagmaSystemConnection, MapBufferGpu) {
  auto msd_drv = std::make_unique<MsdMockDriver>();
  auto msd_dev = std::make_unique<MsdMockDevice>();
  auto msd_connection = msd_dev->Open(0);
  auto dev = std::shared_ptr<MagmaSystemDevice>(
      MagmaSystemDevice::Create(msd_drv.get(), std::move(msd_dev)));
  ASSERT_NE(msd_connection, nullptr);
  MagmaSystemConnection connection(dev, std::move(msd_connection));

  constexpr uint64_t kPageCount = 10;
  auto buffer = magma::PlatformBuffer::Create(kPageCount * page_size(), "test");
  ASSERT_TRUE(buffer);

  constexpr uint64_t kBogusId = 0xabcd12345678cabd;
  constexpr uint64_t kGpuVa = 0;  // arbitrary
  constexpr uint64_t kFlags = MAGMA_MAP_FLAG_READ | MAGMA_MAP_FLAG_WRITE | MAGMA_MAP_FLAG_EXECUTE;

  // Bad id
  EXPECT_FALSE(
      connection.MapBuffer(kBogusId, kGpuVa, /*offset=*/0, kPageCount * page_size(), kFlags));

  zx::handle buffer_handle;
  ASSERT_TRUE(buffer->duplicate_handle(&buffer_handle));

  EXPECT_TRUE(connection.ImportBuffer(std::move(buffer_handle), buffer->id()));

  // Bad page offset
  EXPECT_FALSE(connection.MapBuffer(buffer->id(), kGpuVa, /*offset=*/kPageCount * page_size(),
                                    kPageCount * page_size(), kFlags));

  // Bad page count
  EXPECT_FALSE(connection.MapBuffer(buffer->id(), kGpuVa, /*offset=*/0,
                                    (kPageCount + 1) * page_size(), kFlags));

  // Page offset + page count overflows
  EXPECT_FALSE(
      connection.MapBuffer(buffer->id(), kGpuVa,
                           /*offset=*/(std::numeric_limits<uint64_t>::max() - 1) * page_size(),
                           (kPageCount + 1) * page_size(), kFlags));

  EXPECT_TRUE(
      connection.MapBuffer(buffer->id(), kGpuVa, /*offset=*/0, kPageCount * page_size(), kFlags));
}

TEST(MagmaSystemConnection, PerformanceCounters) {
  auto msd_drv = std::make_unique<MsdMockDriver>();
  auto msd_dev = std::make_unique<MsdMockDevice>();
  auto msd_connection = msd_dev->Open(0);
  auto dev = std::shared_ptr<MagmaSystemDevice>(
      MagmaSystemDevice::Create(msd_drv.get(), std::move(msd_dev)));
  ASSERT_NE(msd_connection, nullptr);
  MagmaSystemConnection connection(dev, std::move(msd_connection));
  connection.set_can_access_performance_counters(true);

  constexpr uint64_t kValidPoolId = 1;
  constexpr uint64_t kInvalidPoolId = 2;

  EXPECT_EQ(MAGMA_STATUS_OK, connection
                                 .CreatePerformanceCounterBufferPool(
                                     std::make_unique<MockPerfCountPool>(kValidPoolId))
                                 .get());
  EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS, connection
                                           .CreatePerformanceCounterBufferPool(
                                               std::make_unique<MockPerfCountPool>(kValidPoolId))
                                           .get());

  EXPECT_EQ(MAGMA_STATUS_INVALID_ARGS,
            connection.DumpPerformanceCounters(kInvalidPoolId, 1u).get());
  EXPECT_EQ(MAGMA_STATUS_OK, connection.DumpPerformanceCounters(kValidPoolId, 1u).get());

  constexpr uint64_t kTestSize = 4096;
  auto buf = magma::PlatformBuffer::Create(kTestSize, "test");

  ASSERT_NE(buf, nullptr);
  EXPECT_GE(buf->size(), kTestSize);

  zx::handle duplicate_handle1;
  ASSERT_TRUE(buf->duplicate_handle(&duplicate_handle1));

  uint64_t id = buf->id();
  EXPECT_TRUE(connection.ImportBuffer(std::move(duplicate_handle1), id));

  EXPECT_EQ(
      MAGMA_STATUS_INVALID_ARGS,
      connection.AddPerformanceCounterBufferOffsetToPool(kValidPoolId, id + 1, 0, kTestSize).get());
  EXPECT_EQ(
      MAGMA_STATUS_INVALID_ARGS,
      connection.AddPerformanceCounterBufferOffsetToPool(kInvalidPoolId, id, 0, kTestSize).get());
  EXPECT_EQ(
      MAGMA_STATUS_OK,
      connection.AddPerformanceCounterBufferOffsetToPool(kValidPoolId, id, 0, kTestSize).get());

  EXPECT_EQ(MAGMA_STATUS_OK,
            connection.RemovePerformanceCounterBufferFromPool(kValidPoolId, id).get());

  // Don't explicitly delete pool to ensure the MagmaSystemConnection will prevent leaks by deleting
  // it when closing.
}

}  // namespace

}  // namespace msd
