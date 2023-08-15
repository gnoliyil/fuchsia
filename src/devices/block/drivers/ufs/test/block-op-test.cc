// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "unit-lib.h"

namespace ufs {
class BlockOpTest : public UfsTest {
 public:
  void SetUp() override {
    UfsTest::SetUp();

    ASSERT_NO_FATAL_FAILURE(RunInit());
    while (device_->child_count() == 0) {
      zx::nanosleep(zx::deadline_after(zx::msec(1)));
    }

    zx_device* lu_dev = device_->GetLatestChild();
    ddk::BlockImplProtocolClient::CreateFromDevice(lu_dev, &client_);
    client_.Query(&info_, &op_size_);
  }

 protected:
  ddk::BlockImplProtocolClient client_;
  block_info_t info_;
  uint64_t op_size_;
};

TEST_F(BlockOpTest, ReadTest) {
  const uint8_t kTestLun = 0;

  char buf[ufs_mock_device::kMockBlockSize];
  std::strncpy(buf, "test", sizeof(buf));
  ASSERT_OK(mock_device_->BufferWrite(kTestLun, buf, 1, 0));

  sync_completion_t done;
  auto callback = [](void* ctx, zx_status_t status, block_op_t* op) {
    EXPECT_OK(status);
    sync_completion_signal(static_cast<sync_completion_t*>(ctx));
  };

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(ufs_mock_device::kMockBlockSize, 0, &vmo));
  auto block_op = std::make_unique<uint8_t[]>(op_size_);
  auto op = reinterpret_cast<block_op_t*>(block_op.get());
  *op = {
      .rw =
          {
              .command =
                  {
                      .opcode = BLOCK_OPCODE_READ,
                  },
              .vmo = vmo.get(),
              .length = 1,
              .offset_dev = 0,
              .offset_vmo = 0,
          },
  };
  client_.Queue(op, callback, &done);
  sync_completion_wait(&done, ZX_TIME_INFINITE);

  zx_vaddr_t vaddr;
  ASSERT_OK(zx::vmar::root_self()->map(ZX_VM_PERM_READ, 0, vmo, 0, ufs_mock_device::kMockBlockSize,
                                       &vaddr));
  char* mapped_vaddr = reinterpret_cast<char*>(vaddr);
  ASSERT_EQ(std::memcmp(buf, mapped_vaddr, ufs_mock_device::kMockBlockSize), 0);
  ASSERT_OK(zx::vmar::root_self()->unmap(vaddr, ufs_mock_device::kMockBlockSize));
}

TEST_F(BlockOpTest, WriteTest) {
  const uint8_t kTestLun = 0;

  sync_completion_t done;
  auto callback = [](void* ctx, zx_status_t status, block_op_t* op) {
    EXPECT_OK(status);
    sync_completion_signal(static_cast<sync_completion_t*>(ctx));
  };

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(ufs_mock_device::kMockBlockSize, 0, &vmo));

  zx_vaddr_t vaddr;
  ASSERT_OK(zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo, 0,
                                       ufs_mock_device::kMockBlockSize, &vaddr));
  char* mapped_vaddr = reinterpret_cast<char*>(vaddr);
  std::strncpy(mapped_vaddr, "test", ufs_mock_device::kMockBlockSize);

  auto block_op = std::make_unique<uint8_t[]>(op_size_);
  auto op = reinterpret_cast<block_op_t*>(block_op.get());
  *op = {
      .rw =
          {
              .command =
                  {
                      .opcode = BLOCK_OPCODE_WRITE,
                  },
              .vmo = vmo.get(),
              .length = 1,
              .offset_dev = 0,
              .offset_vmo = 0,
          },
  };
  client_.Queue(op, callback, &done);
  sync_completion_wait(&done, ZX_TIME_INFINITE);

  char buf[ufs_mock_device::kMockBlockSize];
  ASSERT_OK(mock_device_->BufferRead(kTestLun, buf, 1, 0));

  ASSERT_EQ(std::memcmp(buf, mapped_vaddr, ufs_mock_device::kMockBlockSize), 0);
  ASSERT_OK(zx::vmar::root_self()->unmap(vaddr, ufs_mock_device::kMockBlockSize));
}

TEST_F(BlockOpTest, IoRangeExceptionTest) {
  sync_completion_t done;
  auto callback = [](void* ctx, zx_status_t status, block_op_t* op) {
    EXPECT_OK(status);
    sync_completion_signal(static_cast<sync_completion_t*>(ctx));
  };

  auto exception_callback = [](void* ctx, zx_status_t status, block_op_t* op) {
    // exception_callback expect I/O range error.
    EXPECT_EQ(status, ZX_ERR_OUT_OF_RANGE);
    sync_completion_signal(static_cast<sync_completion_t*>(ctx));
  };

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(ufs_mock_device::kMockBlockSize, 0, &vmo));
  auto block_op = std::make_unique<uint8_t[]>(op_size_);
  auto op = reinterpret_cast<block_op_t*>(block_op.get());

  // Normal I/O. No errors occur.
  *op = {
      .rw =
          {
              .command =
                  {
                      .opcode = BLOCK_OPCODE_READ,
                  },
              .vmo = vmo.get(),
              .length = 1,
              .offset_dev = 0,
              .offset_vmo = 0,
          },
  };
  client_.Queue(op, callback, &done);
  sync_completion_wait(&done, ZX_TIME_INFINITE);
  sync_completion_reset(&done);

  // If the I/O length is zero, an I/O range error occurs.
  *op = {
      .rw =
          {
              .command =
                  {
                      .opcode = BLOCK_OPCODE_READ,
                  },
              .vmo = vmo.get(),
              .length = 0,
              .offset_dev = 0,
          },
  };
  client_.Queue(op, exception_callback, &done);
  sync_completion_wait(&done, ZX_TIME_INFINITE);
  sync_completion_reset(&done);

  // If the I/O length exceeds the total block count, an I/O range error occurs.
  *op = {
      .rw =
          {
              .command =
                  {
                      .opcode = BLOCK_OPCODE_READ,
                  },
              .vmo = vmo.get(),
              .length = static_cast<uint32_t>(info_.block_count) + 1,
              .offset_dev = 0,
          },
  };
  client_.Queue(op, exception_callback, &done);
  sync_completion_wait(&done, ZX_TIME_INFINITE);
  sync_completion_reset(&done);

  // If the request offset does not fit within total block count, an I/O range error occurs.
  *op = {
      .rw =
          {
              .command =
                  {
                      .opcode = BLOCK_OPCODE_READ,
                  },
              .vmo = vmo.get(),
              .length = 1,
              .offset_dev = static_cast<uint32_t>(info_.block_count),
          },
  };
  client_.Queue(op, exception_callback, &done);
  sync_completion_wait(&done, ZX_TIME_INFINITE);
  sync_completion_reset(&done);

  // If the request offset and length does not fit within total block count, an I/O range error
  // occurs.
  *op = {
      .rw =
          {
              .command =
                  {
                      .opcode = BLOCK_OPCODE_READ,
                  },
              .vmo = vmo.get(),
              .length = 2,
              .offset_dev = static_cast<uint32_t>(info_.block_count) - 1,
          },
  };
  client_.Queue(op, exception_callback, &done);
  sync_completion_wait(&done, ZX_TIME_INFINITE);
  sync_completion_reset(&done);
}

}  // namespace ufs
