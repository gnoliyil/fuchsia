// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/diagnostics/cpp/fidl.h>
#include <fuchsia/logger/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/dispatcher.h>
#include <lib/async/wait.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fpromise/promise.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/watchdog/operations.h>
#include <lib/watchdog/watchdog.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include <fbl/unique_fd.h>
#include <src/diagnostics/lib/cpp-log-decoder/log_decoder.h>
#include <src/diagnostics/lib/cpp-log-tester/log_tester.h>
#include <src/lib/diagnostics/accessor2logger/log_message.h>
#include <src/lib/fsl/vmo/sized_vmo.h>
#include <src/lib/fsl/vmo/strings.h>
#include <src/lib/fxl/strings/join_strings.h>
#include <src/lib/fxl/strings/string_printf.h>
#include <src/lib/uuid/uuid.h>
#include <zxtest/zxtest.h>

namespace fs_watchdog {
namespace {

// Default sleep argument for the watchdog
constexpr std::chrono::nanoseconds kSleepDuration = std::chrono::milliseconds(100);

// Custom/overloaded operation timeout
constexpr int kOperationTimeoutSeconds = 1;
constexpr std::chrono::nanoseconds kOperationTimeout =
    std::chrono::seconds(kOperationTimeoutSeconds);

const Options kDefaultOptions = {kSleepDuration, true, kDefaultLogSeverity};
const Options kDisabledOptions = {kSleepDuration, false, kDefaultLogSeverity};

// Test that we can start the watchdog.
TEST(Watchdog, StartTest) {
  auto watchdog = CreateWatchdog(kDefaultOptions);
  ASSERT_TRUE(watchdog->Start().is_ok());
}

// Test that we can shutdown the watchdog.
TEST(Watchdog, ShutDownTest) {
  auto watchdog = CreateWatchdog(kDefaultOptions);
  ASSERT_TRUE(watchdog->Start().is_ok());
  ASSERT_TRUE(watchdog->ShutDown().is_ok());
}

// Test that we can shutdown watchdog without the thread waiting for duration of it's sleep.
TEST(Watchdog, ShutDownImmediatelyTest) {
  auto options = kDefaultOptions;
  options.sleep = std::chrono::hours(1);
  auto watchdog = CreateWatchdog(kDefaultOptions);
  ASSERT_TRUE(watchdog->Start().is_ok());
  std::this_thread::sleep_for(std::chrono::seconds(1));
  auto t1 = std::chrono::steady_clock::now();
  ASSERT_TRUE(watchdog->ShutDown().is_ok());
  auto t2 = std::chrono::steady_clock::now();
  ASSERT_LT(t2 - t1, std::chrono::seconds(10));
}

constexpr const char* kTestOperationName1 = "WatchdogTestOperation1";
constexpr const char* kTestOperationName2 = "WatchdogTestOperation2";
constexpr const char* kTestOperationName3 = "WatchdogTestOperation3";

// These are some of the known messages printed by the watchdog.
const std::string_view kLogMessageOperation("Operation:");
const std::string_view kLogMessageExceededTimeout("exceeded timeout");
const std::string_view kLogMessageTimeout("Timeout");
const std::string_view kLogMessageExceededOperation("exceeded operation:");
const std::string_view kLogMessageCompleted("completed(");

class TestOperation : public OperationBase {
 public:
  explicit TestOperation(const char* operation_name,
                         std::chrono::nanoseconds timeout = kOperationTimeout)
      : operation_name_(operation_name), timeout_(timeout) {}
  std::string_view Name() const final { return operation_name_; }
  std::chrono::nanoseconds Timeout() const final { return timeout_; }

 private:
  // Name of the operation.
  const char* operation_name_ = nullptr;

  // Timeout for this operation.
  std::chrono::nanoseconds timeout_;
};

class TestOperationTracker : public FsOperationTracker {
 public:
  TestOperationTracker(OperationBase* operation, WatchdogInterface* watchdog, bool track = true)
      : FsOperationTracker(operation, watchdog, track) {}
  void OnTimeOut(FILE* out_stream) const final { handler_called_++; }

  bool TimeoutHandlerCalled() const { return handler_called_ > 0; }

  int TimeoutHandlerCalledCount() const { return handler_called_; }

 private:
  // Incremented on each call to TimeoutHandler.
  mutable std::atomic<int> handler_called_ = 0;
};

// Returns true if the number of occurances of string |substr| in string |str|
// matches expected.
bool CheckOccurance(const std::string& str, const std::string_view substr, int expected) {
  int count = 0;
  std::string::size_type start = 0;

  while ((start = str.find(substr, start)) != std::string::npos) {
    ++count;
    start += substr.length();
  }

  return count == expected;
}

TEST(Watchdog, TryToAddDuplicate) {
  auto watchdog = CreateWatchdog(kDefaultOptions);
  EXPECT_TRUE(watchdog->Start().is_ok());
  TestOperation op(kTestOperationName1, kOperationTimeout);
  TestOperationTracker tracker(&op, watchdog.get());
  auto result = watchdog->Track(&tracker);
  ASSERT_FALSE(result.is_ok());
  ASSERT_EQ(result.error_value(), ZX_ERR_ALREADY_EXISTS);
}

TEST(Watchdog, TryToAddDuplicateAfterTimeout) {
  [[maybe_unused]] auto fd_pair = log_tester::SetupFakeLog();
  auto watchdog = CreateWatchdog(kDefaultOptions);
  EXPECT_TRUE(watchdog->Start().is_ok());
  TestOperation op(kTestOperationName1, kOperationTimeout);
  TestOperationTracker tracker(&op, watchdog.get());
  std::this_thread::sleep_for(std::chrono::seconds(kOperationTimeoutSeconds + 1));
  ASSERT_TRUE(tracker.TimeoutHandlerCalled());
  ASSERT_EQ(watchdog->Track(&tracker).error_value(), ZX_ERR_ALREADY_EXISTS);
}

TEST(Watchdog, StartDisabledWatchdog) {
  auto watchdog = CreateWatchdog(kDisabledOptions);
  ASSERT_EQ(watchdog->Start().error_value(), ZX_ERR_BAD_STATE);
}

TEST(Watchdog, StartRunningWatchdog) {
  auto watchdog = CreateWatchdog(kDefaultOptions);
  EXPECT_OK(watchdog->Start());
  ASSERT_EQ(watchdog->Start().error_value(), ZX_ERR_BAD_STATE);
}

TEST(Watchdog, ShutDownUnstarted) {
  auto watchdog = CreateWatchdog(kDefaultOptions);
  ASSERT_EQ(watchdog->ShutDown().error_value(), ZX_ERR_BAD_STATE);
}

TEST(Watchdog, ShutDownAgain) {
  auto watchdog = CreateWatchdog(kDefaultOptions);
  EXPECT_OK(watchdog->Start());
  EXPECT_TRUE(watchdog->ShutDown().is_ok());
  ASSERT_EQ(watchdog->ShutDown().error_value(), ZX_ERR_BAD_STATE);
}

TEST(Watchdog, TrackWithDisabledWatchdog) {
  auto watchdog = CreateWatchdog(kDisabledOptions);
  EXPECT_FALSE(watchdog->Start().is_ok());
  TestOperation op(kTestOperationName1, kOperationTimeout);
  TestOperationTracker tracker(&op, watchdog.get(), false);
  ASSERT_EQ(watchdog->Track(&tracker).error_value(), ZX_ERR_BAD_STATE);
}

TEST(Watchdog, RemoveUntrackedOperation) {
  auto watchdog = CreateWatchdog(kDefaultOptions);
  EXPECT_TRUE(watchdog->Start().is_ok());
  OperationTrackerId id;
  {
    TestOperation op(kTestOperationName1, kOperationTimeout);
    TestOperationTracker tracker(&op, watchdog.get(), false);
    id = tracker.GetId();
  }
  ASSERT_EQ(watchdog->Untrack(id).error_value(), ZX_ERR_NOT_FOUND);
}

TEST(Watchdog, OperationTimesOut) {
  auto fd_pair = log_tester::SetupFakeLog();
  {
    auto watchdog = CreateWatchdog(kDefaultOptions);
    EXPECT_TRUE(watchdog->Start().is_ok());
    {
      TestOperation op(kTestOperationName1, kOperationTimeout);
      TestOperationTracker tracker(&op, watchdog.get());
      std::this_thread::sleep_for(std::chrono::seconds(kOperationTimeoutSeconds + 1));
      ASSERT_TRUE(tracker.TimeoutHandlerCalled());
    }
  }
  auto str = log_tester::RetrieveLogs(std::move(fd_pair));

  // Find known strings.
  ASSERT_TRUE(CheckOccurance(str, kLogMessageOperation, 1));
  ASSERT_TRUE(CheckOccurance(str, kLogMessageExceededTimeout, 1));
  ASSERT_TRUE(CheckOccurance(str, kTestOperationName1, 2));
}

TEST(Watchdog, NoTimeoutsWhenDisabled) {
  auto fd_pair = log_tester::SetupFakeLog();
  {
    auto watchdog = CreateWatchdog(kDisabledOptions);
    EXPECT_TRUE(watchdog->Start().is_error());
    TestOperation op(kTestOperationName1, kOperationTimeout);
    {
      TestOperationTracker tracker(&op, watchdog.get(), false);
      ASSERT_EQ(watchdog->Track(&tracker).error_value(), ZX_ERR_BAD_STATE);
      std::this_thread::sleep_for(std::chrono::seconds(kOperationTimeoutSeconds + 1));
      ASSERT_FALSE(tracker.TimeoutHandlerCalled());
    }
  }
  auto str = log_tester::RetrieveLogs(std::move(fd_pair));
  // Find known strings.
  ASSERT_TRUE(CheckOccurance(str, kLogMessageOperation, 0));
  ASSERT_TRUE(CheckOccurance(str, kLogMessageExceededTimeout, 0));
  ASSERT_TRUE(CheckOccurance(str, kTestOperationName1, 0));
}

TEST(Watchdog, NoTimeoutsWhenShutDown) {
  auto fd_pair = log_tester::SetupFakeLog();
  {
    auto watchdog = CreateWatchdog(kDefaultOptions);
    EXPECT_TRUE(watchdog->Start().is_ok());
    EXPECT_TRUE(watchdog->ShutDown().is_ok());
    TestOperation op(kTestOperationName1, kOperationTimeout);
    {
      TestOperationTracker tracker(&op, watchdog.get());
      std::this_thread::sleep_for(std::chrono::seconds(kOperationTimeoutSeconds + 1));
      ASSERT_FALSE(tracker.TimeoutHandlerCalled());
    }
  }
  auto str = log_tester::RetrieveLogs(std::move(fd_pair));
  // Find known strings.
  ASSERT_TRUE(CheckOccurance(str, kLogMessageOperation, 0));
  ASSERT_TRUE(CheckOccurance(str, kLogMessageExceededTimeout, 0));
  ASSERT_TRUE(CheckOccurance(str, kTestOperationName1, 0));
}

TEST(Watchdog, OperationDoesNotTimesOut) {
  auto fd_pair = log_tester::SetupFakeLog();
  {
    auto watchdog = CreateWatchdog(kDefaultOptions);
    EXPECT_TRUE(watchdog->Start().is_ok());
    TestOperation op(kTestOperationName1, kOperationTimeout + std::chrono::seconds(10));
    {
      TestOperationTracker tracker(&op, watchdog.get());
      std::this_thread::sleep_for(std::chrono::seconds(kOperationTimeoutSeconds));
      ASSERT_FALSE(tracker.TimeoutHandlerCalled());
    }
  }
  auto str = log_tester::RetrieveLogs(std::move(fd_pair));
  // We should not find known strings.
  ASSERT_TRUE(CheckOccurance(str, kLogMessageOperation, 0));
  ASSERT_TRUE(CheckOccurance(str, kLogMessageExceededTimeout, 0));
  ASSERT_TRUE(CheckOccurance(str, kTestOperationName1, 0));
}

TEST(Watchdog, MultipleOperationsTimeout) {
  auto fd_pair = log_tester::SetupFakeLog();
  {
    auto watchdog = CreateWatchdog(kDefaultOptions);
    EXPECT_TRUE(watchdog->Start().is_ok());
    {
      TestOperation op1(kTestOperationName1, kOperationTimeout);
      TestOperation op2(kTestOperationName2, kOperationTimeout);
      TestOperation op3(kTestOperationName3, kOperationTimeout + std::chrono::seconds(10));
      TestOperationTracker tracker1(&op1, watchdog.get());
      TestOperationTracker tracker3(&op3, watchdog.get());
      TestOperationTracker tracker2(&op2, watchdog.get());
      std::this_thread::sleep_for(std::chrono::seconds(kOperationTimeoutSeconds + 1));
      ASSERT_TRUE(tracker1.TimeoutHandlerCalled());
      ASSERT_TRUE(tracker2.TimeoutHandlerCalled());
      ASSERT_FALSE(tracker3.TimeoutHandlerCalled());
    }
  }
  auto str = log_tester::RetrieveLogs(std::move(fd_pair));
  // Find known strings.
  ASSERT_TRUE(CheckOccurance(str, kLogMessageOperation, 2));
  ASSERT_TRUE(CheckOccurance(str, kLogMessageExceededTimeout, 2));
  ASSERT_TRUE(CheckOccurance(str, kTestOperationName1, 2));
  ASSERT_TRUE(CheckOccurance(str, kTestOperationName2, 2));
  ASSERT_TRUE(CheckOccurance(str, kTestOperationName3, 0));
}

TEST(Watchdog, LoggedOnlyOnce) {
  auto fd_pair = log_tester::SetupFakeLog();
  {
    auto watchdog = CreateWatchdog(kDefaultOptions);
    EXPECT_TRUE(watchdog->Start().is_ok());
    TestOperation op(kTestOperationName1, kOperationTimeout);
    {
      TestOperationTracker tracker(&op, watchdog.get());

      // Sleep as long as it takes to scan in-flight operation twice.
      std::this_thread::sleep_for(std::chrono::seconds((2 * kOperationTimeoutSeconds) + 1));
      ASSERT_TRUE(tracker.TimeoutHandlerCalled());
      ASSERT_EQ(tracker.TimeoutHandlerCalledCount(), 1);
    }
  }
  auto str = log_tester::RetrieveLogs(std::move(fd_pair));
  // Find known strings.
  ASSERT_TRUE(CheckOccurance(str, kLogMessageOperation, 1));
  ASSERT_TRUE(CheckOccurance(str, kLogMessageExceededTimeout, 1));

  // Operation name gets printed twice - once when timesout and once when it
  // completes
  ASSERT_TRUE(CheckOccurance(str, kTestOperationName1, 2));
}

TEST(Watchdog, DelayedCompletionLogging) {
  auto fd_pair = log_tester::SetupFakeLog();
  {
    auto watchdog = CreateWatchdog(kDefaultOptions);
    EXPECT_TRUE(watchdog->Start().is_ok());
    TestOperation op(kTestOperationName1, kOperationTimeout);
    {
      TestOperationTracker tracker(&op, watchdog.get());

      // Sleep as long as it takes to scan in-flight operation twice.
      std::this_thread::sleep_for(std::chrono::seconds((2 * kOperationTimeoutSeconds) + 1));
      ASSERT_TRUE(tracker.TimeoutHandlerCalled());
      ASSERT_EQ(tracker.TimeoutHandlerCalledCount(), 1);
    }
  }
  auto str = log_tester::RetrieveLogs(std::move(fd_pair));
  // Find known strings.
  ASSERT_TRUE(CheckOccurance(str, kLogMessageTimeout, 1));
  ASSERT_TRUE(CheckOccurance(str, kLogMessageExceededOperation, 1));
  ASSERT_TRUE(CheckOccurance(str, kLogMessageCompleted, 1));

  // Operation name gets printed twice - once when timesout and once when it
  // completes
  ASSERT_TRUE(CheckOccurance(str, kTestOperationName1, 2));
}

// Define a an operation(of Append type) with new timeout and track it.
TEST(Watchdog, TrackFsOperationType) {
  static const FsOperationType kMyfsAppendOperation(FsOperationType::CommonFsOperation::Append,
                                                    std::chrono::nanoseconds(1000000000));

  auto watchdog = CreateWatchdog(kDefaultOptions);
  EXPECT_TRUE(watchdog->Start().is_ok());

  FsOperationTracker tracker(&kMyfsAppendOperation, watchdog.get());
}
}  // namespace

}  // namespace fs_watchdog
