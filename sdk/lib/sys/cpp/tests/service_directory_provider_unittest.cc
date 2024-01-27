// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async/dispatcher.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/sys/cpp/testing/service_directory_provider.h>
#include <lib/vfs/cpp/service.h>
#include <zircon/types.h>

#include <memory>

#include <gtest/gtest.h>

#include "echo_server.h"
#include "src/lib/testing/loop_fixture/real_loop_fixture.h"

namespace {

using echo_server::EchoImpl;

class ServiceDirectoryProviderTests : public gtest::RealLoopFixture {
 protected:
  void ConnectToService(const std::shared_ptr<sys::ServiceDirectory>& svc,
                        test::placeholders::EchoPtr& echo) {
    svc->Connect(echo.NewRequest());
  }

  EchoImpl echo_impl_;
};

TEST_F(ServiceDirectoryProviderTests, TestInjectedServiceUsingMethod1) {
  sys::testing::ServiceDirectoryProvider svc_provider_;

  ASSERT_EQ(ZX_OK, svc_provider_.AddService(echo_impl_.GetHandler(dispatcher())));

  test::placeholders::EchoPtr echo;

  ConnectToService(svc_provider_.service_directory(), echo);

  std::string result;
  echo->EchoString("hello", [&result](fidl::StringPtr value) { result = *value; });

  RunLoopUntilIdle();
  EXPECT_EQ("hello", result);
}

TEST_F(ServiceDirectoryProviderTests, TestInjectedServiceUsingMethod2) {
  sys::testing::ServiceDirectoryProvider svc_provider_;

  ASSERT_EQ(ZX_OK,
            svc_provider_.AddService(std::make_unique<vfs::Service>(
                                         [&](zx::channel channel, async_dispatcher_t* dispatcher) {
                                           echo_impl_.AddBinding((std::move(channel)), dispatcher);
                                         }),
                                     echo_impl_.Name_));

  test::placeholders::EchoPtr echo;

  ConnectToService(svc_provider_.service_directory(), echo);

  std::string result;
  echo->EchoString("hello", [&result](fidl::StringPtr value) { result = *value; });

  RunLoopUntilIdle();
  EXPECT_EQ("hello", result);
}

}  // namespace
