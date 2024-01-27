// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/syslog/cpp/log_level.h>

#include <memory>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "lib/inspect/cpp/vmo/types.h"
#include "src/developer/forensics/feedback_data/constants.h"
#include "src/developer/forensics/feedback_data/system_log_recorder/encoding/identity_encoder.h"
#include "src/developer/forensics/feedback_data/system_log_recorder/system_log_recorder.h"
#include "src/developer/forensics/testing/log_message.h"
#include "src/developer/forensics/utils/log_format.h"
#include "src/developer/forensics/utils/redact/redactor.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace forensics {
namespace feedback_data {
namespace system_log_recorder {
namespace {

::fpromise::result<fuchsia::logger::LogMessage, std::string> BuildLogMessage(
    const int32_t severity, const std::string& text,
    const zx::duration timestamp_offset = zx::duration(0),
    const std::vector<std::string>& tags = {}) {
  return ::fpromise::ok(testing::BuildLogMessage(severity, text, timestamp_offset, tags));
}

// Only change "X" for one character. i.e. X -> 12 is not allowed.
const auto kMaxLogLineSize =
    StorageSize::Bytes(Format(BuildLogMessage(fuchsia_logging::LOG_INFO, "line X").value()).size());
const auto kRepeatedFormatStrSize =
    StorageSize::Bytes(std::string("!!! MESSAGE REPEATED X MORE TIMES !!!\n").size());
// We set the block size to an arbitrary large numbers for test cases where the block logic does
// not matter.
const auto kVeryLargeBlockSize = kMaxLogLineSize * 100;

std::unique_ptr<Encoder> MakeIdentityEncoder() {
  return std::unique_ptr<Encoder>(new IdentityEncoder());
}

std::unique_ptr<RedactorBase> MakeIdentityRedactor() {
  return std::unique_ptr<RedactorBase>(new IdentityRedactor(inspect::BoolProperty()));
}

class SimpleRedactor : public RedactorBase {
 public:
  SimpleRedactor(const bool count_calls)
      : RedactorBase(inspect::BoolProperty()), count_calls_(count_calls) {}

  std::string& Redact(std::string& text) override {
    text = (count_calls_) ? fxl::StringPrintf("R: %d", ++num_calls_) : "R";
    return text;
  }

  std::string& RedactJson(std::string& text) override { return Redact(text); }

  std::string UnredactedCanary() const override { return ""; }
  std::string RedactedCanary() const override { return ""; }

 private:
  bool count_calls_;
  int num_calls_{0};
};

std::unique_ptr<RedactorBase> MakeSimpleRedactor(const bool count_calls) {
  return std::unique_ptr<RedactorBase>(new SimpleRedactor(count_calls));
}

TEST(LogMessageStoreTest, NotSafeAfterInterruption) {
  LogMessageStore store(kMaxLogLineSize * 10, kMaxLogLineSize, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  EXPECT_FALSE(store.SafeAfterInterruption());
}

TEST(LogMessageStoreTest, UnlimitedMessages) {
  // Set the block to hold 10 log messages while the buffer holds 1 log message (but the buffer
  // limits should be ignored because the component has just started up).
  LogMessageStore store(kMaxLogLineSize * 10, kMaxLogLineSize, MakeIdentityRedactor(),
                        MakeIdentityEncoder());

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));

  bool end_of_block;
  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 2 MORE TIMES !!!
[15604.000][07559][07687][] INFO: line 1
!!! MESSAGE REPEATED 2 MORE TIMES !!!
[15604.000][07559][07687][] INFO: line 2
!!! MESSAGE REPEATED 2 MORE TIMES !!!
)");

  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, AppliesRedaction) {
  // Set the block to hold 10 log messages while the buffer holds 1 log message (but the buffer
  // limits should be ignored because the component has just started up).
  LogMessageStore store(kMaxLogLineSize * 10, kMaxLogLineSize,
                        MakeSimpleRedactor(/*count_calls=*/true), MakeIdentityEncoder());

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));

  bool end_of_block;
  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: R: 1
[15604.000][07559][07687][] INFO: R: 2
[15604.000][07559][07687][] INFO: R: 3
[15604.000][07559][07687][] INFO: R: 4
[15604.000][07559][07687][] INFO: R: 5
[15604.000][07559][07687][] INFO: R: 6
[15604.000][07559][07687][] INFO: R: 7
[15604.000][07559][07687][] INFO: R: 8
[15604.000][07559][07687][] INFO: R: 9
)");

  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, RedactionCompressed) {
  // Set the block to hold 10 log messages while the buffer holds 1 log message (but the buffer
  // limits should be ignored because the component has just started up).
  LogMessageStore store(kMaxLogLineSize * 10, kMaxLogLineSize,
                        MakeSimpleRedactor(/*count_calls=*/false), MakeIdentityEncoder());

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));

  bool end_of_block;
  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: R
!!! MESSAGE REPEATED 8 MORE TIMES !!!
)");

  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyBlock) {
  // Set the block to hold 2 log messages while the buffer holds 1 log message.
  LogMessageStore store(kMaxLogLineSize * 2, kMaxLogLineSize, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();
  bool end_of_block;

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
)");
  EXPECT_FALSE(end_of_block);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 1
)");
  EXPECT_TRUE(end_of_block);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));
  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 2
)");
  EXPECT_FALSE(end_of_block);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 3")));
  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 3
)");
  EXPECT_TRUE(end_of_block);
}

TEST(LogMessageStoreTest, AddAndConsume) {
  // Set up the store to hold 2 log lines.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 2, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();
  bool end_of_block;

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
[15604.000][07559][07687][] INFO: line 1
)");
  EXPECT_FALSE(end_of_block);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 3")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 2
[15604.000][07559][07687][] INFO: line 3
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, DropsCorrectly) {
  bool end_of_block;
  // Set up the store to hold 2 log lines to test that the subsequent 3 are dropped.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 2, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 3")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 4")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
[15604.000][07559][07687][] INFO: line 1
!!! DROPPED 3 MESSAGES !!!
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, DropsSubsequentShorterMessages) {
  bool end_of_block;
  // Even though the store could hold 2 log lines, all the lines after the first one will be
  // dropped because the second log message is very long.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 2, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_FALSE(store.Add(BuildLogMessage(
      fuchsia_logging::LOG_INFO,
      "This is a very big message that will not fit so it should not be displayed!")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 3")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 4")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! DROPPED 4 MESSAGES !!!
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyRepetitionMessage_AtConsume) {
  bool end_of_block;
  // Set up the store to hold 2 log line. With three repeated messages, the last two messages
  // should get reduced to a single repeated message.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 2 MORE TIMES !!!
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyRepetition_DoNotResetRepeatedWarningOnConsume) {
  bool end_of_block;
  // Test that we only write repeated warning messages when repeated messages span over 2 buffers.
  // Block capacity: very large (unlimited for this example)
  // Buffer capacity: 1 log message
  //
  // __________________
  // |input   |output |
  // |________|_______| _
  // |line 0  |line 0 |  |
  // |line 0  |x2     |  |---- Consume 1
  // |line 0  |       |  |
  // |________|_______| _|
  // |line 0  |x2     |  |
  // |line 0  |       |  |---- Consume 2
  // |________|_______| _|
  //
  // Note: xN = last message repeated N times
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 2 MORE TIMES !!!
)");

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));

  EXPECT_EQ(store.Consume(&end_of_block), R"(!!! MESSAGE REPEATED 2 MORE TIMES !!!
)");

  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyRepetition_ResetRepeatedWarningOnConsume) {
  bool end_of_block;
  // Test that the first log of a block should not be a repeated warning message.
  // Block capacity: 1 log message
  // Buffer capacity: 1 log message
  //
  // __________________
  // |input   |output |
  // |________|_______| _
  // |line 0  |line 0 |  |
  // |line 0  |x2     |  |---- Consume 1
  // |line 0  |       |  |
  // |________|_______| _|
  // |  End of Block  |
  // |----------------| _
  // |line 0  |line 0 |  |
  // |line 0  |x1     |  |---- Consume 2
  // |________|_______| _|
  // |  End of Block  |
  // -----------------
  // Note: xN = last message repeated N times
  LogMessageStore store(kMaxLogLineSize, kMaxLogLineSize, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 2 MORE TIMES !!!
)");

  EXPECT_TRUE(end_of_block);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 1 MORE TIME !!!
)");

  EXPECT_TRUE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyRepetition_LimitRepetitionBuffers) {
  bool end_of_block;
  // Test that repeated messages do not extend over more than kMaxRepeatedBuffers.
  // Block capacity: 2 * kMaxRepeatedBuffer log message (test only uses one block)
  // Buffer capacity: 2 log messages
  // __________________
  // |input   |output |
  // |________|_______| _
  // |line 0  |line 0 |  |
  // |line 0  |x1     |  |---- Consume 1 time
  // |________|_______| _|
  // |line 0  |x1     |  |
  // |        |       |  |---- Consume (kMaxRepeatedBuffers - 1) times
  // |________|_______| _|
  // |line 0  |       |  |
  // |        |       |  |---- Consume kMaxRepeatedBuffers times
  // |________|_______| _|
  // |line 1  |xRep   |  |
  // |        |line 1 |  |---- Consume 1 time
  // |________|_______| _|
  //
  // Note: xN = last message repeated N times, Rep = kMaxRepeatedBuffers.
  LogMessageStore store(2 * kMaxRepeatedBuffers * kMaxLogLineSize, 2 * kMaxLogLineSize,
                        MakeIdentityRedactor(), MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 1 MORE TIME !!!
)");

  for (size_t i = 1; i < kMaxRepeatedBuffers; i++) {
    EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
    EXPECT_EQ(store.Consume(&end_of_block), R"(!!! MESSAGE REPEATED 1 MORE TIME !!!
)");
  }

  for (size_t i = 0; i < kMaxRepeatedBuffers; i++) {
    EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
    EXPECT_EQ(store.Consume(&end_of_block), "");
  }

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_EQ(store.Consume(&end_of_block),
            "!!! MESSAGE REPEATED " + std::to_string(kMaxRepeatedBuffers) + " MORE TIMES !!!\n" +
                "[15604.000][07559][07687][] INFO: line 1\n");

  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyRepetitionMessage_WhenMessageChanges) {
  bool end_of_block;
  // Set up the store to hold 3 log line. Verify that a repetition message appears after input
  // repetition and before the input change.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 2 + kRepeatedFormatStrSize,
                        MakeIdentityRedactor(), MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 1 MORE TIME !!!
[15604.000][07559][07687][] INFO: line 1
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyRepetitionMessage_WhenSeverityChanges) {
  bool end_of_block;
  // Set up the store to hold 3 log line. Verify that a repetition message appears after input
  // repetition and before the input severity change.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 2 + kRepeatedFormatStrSize,
                        MakeIdentityRedactor(), MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_WARNING, "line 0")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 1 MORE TIME !!!
[15604.000][07559][07687][] WARN: line 0
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyRepetitionMessage_WhenTagsChange) {
  bool end_of_block;
  // Set up the store to hold 4 log lines. Verify that a repetition message appears after
  // input repetition and before the input change.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 3 + kRepeatedFormatStrSize,
                        MakeIdentityRedactor(), MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(
      store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0", zx::duration(0), {"tag1"})));
  EXPECT_TRUE(
      store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0", zx::duration(0), {"tag1"})));
  EXPECT_TRUE(store.Add(
      BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0", zx::duration(0), {"tag1", "tag2"})));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][tag1] INFO: line 0
!!! MESSAGE REPEATED 1 MORE TIME !!!
[15604.000][07559][07687][tag1, tag2] INFO: line 0
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyDroppedRepeatedMessage_OnBufferFull) {
  bool end_of_block;
  // Set up the store to hold 1 log line. Verify that repeated messages that occur after the
  // buffer is full get dropped.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 1, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! DROPPED 2 MESSAGES !!!
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyNoRepeatMessage_AfterFirstConsume) {
  bool end_of_block;
  // Set up the store to hold 1 log line. Verify that there is no repeat message right after
  // dropping messages.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 1, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! DROPPED 1 MESSAGES !!!
)");
  EXPECT_FALSE(end_of_block);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 1
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyRepeatMessage_AfterFirstConsume) {
  bool end_of_block;
  // Set up the store to hold 3 log lines. Verify that there can be a repeat message after
  // consume, when no messages were dropped.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 3, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 1 MORE TIME !!!
[15604.000][07559][07687][] INFO: line 1
)");
  EXPECT_FALSE(end_of_block);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_EQ(store.Consume(&end_of_block), R"(!!! MESSAGE REPEATED 1 MORE TIME !!!
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyRepeatedAndDropped) {
  bool end_of_block;
  // Set up the store to hold 2 log lines. Verify that we can have the repeated message, and then
  // the dropped message.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 2, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 1 MORE TIME !!!
!!! DROPPED 1 MESSAGES !!!
)");
  EXPECT_FALSE(end_of_block);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1")));
  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 1
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyNoRepeatMessage_TimeOrdering) {
  bool end_of_block;
  // Set up the store to hold 2 log line. Verify time ordering: a message cannot be counted as
  // repeated if it's in between messages, even if those messages get dropped.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 2, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1 overflow msg")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1 overflow msg")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! DROPPED 5 MESSAGES !!!
)");
  EXPECT_FALSE(end_of_block);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyAppendToEnd) {
  bool end_of_block;
  // Set up the store to hold 2 log line. Verify time ordering: a message cannot be counted as
  // repeated if it's in between messages, even if those messages get dropped.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 2, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1 overflow msg")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1 overflow msg")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_FALSE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  store.AppendToEnd("DONE\n");

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 1 MORE TIME !!!
!!! DROPPED 4 MESSAGES !!!
DONE
)");
  EXPECT_FALSE(end_of_block);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
)");
  EXPECT_FALSE(end_of_block);
}

TEST(LogMessageStoreTest, VerifyNoRepeatWarningAfter_AppendToEnd) {
  bool end_of_block;
  // Set up the store to hold 2 log line. Verify time ordering: a message cannot be counted as
  // repeated if it's in between messages, even if those messages get dropped.
  LogMessageStore store(kVeryLargeBlockSize, kMaxLogLineSize * 2, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  store.TurnOnRateLimiting();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));
  store.AppendToEnd("DONE\n");

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 1 MORE TIME !!!
DONE
)");
  EXPECT_FALSE(end_of_block);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0")));

  EXPECT_EQ(store.Consume(&end_of_block), R"([15604.000][07559][07687][] INFO: line 0
)");
  EXPECT_FALSE(end_of_block);
}

}  // namespace
}  // namespace system_log_recorder
}  // namespace feedback_data
}  // namespace forensics
