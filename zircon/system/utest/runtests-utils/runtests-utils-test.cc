// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>

#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>
#include <fbl/vector.h>
#include <re2/re2.h>
#include <runtests-utils/runtests-utils.h>
#include <zxtest/zxtest.h>

#include "runtests-utils-test-utils.h"

namespace runtests {
namespace {

constexpr size_t kOneMegabyte = 1 << 20;

TEST(ParseTestNames, ParseTestNamesEmptyStr) {
  fbl::String input("");
  fbl::Vector<fbl::String> parsed;
  ParseTestNames(input, &parsed);
  EXPECT_EQ(0, parsed.size());
}

TEST(ParseTestNames, ParseTestNamesEmptyStrInMiddle) {
  fbl::String input("a,,b");
  fbl::Vector<fbl::String> parsed;
  ParseTestNames(input, &parsed);
  ASSERT_EQ(2, parsed.size());
  EXPECT_STREQ("a", parsed[0].c_str());
  EXPECT_STREQ("b", parsed[1].c_str());
}

TEST(ParseTestNames, ParseTestNamesTrailingComma) {
  fbl::String input("a,");
  fbl::Vector<fbl::String> parsed;
  ParseTestNames(input, &parsed);
  ASSERT_EQ(1, parsed.size());
  EXPECT_STREQ("a", parsed[0].c_str());
}

TEST(ParseTestNames, ParseTestNamesNormal) {
  fbl::String input("a,b");
  fbl::Vector<fbl::String> parsed;
  ParseTestNames(input, &parsed);
  ASSERT_EQ(2, parsed.size());
  EXPECT_STREQ("a", parsed[0].c_str());
  EXPECT_STREQ("b", parsed[1].c_str());
}

TEST(IsInWhitelist, EmptyWhitelist) {
  fbl::Vector<fbl::String> whitelist;
  EXPECT_FALSE(IsInWhitelist("a", whitelist));
}

TEST(IsInWhitelist, NonemptyWhitelist) {
  fbl::Vector<fbl::String> whitelist = {"b", "a"};
  EXPECT_TRUE(IsInWhitelist("a", whitelist));
}

TEST(JoinPath, JoinPathNoTrailingSlash) { EXPECT_STREQ("a/b/c/d", JoinPath("a/b", "c/d").c_str()); }

TEST(JoinPath, JoinPathTrailingSlash) { EXPECT_STREQ("a/b/c/d", JoinPath("a/b/", "c/d").c_str()); }

TEST(JoinPath, JoinPathAbsoluteChild) { EXPECT_STREQ("a/b/c/d", JoinPath("a/b/", "/c/d").c_str()); }

TEST(MkDirAll, MkDirAllTooLong) {
  char too_long[PATH_MAX + 1];
  memset(too_long, 'a', sizeof(too_long) - 1);
  too_long[sizeof(too_long) - 1] = '\0';
  EXPECT_EQ(ENAMETOOLONG, MkDirAll(too_long));
}

constexpr char kTmp[] = "/tmp";

TEST(MkDirAll, MkDirAllAlreadyExists) {
  ScopedTestDir test_dir(kTmp);
  const fbl::String already = JoinPath(test_dir.path(), "already");
  const fbl::String exists = JoinPath(already, "exists");
  ASSERT_EQ(0, mkdir(already.c_str(), 0755));
  ASSERT_EQ(0, mkdir(exists.c_str(), 0755));
  EXPECT_EQ(0, MkDirAll(exists));
}
TEST(MkDirAll, MkDirAllParentAlreadyExists) {
  ScopedTestDir test_dir(kTmp);
  const fbl::String parent = JoinPath(test_dir.path(), "existing-parent");
  const fbl::String child = JoinPath(parent, "child");
  ASSERT_EQ(0, mkdir(parent.c_str(), 0755));
  EXPECT_EQ(0, MkDirAll(child));
  struct stat s;
  EXPECT_EQ(0, stat(child.c_str(), &s));
}
TEST(MkDirAll, MkDirAllParentDoesNotExist) {
  ScopedTestDir test_dir(kTmp);
  const fbl::String parent = JoinPath(test_dir.path(), "not-existing-parent");
  const fbl::String child = JoinPath(parent, "child");
  struct stat s;
  ASSERT_NE(0, stat(parent.c_str(), &s));
  EXPECT_EQ(0, MkDirAll(child));
  EXPECT_EQ(0, stat(child.c_str(), &s));
}

TEST(WriteSummaryJSON, WriteSummaryJSONSucceeds) {
  // A reasonable guess that the function won't output more than this.
  std::unique_ptr<char[]> buf(new char[kOneMegabyte]);
  FILE* buf_file = fmemopen(buf.get(), kOneMegabyte, "w");
  fbl::Vector<std::unique_ptr<Result>> results;
  results.push_back(std::make_unique<Result>("/a", SUCCESS, 0, 10));
  results.push_back(std::make_unique<Result>("b", FAILED_TO_LAUNCH, 0, 0));
  ASSERT_EQ(0, WriteSummaryJSON(results, "/tmp/file_path", buf_file));
  fclose(buf_file);
  // We don't have a JSON parser in zircon right now, so just hard-code the
  // expected output.
  const char kExpectedJSONOutput[] = R"({
  "tests": [
    {
      "name": "/a",
      "duration_milliseconds": 10
    },
    {
      "name": "b",
      "duration_milliseconds": 0
    }
  ],
  "outputs": {
    "syslog_file": "/tmp/file_path"
  }
}
)";
  EXPECT_STREQ(kExpectedJSONOutput, buf.get());
}

TEST(WriteSummaryJSON, WriteSummaryJSONSucceedsWithoutSyslogPath) {
  // A reasonable guess that the function won't output more than this.
  std::unique_ptr<char[]> buf(new char[kOneMegabyte]);
  FILE* buf_file = fmemopen(buf.get(), kOneMegabyte, "w");
  fbl::Vector<std::unique_ptr<Result>> results;
  results.push_back(std::make_unique<Result>("/a", SUCCESS, 0, 10));
  results.push_back(std::make_unique<Result>("b", FAILED_TO_LAUNCH, 0, 0));
  ASSERT_EQ(0, WriteSummaryJSON(results, /*syslog_path=*/"", buf_file));
  fclose(buf_file);
  // With an empty syslog_path, we expect no values under "outputs" and
  // "syslog_file" to be generated in the JSON output.
  const char kExpectedJSONOutput[] = R"({
  "tests": [
    {
      "name": "/a",
      "duration_milliseconds": 10
    },
    {
      "name": "b",
      "duration_milliseconds": 0
    }
  ]
}
)";

  EXPECT_STREQ(kExpectedJSONOutput, buf.get());
}

TEST(ResolveGlobs, ResolveGlobsNoMatches) {
  ScopedTestDir test_dir(kTmp);
  fbl::Vector<fbl::String> resolved;
  fbl::String test_fs_glob = JoinPath(test_dir.path(), "bar*");
  const fbl::Vector<fbl::String> globs = {"/foo/bar/*", test_fs_glob};
  ASSERT_EQ(0, ResolveGlobs(globs, &resolved));
  EXPECT_EQ(0, resolved.size());
}

TEST(ResolveGlobs, ResolveGlobsMultipleMatches) {
  ScopedTestDir test_dir(kTmp);
  fbl::String existing_dir_path = JoinPath(test_dir.path(), "existing-dir/prefix-suffix");
  fbl::String existing_file_path = JoinPath(test_dir.path(), "existing-file");
  fbl::String existing_dir_glob = JoinPath(test_dir.path(), "existing-dir/prefix*");
  const fbl::Vector<fbl::String> globs = {"/does/not/exist/*",
                                          existing_dir_glob,  // matches existing_dir_path.
                                          existing_file_path};
  ASSERT_EQ(0, MkDirAll(existing_dir_path));
  const int existing_file_fd = open(existing_file_path.c_str(), O_CREAT, S_IRUSR | S_IWUSR);
  ASSERT_NE(-1, existing_file_fd, "%s", strerror(errno));
  ASSERT_NE(-1, close(existing_file_fd), "%s", strerror(errno));
  fbl::Vector<fbl::String> resolved;
  ASSERT_EQ(0, ResolveGlobs(globs, &resolved));
  ASSERT_EQ(2, resolved.size());
  EXPECT_STREQ(existing_dir_path.c_str(), resolved[0].c_str());
}

TEST(RunTest, RunTestSuccess) {
  PackagedScriptFile script_file("succeed.sh");
  fbl::String test_name = script_file.path();
  const char* argv[] = {test_name.c_str(), nullptr};
  std::unique_ptr<Result> result = RunTest(argv, nullptr, test_name.c_str(), 0, nullptr);
  EXPECT_STREQ(argv[0], result->name.c_str());
  EXPECT_EQ(SUCCESS, result->launch_status);
  EXPECT_EQ(0, result->return_code);
}

TEST(RunTest, RunTestTimeout) {
  // Test timeout is enforced if the test runs too long.
  PackagedScriptFile inf_loop_file("test-inf-loop.sh");
  fbl::String inf_loop_name = inf_loop_file.path();
  const char* inf_loop_argv[] = {inf_loop_name.c_str(), nullptr};
  std::unique_ptr<Result> result =
      RunTest(inf_loop_argv, nullptr, inf_loop_name.c_str(), 1, nullptr);
  EXPECT_STREQ(inf_loop_argv[0], result->name.c_str());
  EXPECT_EQ(TIMED_OUT, result->launch_status);
  EXPECT_EQ(0, result->return_code);

  // Test timeout is not enforced if the test finishes quickly.
  PackagedScriptFile success_file("succeed.sh");
  fbl::String succeed_name = success_file.path();
  const char* succeed_argv[] = {succeed_name.c_str(), nullptr};
  result = RunTest(succeed_argv, nullptr, succeed_name.c_str(), 100000, nullptr);
  EXPECT_STREQ(succeed_argv[0], result->name.c_str());
  EXPECT_EQ(SUCCESS, result->launch_status);
  EXPECT_EQ(0, result->return_code);
}

TEST(RunTest, RunTestFailure) {
  ScopedTestDir test_dir(kTmp);
  PackagedScriptFile script_file("expect-this-failure.sh");
  fbl::String test_name = script_file.path();
  const char* argv[] = {test_name.c_str(), nullptr};

  std::unique_ptr<Result> result = RunTest(argv, nullptr, test_name.c_str(), 0, nullptr);

  EXPECT_STREQ(argv[0], result->name.c_str());
  EXPECT_EQ(FAILED_NONZERO_RETURN_CODE, result->launch_status);
  EXPECT_EQ(77, result->return_code);
}

TEST(RunTest, RunTestFailureToLoadFile) {
  const char* argv[] = {"i/do/not/exist/", nullptr};

  std::unique_ptr<Result> result = RunTest(argv, nullptr, argv[0], 0, nullptr);
  EXPECT_STREQ(argv[0], result->name.c_str());
  EXPECT_EQ(FAILED_TO_LAUNCH, result->launch_status);
}

TEST(DiscoverTestsInDirGlobs, DiscoverTestsInDirGlobsBasic) {
  ScopedTestDir test_dir(kTmp);

  const fbl::String a_file_name = JoinPath(test_dir.path(), "a.sh");
  const fbl::String b_file_name = JoinPath(test_dir.path(), "b.sh");
  ScopedStubFile a_file(a_file_name);
  ScopedStubFile b_file(b_file_name);

  fbl::Vector<fbl::String> discovered_paths;
  EXPECT_EQ(0, DiscoverTestsInDirGlobs({test_dir.path()}, nullptr, {}, &discovered_paths));
  EXPECT_EQ(2, discovered_paths.size());
  bool discovered_a = false;
  bool discovered_b = false;
  // The order of the results is not defined, so just check that each is
  // present.
  for (const auto& path : discovered_paths) {
    if (path == a_file_name) {
      discovered_a = true;
    } else if (path == b_file_name) {
      discovered_b = true;
    }
  }
  EXPECT_TRUE(discovered_a);
  EXPECT_TRUE(discovered_b);
}

TEST(DiscoverTestsInDirGlobs, DiscoverTestsInDirGlobsFilter) {
  ScopedTestDir test_dir(kTmp);
  const char kHopefullyUniqueFileBasename[] = "e829cea9919fe045ca199945db7ac99a";
  const fbl::String unique_file_name = JoinPath(test_dir.path(), kHopefullyUniqueFileBasename);
  ScopedStubFile unique_file(unique_file_name);

  // This one should be ignored because its basename is not in the include list.
  const fbl::String other_file_name = JoinPath(test_dir.path(), "foo.sh");
  ScopedStubFile fail_file(other_file_name);

  fbl::Vector<fbl::String> discovered_paths;
  EXPECT_EQ(0, DiscoverTestsInDirGlobs({JoinPath(kTmp, "*")}, nullptr,
                                       {kHopefullyUniqueFileBasename}, &discovered_paths));
  EXPECT_EQ(1, discovered_paths.size());
  EXPECT_STREQ(unique_file_name.c_str(), discovered_paths[0].c_str());
}

TEST(DiscoverTestsInDirGlobs, DiscoverTestsInDirGlobsIgnore) {
  ScopedTestDir test_dir_a(kTmp), test_dir_b(kTmp);
  const fbl::String a_name = JoinPath(test_dir_a.path(), "foo.sh");
  ScopedStubFile a_file(a_name);
  const fbl::String b_name = JoinPath(test_dir_b.path(), "foo.sh");
  ScopedStubFile fail_file(b_name);
  fbl::Vector<fbl::String> discovered_paths;
  EXPECT_EQ(0, DiscoverTestsInDirGlobs({test_dir_a.path(), test_dir_b.path()},
                                       test_dir_b.basename(), {}, &discovered_paths));
  EXPECT_EQ(1, discovered_paths.size());
  EXPECT_STREQ(a_name.c_str(), discovered_paths[0].c_str());
}

TEST(RunTests, RunTestsWithArguments) {
  ScopedTestDir test_dir(kTmp);
  PackagedScriptFile succeed_script("succeed-with-echo.sh");
  const fbl::String succeed_file_name = succeed_script.path();
  int num_failed = 0;
  fbl::Vector<std::unique_ptr<Result>> results;
  fbl::Vector<fbl::String> args{"first", "second", "third", "-4", "--", "-", "seventh"};
  const fbl::String output_dir = JoinPath(test_dir.path(), "output");
  ASSERT_EQ(0, MkDirAll(output_dir));
  EXPECT_TRUE(RunTests({succeed_file_name}, args, 1, 0, output_dir.c_str(), nullptr, &num_failed,
                       &results));
  EXPECT_EQ(0, num_failed);
  EXPECT_EQ(1, results.size());
}

TEST(DiscoverAndRunTests, DiscoverAndRunTestsBasicPass) {
  // The build templates assemble two tests, a.sh and b.sh, in the
  // runtestsbasicpass/ subdirectory.
  const fbl::String script_dir = packaged_script_dir();
  const fbl::String test_script = JoinPath(script_dir, "runtestsbasicpass/a.sh");
  const char* const argv[] = {"./runtests", test_script.c_str()};
  TestStopwatch stopwatch;
  EXPECT_EQ(EXIT_SUCCESS, DiscoverAndRunTests(2, argv, {}, &stopwatch, ""));
}

TEST(DiscoverAndRunTests, DiscoverAndRunTestsBasicFail) {
  // The build templates assemble two tests, test-basic-succeed.sh and
  // test-basic-fail.sh, in the runtestsbasicfail/ subdirectory.
  const fbl::String script_dir = packaged_script_dir();
  const fbl::String test_script = JoinPath(script_dir, "runtestsbasicfail/test-basic-fail.sh");
  const char* const argv[] = {"./runtests", test_script.c_str()};
  TestStopwatch stopwatch;
  EXPECT_EQ(EXIT_FAILURE, DiscoverAndRunTests(2, argv, {}, &stopwatch, ""));
}

TEST(DiscoverAndRunTests, DiscoverAndRunTestsFailsWithNoTestOrDefaultDirs) {
  ScopedTestDir test_dir(kTmp);
  const fbl::String succeed_file_name = JoinPath(test_dir.path(), "succeed.sh");
  ScopedStubFile succeed_file(succeed_file_name);
  const char* const argv[] = {"./runtests"};
  TestStopwatch stopwatch;
  EXPECT_EQ(EXIT_FAILURE, DiscoverAndRunTests(1, argv, {}, &stopwatch, ""));
}

TEST(DiscoverAndRunTests, DiscoverAndRunTestsFailsWithBadArgs) {
  fbl::String script_dir = packaged_script_dir();
  const char* const argv[] = {"./runtests", "-?", "unknown-arg", script_dir.c_str()};
  TestStopwatch stopwatch;
  EXPECT_EQ(EXIT_FAILURE, DiscoverAndRunTests(4, argv, {}, &stopwatch, ""));
}

TEST(DiscoverAndRunTests, DiscoverAndRunTestsWithDefaultGlobs) {
  // There are three scripts generated in a directory by the build templates:
  //
  // testglobs/test-globs-root.sh
  // testglobs/A/B/C/test-globs-one.sh
  // testglobs/A/D/C/test-globs-two.sh
  //
  // Verify that we find and run all three.
  fbl::String all_scripts_dir = packaged_script_dir();
  fbl::String script_dir = JoinPath(all_scripts_dir, "testglobs");
  fbl::String glob = JoinPath(script_dir, "A/*/C");
  const char* const argv[] = {"./runtests", "--all"};
  TestStopwatch stopwatch;
  EXPECT_EQ(EXIT_SUCCESS,
            DiscoverAndRunTests(2, argv, {script_dir.c_str(), glob.c_str()}, &stopwatch, ""));
}

// Passing an --output argument should result in output being written to that
// location.
TEST(DiscoverAndRunTests, DiscoverAndRunTestsWithOutput) {
  fbl::String all_scripts_dir = packaged_script_dir();
  fbl::String script_dir = JoinPath(all_scripts_dir, "testwithoutput");

  ScopedTestDir test_dir(kTmp);

  const fbl::String succeed_file_name = JoinPath(script_dir, "test-with-output-succeed.sh");
  const fbl::String fail_file_name = JoinPath(script_dir, "test-with-output-fail.sh");
  const fbl::String output_dir = JoinPath(test_dir.path(), "run-all-tests-output-1");
  EXPECT_EQ(0, MkDirAll(output_dir));

  const char* const argv[] = {"./runtests", "--all", "--output", output_dir.c_str()};
  TestStopwatch stopwatch;
  EXPECT_EQ(EXIT_FAILURE, DiscoverAndRunTests(4, argv, {script_dir.c_str()}, &stopwatch, ""));

  // Prepare the expected output.
  char expected_pass_output_buf[1024];
  sprintf(expected_pass_output_buf,
          R"(    \{
      "name": "%s",
      "duration_milliseconds": \d+
    \})",
          succeed_file_name.c_str());
  re2::RE2 expected_pass_output_regex(expected_pass_output_buf);

  char expected_fail_output_buf[1024];
  sprintf(expected_fail_output_buf,
          R"(    \{
      "name": "%s",
      "duration_milliseconds": \d+
    \})",
          fail_file_name.c_str());
  re2::RE2 expected_fail_output_regex(expected_fail_output_buf);

  // Extract the actual output.
  const fbl::String output_path = JoinPath(output_dir, "summary.json");
  FILE* output_file = fopen(output_path.c_str(), "r");
  ASSERT_TRUE(output_file);
  char buf[1024];
  memset(buf, 0, sizeof(buf));
  EXPECT_LT(0, fread(buf, sizeof(buf[0]), sizeof(buf), output_file));
  fclose(output_file);

  // The order of the tests in summary.json is not defined, so first check the
  // prefix, then be permissive about order of the actual tests.
  EXPECT_EQ(0, strncmp(kExpectedJSONOutputPrefix, buf, kExpectedJSONOutputPrefixSize));

  re2::StringPiece buf_for_pass(buf);
  re2::StringPiece buf_for_fail(buf);

  EXPECT_TRUE(re2::RE2::FindAndConsume(&buf_for_pass, expected_pass_output_regex));
  EXPECT_TRUE(re2::RE2::FindAndConsume(&buf_for_fail, expected_fail_output_regex));

  auto outputs_end = buf_for_pass.length() < buf_for_fail.length() ? buf_for_pass : buf_for_fail;
  EXPECT_STREQ("\n  ]\n}\n", outputs_end.data());
}

// Passing an --output argument *and* a syslog file name should result in output being
// written that includes a syslog reference.
TEST(DiscoverAndRunTests, DiscoverAndRunTestsWithSyslogOutput) {
  fbl::String all_scripts_dir = packaged_script_dir();
  fbl::String script_dir = JoinPath(all_scripts_dir, "testwithoutput");

  ScopedTestDir test_dir(kTmp);
  const fbl::String succeed_file_name = JoinPath(script_dir, "test-with-output-succeed.sh");
  const fbl::String fail_file_name = JoinPath(script_dir, "test-with-output-fail.sh");
  const fbl::String output_dir = JoinPath(test_dir.path(), "run-all-tests-output-2");
  EXPECT_EQ(0, MkDirAll(output_dir));

  const char* const argv[] = {"./runtests", "--all", "--output", output_dir.c_str()};
  TestStopwatch stopwatch;
  EXPECT_EQ(EXIT_FAILURE,
            DiscoverAndRunTests(4, argv, {script_dir.c_str()}, &stopwatch, "syslog.txt"));

  // Prepare the expected output.
  const char kExpectedOutputsStr[] =
      "  \"outputs\": {\n"
      "    \"syslog_file\": \"syslog.txt\"\n"
      "  }";

  // Extract the actual output.
  const fbl::String output_path = JoinPath(output_dir, "summary.json");
  FILE* output_file = fopen(output_path.c_str(), "r");
  ASSERT_TRUE(output_file);
  char buf[1024];
  memset(buf, 0, sizeof(buf));
  EXPECT_LT(0, fread(buf, sizeof(buf[0]), sizeof(buf), output_file));
  fclose(output_file);

  // We don't actually care if the string is at the beginning or the end of
  // the JSON, so just search for it anywhere.
  bool found_expected_outputs_str = false;
  for (size_t buf_index = 0; buf[buf_index]; ++buf_index) {
    if (!strncmp(kExpectedOutputsStr, &buf[buf_index], sizeof(kExpectedOutputsStr) - 1)) {
      found_expected_outputs_str = true;
      break;
    }
  }
  if (!found_expected_outputs_str) {
    printf("Unexpected buffer contents: %s\n", buf);
  }
  EXPECT_TRUE(found_expected_outputs_str, "Didn't find expected outputs str in buf");
}
}  // namespace
}  // namespace runtests
