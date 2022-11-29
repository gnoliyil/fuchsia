// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/e2e_tests/e2e_test.h"
#include "src/developer/debug/ipc/records.h"
#include "src/developer/debug/zxdb/client/frame.h"
#include "src/developer/debug/zxdb/client/thread.h"

namespace zxdb {

namespace {

// Zxdb should be able to symbolize inlined frames correctly.

class InlinedFrames : public E2eTest {
 public:
  void Run() {
    console().ProcessInputLine(
        "run-component fuchsia-pkg://fuchsia.com/zxdb_e2e_inferiors#meta/inlined_crasher.cm");

    loop().Run();
  }

  void OnThreadStopped(Thread* thread, const StopInfo& info) override {
    ASSERT_EQ(info.exception_type, debug_ipc::ExceptionType::kUndefinedInstruction);

    thread->GetStack().SyncFrames(
        [this, thread](const Err& err) { OnFramesSynced(err, thread->GetStack()); });
  }

  void OnFramesSynced(const Err& err, const Stack& stack) {
    ASSERT_TRUE(stack.has_all_frames());
    ASSERT_GE(stack.size(), 5u);

    // f0, inlined
    EXPECT_EQ(stack[0]->GetLocation().symbol().Get()->GetFullName(), "f0");
    EXPECT_TRUE(stack[0]->IsInline());

    // f1, physical
    EXPECT_EQ(stack[1]->GetLocation().symbol().Get()->GetFullName(), "f1");
    EXPECT_FALSE(stack[1]->IsInline());
    EXPECT_EQ(stack[0]->GetAddress(), stack[1]->GetAddress());

    // f2, inlined
    EXPECT_EQ(stack[2]->GetLocation().symbol().Get()->GetFullName(), "f2");
    EXPECT_TRUE(stack[2]->IsInline());

    // f3, physical
    EXPECT_EQ(stack[3]->GetLocation().symbol().Get()->GetFullName(), "f3");
    EXPECT_FALSE(stack[3]->IsInline());
    EXPECT_EQ(stack[2]->GetAddress(), stack[3]->GetAddress());

    // The bottommost frame should be "_start".
    EXPECT_EQ(stack[stack.size() - 1]->GetLocation().symbol().Get()->GetFullName(), "_start");

    console().ProcessInputLine("kill");
  }

  void WillDestroyProcess(Process* process, DestroyReason reason, int exit_code,
                          uint64_t timestamp) override {
    EXPECT_EQ(reason, DestroyReason::kKill);
    loop().QuitNow();
  }
};

}  // namespace

TEST_F(InlinedFrames, InlinedFrames) { Run(); }

}  // namespace zxdb
