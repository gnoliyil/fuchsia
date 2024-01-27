// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_TEST_SUITE_TEST_SUITE_H_
#define SRC_LIB_TEST_SUITE_TEST_SUITE_H_

#include <fuchsia/test/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/binding_set.h>

#include <string>
#include <unordered_set>

namespace example {

struct Options {
  bool dont_service_get_tests = false;
  bool dont_service_run = false;
  bool close_channel_get_tests = false;
  bool close_channel_run = false;
  bool dont_send_on_finish_event = false;
};

struct TestInput {
  std::string name;
  fuchsia::test::Status status;
  // Treat as disabled by developer if true
  bool disabled = false;
  /// Skips OnTestCaseFinished if true
  bool incomplete_test = false;
  // will not set status if false.
  bool set_result_status = true;
  // will not write to stderr if false.
  bool write_stderr_logs = false;
};

class CaseIterator : public fuchsia::test::CaseIterator {
 public:
  CaseIterator(const std::vector<TestInput>& test_inputs,
               fit::function<void(CaseIterator*)> done_callback)
      : test_inputs_(test_inputs),
        iter_(test_inputs_.begin()),
        done_callback_(std::move(done_callback)) {}

  void GetNext(GetNextCallback callback) override;

 private:
  const std::vector<TestInput>& test_inputs_;
  std::vector<TestInput>::const_iterator iter_;
  fit::function<void(CaseIterator*)> done_callback_;
};

class TestSuite : public fuchsia::test::Suite {
 public:
  explicit TestSuite(async::Loop* loop, std::vector<TestInput> inputs, Options options = Options{});

  void GetTests(::fidl::InterfaceRequest<fuchsia::test::CaseIterator>) override;

  void Run(std::vector<fuchsia::test::Invocation> tests, fuchsia::test::RunOptions /*unused*/,
           fidl::InterfaceHandle<fuchsia::test::RunListener> run_listener) override;

  fidl::InterfaceRequestHandler<fuchsia::test::Suite> GetHandler();

 private:
  bool ShouldSkipTest(const fuchsia::test::RunOptions& run_options,
                      const std::string& test_name) const;

  fidl::Binding<fuchsia::test::Suite> binding_;
  fidl::BindingSet<fuchsia::test::CaseIterator, std::unique_ptr<CaseIterator>>
      case_iterator_bindings_;
  const std::vector<TestInput> test_inputs_;
  std::unordered_set<std::string> disabled_tests_;
  Options options_;
  async::Loop* loop_;
};

}  // namespace example

#endif  // SRC_LIB_TEST_SUITE_TEST_SUITE_H_
