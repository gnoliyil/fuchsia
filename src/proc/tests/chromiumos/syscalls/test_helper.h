// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_PROC_TESTS_CHROMIUMOS_SYSCALLS_TEST_HELPER_H_
#define SRC_PROC_TESTS_CHROMIUMOS_SYSCALLS_TEST_HELPER_H_

#include <functional>

#include "syscall_matchers.h"

#define SAFE_SYSCALL(X)                                                             \
  ({                                                                                \
    auto retval = (X);                                                              \
    if (retval < 0) {                                                               \
      ADD_FAILURE() << #X << " failed: " << strerror(errno) << "(" << errno << ")"; \
      _exit(-1);                                                                    \
    }                                                                               \
    retval;                                                                         \
  })

// Helper class to handle test that needs to fork and do assertion on the child
// process.
class ForkHelper {
 public:
  ForkHelper();
  ~ForkHelper();

  // Wait for all children of the current process, and return true if all exited
  // with a 0 status.
  bool WaitForChildren();

  // For the current process and execute the given |action| inside the child,
  // then exit with a status equals to the number of failed expectation and
  // assertion. Return immediately with the pid of the child.
  pid_t RunInForkedProcess(std::function<void()> action);
};

// Helper class to handle tests that needs to clone processes.
class CloneHelper {
 public:
  CloneHelper();
  ~CloneHelper();

  // Call clone with the specified childFunction and cloneFlags.
  // Perform the necessary asserts to ensure the clone was performed with
  // no errors and return the new process ID.
  int runInClonedChild(unsigned int cloneFlags, int (*childFunction)(void *));

  // Handy trivial function for passing clone when we want the child to
  // sleep for 1 second and return 0.
  static int sleep_1sec(void *);

  // Handy trivial function for passing clone when we want the child to
  // do nothing and return 0.
  static int doNothing(void *);

 private:
  uint8_t *_childStack;
  uint8_t *_childStackBegin;
  static constexpr size_t _childStackSize = 0x5000;
};

#endif  // SRC_PROC_TESTS_CHROMIUMOS_SYSCALLS_TEST_HELPER_H_
