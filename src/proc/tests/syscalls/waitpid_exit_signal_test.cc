// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

#include <gtest/gtest.h>

#include "src/proc/tests/syscalls/test_helper.h"

namespace {
std::vector<int> receivedSignals;

void handler(int signum) { receivedSignals.push_back(signum); }

// This CloneHelper instance must only be used after a clone without 'CLONE_THREAD | CLONE_VM'.
CloneHelper nestedCloneHelper;

void ensureWait(int pid, unsigned int waitFlags) {
  int actualWaitpid = waitpid(pid, NULL, waitFlags);
  EXPECT_EQ(errno, 0);
  EXPECT_EQ(pid, actualWaitpid);
}
}  // namespace

class WaitpidExitSignalTest : public testing::Test {
 protected:
  void SetUp() override {
    receivedSignals.clear();
    errno = 0;
    signal(SIGUSR1, handler);
    signal(SIGCHLD, handler);
  }
  void TearDown() override {
    signal(SIGUSR1, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
  }
};

/*
 * Main process (P0) creates a child process (P1).
 * On termination, P1 sends its exit signal (if any) to P0.
 */
TEST_F(WaitpidExitSignalTest, childProcessSendsDefaultSignalOnTerminationToParentProcess) {
  CloneHelper testCloneHelper;
  int pid = testCloneHelper.runInClonedChild(SIGCHLD, CloneHelper::doNothing);
  ensureWait(pid, __WALL);
  EXPECT_TRUE(receivedSignals.size() == 1);
  EXPECT_EQ(receivedSignals[0], SIGCHLD);
}

TEST_F(WaitpidExitSignalTest, childProcessSendsCustomExitSignalOnTerminationToParentProcess) {
  CloneHelper testCloneHelper;
  int pid = testCloneHelper.runInClonedChild(SIGUSR1, CloneHelper::doNothing);
  ensureWait(pid, __WALL);
  EXPECT_TRUE(receivedSignals.size() == 1);
  EXPECT_EQ(receivedSignals[0], SIGUSR1);
}

TEST_F(WaitpidExitSignalTest, childProcessSendsNoExitSignalOnTerminationToParentProcess) {
  CloneHelper testCloneHelper;
  int pid = testCloneHelper.runInClonedChild(0, CloneHelper::doNothing);
  ensureWait(pid, __WALL);
  EXPECT_TRUE(receivedSignals.empty());
}

/*
 * Main process (P0) creates a child process (P1) and P1 creates a child thread (T1).
 * After both P1 and T1 terminate, no matter the order of these termination, P0 should receive P1
 * exit signal.
 */
int processThatFinishAfterChildThread(void *) {
  nestedCloneHelper.runInClonedChild(CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | SIGUSR2,
                                     CloneHelper::doNothing);
  CloneHelper::sleep_1sec(NULL);
  return 0;
}

int processThatFinishBeforeChildThread(void *) {
  nestedCloneHelper.runInClonedChild(CLONE_THREAD | CLONE_VM | CLONE_SIGHAND | SIGUSR2,
                                     CloneHelper::sleep_1sec);
  return 0;
}

TEST_F(WaitpidExitSignalTest, childThreadGroupSendsCorrectExitSignalWhenLeaderTerminatesLast) {
  CloneHelper testCloneHelper;
  int pid = testCloneHelper.runInClonedChild(SIGUSR1, processThatFinishAfterChildThread);
  ensureWait(pid, __WALL);
  EXPECT_TRUE(receivedSignals.size() == 1);
  EXPECT_EQ(receivedSignals[0], SIGUSR1);
}

TEST_F(WaitpidExitSignalTest, childThreadGroupSendsCorrectExitSignalWhenLeaderTerminatesFirst) {
  CloneHelper testCloneHelper;
  int pid = testCloneHelper.runInClonedChild(SIGUSR1, processThatFinishBeforeChildThread);
  ensureWait(pid, __WALL);
  EXPECT_TRUE(receivedSignals.size() == 1);
  EXPECT_EQ(receivedSignals[0], SIGUSR1);
}
