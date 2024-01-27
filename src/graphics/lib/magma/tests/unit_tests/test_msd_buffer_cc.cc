// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "helper/platform_msd_device_helper.h"
#include "magma_util/dlog.h"
#include "magma_util/short_macros.h"
#include "msd_cc.h"
#include "platform_buffer.h"

namespace {

inline uint64_t page_size() { return sysconf(_SC_PAGESIZE); }

class TestMsd {
 public:
  ~TestMsd() {}

  bool Init() {
    driver_ = msd::Driver::Create();
    if (!driver_)
      return DRETF(false, "msd_driver_create failed");

    device_ = driver_->CreateDevice(GetTestDeviceHandle());
    if (!device_)
      return DRETF(false, "msd_driver_create_device failed");

    return true;
  }

  bool Connect() {
    connection_ = device_->Open(0);
    if (!connection_)
      return DRETF(false, "msd_device_open failed");
    return true;
  }

  bool CreateBuffer(uint32_t size_in_pages, std::unique_ptr<msd::Buffer>* buffer_out) {
    auto platform_buf = magma::PlatformBuffer::Create(size_in_pages * page_size(), "test");
    if (!platform_buf)
      return DRETF(false, "couldn't create platform buffer size_in_pages %u", size_in_pages);

    uint32_t duplicate_handle;
    if (!platform_buf->duplicate_handle(&duplicate_handle))
      return DRETF(false, "couldn't duplicate handle");

    std::unique_ptr<msd::Buffer> buffer =
        driver_->ImportBuffer(zx::vmo(duplicate_handle), platform_buf->id());
    if (!buffer)
      return DRETF(false, "msd_buffer_import failed");

    *buffer_out = std::move(buffer);
    return true;
  }

  msd::Connection* connection() { return connection_.get(); }
  msd::Device* device() { return device_.get(); }
  msd::Driver* driver() { return driver_.get(); }

 private:
  std::unique_ptr<msd::Driver> driver_ = nullptr;
  std::unique_ptr<msd::Device> device_ = nullptr;
  std::unique_ptr<msd::Connection> connection_ = nullptr;
};

}  // namespace

TEST(MsdBuffer, ImportAndDestroy) {
  TestMsd test;
  ASSERT_TRUE(test.Init());
  auto platform_buf = magma::PlatformBuffer::Create(4096, "test");
  ASSERT_NE(platform_buf, nullptr);

  uint32_t duplicate_handle;
  ASSERT_TRUE(platform_buf->duplicate_handle(&duplicate_handle));

  auto msd_buffer = test.driver()->ImportBuffer(zx::vmo(duplicate_handle), platform_buf->id());
  ASSERT_NE(msd_buffer, nullptr);

  msd_buffer.reset();
}

TEST(MsdBuffer, Map) {
  TestMsd test;
  ASSERT_TRUE(test.Init());
  ASSERT_TRUE(test.Connect());

  constexpr uint32_t kBufferSizeInPages = 2;

  std::unique_ptr<msd::Buffer> buffer;
  ASSERT_TRUE(test.CreateBuffer(kBufferSizeInPages, &buffer));

  constexpr uint64_t kGpuAddress = (1ull << 31) / 2;  // Centered in 31 bit space

  EXPECT_EQ(MAGMA_STATUS_OK,
            test.connection()->MapBuffer(*buffer, kGpuAddress,
                                         0,                                 // page offset
                                         kBufferSizeInPages * page_size(),  // page count
                                         MAGMA_MAP_FLAG_READ | MAGMA_MAP_FLAG_WRITE));
  buffer.reset();
}

TEST(MsdBuffer, MapAndUnmap) {
  TestMsd test;
  ASSERT_TRUE(test.Init());
  ASSERT_TRUE(test.Connect());
  std::unique_ptr<magma::PlatformHandle> buffer_handle;
  std::unique_ptr<msd::Buffer> buffer = nullptr;

  constexpr uint32_t kBufferSizeInPages = 1;

  {
    auto platform_buf = magma::PlatformBuffer::Create(kBufferSizeInPages * page_size(), "test");
    ASSERT_TRUE(platform_buf);

    uint32_t raw_handle;
    EXPECT_TRUE(platform_buf->duplicate_handle(&raw_handle));
    buffer_handle = magma::PlatformHandle::Create(raw_handle);
    ASSERT_TRUE(buffer_handle);

    EXPECT_TRUE(platform_buf->duplicate_handle(&raw_handle));
    buffer = test.driver()->ImportBuffer(zx::vmo(raw_handle), platform_buf->id());
    ASSERT_TRUE(buffer);
  }

  // There should be at least two handles, the msd buffer and the "checker handle".
  uint32_t handle_count;
  EXPECT_TRUE(buffer_handle->GetCount(&handle_count));
  EXPECT_GE(2u, handle_count);

  std::vector<uint64_t> gpu_addr{0, page_size() * 1024};

  // Mapping should keep alive the msd buffer.
  for (uint32_t i = 0; i < gpu_addr.size(); i++) {
    EXPECT_EQ(MAGMA_STATUS_OK,
              test.connection()->MapBuffer(*buffer,
                                           gpu_addr[i],                       // gpu addr
                                           0,                                 // page offset
                                           kBufferSizeInPages * page_size(),  // page count
                                           MAGMA_MAP_FLAG_READ | MAGMA_MAP_FLAG_WRITE));
  }

  // Verify we haven't lost any handles.
  EXPECT_TRUE(buffer_handle->GetCount(&handle_count));
  EXPECT_GE(2u, handle_count);

  // Try to unmap a region that doesn't exist.
  EXPECT_NE(MAGMA_STATUS_OK, test.connection()->UnmapBuffer(*buffer, page_size() * 2048));

  // Unmap the valid regions.
  magma_status_t status;
  for (uint32_t i = 0; i < gpu_addr.size(); i++) {
    status = test.connection()->UnmapBuffer(*buffer, gpu_addr[i]);
    EXPECT_TRUE(status == MAGMA_STATUS_UNIMPLEMENTED || status == MAGMA_STATUS_OK);
  }

  if (status != MAGMA_STATUS_OK) {
    // If unmap unsupported, mappings should be released here.
    test.connection()->ReleaseBuffer(*buffer);
  }

  // Mapping should keep alive the msd buffer.
  for (uint32_t i = 0; i < gpu_addr.size(); i++) {
    EXPECT_EQ(MAGMA_STATUS_OK,
              test.connection()->MapBuffer(*buffer,
                                           gpu_addr[i],                       // gpu addr
                                           0,                                 // page offset
                                           kBufferSizeInPages * page_size(),  // page count
                                           MAGMA_MAP_FLAG_READ | MAGMA_MAP_FLAG_WRITE));
  }

  buffer.reset();
}

TEST(MsdBuffer, MapAndAutoUnmap) {
  TestMsd test;
  ASSERT_TRUE(test.Init());
  ASSERT_TRUE(test.Connect());

  std::unique_ptr<magma::PlatformHandle> buffer_handle;
  std::unique_ptr<msd::Buffer> buffer = nullptr;

  constexpr uint32_t kBufferSizeInPages = 1;

  {
    auto platform_buf = magma::PlatformBuffer::Create(kBufferSizeInPages * page_size(), "test");
    ASSERT_TRUE(platform_buf);

    uint32_t raw_handle;
    EXPECT_TRUE(platform_buf->duplicate_handle(&raw_handle));
    buffer_handle = magma::PlatformHandle::Create(raw_handle);
    ASSERT_TRUE(buffer_handle);

    EXPECT_TRUE(platform_buf->duplicate_handle(&raw_handle));
    buffer = test.driver()->ImportBuffer(zx::vmo(raw_handle), platform_buf->id());
    ASSERT_TRUE(buffer);
  }

  // There should be at least two handles, the msd buffer and the "checker handle".
  uint32_t handle_count;
  EXPECT_TRUE(buffer_handle->GetCount(&handle_count));
  EXPECT_GE(2u, handle_count);

  // Mapping should keep alive the msd buffer.
  EXPECT_EQ(MAGMA_STATUS_OK,
            test.connection()->MapBuffer(*buffer,
                                         0,                                 // gpu addr
                                         0,                                 // offset
                                         kBufferSizeInPages * page_size(),  // length
                                         MAGMA_MAP_FLAG_READ | MAGMA_MAP_FLAG_WRITE));

  // Verify we haven't lost any handles.
  EXPECT_TRUE(buffer_handle->GetCount(&handle_count));
  EXPECT_GE(2u, handle_count);

  // Mapping auto released either here...
  test.connection()->ReleaseBuffer(*buffer);

  // OR here.
  buffer.reset();

  // Buffer should be now be released.
  EXPECT_TRUE(buffer_handle->GetCount(&handle_count));
  EXPECT_EQ(1u, handle_count);
}

TEST(MsdBuffer, MapDoesntFit) {
  TestMsd test;
  ASSERT_TRUE(test.Init());
  ASSERT_TRUE(test.Connect());

  constexpr uint32_t kBufferSizeInPages = 2;

  std::unique_ptr<msd::Buffer> buffer;
  ASSERT_TRUE(test.CreateBuffer(kBufferSizeInPages, &buffer));

  constexpr uint64_t kGpuAddressSpaceSize = 1ull << 48;
  magma_status_t status = test.connection()->MapBuffer(
      *buffer,
      kGpuAddressSpaceSize - kBufferSizeInPages / 2 * page_size(),  // gpu addr
      0,                                                            // offset
      kBufferSizeInPages * page_size(),                             // length
      MAGMA_MAP_FLAG_READ | MAGMA_MAP_FLAG_WRITE);
  EXPECT_TRUE(status == MAGMA_STATUS_INVALID_ARGS || status == MAGMA_STATUS_INTERNAL_ERROR);

  buffer.reset();
}
