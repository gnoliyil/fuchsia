// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/bin/guest_manager/guest_manager.h"

#include <fuchsia/net/interfaces/cpp/fidl.h>
#include <fuchsia/virtualization/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/sys/cpp/testing/component_context_provider.h>
#include <lib/syslog/cpp/macros.h>

#include <future>

#include <src/lib/testing/loop_fixture/real_loop_fixture.h>
#include <test/placeholders/cpp/fidl.h>

namespace {

using ::fuchsia::virtualization::GuestConfig;
using ::fuchsia::virtualization::GuestDescriptor;
using ::fuchsia::virtualization::GuestError;
using ::fuchsia::virtualization::GuestLifecycle;
using ::fuchsia::virtualization::GuestManagerError;

class FakeGuestLifecycle : public GuestLifecycle {
 public:
  FakeGuestLifecycle(sys::testing::ComponentContextProvider* provider,
                     async_dispatcher_t* dispatcher)
      : dispatcher_(dispatcher) {
    FX_CHECK(ZX_OK ==
             provider->service_directory_provider()->AddService(bindings_.GetHandler(this)));
  }

  // |fuchsia::virtualization::GuestManager|
  void Create(::fuchsia::virtualization::GuestConfig guest_config,
              CreateCallback callback) override {
    captured_config_ = std::move(guest_config);
    if (create_response_.is_ok()) {
      callback(fpromise::ok());
    } else {
      callback(fpromise::error(create_response_.error_value()));
    }
  }
  // |fuchsia::virtualization::GuestManager|
  void Bind(fidl::InterfaceRequest<::fuchsia::virtualization::Guest> guest) override {}
  // |fuchsia::virtualization::GuestManager|
  void Run(RunCallback callback) override { captured_run_callback_ = std::move(callback); }
  // |fuchsia::virtualization::GuestManager|
  void Stop(StopCallback callback) override {
    async::PostTask(dispatcher_, [this]() {
      captured_run_callback_(fpromise::error(GuestError::CONTROLLER_FORCED_HALT));
    });
    callback();
  }

  // The guest lifecycle provider never intentionally closes the server end of the channel. This
  // simulates what happens when the component terminates unexpectedly (such as a crash).
  void SimulateCrash() { bindings_.CloseAll(); }

  void set_create_response(fit::result<GuestError> err) { create_response_ = err; }
  RunCallback take_run_callback() { return std::move(captured_run_callback_); }
  GuestConfig take_guest_config() { return std::move(captured_config_); }

 private:
  fit::result<GuestError> create_response_ = fit::ok();
  RunCallback captured_run_callback_;
  GuestConfig captured_config_;
  fidl::BindingSet<GuestLifecycle> bindings_;
  async_dispatcher_t* dispatcher_ = nullptr;  // Unowned.
};

class FakeNetInterfaces : public ::fuchsia::net::interfaces::State,
                          fuchsia::net::interfaces::Watcher {
 public:
  explicit FakeNetInterfaces(sys::testing::ComponentContextProvider* provider) {
    FX_CHECK(ZX_OK ==
             provider->service_directory_provider()->AddService(state_bindings_.GetHandler(this)));
  }

  // |fuchsia::net::interfaces::State|
  void GetWatcher(::fuchsia::net::interfaces::WatcherOptions options,
                  ::fidl::InterfaceRequest<fuchsia::net::interfaces::Watcher> watcher) override {
    watcher_binding_.Bind(std::move(watcher));
  }

  // |fuchsia::net::interfaces::Watcher|
  void Watch(WatchCallback callback) override {
    ::fuchsia::net::interfaces::Event event;
    if (events_.empty()) {
      event.set_idle({});
    } else {
      event = std::move(events_.front());
      events_.pop();
    }

    callback(std::move(event));
  }

  void AddExistingInterface(::fuchsia::hardware::network::DeviceClass device_class,
                            bool online = true) {
    ::fuchsia::net::interfaces::DeviceClass device;
    device.set_device(device_class);

    ::fuchsia::net::interfaces::Properties properties;
    properties.set_device_class(std::move(device));
    properties.set_online(online);

    ::fuchsia::net::interfaces::Event event;
    event.set_existing(std::move(properties));

    events_.push(std::move(event));
  }

 private:
  std::queue<::fuchsia::net::interfaces::Event> events_;
  ::fidl::BindingSet<::fuchsia::net::interfaces::State> state_bindings_;
  ::fidl::Binding<fuchsia::net::interfaces::Watcher> watcher_binding_{this};
};

class GuestManagerTest : public gtest::RealLoopFixture {
 public:
  void SetUp() override {
    RealLoopFixture::SetUp();
    fake_net_interfaces_ = std::make_unique<FakeNetInterfaces>(&provider_);
    fake_guest_lifecycle_ = std::make_unique<FakeGuestLifecycle>(&provider_, dispatcher());
  }

  sys::testing::ComponentContextProvider provider_;
  std::unique_ptr<FakeNetInterfaces> fake_net_interfaces_;
  std::unique_ptr<FakeGuestLifecycle> fake_guest_lifecycle_;
};

TEST_F(GuestManagerTest, LaunchFailInvalidPath) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "invalid_path.cfg");
  bool launch_callback_called = false;
  manager.Launch({}, {}, [&launch_callback_called](auto res) {
    ASSERT_TRUE(res.is_err());
    ASSERT_EQ(GuestManagerError::BAD_CONFIG, res.err());
    launch_callback_called = true;
  });
  ASSERT_TRUE(launch_callback_called);
}

TEST_F(GuestManagerTest, LaunchFailInvalidConfig) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/",
                       "data/configs/bad_schema_invalid_field.cfg");
  bool launch_callback_called = false;
  manager.Launch({}, {}, [&launch_callback_called](auto res) {
    ASSERT_TRUE(res.is_err());
    ASSERT_EQ(GuestManagerError::BAD_CONFIG, res.err());
    launch_callback_called = true;
  });
  ASSERT_TRUE(launch_callback_called);
}

TEST_F(GuestManagerTest, ForceShutdownNonRunningGuest) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");

  bool get_callback_called = false;
  manager.GetInfo([&get_callback_called](auto info) {
    ASSERT_EQ(info.guest_status(), fuchsia::virtualization::GuestStatus::NOT_STARTED);
    get_callback_called = true;
  });
  ASSERT_TRUE(get_callback_called);

  bool shutdown_callback_called = false;
  manager.ForceShutdown([&shutdown_callback_called]() { shutdown_callback_called = true; });
  RunLoopUntilIdle();
  ASSERT_TRUE(shutdown_callback_called);

  // Shutting down a non-running guest does nothing, including changing state from NOT_STARTED
  // (for example to STOPPING or STOPPED).
  get_callback_called = false;
  manager.GetInfo([&get_callback_called](auto info) {
    ASSERT_EQ(info.guest_status(), fuchsia::virtualization::GuestStatus::NOT_STARTED);
    get_callback_called = true;
  });
  ASSERT_TRUE(get_callback_called);
}

TEST_F(GuestManagerTest, ForceShutdown) {
  bool launch_callback_called = false;
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");
  fuchsia::virtualization::GuestConfig user_guest_config;
  fuchsia::virtualization::GuestPtr guest;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  bool get_callback_called = false;
  manager.GetInfo([&get_callback_called](auto info) {
    ASSERT_EQ(info.guest_status(), fuchsia::virtualization::GuestStatus::RUNNING);
    get_callback_called = true;
  });
  ASSERT_TRUE(get_callback_called);

  bool shutdown_callback_called = false;
  manager.ForceShutdown([&shutdown_callback_called]() { shutdown_callback_called = true; });
  RunLoopUntilIdle();
  ASSERT_TRUE(shutdown_callback_called);

  get_callback_called = false;
  manager.GetInfo([&get_callback_called](auto info) {
    ASSERT_EQ(info.guest_status(), fuchsia::virtualization::GuestStatus::STOPPED);
    ASSERT_EQ(info.stop_error(), GuestError::CONTROLLER_FORCED_HALT);
    get_callback_called = true;
  });
  ASSERT_TRUE(get_callback_called);
}

TEST_F(GuestManagerTest, VmmComponentCrash) {
  bool launch_callback_called = false;
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");
  fuchsia::virtualization::GuestConfig user_guest_config;
  fuchsia::virtualization::GuestPtr guest;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  bool get_callback_called = false;
  manager.GetInfo([&get_callback_called](auto info) {
    ASSERT_EQ(info.guest_status(), fuchsia::virtualization::GuestStatus::RUNNING);
    get_callback_called = true;
  });
  ASSERT_TRUE(get_callback_called);

  // The VMM controller closing the lifecycle channel means that it went away unexpectedly.
  fake_guest_lifecycle_->SimulateCrash();
  RunLoopUntilIdle();

  get_callback_called = false;
  manager.GetInfo([&get_callback_called](auto info) {
    ASSERT_EQ(info.guest_status(),
              fuchsia::virtualization::GuestStatus::VMM_UNEXPECTED_TERMINATION);
    get_callback_called = true;
  });
  ASSERT_TRUE(get_callback_called);
}

TEST_F(GuestManagerTest, FailedToCreateAndInitializeVmmWithRestart) {
  // Inject a failure into Launch.
  fake_guest_lifecycle_->set_create_response(fit::error(GuestError::GUEST_INITIALIZATION_FAILURE));
  bool launch_callback_called = false;
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");
  {
    fuchsia::virtualization::GuestConfig user_guest_config;
    fuchsia::virtualization::GuestPtr guest;
    manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                   [&launch_callback_called](auto res) {
                     ASSERT_TRUE(res.is_err());
                     ASSERT_EQ(res.err(), GuestManagerError::START_FAILURE);
                     launch_callback_called = true;
                   });
  }
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  // Second Launch succeeds.
  fake_guest_lifecycle_->set_create_response(fit::ok());
  launch_callback_called = false;
  {
    fuchsia::virtualization::GuestConfig user_guest_config;
    fuchsia::virtualization::GuestPtr guest;
    manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                   [&launch_callback_called](auto res) {
                     ASSERT_FALSE(res.is_err());
                     launch_callback_called = true;
                   });
  }
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);
}

TEST_F(GuestManagerTest, GuestInitiatedCleanShutdown) {
  bool launch_callback_called = false;
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");
  fuchsia::virtualization::GuestConfig user_guest_config;
  fuchsia::virtualization::GuestPtr guest;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  bool get_callback_called = false;
  manager.GetInfo([&get_callback_called](auto info) {
    ASSERT_EQ(info.guest_status(), fuchsia::virtualization::GuestStatus::RUNNING);
    get_callback_called = true;
  });
  ASSERT_TRUE(get_callback_called);

  // VMM controller only calls the run callback when the guest has terminated.
  fake_guest_lifecycle_->take_run_callback()(fpromise::ok());
  RunLoopUntilIdle();

  get_callback_called = false;
  manager.GetInfo([&get_callback_called](auto info) {
    ASSERT_EQ(info.guest_status(), fuchsia::virtualization::GuestStatus::STOPPED);
    ASSERT_FALSE(info.has_stop_error());  // Clean shutdown.
    get_callback_called = true;
  });
  ASSERT_TRUE(get_callback_called);
}

TEST_F(GuestManagerTest, LaunchAndApplyUserGuestConfig) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");

  fidl::InterfaceHandle<fuchsia::io::File> file;
  fidl::InterfaceRequest request = file.NewRequest();

  fuchsia::virtualization::GuestConfig user_guest_config;
  user_guest_config.mutable_cmdline_add()->emplace_back("extra_cmd_line_arg=0");

  user_guest_config.mutable_block_devices()->push_back({
      .id = "lessthan20charid",
      .mode = fuchsia::virtualization::BlockMode::READ_ONLY,
      .format = fuchsia::virtualization::BlockFormat::WithFile(std::move(file)),
  });
  fuchsia::virtualization::GuestPtr guest;
  bool launch_callback_called = false;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  const GuestConfig config = fake_guest_lifecycle_->take_guest_config();
  const fuchsia::virtualization::BlockSpec& spec0 = config.block_devices()[0];
  ASSERT_EQ("data", spec0.id);
  ASSERT_TRUE(spec0.format.is_file()) << spec0.format.Which();

  const fuchsia::virtualization::BlockSpec& spec1 = config.block_devices()[1];
  ASSERT_EQ("lessthan20charid", spec1.id);
  ASSERT_TRUE(spec1.format.is_file()) << spec1.format.Which();

  ASSERT_EQ(2u, config.block_devices().size());

  ASSERT_EQ("test cmdline extra_cmd_line_arg=0", config.cmdline());

  ASSERT_EQ(fuchsia::virtualization::KernelType::ZIRCON, config.kernel_type());
  ASSERT_TRUE(config.kernel());
  ASSERT_TRUE(config.ramdisk());
  ASSERT_EQ(4u, config.cpus());
}

TEST_F(GuestManagerTest, DoubleLaunchFail) {
  bool launch_callback_called = false;
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");
  fuchsia::virtualization::GuestConfig user_guest_config;
  fuchsia::virtualization::GuestPtr guest;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  launch_callback_called = false;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_TRUE(res.is_err());
                   ASSERT_EQ(GuestManagerError::ALREADY_RUNNING, res.err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);
}

TEST_F(GuestManagerTest, LaunchAndGetInfo) {
  bool get_callback_called = false;
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");

  manager.GetInfo([&get_callback_called](auto info) {
    ASSERT_EQ(info.guest_status(), fuchsia::virtualization::GuestStatus::NOT_STARTED);
    ASSERT_FALSE(info.has_uptime());
    ASSERT_FALSE(info.has_guest_descriptor());
    ASSERT_FALSE(info.has_stop_error());

    get_callback_called = true;
  });
  ASSERT_TRUE(get_callback_called);

  bool launch_callback_called = false;
  fuchsia::virtualization::GuestConfig user_guest_config;
  user_guest_config.set_default_net(true);

  fuchsia::virtualization::GuestPtr guest;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  fuchsia::virtualization::GuestConfig finalized_config =
      fake_guest_lifecycle_->take_guest_config();

  std::optional<fuchsia::virtualization::GuestInfo> info;
  auto handle = std::async(std::launch::async, [&manager, &info] {
    manager.GetInfo([&info](auto guest_info) { info = std::move(guest_info); });
  });

  RunLoopUntil([&info]() { return info.has_value(); });

  ASSERT_EQ(info->guest_status(), fuchsia::virtualization::GuestStatus::RUNNING);
  ASSERT_GT(info->uptime(), 0);
  ASSERT_FALSE(info->has_stop_error());

  ASSERT_TRUE(info->has_detected_problems());
  ASSERT_EQ(info->detected_problems().size(), 1u);
  ASSERT_EQ(info->detected_problems()[0], GuestManager::GuestNetworkStateToStringExplanation(
                                              GuestNetworkState::NO_HOST_NETWORKING));

  const GuestDescriptor& guest_descriptor = info->guest_descriptor();
  ASSERT_EQ(guest_descriptor.guest_memory(), finalized_config.guest_memory());
  ASSERT_EQ(guest_descriptor.num_cpus(), finalized_config.cpus());

  ASSERT_EQ(guest_descriptor.wayland(), finalized_config.has_wayland_device());
  ASSERT_EQ(guest_descriptor.magma(), finalized_config.has_magma_device());

  ASSERT_EQ(guest_descriptor.has_networks() && !guest_descriptor.networks().empty(),
            finalized_config.has_default_net() && finalized_config.default_net());
  ASSERT_EQ(guest_descriptor.balloon(),
            finalized_config.has_virtio_balloon() && finalized_config.virtio_balloon());
  ASSERT_EQ(guest_descriptor.console(),
            finalized_config.has_virtio_console() && finalized_config.virtio_console());
  ASSERT_EQ(guest_descriptor.gpu(),
            finalized_config.has_virtio_gpu() && finalized_config.virtio_gpu());
  ASSERT_EQ(guest_descriptor.rng(),
            finalized_config.has_virtio_rng() && finalized_config.virtio_rng());
  ASSERT_EQ(guest_descriptor.vsock(),
            finalized_config.has_virtio_vsock() && finalized_config.virtio_vsock());
  ASSERT_EQ(guest_descriptor.sound(),
            finalized_config.has_virtio_sound() && finalized_config.virtio_sound());
}

TEST_F(GuestManagerTest, Connect) {
  bool connect_callback_called = false;
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");

  fuchsia::virtualization::GuestPtr guest;
  manager.Connect(guest.NewRequest(), [&connect_callback_called](auto res) {
    ASSERT_TRUE(res.is_err());
    ASSERT_EQ(GuestManagerError::NOT_RUNNING, res.err());
    connect_callback_called = true;
  });
  ASSERT_TRUE(connect_callback_called);
  guest.Unbind();

  bool launch_callback_called = false;
  fuchsia::virtualization::GuestConfig user_guest_config;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);
  guest.Unbind();

  connect_callback_called = false;
  manager.Connect(guest.NewRequest(), [&connect_callback_called](auto res) {
    ASSERT_FALSE(res.is_err());
    connect_callback_called = true;
  });
  ASSERT_TRUE(connect_callback_called);
}

TEST_F(GuestManagerTest, DuplicateListenersProvidedByUserGuestConfig) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");
  fuchsia::virtualization::GuestConfig user_guest_config;

  // Two listeners with the same port.
  const uint32_t host_port = 12345;
  user_guest_config.mutable_vsock_listeners()->push_back(
      {host_port, fidl::InterfaceHandle<fuchsia::virtualization::HostVsockAcceptor>()});
  user_guest_config.mutable_vsock_listeners()->push_back(
      {host_port, fidl::InterfaceHandle<fuchsia::virtualization::HostVsockAcceptor>()});

  fuchsia::virtualization::GuestPtr guest;
  bool launch_callback_called = false;
  fuchsia::virtualization::GuestManager_Launch_Result result;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&result, &launch_callback_called](auto res) {
                   result = std::move(res);
                   launch_callback_called = true;
                 });

  ASSERT_TRUE(launch_callback_called);
  ASSERT_EQ(result.err(), GuestManagerError::BAD_CONFIG);
}

TEST_F(GuestManagerTest, UserProvidedInitialListeners) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");
  fuchsia::virtualization::GuestConfig user_guest_config;

  fidl::InterfaceHandle<fuchsia::virtualization::HostVsockAcceptor> acceptor1, acceptor2;

  // Give the handles valid channels (although the endpoint will go unused).
  auto request1 = acceptor1.NewRequest();
  auto request2 = acceptor2.NewRequest();

  user_guest_config.mutable_vsock_listeners()->push_back({123, std::move(acceptor1)});
  user_guest_config.mutable_vsock_listeners()->push_back({456, std::move(acceptor2)});

  bool launch_callback_called = false;
  fuchsia::virtualization::GuestPtr guest;
  fuchsia::virtualization::GuestManager_Launch_Result result;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_TRUE(res.is_response());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  // Initial Listeners are passed to the VMM via the guest config.
  fuchsia::virtualization::GuestConfig finalized_config =
      fake_guest_lifecycle_->take_guest_config();
  ASSERT_TRUE(finalized_config.has_vsock_listeners());
  ASSERT_EQ(finalized_config.vsock_listeners().size(), 2ul);
}

TEST_F(GuestManagerTest, GuestProbablyHasNetworking) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");

  fuchsia::virtualization::GuestConfig user_guest_config;
  user_guest_config.mutable_net_devices()->push_back({
      .mac_address = {{0x02, 0x1a, 0x11, 0x00, 0x01, 0x00}},
      .enable_bridge = true,
  });

  fuchsia::virtualization::GuestPtr guest;
  bool launch_callback_called = false;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  // There's a virtual interface, a bridge, and host networking. Everything looks good!
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::ETHERNET);
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::VIRTUAL);
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::BRIDGE);

  std::optional<GuestNetworkState> result;
  auto handle = std::async(std::launch::async, [&manager, &result] {
    // Blocking, so run on a non-dispatch loop thread.
    result = manager.QueryGuestNetworkState();
  });

  RunLoopUntil([&result]() { return result.has_value(); });

  ASSERT_EQ(result.value(), GuestNetworkState::OK);
}

TEST_F(GuestManagerTest, NoNetworkDevices) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");

  fuchsia::virtualization::GuestConfig user_guest_config;
  fuchsia::virtualization::GuestPtr guest;
  bool launch_callback_called = false;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  const GuestConfig config = fake_guest_lifecycle_->take_guest_config();
  ASSERT_FALSE((config.has_net_devices() && !config.net_devices().empty()));

  std::optional<GuestNetworkState> result;
  auto handle = std::async(std::launch::async, [&manager, &result] {
    // Blocking, so run on a non-dispatch loop thread.
    result = manager.QueryGuestNetworkState();
  });

  RunLoopUntil([&result]() { return result.has_value(); });

  ASSERT_EQ(result.value(), GuestNetworkState::NO_NETWORK_DEVICE);
}

TEST_F(GuestManagerTest, BridgingRequiredHostOnWifi) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");

  fuchsia::virtualization::GuestConfig user_guest_config;
  user_guest_config.mutable_net_devices()->push_back({
      .mac_address = {{0x02, 0x1a, 0x11, 0x00, 0x01, 0x00}},
      .enable_bridge = true,
  });

  fuchsia::virtualization::GuestPtr guest;
  bool launch_callback_called = false;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  // There's no bridge, and the host is on WiFi with no ethernet connection. This will result
  // in no internet connection.
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::WLAN);
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::VIRTUAL);

  std::optional<GuestNetworkState> result;
  auto handle = std::async(std::launch::async, [&manager, &result] {
    // Blocking, so run on a non-dispatch loop thread.
    result = manager.QueryGuestNetworkState();
  });

  RunLoopUntil([&result]() { return result.has_value(); });

  ASSERT_EQ(result.value(), GuestNetworkState::ATTEMPTED_TO_BRIDGE_WITH_WLAN);
}

TEST_F(GuestManagerTest, BridgingRequiredHostOnWifiAndEthernet) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");

  fuchsia::virtualization::GuestConfig user_guest_config;
  user_guest_config.mutable_net_devices()->push_back({
      .mac_address = {{0x02, 0x1a, 0x11, 0x00, 0x01, 0x00}},
      .enable_bridge = true,
  });

  fuchsia::virtualization::GuestPtr guest;
  bool launch_callback_called = false;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  // There's no bridge, but the host has both a WiFi and ethernet connection. The host should
  // be able to bridge the virtual connection to the ethernet connection, so the lack of a bridge
  // may be transient.
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::WLAN);
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::ETHERNET);
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::VIRTUAL);

  std::optional<GuestNetworkState> result;
  auto handle = std::async(std::launch::async, [&manager, &result] {
    // Blocking, so run on a non-dispatch loop thread.
    result = manager.QueryGuestNetworkState();
  });

  RunLoopUntil([&result]() { return result.has_value(); });

  ASSERT_EQ(result.value(), GuestNetworkState::NO_BRIDGE_CREATED);
}

TEST_F(GuestManagerTest, BridgingRequiredHostOnEthernet) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");

  fuchsia::virtualization::GuestConfig user_guest_config;
  user_guest_config.mutable_net_devices()->push_back({
      .mac_address = {{0x02, 0x1a, 0x11, 0x00, 0x01, 0x00}},
      .enable_bridge = true,
  });

  fuchsia::virtualization::GuestPtr guest;
  bool launch_callback_called = false;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  // There's no bridge, but the host has a working ethernet connection. This may be transient.
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::ETHERNET);
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::VIRTUAL);

  std::optional<GuestNetworkState> result;
  auto handle = std::async(std::launch::async, [&manager, &result] {
    // Blocking, so run on a non-dispatch loop thread.
    result = manager.QueryGuestNetworkState();
  });

  RunLoopUntil([&result]() { return result.has_value(); });

  ASSERT_EQ(result.value(), GuestNetworkState::NO_BRIDGE_CREATED);
}

TEST_F(GuestManagerTest, NotEnoughVirtualInterfaces) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");

  fuchsia::virtualization::GuestConfig user_guest_config;
  user_guest_config.mutable_net_devices()->push_back({
      .mac_address = {{0x02, 0x1a, 0x11, 0x00, 0x01, 0x00}},
      .enable_bridge = true,
  });
  user_guest_config.mutable_net_devices()->push_back({
      .mac_address = {{0x01, 0x02, 0x03, 0x04, 0x05, 0x06}},
      .enable_bridge = true,
  });

  fuchsia::virtualization::GuestPtr guest;
  bool launch_callback_called = false;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  // Config has two guest interfaces, but there's only one present.
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::ETHERNET);
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::VIRTUAL);

  std::optional<GuestNetworkState> result;
  auto handle = std::async(std::launch::async, [&manager, &result] {
    // Blocking, so run on a non-dispatch loop thread.
    result = manager.QueryGuestNetworkState();
  });

  RunLoopUntil([&result]() { return result.has_value(); });

  ASSERT_EQ(result.value(), GuestNetworkState::MISSING_VIRTUAL_INTERFACES);
}

TEST_F(GuestManagerTest, NoHostNetworking) {
  GuestManager manager(dispatcher(), provider_.context(), "/pkg/", "data/configs/valid_guest.cfg");

  fuchsia::virtualization::GuestConfig user_guest_config;
  user_guest_config.mutable_net_devices()->push_back({
      .mac_address = {{0x02, 0x1a, 0x11, 0x00, 0x01, 0x00}},
      .enable_bridge = true,
  });

  fuchsia::virtualization::GuestPtr guest;
  bool launch_callback_called = false;
  manager.Launch(std::move(user_guest_config), guest.NewRequest(),
                 [&launch_callback_called](auto res) {
                   ASSERT_FALSE(res.is_err());
                   launch_callback_called = true;
                 });
  RunLoopUntilIdle();
  ASSERT_TRUE(launch_callback_called);

  // Only active interfaces are used.
  fake_net_interfaces_->AddExistingInterface(::fuchsia::hardware::network::DeviceClass::ETHERNET,
                                             /*online=*/false);

  std::optional<GuestNetworkState> result;
  auto handle = std::async(std::launch::async, [&manager, &result] {
    // Blocking, so run on a non-dispatch loop thread.
    result = manager.QueryGuestNetworkState();
  });

  RunLoopUntil([&result]() { return result.has_value(); });

  ASSERT_EQ(result.value(), GuestNetworkState::NO_HOST_NETWORKING);
}

}  // namespace
