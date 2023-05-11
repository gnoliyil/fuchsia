// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "src/starnix/tests/syscalls/test_helper.h"

namespace {

TEST(PipeTest, NonBlockingPartialWrite) {
  // Allocate 1M that should be bigger than the pipe buffer.
  constexpr ssize_t kBufferSize = 1024 * 1024;

  int pipefd[2];
  SAFE_SYSCALL(pipe2(pipefd, O_NONBLOCK));

  char* buffer = static_cast<char*>(malloc(kBufferSize));
  ASSERT_NE(buffer, nullptr);
  ssize_t write_result = write(pipefd[1], buffer, kBufferSize);
  free(buffer);
  ASSERT_GT(write_result, 0);
  ASSERT_LT(write_result, kBufferSize);
}

TEST(PipeTest, BlockingSmallWrites) {
  // Create a pipe with size 4096, and fill all but 128 bytes of it.
  int pipefd[2];
  SAFE_SYSCALL(pipe2(pipefd, O_NONBLOCK));
  SAFE_SYSCALL(fcntl(pipefd[1], F_SETPIPE_SZ, getpagesize()));
  const int kWriteSize = getpagesize() - 128;
  char buf[kWriteSize];
  ASSERT_EQ(write(pipefd[1], buf, kWriteSize), kWriteSize);
  // Trying to write 256 bytes must returns EAGAIN
  ASSERT_EQ(write(pipefd[1], buf, 256), -1);
  ASSERT_EQ(errno, EAGAIN);
}

}  // namespace
