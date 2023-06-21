// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STARNIX_TESTS_SYSCALLS_PROC_TEST_H_
#define SRC_STARNIX_TESTS_SYSCALLS_PROC_TEST_H_

#include <fcntl.h>
#include <poll.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#include <gtest/gtest.h>
#include <linux/magic.h>

#include "src/starnix/tests/syscalls/syscall_matchers.h"
#include "src/starnix/tests/syscalls/test_helper.h"

#define OK_OR_RETURN(x) \
  {                     \
    auto result = (x);  \
    if (!result) {      \
      return result;    \
    }                   \
  }

constexpr char kOverlayFsPath[] = "OVERLAYFS_PATH";

class FuseTest : public ::testing::Test {
 public:
  void SetUp() override {}

  void TearDown() override {
    if (base_dir_) {
      if (umount(GetMountDir().c_str()) != 0) {
        FAIL() << "Unable to umount: " << strerror(errno);
      }
      base_dir_.reset();
    }
  }

 protected:
  std::string GetOverlayFsPath() {
    if (getenv(kOverlayFsPath)) {
      return getenv(kOverlayFsPath);
    } else {
      return "/data/fuse-overlayfs";
    }
  }

  std::string GetMountDir() { return *base_dir_ + "/merge"; }

  testing::AssertionResult MkDir(const std::string& directory) {
    if (mkdir(directory.c_str(), 0700) != 0) {
      return testing::AssertionFailure()
             << "Unable to create '" << directory << "': " << strerror(errno);
    }
    return testing::AssertionSuccess();
  }

  testing::AssertionResult Mount() {
    if (getuid() != 0) {
      return testing::AssertionFailure()
             << "Unable to run without CAP_SYS_ADMIN (please run as root)";
    }
    if (access(GetOverlayFsPath().c_str(), R_OK | X_OK) != 0) {
      return testing::AssertionFailure()
             << "Unable to find fuse binary at: " << GetOverlayFsPath() << "(set OVERLAYFS_PATH)";
    }

    std::string base_dir = "/tmp/fuse_base_dir_XXXXXX";
    if (mkdtemp(const_cast<char*>(base_dir.c_str())) == nullptr) {
      return testing::AssertionFailure()
             << "Unable to create temporary directory: " << strerror(errno);
    }
    std::string lowerdir = base_dir + "/lower";
    OK_OR_RETURN(MkDir(lowerdir));
    std::string upperdir = base_dir + "/upper";
    OK_OR_RETURN(MkDir(upperdir));
    std::string workdir = base_dir + "/work";
    OK_OR_RETURN(MkDir(workdir));
    std::string mergedir = base_dir + "/merge";
    OK_OR_RETURN(MkDir(mergedir));
    std::string witness_name = "witness";
    {
      ScopedFD witness(open((lowerdir + "/" + witness_name).c_str(), O_RDWR | O_CREAT, 0600));
      if (!witness.is_valid()) {
        return testing::AssertionFailure() << "Unable to create witness file: " << strerror(errno);
      }
      if (write(witness.get(), "hello\n", 6) != 6) {
        return testing::AssertionFailure()
               << "Unable to insert data in witness file: " << strerror(errno);
      }
    }
    pid_t child_pid = fork_helper_.RunInForkedProcess([&] {
      std::string configuration =
          "lowerdir=" + lowerdir + ",upperdir=" + upperdir + ",workdir=" + workdir;
      execl(GetOverlayFsPath().c_str(), GetOverlayFsPath().c_str(), "-f", "-o",
            configuration.c_str(), mergedir.c_str(), NULL);
    });
    if (child_pid <= 0) {
      return testing::AssertionFailure() << "Unable to fork to start the fuse server process";
    }
    base_dir_ = base_dir;

    std::string witness = mergedir + "/" + witness_name;
    for (int i = 0; i < 20 && access(witness.c_str(), R_OK) != 0; ++i) {
      usleep(100000);
    }
    if (access(witness.c_str(), R_OK) != 0) {
      return testing::AssertionFailure() << "Unable to see witness file. Mount failed?";
    }
    return testing::AssertionSuccess();
  }

  ForkHelper fork_helper_;
  std::optional<std::string> base_dir_;
};

TEST_F(FuseTest, Mount) { ASSERT_TRUE(Mount()); }

TEST_F(FuseTest, Stats) {
  ASSERT_TRUE(Mount());
  std::string mounted_witness = GetMountDir() + "/witness";
  std::string original_witness = *base_dir_ + "/lower/witness";
  ScopedFD fd(open(mounted_witness.c_str(), O_RDONLY));
  ASSERT_TRUE(fd.is_valid());
  struct stat mounted_stats;
  ASSERT_EQ(fstat(fd.get(), &mounted_stats), 0);
  fd = ScopedFD(open(original_witness.c_str(), O_RDONLY));
  ASSERT_TRUE(fd.is_valid());
  struct stat original_stats;
  ASSERT_EQ(fstat(fd.get(), &original_stats), 0);
  fd.reset();

  // Check that the stat of the mounted file are the same as the origin one,
  // except for the fs id.
  ASSERT_NE(mounted_stats.st_dev, original_stats.st_dev);
  // Clobber st_dev and check the rest of the data is the same.
  mounted_stats.st_dev = 0;
  original_stats.st_dev = 0;
  ASSERT_EQ(memcmp(&mounted_stats, &original_stats, sizeof(struct stat)), 0);
}

TEST_F(FuseTest, Read) {
  ASSERT_TRUE(Mount());
  std::string mounted_witness = GetMountDir() + "/witness";
  ScopedFD fd(open(mounted_witness.c_str(), O_RDONLY));
  ASSERT_TRUE(fd.is_valid());
  char buffer[100];
  ASSERT_EQ(read(fd.get(), buffer, 100), 6);
  ASSERT_EQ(strncmp(buffer, "hello\n", 6), 0);
}

TEST_F(FuseTest, NoFile) {
  ASSERT_TRUE(Mount());
  std::string mounted_witness = GetMountDir() + "/unexistent";
  ScopedFD fd(open(mounted_witness.c_str(), O_RDONLY));
  ASSERT_FALSE(fd.is_valid());
  ASSERT_EQ(errno, ENOENT);
}

TEST_F(FuseTest, Mknod) {
  ASSERT_TRUE(Mount());
  std::string filename = GetMountDir() + "/file";
  ScopedFD fd(open(filename.c_str(), O_WRONLY | O_CREAT));
  ASSERT_TRUE(fd.is_valid());
}

TEST_F(FuseTest, Write) {
  ASSERT_TRUE(Mount());
  std::string filename = GetMountDir() + "/file";
  ScopedFD fd(open(filename.c_str(), O_WRONLY | O_CREAT));
  ASSERT_TRUE(fd.is_valid());
  EXPECT_EQ(write(fd.get(), "hello\n", 6), 6);
}

TEST_F(FuseTest, Statfs) {
  ASSERT_TRUE(Mount());
  struct statfs stats;
  ASSERT_EQ(statfs((GetMountDir() + "/witness").c_str(), &stats), 0);
  ASSERT_EQ(stats.f_type, FUSE_SUPER_MAGIC);
}

TEST_F(FuseTest, Seek) {
  ASSERT_TRUE(Mount());
  ScopedFD fd(open((GetMountDir() + "/witness").c_str(), O_RDONLY));
  ASSERT_TRUE(fd.is_valid());
  char buffer[100];
  ASSERT_EQ(read(fd.get(), buffer, 100), 6);
  ASSERT_EQ(strncmp(buffer, "hello\n", 6), 0);
  ASSERT_EQ(lseek(fd.get(), 0, SEEK_CUR), 6);
  ASSERT_EQ(lseek(fd.get(), -5, SEEK_END), 1);
  ASSERT_EQ(read(fd.get(), buffer, 100), 5);
  ASSERT_EQ(strncmp(buffer, "ello\n", 5), 0);

  ASSERT_EQ(lseek(fd.get(), 1, SEEK_DATA), 1);
  ASSERT_EQ(lseek(fd.get(), 7, SEEK_DATA), -1);
  ASSERT_EQ(errno, ENXIO);
  ASSERT_EQ(lseek(fd.get(), 0, SEEK_HOLE), 6);
  ASSERT_EQ(lseek(fd.get(), 7, SEEK_HOLE), -1);
  ASSERT_EQ(errno, ENXIO);
}

TEST_F(FuseTest, Poll) {
  ASSERT_TRUE(Mount());
  ScopedFD fd(open((GetMountDir() + "/witness").c_str(), O_RDONLY));
  ASSERT_TRUE(fd.is_valid());
  struct pollfd poll_struct;
  poll_struct.fd = fd.get();
  poll_struct.events = -1;
  ASSERT_EQ(poll(&poll_struct, 1, 0), 1);
  ASSERT_EQ(poll_struct.revents, POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
}

TEST_F(FuseTest, Mkdir) {
  ASSERT_TRUE(Mount());
  std::string dirname = GetMountDir() + "/dir";
  ASSERT_EQ(open(dirname.c_str(), O_RDONLY), -1);
  ASSERT_EQ(mkdir(dirname.c_str(), 0777), 0);
  ScopedFD fd(open(dirname.c_str(), O_RDONLY));
  ASSERT_TRUE(fd.is_valid());
  struct stat stats;
  ASSERT_EQ(fstat(fd.get(), &stats), 0);
  ASSERT_TRUE(S_ISDIR(stats.st_mode));
}

TEST_F(FuseTest, Symlink) {
  ASSERT_TRUE(Mount());
  std::string witness = GetMountDir() + "/witness";
  std::string link = GetMountDir() + "/symlink";
  ASSERT_EQ(symlink(witness.c_str(), link.c_str()), 0);
  struct stat stats;
  ASSERT_EQ(lstat(link.c_str(), &stats), 0);
  ASSERT_TRUE(S_ISLNK(stats.st_mode));
  std::vector<char> buffer;
  buffer.resize(100);
  ASSERT_EQ(readlink(link.c_str(), &buffer[0], buffer.size()),
            static_cast<ssize_t>(witness.size()));
  buffer.resize(witness.size());
  ASSERT_EQ(memcmp(&buffer[0], &witness[0], witness.size()), 0);
}

TEST_F(FuseTest, Link) {
  ASSERT_TRUE(Mount());
  std::string witness = GetMountDir() + "/witness";
  std::string linkname = GetMountDir() + "/link";
  ASSERT_EQ(link(witness.c_str(), linkname.c_str()), 0);
  struct stat stats;
  ASSERT_EQ(lstat(linkname.c_str(), &stats), 0);
  ASSERT_TRUE(S_ISREG(stats.st_mode));
  ino_t ino = stats.st_ino;
  ASSERT_EQ(lstat(witness.c_str(), &stats), 0);
  ASSERT_EQ(ino, stats.st_ino);
}

#endif  // SRC_STARNIX_TESTS_SYSCALLS_PROC_TEST_H_
