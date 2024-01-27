// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/blobfs/blob.h"

#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/lib/digest/digest.h"
#include "src/lib/digest/node-digest.h"
#include "src/lib/storage/block_client/cpp/fake_block_device.h"
#include "src/storage/blobfs/blob_layout.h"
#include "src/storage/blobfs/blobfs.h"
#include "src/storage/blobfs/common.h"
#include "src/storage/blobfs/format.h"
#include "src/storage/blobfs/fsck.h"
#include "src/storage/blobfs/mkfs.h"
#include "src/storage/blobfs/test/blob_utils.h"
#include "src/storage/blobfs/test/blobfs_test_setup.h"
#include "src/storage/blobfs/test/test_scoped_vnode_open.h"
#include "src/storage/blobfs/test/unit/utils.h"
#include "zircon/compiler.h"

namespace blobfs {

namespace {

constexpr const char kEmptyBlobName[] =
    "15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b";

constexpr uint32_t kTestDeviceBlockSize = 512;
constexpr uint32_t kTestDeviceNumBlocks = 400 * kBlobfsBlockSize / kTestDeviceBlockSize;
namespace fio = fuchsia_io;

struct BlobWriteOptions {
  CompressionAlgorithm compression_algorithm;
  bool streaming_writes = false;
};

constexpr BlobWriteOptions kAllValidWriteOptions[] = {
    {.compression_algorithm = CompressionAlgorithm::kUncompressed},
    {.compression_algorithm = CompressionAlgorithm::kChunked},
    // Streaming writes are only supported when compression is disabled.
    {.compression_algorithm = CompressionAlgorithm::kUncompressed, .streaming_writes = true},
};

}  // namespace

class BlobTest : public BlobfsTestSetup,
                 public testing::TestWithParam<std::tuple<BlobLayoutFormat, BlobWriteOptions>> {
 public:
  // Tests that need to test migration from a specific revision can override this method to
  // specify an older minor revision. See also blobfs_revision_test.cc for general migration tests.
  virtual uint64_t GetOldestMinorVersion() const { return kBlobfsCurrentMinorVersion; }

  void SetUp() override {
    auto device =
        std::make_unique<block_client::FakeBlockDevice>(kTestDeviceNumBlocks, kTestDeviceBlockSize);
    device->set_hook([this](const block_fifo_request_t& request, const zx::vmo* vmo) {
      std::lock_guard l(this->hook_lock_);
      if (hook_) {
        return hook_(request, vmo);
      }
      return ZX_OK;
    });

    FilesystemOptions filesystem_options{
        .blob_layout_format = std::get<0>(GetParam()),
        .oldest_minor_version = GetOldestMinorVersion(),
    };
    ASSERT_EQ(FormatFilesystem(device.get(), filesystem_options), ZX_OK);

    MountOptions mount_options{
        .compression_settings =
            {
                .compression_algorithm = std::get<1>(GetParam()).compression_algorithm,
            },
        .streaming_writes = std::get<1>(GetParam()).streaming_writes,
    };
    ASSERT_EQ(ZX_OK, Mount(std::move(device), mount_options));
  }

  const zx::vmo& GetPagedVmo(Blob& blob) {
    std::lock_guard lock(blob.mutex_);
    return blob.paged_vmo();
  }

  void set_hook(block_client::FakeBlockDevice::Hook hook) {
    std::lock_guard l(this->hook_lock_);
    hook_ = std::move(hook);
  }

 private:
  std::mutex hook_lock_;
  block_client::FakeBlockDevice::Hook hook_ __TA_GUARDED(hook_lock_);
};

namespace {

TEST_P(BlobTest, TruncateWouldOverflow) {
  fbl::RefPtr root = OpenRoot();
  fbl::RefPtr<fs::Vnode> file;
  ASSERT_EQ(root->Create(kEmptyBlobName, 0, &file), ZX_OK);

  EXPECT_EQ(file->Truncate(UINT64_MAX), ZX_ERR_OUT_OF_RANGE);
}

// Tests that Blob::Sync issues the callback in the right way in the right cases. This does not
// currently test that the data was actually written to the block device.
TEST_P(BlobTest, SyncBehavior) {
  auto root = OpenRoot();

  std::unique_ptr<BlobInfo> info = GenerateRandomBlob("", 64);
  memmove(info->path, info->path + 1, strlen(info->path));  // Remove leading slash.

  fbl::RefPtr<fs::Vnode> file;
  ASSERT_EQ(root->Create(info->path, 0, &file), ZX_OK);

  size_t out_actual = 0;
  EXPECT_EQ(file->Truncate(info->size_data), ZX_OK);

  // Try syncing before the data has been written. This currently issues an error synchronously but
  // we accept either synchronous or asynchronous callbacks.
  sync_completion_t sync;
  file->Sync([&](zx_status_t status) {
    EXPECT_EQ(ZX_ERR_BAD_STATE, status);
    sync_completion_signal(&sync);
  });
  sync_completion_wait(&sync, ZX_TIME_INFINITE);

  EXPECT_EQ(file->Write(info->data.get(), info->size_data, 0, &out_actual), ZX_OK);
  EXPECT_EQ(info->size_data, out_actual);

  // It's difficult to get a precise hook into the period between when data has been written and
  // when it has been flushed to disk.  The journal will delay flushing metadata, so the following
  // should test sync being called before metadata has been flushed, and then again afterwards.
  for (int i = 0; i < 2; ++i) {
    sync_completion_t sync;
    file->Sync([&](zx_status_t status) {
      EXPECT_EQ(ZX_OK, status) << i;
      sync_completion_signal(&sync);
    });
    sync_completion_wait(&sync, ZX_TIME_INFINITE);
  }
}

TEST_P(BlobTest, ReadingBlobZerosTail) {
  // Remount without compression so that we can manipulate the data that is loaded.
  MountOptions options = {.compression_settings = {
                              .compression_algorithm = CompressionAlgorithm::kUncompressed,
                          }};
  ASSERT_EQ(ZX_OK, Remount(options));

  std::unique_ptr<BlobInfo> info = GenerateRandomBlob("", 64);
  uint64_t block;
  {
    auto root = OpenRoot();
    fbl::RefPtr<fs::Vnode> file;
    ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
    size_t out_actual = 0;
    EXPECT_EQ(file->Truncate(info->size_data), ZX_OK);
    EXPECT_EQ(file->Write(info->data.get(), info->size_data, 0, &out_actual), ZX_OK);
    EXPECT_EQ(out_actual, info->size_data);
    {
      auto blob = fbl::RefPtr<Blob>::Downcast(file);
      block = blobfs()->GetNode(blob->Ino())->extents[0].Start() + DataStartBlock(blobfs()->Info());
    }
  }

  auto block_device = Unmount();

  // Read the block that contains the blob.
  storage::VmoBuffer buffer;
  ASSERT_EQ(buffer.Initialize(block_device.get(), 1, kBlobfsBlockSize, "test_buffer"), ZX_OK);
  block_fifo_request_t request = {
      .opcode = BLOCKIO_READ,
      .vmoid = buffer.vmoid(),
      .length = kBlobfsBlockSize / kTestDeviceBlockSize,
      .vmo_offset = 0,
      .dev_offset = block * kBlobfsBlockSize / kTestDeviceBlockSize,
  };
  ASSERT_EQ(block_device->FifoTransaction(&request, 1), ZX_OK);

  // Corrupt the end of the page.
  static_cast<uint8_t*>(buffer.Data(0))[PAGE_SIZE - 1] = 1;

  // Write the block back.
  request.opcode = BLOCKIO_WRITE;
  ASSERT_EQ(block_device->FifoTransaction(&request, 1), ZX_OK);

  // Remount and try and read the blob.
  ASSERT_EQ(ZX_OK, Mount(std::move(block_device), options));

  auto root = OpenRoot();
  fbl::RefPtr<fs::Vnode> file;
  ASSERT_EQ(root->Lookup(info->path + 1, &file), ZX_OK);

  TestScopedVnodeOpen open(file);  // Must be open to read or get the Vmo.

  // Trying to read from the blob would fail if the tail wasn't zeroed.
  size_t actual;
  uint8_t data;
  EXPECT_EQ(file->Read(&data, 1, 0, &actual), ZX_OK);

  zx::vmo vmo;
  EXPECT_EQ(file->GetVmo(fio::wire::VmoFlags::kRead, &vmo), ZX_OK);

  uint64_t data_size;
  EXPECT_EQ(vmo.get_prop_content_size(&data_size), ZX_OK);
  EXPECT_EQ(data_size, 64ul);

  size_t vmo_size;
  EXPECT_EQ(vmo.get_size(&vmo_size), ZX_OK);
  ASSERT_EQ(vmo_size, size_t{PAGE_SIZE});

  EXPECT_EQ(vmo.read(&data, PAGE_SIZE - 1, 1), ZX_OK);
  // The corrupted bit in the tail was zeroed when being read.
  EXPECT_EQ(data, 0);
}

TEST_P(BlobTest, WriteBlobWithSharedBlockInCompactFormat) {
  // Remount without compression so we can force a specific blob size in storage.
  MountOptions options = {.compression_settings = {
                              .compression_algorithm = CompressionAlgorithm::kUncompressed,
                          }};
  Remount(options);

  // Create a blob where the Merkle tree in the compact layout fits perfectly into the space
  // remaining at the end of the blob.
  ASSERT_EQ(blobfs()->Info().block_size, digest::kDefaultNodeSize);
  std::unique_ptr<BlobInfo> info =
      GenerateRealisticBlob("", (digest::kDefaultNodeSize - digest::kSha256Length) * 3);
  {
    if (GetBlobLayoutFormat(blobfs()->Info()) == BlobLayoutFormat::kCompactMerkleTreeAtEnd) {
      std::unique_ptr<MerkleTreeInfo> merkle_tree =
          CreateMerkleTree(info->data.get(), info->size_data, /*use_compact_format=*/true);
      EXPECT_EQ(info->size_data + merkle_tree->merkle_tree_size, digest::kDefaultNodeSize * 3);
    }
    fbl::RefPtr<fs::Vnode> file;
    auto root = OpenRoot();
    ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
    size_t out_actual = 0;
    EXPECT_EQ(file->Truncate(info->size_data), ZX_OK);
    EXPECT_EQ(file->Write(info->data.get(), info->size_data, 0, &out_actual), ZX_OK);
    EXPECT_EQ(out_actual, info->size_data);
  }

  // Remount to avoid caching.
  Remount(options);

  // Read back the blob
  {
    fbl::RefPtr<fs::Vnode> file;
    auto root = OpenRoot();
    ASSERT_EQ(root->Lookup(info->path + 1, &file), ZX_OK);

    TestScopedVnodeOpen open(file);  // Must be open to read.
    size_t actual;
    uint8_t data[info->size_data];
    EXPECT_EQ(file->Read(&data, info->size_data, 0, &actual), ZX_OK);
    EXPECT_EQ(info->size_data, actual);
    EXPECT_EQ(memcmp(data, info->data.get(), info->size_data), 0);
  }
}

TEST_P(BlobTest, WriteErrorsAreFused) {
  std::unique_ptr<BlobInfo> info =
      GenerateRandomBlob("", static_cast<size_t>(kTestDeviceBlockSize) * kTestDeviceNumBlocks);
  auto root = OpenRoot();
  fbl::RefPtr<fs::Vnode> file;
  ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
  ASSERT_EQ(file->Truncate(info->size_data), ZX_OK);
  uint64_t out_actual;
  EXPECT_EQ(file->Write(info->data.get(), info->size_data, 0, &out_actual), ZX_ERR_NO_SPACE);
  // Writing just 1 byte now should see the same error returned.
  EXPECT_EQ(file->Write(info->data.get(), 1, 0, &out_actual), ZX_ERR_NO_SPACE);

  // Whilst we have the failed file still open, we should be able to try again immediately.
  fbl::RefPtr<fs::Vnode> file2;
  ASSERT_EQ(root->Create(info->path + 1, 0, &file2), ZX_OK);
  ASSERT_EQ(file2->Truncate(info->size_data), ZX_OK);
}

TEST_P(BlobTest, UnlinkBlocksUntilNoVmoChildren) {
  std::unique_ptr<BlobInfo> info = GenerateRealisticBlob("", 1 << 16);
  auto root = OpenRoot();

  // Write the blob
  {
    fbl::RefPtr<fs::Vnode> file;
    ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
    size_t out_actual = 0;
    ASSERT_EQ(file->Truncate(info->size_data), ZX_OK);
    ASSERT_EQ(file->Write(info->data.get(), info->size_data, 0, &out_actual), ZX_OK);
    ASSERT_EQ(file->Close(), ZX_OK);
    ASSERT_EQ(out_actual, info->size_data);
  }

  // Get a copy of the VMO, but discard the vnode reference.
  zx::vmo vmo = [&]() {
    fbl::RefPtr<fs::Vnode> file;
    // Lookup doesn't call Open, so no need to Close later.
    EXPECT_EQ(root->Lookup(info->path + 1, &file), ZX_OK);

    // Open the blob, get the vmo, and close the blob.
    TestScopedVnodeOpen open(file);
    zx::vmo vmo;
    EXPECT_EQ(file->GetVmo(fio::wire::VmoFlags::kRead, &vmo), ZX_OK);

    uint64_t data_size;
    EXPECT_EQ(vmo.get_prop_content_size(&data_size), ZX_OK);
    EXPECT_EQ(data_size, info->size_data);

    return vmo;
  }();

  ASSERT_EQ(root->Unlink(info->path + 1, /* must_be_dir=*/false), ZX_OK);
  uint8_t buf[8192];
  for (size_t off = 0; off < 1 << 16; off += kBlobfsBlockSize) {
    EXPECT_EQ(vmo.read(buf, off, kBlobfsBlockSize), ZX_OK);
  }
}

TEST_P(BlobTest, VmoChildDeletedTriggersPurging) {
  std::unique_ptr<BlobInfo> info = GenerateRealisticBlob("", 1 << 16);
  auto root = OpenRoot();

  // Write the blob
  {
    fbl::RefPtr<fs::Vnode> file;
    ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
    size_t out_actual = 0;
    ASSERT_EQ(file->Truncate(info->size_data), ZX_OK);
    ASSERT_EQ(file->Write(info->data.get(), info->size_data, 0, &out_actual), ZX_OK);
    ASSERT_EQ(file->Close(), ZX_OK);
    ASSERT_EQ(out_actual, info->size_data);
  }

  // Get a copy of the VMO, but discard the vnode reference.
  zx::vmo vmo = [&]() {
    fbl::RefPtr<fs::Vnode> file;
    // Lookup doesn't call Open, so no need to Close later.
    EXPECT_EQ(root->Lookup(info->path + 1, &file), ZX_OK);
    zx::vmo vmo;

    // Open the blob, get the vmo, and close the blob.
    TestScopedVnodeOpen open(file);
    EXPECT_EQ(file->GetVmo(fio::wire::VmoFlags::kRead, &vmo), ZX_OK);

    uint64_t data_size;
    EXPECT_EQ(vmo.get_prop_content_size(&data_size), ZX_OK);
    EXPECT_EQ(data_size, info->size_data);

    return vmo;
  }();

  ASSERT_EQ(root->Unlink(info->path + 1, /* must_be_dir=*/false), ZX_OK);

  // Delete the VMO. This should eventually trigger deletion of the blob.
  vmo.reset();

  // Unfortunately, polling the filesystem is the best option for checking the file as deleted.
  bool deleted = false;
  const auto start = std::chrono::steady_clock::now();
  constexpr auto kMaxWait = std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() <= start + kMaxWait) {
    loop().RunUntilIdle();

    fbl::RefPtr<fs::Vnode> file;
    zx_status_t status = root->Lookup(info->path + 1, &file);
    if (status == ZX_ERR_NOT_FOUND) {
      deleted = true;
      break;
    }
    ASSERT_EQ(status, ZX_OK);

    zx::nanosleep(zx::deadline_after(zx::sec(1)));
  }
  EXPECT_TRUE(deleted);
}

// Some paging failures result in permenent failure. Failed block ops should not.
TEST_P(BlobTest, ReadErrorsTemporary) {
  std::unique_ptr<BlobInfo> info = GenerateRealisticBlob("", 1 << 16);
  auto root = OpenRoot();

  // Write the blob
  {
    fbl::RefPtr<fs::Vnode> file;
    ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
    size_t out_actual = 0;
    ASSERT_EQ(file->Truncate(info->size_data), ZX_OK);
    ASSERT_EQ(file->Write(info->data.get(), info->size_data, 0, &out_actual), ZX_OK);
    ASSERT_EQ(file->Close(), ZX_OK);
    ASSERT_EQ(out_actual, info->size_data);
  }

  // Get a copy of the VMO.
  zx::vmo vmo = [&]() {
    fbl::RefPtr<fs::Vnode> file;
    // Lookup doesn't call Open, so no need to Close later.
    EXPECT_EQ(root->Lookup(info->path + 1, &file), ZX_OK);
    zx::vmo vmo;

    // Open the blob, get the vmo, and close the blob.
    TestScopedVnodeOpen open(file);
    EXPECT_EQ(file->GetVmo(fio::wire::VmoFlags::kRead, &vmo), ZX_OK);

    uint64_t data_size;
    EXPECT_EQ(vmo.get_prop_content_size(&data_size), ZX_OK);
    EXPECT_EQ(data_size, info->size_data);

    return vmo;
  }();

  // Add a hook to toggle read failure.
  std::atomic<zx_status_t> fail_ops = ZX_OK;
  set_hook([&fail_ops](const block_fifo_request_t& _req, const zx::vmo* _vmo) {
    return fail_ops.load();
  });

  // Attempt a read with various failure modes.
  char buf;
  for (zx_status_t err :
       {ZX_ERR_IO, ZX_ERR_IO_DATA_INTEGRITY, ZX_ERR_IO_REFUSED, ZX_ERR_BAD_STATE}) {
    fail_ops.store(err);
    ASSERT_NE(vmo.read(&buf, 0, 1), ZX_OK);
  }

  // Now succeed.
  fail_ops.store(ZX_OK);
  ASSERT_EQ(vmo.read(&buf, 0, 1), ZX_OK);

  // Clear the hook to stop using the atomic.
  set_hook([](const block_fifo_request_t& _req, const zx::vmo* _vmo) { return ZX_OK; });
}

std::string GetVmoName(const zx::vmo& vmo) {
  char buf[ZX_MAX_NAME_LEN + 1] = {'\0'};
  EXPECT_EQ(vmo.get_property(ZX_PROP_NAME, buf, ZX_MAX_NAME_LEN), ZX_OK);
  return std::string(buf, ::strlen(buf));
}

TEST_P(BlobTest, VmoNameActiveWhileFdOpen) {
  std::unique_ptr<BlobInfo> info = GenerateRandomBlob("", 64);
  auto root = OpenRoot();
  const std::string active_name = std::string("blob-").append(std::string_view(info->path + 1, 8));
  const std::string inactive_name =
      std::string("inactive-blob-").append(std::string_view(info->path + 1, 8));

  fbl::RefPtr<fs::Vnode> file;
  ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
  size_t out_actual = 0;
  ASSERT_EQ(file->Truncate(info->size_data), ZX_OK);
  ASSERT_EQ(file->Write(info->data.get(), info->size_data, 0, &out_actual), ZX_OK);
  // Make sure the async part of the write finishes.
  loop().RunUntilIdle();
  ASSERT_EQ(file->Close(), ZX_OK);
  ASSERT_EQ(file->OpenValidating(fs::VnodeConnectionOptions(), nullptr), ZX_OK);
  auto blob = fbl::RefPtr<Blob>::Downcast(std::move(file));

  // Blobfs lazily creates the data VMO on first read.
  EXPECT_FALSE(GetPagedVmo(*blob));
  char c;
  size_t actual;
  ASSERT_EQ(blob->Read(&c, sizeof(c), 0u, &actual), ZX_OK);
  EXPECT_TRUE(GetPagedVmo(*blob));
  EXPECT_EQ(GetVmoName(GetPagedVmo(*blob)), active_name);

  ASSERT_EQ(blob->Close(), ZX_OK);
  EXPECT_EQ(GetVmoName(GetPagedVmo(*blob)), inactive_name);

  ASSERT_EQ(blob->OpenValidating(fs::VnodeConnectionOptions(), nullptr), ZX_OK);
  EXPECT_EQ(GetVmoName(GetPagedVmo(*blob)), active_name);

  ASSERT_EQ(blob->Close(), ZX_OK);
}

TEST_P(BlobTest, VmoNameActiveWhileVmoClonesExist) {
  std::unique_ptr<BlobInfo> info = GenerateRandomBlob("", 64);
  auto root = OpenRoot();
  const std::string active_name = std::string("blob-").append(std::string_view(info->path + 1, 8));
  const std::string inactive_name =
      std::string("inactive-blob-").append(std::string_view(info->path + 1, 8));

  fbl::RefPtr<fs::Vnode> file;
  ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
  size_t out_actual = 0;
  ASSERT_EQ(file->Truncate(info->size_data), ZX_OK);
  ASSERT_EQ(file->Write(info->data.get(), info->size_data, 0, &out_actual), ZX_OK);
  // Make sure the async part of the write finishes.
  loop().RunUntilIdle();
  ASSERT_EQ(file->Close(), ZX_OK);
  ASSERT_EQ(file->OpenValidating(fs::VnodeConnectionOptions(), nullptr), ZX_OK);
  auto blob = fbl::RefPtr<Blob>::Downcast(std::move(file));

  zx::vmo vmo;
  ASSERT_EQ(blob->GetVmo(fio::wire::VmoFlags::kRead, &vmo), ZX_OK);
  ASSERT_EQ(blob->Close(), ZX_OK);
  EXPECT_EQ(GetVmoName(GetPagedVmo(*blob)), active_name);

  // The ZX_VMO_ZERO_CHILDREN signal is asynchronous; unfortunately polling is the best we can do.
  vmo.reset();
  bool active = true;
  const auto start = std::chrono::steady_clock::now();
  constexpr auto kMaxWait = std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() <= start + kMaxWait) {
    loop().RunUntilIdle();
    if (GetVmoName(GetPagedVmo(*blob)) == inactive_name) {
      active = false;
      break;
    }
    zx::nanosleep(zx::deadline_after(zx::sec(1)));
  }
  EXPECT_FALSE(active) << "Name did not become inactive after deadline";
}

TEST_P(BlobTest, GetAttributes) {
  std::unique_ptr<BlobInfo> info = GenerateRandomBlob("", 64);
  uint32_t inode;
  uint64_t block_count;

  auto check_attributes = [&](const fs::VnodeAttributes& attributes) {
    ASSERT_EQ(attributes.mode, unsigned{V_TYPE_FILE | V_IRUSR | V_IXUSR});
    ASSERT_EQ(attributes.inode, inode);
    ASSERT_EQ(attributes.content_size, 64u);
    ASSERT_EQ(attributes.storage_size, block_count * kBlobfsBlockSize);
    ASSERT_EQ(attributes.link_count, 1u);
    ASSERT_EQ(attributes.creation_time, 0u);
    ASSERT_EQ(attributes.modification_time, 0u);
  };

  {
    auto root = OpenRoot();

    fbl::RefPtr<fs::Vnode> file;
    ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
    ASSERT_EQ(file->Truncate(info->size_data), ZX_OK);

    size_t out_actual;
    ASSERT_EQ(file->Write(info->data.get(), info->size_data, 0, &out_actual), ZX_OK);
    ASSERT_EQ(out_actual, info->size_data);

    auto blob = fbl::RefPtr<Blob>::Downcast(file);
    inode = blob->Ino();
    block_count = blobfs()->GetNode(inode)->block_count;

    fs::VnodeAttributes attributes;
    ASSERT_EQ(file->GetAttributes(&attributes), ZX_OK);
    check_attributes(attributes);
  }

  Remount();

  auto root = OpenRoot();
  fbl::RefPtr<fs::Vnode> file;
  ASSERT_EQ(root->Lookup(info->path + 1, &file), ZX_OK);
  fs::VnodeAttributes attributes;
  ASSERT_EQ(file->GetAttributes(&attributes), ZX_OK);
  check_attributes(attributes);
}

TEST_P(BlobTest, AppendSetsOutEndCorrectly) {
  std::unique_ptr<BlobInfo> info = GenerateRandomBlob("", 64);
  auto root = OpenRoot();

  fbl::RefPtr<fs::Vnode> file;
  ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
  ASSERT_EQ(file->Truncate(info->size_data), ZX_OK);

  size_t out_end;
  size_t out_actual;
  ASSERT_EQ(file->Append(info->data.get(), 32, &out_end, &out_actual), ZX_OK);
  ASSERT_EQ(out_end, 32u);
  ASSERT_EQ(out_actual, 32u);

  ASSERT_EQ(file->Append(info->data.get() + 32, 32, &out_end, &out_actual), ZX_OK);
  ASSERT_EQ(out_end, 64u);
  ASSERT_EQ(out_actual, 32u);
}

TEST_P(BlobTest, WritesToArbitraryOffsetsFails) {
  std::unique_ptr<BlobInfo> info = GenerateRandomBlob("", 64);
  auto root = OpenRoot();

  fbl::RefPtr<fs::Vnode> file;
  ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
  ASSERT_EQ(file->Truncate(info->size_data), ZX_OK);

  size_t out_actual;
  ASSERT_EQ(file->Write(info->data.get(), 10, 10, &out_actual), ZX_ERR_NOT_SUPPORTED);
  ASSERT_EQ(file->Write(info->data.get(), 10, 0, &out_actual), ZX_OK);
  ASSERT_EQ(out_actual, 10u);
  ASSERT_EQ(file->Write(info->data.get() + 10, info->size_data - 10, 20, &out_actual),
            ZX_ERR_NOT_SUPPORTED);
  ASSERT_EQ(file->Write(info->data.get() + 10, info->size_data - 10, 10, &out_actual), ZX_OK);
  ASSERT_EQ(out_actual, info->size_data - 10);
}

TEST_P(BlobTest, WrittenBlobsArePagedOutWhenClosed) {
  std::unique_ptr<BlobInfo> info = GenerateRandomBlob("", 64);
  auto root = OpenRoot();
  {
    fbl::RefPtr<fs::Vnode> file;
    ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
    size_t out_actual = 0;
    EXPECT_EQ(file->Truncate(info->size_data), ZX_OK);
    EXPECT_EQ(file->Write(info->data.get(), info->size_data, 0, &out_actual), ZX_OK);
    EXPECT_EQ(out_actual, info->size_data);

    auto blob = fbl::RefPtr<Blob>::Downcast(std::move(file));
    // Blobfs lazily creates the data VMO on first read.
    char c;
    size_t actual;
    ASSERT_EQ(blob->Read(&c, sizeof(c), 0u, &actual), ZX_OK);

    EXPECT_EQ(blob->Close(), ZX_OK);
  }

  fbl::RefPtr<fs::Vnode> file;
  // Lookup doesn't call Open, so no need to Close later.
  EXPECT_EQ(root->Lookup(info->path + 1, &file), ZX_OK);
  auto blob = fbl::RefPtr<Blob>::Downcast(std::move(file));

  // Unfortunately, the name of the blob is the best signal that we have for whether it's pageable
  // or not.
  const std::string inactive_name =
      std::string("inactive-blob-").append(std::string_view(info->path + 1, 8));
  EXPECT_EQ(GetVmoName(GetPagedVmo(*blob)), inactive_name) << "VMO wasn't inactive";
}

// Verify that Blobfs handles writing blobs in chunks by repeatedly appending data.
TEST_P(BlobTest, WriteBlobChunked) {
  // To exercise more code paths, we use a non-block aligned blob size.
  constexpr size_t kUnalignedBlobSize = (1 << 10) - 1;
  static_assert(kUnalignedBlobSize % kBlobfsBlockSize, "blob size must be non block aligned");
  std::unique_ptr<BlobInfo> info = GenerateRealisticBlob("", kUnalignedBlobSize);
  auto root = OpenRoot();

  // Write the blob in chunks.
  constexpr size_t kWriteLength = 100;
  {
    fbl::RefPtr<fs::Vnode> file;
    ASSERT_EQ(root->Create(info->path + 1, 0, &file), ZX_OK);
    ASSERT_EQ(file->Truncate(info->size_data), ZX_OK);
    size_t bytes_written = 0;
    while (bytes_written < info->size_data) {
      const size_t to_write = std::min(kWriteLength, info->size_data - bytes_written);
      size_t out_end;
      size_t out_actual;
      ASSERT_EQ(file->Append(info->data.get() + bytes_written, to_write, &out_end, &out_actual),
                ZX_OK);
      bytes_written += out_actual;
    }
    ASSERT_EQ(bytes_written, info->size_data);
    ASSERT_EQ(file->Close(), ZX_OK);
  }

  fbl::RefPtr<fs::Vnode> file;
  // Lookup doesn't call Open, so no need to Close later.
  EXPECT_EQ(root->Lookup(info->path + 1, &file), ZX_OK);

  // Open the blob, get the vmo, and close the blob.
  TestScopedVnodeOpen open(file);

  // Read back data in chunks.
  std::array<uint8_t, 4096> read_buff;
  size_t offset = 0;
  while (offset < kUnalignedBlobSize) {
    size_t out_actual;
    size_t read_len = std::min(read_buff.size(), kUnalignedBlobSize - offset);
    ASSERT_EQ(file->Read(read_buff.data(), read_len, offset, &out_actual), ZX_OK);
    offset += out_actual;
  }
}

std::string GetTestParamName(
    const ::testing::TestParamInfo<std::tuple<BlobLayoutFormat, BlobWriteOptions>>& param) {
  const auto& [layout, write_options] = param.param;
  return GetBlobLayoutFormatNameForTests(layout) +
         GetCompressionAlgorithmName(write_options.compression_algorithm) +
         std::string(write_options.streaming_writes ? "Streaming" : "");
}

INSTANTIATE_TEST_SUITE_P(
    /*no prefix*/, BlobTest,
    testing::Combine(testing::Values(BlobLayoutFormat::kDeprecatedPaddedMerkleTreeAtStart,
                                     BlobLayoutFormat::kCompactMerkleTreeAtEnd),
                     testing::ValuesIn(kAllValidWriteOptions)),
    GetTestParamName);

}  // namespace
}  // namespace blobfs
