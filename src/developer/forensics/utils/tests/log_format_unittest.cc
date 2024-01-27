// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/utils/log_format.h"

#include <fuchsia/logger/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>

#include <gtest/gtest.h>

namespace forensics {
namespace {

constexpr zx::duration kLogMessageBaseTimestamp = zx::sec(15604);
constexpr uint64_t kLogMessageProcessId = 7559;
constexpr uint64_t kLogMessageThreadId = 7687;

fuchsia::logger::LogMessage BuildLogMessage(const int32_t severity, const std::string& text,
                                            const zx::duration timestamp_offset = zx::nsec(0),
                                            const std::vector<std::string>& tags = {}) {
  fuchsia::logger::LogMessage msg{};
  msg.time = (kLogMessageBaseTimestamp + timestamp_offset).get();
  msg.pid = kLogMessageProcessId;
  msg.tid = kLogMessageThreadId;
  msg.tags = tags;
  msg.severity = severity;
  msg.msg = text;
  return msg;
}

TEST(LogFormatTest, Check_CorrectSeverity) {
  std::string log_message;

  log_message = Format(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1"));
  EXPECT_EQ(log_message, "[15604.000][07559][07687][] INFO: line 1\n");

  log_message = Format(BuildLogMessage(fuchsia_logging::LOG_WARNING, "line 2"));
  EXPECT_EQ(log_message, "[15604.000][07559][07687][] WARN: line 2\n");

  log_message = Format(BuildLogMessage(fuchsia_logging::LOG_ERROR, "line 3"));
  EXPECT_EQ(log_message, "[15604.000][07559][07687][] ERROR: line 3\n");

  log_message = Format(BuildLogMessage(fuchsia_logging::LOG_FATAL, "line 4"));
  EXPECT_EQ(log_message, "[15604.000][07559][07687][] FATAL: line 4\n");

  log_message = Format(BuildLogMessage(fuchsia_logging::LOG_INFO + fuchsia_logging::LOG_WARNING +
                                           fuchsia_logging::LOG_ERROR + fuchsia_logging::LOG_FATAL,
                                       "line 5"));
  EXPECT_EQ(log_message, "[15604.000][07559][07687][] INVALID: line 5\n");

  log_message = Format(BuildLogMessage(fuchsia_logging::LOG_TRACE, "line 6"));
  EXPECT_EQ(log_message, "[15604.000][07559][07687][] TRACE: line 6\n");

  log_message = Format(BuildLogMessage(fuchsia_logging::LOG_DEBUG, "line 7"));
  EXPECT_EQ(log_message, "[15604.000][07559][07687][] DEBUG: line 7\n");

  log_message = Format(BuildLogMessage(fuchsia_logging::LOG_INFO - 1, "line 8"));
  EXPECT_EQ(log_message, "[15604.000][07559][07687][] VLOG(1): line 8\n");

  log_message = Format(BuildLogMessage(fuchsia_logging::LOG_INFO - 12, "line 9"));
  EXPECT_EQ(log_message, "[15604.000][07559][07687][] VLOG(12): line 9\n");
}

TEST(LogFormatTest, Check_CorrectTime) {
  std::string log_message =
      Format(BuildLogMessage(fuchsia_logging::LOG_WARNING, "line 1", zx::msec(1)));
  EXPECT_EQ(log_message, "[15604.001][07559][07687][] WARN: line 1\n");
}

TEST(LogFormatTest, Check_CorrectTags) {
  std::string log_message = Format(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1", zx::msec(1),
                                                   /*tags=*/{"foo", "bar"}));
  EXPECT_EQ(log_message, "[15604.001][07559][07687][foo, bar] INFO: line 1\n");
}

TEST(LogFormatTest, Check_InsertsDroppedLogMessageSingular) {
  fuchsia::logger::LogMessage message =
      BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1", zx::msec(1),
                      /*tags=*/{"foo", "bar"});
  message.dropped_logs = 1;

  EXPECT_EQ(
      Format(message),
      "[15604.001][07559][07687][foo, bar] WARN: !!! DROPPED 1 LOG !!!\n[15604.001][07559][07687][foo, bar] INFO: line 1\n");
}

TEST(LogFormatTest, Check_InsertsDroppedLogMessageMultiple) {
  fuchsia::logger::LogMessage message =
      BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1", zx::msec(1),
                      /*tags=*/{"foo", "bar"});
  message.dropped_logs = 2;

  EXPECT_EQ(
      Format(message),
      "[15604.001][07559][07687][foo, bar] WARN: !!! DROPPED 2 LOGS !!!\n[15604.001][07559][07687][foo, bar] INFO: line 1\n");
}

}  // namespace
}  // namespace forensics
