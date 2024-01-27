// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback_data/system_log_recorder/reader.h"

#include <lib/inspect/cpp/vmo/types.h>
#include <lib/syslog/cpp/log_level.h>

#include <memory>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/developer/forensics/feedback_data/system_log_recorder/encoding/identity_decoder.h"
#include "src/developer/forensics/feedback_data/system_log_recorder/encoding/identity_encoder.h"
#include "src/developer/forensics/feedback_data/system_log_recorder/system_log_recorder.h"
#include "src/developer/forensics/testing/log_message.h"
#include "src/developer/forensics/utils/log_format.h"
#include "src/lib/files/file.h"
#include "src/lib/files/path.h"
#include "src/lib/files/scoped_temp_dir.h"
#include "src/lib/fxl/strings/join_strings.h"

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
const StorageSize kMaxLogLineSize =
    StorageSize::Bytes(Format(BuildLogMessage(fuchsia_logging::LOG_INFO, "line X").value()).size());

const StorageSize kMaxDecompressedSize = StorageSize::Kilobytes(256);

std::unique_ptr<Encoder> MakeIdentityEncoder() {
  return std::unique_ptr<Encoder>(new IdentityEncoder());
}

std::unique_ptr<RedactorBase> MakeIdentityRedactor() {
  return std::unique_ptr<RedactorBase>(new IdentityRedactor(inspect::BoolProperty()));
}

std::string MakeLogFilePath(files::ScopedTempDir& temp_dir, const size_t file_num) {
  return files::JoinPath(temp_dir.path(), std::to_string(file_num));
}

TEST(ReaderTest, MergeRepeatedMessages) {
  // Merge repeated consecutive message together.
  // Test:
  // Input = msg_0 x123 x1, msg_1 x5 x2.
  // Output = msg_0 x124, msg_1 x7.
  //
  // Note: x123 = Last message repeated 123 times.

  files::ScopedTempDir temp_dir;

  // Write input: msg_0 x123 x1, msg_1 x5 x2.
  EXPECT_TRUE(
      files::WriteFile(MakeLogFilePath(temp_dir, 0u), R"([00001.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 123 MORE TIMES !!!
!!! MESSAGE REPEATED 1 MORE TIME !!!
[00001.000][07559][07687][] INFO: line 1
!!! MESSAGE REPEATED 5 MORE TIMES !!!
!!! MESSAGE REPEATED 2 MORE TIMES !!!
!!! MESSAGE REPEATED FOR 3 SECONDS !!!
!!! MESSAGE REPEATED 70 MORE TIMES !!!
!!! MESSAGE REPEATED 2 MORE TIMES !!!
)"));

  files::ScopedTempDir output_dir;
  const std::string output_path = files::JoinPath(output_dir.path(), "output.txt");
  float compression_ratio;
  IdentityDecoder decoder;

  ASSERT_TRUE(Concatenate(temp_dir.path(), kMaxDecompressedSize, &decoder, output_path,
                          &compression_ratio));

  std::string contents;
  ASSERT_TRUE(files::ReadFileToString(output_path, &contents));

  // Verify output, expect: msg_0 x124 msg_1 x7.
  EXPECT_EQ(contents, R"([00001.000][07559][07687][] INFO: line 0
!!! MESSAGE REPEATED 124 MORE TIMES !!!
[00001.000][07559][07687][] INFO: line 1
!!! MESSAGE REPEATED 7 MORE TIMES !!!
!!! MESSAGE REPEATED FOR 3 SECONDS !!!
!!! MESSAGE REPEATED 72 MORE TIMES !!!
)");
}

TEST(ReaderTest, SortsMessagesNoTimeTagOnly) {
  // Output messages even if no time tag is found. This can happen if the file could not be
  // decoded.
  files::ScopedTempDir temp_dir;

  const std::string message = "!!! CANNOT DECODE!!!\n!!! CANNOT DECODE!!\n";

  EXPECT_TRUE(files::WriteFile(MakeLogFilePath(temp_dir, 0u), message));

  files::ScopedTempDir output_dir;
  const std::string output_path = files::JoinPath(output_dir.path(), "output.txt");
  float compression_ratio;
  IdentityDecoder decoder;

  ASSERT_TRUE(Concatenate(temp_dir.path(), kMaxDecompressedSize, &decoder, output_path,
                          &compression_ratio));

  std::string contents;
  ASSERT_TRUE(files::ReadFileToString(output_path, &contents));

  EXPECT_EQ(contents, message);
}

TEST(ReaderTest, SortsMessagesMixed) {
  // Output header + sorted log messages
  files::ScopedTempDir temp_dir;

  const std::string header = "!!! CANNOT DECODE!!!\n!!! CANNOT DECODE!!";
  const std::string msg_0 = "[00002.000][07559][07687][] INFO: line 0";
  const std::string msg_1 = "[00001.000][07559][07687][] INFO: line 1";

  using logs = std::vector<std::string>;
  // The logs expect end-of-line at the end of file.
  const std::string input_message = fxl::JoinStrings((logs){header, msg_0, msg_1}, "\n") + "\n";
  const std::string output_message = fxl::JoinStrings((logs){header, msg_1, msg_0}, "\n") + "\n";

  EXPECT_TRUE(files::WriteFile(MakeLogFilePath(temp_dir, 0u), input_message));

  files::ScopedTempDir output_dir;
  const std::string output_path = files::JoinPath(output_dir.path(), "output.txt");
  float compression_ratio;
  IdentityDecoder decoder;

  ASSERT_TRUE(Concatenate(temp_dir.path(), kMaxDecompressedSize, &decoder, output_path,
                          &compression_ratio));

  std::string contents;
  ASSERT_TRUE(files::ReadFileToString(output_path, &contents));

  EXPECT_EQ(contents, output_message);
}

TEST(ReaderTest, SortsMessages) {
  files::ScopedTempDir temp_dir;

  LogMessageStore store(StorageSize::Kilobytes(8), StorageSize::Kilobytes(8),
                        MakeIdentityRedactor(), MakeIdentityEncoder());
  SystemLogWriter writer(temp_dir.path(), 1u, &store);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0", zx::msec(0))));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 3", zx::msec(3))));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2", zx::msec(2))));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1", zx::msec(1))));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1.1", zx::msec(1))));
  EXPECT_TRUE(
      store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "duplicated line", zx::msec(5))));
  EXPECT_TRUE(
      store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "duplicated line", zx::msec(6))));
  EXPECT_TRUE(
      store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "duplicated line", zx::msec(7))));
  EXPECT_TRUE(
      store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "multi\nline\nmessage", zx::msec(4))));
  writer.Write();

  files::ScopedTempDir output_dir;
  const std::string output_path = files::JoinPath(output_dir.path(), "output.txt");
  IdentityDecoder decoder;

  float compression_ratio;
  ASSERT_TRUE(Concatenate(temp_dir.path(), kMaxDecompressedSize, &decoder, output_path,
                          &compression_ratio));
  EXPECT_EQ(compression_ratio, 1.0);

  std::string contents;
  ASSERT_TRUE(files::ReadFileToString(output_path, &contents));
  EXPECT_EQ(contents, R"([15604.000][07559][07687][] INFO: line 0
[15604.001][07559][07687][] INFO: line 1
[15604.001][07559][07687][] INFO: line 1.1
[15604.002][07559][07687][] INFO: line 2
[15604.003][07559][07687][] INFO: line 3
[15604.004][07559][07687][] INFO: multi
line
message
[15604.005][07559][07687][] INFO: duplicated line
!!! MESSAGE REPEATED 2 MORE TIMES !!!
)");
}

TEST(ReaderTest, SortsMessagesDifferentTimestampLength) {
  // Sort correctly when the timestamp has different length.
  files::ScopedTempDir temp_dir;

  const std::string msg_0 = "[100000000.000][07559][07687][] INFO: line 0";
  const std::string msg_1 = "[20000000.000][07559][07687][] INFO: line 1";
  const std::string msg_2 = "[3000000.000][07559][07687][] INFO: line 2";
  const std::string msg_3 = "[400000.000][07559][07687][] INFO: line 3";
  const std::string msg_4 = "[50000.000][07559][07687][] INFO: line 4";

  using logs = std::vector<std::string>;
  // The logs expect end-of-line at the end of file.
  const std::string input_message =
      fxl::JoinStrings((logs){msg_0, msg_1, msg_2, msg_3, msg_4}, "\n") + "\n";
  const std::string output_message =
      fxl::JoinStrings((logs){msg_4, msg_3, msg_2, msg_1, msg_0}, "\n") + "\n";

  EXPECT_TRUE(files::WriteFile(MakeLogFilePath(temp_dir, 0u), input_message));

  files::ScopedTempDir output_dir;
  const std::string output_path = files::JoinPath(output_dir.path(), "output.txt");
  float compression_ratio;
  IdentityDecoder decoder;

  ASSERT_TRUE(Concatenate(temp_dir.path(), kMaxDecompressedSize, &decoder, output_path,
                          &compression_ratio));

  std::string contents;
  ASSERT_TRUE(files::ReadFileToString(output_path, &contents));

  EXPECT_EQ(contents, output_message);
}

TEST(ReaderTest, SortsMessagesMultipleFiles) {
  files::ScopedTempDir temp_dir;

  // Set the block and buffer to both hold 4 log messages.
  LogMessageStore store(kMaxLogLineSize * 4, kMaxLogLineSize * 4, MakeIdentityRedactor(),
                        MakeIdentityEncoder());
  SystemLogWriter writer(temp_dir.path(), 8u, &store);

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 0", zx::msec(0))));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 3", zx::msec(3))));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 2", zx::msec(2))));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line 1", zx::msec(1))));
  writer.Write();

  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line11", zx::msec(1))));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "dup", zx::msec(5))));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "dup", zx::msec(6))));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "dup", zx::msec(7))));
  EXPECT_TRUE(store.Add(BuildLogMessage(fuchsia_logging::LOG_INFO, "line\n4", zx::msec(4))));
  writer.Write();

  files::ScopedTempDir output_dir;
  const std::string output_path = files::JoinPath(output_dir.path(), "output.txt");
  IdentityDecoder decoder;

  float compression_ratio;
  ASSERT_TRUE(Concatenate(temp_dir.path(), kMaxDecompressedSize, &decoder, output_path,
                          &compression_ratio));
  EXPECT_EQ(compression_ratio, 1.0);

  std::string contents;
  ASSERT_TRUE(files::ReadFileToString(output_path, &contents));
  EXPECT_EQ(contents, R"([15604.000][07559][07687][] INFO: line 0
[15604.001][07559][07687][] INFO: line 1
[15604.001][07559][07687][] INFO: line11
[15604.002][07559][07687][] INFO: line 2
[15604.003][07559][07687][] INFO: line 3
[15604.004][07559][07687][] INFO: line
4
[15604.005][07559][07687][] INFO: dup
!!! MESSAGE REPEATED 2 MORE TIMES !!!
)");
}

}  // namespace
}  // namespace system_log_recorder
}  // namespace feedback_data
}  // namespace forensics
