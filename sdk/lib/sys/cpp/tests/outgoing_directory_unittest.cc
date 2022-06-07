// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fdio/directory.h>
#include <lib/fidl/cpp/message_buffer.h>
#include <lib/sys/cpp/outgoing_directory.h>
#include <lib/zx/channel.h>

#include <gtest/gtest.h>

#include "echo_server.h"
#include "src/lib/testing/loop_fixture/real_loop_fixture.h"

namespace {

using OutgoingDirectorySetupTest = gtest::RealLoopFixture;

class OutgoingDirectoryTest : public gtest::RealLoopFixture {
 protected:
  void SetUp() override {
    gtest::RealLoopFixture::SetUp();
    zx::channel svc_server;
    ASSERT_EQ(ZX_OK, zx::channel::create(0, &svc_client_, &svc_server));

    ASSERT_EQ(ZX_OK, outgoing_.Serve(std::move(svc_server), dispatcher()));
  }

  void TestCanAccessEchoService(const char* service_path, bool succeeds = true) {
    test::placeholders::EchoPtr echo;
    fdio_service_connect_at(svc_client_.get(), service_path,
                            echo.NewRequest(dispatcher()).TakeChannel().release());

    std::string result = "no callback";
    echo->EchoString("hello", [&result](fidl::StringPtr value) { result = *value; });

    RunLoopUntilIdle();
    EXPECT_EQ(succeeds ? "hello" : "no callback", result);
  }

  void AddEchoService(vfs::PseudoDir* dir) {
    ASSERT_EQ(ZX_OK,
              dir->AddEntry(test::placeholders::Echo::Name_,
                            std::make_unique<vfs::Service>(echo_impl_.GetHandler(dispatcher()))));
  }

  EchoImpl echo_impl_;
  zx::channel svc_client_;
  sys::OutgoingDirectory outgoing_;
};

TEST_F(OutgoingDirectoryTest, Control) {
  ASSERT_EQ(ZX_OK, outgoing_.AddPublicService(echo_impl_.GetHandler(dispatcher())));

  TestCanAccessEchoService("svc/test.placeholders.Echo");

  // Ensure GetOrCreateDirectory refers to the same "svc" directory.
  outgoing_.GetOrCreateDirectory("svc")->RemoveEntry("test.placeholders.Echo");
  TestCanAccessEchoService("svc/test.placeholders.Echo", false);
}

TEST_F(OutgoingDirectoryTest, AddAndRemove) {
  ASSERT_EQ(ZX_ERR_NOT_FOUND, outgoing_.RemovePublicService<test::placeholders::Echo>());

  ASSERT_EQ(ZX_OK, outgoing_.AddPublicService(echo_impl_.GetHandler(dispatcher())));

  ASSERT_EQ(ZX_ERR_ALREADY_EXISTS, outgoing_.AddPublicService(echo_impl_.GetHandler(dispatcher())));

  TestCanAccessEchoService("svc/test.placeholders.Echo");

  ASSERT_EQ(ZX_OK, outgoing_.RemovePublicService<test::placeholders::Echo>());
  ASSERT_EQ(ZX_ERR_NOT_FOUND, outgoing_.RemovePublicService<test::placeholders::Echo>());

  TestCanAccessEchoService("svc/test.placeholders.Echo", false);
}

TEST_F(OutgoingDirectoryTest, DebugDir) {
  AddEchoService(outgoing_.debug_dir());

  TestCanAccessEchoService("debug/test.placeholders.Echo");
  outgoing_.GetOrCreateDirectory("debug")->RemoveEntry("test.placeholders.Echo");
  TestCanAccessEchoService("debug/test.placeholders.Echo", false);
}

TEST_F(OutgoingDirectoryTest, GetOrCreateDirectory) {
  outgoing_.GetOrCreateDirectory("objects")->AddEntry(
      "test_svc_a", std::make_unique<vfs::Service>(echo_impl_.GetHandler(dispatcher())));
  outgoing_.GetOrCreateDirectory("objects")->AddEntry(
      "test_svc_b", std::make_unique<vfs::Service>(echo_impl_.GetHandler(dispatcher())));
  TestCanAccessEchoService("objects/test_svc_a");
  TestCanAccessEchoService("objects/test_svc_b");
}

TEST_F(OutgoingDirectorySetupTest, Invalid) {
  sys::OutgoingDirectory outgoing;
  ASSERT_EQ(ZX_ERR_BAD_HANDLE, outgoing.Serve(zx::channel(), dispatcher()));
}

TEST_F(OutgoingDirectorySetupTest, AccessDenied) {
  zx::channel svc_client, svc_server;
  ASSERT_EQ(ZX_OK, zx::channel::create(0, &svc_client, &svc_server));

  svc_server.replace(ZX_RIGHT_NONE, &svc_server);

  sys::OutgoingDirectory outgoing;
  ASSERT_EQ(ZX_ERR_ACCESS_DENIED, outgoing.Serve(std::move(svc_server), dispatcher()));
}

}  // namespace
