// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.io.admin/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <fuchsia/hardware/block/c/fidl.h>
#include <fuchsia/hardware/block/volume/c/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/namespace.h>
#include <lib/memfs/memfs.h>
#include <lib/syslog/cpp/macros.h>
#include <limits.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zircon/device/block.h>
#include <zircon/device/vfs.h>
#include <zircon/syscalls.h>

#include <iterator>
#include <utility>

#include <fbl/string.h>
#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>
#include <fs-management/fvm.h>
#include <fs-management/mount.h>
#include <gtest/gtest.h>
#include <ramdevice-client/ramdisk.h>

#include "src/lib/fxl/test/test_settings.h"
#include "src/storage/fvm/format.h"
#include "src/storage/testing/fvm.h"
#include "src/storage/testing/ram_disk.h"

namespace {

namespace fio = fuchsia_io;

void CheckMountedFs(const char* path, const char* fs_name, size_t len) {
  fbl::unique_fd fd(open(path, O_RDONLY | O_DIRECTORY));
  ASSERT_TRUE(fd);

  fdio_cpp::FdioCaller caller(std::move(fd));
  auto result = fidl::WireCall(fidl::UnownedClientEnd<fuchsia_io_admin::DirectoryAdmin>(
                                   caller.borrow_channel()))
                    .QueryFilesystem();
  ASSERT_EQ(result.status(), ZX_OK);
  ASSERT_EQ(result->s, ZX_OK);
  fuchsia_io_admin::wire::FilesystemInfo info = *result.value().info;
  ASSERT_EQ(strncmp(fs_name, reinterpret_cast<char*>(info.name.data()), strlen(fs_name)), 0);
  ASSERT_LE(info.used_nodes, info.total_nodes) << "Used nodes greater than free nodes";
  ASSERT_LE(info.used_bytes, info.total_bytes) << "Used bytes greater than free bytes";
  // TODO(planders): eventually check that total/used counts are > 0
}

void MountUnmountShared(size_t block_size) {
  ramdisk_client_t* ramdisk = nullptr;
  const char* mount_path = "/memfs/mount_unmount";

  ASSERT_EQ(ramdisk_create(block_size, 1 << 16, &ramdisk), ZX_OK);
  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  std::cout << std::string(ramdisk_path) << std::endl;

  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);
  ASSERT_EQ(mkdir(mount_path, 0666), 0);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));
  int fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);

  ASSERT_EQ(mount(fd, mount_path, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async), ZX_OK);
  CheckMountedFs(mount_path, "minfs", strlen("minfs"));
  ASSERT_EQ(umount(mount_path), ZX_OK);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));
  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(unlink(mount_path), 0);
}

TEST(MountUnmountCase, MountUnmount) { MountUnmountShared(512); }

TEST(MountUnmountLargeBlockCase, MountUnmountLargeBlock) { MountUnmountShared(8192); }

TEST(MountMkdirUnmountCase, MountMkdirUnmount) {
  const char* mount_path = "/memfs/mount_mkdir_unmount";

  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);

  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);

  int fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);
  MountOptions options;
  options.create_mountpoint = true;
  ASSERT_EQ(mount(fd, mount_path, DISK_FORMAT_MINFS, options, launch_stdio_async), ZX_OK);
  CheckMountedFs(mount_path, "minfs", strlen("minfs"));
  ASSERT_EQ(umount(mount_path), ZX_OK);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));
  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(unlink(mount_path), 0);
}

TEST(FmountFunmountCase, FmountFunmount) {
  const char* mount_path = "/memfs/mount_unmount";

  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);

  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);
  ASSERT_EQ(mkdir(mount_path, 0666), 0);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));

  int fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);

  int mountfd = open(mount_path, O_RDONLY | O_DIRECTORY | O_ADMIN);
  ASSERT_GT(mountfd, 0) << "Couldn't open mount point";

  ASSERT_EQ(fmount(fd, mountfd, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async), ZX_OK);
  CheckMountedFs(mount_path, "minfs", strlen("minfs"));
  ASSERT_EQ(fumount(mountfd), ZX_OK);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));
  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(close(mountfd), 0) << "Couldn't close ex-mount point";
  ASSERT_EQ(unlink(mount_path), 0);
}

// TODO(fxbug.dev/8478): Re-enable once deflaked.
#if 0
// All "parent" filesystems attempt to mount a MinFS ramdisk under malicious
// conditions.
//
// Note: For cases where "fmount" fails, we briefly sleep to allow the
// filesystem to unmount itself and relinquish control of the block device.
void DoMountEvil(const char* parentfs_name, const char* mount_path) {
  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);

  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);
  ASSERT_EQ(mkdir(mount_path, 0666), 0);

  int fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);

  int mountfd = open(mount_path, O_RDONLY | O_DIRECTORY | O_ADMIN);
  ASSERT_GT(mountfd, 0) << "Couldn't open mount point";

  // Everything *would* be perfect to call fmount, when suddenly...
  ASSERT_EQ(rmdir(mount_path), 0);

  // The directory was unlinked! We can't mount now!
  ASSERT_EQ(fmount(fd, mountfd, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async),
            ZX_ERR_NOT_DIR);
  usleep(10000);
  ASSERT_NE(fumount(mountfd), ZX_OK);
  ASSERT_EQ(close(mountfd), 0) << "Couldn't close unlinked not-mount point";

  // Re-acquire the ramdisk mount point; it's always consumed...
  fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);

  // Okay, okay, let's get a new mount path...
  mountfd = open(mount_path, O_CREAT | O_RDWR);
  ASSERT_GT(mountfd, 0);

  // Wait a sec, that was a file, not a directory! We can't mount that!
  ASSERT_EQ(fmount(fd, mountfd, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async),
            ZX_ERR_ACCESS_DENIED);
  usleep(10000);
  ASSERT_NE(fumount(mountfd), ZX_OK);
  ASSERT_EQ(close(mountfd), 0) << "Couldn't close file not-mount point";
  ASSERT_EQ(unlink(mount_path), 0);

  // Re-acquire the ramdisk mount point again...
  fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);
  ASSERT_EQ(mkdir(mount_path, 0666), 0);
  // Try mounting without O_ADMIN (which is disallowed)
  mountfd = open(mount_path, O_RDONLY | O_DIRECTORY);
  ASSERT_GT(mountfd, 0) << "Couldn't open mount point";

  ASSERT_EQ(fmount(fd, mountfd, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async),
            ZX_ERR_ACCESS_DENIED);
  usleep(10000);
  ASSERT_EQ(close(mountfd), 0) << "Couldn't close the unpriviledged mount point";

  // Okay, fine, let's mount successfully...
  fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);
  mountfd = open(mount_path, O_RDONLY | O_DIRECTORY | O_ADMIN);
  ASSERT_GT(mountfd, 0) << "Couldn't open mount point";

  ASSERT_EQ(fmount(fd, mountfd, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async), ZX_OK);
  // Awesome, that worked. But we shouldn't be able to mount again!
  fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);
  ASSERT_EQ(fmount(fd, mountfd, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async),
            ZX_ERR_BAD_STATE);
  usleep(10000);
  CheckMountedFs(mount_path, "minfs", strlen("minfs"));

  // Let's try removing the mount point (we shouldn't be allowed to do so)
  ASSERT_EQ(rmdir(mount_path), -1);
  ASSERT_EQ(errno, EBUSY);

  // Let's try telling the target filesystem to shut down
  // WITHOUT O_ADMIN
  fbl::unique_fd badfd(open(mount_path, O_RDONLY | O_DIRECTORY));
  ASSERT_TRUE(badfd);
  zx_status_t status;
  fdio_cpp::FdioCaller caller(std::move(badfd));
  ASSERT_EQ(fuchsia_io_DirectoryAdminUnmount(caller.borrow_channel(), &status), ZX_OK);
  ASSERT_EQ(status, ZX_ERR_ACCESS_DENIED);
  ASSERT_EQ(close(caller.release().release()), 0);

  // Let's try unmounting the filesystem WITHOUT O_ADMIN
  // (unpinning the remote handle from the parent FS).
  badfd.reset(open(mount_path, O_RDONLY | O_DIRECTORY));
  ASSERT_TRUE(badfd);
  zx_handle_t h;
  caller.reset(std::move(badfd));
  ASSERT_EQ(fuchsia_io_DirectoryAdminUnmountNode(caller.borrow_channel(), &status, &h), ZX_OK);
  ASSERT_EQ(h, ZX_HANDLE_INVALID);
  ASSERT_EQ(status, ZX_ERR_ACCESS_DENIED);
  ASSERT_EQ(close(caller.release().release()), 0);

  // When we unmount with an O_ADMIN handle, it should successfully detach.
  ASSERT_EQ(fumount(mountfd), ZX_OK);
  CheckMountedFs(mount_path, parentfs_name, strlen(parentfs_name));
  ASSERT_EQ(close(mountfd), 0);
  ASSERT_EQ(rmdir(mount_path), 0);
  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
}
#endif

// TODO(fxbug.dev/8478): Re-enable once deflaked.
// TEST(MountEvilMemfsCase, MountEvilMemfs) {
//   const char* mount_path = "/memfs/mount_evil";
//   DoMountEvil("memfs", mount_path);
// }

TEST(UnmountTestEvilCase, UnmountTestEvil) {
  const char* mount_path = "/memfs/umount_test_evil";

  // Create a ramdisk, mount minfs
  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);

  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);

  ASSERT_EQ(mkdir(mount_path, 0666), 0);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));
  int fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);

  ASSERT_EQ(mount(fd, mount_path, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async), ZX_OK);
  CheckMountedFs(mount_path, "minfs", strlen("minfs"));

  // Try re-opening the root without O_ADMIN. We shouldn't be able to umount.
  fbl::unique_fd weak_root_fd(open(mount_path, O_RDONLY | O_DIRECTORY));
  ASSERT_TRUE(weak_root_fd);
  fdio_cpp::FdioCaller caller(std::move(weak_root_fd));
  auto result = fidl::WireCall(fidl::UnownedClientEnd<fuchsia_io_admin::DirectoryAdmin>(
                                   caller.borrow_channel()))
                    .Unmount();
  ASSERT_EQ(result.status(), ZX_OK);
  ASSERT_EQ(result->s, ZX_ERR_ACCESS_DENIED);
  weak_root_fd.reset(caller.release().release());

  // Try opening a non-root directory without O_ADMIN. We shouldn't be able
  // to umount.
  ASSERT_EQ(mkdirat(weak_root_fd.get(), "subdir", 0666), 0);
  fbl::unique_fd weak_subdir_fd(openat(weak_root_fd.get(), "subdir", O_RDONLY | O_DIRECTORY));
  ASSERT_TRUE(weak_subdir_fd);
  caller.reset(std::move(weak_subdir_fd));
  auto result2 = fidl::WireCall(fidl::UnownedClientEnd<fuchsia_io_admin::DirectoryAdmin>(
                                    caller.borrow_channel()))
                     .Unmount();
  ASSERT_EQ(result2.status(), ZX_OK);
  ASSERT_EQ(result2->s, ZX_ERR_ACCESS_DENIED);

  // Try opening a new directory with O_ADMIN. It shouldn't open.
  weak_subdir_fd.reset(openat(weak_root_fd.get(), "subdir", O_RDONLY | O_DIRECTORY | O_ADMIN));
  ASSERT_FALSE(weak_subdir_fd);

  // Finally, umount using O_NOREMOTE and acquiring the connection
  // that has "O_ADMIN" set.
  ASSERT_EQ(umount(mount_path), ZX_OK);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));
  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(unlink(mount_path), 0);
}

TEST(MountFailsWithoutAdminCase, MountFailsWithoutAdmin) {
  const char* mount_path = "/memfs/fail_root";

  // Create a ramdisk
  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);

  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);
  ASSERT_EQ(mkdir(mount_path, 0666), 0);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));

  // open the directory without O_ADMIN and try to mount
  int ramdisk_fd = open(ramdisk_path, O_RDWR);
  ASSERT_GE(ramdisk_fd, 0);
  int mount_fd = open(mount_path, O_RDONLY);
  ASSERT_GE(mount_fd, 0);

  ASSERT_NE(fmount(ramdisk_fd, mount_fd, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async),
            ZX_OK);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));
  ASSERT_EQ(close(mount_fd), 0);

  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(rmdir(mount_path), 0);
}

TEST(DoubleMountRootCase, DoubleMountRoot) {
  const char* mount_path = "/memfs/double_mount_root";

  // Create a ramdisk, mount minfs
  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);

  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);

  ASSERT_EQ(mkdir(mount_path, 0666), 0);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));
  int fd = open(ramdisk_path, O_RDWR);
  ASSERT_GE(fd, 0);

  ASSERT_EQ(mount(fd, mount_path, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async), ZX_OK);
  CheckMountedFs(mount_path, "minfs", strlen("minfs"));

  // Create ANOTHER ramdisk, ready to be mounted...
  // Try mounting again on top Minfs' remote root.
  ramdisk_client_t* ramdisk2;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk2), ZX_OK);

  const char* ramdisk_path2 = ramdisk_get_path(ramdisk2);
  ASSERT_EQ(mkfs(ramdisk_path2, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);

  // Try mounting on the mount point (locally; should fail because something is already mounted)
  int mount_fd = open(mount_path, O_RDONLY | O_NOREMOTE | O_ADMIN);
  ASSERT_GE(mount_fd, 0);
  fd = open(ramdisk_path2, O_RDWR);
  ASSERT_GE(fd, 0);

  ASSERT_NE(fmount(fd, mount_fd, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async), ZX_OK);
  ASSERT_EQ(close(mount_fd), 0);

  // Try mounting on the mount root (remote; should fail because MinFS doesn't allow mounting
  // on top of the root directory).
  mount_fd = open(mount_path, O_RDONLY | O_ADMIN);
  ASSERT_GE(mount_fd, 0);
  fd = open(ramdisk_path2, O_RDWR);
  ASSERT_GE(fd, 0);

  ASSERT_NE(fmount(fd, mount_fd, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async), ZX_OK);
  ASSERT_EQ(close(mount_fd), 0);

  ASSERT_EQ(umount(mount_path), ZX_OK);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));
  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(ramdisk_destroy(ramdisk2), 0);
  ASSERT_EQ(rmdir(mount_path), 0);
}

TEST(MountRemountCase, MountRemount) {
  const char* mount_path = "/memfs/mount_remount";

  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);

  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);
  ASSERT_EQ(mkdir(mount_path, 0666), 0);

  // We should still be able to mount and unmount the filesystem multiple times
  for (size_t i = 0; i < 10; i++) {
    int fd = open(ramdisk_path, O_RDWR);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(mount(fd, mount_path, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async), ZX_OK);
    ASSERT_EQ(umount(mount_path), ZX_OK);
  }
  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(unlink(mount_path), 0);
}

TEST(MountFsckCase, MountFsck) {
  const char* mount_path = "/memfs/mount_fsck";

  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);

  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);

  ASSERT_EQ(mkdir(mount_path, 0666), 0);
  int fd = open(ramdisk_path, O_RDWR);
  ASSERT_GE(fd, 0) << "Could not open ramdisk device";

  ASSERT_EQ(mount(fd, mount_path, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async), ZX_OK);
  ASSERT_EQ(umount(mount_path), ZX_OK);

  // Fsck shouldn't require any user input for a newly mkfs'd filesystem.
  ASSERT_EQ(fsck(ramdisk_path, DISK_FORMAT_MINFS, FsckOptions(), launch_stdio_sync), ZX_OK);
  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(unlink(mount_path), 0);
}

TEST(MountGetDeviceCase, MountGetDevice) {
  const char* mount_path = "/memfs/mount_get_device";

  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);

  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);
  ASSERT_EQ(mkdir(mount_path, 0666), 0);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));

  fbl::unique_fd mountfd(open(mount_path, O_RDONLY | O_ADMIN));
  ASSERT_TRUE(mountfd);
  fdio_cpp::FdioCaller caller(std::move(mountfd));
  auto result = fidl::WireCall(fidl::UnownedClientEnd<fuchsia_io_admin::DirectoryAdmin>(
                                   caller.borrow_channel()))
                    .GetDevicePath();
  ASSERT_EQ(result.status(), ZX_OK);
  ASSERT_EQ(result->s, ZX_ERR_NOT_SUPPORTED);

  int fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);

  ASSERT_EQ(mount(fd, mount_path, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async), ZX_OK);
  CheckMountedFs(mount_path, "minfs", strlen("minfs"));

  mountfd.reset(open(mount_path, O_RDONLY | O_ADMIN));
  ASSERT_TRUE(mountfd);
  caller.reset(std::move(mountfd));
  auto result2 = fidl::WireCall(fidl::UnownedClientEnd<fuchsia_io_admin::DirectoryAdmin>(
                                    caller.borrow_channel()))
                     .GetDevicePath();
  ASSERT_EQ(result2.status(), ZX_OK);
  ASSERT_EQ(result2->s, ZX_OK);
  ASSERT_GT(result2.value().path.size(), 0ul) << "Device path not found";
  ASSERT_STREQ(ramdisk_path, result2.value().path.data()) << "Unexpected device path";

  mountfd.reset(open(mount_path, O_RDONLY));
  ASSERT_TRUE(mountfd);
  caller.reset(std::move(mountfd));
  auto result3 = fidl::WireCall(fidl::UnownedClientEnd<fuchsia_io_admin::DirectoryAdmin>(
                                    caller.borrow_channel()))
                     .GetDevicePath();
  ASSERT_EQ(result3.status(), ZX_OK);
  ASSERT_EQ(result3->s, ZX_ERR_ACCESS_DENIED);

  ASSERT_EQ(umount(mount_path), ZX_OK);
  CheckMountedFs(mount_path, "memfs", strlen("memfs"));

  mountfd.reset(open(mount_path, O_RDONLY | O_ADMIN));
  ASSERT_TRUE(mountfd);
  caller.reset(std::move(mountfd));
  auto result4 =
      fidl::WireCall(
          fidl::UnownedClientEnd<fuchsia_io_admin::DirectoryAdmin>(caller.borrow_channel()))
          .GetDevicePath(
              fidl::UnownedClientEnd<fuchsia_io_admin::DirectoryAdmin>(caller.borrow_channel()));
  ASSERT_EQ(result4.status(), ZX_OK);
  ASSERT_EQ(result4->s, ZX_ERR_NOT_SUPPORTED);

  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(unlink(mount_path), 0);
}

// Mounts a minfs formatted partition to the desired point.
void MountMinfs(int block_fd, bool read_only, const char* mount_path) {
  MountOptions options;
  options.readonly = read_only;

  ASSERT_EQ(mount(block_fd, mount_path, DISK_FORMAT_MINFS, options, launch_stdio_async), ZX_OK);
  CheckMountedFs(mount_path, "minfs", strlen("minfs"));
}

// Formats the ramdisk with minfs, and writes a small file to it.
void CreateTestFile(const char* ramdisk_path, const char* mount_path, const char* file_name) {
  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);
  ASSERT_EQ(mkdir(mount_path, 0666), 0);

  int fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);
  MountMinfs(fd, /*read_only=*/false, mount_path);

  int root_fd = open(mount_path, O_RDONLY | O_DIRECTORY);
  ASSERT_GE(root_fd, 0);
  fd = openat(root_fd, file_name, O_CREAT | O_RDWR);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(write(fd, "hello", 6), 6);

  ASSERT_EQ(close(fd), 0);
  ASSERT_EQ(close(root_fd), 0);
  ASSERT_EQ(umount(mount_path), ZX_OK);
}

// Tests that setting read-only on the mount options works as expected.
TEST(MountReadonlyCase, MountReadonly) {
  const char* mount_path = "/memfs/mount_readonly";
  const char file_name[] = "some_file";

  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);
  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  CreateTestFile(ramdisk_path, mount_path, file_name);

  int fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);

  bool read_only = true;
  MountMinfs(fd, read_only, mount_path);

  int root_fd = open(mount_path, O_RDONLY | O_DIRECTORY);
  ASSERT_GE(root_fd, 0);
  fd = openat(root_fd, file_name, O_CREAT | O_RDWR);

  // We can no longer open the file as writable
  ASSERT_LT(fd, 0);

  // We CAN open it as readable though
  fd = openat(root_fd, file_name, O_RDONLY);
  ASSERT_GT(fd, 0);
  ASSERT_LT(write(fd, "hello", 6), 0);
  char buf[6];
  ASSERT_EQ(read(fd, buf, 6), 6);
  ASSERT_EQ(memcmp(buf, "hello", 6), 0);

  ASSERT_LT(renameat(root_fd, file_name, root_fd, "new_file"), 0);
  ASSERT_LT(unlinkat(root_fd, file_name, 0), 0);

  ASSERT_EQ(close(fd), 0);
  ASSERT_EQ(close(root_fd), 0);
  ASSERT_EQ(umount(mount_path), ZX_OK);

  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(unlink(mount_path), 0);
}

// Test that when a block device claims to be read-only, the filesystem is mounted as read-only.
TEST(MountBlockReadonlyCase, MountBlockReadonly) {
  const char* mount_path = "/memfs/mount_readonly";
  const char file_name[] = "some_file";

  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);
  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  CreateTestFile(ramdisk_path, mount_path, file_name);

  uint32_t flags = BLOCK_FLAG_READONLY;
  ASSERT_EQ(ramdisk_set_flags(ramdisk, flags), ZX_OK);

  bool read_only = false;
  MountMinfs(ramdisk_get_block_fd(ramdisk), read_only, mount_path);

  // We can't modify the file.
  int root_fd = open(mount_path, O_RDONLY | O_DIRECTORY);
  ASSERT_GE(root_fd, 0);
  int fd = openat(root_fd, file_name, O_CREAT | O_RDWR);
  ASSERT_LT(fd, 0);

  // We can open it as read-only.
  fd = openat(root_fd, file_name, O_RDONLY);
  ASSERT_GT(fd, 0);
  ASSERT_EQ(close(fd), 0);
  ASSERT_EQ(close(root_fd), 0);
  ASSERT_EQ(umount(mount_path), ZX_OK);

  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(unlink(mount_path), 0);
}

TEST(StatfsTestCase, StatfsTest) {
  const char* mount_path = "/memfs/mount_unmount";

  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);

  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);

  ASSERT_EQ(mkdir(mount_path, 0666), 0);
  int fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);

  ASSERT_EQ(mount(fd, mount_path, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async), ZX_OK);

  struct statfs stats;
  int rc = statfs("", &stats);
  int err = errno;
  ASSERT_EQ(rc, -1);
  ASSERT_EQ(err, ENOENT);

  rc = statfs(mount_path, &stats);
  ASSERT_EQ(rc, 0);

  // Verify that at least some values make sense, without making the test too brittle.
  ASSERT_EQ(stats.f_type, VFS_TYPE_MINFS);
  ASSERT_NE(stats.f_fsid.__val[0] | stats.f_fsid.__val[1], 0);
  ASSERT_EQ(stats.f_bsize, 8192u);
  ASSERT_EQ(stats.f_namelen, 255u);
  ASSERT_GT(stats.f_bavail, 0u);
  ASSERT_GT(stats.f_ffree, 0u);

  ASSERT_EQ(umount(mount_path), ZX_OK);
  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(unlink(mount_path), 0);
}

TEST(StatvfsTestCase, StatvfsTest) {
  const char* mount_path = "/memfs/mount_unmount";

  ramdisk_client_t* ramdisk = nullptr;
  ASSERT_EQ(ramdisk_create(512, 1 << 16, &ramdisk), ZX_OK);

  const char* ramdisk_path = ramdisk_get_path(ramdisk);
  ASSERT_EQ(mkfs(ramdisk_path, DISK_FORMAT_MINFS, launch_stdio_sync, MkfsOptions()), ZX_OK);

  ASSERT_EQ(mkdir(mount_path, 0666), 0);
  int fd = open(ramdisk_path, O_RDWR);
  ASSERT_GT(fd, 0);

  ASSERT_EQ(mount(fd, mount_path, DISK_FORMAT_MINFS, MountOptions(), launch_stdio_async), ZX_OK);

  struct statvfs stats;
  int rc = statvfs("", &stats);
  int err = errno;
  ASSERT_EQ(rc, -1);
  ASSERT_EQ(err, ENOENT);

  rc = statvfs(mount_path, &stats);
  ASSERT_EQ(rc, 0);

  // Verify that at least some values make sense, without making the test too brittle.
  ASSERT_NE(stats.f_fsid, 0ul);
  ASSERT_EQ(stats.f_bsize, 8192u);
  ASSERT_EQ(stats.f_frsize, 8192u);
  ASSERT_EQ(stats.f_namemax, 255u);
  ASSERT_GT(stats.f_bavail, 0u);
  ASSERT_GT(stats.f_ffree, 0u);
  ASSERT_GT(stats.f_favail, 0u);

  ASSERT_EQ(umount(mount_path), ZX_OK);
  ASSERT_EQ(ramdisk_destroy(ramdisk), 0);
  ASSERT_EQ(unlink(mount_path), 0);
}

void GetPartitionSliceCount(const zx::unowned_channel& channel, size_t* out_count) {
  fuchsia_hardware_block_volume_VolumeInfo fvm_info;
  zx_status_t status;
  ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuery(channel->get(), &status, &fvm_info), ZX_OK);
  ASSERT_EQ(status, ZX_OK);

  size_t allocated_slices = 0;
  uint64_t start_slices[1];
  start_slices[0] = 0;
  while (start_slices[0] < fvm_info.vslice_count) {
    fuchsia_hardware_block_volume_VsliceRange
        ranges[fuchsia_hardware_block_volume_MAX_SLICE_REQUESTS];
    size_t actual_ranges_count;
    ASSERT_EQ(fuchsia_hardware_block_volume_VolumeQuerySlices(channel->get(), start_slices,
                                                              std::size(start_slices), &status,
                                                              ranges, &actual_ranges_count),
              ZX_OK);
    ASSERT_EQ(status, ZX_OK);
    ASSERT_EQ(actual_ranges_count, 1ul);
    start_slices[0] += ranges[0].count;
    if (ranges[0].allocated) {
      allocated_slices += ranges[0].count;
    }
  }

  *out_count = allocated_slices;
}

class PartitionOverFvmWithRamdiskFixture : public testing::Test {
 public:
  const char* partition_path() const { return partition_path_.c_str(); }

 protected:
  static constexpr uint64_t kBlockSize = 512;

  void SetUp() override {
    size_t ramdisk_block_count = zx_system_get_physmem() / (1024);
    ASSERT_EQ(ramdisk_create(kBlockSize, ramdisk_block_count, &ramdisk_), ZX_OK);

    std::string ramdisk_path(ramdisk_get_path(ramdisk_));
    uint64_t slice_size = kBlockSize * (2 << 10);

    fbl::unique_fd ramdisk_fd(open(ramdisk_path.c_str(), O_RDWR));
    ASSERT_TRUE(ramdisk_fd.is_valid()) << strerror(errno);

    storage::FvmOptions options;
    options.name = "my-fake-partition";
    options.type = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};

    auto partition_or = storage::CreateFvmPartition(ramdisk_path, slice_size, options);
    ASSERT_TRUE(partition_or.is_ok()) << partition_or.status_string();
    partition_path_ = std::move(partition_or).value();
  }

  void TearDown() override {
    if (ramdisk_ != nullptr) {
      ramdisk_destroy(ramdisk_);
    }
  }

 private:
  ramdisk_client_t* ramdisk_ = nullptr;
  std::string partition_path_;
};  // namespace

using PartitionOverFvmWithRamdiskCase = PartitionOverFvmWithRamdiskFixture;

// Reformat the partition using a number of slices and verify that there are as many slices as
// originally pre-allocated.
TEST_F(PartitionOverFvmWithRamdiskCase, MkfsMinfsWithMinFvmSlices) {
  MkfsOptions options;
  size_t base_slices = 0;
  ASSERT_EQ(mkfs(partition_path(), DISK_FORMAT_MINFS, launch_stdio_sync, options), ZX_OK);
  fbl::unique_fd partition_fd(open(partition_path(), O_RDONLY));
  ASSERT_TRUE(partition_fd);
  fdio_cpp::UnownedFdioCaller caller(partition_fd.get());
  GetPartitionSliceCount(zx::unowned_channel(caller.borrow_channel()), &base_slices);
  options.fvm_data_slices += 10;

  ASSERT_EQ(mkfs(partition_path(), DISK_FORMAT_MINFS, launch_stdio_sync, options), ZX_OK);
  size_t allocated_slices = 0;
  GetPartitionSliceCount(zx::unowned_channel(caller.borrow_channel()), &allocated_slices);
  EXPECT_GE(allocated_slices, base_slices + 10);

  disk_format_t actual_format = detect_disk_format(partition_fd.get());
  ASSERT_EQ(actual_format, DISK_FORMAT_MINFS);
}

zx_status_t MountMemFs(async::Loop* loop, memfs_filesystem_t** memfs_out) {
  zx_status_t result = ZX_OK;
  result = loop->StartThread("mountest-test-memfs");
  if (result != ZX_OK) {
    std::cout << "Failed to start serving thread for MemFs." << std::endl;
    return result;
  }

  return memfs_install_at(loop->dispatcher(), "/memfs", memfs_out);
}

zx_status_t UnmountMemFs(memfs_filesystem_t* memfs) {
  return memfs_uninstall_unsafe(memfs, "/memfs");
}

int RunWithMemFs(const fit::function<int()>& main_fn) {
  memfs_filesystem_t* fs;
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  zx_status_t status;
  if ((status = MountMemFs(&loop, &fs)) != ZX_OK) {
    std::cout << "Failed to mount memfs" << std::endl;
    return -1;
  }
  int result = main_fn();
  loop.Shutdown();
  if ((status = UnmountMemFs(fs)) != ZX_OK) {
    std::cout << "Failed to unmount memfs" << std::endl;
    return -1;
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (!fxl::SetTestSettings(argc, argv)) {
    FX_LOGS(ERROR) << "Failed to parse log settings from command-line";
    return EXIT_FAILURE;
  }
  testing::InitGoogleTest(&argc, argv);
  auto status = storage::WaitForRamctl();
  if (status.is_error()) {
    return EXIT_FAILURE;
  }

  // Memfs path is used for allowing mount operations, with are not allowed on local namespace.
  return RunWithMemFs([]() { return RUN_ALL_TESTS(); });
}
