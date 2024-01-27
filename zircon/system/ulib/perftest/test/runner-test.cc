// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zircon/assert.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <fbl/algorithm.h>
#include <perftest/perftest.h>
#include <perftest/runner.h>
#include <zxtest/zxtest.h>

// This is a helper for creating a FILE* that we can redirect output to, in
// order to make the tests below less noisy.  We don't look at the output
// that is sent to the stream.
class DummyOutputStream {
 public:
  DummyOutputStream() {
    fp_ = fmemopen(buf_, sizeof(buf_), "w+");
    ZX_ASSERT(fp_);
  }
  ~DummyOutputStream() {
    // Ignore any errors that might arise from running out of space in
    // the buffer when flushing the stream.
    fflush(fp_);
    ZX_ASSERT(fclose(fp_) == 0);
  }

  FILE* fp() { return fp_; }

 private:
  FILE* fp_;
  // Non-zero-size dummy buffer that fmemopen() will accept.
  char buf_[1];
};

// Example of a valid test that passes.
static bool NoOpTest(perftest::RepeatState* state) {
  while (state->KeepRunning()) {
  }
  return true;
}

// Example of a test that fails by returning false.
static bool FailingTest(perftest::RepeatState* state) {
  while (state->KeepRunning()) {
  }
  return false;
}

// Sanity-check time values.
static bool check_times(perftest::TestCaseResults* test_case) {
  for (auto time_taken : test_case->values) {
    EXPECT_GE(time_taken, 0);
    // Check for unreasonably large values, which suggest that we
    // subtracted timestamps incorrectly.
    EXPECT_LT(time_taken, static_cast<double>(1ULL << 60));
  }
  return true;
}

// Test that a successful run of a perf test produces sensible results.
TEST(PerfTestRunner, TestResults) {
  perftest::internal::TestList test_list;
  perftest::internal::NamedTest test{"no_op_example_test", NoOpTest};
  test_list.push_back(std::move(test));

  const uint32_t kRunCount = 7;
  perftest::ResultsSet results;
  DummyOutputStream out;
  EXPECT_TRUE(
      perftest::internal::RunTests("test-suite", &test_list, kRunCount, "", out.fp(), &results));

  auto* test_cases = results.results();
  ASSERT_EQ(test_cases->size(), 1);
  // The output should have time values for the number of runs we requested.
  auto* test_case = &(*test_cases)[0];
  EXPECT_EQ(test_case->values.size(), kRunCount);
  EXPECT_STREQ(test_case->label.c_str(), "no_op_example_test");
  EXPECT_TRUE(check_times(test_case));
}

// Test that if a perf test fails by returning "false", the failure gets
// propagated correctly.
TEST(PerfTestRunner, TestFailingTest) {
  perftest::internal::TestList test_list;
  perftest::internal::NamedTest test{"example_test", FailingTest};
  test_list.push_back(std::move(test));

  const uint32_t kRunCount = 7;
  perftest::ResultsSet results;
  DummyOutputStream out;
  EXPECT_FALSE(
      perftest::internal::RunTests("test-suite", &test_list, kRunCount, "", out.fp(), &results));
  EXPECT_EQ(results.results()->size(), 0);
}

// Test that we report a test as failed if it calls KeepRunning() too many
// or too few times.  Make sure that we don't overrun the array of
// timestamps or report uninitialized data from that array.
TEST(PerfTestRunner, TestBadKeepRunningCalls) {
  for (int actual_runs = 0; actual_runs < 10; ++actual_runs) {
    // Example test function which might call KeepRunning() the wrong
    // number of times.
    auto test_func = [=](perftest::RepeatState* state) {
      for (int i = 0; i < actual_runs + 1; ++i)
        state->KeepRunning();
      return true;
    };

    perftest::internal::TestList test_list;
    perftest::internal::NamedTest test{"example_bad_test", test_func};
    test_list.push_back(std::move(test));

    const uint32_t kRunCount = 5;
    perftest::ResultsSet results;
    DummyOutputStream out;
    bool success =
        perftest::internal::RunTests("test-suite", &test_list, kRunCount, "", out.fp(), &results);
    EXPECT_EQ(success, kRunCount == actual_runs);
    EXPECT_EQ(results.results()->size(), (size_t)(kRunCount == actual_runs ? 1 : 0));
  }
}

static bool MultistepTest(perftest::RepeatState* state) {
  state->DeclareStep("step1");
  state->DeclareStep("step2");
  state->DeclareStep("step3");
  while (state->KeepRunning()) {
    // Step 1 would go here.
    state->NextStep();
    // Step 2 would go here.
    state->NextStep();
    // Step 3 would go here.
  }
  return true;
}

// Test the results for a simple multi-step test.
TEST(PerfTestRunner, TestMultistepTest) {
  perftest::internal::TestList test_list;
  perftest::internal::NamedTest test{"example_test", MultistepTest};
  test_list.push_back(std::move(test));

  const uint32_t kRunCount = 7;
  perftest::ResultsSet results;
  DummyOutputStream out;
  EXPECT_TRUE(
      perftest::internal::RunTests("test-suite", &test_list, kRunCount, "", out.fp(), &results));
  ASSERT_EQ(results.results()->size(), 3);
  EXPECT_STREQ((*results.results())[0].label.c_str(), "example_test.step1");
  EXPECT_STREQ((*results.results())[1].label.c_str(), "example_test.step2");
  EXPECT_STREQ((*results.results())[2].label.c_str(), "example_test.step3");
  for (auto& test_case : *results.results()) {
    EXPECT_EQ(test_case.values.size(), kRunCount);
    EXPECT_TRUE(check_times(&test_case));
  }
}

static bool MultistepTestWithDuplicateNames(perftest::RepeatState* state) {
  // These duplicate names should be caught as an error.
  state->DeclareStep("step1");
  state->DeclareStep("step1");
  while (state->KeepRunning()) {
    state->NextStep();
  }
  return true;
}

// Test that we report a test as failed if it declares duplicate step names
// via DeclareStep().
TEST(PerfTestRunner, TestDuplicateStepNamesAreRejected) {
  perftest::internal::TestList test_list;
  perftest::internal::NamedTest test{"example_test", MultistepTestWithDuplicateNames};
  test_list.push_back(std::move(test));
  const uint32_t kRunCount = 7;
  perftest::ResultsSet results;
  DummyOutputStream out;
  EXPECT_FALSE(
      perftest::internal::RunTests("test-suite", &test_list, kRunCount, "", out.fp(), &results));
}

// Test that we report a test as failed if it calls NextStep() before
// KeepRunning(), which is invalid.
TEST(PerfTestRunner, TestNextStepCalledBeforeKeepRunning) {
  bool keeprunning_retval = true;
  // Invalid test function that calls NextStep() at the wrong time,
  // before calling KeepRunning().
  auto test_func = [&](perftest::RepeatState* state) {
    state->NextStep();
    keeprunning_retval = state->KeepRunning();
    return true;
  };

  perftest::internal::TestList test_list;
  perftest::internal::NamedTest test{"example_bad_test", test_func};
  test_list.push_back(std::move(test));
  const uint32_t kRunCount = 5;
  perftest::ResultsSet results;
  DummyOutputStream out;
  bool success =
      perftest::internal::RunTests("test-suite", &test_list, kRunCount, "", out.fp(), &results);
  EXPECT_FALSE(success);
  EXPECT_FALSE(keeprunning_retval);
}

// Test that we report a test as failed if it calls NextStep() too many or
// too few times.
TEST(PerfTestRunner, TestBadNextStepCalls) {
  for (int actual_calls = 0; actual_calls < 10; ++actual_calls) {
    // Example test function which might call NextStep() the wrong
    // number of times.
    auto test_func = [=](perftest::RepeatState* state) {
      state->DeclareStep("step1");
      state->DeclareStep("step2");
      state->DeclareStep("step3");
      while (state->KeepRunning()) {
        for (int i = 0; i < actual_calls; ++i) {
          state->NextStep();
        }
      }
      return true;
    };

    perftest::internal::TestList test_list;
    perftest::internal::NamedTest test{"example_bad_test", test_func};
    test_list.push_back(std::move(test));

    const uint32_t kRunCount = 5;
    perftest::ResultsSet results;
    DummyOutputStream out;
    bool success =
        perftest::internal::RunTests("test-suite", &test_list, kRunCount, "", out.fp(), &results);
    const int kCorrectNumberOfCalls = 2;
    EXPECT_EQ(success, actual_calls == kCorrectNumberOfCalls);
    EXPECT_EQ(results.results()->size(),
              static_cast<size_t>(actual_calls == kCorrectNumberOfCalls ? 3 : 0));
  }
}

// When no tests have been registered, a null pointer is passed to
// RunTests() as the test list.  Check that that does not crash.
TEST(PerfTestRunner, NullTestList) {
  const uint32_t kRunCount = 5;
  perftest::ResultsSet results;
  DummyOutputStream out;
  EXPECT_FALSE(
      perftest::internal::RunTests("test-suite", nullptr, kRunCount, "", out.fp(), &results));
}

// Check that, by default, test cases are run in sorted order, sorted by
// name.
TEST(PerfTestRunner, TestRunningInSortedOrder) {
  perftest::internal::TestList test_list;
  perftest::internal::NamedTest test1{"test1", NoOpTest};
  perftest::internal::NamedTest test2{"test2", NoOpTest};
  perftest::internal::NamedTest test3{"test3", NoOpTest};
  // Add tests in non-sorted order.
  test_list.push_back(std::move(test3));
  test_list.push_back(std::move(test1));
  test_list.push_back(std::move(test2));

  const uint32_t kRunCount = 5;
  perftest::ResultsSet results;
  DummyOutputStream out;
  EXPECT_TRUE(
      perftest::internal::RunTests("test-suite", &test_list, kRunCount, "", out.fp(), &results));
  auto* test_cases = results.results();
  ASSERT_EQ(test_cases->size(), 3);
  // Check that the tests are reported as being run in sorted order.
  EXPECT_STREQ((*test_cases)[0].label.c_str(), "test1");
  EXPECT_STREQ((*test_cases)[1].label.c_str(), "test2");
  EXPECT_STREQ((*test_cases)[2].label.c_str(), "test3");
}

// Test the option for running tests in a randomized order.
TEST(PerfTestRunner, TestRunningInRandomOrder) {
  perftest::internal::TestList test_list;
  perftest::internal::NamedTest test1{"test1", NoOpTest};
  perftest::internal::NamedTest test2{"test2", NoOpTest};
  perftest::internal::NamedTest test3{"test3", NoOpTest};
  test_list.push_back(std::move(test1));
  test_list.push_back(std::move(test2));
  test_list.push_back(std::move(test3));

  // Run the tests in test_list and return the names of the tests in the
  // order that they were run.
  auto run_tests = [&] {
    const uint32_t kRunCount = 5;
    perftest::ResultsSet results;
    DummyOutputStream out;
    EXPECT_TRUE(perftest::internal::RunTests("test-suite", &test_list, kRunCount, "", out.fp(),
                                             &results, /* quiet= */ false, /* random_order=*/true));
    std::vector<std::string> names;
    for (auto& test_case : *results.results()) {
      names.push_back(test_case.label.c_str());
    }

    // Check that we did run all the test cases (ignoring the order).
    std::vector<std::string> names_sorted = names;
    std::sort(names_sorted.begin(), names_sorted.end());
    EXPECT_EQ(names_sorted.size(), 3);
    EXPECT_STREQ(names_sorted[0].c_str(), "test1");
    EXPECT_STREQ(names_sorted[1].c_str(), "test2");
    EXPECT_STREQ(names_sorted[2].c_str(), "test3");

    return names;
  };

  // Check that the ordering varies between runs of test_list.  If the
  // ordering does not vary, the following loop will fail to terminate.
  std::vector<std::string> first_ordering = run_tests();
  while (run_tests() == first_ordering) {
  }
}

TEST(PerfTestRunner, TestParsingCommandArgs) {
  const char* argv[] = {
    "unused_argv0",
    "--runs",
    "123",
    "--out",
    "dest_file",
    "--filter",
    "some_regex",
    "--quiet",
#if defined(__Fuchsia__)
    "--enable-tracing",
    "--startup-delay=456"
#endif
  };
  perftest::internal::CommandArgs args;
  perftest::internal::ParseCommandArgs(std::size(argv), const_cast<char**>(argv), &args);
  EXPECT_EQ(args.run_count, 123);
  EXPECT_STREQ(args.output_filename, "dest_file");
  EXPECT_STREQ(args.filter_regex, "some_regex");
  EXPECT_TRUE(args.quiet);
#if defined(__Fuchsia__)
  EXPECT_TRUE(args.enable_tracing);
  EXPECT_EQ(args.startup_delay_seconds, 456);
#endif
}
