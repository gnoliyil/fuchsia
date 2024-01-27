// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/default.h>
#include <lib/async_patterns/testing/cpp/dispatcher_bound.h>
#include <lib/driver/component/cpp/driver_base.h>
#include <lib/driver/component/cpp/tests/test_driver.h>
#include <lib/driver/testing/cpp/driver_lifecycle.h>
#include <lib/driver/testing/cpp/driver_runtime.h>
#include <lib/driver/testing/cpp/test_environment.h>
#include <lib/driver/testing/cpp/test_node.h>
#include <lib/fdf/env.h>
#include <lib/fdf/testing.h>

#include <gtest/gtest.h>

// This is the recommended setup if you have a driver that doesn't need to make synchronous FIDL
// calls. Everything runs on the main test thread so everything is safe to access directly.
class TestForegroundDispatcher : public ::testing::Test {
 public:
  void SetUp() override {
    // Create start args
    zx::result start_args = node_server_.CreateStartArgsAndServe();
    EXPECT_EQ(ZX_OK, start_args.status_value());

    // Start the test environment
    zx::result result =
        test_environment_.Initialize(std::move(start_args->incoming_directory_server));
    EXPECT_EQ(ZX_OK, result.status_value());

    // Start driver
    zx::result start_result =
        runtime_.RunToCompletion(driver_.Start(std::move(start_args->start_args)));
    EXPECT_EQ(ZX_OK, start_result.status_value());
  }

  void TearDown() override {
    zx::result prepare_stop_result = runtime_.RunToCompletion(driver_.PrepareStop());
    EXPECT_EQ(ZX_OK, prepare_stop_result.status_value());
  }

  fdf_testing::DriverUnderTest<TestDriver>& driver() { return driver_; }

 private:
  // Attaches a foreground dispatcher for us automatically.
  fdf_testing::DriverRuntime runtime_;

  // These will use the foreground dispatcher.
  fdf_testing::TestNode node_server_{"root"};
  fdf_testing::TestEnvironment test_environment_;
  fdf_testing::DriverUnderTest<TestDriver> driver_;
};

TEST_F(TestForegroundDispatcher, CreateChildNodeAsync) {
  // Safe to touch driver since the dispatcher is in the foreground.
  // Dispatcher does not allow sync calls from the driver so we have to use the async version.
  driver()->CreateChildNodeAsync();
  while (!driver()->async_added_child()) {
    fdf_testing_run_until_idle();
  }
}

// If the environment needs to run on a background dispatcher (for example if the driver needs
// to make sync FIDL calls), we need to run the environment on a background dispatcher while keeping
// the driver on the main thread.
class TestForegroundDriverBackgroundEnv : public ::testing::Test {
 public:
  void SetUp() override {
    // Create start args
    zx::result start_args = node_server_.SyncCall(&fdf_testing::TestNode::CreateStartArgsAndServe);
    EXPECT_EQ(ZX_OK, start_args.status_value());

    // Start the test environment
    zx::result init_result =
        test_environment_.SyncCall(&fdf_testing::TestEnvironment::Initialize,
                                   std::move(start_args->incoming_directory_server));
    EXPECT_EQ(ZX_OK, init_result.status_value());

    zx::result start_result =
        runtime_.RunToCompletion(driver_.Start(std::move(start_args->start_args)));
    EXPECT_EQ(ZX_OK, start_result.status_value());
  }

  void TearDown() override {
    zx::result prepare_stop_result = runtime_.RunToCompletion(driver_.PrepareStop());
    EXPECT_EQ(ZX_OK, prepare_stop_result.status_value());
  }

  fdf_testing::DriverUnderTest<TestDriver>& driver() { return driver_; }

  async_dispatcher_t* env_dispatcher() { return env_dispatcher_->async_dispatcher(); }

 private:
  // Attaches a foreground dispatcher for us automatically.
  fdf_testing::DriverRuntime runtime_;

  // Env dispatcher runs in the background because we need to make sync calls into it.
  fdf::UnownedSynchronizedDispatcher env_dispatcher_ = runtime_.StartBackgroundDispatcher();

  async_patterns::TestDispatcherBound<fdf_testing::TestNode> node_server_{
      env_dispatcher(), std::in_place, std::string("root")};

  async_patterns::TestDispatcherBound<fdf_testing::TestEnvironment> test_environment_{
      env_dispatcher(), std::in_place};

  fdf_testing::DriverUnderTest<TestDriver> driver_;
};

TEST_F(TestForegroundDriverBackgroundEnv, CreateChildNodeSync) {
  // Safe to touch the driver from here since the dispatcher is in the foreground.
  // Dispatcher allows sync calls from the driver so we use the sync version.
  driver()->CreateChildNodeSync();
  EXPECT_TRUE(driver()->sync_added_child());
}
