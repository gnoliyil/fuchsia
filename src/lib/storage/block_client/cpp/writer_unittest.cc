// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/storage/block_client/cpp/writer.h"

#include <gtest/gtest.h>

#include "src/lib/storage/block_client/cpp/fake_block_device.h"

namespace block_client {
namespace {

void CreateAndRegisterVmo(BlockDevice& device, size_t size, zx::vmo& vmo,
                          storage::OwnedVmoid& vmoid) {
  fuchsia_hardware_block::wire::BlockInfo info = {};
  ASSERT_EQ(device.BlockGetInfo(&info), ZX_OK);
  ASSERT_EQ(zx::vmo::create(size, 0, &vmo), ZX_OK);
  ASSERT_EQ(device.BlockAttachVmo(vmo, &vmoid.GetReference(&device)), ZX_OK);
}

TEST(WriterTest, Write) {
  const uint64_t kBlockCount = 2048;
  const uint32_t kBlockSize = 512;

  FakeBlockDevice device(kBlockCount, kBlockSize);
  Writer writer(device);

  const uint64_t kBufferSize = 1024 * 1024;
  zx::vmo vmo;
  storage::OwnedVmoid vmoid;
  ASSERT_NO_FATAL_FAILURE(CreateAndRegisterVmo(device, kBufferSize, vmo, vmoid));

  std::vector<uint8_t> buf(kBufferSize);

  for (unsigned i = 0; i < kBufferSize; ++i)
    buf[i] = static_cast<uint8_t>(i * 17);

  ASSERT_EQ(writer.Write(0, kBufferSize, buf.data()), ZX_OK);

  block_fifo_request_t request{
      .opcode = BLOCK_OP_READ,
      .vmoid = vmoid.get(),
      .length = kBufferSize / kBlockSize,
  };
  ASSERT_EQ(device.FifoTransaction(&request, 1), ZX_OK);

  std::vector<uint8_t> read_buf(kBufferSize);
  ASSERT_EQ(vmo.read(read_buf.data(), 0, read_buf.size()), ZX_OK);

  EXPECT_EQ(read_buf, buf);
}

}  // namespace
}  // namespace block_client
