// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/bin/linux_runner/block_devices.h"

#include <fuchsia/hardware/block/partition/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>

#include <gtest/gtest.h>

#include "src/lib/files/directory.h"
#include "src/lib/files/path.h"
#include "src/storage/testing/fvm.h"
#include "src/storage/testing/ram_disk.h"

class BlockDevicesTest : public ::testing::Test {
 public:
  static constexpr int kBlockSize = 512;
  static constexpr uint64_t kBlockCount = 16 * 1024 * 1024 / kBlockSize;
  static constexpr size_t kFvmSliceSize = 32 * 1024;

  void SetUp() override {
    // Create a ramdisk. We tag with with the FVM GUID so that our code can correctly locate
    // the FVM volume manager on this partition.
    storage::RamDisk::Options ramdisk_options{
        .type_guid = {GUID_FVM_VALUE},
    };
    auto ramdisk = storage::RamDisk::Create(kBlockSize, kBlockCount, ramdisk_options);
    FX_CHECK(ramdisk.is_ok());
    ramdisk_ = std::move(ramdisk.value());
  }

 protected:
  void InitializeFvm() {
    auto fvm_path = storage::CreateFvmInstance(ramdisk_.path(), kFvmSliceSize);
    FX_CHECK(fvm_path.is_ok());
    fvm_path_ = std::move(fvm_path.value());
  }

  void InitializeFvmWithGuestPartition(size_t partition_size) {
    storage::FvmOptions options{.name = kGuestPartitionName,
                                .type = kGuestPartitionGuid,
                                .initial_fvm_slice_count = partition_size / kFvmSliceSize};
    auto fvm_path = storage::CreateFvmPartition(ramdisk_.path(), kFvmSliceSize, options);
    FX_CHECK(fvm_path.is_ok());
    fvm_path_ = std::move(fvm_path.value());
  }

  zx::status<std::array<uint8_t, GPT_GUID_LEN>> ReadPartitionTypeGuid(const std::string& path) {
    fuchsia::hardware::block::partition::PartitionSyncPtr partition;
    zx_status_t status =
        fdio_service_connect(path.c_str(), partition.NewRequest().TakeChannel().release());
    if (status != ZX_OK) {
      FX_PLOGS(ERROR, status) << "Failed to connect to '" << path;
      return zx::error(status);
    }

    zx_status_t guid_status;
    std::unique_ptr<fuchsia::hardware::block::partition::GUID> guid;
    status = partition->GetTypeGuid(&guid_status, &guid);
    if (status != ZX_OK || guid_status != ZX_OK || !guid) {
      return zx::error(ZX_ERR_NOT_FOUND);
    }
    return zx::ok(guid->value);
  }

  std::optional<std::string> FindPartitionWithGuid(std::array<uint8_t, GPT_GUID_LEN> guid) {
    std::vector<std::string> contents;
    bool result = files::ReadDirContents("/dev/class/block", &contents);
    FX_CHECK(result) << "Failed to read block device directory: " << std::strerror(errno);
    for (const auto& entry : contents) {
      auto path = files::JoinPath("/dev/class/block", entry);
      auto result = ReadPartitionTypeGuid(path);
      if (result.is_ok()) {
        if (result.value() == guid) {
          return {std::move(path)};
        }
      }
    }
    return {};
  }

  struct VolumeInfo {
    uint64_t size;
    std::string partition_name;
  };
  zx::status<VolumeInfo> QueryVolumeInfo(const std::string& path) {
    fuchsia::hardware::block::partition::PartitionSyncPtr partition;
    zx_status_t status =
        fdio_service_connect(path.c_str(), partition.NewRequest().TakeChannel().release());
    if (status != ZX_OK) {
      FX_PLOGS(ERROR, status) << "Failed to connect to '" << path;
      return zx::error(status);
    }

    zx_status_t op_status;
    fidl::StringPtr name;
    status = partition->GetName(&op_status, &name);
    if (status != ZX_OK) {
      return zx::error(status);
    }
    if (op_status != ZX_OK) {
      return zx::error(status);
    }

    std::unique_ptr<fuchsia::hardware::block::BlockInfo> block_info;
    status = partition->GetInfo(&op_status, &block_info);
    if (status != ZX_OK) {
      return zx::error(status);
    }
    if (op_status != ZX_OK) {
      return zx::error(status);
    }

    return zx::ok(VolumeInfo{
        .size = block_info->block_count * block_info->block_size,
        .partition_name = *name,
    });
  }

 private:
  storage::RamDisk ramdisk_;
  std::string fvm_path_;
};

TEST_F(BlockDevicesTest, SetupWithoutPartition) {
  InitializeFvm();
  EXPECT_TRUE(FindPartitionWithGuid(GUID_FVM_VALUE));
  EXPECT_FALSE(FindPartitionWithGuid(kGuestPartitionGuid));
}

TEST_F(BlockDevicesTest, SetupWithPartition) {
  InitializeFvmWithGuestPartition(kFvmSliceSize);
  EXPECT_TRUE(FindPartitionWithGuid(GUID_FVM_VALUE));
  EXPECT_TRUE(FindPartitionWithGuid(kGuestPartitionGuid));
}

TEST_F(BlockDevicesTest, CreateFvmPartitionIfNonExistant) {
  InitializeFvm();

  // Get the block devices. This should create a guest partition that is 10 FVM slices.
  auto result = GetBlockDevices(10 * kFvmSliceSize);

  // Expect the partition is created.
  ASSERT_TRUE(result.is_ok());
  EXPECT_TRUE(FindPartitionWithGuid(GUID_FVM_VALUE));
  auto guest_partition = FindPartitionWithGuid(kGuestPartitionGuid);
  EXPECT_TRUE(guest_partition);

  // Verify size/name
  auto info = QueryVolumeInfo(*guest_partition);
  EXPECT_TRUE(info.is_ok());
  EXPECT_EQ(info.value().partition_name, kGuestPartitionName);
  EXPECT_EQ(info.value().size, 10 * kFvmSliceSize);
}

TEST_F(BlockDevicesTest, ReuseExistingPartition) {
  // Initialize a guest partition with a single FVM slice.
  InitializeFvmWithGuestPartition(kFvmSliceSize);

  // Get block devices and request the partition to be 10 slices. This doesn't resize an existing
  // partition so size parameter here is effectively ignored.
  auto result = GetBlockDevices(10 * kFvmSliceSize);

  // Expect to find a partition with a single slice.
  ASSERT_TRUE(result.is_ok());
  EXPECT_TRUE(FindPartitionWithGuid(GUID_FVM_VALUE));
  auto guest_partition = FindPartitionWithGuid(kGuestPartitionGuid);
  EXPECT_TRUE(guest_partition);

  // Verify size/name
  auto info = QueryVolumeInfo(*guest_partition);
  EXPECT_TRUE(info.is_ok());
  EXPECT_EQ(info.value().partition_name, kGuestPartitionName);
  EXPECT_EQ(info.value().size, kFvmSliceSize);
}
