// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_PROC_TESTS_CHROMIUMOS_SYSCALLS_PROC_TEST_H_
#define SRC_PROC_TESTS_CHROMIUMOS_SYSCALLS_PROC_TEST_H_

#include <sys/mount.h>

#include <gtest/gtest.h>

#include "src/proc/tests/chromiumos/syscalls/syscall_matchers.h"
#include "src/proc/tests/chromiumos/syscalls/test_helper.h"

// Some tests run in environments where /proc is not mounted by default, such as the chromiumos
// galaxy. This harness mounts the proc filesystem to /proc if needed.

class ProcTest : public ::testing::Test {
 public:
  static constexpr char kProc[] = "/proc";

  void SetUp() override {
    // TODO Ideally the test galaxy should just have /proc mounted by default
    // but first the mount point needs to exist in the image
    // (prebuilt/starnix/chromiumos-image-amd64/system.img).
    if (access(kProc, F_OK) != 0) {
      proc_ = get_tmp_path() + kProc;
      ASSERT_THAT(mkdir(proc_.c_str(), 0777), SyscallSucceeds());
      ASSERT_THAT(mount(nullptr, proc_.c_str(), "proc", 0, nullptr), SyscallSucceeds());
      mounted_ = true;
    }
  }

  void TearDown() override {
    if (mounted_) {
      ASSERT_THAT(umount(proc_.c_str()), SyscallSucceeds());
      ASSERT_THAT(rmdir(proc_.c_str()), SyscallSucceeds());
    }
  }

  std::string proc_path() const {
    if (mounted_) {
      return proc_;
    } else {
      return kProc;
    }
  }

 protected:
  bool mounted_ = false;
  std::string proc_;
};

#endif  // SRC_PROC_TESTS_CHROMIUMOS_SYSCALLS_PROC_TEST_H_
