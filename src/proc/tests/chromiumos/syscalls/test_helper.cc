// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/proc/tests/chromiumos/syscalls/test_helper.h"

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#include <gtest/gtest.h>

namespace {
::testing::AssertionResult WaitForChildrenInternal() {
  ::testing::AssertionResult result = ::testing::AssertionSuccess();
  for (;;) {
    int wstatus;
    if (wait(&wstatus) == -1) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == ECHILD) {
        // No more child, reaping is done.
        return result;
      }
      // Another error is unexpected.
      result = ::testing::AssertionFailure()
               << "wait error: " << strerror(errno) << "(" << errno << ")";
    }
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
      result = ::testing::AssertionFailure()
               << "wait_status: WIFEXITED(wstatus) = " << WIFEXITED(wstatus)
               << ", WEXITSTATUS(wstatus) = " << WEXITSTATUS(wstatus)
               << ", WTERMSIG(wstatus) = " << WTERMSIG(wstatus);
    }
  }
}

}  // namespace

ForkHelper::ForkHelper() {
  // Ensure that all children will ends up being parented to the process that
  // created the helper.
  prctl(PR_SET_CHILD_SUBREAPER, 1);
}

ForkHelper::~ForkHelper() {
  // Wait for all remaining children, and ensure non failed.
  EXPECT_TRUE(WaitForChildrenInternal()) << ": at least a child had a failure";
}

bool ForkHelper::WaitForChildren() { return WaitForChildrenInternal(); }

pid_t ForkHelper::RunInForkedProcess(std::function<void()> action) {
  pid_t pid = SAFE_SYSCALL(fork());
  if (pid != 0) {
    return pid;
  }
  action();
  _exit(testing::Test::HasFailure());
}

CloneHelper::CloneHelper() {
  // Stack setup
  this->_childStack = (uint8_t *)mmap(NULL, CloneHelper::_childStackSize, PROT_WRITE | PROT_READ,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(errno == 0);
  assert(this->_childStack != (uint8_t *)(-1));
  this->_childStackBegin = this->_childStack + CloneHelper::_childStackSize;
}

CloneHelper::~CloneHelper() { munmap(this->_childStack, CloneHelper::_childStackSize); }

int CloneHelper::runInClonedChild(unsigned int cloneFlags, int (*childFunction)(void *)) {
  int childPid = clone(childFunction, this->_childStackBegin, cloneFlags, NULL);
  assert(errno == 0);
  assert(childPid != -1);
  return childPid;
}

int CloneHelper::sleep_1sec(void *) {
  struct timespec res;
  res.tv_sec = 1;
  res.tv_nsec = 0;
  clock_nanosleep(CLOCK_MONOTONIC, 0, &res, &res);
  return 0;
}

int CloneHelper::doNothing(void *) { return 0; }

void SignalMaskHelper::blockSignal(int signal) {
  sigemptyset(&this->_sigset);
  sigaddset(&this->_sigset, signal);
  sigprocmask(SIG_BLOCK, &this->_sigset, &this->_sigmaskCopy);
}

void SignalMaskHelper::waitForSignal(int signal) {
  int sig;
  int result = sigwait(&this->_sigset, &sig);
  ASSERT_EQ(result, 0);
  ASSERT_EQ(sig, signal);
}

void SignalMaskHelper::restoreSigmask() { sigprocmask(SIG_SETMASK, &this->_sigmaskCopy, NULL); }

ScopedTempFD::ScopedTempFD() : name_("/tmp/proc_test_file_XXXXXX") {
  char *mut_name = const_cast<char *>(name_.c_str());
  fd_ = ScopedFD(mkstemp(mut_name));
}

void waitForChildSucceeds(unsigned int waitFlag, int cloneFlags, int (*childRunFunction)(void *),
                          int (*parentRunFunction)(void *)) {
  CloneHelper cloneHelper;
  int expectedWaitPid = cloneHelper.runInClonedChild(cloneFlags, childRunFunction);

  parentRunFunction(NULL);

  int expectedWaitStatus = 0;
  int expectedErrno = 0;
  int actualWaitStatus;
  int actualWaitPid = waitpid(expectedWaitPid, &actualWaitStatus, waitFlag);
  EXPECT_EQ(actualWaitPid, expectedWaitPid);
  EXPECT_EQ(actualWaitStatus, expectedWaitStatus);
  EXPECT_EQ(errno, expectedErrno);
}

void waitForChildFails(unsigned int waitFlag, int cloneFlags, int (*childRunFunction)(void *),
                       int (*parentRunFunction)(void *)) {
  CloneHelper cloneHelper;
  int pid = cloneHelper.runInClonedChild(cloneFlags, childRunFunction);

  parentRunFunction(NULL);

  int expectedWaitPid = -1;
  int actualWaitPid = waitpid(pid, NULL, waitFlag);
  EXPECT_EQ(actualWaitPid, expectedWaitPid);
  EXPECT_EQ(errno, ECHILD);
  errno = 0;
}
