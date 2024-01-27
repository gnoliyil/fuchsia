// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.heapdump.process/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/zx/channel.h>
#include <lib/zx/eventpair.h>

#include <gtest/gtest.h>
#include <heapdump/bind_with_channel.h>

// Why death tests?
//
// All the tests in this file call heapdump_bind_with_channel, which can only be called once per
// process. Calling it multiple times within the same process results in a panic, by design (as the
// "BindTwiceShouldCrash" test below verifies).
//
// Therefore, each test must run in isolation as a separate process. Since gtest runs death tests in
// subprocesses, expressing each test as a death test (ASSERT_EXIT or ASSERT_DEATH) is a convenient
// way to obtain this level of isolation.

// Verify that binding to a registry given a channel results in the expected message being written.
TEST(BindTest, Success) {
  const auto action = [] {
    zx::channel client, server;
    ASSERT_EQ(ZX_OK, zx::channel::create(0, &client, &server));
    heapdump_bind_with_channel(client.release());

    class FakeRegistry : public fidl::WireServer<fuchsia_heapdump_process::Registry> {
      void RegisterV1(fuchsia_heapdump_process::wire::RegistryRegisterV1Request*,
                      RegisterV1Completer::Sync&) override {
        _exit(99);  // Test passed: exit with a known exit code.
      }
    };

    async::Loop loop = async::Loop(&kAsyncLoopConfigNeverAttachToThread);
    fidl::BindServer(loop.dispatcher(),
                     fidl::ServerEnd<fuchsia_heapdump_process::Registry>(std::move(server)),
                     std::make_unique<FakeRegistry>());
    loop.Run();
  };

  // Run action() in a subprocess.
  ASSERT_EXIT(action(), testing::ExitedWithCode(99), "");
}

// Verify that passing an invalid handle does not make the program crash.
TEST(BindTest, InvalidHandle) {
  const auto action = [] {
    heapdump_bind_with_channel(ZX_HANDLE_INVALID);
    _exit(99);  // Test passed: exit with a known exit code.
  };

  // Run action() in a subprocess.
  ASSERT_EXIT(action(), testing::ExitedWithCode(99), "");
}

// Verify that attempting to bind with a non-channel handle results in a crash.
TEST(BindTest, BadHandleTypeShouldCrash) {
  const auto action = [] {
    zx::eventpair ev1, ev2;
    ASSERT_EQ(ZX_OK, zx::eventpair::create(0, &ev1, &ev2));
    heapdump_bind_with_channel(ev1.release());
  };

  // Run action() in a subprocess.
  ASSERT_DEATH(action(), "");
}

// Verify that attempting to bind twice results in a crash.
TEST(BindTest, BindTwiceShouldCrash) {
  const auto action = [] {
    heapdump_bind_with_channel(ZX_HANDLE_INVALID);
    heapdump_bind_with_channel(ZX_HANDLE_INVALID);
  };

  // Run action() in a subprocess and expect a panic.
  ASSERT_DEATH(action(), "panic");
}
