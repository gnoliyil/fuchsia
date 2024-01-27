// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Note this file may be used for multiple guest integration test binaries. Do not add any logic
// to this file that is specific to any one test binary or suite.

#include <gtest/gtest.h>

#include "logger.h"
#include "src/lib/fxl/test/test_settings.h"

// This test event listener dumps the guest's serial logs when a test fails.
class LoggerOutputListener : public ::testing::EmptyTestEventListener {
  void OnTestEnd(const ::testing::TestInfo& info) override {
    // Don't show guest output on success, or if it was already printed
    // during the test.
    if (!info.result()->Failed() || Logger::kLogAllGuestOutput) {
      return;
    }

    std::cout << "[----------] Begin guest output\n";
    std::cout << Logger::Get().Buffer();
    std::cout << "\n[----------] End guest output\n";
    std::cout.flush();
  }
};

int main(int argc, char** argv) {
  if (!fxl::SetTestSettings(argc, argv)) {
    return EXIT_FAILURE;
  }

  // TODO(https://fxbug.dev/122526): Remove this once the elf runner no longer
  // fools libc into block-buffering stdout.
  setlinebuf(stdout);

  LoggerOutputListener listener;

  testing::InitGoogleTest(&argc, argv);
  testing::UnitTest::GetInstance()->listeners().Append(&listener);
  int status = RUN_ALL_TESTS();
  testing::UnitTest::GetInstance()->listeners().Release(&listener);

  return status;
}
