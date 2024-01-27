// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_VFS_CPP_TESTING_DIR_TEST_UTIL_H_
#define LIB_VFS_CPP_TESTING_DIR_TEST_UTIL_H_

#include <fuchsia/io/cpp/fidl.h>
#include <lib/fdio/vfs.h>
#include <lib/vfs/cpp/internal/directory.h>

#include "src/lib/testing/loop_fixture/real_loop_fixture.h"

namespace vfs_tests {

class Dirent {
 public:
  static Dirent DirentForDot();

  static Dirent DirentForDirectory(const std::string& name);

  static Dirent DirentForFile(const std::string& name);

  static Dirent DirentForService(const std::string& name);

  std::string String();

  uint64_t ino() const { return ino_; }

  uint8_t type() const { return type_; }

  uint8_t size() const { return size_; }

  const std::string& name() const { return name_; }

  uint64_t size_in_bytes() const { return size_in_bytes_; }

 private:
  Dirent(uint64_t ino, fuchsia::io::DirentType type, const std::string& name);

  uint64_t ino_;
  uint8_t type_;
  uint8_t size_;
  std::string name_;

  uint64_t size_in_bytes_;
};

class DirConnection : public gtest::RealLoopFixture {
 protected:
  virtual vfs::internal::Directory* GetDirectoryNode() = 0;

  void AssertOpen(async_dispatcher_t* dispatcher, fuchsia::io::OpenFlags flags,
                  zx_status_t expected_status, bool test_on_open_event = true);

  void AssertReadDirents(fuchsia::io::DirectorySyncPtr& ptr, uint64_t max_bytes,
                         std::vector<Dirent>& expected_dirents,
                         zx_status_t expected_status = ZX_OK);

  void AssertRewind(fuchsia::io::DirectorySyncPtr& ptr, zx_status_t expected_status = ZX_OK);

  template <typename T>
  void AssertOpenPathImpl(std::string caller_file, int caller_line,
                          fuchsia::io::DirectorySyncPtr& dir_ptr, const std::string& path,
                          ::fidl::SynchronousInterfacePtr<T>& out_sync_ptr,
                          fuchsia::io::OpenFlags flags, fuchsia::io::ModeType mode = {},
                          zx_status_t expected_status = ZX_OK) {
    ::fidl::InterfacePtr<fuchsia::io::Node> node_ptr;
    dir_ptr->Open(flags | fuchsia::io::OpenFlags::DESCRIBE, mode, path, node_ptr.NewRequest());
    bool on_open_called = false;
    node_ptr.events().OnOpen = [&](zx_status_t status,
                                   std::unique_ptr<fuchsia::io::NodeInfoDeprecated> unused) {
      EXPECT_FALSE(on_open_called);  // should be called only once
      on_open_called = true;
      EXPECT_EQ(expected_status, status) << "from file " << caller_file << ", line " << caller_line;
    };

    RunLoopUntil([&]() { return on_open_called; }, zx::msec(1));

    // Bind channel to sync_ptr
    out_sync_ptr.Bind(node_ptr.Unbind().TakeChannel());
  }

  void AssertRead(fuchsia::io::FileSyncPtr& file, int count, const std::string& expected_str,
                  zx_status_t expected_status = ZX_OK);
};

}  // namespace vfs_tests

#define AssertOpenPath(...) AssertOpenPathImpl(__FILE__, __LINE__, __VA_ARGS__)

#endif  // LIB_VFS_CPP_TESTING_DIR_TEST_UTIL_H_
