// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/blobfs/test/blobfs_test_setup.h"

#include <gtest/gtest.h>

#include "src/lib/storage/block_client/cpp/fake_block_device.h"
#include "src/storage/blobfs/mkfs.h"
#include "src/storage/blobfs/test/unit/local_decompressor_creator.h"

namespace blobfs {

zx_status_t BlobfsTestSetupBase::CreateFormatMount(uint64_t block_count, uint64_t block_size,
                                                   const FilesystemOptions& fs_options,
                                                   const MountOptions& mount_options) {
  auto device = std::make_unique<block_client::FakeBlockDevice>(block_count, block_size);
  if (zx_status_t status = FormatFilesystem(device.get(), fs_options); status != ZX_OK)
    return status;
  return Mount(std::move(device), mount_options);
}

zx_status_t BlobfsTestSetupBase::Mount(std::unique_ptr<BlockDevice> device,
                                       const MountOptions& options) {
  EXPECT_EQ(blobfs_, nullptr);  // Should not already be mounted.

  vfs_ = std::make_unique<fs::PagedVfs>(dispatcher());
  if (auto status = vfs_->Init(); status.is_error()) {
    vfs_.reset();
    return status.error_value();
  }

  MountOptions options_copy = options;
  // Override default decompression sandbox connector to be a local threaded version.
  if (options.decompression_connector == nullptr) {
    auto connector_or = GetDecompressorCreatorConnector();
    if (connector_or.is_error()) {
      return connector_or.error_value();
    }
    options_copy.decompression_connector = connector_or.value();
  }

  auto blobfs_or = Blobfs::Create(dispatcher(), std::move(device), vfs_.get(), options_copy);
  if (blobfs_or.is_error())
    return blobfs_or.error_value();
  blobfs_ = std::move(blobfs_or.value());

  return ZX_OK;
}

zx::result<DecompressorCreatorConnector*> BlobfsTestSetupBase::GetDecompressorCreatorConnector() {
  if (!decompressor_connector_) {
    auto connector_or = LocalDecompressorCreator::Create();
    if (!connector_or.is_ok()) {
      return connector_or.take_error();
    }
    decompressor_connector_ = std::move(connector_or.value());
  }
  return zx::ok(&decompressor_connector_->GetDecompressorConnector());
}

std::unique_ptr<BlockDevice> BlobfsTestSetupBase::Unmount() {
  auto block_device = Blobfs::Destroy(std::move(blobfs_));
  ShutdownVfs();
  vfs_.reset();
  return block_device;
}

zx_status_t BlobfsTestSetupBase::Remount(const MountOptions& options) {
  auto block_device = Unmount();
  return Mount(std::move(block_device), options);
}

BlobfsTestSetup::~BlobfsTestSetup() {
  loop_.RunUntilIdle();
  if (blobfs_)
    Blobfs::Destroy(std::move(blobfs_));
  if (vfs())
    ShutdownVfs();
}

void BlobfsTestSetup::ShutdownVfs() {
  vfs()->Shutdown([](zx_status_t) {});
  loop().RunUntilIdle();
  vfs()->TearDown();
}

BlobfsTestSetupWithThread::BlobfsTestSetupWithThread() { loop_.StartThread("blobfs-async-loop"); }

BlobfsTestSetupWithThread::~BlobfsTestSetupWithThread() {
  if (blobfs_)
    Blobfs::Destroy(std::move(blobfs_));
  if (vfs())
    ShutdownVfs();
}

void BlobfsTestSetupWithThread::ShutdownVfs() {
  sync_completion_t completion;
  auto cb = [&completion](zx_status_t status) { sync_completion_signal(&completion); };
  vfs()->Shutdown(cb);
  ASSERT_EQ(sync_completion_wait(&completion, ZX_TIME_INFINITE), ZX_OK);
  vfs()->TearDown();
}

}  // namespace blobfs
