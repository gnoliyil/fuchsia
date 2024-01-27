// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdlib>

#include <gtest/gtest.h>
#include <pw_random/xor_shift.h>
#include <pw_string/format.h>

#include "src/connectivity/bluetooth/core/bt-host/common/assert.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/random.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/parse_args.h"

#ifdef PW_LOG_DECLARE_FAKE_DRIVER
PW_LOG_DECLARE_FAKE_DRIVER();
#endif

using bt::LogSeverity;
using bt::testing::GetArgValue;

namespace {

constexpr LogSeverity LogSeverityFromString(std::string_view str) {
  if (str == "DEBUG") {
    return LogSeverity::DEBUG;
  } else if (str == "INFO") {
    return LogSeverity::INFO;
  } else if (str == "WARN") {
    return LogSeverity::WARN;
  } else if (str == "ERROR") {
    return LogSeverity::ERROR;
  }
  return LogSeverity::ERROR;
}

// A valid random seed must be in [1, kMaxRandomSeed].
constexpr uint32_t kMaxRandomSeed = 99999;

// Normalizes the seed to range [1, kMaxRandomSeed].
int32_t NormalizeRandomSeed(uint32_t seed) {
  return static_cast<int32_t>((seed - 1U) % kMaxRandomSeed) + 1;
}

int32_t GenerateRandomSeed() {
  // TODO(fxbug.dev/118898): Get time using pw::chrono for portability.
  const int64_t time_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now() - std::chrono::system_clock::from_time_t(0))
          .count();
  return NormalizeRandomSeed(static_cast<uint32_t>(time_ms));
}

}  // namespace

int main(int argc, char** argv) {
  LogSeverity log_severity = LogSeverity::ERROR;

  std::optional<std::string_view> severity_arg_value = GetArgValue("severity", argc, argv);
  if (severity_arg_value) {
    log_severity = LogSeverityFromString(*severity_arg_value);
  }

  // Set all library log messages to use printf.
  bt::UsePrintf(log_severity);

  // If --gtest_random_seed is not specified, then GoogleTest calculates a seed based on time. To
  // avoid using different seeds, we need to tell GoogleTest what seed we are using.
  std::vector<char*> new_argv(argv, argv + argc);
  char new_argv_seed_option[sizeof("--gtest_random_seed=-2147483648")] = {};

  // GoogleTest doesn't initialize the random seed (UnitTest::random_seed()) until RUN_ALL_TESTS, so
  // we need to parse it now to avoid configuring the random generator in every test suite.
  int32_t random_seed = 0;
  std::optional<std::string_view> seed_arg_value = GetArgValue("gtest_random_seed", argc, argv);
  if (seed_arg_value) {
    std::from_chars_result result = std::from_chars(
        seed_arg_value->data(), seed_arg_value->data() + seed_arg_value->size(), random_seed);
    if (result.ec != std::errc()) {
      fprintf(stderr, "\nERROR: Invalid gtest_random_seed value\n");
      return 1;
    }
    random_seed = NormalizeRandomSeed(random_seed);
  } else {
    random_seed = GenerateRandomSeed();
    BT_ASSERT(pw::string::Format(new_argv_seed_option, "--gtest_random_seed=%d", random_seed).ok());
    new_argv.push_back(new_argv_seed_option);
  }

  // Print the random seed so that it is easy to reproduce a test run.
  printf("\nGTEST_RANDOM_SEED=%d\n", random_seed);

  pw::random::XorShiftStarRng64 rng(random_seed);
  bt::set_random_generator(&rng);

  int new_argc = static_cast<int>(new_argv.size());
  // argv[argc] must be nullptr according to the C++ standard.
  new_argv.push_back(nullptr);

  testing::InitGoogleTest(&new_argc, new_argv.data());

  return RUN_ALL_TESTS();
}
