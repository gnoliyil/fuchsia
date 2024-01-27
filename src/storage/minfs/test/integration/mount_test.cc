// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <fcntl.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/fd.h>
#include <lib/fit/defer.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <utility>

#include <fbl/string.h>
#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <ramdevice-client/ramdisk.h>

#include "src/lib/storage/block_client/cpp/block_device.h"
#include "src/lib/storage/block_client/cpp/remote_block_device.h"
#include "src/lib/storage/fs_management/cpp/admin.h"
#include "src/storage/minfs/format.h"
#include "src/storage/minfs/runner.h"
#include "src/storage/testing/ram_disk.h"

namespace minfs {
namespace {

namespace fio = fuchsia_io;

template <bool repairable>
class MountTestTemplate : public testing::Test {
 public:
  void SetUp() final {
    ramdisk_ = storage::RamDisk::Create(/*block_size=*/512, /*block_count=*/1 << 16).value();

    ramdisk_path_ = ramdisk_->path();
    ASSERT_EQ(fs_management::Mkfs(ramdisk_path_.c_str(), fs_management::kDiskFormatMinfs,
                                  fs_management::LaunchStdioSync, fs_management::MkfsOptions()),
              0);

    // TODO(https://fxbug.dev/112484): this relies on multiplexing.
    zx::result block_channel = ramdisk_->channel();
    ASSERT_TRUE(block_channel.is_ok()) << zx_status_get_string(block_channel.status_value());
    std::unique_ptr<block_client::RemoteBlockDevice> device;
    ASSERT_EQ(block_client::RemoteBlockDevice::Create(std::move(block_channel.value()), &device),
              ZX_OK);
    auto bcache_or = minfs::CreateBcache(std::move(device));
    ASSERT_TRUE(bcache_or.is_ok());
    ASSERT_FALSE(bcache_or->is_read_only);
    bcache_ = std::move(bcache_or->bcache);

    auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    ASSERT_EQ(endpoints.status_value(), ZX_OK);
    root_client_end_ = std::move(endpoints->client);
    root_server_end_ = std::move(endpoints->server);
    ASSERT_EQ(loop_.StartThread("minfs test dispatcher"), ZX_OK);
  }

  void ReadSuperblock(minfs::Superblock* out) {
    fbl::unique_fd fd(open(ramdisk_path_.c_str(), O_RDONLY));
    EXPECT_TRUE(fd);

    EXPECT_EQ(pread(fd.get(), out, sizeof(*out), minfs::kSuperblockStart * minfs::kMinfsBlockSize),
              static_cast<ssize_t>(sizeof(*out)));
  }

  void Unmount() {
    if (unmounted_) {
      return;
    }
    // Unmount the filesystem, thereby terminating the minfs instance.
    auto admin_client = component::ConnectAt<fuchsia_fs::Admin>(root_client_end_.borrow());
    ASSERT_EQ(admin_client.status_value(), ZX_OK);
    EXPECT_EQ(fidl::WireCall(*admin_client)->Shutdown().status(), ZX_OK);
    unmounted_ = true;
  }

  void TearDown() final { Unmount(); }

 protected:
  ramdisk_client_t* ramdisk() const { return ramdisk_->client(); }

  const char* ramdisk_path() const { return ramdisk_path_.c_str(); }

  std::unique_ptr<minfs::Bcache> bcache() { return std::move(bcache_); }

  minfs::MountOptions mount_options() const {
    return minfs::MountOptions{.writability = minfs::Writability::Writable,
                               .verbose = true,
                               .repair_filesystem = repairable,
                               .fvm_data_slices = fs_management::MkfsOptions().fvm_data_slices};
  }

  fidl::UnownedClientEnd<fuchsia_io::Directory> root_client_end() { return root_client_end_; }

  fidl::ClientEnd<fuchsia_io::Directory> clone_root_client_end() {
    auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    EXPECT_EQ(endpoints.status_value(), ZX_OK);
    auto [clone_root_client_end, clone_root_server_end] = std::move(*endpoints);
    ZX_ASSERT(fidl::WireCall(root_client_end())
                  ->Clone(fio::wire::OpenFlags::kCloneSameRights,
                          fidl::ServerEnd<fuchsia_io::Node>(clone_root_server_end.TakeChannel()))
                  .ok());
    return std::move(clone_root_client_end);
  }

  fbl::unique_fd clone_root_as_fd() {
    fidl::ClientEnd<fuchsia_io::Directory> clone_client_end = clone_root_client_end();
    fbl::unique_fd root_fd;
    EXPECT_EQ(
        fdio_fd_create(clone_client_end.TakeChannel().release(), root_fd.reset_and_get_address()),
        ZX_OK);
    EXPECT_TRUE(root_fd.is_valid());
    return root_fd;
  }

  async::Loop& loop() { return loop_; }

  zx_status_t MountAndServe() {
    auto runner = minfs::Runner::Create(loop().dispatcher(), bcache(), mount_options());
    if (runner.is_error()) {
      return runner.error_value();
    }
    zx::result status = runner->ServeRoot(std::move(root_server_end_));
    if (status.is_error()) {
      return status.error_value();
    }
    runner_ = *std::move(runner);
    return ZX_OK;
  }

 private:
  bool unmounted_ = false;
  std::optional<storage::RamDisk> ramdisk_;
  std::string ramdisk_path_;
  std::unique_ptr<minfs::Bcache> bcache_ = nullptr;
  fidl::ClientEnd<fuchsia_io::Directory> root_client_end_;
  fidl::ServerEnd<fuchsia_io::Directory> root_server_end_;
  std::unique_ptr<Runner> runner_;
  async::Loop loop_ = async::Loop(&kAsyncLoopConfigNoAttachToCurrentThread);
};

using MountTest = MountTestTemplate<false>;

TEST_F(MountTest, ServeExportDirectoryExportRootDirectoryEntries) {
  ASSERT_EQ(MountAndServe(), ZX_OK);
  fbl::unique_fd root_fd = clone_root_as_fd();
  ASSERT_TRUE(root_fd.is_valid());

  // Verify that |root_client_end| corresponds to the export directory.
  struct dirent* entry = nullptr;
  fbl::unique_fd dir_fd(dup(root_fd.get()));
  ASSERT_TRUE(dir_fd.is_valid());
  DIR* dir = fdopendir(dir_fd.get());
  ASSERT_NE(dir, nullptr);
  dir_fd.release();
  auto close_dir = fit::defer([&]() { closedir(dir); });

  // Verify that there are exactly two entries, "root" and "diagnostics".
  // TODO(fxbug.dev/34531): Adjust this test accordingly when the admin service is added.
  std::vector<std::string> directory_entries;
  while ((entry = readdir(dir)) != nullptr) {
    if ((strcmp(entry->d_name, ".") != 0) && (strcmp(entry->d_name, "..") != 0)) {
      directory_entries.emplace_back(entry->d_name);
      EXPECT_EQ(entry->d_type, DT_DIR);
    }
  }
  EXPECT_THAT(directory_entries, testing::UnorderedElementsAre("root", "diagnostics"));
}

TEST_F(MountTest, ServeExportDirectoryDisallowFileCreationInExportRoot) {
  ASSERT_EQ(MountAndServe(), ZX_OK);
  fbl::unique_fd root_fd = clone_root_as_fd();
  ASSERT_TRUE(root_fd.is_valid());

  // Adding a file is disallowed here...
  fbl::unique_fd foo_fd(openat(root_fd.get(), "foo", O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR));
  EXPECT_FALSE(foo_fd.is_valid());
}

TEST_F(MountTest, ServeExportDirectoryAllowFileCreationInDataRoot) {
  ASSERT_EQ(MountAndServe(), ZX_OK);
  fbl::unique_fd root_fd = clone_root_as_fd();
  ASSERT_TRUE(root_fd.is_valid());

  // Adding a file in "root/" is allowed, since "root/" is within the mutable minfs filesystem.
  fbl::unique_fd foo_fd(
      openat(root_fd.get(), "root/foo", O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR));
  EXPECT_TRUE(foo_fd.is_valid());
}

using RepairableMountTest = MountTestTemplate<true>;

// After successful mount, superblock's clean bit should be cleared and
// persisted to the disk. Reading superblock from raw disk should return cleared
// clean bit.
TEST_F(RepairableMountTest, SyncDuringMount) {
  minfs::Superblock info;
  ReadSuperblock(&info);
  ASSERT_EQ(minfs::kMinfsFlagClean & info.flags, minfs::kMinfsFlagClean);
  ASSERT_EQ(MountAndServe(), ZX_OK);

  // Reading raw device after mount should get us superblock with clean bit
  // unset.
  ReadSuperblock(&info);
  ASSERT_EQ(minfs::kMinfsFlagClean & info.flags, 0u);
}

// After successful unmount, superblock's clean bit should be set and persisted
// to the disk. Reading superblock from raw disk should return set clean bit.
TEST_F(RepairableMountTest, SyncDuringUnmount) {
  minfs::Superblock info;
  ASSERT_EQ(MountAndServe(), ZX_OK);

  // Reading raw device after mount should get us superblock with clean bit
  // unset.
  ReadSuperblock(&info);
  ASSERT_EQ(minfs::kMinfsFlagClean & info.flags, 0u);
  Unmount();

  // Reading raw device after unmount should get us superblock with clean bit
  // set.
  ReadSuperblock(&info);
  ASSERT_EQ(minfs::kMinfsFlagClean & info.flags, minfs::kMinfsFlagClean);
}

}  // namespace
}  // namespace minfs
