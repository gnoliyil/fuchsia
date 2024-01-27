// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fidl.service.test/cpp/fidl.h>
#include <fidl/fidl.service.test/cpp/wire_test_base.h>
#include <lib/component/incoming/cpp/clone.h>
#include <lib/component/incoming/cpp/constants.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/component/incoming/cpp/service.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>

#include <gtest/gtest.h>
#include <src/lib/testing/loop_fixture/real_loop_fixture.h>
#include <src/lib/testing/predicates/status.h>

#include "echo_server.h"

namespace {

using Echo = fidl_service_test::Echo;
constexpr char kTestString[] = "foobar";

class IncomingTest : public gtest::RealLoopFixture {
 public:
  IncomingTest() : outgoing_(dispatcher()) {
    auto directory = fidl::CreateEndpoints<fuchsia_io::Directory>();
    auto result = outgoing()->Serve(std::move(directory->server));
    ZX_ASSERT_MSG(result.status_value() == ZX_OK, "Failed to serve outgoing: %s",
                  result.status_string());
    outgoing_client_.Bind(std::move(directory->client));
  }

 protected:
  component::OutgoingDirectory* outgoing() { return &outgoing_; }

  fidl::ClientEnd<fuchsia_io::Directory> TakeSvcDirectoryRoot() {
    return PerformBlockingWork([&]() {
      zx::result endpoints = fidl::CreateEndpoints<fuchsia_io::Node>();
      ZX_ASSERT_MSG(endpoints.is_ok(), "Failed to create fuchsia.io/Node endpoints: %s",
                    endpoints.status_string());
      auto& [client, server] = endpoints.value();
      fidl::Request<fuchsia_io::Directory::Open> request(
          /*flags=*/fuchsia_io::OpenFlags{},
          /*mode=*/fuchsia_io::ModeType{},
          /*path=*/component::OutgoingDirectory::kServiceDirectory,
          /*object=*/std::move(server));
      fit::result status = outgoing_client_->Open(std::move(request));
      ZX_ASSERT_MSG(status.is_ok(), "Failed to invoke fuchsia.io/Directory.Open: %s",
                    status.error_value().FormatDescription().c_str());
      return fidl::ClientEnd<fuchsia_io::Directory>(client.TakeChannel());
    });
  }

 private:
  component::OutgoingDirectory outgoing_;
  fidl::SyncClient<fuchsia_io::Directory> outgoing_client_;
};

class IncomingProtocolTest : public IncomingTest {
 public:
  void SetUp() override {
    ASSERT_OK(
        outgoing()->AddProtocol<Echo>(std::make_unique<EchoCommon>(dispatcher())).status_value());
  }
};

TEST_F(IncomingProtocolTest, ConnectAtWithNaturalClient) {
  auto client_end = component::ConnectAt<Echo>(TakeSvcDirectoryRoot());
  ASSERT_TRUE(client_end.is_ok());

  fidl::Client<Echo> client(std::move(client_end.value()), dispatcher());
  client->EchoString(fidl::Request<Echo::EchoString>(kTestString))
      .Then([quit_loop = QuitLoopClosure()](fidl::Result<Echo::EchoString>& result) {
        EXPECT_EQ(std::string(kTestString), result->response()->data());
        quit_loop();
      });

  RunLoop();
}

TEST_F(IncomingProtocolTest, ConnectAtWithWireClient) {
  auto client_end = component::ConnectAt<Echo>(TakeSvcDirectoryRoot());
  ASSERT_TRUE(client_end.is_ok());

  fidl::WireClient<Echo> client(std::move(client_end.value()), dispatcher());
  client->EchoString(kTestString)
      .Then([quit_loop = QuitLoopClosure()](fidl::WireUnownedResult<Echo::EchoString>& result) {
        EXPECT_EQ(std::string(kTestString), result->response.data());
        quit_loop();
      });

  RunLoop();
}

TEST_F(IncomingProtocolTest, ConnectAtUsesServerEndForRequest) {
  auto endpoints = fidl::CreateEndpoints<Echo>();
  ASSERT_OK(component::ConnectAt<Echo>(TakeSvcDirectoryRoot(), std::move(endpoints->server))
                .status_value());

  fidl::Client<Echo> client(std::move(endpoints->client), dispatcher());
  client->EchoString(fidl::Request<Echo::EchoString>(kTestString))
      .Then([quit_loop = QuitLoopClosure()](fidl::Result<Echo::EchoString>& result) {
        EXPECT_EQ(std::string(kTestString), result->response()->data());
        quit_loop();
      });

  RunLoop();
}

TEST_F(IncomingProtocolTest, ConnectReturnsErrorIfPathInvalid) {
  EXPECT_TRUE(component::Connect<Echo>("no_leading_slash").is_error());
  EXPECT_TRUE(component::Connect<Echo>("/with_trailing_slash/").is_error());
  EXPECT_TRUE(component::Connect<Echo>("//").is_error());
  EXPECT_TRUE(component::Connect<Echo>("").is_error());
}

TEST_F(IncomingProtocolTest, CloneWorksForCloneableProtocol) {
  auto client_end = component::ConnectAt<Echo>(TakeSvcDirectoryRoot());
  ASSERT_OK(client_end.status_value());

  auto clone_result = component::Clone(*client_end);
  ASSERT_OK(clone_result.status_value());

  fidl::Client<Echo> client(std::move(clone_result.value()), dispatcher());
  client->EchoString(fidl::Request<Echo::EchoString>(kTestString))
      .Then([quit_loop = QuitLoopClosure()](fidl::Result<Echo::EchoString>& result) {
        EXPECT_EQ(std::string(kTestString), result->response()->data());
        quit_loop();
      });

  RunLoop();
}

TEST_F(IncomingProtocolTest, CloneWorksForServiceDirectories) {
  auto svc = TakeSvcDirectoryRoot();

  auto svc_clone = component::Clone(svc);

  ASSERT_OK(svc_clone.status_value());
  static_assert(
      std::is_same_v<decltype(svc_clone.value()), fidl::ClientEnd<fuchsia_io::Directory>&>);

  ASSERT_NE(svc.channel(), svc_clone->channel().get());
  auto client_end = component::ConnectAt<Echo>(*svc_clone);
  ASSERT_OK(client_end.status_value());
  fidl::Client<Echo> client(std::move(client_end.value()), dispatcher());
  client->EchoString(fidl::Request<Echo::EchoString>(kTestString))
      .Then([quit_loop = QuitLoopClosure()](fidl::Result<Echo::EchoString>& result) {
        EXPECT_EQ(std::string(kTestString), result->response()->data());
        quit_loop();
      });

  RunLoop();
}

using Empty = fidl_service_test::Empty;
using EmptyCloneableNode = fidl_service_test::EmptyCloneableNode;

// |EmptyCloneableNode| server that asserts that only |fuchsia.io/Node.Clone| is called.

class EmptyCloneableNodeImpl : public fidl::testing::WireTestBase<EmptyCloneableNode> {
 public:
  explicit EmptyCloneableNodeImpl(async_dispatcher_t* dispatcher) : dispatcher_(dispatcher) {}
  fidl::ServerBindingRef<EmptyCloneableNode> Connect(fidl::ServerEnd<EmptyCloneableNode> request) {
    return fidl::BindServer(dispatcher_, std::move(request), this);
  }

  void Clone2(Clone2RequestView request, Clone2Completer::Sync& completer) override {
    GTEST_FAIL();
    completer.Close(ZX_ERR_BAD_STATE);
  }

  void Clone(CloneRequestView request, CloneCompleter::Sync& completer) override {
    fidl::BindServer(dispatcher_,
                     fidl::ServerEnd<EmptyCloneableNode>(request->object.TakeChannel()), this);
  }

  void NotImplemented_(const std::string& name, fidl::CompleterBase& completer) override {
    GTEST_FAIL();
    completer.Close(ZX_ERR_BAD_STATE);
  }

 private:
  async_dispatcher_t* dispatcher_;
};

class IncomingCloneTest : public IncomingTest {
 public:
  void SetUp() override {
    ASSERT_OK(outgoing()
                  ->AddProtocol<EmptyCloneableNode>(
                      std::make_unique<EmptyCloneableNodeImpl>(dispatcher()), kProtocolName)
                  .status_value());
  }

 protected:
  constexpr static const char kProtocolName[] = "empty";
};

TEST_F(IncomingCloneTest, CloneWithTagAssumeProtocolComposesNodeAlwaysDispatchesToNode) {
  // Manually convert to the |Empty| protocol.
  auto empty = component::ConnectAt<Empty>(TakeSvcDirectoryRoot(), kProtocolName);
  ASSERT_OK(empty.status_value());

  auto empty_clone = component::Clone<Empty>(empty.value(), component::AssumeProtocolComposesNode);
  ASSERT_OK(empty_clone.status_value());

  // The channel should not have been closed with an error.
  ASSERT_STATUS(empty_clone->channel().wait_one(ZX_CHANNEL_PEER_CLOSED,
                                                zx::deadline_after(zx::msec(500)), nullptr),
                ZX_ERR_TIMED_OUT);
}

TEST_F(IncomingCloneTest, CloneReturnsPeerClosedIfOtherEndClosesChannel) {
  auto bad_endpoint = fidl::CreateEndpoints<fuchsia_io::Directory>();
  bad_endpoint->server.reset();

  // To mitigate race conditions, the FIDL bindings do not expose PEER_CLOSED
  // errors when writing. The behavior is thus that the cloning succeeds, but
  // the returned endpoint is always closed. This behavior will be consistent
  // regardless if the server closed the |bad_endpoint->server| before or after
  // the |component::Clone| call.
  {
    auto failure = component::Clone(bad_endpoint->client);
    ASSERT_OK(failure.status_value());
    zx_signals_t observed = ZX_SIGNAL_NONE;
    ASSERT_OK(
        failure->channel().wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time::infinite_past(), &observed));
    ASSERT_EQ(ZX_CHANNEL_PEER_CLOSED, observed);
  }
  {
    auto failure = component::MaybeClone(bad_endpoint->client);
    ASSERT_TRUE(failure.is_valid());
    zx_signals_t observed = ZX_SIGNAL_NONE;
    ASSERT_OK(
        failure.channel().wait_one(ZX_CHANNEL_PEER_CLOSED, zx::time::infinite_past(), &observed));
    ASSERT_EQ(ZX_CHANNEL_PEER_CLOSED, observed);
  }
}

TEST_F(IncomingCloneTest, CloneReturnsError) {
  auto bad_endpoint = fidl::CreateEndpoints<fuchsia_io::Directory>();
  zx::channel result;
  ASSERT_OK(bad_endpoint->client.TakeHandle().replace(ZX_RIGHT_NONE, &result));
  bad_endpoint->client.reset(result.release());
  auto failure = component::Clone(bad_endpoint->client);
  EXPECT_STATUS(failure.status_value(), ZX_ERR_ACCESS_DENIED);
  auto invalid = component::MaybeClone(bad_endpoint->client);
  ASSERT_FALSE(invalid.is_valid());
}

using EchoService = fidl_service_test::EchoService;

constexpr char kDefaultInstanceName[] = "default";
constexpr char kOtherInstanceName[] = "other";

class IncomingServiceTest : public IncomingTest {
 public:
  IncomingServiceTest() = default;

  void SetUp() override {
    auto create_echo_server = [this](const char* prefix) {
      auto& echo = echo_servers_.emplace_back(std::make_unique<EchoCommon>(prefix, dispatcher()));
      return echo->CreateHandler();
    };
    ASSERT_OK(outgoing()
                  ->AddService<EchoService>(
                      EchoService::InstanceHandler({.foo = create_echo_server("default-foo"),
                                                    .bar = create_echo_server("default-bar")}),
                      kDefaultInstanceName)
                  .status_value());
    ASSERT_OK(outgoing()
                  ->AddService<EchoService>(
                      EchoService::InstanceHandler({.foo = create_echo_server("other-foo"),
                                                    .bar = create_echo_server("other-bar")}),
                      kOtherInstanceName)
                  .status_value());
  }

 protected:
 private:
  // EchoCommon is wrapped in a unique_ptr because it's not moveable nor copyable.
  // Therefore, vector can't copy the object during resize.
  std::vector<std::unique_ptr<EchoCommon>> echo_servers_;
};

class ParameterizedIncomingServiceTest
    : public IncomingServiceTest,
      public ::testing::WithParamInterface<std::pair<std::string, bool>> {};

TEST_P(ParameterizedIncomingServiceTest, OpensServiceAtConnects) {
  auto [instance, is_foo] = GetParam();

  zx::result<EchoService::ServiceClient> open_result =
      component::OpenServiceAt<EchoService>(TakeSvcDirectoryRoot(), instance);
  ASSERT_TRUE(open_result.is_ok());

  EchoService::ServiceClient service = std::move(open_result.value());
  // Connect to the member 'foo' or 'bar'.
  zx::result<fidl::ClientEnd<Echo>> connect_result =
      is_foo ? service.connect_foo() : service.connect_bar();
  ASSERT_TRUE(connect_result.is_ok());

  fidl::WireClient client(std::move(connect_result.value()), dispatcher());
  const std::string prefix = instance + std::string("-") + std::string(is_foo ? "foo" : "bar");
  const std::string expected = prefix + std::string(": ") + kTestString;
  client->EchoString(fidl::StringView(kTestString))
      .Then([quit_loop = QuitLoopClosure(),
             expected](fidl::WireUnownedResult<Echo::EchoString>& echo_result) {
        ASSERT_OK(echo_result.status());
        if (!echo_result.ok()) {
          quit_loop();
        }
        auto response = echo_result.Unwrap();
        std::string actual(response->response.data(), response->response.size());
        EXPECT_EQ(actual, expected);
        quit_loop();
      });

  RunLoop();
}

INSTANTIATE_TEST_SUITE_P(
    /*no prefix*/, ParameterizedIncomingServiceTest,
    testing::Values(std::make_pair(kDefaultInstanceName, true),
                    std::make_pair(kDefaultInstanceName, false),
                    std::make_pair(kOtherInstanceName, true),
                    std::make_pair(kOtherInstanceName, false)));

TEST_F(IncomingServiceTest, FilePathTooLong) {
  std::string illegal_path;
  illegal_path.assign(256, 'a');
  // Use an instance name that is too long.
  zx::result<EchoService::ServiceClient> open_result =
      component::OpenServiceAt<EchoService>(TakeSvcDirectoryRoot(), illegal_path);
  ASSERT_TRUE(open_result.is_error());
  ASSERT_STATUS(open_result.status_value(), ZX_ERR_INVALID_ARGS);
}

}  // namespace
