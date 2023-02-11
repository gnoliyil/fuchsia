// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/mount.h>
#include <unistd.h>

#include <fstream>
#include <iostream>

#include <gtest/gtest.h>

#include "src/proc/tests/chromiumos/syscalls/proc_test.h"
#include "src/proc/tests/chromiumos/syscalls/syscall_matchers.h"
#include "src/proc/tests/chromiumos/syscalls/test_helper.h"

namespace {

using ::testing::IsSupersetOf;
using ::testing::UnorderedElementsAreArray;

/// Unmount anything mounted at or under path.
void RecursiveUnmount(const char *path) {
  int dir_fd = open(path, O_DIRECTORY | O_NOFOLLOW);
  if (dir_fd >= 0) {
    // Grab the entries first because having the directory open to enumerate it may cause a umount
    // to fail with EBUSY
    DIR *dir = fdopendir(dir_fd);
    std::vector<std::string> entries;
    while (struct dirent *entry = readdir(dir)) {
      entries.push_back(entry->d_name);
    }
    closedir(dir);
    for (auto &entry : entries) {
      if (entry == "." || entry == "..")
        continue;
      std::string subpath = path;
      subpath.append("/");
      subpath.append(entry);
      RecursiveUnmount(subpath.c_str());
    }
  }
  // Repeatedly call umount to handle shadowed mounts properly.
  do {
    errno = 0;
    ASSERT_THAT(umount(path), AnyOf(SyscallSucceeds(), SyscallFailsWithErrno(EINVAL))) << path;
  } while (errno != EINVAL);
}

static bool skip_mount_tests = false;

class MountTest : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    int rv = unshare(CLONE_NEWNS);
    if (rv == -1 && errno == EPERM) {
      // GTest does not support GTEST_SKIP() from a suite setup, so record that we want to skip
      // every test here and skip in SetUp().
      skip_mount_tests = true;
      return;
    }
    ASSERT_EQ(rv, 0) << "unshare(CLONE_NEWNS) failed: " << strerror(errno) << "(" << errno << ")";
  }

  void SetUp() override {
    if (skip_mount_tests) {
      GTEST_SKIP() << "Permission denied for unshare(CLONE_NEWNS), skipping suite.";
    }
    tmp_ = get_tmp_path() + "/mounttest";
    mkdir(tmp_.c_str(), 0777);
    RecursiveUnmount(tmp_.c_str());
    ASSERT_THAT(mount(nullptr, tmp_.c_str(), "tmpfs", 0, nullptr), SyscallSucceeds());

    MakeOwnMount("1");
    MakeDir("1/1");
    MakeOwnMount("2");
    MakeDir("2/2");

    ASSERT_TRUE(FileExists("1/1"));
    ASSERT_TRUE(FileExists("2/2"));
  }

  void TearDown() override {
    // Clean up after the tests so that other tests that care about mount state
    // don't fail.
    RecursiveUnmount(tmp_.c_str());
  }

  /// All paths used in test functions are relative to the temp directory. This function makes the
  /// path absolute.
  std::string TestPath(const char *path) const { return tmp_ + "/" + path; }

  // Create a directory.
  int MakeDir(const char *name) const {
    auto path = TestPath(name);
    return mkdir(path.c_str(), 0777);
  }

  /// Make the directory into a bind mount of itself.
  int MakeOwnMount(const char *name) const {
    int err = MakeDir(name);
    if (err < 0)
      return err;
    return Mount(name, name, MS_BIND);
  }

  // Call mount with a null fstype and data.
  int Mount(const char *src, const char *target, int flags) const {
    return mount(src == nullptr ? nullptr : TestPath(src).c_str(), TestPath(target).c_str(),
                 nullptr, flags, nullptr);
  }

  ::testing::AssertionResult FileExists(const char *name) const {
    auto path = TestPath(name);
    if (access(path.c_str(), F_OK) != 0)
      return ::testing::AssertionFailure() << path << ": " << strerror(errno);
    return ::testing::AssertionSuccess();
  }

 private:
  std::string tmp_;
};

[[maybe_unused]] void DumpMountinfo() {
  int fd = open("/proc/self/mountinfo", O_RDONLY);
  char buf[10000];
  size_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    write(STDOUT_FILENO, buf, n);
  close(fd);
}

#define ASSERT_SUCCESS(call) ASSERT_THAT((call), SyscallSucceeds())

TEST_F(MountTest, RecursiveBind) {
  // Make some mounts
  ASSERT_SUCCESS(MakeDir("a"));
  ASSERT_SUCCESS(Mount("1", "a", MS_BIND));
  ASSERT_SUCCESS(Mount("2", "a/1", MS_BIND));
  ASSERT_TRUE(FileExists("a/1"));
  ASSERT_TRUE(FileExists("a/1/2"));

  // Copy the tree
  ASSERT_SUCCESS(MakeDir("b"));
  ASSERT_SUCCESS(Mount("a", "b", MS_BIND | MS_REC));
  ASSERT_TRUE(FileExists("b/1"));
  ASSERT_TRUE(FileExists("b/1/2"));
}

TEST_F(MountTest, BindIgnoresSharingFlags) {
  ASSERT_SUCCESS(MakeDir("a"));
  // The bind mount should ignore the MS_SHARED flag, so we should end up with non-shared mounts.
  ASSERT_SUCCESS(Mount("1", "a", MS_BIND | MS_SHARED));
  ASSERT_SUCCESS(MakeDir("b"));
  ASSERT_SUCCESS(Mount("a", "b", MS_BIND | MS_SHARED));

  ASSERT_SUCCESS(Mount("2", "a/1", MS_BIND));
  ASSERT_TRUE(FileExists("a/1/2"));
  ASSERT_FALSE(FileExists("b/1/2"));
}

TEST_F(MountTest, BasicSharing) {
  ASSERT_SUCCESS(MakeDir("a"));
  ASSERT_SUCCESS(Mount("1", "a", MS_BIND));
  // Must be done in two steps! MS_BIND | MS_SHARED just ignores the MS_SHARED
  ASSERT_SUCCESS(Mount(nullptr, "a", MS_SHARED));
  ASSERT_SUCCESS(MakeDir("b"));
  ASSERT_SUCCESS(Mount("a", "b", MS_BIND));

  ASSERT_SUCCESS(Mount("2", "a/1", MS_BIND));
  ASSERT_TRUE(FileExists("a/1/2"));
  ASSERT_TRUE(FileExists("b/1/2"));
  ASSERT_FALSE(FileExists("1/1/2"));
}

TEST_F(MountTest, FlagVerification) {
  ASSERT_THAT(Mount(nullptr, "1", MS_SHARED | MS_PRIVATE), SyscallFailsWithErrno(EINVAL));
  ASSERT_THAT(Mount(nullptr, "1", MS_SHARED | MS_NOUSER), SyscallFailsWithErrno(EINVAL));
  ASSERT_THAT(Mount(nullptr, "1", MS_SHARED | MS_SILENT), SyscallSucceeds());
}

// Quiz question B from https://www.kernel.org/doc/Documentation/filesystems/sharedsubtree.txt
TEST_F(MountTest, QuizBRecursion) {
  // Create a hierarchy
  ASSERT_SUCCESS(MakeDir("a"));
  ASSERT_SUCCESS(Mount("1", "a", MS_BIND));
  ASSERT_SUCCESS(Mount("2", "a/1", MS_BIND));

  // Make it shared
  ASSERT_SUCCESS(Mount(nullptr, "a", MS_SHARED | MS_REC));

  // Clone it into itself
  ASSERT_SUCCESS(Mount("a", "a/1/2", MS_BIND | MS_REC));
  ASSERT_TRUE(FileExists("a/1/2/1/2"));
  ASSERT_FALSE(FileExists("a/1/2/1/2/1/2"));
  ASSERT_FALSE(FileExists("a/1/2/1/2/1/2/1/2"));
}

// Quiz question C from https://www.kernel.org/doc/Documentation/filesystems/sharedsubtree.txt
TEST_F(MountTest, QuizCPropagation) {
  ASSERT_SUCCESS(Mount(nullptr, "1", MS_SHARED));
  ASSERT_SUCCESS(MakeDir("1/1/2"));
  ASSERT_SUCCESS(MakeDir("1/1/2/3"));
  ASSERT_SUCCESS(MakeDir("1/1/test"));

  ASSERT_SUCCESS(MakeDir("a"));
  ASSERT_SUCCESS(Mount("1/1", "a", MS_BIND));
  ASSERT_SUCCESS(Mount(nullptr, "1", MS_SLAVE));
  ASSERT_SUCCESS(Mount(nullptr, "1", MS_SHARED));
  ASSERT_SUCCESS(MakeDir("b"));
  ASSERT_SUCCESS(Mount("1/1/2", "b", MS_BIND));
  ASSERT_SUCCESS(Mount(nullptr, "1", MS_SLAVE));

  ASSERT_SUCCESS(Mount("2", "a/test", MS_BIND));
  ASSERT_TRUE(FileExists("1/1/test/2"));
}

TEST_F(MountTest, PropagateOntoMountRoot) {
  ASSERT_SUCCESS(Mount(nullptr, "1", MS_SHARED));
  ASSERT_SUCCESS(MakeDir("1/1/1"));
  ASSERT_SUCCESS(MakeDir("a"));
  ASSERT_SUCCESS(Mount("1/1", "a", MS_BIND));
  // The propagation of this should be equivalent to shadowing the "a" mount.
  ASSERT_SUCCESS(Mount("2", "1/1", MS_BIND));
  ASSERT_TRUE(FileExists("a/2"));
}

TEST_F(MountTest, InheritShared) {
  ASSERT_SUCCESS(MakeDir("a"));
  ASSERT_SUCCESS(Mount("1", "a", MS_BIND));
  ASSERT_SUCCESS(Mount(nullptr, "a", MS_SHARED));
  ASSERT_SUCCESS(Mount("2", "a/1", MS_BIND));
  ASSERT_SUCCESS(MakeDir("b"));
  ASSERT_SUCCESS(Mount("a/1", "b", MS_BIND));
  ASSERT_SUCCESS(Mount("1", "b/2", MS_BIND));
  ASSERT_TRUE(FileExists("a/1/2/1"));
}

TEST_F(MountTest, LotsOfShadowing) {
  ASSERT_SUCCESS(MakeDir("a"));
  ASSERT_SUCCESS(Mount(nullptr, "1", MS_SHARED));
  ASSERT_SUCCESS(Mount("1", "a", MS_BIND));
  ASSERT_SUCCESS(Mount("1", "a", MS_BIND));
  ASSERT_SUCCESS(Mount("1", "a", MS_BIND));
}

// TODO(tbodt): write more tests:
// - A and B are shared, make B downstream, make A private, should now both be private

class ProcMountsTest : public ProcTest {
  // Note that these tests can be affected by those in other suites e.g. a
  // MountTest above that doesn't clean up its mounts may change the value of
  // /proc/mounts observed by these tests. Ideally, we'd run a each suite in a
  // different process (and mount namespace, as is already the case) to minimise
  // the blast radius.

 public:
  std::vector<std::string> read_mounts() {
    std::vector<std::string> ret;

    std::ifstream ifs(proc_path() + "/mounts");
    std::string s;
    while (getline(ifs, s)) {
      ret.push_back(s);
    }
    return ret;
  }

 protected:
  const std::string tmp_ = get_tmp_path();
};

TEST_F(ProcMountsTest, Basic) {
  EXPECT_THAT(read_mounts(), IsSupersetOf({
                                 " / TODO TODO 0 0",
                                 " /dev TODO TODO 0 0",
                                 " /data/tmp TODO TODO 0 0",
                             }));
}

TEST_F(ProcMountsTest, MountAdded) {
  auto before_mounts = read_mounts();

  std::string mp = tmp_ + "/foo";
  ASSERT_THAT(mkdir(mp.c_str(), 0777), SyscallSucceeds());
  ASSERT_THAT(mount(nullptr, mp.c_str(), "tmpfs", 0, nullptr), SyscallSucceeds());

  auto expected_mounts = before_mounts;
  std::string mount = " " + mp + " TODO TODO 0 0";
  expected_mounts.push_back(mount);
  EXPECT_THAT(read_mounts(), UnorderedElementsAreArray(expected_mounts));

  // Clean-up.
  ASSERT_THAT(umount(mp.c_str()), SyscallSucceeds());
  ASSERT_THAT(rmdir(mp.c_str()), SyscallSucceeds());

  EXPECT_THAT(read_mounts(), UnorderedElementsAreArray(before_mounts));
}

}  // namespace
