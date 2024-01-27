// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "adb-shell.h"

#include <fidl/fuchsia.dash/cpp/wire_test_base.h>
#include <fidl/fuchsia.hardware.adb/cpp/wire_test_base.h>
#include <fidl/fuchsia.sys2/cpp/wire_test_base.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async-loop/testing/cpp/real_loop.h>
#include <lib/component/outgoing/cpp/outgoing_directory.h>
#include <lib/syslog/cpp/macros.h>

#include <cstring>
#include <memory>
#include <optional>

#include <zxtest/zxtest.h>

#include "src/developer/adb/bin/adb-shell/adb_shell_config.h"

namespace adb_shell {

class FakeDashLauncher : public fidl::testing::WireTestBase<fuchsia_dash::Launcher> {
 public:
  void NotImplemented_(const std::string& name, ::fidl::CompleterBase& completer) override {
    FX_LOGS(ERROR) << "Not implemented " << name;
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void LaunchWithSocket(::fuchsia_dash::wire::LauncherLaunchWithSocketRequest* request,
                        LaunchWithSocketCompleter::Sync& completer) override {
    command_ = request->command.get();
    socket_ = std::move(request->socket);
    completer.ReplySuccess();
  }

  // index is the dash launcher instance to send OnTerminate for. Useful when launching multiple
  // shells.
  zx_status_t SendOnTerminateEvent(uint32_t index = 0) {
    if (index < binding_refs_.size()) {
      auto result = fidl::WireSendEvent(binding_refs_[index])->OnTerminated(ZX_OK);
      return result.status();
    }
    return ZX_ERR_INVALID_ARGS;
  }

  void BindServer(async_dispatcher_t* dispatcher,
                  fidl::ServerEnd<fuchsia_dash::Launcher> server_end) {
    binding_refs_.emplace_back(fidl::BindServer(dispatcher, std::move(server_end), this));
  }

  zx::unowned_socket socket() { return socket_.borrow(); }
  std::string command() { return command_; }

 private:
  // Dash launcher server bindings. Expect multiple when launching multiple shells.
  std::vector<fidl::ServerBindingRef<fuchsia_dash::Launcher>> binding_refs_;
  // Command string passed in the latest request if any.
  std::string command_;
  // Server end dash socket passed in the latest request.
  zx::socket socket_;
};

class FakeLifecycleController
    : public fidl::testing::WireTestBase<fuchsia_sys2::LifecycleController> {
 public:
  void NotImplemented_(const std::string& name, ::fidl::CompleterBase& completer) override {
    FX_LOGS(ERROR) << "Not implemented " << name;
    completer.Close(ZX_ERR_NOT_SUPPORTED);
  }

  void Resolve(::fuchsia_sys2::wire::LifecycleControllerResolveRequest* request,
               ResolveCompleter::Sync& completer) override {
    moniker_ = request->moniker.get();
    completer.ReplySuccess();
  }

  void BindServer(async_dispatcher_t* dispatcher,
                  fidl::ServerEnd<fuchsia_sys2::LifecycleController> server_end) {
    binding_refs_.emplace_back(fidl::BindServer(dispatcher, std::move(server_end), this));
  }

  std::string moniker() { return moniker_; }

 private:
  // Lifecycle controller server bindings.
  std::vector<fidl::ServerBindingRef<fuchsia_sys2::LifecycleController>> binding_refs_;
  // Moniker string passed in the latest request if any.
  std::string moniker_;
};

class AdbShellTest : public zxtest::Test, public loop_fixture::RealLoop {
 public:
  AdbShellTest() : shell_loop_(&kAsyncLoopConfigNeverAttachToThread) {}

  void SetUp() override {
    shell_loop_.StartThread("adb-shell-test-shell");
    incoming_ = std::make_unique<component::OutgoingDirectory>(dispatcher());
    auto svc_endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    ASSERT_TRUE(svc_endpoints.is_ok());
    SetupIncomingServices(std::move(svc_endpoints->server));
    adb_ = std::make_unique<adb_shell::AdbShell>(
        std::move(svc_endpoints->client), shell_loop_.dispatcher(), adb_shell_config::Config());
    ASSERT_NO_FAILURES();
  }

  void SetupIncomingServices(fidl::ServerEnd<fuchsia_io::Directory> svc) {
    auto endpoints = fidl::CreateEndpoints<fuchsia_io::Directory>();
    ASSERT_EQ(ZX_OK, endpoints.status_value());
    ASSERT_EQ(ZX_OK, incoming_
                         ->AddUnmanagedProtocol<fuchsia_dash::Launcher>(
                             [this](fidl::ServerEnd<fuchsia_dash::Launcher> server_end) {
                               fake_dash_launcher_.BindServer(dispatcher(), std::move(server_end));
                             })
                         .status_value());
    ASSERT_EQ(ZX_OK, incoming_
                         ->AddUnmanagedProtocol<fuchsia_sys2::LifecycleController>(
                             [this](fidl::ServerEnd<fuchsia_sys2::LifecycleController> server_end) {
                               fake_lifecycle_controller_.BindServer(dispatcher(),
                                                                     std::move(server_end));
                             },
                             "fuchsia.sys2.LifecycleController.root")
                         .status_value());
    ASSERT_EQ(ZX_OK, incoming_->Serve(std::move(endpoints->server)).status_value());
    ASSERT_OK(fidl::WireCall(endpoints->client)
                  ->Open(fuchsia_io::wire::OpenFlags::kRightWritable |
                             fuchsia_io::wire::OpenFlags::kRightReadable,
                         0, "svc", fidl::ServerEnd<fuchsia_io::Node>(svc.TakeChannel())));
  }

 protected:
  std::unique_ptr<AdbShell> adb_;
  FakeDashLauncher fake_dash_launcher_;
  FakeLifecycleController fake_lifecycle_controller_;
  std::unique_ptr<component::OutgoingDirectory> incoming_;
  async::Loop shell_loop_;
};

TEST_F(AdbShellTest, LaunchShell) {
  PerformBlockingWork([&] {
    zx::socket endpoint0, endpoint1;
    ASSERT_OK(zx::socket::create(0, &endpoint0, &endpoint1));
    ASSERT_OK(adb_->AddShell({}, std::move(endpoint0)));
    EXPECT_TRUE(fake_dash_launcher_.socket()->is_valid());
    EXPECT_EQ(fake_dash_launcher_.command(), "");
    EXPECT_EQ(fake_lifecycle_controller_.moniker(), "");
    EXPECT_EQ(1, adb_->ActiveShellInstances());
    EXPECT_OK(fake_dash_launcher_.SendOnTerminateEvent());
    // Wait for the OnTerminate event to take effect.
    while (adb_->ActiveShellInstances() > 0) {
      shell_loop_.RunUntilIdle();
    }
  });
}

TEST_F(AdbShellTest, LaunchShellWithCommands) {
  PerformBlockingWork([&] {
    zx::socket endpoint0, endpoint1;
    ASSERT_OK(zx::socket::create(0, &endpoint1, &endpoint0));
    ASSERT_OK(adb_->AddShell("ls", std::move(endpoint0)));
    EXPECT_TRUE(fake_dash_launcher_.socket()->is_valid());
    EXPECT_EQ(fake_dash_launcher_.command(), "ls");
    EXPECT_EQ(1, adb_->ActiveShellInstances());
    EXPECT_OK(fake_dash_launcher_.SendOnTerminateEvent());
    // Wait for the OnTerminate event to take effect.
    while (adb_->ActiveShellInstances() > 0) {
      shell_loop_.RunUntilIdle();
    }
  });
}

TEST_F(AdbShellTest, MultipleShells) {
  PerformBlockingWork([&] {
    zx::socket endpoint0, endpoint1;
    ASSERT_OK(zx::socket::create(0, &endpoint0, &endpoint1));
    ASSERT_OK(adb_->AddShell("echo 'hi'", std::move(endpoint0)));
    EXPECT_TRUE(fake_dash_launcher_.socket()->is_valid());
    EXPECT_EQ(fake_dash_launcher_.command(), "echo 'hi'");
    EXPECT_EQ(1, adb_->ActiveShellInstances());
    ASSERT_OK(adb_->AddShell("ls", std::move(endpoint1)));
    EXPECT_TRUE(fake_dash_launcher_.socket()->is_valid());
    EXPECT_EQ(fake_dash_launcher_.command(), "ls");
    EXPECT_EQ(2, adb_->ActiveShellInstances());
    EXPECT_OK(fake_dash_launcher_.SendOnTerminateEvent(0));
    EXPECT_OK(fake_dash_launcher_.SendOnTerminateEvent(1));
    // Wait for the OnTerminate event to take effect.
    while (adb_->ActiveShellInstances() > 0) {
      shell_loop_.RunUntilIdle();
    }
  });
}

}  // namespace adb_shell
