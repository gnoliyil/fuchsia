// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <zircon/device/block.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <iterator>
#include <memory>

#include <fbl/unique_fd.h>
#include <zxtest/zxtest.h>

#include "src/security/lib/zxcrypt/tests/test-device.h"
#include "src/security/lib/zxcrypt/volume.h"

namespace zxcrypt {
namespace testing {
namespace {

// See test-device.h; the following macros allow reusing tests for each of the supported versions.
#define EACH_PARAM(OP, TestSuite, Test) OP(TestSuite, Test, Volume, AES256_XTS_SHA256)

void TestBind(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestBind)

// TODO(aarongreen): When fxbug.dev/31073 is resolved, add tests that check zxcrypt_rekey and
// zxcrypt_shred.

// FIDL tests
void TestBlockGetInfo(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));

  const fidl::WireResult parent_result = fidl::WireCall(device.parent_block())->GetInfo();
  ASSERT_OK(parent_result.status());
  const fit::result parent_response = parent_result.value();
  ASSERT_TRUE(parent_response.is_ok(), "%s", zx_status_get_string(parent_response.error_value()));

  const fidl::WireResult zxcrypt_result = fidl::WireCall(device.zxcrypt_block())->GetInfo();
  ASSERT_OK(zxcrypt_result.status());
  const fit::result zxcrypt_response = zxcrypt_result.value();
  ASSERT_TRUE(zxcrypt_response.is_ok(), "%s", zx_status_get_string(zxcrypt_response.error_value()));

  zx::result reserved_blocks = device.reserved_blocks();
  ASSERT_OK(reserved_blocks);

  EXPECT_EQ(parent_response.value()->info.block_size, zxcrypt_response.value()->info.block_size);
  EXPECT_GE(parent_response.value()->info.block_count,
            zxcrypt_response.value()->info.block_count + reserved_blocks.value());
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestBlockGetInfo)

void TestBlockFvmQuery(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));

  if (!fvm) {
    // Send FVM query to non-FVM device.
    const fidl::WireResult result = fidl::WireCall(device.zxcrypt_volume())->GetVolumeInfo();
    ASSERT_OK(result.status());
    const fidl::WireResponse response = result.value();
    ASSERT_STATUS(response.status, ZX_ERR_NOT_SUPPORTED);
  } else {
    // Get the zxcrypt info.
    const fidl::WireResult parent_result = fidl::WireCall(device.parent_volume())->GetVolumeInfo();
    ASSERT_OK(parent_result.status());
    const fidl::WireResponse parent_response = parent_result.value();
    ASSERT_OK(parent_response.status);

    const fidl::WireResult zxcrypt_result =
        fidl::WireCall(device.zxcrypt_volume())->GetVolumeInfo();
    ASSERT_OK(zxcrypt_result.status());
    const fidl::WireResponse zxcrypt_response = zxcrypt_result.value();
    ASSERT_OK(zxcrypt_response.status);

    zx::result reserved_slices = device.reserved_slices();
    ASSERT_OK(reserved_slices);

    EXPECT_EQ(parent_response.manager->slice_size, zxcrypt_response.manager->slice_size);
    EXPECT_EQ(parent_response.manager->slice_count,
              zxcrypt_response.manager->slice_count + reserved_slices.value());
  }
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestBlockFvmQuery)

void QueryLeadingFvmSlice(const TestDevice& device, bool fvm) {
  uint64_t start_slices[] = {
      0,
  };

  const fidl::WireResult parent_result =
      fidl::WireCall(device.parent_volume())
          ->QuerySlices(fidl::VectorView<uint64_t>::FromExternal(start_slices));
  ASSERT_OK(parent_result.status());
  const fidl::WireResponse parent_response = parent_result.value();

  const fidl::WireResult zxcrypt_result =
      fidl::WireCall(device.zxcrypt_volume())
          ->QuerySlices(fidl::VectorView<uint64_t>::FromExternal(start_slices));
  ASSERT_OK(zxcrypt_result.status());
  const fidl::WireResponse zxcrypt_response = zxcrypt_result.value();

  if (fvm) {
    ASSERT_OK(parent_response.status);
    ASSERT_OK(zxcrypt_response.status);

    // Query zxcrypt about the slices, which should omit those reserved.
    ASSERT_EQ(parent_response.response_count, 1);
    EXPECT_TRUE(parent_response.response[0].allocated);

    ASSERT_EQ(zxcrypt_response.response_count, 1);
    EXPECT_TRUE(zxcrypt_response.response[0].allocated);

    zx::result reserved_slices = device.reserved_slices();
    ASSERT_OK(reserved_slices);

    EXPECT_EQ(parent_response.response[0].count,
              zxcrypt_response.response[0].count + reserved_slices.value());
  } else {
    ASSERT_STATUS(parent_response.status, ZX_ERR_NOT_SUPPORTED);
    ASSERT_STATUS(zxcrypt_response.status, ZX_ERR_NOT_SUPPORTED);
  }
}

void TestBlockFvmVSliceQuery(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  ASSERT_NO_FATAL_FAILURE(QueryLeadingFvmSlice(device, fvm));
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestBlockFvmVSliceQuery)

void TestBlockFvmShrinkAndExtend(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));

  const uint64_t offset = 1;
  const uint64_t length = 1;

  const fidl::WireResult shrink_result =
      fidl::WireCall(device.zxcrypt_volume())->Shrink(offset, length);
  ASSERT_OK(shrink_result.status());
  const fidl::WireResponse shrink_response = shrink_result.value();

  const fidl::WireResult extend_result =
      fidl::WireCall(device.zxcrypt_volume())->Extend(offset, length);
  ASSERT_OK(extend_result.status());
  const fidl::WireResponse extend_response = extend_result.value();

  if (!fvm) {
    ASSERT_STATUS(shrink_response.status, ZX_ERR_NOT_SUPPORTED);
    ASSERT_STATUS(extend_response.status, ZX_ERR_NOT_SUPPORTED);
  } else {
    ASSERT_OK(shrink_response.status);
    ASSERT_OK(extend_response.status);
  }
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestBlockFvmShrinkAndExtend)

void TestFdZeroLength(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));

  ASSERT_NO_FATAL_FAILURE(device.Write(0, 0));
  ASSERT_NO_FATAL_FAILURE(device.Read(0, 0));
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestFdZeroLength)

void TestFdFirstBlock(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  size_t one = device.block_size();

  ASSERT_NO_FATAL_FAILURE(device.Write(0, one));
  ASSERT_NO_FATAL_FAILURE(device.Read(0, one));
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestFdFirstBlock)

void TestFdLastBlock(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  size_t n = device.size();
  size_t one = device.block_size();

  ASSERT_NO_FATAL_FAILURE(device.Write(n - one, one));
  ASSERT_NO_FATAL_FAILURE(device.Read(n - one, one));
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestFdLastBlock)

void TestFdAllBlocks(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  size_t n = device.size();

  ASSERT_NO_FATAL_FAILURE(device.Write(0, n));
  ASSERT_NO_FATAL_FAILURE(device.Read(0, n));
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestFdAllBlocks)

void TestFdUnaligned(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  size_t one = device.block_size();

  ASSERT_NO_FATAL_FAILURE(device.Write(one, one));
  ASSERT_NO_FATAL_FAILURE(device.Read(one, one));

  EXPECT_STATUS(device.SingleWriteBytes(one, one, one - 1), ZX_ERR_INVALID_ARGS);
  EXPECT_STATUS(device.SingleReadBytes(one, one, one - 1), ZX_ERR_INVALID_ARGS);

  EXPECT_STATUS(device.SingleWriteBytes(one, one, one + 1), ZX_ERR_INVALID_ARGS);
  EXPECT_STATUS(device.SingleReadBytes(one, one, one + 1), ZX_ERR_INVALID_ARGS);

  EXPECT_STATUS(device.SingleWriteBytes(one, one - 1, one), ZX_ERR_INVALID_ARGS);
  EXPECT_STATUS(device.SingleReadBytes(one, one - 1, one), ZX_ERR_INVALID_ARGS);

  EXPECT_STATUS(device.SingleWriteBytes(one, one + 1, one), ZX_ERR_INVALID_ARGS);
  EXPECT_STATUS(device.SingleReadBytes(one, one + 1, one), ZX_ERR_INVALID_ARGS);
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestFdUnaligned)

void TestFdOutOfBounds(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  size_t n = device.size();
  size_t one = device.block_size();
  size_t two = one + one;

  ASSERT_NO_FATAL_FAILURE(device.Write(0, one));

  EXPECT_STATUS(device.SingleWriteBytes(n, one, n), ZX_ERR_OUT_OF_RANGE);

  EXPECT_STATUS(device.SingleWriteBytes(n - one, two, n - one), ZX_ERR_OUT_OF_RANGE);

  EXPECT_STATUS(device.SingleWriteBytes(two, n - one, two), ZX_ERR_OUT_OF_RANGE);

  EXPECT_STATUS(device.SingleWriteBytes(one, n, one), ZX_ERR_OUT_OF_RANGE);

  ASSERT_NO_FATAL_FAILURE(device.Read(0, one));

  EXPECT_STATUS(device.SingleReadBytes(n, one, n), ZX_ERR_OUT_OF_RANGE);

  EXPECT_STATUS(device.SingleReadBytes(n - one, two, n - one), ZX_ERR_OUT_OF_RANGE);

  EXPECT_STATUS(device.SingleReadBytes(two, n - one, two), ZX_ERR_OUT_OF_RANGE);

  EXPECT_STATUS(device.SingleReadBytes(one, n, one), ZX_ERR_OUT_OF_RANGE);
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestFdOutOfBounds)

void TestFdOneToMany(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  size_t n = device.size();
  size_t one = device.block_size();

  ASSERT_NO_FATAL_FAILURE(device.Write(0, n));
  ASSERT_NO_FATAL_FAILURE(device.Rebind());

  for (size_t off = 0; off < n; off += one) {
    ASSERT_NO_FATAL_FAILURE(device.Read(off, one));
  }
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestFdOneToMany)

void TestFdManyToOne(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  size_t n = device.size();
  size_t one = device.block_size();

  for (size_t off = 0; off < n; off += one) {
    ASSERT_NO_FATAL_FAILURE(device.Write(off, one));
  }

  ASSERT_NO_FATAL_FAILURE(device.Rebind());
  ASSERT_NO_FATAL_FAILURE(device.Read(0, n));
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestFdManyToOne)

// Device::BlockWrite and Device::BlockRead tests
void TestVmoZeroLength(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));

  // Zero length is illegal for the block fifo
  EXPECT_STATUS(device.block_fifo_txn(BLOCKIO_WRITE, 0, 0), ZX_ERR_INVALID_ARGS);
  EXPECT_STATUS(device.block_fifo_txn(BLOCKIO_READ, 0, 0), ZX_ERR_INVALID_ARGS);
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestVmoZeroLength)

void TestVmoFirstBlock(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));

  ASSERT_NO_FATAL_FAILURE(device.WriteVmo(0, 1));
  ASSERT_NO_FATAL_FAILURE(device.ReadVmo(0, 1));
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestVmoFirstBlock)

void TestVmoLastBlock(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  size_t n = device.block_count();

  ASSERT_NO_FATAL_FAILURE(device.WriteVmo(n - 1, 1));
  ASSERT_NO_FATAL_FAILURE(device.ReadVmo(n - 1, 1));
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestVmoLastBlock)

void TestVmoAllBlocks(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  size_t n = device.block_count();

  ASSERT_NO_FATAL_FAILURE(device.WriteVmo(0, n));
  ASSERT_NO_FATAL_FAILURE(device.ReadVmo(0, n));
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestVmoAllBlocks)

void TestVmoOutOfBounds(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  size_t n = device.block_count();

  ASSERT_NO_FATAL_FAILURE(device.WriteVmo(0, 1));

  EXPECT_STATUS(device.block_fifo_txn(BLOCKIO_WRITE, n, 1), ZX_ERR_OUT_OF_RANGE);
  EXPECT_STATUS(device.block_fifo_txn(BLOCKIO_WRITE, n - 1, 2), ZX_ERR_OUT_OF_RANGE);
  EXPECT_STATUS(device.block_fifo_txn(BLOCKIO_WRITE, 2, n - 1), ZX_ERR_OUT_OF_RANGE);
  EXPECT_STATUS(device.block_fifo_txn(BLOCKIO_WRITE, 1, n), ZX_ERR_OUT_OF_RANGE);

  ASSERT_NO_FATAL_FAILURE(device.ReadVmo(0, 1));

  EXPECT_STATUS(device.block_fifo_txn(BLOCKIO_READ, n, 1), ZX_ERR_OUT_OF_RANGE);
  EXPECT_STATUS(device.block_fifo_txn(BLOCKIO_READ, n - 1, 2), ZX_ERR_OUT_OF_RANGE);
  EXPECT_STATUS(device.block_fifo_txn(BLOCKIO_READ, 2, n - 1), ZX_ERR_OUT_OF_RANGE);
  EXPECT_STATUS(device.block_fifo_txn(BLOCKIO_READ, 1, n), ZX_ERR_OUT_OF_RANGE);
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestVmoOutOfBounds)

void TestVmoOneToMany(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  size_t n = device.block_count();

  ASSERT_NO_FATAL_FAILURE(device.WriteVmo(0, n));
  ASSERT_NO_FATAL_FAILURE(device.Rebind());
  for (size_t off = 0; off < n; ++off) {
    ASSERT_NO_FATAL_FAILURE(device.ReadVmo(off, 1));
  }
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestVmoOneToMany)

void TestVmoManyToOne(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));
  size_t n = device.block_count();

  for (size_t off = 0; off < n; ++off) {
    ASSERT_NO_FATAL_FAILURE(device.WriteVmo(off, 1));
  }

  ASSERT_NO_FATAL_FAILURE(device.Rebind());
  ASSERT_NO_FATAL_FAILURE(device.ReadVmo(0, n));
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestVmoManyToOne)

// Disabled due to flakiness (see fxbug.dev/31974).
void DISABLED_TestVmoStall(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));

  // The device can have up to 4 * max_transfer_size bytes in flight before it begins queuing them
  // internally.
  //
  // TODO(https://fxbug.dev/31974): the result of this call is unused. Why?
  const fidl::WireResult result = fidl::WireCall(device.zxcrypt_block())->GetInfo();
  ASSERT_OK(result.status());
  const fit::result response = result.value();
  ASSERT_TRUE(response.is_ok(), "%s", zx_status_get_string(response.error_value()));

  size_t blks_per_req = 4;
  size_t max = Volume::kBufferSize / (device.block_size() * blks_per_req);
  size_t num = max + 1;
  fbl::AllocChecker ac;
  std::unique_ptr<block_fifo_request_t[]> requests(new (&ac) block_fifo_request_t[num]);
  ASSERT_TRUE(ac.check());
  for (size_t i = 0; i < num; ++i) {
    requests[i].opcode = (i % 2 == 0 ? BLOCKIO_WRITE : BLOCKIO_READ);
    requests[i].length = static_cast<uint32_t>(blks_per_req);
    requests[i].dev_offset = 0;
    requests[i].vmo_offset = 0;
  }

  ASSERT_NO_FATAL_FAILURE(device.SleepUntil(max, true /* defer transactions */));
  EXPECT_EQ(device.block_fifo_txn(requests.get(), num), ZX_OK);
  ASSERT_NO_FATAL_FAILURE(device.WakeUp());
}
DEFINE_EACH_DEVICE(ZxcryptTest, DISABLED_TestVmoStall)

void TestWriteAfterFvmExtend(Volume::Version version) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, true));
  size_t n = device.size();
  size_t one = device.block_size();

  EXPECT_STATUS(device.SingleWriteBytes(n, one, n), ZX_ERR_OUT_OF_RANGE);

  const fidl::WireResult result = fidl::WireCall(device.zxcrypt_volume())->GetVolumeInfo();
  ASSERT_OK(result.status());
  const fidl::WireResponse response = result.value();
  ASSERT_OK(response.status);

  uint64_t offset = device.size() / response.manager->slice_size;
  uint64_t length = 1;

  {
    const fidl::WireResult result = fidl::WireCall(device.zxcrypt_volume())->Extend(offset, length);
    ASSERT_OK(result.status());
    const fidl::WireResponse response = result.value();
    ASSERT_OK(response.status);
  }

  EXPECT_OK(device.SingleWriteBytes(n, one, n));
}
DEFINE_EACH(ZxcryptTest, TestWriteAfterFvmExtend)

void TestUnalignedVmoOffset(Volume::Version version, bool fvm) {
  TestDevice device;
  ASSERT_NO_FATAL_FAILURE(device.SetupDevmgr());
  ASSERT_NO_FATAL_FAILURE(device.Bind(version, fvm));

  block_fifo_request_t request{
      .opcode = BLOCKIO_READ,
      .length = 2,
      .vmo_offset = 1,
      .dev_offset = 0,
  };

  ASSERT_OK(device.block_fifo_txn(&request, 1));
}
DEFINE_EACH_DEVICE(ZxcryptTest, TestUnalignedVmoOffset)

// TODO(aarongreen): Currently, we're using XTS, which provides no data integrity.  When possible,
// we should switch to an AEAD, which would allow us to detect data corruption when doing I/O.
// void TestBadData(void) {
// }

}  // namespace
}  // namespace testing
}  // namespace zxcrypt
