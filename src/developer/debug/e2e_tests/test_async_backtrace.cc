// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/fit/defer.h"
#include "src/developer/debug/e2e_tests/e2e_test.h"
#include "src/developer/debug/e2e_tests/fuzzy_matcher.h"
#include "src/developer/debug/zxdb/client/frame.h"
#include "src/developer/debug/zxdb/client/thread.h"

namespace zxdb {

namespace {

// Zxdb should be able to display an async backtrace from a single threaded executor in Rust.

class AsyncBacktrace : public E2eTest {
 public:
  void Run() {
    console().ProcessInputLine(
        "run-component fuchsia-pkg://fuchsia.com/zxdb_e2e_inferiors#meta/async_rust.cm");

    loop().Run();
  }

  void OnThreadStopped(Thread* thread, const StopInfo& info) override {
    console().ProcessInputLine("abt -v", [this](OutputBuffer output, std::vector<Err> errors) {
      // Ensure the process is killed even when the following assertions fail.
      auto _ = fit::defer([this]() { console().ProcessInputLine("kill"); });

      // The output should look like
      //
      // async_rust::main::func • select_mod.rs:321
      // └─ select!
      //    └─ async_rust::foo • join_mod.rs:95
      //       └─ join!
      //          └─ async_rust::baz • async_rust.rs:34
      //               i = 10
      //             └─ Timer{...}
      //          └─ async_rust::baz • async_rust.rs:34
      //               i = 11
      //             └─ Timer{...}
      //    └─ async_rust::bar • async_rust.rs:27
      //       └─ async_rust::baz • async_rust.rs:34
      //            i = 30
      //          └─ Timer{...}
      //    └─ async_rust::main::func::λ • async_rust.rs:12
      //       └─ Task<()>{...}
      FuzzyMatcher matcher(output.AsString());
      ASSERT_TRUE(matcher.MatchesLine({"async_rust::main::func"}));
      ASSERT_TRUE(matcher.MatchesLine({"select!"}));
      ASSERT_TRUE(matcher.MatchesLine({"   ", "async_rust::foo"}));
      ASSERT_TRUE(matcher.MatchesLine({"      ", "join!"}));
      ASSERT_TRUE(matcher.MatchesLine({"         ", "async_rust::baz", "async_rust.rs"}));
      ASSERT_TRUE(matcher.MatchesLine({"         ", "i = 10"}));
      ASSERT_TRUE(matcher.MatchesLine({"            ", "Timer"}));
      ASSERT_TRUE(matcher.MatchesLine({"         ", "async_rust::baz", "async_rust.rs"}));
      ASSERT_TRUE(matcher.MatchesLine({"         ", "i = 11"}));
      ASSERT_TRUE(matcher.MatchesLine({"   ", "async_rust::bar", "async_rust.rs"}));
      ASSERT_TRUE(matcher.MatchesLine({"      ", "async_rust::baz", "async_rust.rs"}));
      ASSERT_TRUE(matcher.MatchesLine({"      ", "i = 30"}));
      ASSERT_TRUE(matcher.MatchesLine({"   ", "async_rust::main::func::λ", "async_rust.rs"}));
    });
  }

  void WillDestroyProcess(Process* process, DestroyReason reason, int exit_code,
                          uint64_t timestamp) override {
    EXPECT_EQ(reason, DestroyReason::kKill);
    loop().QuitNow();
  }
};

}  // namespace

TEST_F(AsyncBacktrace, AsyncBacktrace) { Run(); }

}  // namespace zxdb
