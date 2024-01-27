// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <fuchsia/hardware/block/driver/c/banjo.h>
#include <lib/fdio/spawn.h>
#include <lib/zx/job.h>
#include <lib/zx/process.h>

#include <fbl/unique_fd.h>
#include <gtest/gtest.h>

#include "src/lib/storage/fs_management/cpp/format.h"
#include "src/lib/storage/fs_management/cpp/launch.h"
#include "src/lib/storage/fs_management/cpp/mount.h"
#include "src/storage/testing/ram_disk.h"

namespace factoryfs {
namespace {

TEST(FactoryFs, ExportedFilesystemIsMountable) {
  constexpr int kDeviceBlockSize = 4096;
  constexpr int kBlockCount = 1024;

  auto ram_disk_or = storage::RamDisk::Create(kDeviceBlockSize, kBlockCount);
  ASSERT_TRUE(ram_disk_or.is_ok()) << ram_disk_or.status_string();

  char staging_path[] = "/tmp/factoryfs.XXXXXX";
  ASSERT_NE(mkdtemp(staging_path), nullptr);
  constexpr char kMountPath[] = "/test/factoryfs";

  constexpr char hello[] = "hello";
  constexpr char foo[] = "foo";
  constexpr char bar[] = "foo/bar";

  fbl::unique_fd staging(open(staging_path, O_RDONLY));
  ASSERT_TRUE(staging);

  {
    fbl::unique_fd fd(openat(staging.get(), hello, O_CREAT | O_RDWR, 0777));
    ASSERT_TRUE(fd) << errno;
    ASSERT_EQ(write(fd.get(), "world", 5), 5);
    ASSERT_EQ(mkdirat(staging.get(), foo, 0777), 0);
  }
  {
    fbl::unique_fd fd(openat(staging.get(), bar, O_CREAT | O_RDWR, 0777));
    ASSERT_TRUE(fd) << errno;
    ASSERT_EQ(write(fd.get(), "bar", 3), 3);
  }

  std::string ram_disk_path = ram_disk_or.value().path();
  const char *argv[] = {"/pkg/bin/export-ffs", staging_path, ram_disk_path.c_str(), nullptr};
  zx::process process;
  zx_status_t status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, argv[0], argv,
                                  process.reset_and_get_address());
  ASSERT_EQ(status, ZX_OK);

  status = process.wait_one(ZX_PROCESS_TERMINATED, zx::time::infinite(), nullptr);
  ASSERT_EQ(status, ZX_OK);

  // Now try and mount Factoryfs.
  ASSERT_EQ(ramdisk_set_flags(ram_disk_or.value().client(), BLOCK_FLAG_READONLY), ZX_OK);

  zx::result device = ram_disk_or.value().channel();
  ASSERT_EQ(device.status_value(), ZX_OK);

  auto result =
      fs_management::Mount(std::move(device.value()), fs_management::kDiskFormatFactoryfs,
                           fs_management::MountOptions(), fs_management::LaunchStdioAsync);
  ASSERT_EQ(result.status_value(), ZX_OK);
  auto data = result->DataRoot();
  ASSERT_EQ(data.status_value(), ZX_OK);
  auto binding = fs_management::NamespaceBinding::Create(kMountPath, std::move(*data));
  ASSERT_EQ(binding.status_value(), ZX_OK);

  // And check contents of factoryfs.
  fbl::unique_fd factoryfs(open(kMountPath, O_RDONLY));
  ASSERT_TRUE(factoryfs);
  char buf[11];
  {
    fbl::unique_fd fd(openat(factoryfs.get(), hello, O_RDONLY));
    ASSERT_TRUE(fd) << errno;
    EXPECT_EQ(read(fd.get(), buf, sizeof(buf)), 5);
    EXPECT_EQ(memcmp(buf, "world", 5), 0);
  }
  {
    fbl::unique_fd fd(openat(factoryfs.get(), bar, O_RDONLY));
    ASSERT_TRUE(fd) << errno;
    EXPECT_EQ(read(fd.get(), buf, sizeof(buf)), 3) << errno;
    EXPECT_EQ(memcmp(buf, "bar", 3), 0);
  }
}

}  // namespace
}  // namespace factoryfs
