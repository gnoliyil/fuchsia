// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/volume_image/fvm/fvm_sparse_image_reader.h"

#include <fidl/fuchsia.device/cpp/wire.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/device-watcher/cpp/device-watcher.h>
#include <lib/fdio/fdio.h>
#include <lib/fzl/resizeable-vmo-mapper.h>
#include <zircon/hw/gpt.h>

#include <fbl/unique_fd.h>
#include <gtest/gtest.h>

#include "src/lib/storage/block_client/cpp/remote_block_device.h"
#include "src/lib/storage/fs_management/cpp/admin.h"
#include "src/lib/storage/fs_management/cpp/fvm.h"
#include "src/storage/fvm/format.h"
#include "src/storage/fvm/fvm_sparse.h"
#include "src/storage/testing/fvm.h"
#include "src/storage/testing/ram_disk.h"
#include "src/storage/volume_image/ftl/ftl_image.h"
#include "src/storage/volume_image/ftl/options.h"
#include "src/storage/volume_image/utils/fd_reader.h"
#include "src/storage/volume_image/utils/writer.h"

namespace storage::volume_image {
namespace {

constexpr std::string_view sparse_image_path = "/pkg/data/test_fvm.sparse.blk";

zx::result<std::string> AttachFvm(const std::string& device_path) {
  zx::result device = component::Connect<fuchsia_device::Controller>(device_path.c_str());
  if (device.is_error()) {
    return device.take_error();
  }
  if (auto status = storage::BindFvm(device.value()); status.is_error())
    return status.take_error();
  std::string fvm_disk_path = device_path + "/fvm";
  if (zx::result channel = device_watcher::RecursiveWaitForFile(fvm_disk_path.c_str(), zx::sec(3));
      channel.is_error()) {
    return channel.take_error();
  }
  return zx::ok(fvm_disk_path);
}

// Create a ram-disk and copy the output directly into the ram-disk, and then see if FVM can read
// it and minfs Fsck passes.
TEST(FvmSparseImageReaderTest, PartitionsInImagePassFsck) {
  auto base_reader_or = FdReader::Create(sparse_image_path);
  ASSERT_TRUE(base_reader_or.is_ok()) << base_reader_or.error();
  auto sparse_image_or = OpenSparseImage(base_reader_or.value(), std::nullopt);
  ASSERT_TRUE(sparse_image_or.is_ok()) << sparse_image_or.error();

  // Create a ram-disk.
  constexpr int kDeviceBlockSize = 8192;
  const uint64_t disk_size = sparse_image_or.value().volume().size;
  auto ram_disk_or = storage::RamDisk::Create(kDeviceBlockSize, disk_size / kDeviceBlockSize);
  ASSERT_TRUE(ram_disk_or.is_ok()) << ram_disk_or.status_string();

  // Open the ram disk
  zx::result channel =
      component::Connect<fuchsia_hardware_block_volume::Volume>(ram_disk_or.value().path());
  ASSERT_TRUE(channel.is_ok()) << channel.status_string();

  zx::result device = block_client::RemoteBlockDevice::Create(std::move(channel.value()));
  ASSERT_TRUE(device.is_ok()) << device.status_string();
  std::unique_ptr<block_client::RemoteBlockDevice> client = std::move(device.value());

  constexpr uint64_t kInitialVmoSize = 1048576;
  auto vmo = fzl::ResizeableVmoMapper::Create(kInitialVmoSize, "test");
  ASSERT_TRUE(vmo);

  storage::Vmoid vmoid;
  ASSERT_EQ(client->BlockAttachVmo(vmo->vmo(), &vmoid), ZX_OK);
  // This is a test, so we don't need to worry about cleaning it up.
  vmoid_t vmo_id = vmoid.TakeId();

  memset(vmo->start(), 0xaf, kInitialVmoSize);

  // Initialize the entire ramdisk with a filler (that isn't zero).
  for (uint64_t offset = 0; offset < disk_size; offset += kInitialVmoSize) {
    block_fifo_request_t request = {
        .opcode = BLOCK_OP_WRITE,
        .vmoid = vmo_id,
        .length =
            static_cast<uint32_t>(std::min(disk_size - offset, kInitialVmoSize) / kDeviceBlockSize),
        .dev_offset = offset / kDeviceBlockSize};
    ASSERT_EQ(client->FifoTransaction(&request, 1), ZX_OK);
  }

  for (const AddressMap& map : sparse_image_or.value().address().mappings) {
    ASSERT_EQ(map.count % kDeviceBlockSize, 0u);
    ASSERT_EQ(map.target % kDeviceBlockSize, 0u);
    ASSERT_LT(map.target, disk_size);
    ASSERT_LE(map.target + map.count, disk_size);
    EXPECT_TRUE(map.options.empty());

    if (vmo->size() < map.count) {
      ASSERT_EQ(vmo->Grow(map.count), ZX_OK);
    }
    auto result = sparse_image_or.value().reader()->Read(
        map.source, cpp20::span<uint8_t>(reinterpret_cast<uint8_t*>(vmo->start()), map.count));
    ASSERT_TRUE(result.is_ok()) << result.error();

    // Write the mapping to the ram disk.
    block_fifo_request_t request = {.opcode = BLOCK_OP_WRITE,
                                    .vmoid = vmo_id,
                                    .length = static_cast<uint32_t>(map.count / kDeviceBlockSize),
                                    .dev_offset = map.target / kDeviceBlockSize};

    ASSERT_EQ(client->FifoTransaction(&request, 1), ZX_OK)
        << "length=" << request.length << ", dev_offset=" << request.dev_offset;
  }

  client.reset();

  // Now try and attach FVM.
  auto result = AttachFvm(ram_disk_or.value().path());
  ASSERT_TRUE(result.is_ok()) << result.status_string();

  fs_management::PartitionMatcher matcher{
      .type_guids = {GUID_DATA_VALUE},
  };

  ASSERT_EQ(
      fs_management::OpenPartition(matcher, zx::duration::infinite().get(), nullptr).status_value(),
      ZX_OK);

  // Attempt to fsck minfs.
  {
    std::string path;
    ASSERT_EQ(
        fs_management::OpenPartition(matcher, zx::duration::infinite().get(), &path).status_value(),
        ZX_OK);

    // And finally run fsck on the volume.
    fs_management::FsckOptions options{
        .verbose = false,
        .never_modify = true,
        .always_modify = false,
        .force = true,
    };
    EXPECT_EQ(fs_management::Fsck(path, fs_management::kDiskFormatMinfs, options,
                                  fs_management::LaunchStdioSync),
              ZX_OK);
  }

  // Attempt to fsck blobfs.
  {
    std::string path;
    fs_management::PartitionMatcher matcher{
        .type_guids = {GUID_BLOB_VALUE},
    };
    ASSERT_EQ(
        fs_management::OpenPartition(matcher, zx::duration::infinite().get(), &path).status_value(),
        ZX_OK);

    // And finally run fsck on the volume.
    fs_management::FsckOptions options{
        .verbose = false,
        .never_modify = true,
        .always_modify = false,
        .force = true,
        .component_child_name = "test-blobfs",
        .component_collection_name = "fs-collection",
    };
    EXPECT_EQ(fs_management::Fsck(path, fs_management::kDiskFormatBlobfs, options,
                                  fs_management::LaunchStdioSync),
              ZX_OK);
  }
}

class NullWriter : public Writer {
 public:
  fpromise::result<void, std::string> Write(uint64_t offset,
                                            cpp20::span<const uint8_t> buffer) override {
    return fpromise::ok();
  }
};

TEST(FvmSparseImageReaderTest, ImageWithMaxSizeAllocatesEnoughMetadata) {
  auto base_reader_or = FdReader::Create(sparse_image_path);
  ASSERT_TRUE(base_reader_or.is_ok()) << base_reader_or.error();

  fvm::SparseImage image = {};
  auto image_stream = cpp20::span<uint8_t>(reinterpret_cast<uint8_t*>(&image), sizeof(image));
  auto read_result = base_reader_or.value().Read(0, image_stream);
  ASSERT_TRUE(read_result.is_ok()) << read_result.error();
  ASSERT_EQ(image.magic, fvm::kSparseFormatMagic);

  auto sparse_image_or = OpenSparseImage(base_reader_or.value(), 300 << 20);
  ASSERT_TRUE(sparse_image_or.is_ok()) << sparse_image_or.error();
  auto sparse_image = sparse_image_or.take_value();

  auto expected_header =
      fvm::Header::FromDiskSize(fvm::kMaxUsablePartitions, 300 << 20, image.slice_size);

  fvm::Header header = {};
  auto header_stream = cpp20::span<uint8_t>(reinterpret_cast<uint8_t*>(&header), sizeof(header));
  read_result = sparse_image.reader()->Read(
      sparse_image.reader()->length() - expected_header.GetMetadataAllocatedBytes(), header_stream);
  ASSERT_TRUE(read_result.is_ok()) << read_result.error();
  ASSERT_EQ(header.magic, fvm::kMagic);

  EXPECT_EQ(expected_header.GetMetadataAllocatedBytes(), header.GetMetadataAllocatedBytes());
}

// This doesn't test that the resulting image is valid, but it least tests that FtlImageWrite can
// consume the sparse image without complaining.
TEST(FvmSparseImageReaderTest, WriteFtlImageSucceeds) {
  auto base_reader_or = FdReader::Create(sparse_image_path);
  ASSERT_TRUE(base_reader_or.is_ok()) << base_reader_or.error();
  auto sparse_image_or = OpenSparseImage(base_reader_or.value(), std::nullopt);
  ASSERT_TRUE(sparse_image_or.is_ok()) << sparse_image_or.error();

  constexpr int kFtlPageSize = 8192;
  NullWriter writer;
  auto result = FtlImageWrite(
      RawNandOptions{
          .page_size = kFtlPageSize,
          .page_count = static_cast<uint32_t>(sparse_image_or.value().volume().size / kFtlPageSize),
          .pages_per_block = 32,
          .oob_bytes_size = 16,
      },
      sparse_image_or.value(), &writer);
  EXPECT_TRUE(result.is_ok()) << result.error();
}

}  // namespace
}  // namespace storage::volume_image
