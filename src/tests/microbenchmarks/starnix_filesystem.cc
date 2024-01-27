// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <lib/syslog/cpp/macros.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fbl/unique_fd.h>
#include <perftest/perftest.h>

namespace {

// Measure the time taken by stat() on the given path.
bool StatTest(perftest::RepeatState* state, const char* path) {
  while (state->KeepRunning()) {
    struct stat st;
    FX_CHECK(stat(path, &st) == 0);
  }
  return true;
}

// Measure the time taken by open()+close() on the given path.
bool OpenTest(perftest::RepeatState* state, const char* path) {
  while (state->KeepRunning()) {
    int fd = open(path, O_RDONLY);
    FX_CHECK(fd >= 0);
    FX_CHECK(close(fd) == 0);
  }
  return true;
}

// Measure the time taken by fstat() on an FD on the given path.
bool FstatTest(perftest::RepeatState* state, const char* path) {
  fbl::unique_fd fd(open(path, O_RDONLY));
  FX_CHECK(fd.is_valid());

  while (state->KeepRunning()) {
    struct stat st;
    FX_CHECK(fstat(fd.get(), &st) == 0);
  }
  return true;
}

void RegisterTests() {
  perftest::RegisterTest("Filesystem_Stat_Ext4", StatTest, "/");
  perftest::RegisterTest("Filesystem_Fstat_Ext4", FstatTest, "/");

  // Test open() times for different filesystem implementations.
  perftest::RegisterTest("Filesystem_Open_Bpf", OpenTest, "/sys/fs/bpf");
  perftest::RegisterTest("Filesystem_Open_DevPts", OpenTest, "/dev/pts");
  perftest::RegisterTest("Filesystem_Open_DevTmpfs", OpenTest, "/dev");
  perftest::RegisterTest("Filesystem_Open_Ext4", OpenTest, "/");
  perftest::RegisterTest("Filesystem_Open_Remotefs", OpenTest, "/data");
  perftest::RegisterTest("Filesystem_Open_Sysfs", OpenTest, "/sys");
  perftest::RegisterTest("Filesystem_Open_Tmpfs", OpenTest, "/tmp");
}
PERFTEST_CTOR(RegisterTests)

}  // namespace
