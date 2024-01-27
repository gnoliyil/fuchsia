// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>
#include <fbl/vector.h>
#include <re2/re2.h>
#include <runtests-utils/fuchsia-run-test.h>
#include <runtests-utils/runtests-utils.h>
#include <zxtest/zxtest.h>

#include "runtests-utils-test-utils.h"

namespace runtests {
namespace {

TEST(SetUpForTestComponent, SetUpForTestComponentCMX) {
  fbl::String component_executor;
  EXPECT_TRUE(SetUpForTestComponent("fuchsia-pkg://fuchsia.com/foo-tests#meta/bar.cmx",
                                    &component_executor));
  EXPECT_GT(component_executor.length(), 0);
}

TEST(SetUpForTestComponent, SetUpForTestComponentCM) {
  fbl::String component_executor;
  EXPECT_TRUE(SetUpForTestComponent("fuchsia-pkg://fuchsia.com/foo-tests#meta/bar.cm",
                                    &component_executor));
  EXPECT_GT(component_executor.length(), 0);
}

TEST(SetUpForTestComponent, SetUpForTestComponentBadURI) {
  fbl::String component_executor;
  EXPECT_FALSE(SetUpForTestComponent("fuchsia-pkg://fuchsia.com/foo-tests#meta/bar.xyz",
                                     &component_executor));
  EXPECT_EQ(component_executor.length(), 0);
}

TEST(SetUpForTestComponent, SetUpForTestComponentPkgFS) {
  fbl::String component_executor;
  EXPECT_FALSE(SetUpForTestComponent("/pkgfs/packages/foo-tests/bar", &component_executor));
  EXPECT_EQ(component_executor.length(), 0);
}

TEST(SetUpForTestComponent, SetUpForTestComponentPath) {
  fbl::String component_executor;
  EXPECT_TRUE(SetUpForTestComponent("/boot/test/foo", &component_executor));
  EXPECT_EQ(component_executor.length(), 0);
}

fbl::String PublishDataHelperDir() { return JoinPath(packaged_script_dir(), "publish-data"); }

fbl::String PublishDataHelperBin() {
  return JoinPath(PublishDataHelperDir(), "publish-data-helper");
}

fbl::String ProfileHelperDir() { return JoinPath(packaged_script_dir(), "profile"); }

fbl::String ProfileHelperBin() { return JoinPath(ProfileHelperDir(), "profile-helper"); }

constexpr char kTmp[] = "/tmp";

TEST(RunTests, RunTestDontPublishData) {
  ScopedTestDir test_dir(kTmp);
  fbl::String test_name = PublishDataHelperBin();

  const char* argv[] = {test_name.c_str(), nullptr};
  std::unique_ptr<Result> result = RunTest(argv, nullptr, test_name.c_str(), 0, nullptr);
  EXPECT_STREQ(argv[0], result->name.c_str());
  EXPECT_EQ(SUCCESS, result->launch_status);
  EXPECT_EQ(0, result->return_code);
  EXPECT_EQ(0, result->data_sinks.size());
}

TEST(RunTests, RunTestPublishData) {
  ScopedTestDir test_dir(kTmp);
  fbl::String test_name = PublishDataHelperBin();

  const char* argv[] = {test_name.c_str(), nullptr};
  const fbl::String output_dir = JoinPath(test_dir.path(), "output");
  ASSERT_EQ(0, MkDirAll(output_dir));
  std::unique_ptr<Result> result = RunTest(argv, output_dir.c_str(), test_name.c_str(), 0, nullptr);
  EXPECT_STREQ(argv[0], result->name.c_str());
  EXPECT_EQ(SUCCESS, result->launch_status);
  EXPECT_EQ(0, result->return_code);
  EXPECT_EQ(1, result->data_sinks.size());
}

TEST(RunTests, RunTestsPublishData) {
  ScopedTestDir test_dir(kTmp);
  fbl::String test_name = PublishDataHelperBin();
  int num_failed = 0;
  fbl::Vector<std::unique_ptr<Result>> results;
  const fbl::String output_dir = JoinPath(test_dir.path(), "output");
  ASSERT_EQ(0, MkDirAll(output_dir));
  EXPECT_TRUE(RunTests({test_name}, {}, 1, 0, output_dir.c_str(), nullptr, &num_failed, &results));
  EXPECT_EQ(0, num_failed);
  EXPECT_EQ(1, results.size());
  EXPECT_LE(1, results[0]->data_sinks.size());
}

TEST(RunTests, RunDuplicateTestsPublishData) {
  ScopedTestDir test_dir(kTmp);
  fbl::String test_name = PublishDataHelperBin();
  int num_failed = 0;
  fbl::Vector<std::unique_ptr<Result>> results;
  const fbl::String output_dir = JoinPath(test_dir.path(), "output");
  ASSERT_EQ(0, MkDirAll(output_dir));
  EXPECT_TRUE(RunTests({test_name, test_name, test_name}, {}, 1, 0, output_dir.c_str(), nullptr,
                       &num_failed, &results));
  EXPECT_EQ(0, num_failed);
  EXPECT_EQ(3, results.size());
  EXPECT_STREQ(test_name.c_str(), results[0]->name.c_str());
  EXPECT_STREQ(fbl::String::Concat({test_name, " (2)"}).c_str(), results[1]->name.c_str());
  EXPECT_STREQ(fbl::String::Concat({test_name, " (3)"}).c_str(), results[2]->name.c_str());
}

TEST(RunTests, RunAllTestsPublishData) {
  ScopedTestDir test_dir(kTmp);
  fbl::String test_containing_dir = PublishDataHelperDir();
  fbl::String test_name = PublishDataHelperBin();

  const fbl::String output_dir = JoinPath(test_dir.path(), "run-all-tests-output-1");
  EXPECT_EQ(0, MkDirAll(output_dir));

  const char* const argv[] = {"./runtests", "--all", "--output", output_dir.c_str()};
  TestStopwatch stopwatch;
  EXPECT_EQ(EXIT_SUCCESS,
            DiscoverAndRunTests(4, argv, {test_containing_dir.c_str()}, &stopwatch, ""));

  // Prepare the expected output.
  fbl::StringBuffer<1024> expected_output_buf;
  expected_output_buf.AppendPrintf(
      R"(
      "name": "%s",
      "duration_milliseconds": \d+)",
      test_name.c_str());
  re2::RE2 expected_output_regex(expected_output_buf.c_str());

  fbl::String test_data_sink_rel_path;
  ASSERT_TRUE(GetOutputFileRelPath(output_dir, "test", &test_data_sink_rel_path));

  fbl::StringBuffer<1024> expected_data_sink_buf;
  expected_data_sink_buf.AppendPrintf(
      "        \"test\": [\n"
      "          {\n"
      "            \"name\": \"test\",\n"
      "            \"file\": \"%s\"\n"
      "          }\n"
      "        ]",
      test_data_sink_rel_path.c_str());

  // Extract the actual output.
  const fbl::String output_path = JoinPath(output_dir, "summary.json");
  FILE* output_file = fopen(output_path.c_str(), "r");
  ASSERT_TRUE(output_file);
  char buf[1024];
  memset(buf, 0, sizeof(buf));
  EXPECT_LT(0, fread(buf, sizeof(buf[0]), sizeof(buf), output_file));
  fclose(output_file);

  EXPECT_TRUE(re2::RE2::PartialMatch(buf, expected_output_regex));
  EXPECT_SUBSTR(buf, expected_data_sink_buf.c_str());
}

TEST(RunTests, RunProfileMergeData) {
  ScopedTestDir test_dir(kTmp);
  fbl::String test_name = ProfileHelperBin();
  int num_failed = 0;
  fbl::Vector<std::unique_ptr<Result>> results;
  const fbl::String output_dir = JoinPath(test_dir.path(), "output");
  ASSERT_EQ(0, MkDirAll(output_dir));

  // Run the test for the first time.
  ASSERT_TRUE(RunTests({test_name, test_name}, {}, 1, 0, output_dir.c_str(), nullptr, &num_failed,
                       &results));
  EXPECT_EQ(0, num_failed);
  ASSERT_EQ(2, results.size());
  auto llvm_profile0 = results[0]->data_sinks.find("llvm-profile");
  ASSERT_NE(llvm_profile0, results[0]->data_sinks.end());
  ASSERT_EQ(1, llvm_profile0->second.size());

  // Run the test for the second time.
  ASSERT_TRUE(RunTests({test_name}, {}, 1, 0, output_dir.c_str(), nullptr, &num_failed, &results));
  EXPECT_EQ(0, num_failed);
  ASSERT_EQ(3, results.size());
  auto llvm_profile1 = results[1]->data_sinks.find("llvm-profile");
  ASSERT_NE(llvm_profile1, results[1]->data_sinks.end());
  ASSERT_EQ(1, llvm_profile1->second.size());

  // Check that the data was merged (i.e. they're the same).
  EXPECT_EQ(llvm_profile0->second[0].file, llvm_profile1->second[0].file);
}

TEST(RunTests, RunTestRootDir) {
  PackagedScriptFile test_script("test-root-dir.sh");
  fbl::String test_name = test_script.path();
  const char* argv[] = {test_name.c_str(), nullptr};
  ScopedTestDir test_dir(kTmp);

  // This test should have gotten TEST_ROOT_DIR. Confirm that we can find our
  // artifact in the "testdata/" directory under TEST_ROOT_DIR.
  const char* root_dir = getenv("TEST_ROOT_DIR");
  if (!root_dir) {
    root_dir = "";
  }

  // Run a test and confirm TEST_ROOT_DIR gets passed along.
  {
    std::unique_ptr<Result> result = RunTest(argv, nullptr, test_name.c_str(), 0, nullptr);

    EXPECT_STREQ(argv[0], result->name.c_str());
    EXPECT_EQ(SUCCESS, result->launch_status);
    EXPECT_EQ(0, result->return_code);
  }
}

}  // namespace
}  // namespace runtests
