// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.device/cpp/wire.h>
#include <fidl/fuchsia.hardware.sample/cpp/wire.h>
#include <fuchsia/driver/test/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/component/incoming/cpp/protocol.h>
#include <lib/device-watcher/cpp/device-watcher.h>
#include <lib/driver_test_realm/realm_builder/cpp/lib.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fidl/cpp/synchronous_interface_ptr.h>
#include <lib/sys/component/cpp/testing/realm_builder.h>
#include <lib/sys/component/cpp/testing/realm_builder_types.h>

#include "src/lib/testing/loop_fixture/test_loop_fixture.h"

class DeviceControllerFidl : public gtest::TestLoopFixture {};

TEST_F(DeviceControllerFidl, ControllerTest) {
  // Create and build the realm.
  auto realm_builder = component_testing::RealmBuilder::Create();
  driver_test_realm::Setup(realm_builder);
  auto realm = realm_builder.Build(dispatcher());

  // Start DriverTestRealm.
  fidl::SynchronousInterfacePtr<fuchsia::driver::test::Realm> driver_test_realm;
  ASSERT_EQ(ZX_OK, realm.component().Connect(driver_test_realm.NewRequest()));
  fuchsia::driver::test::Realm_Start_Result realm_result;
  ASSERT_EQ(ZX_OK, driver_test_realm->Start(fuchsia::driver::test::RealmArgs(), &realm_result));
  ASSERT_FALSE(realm_result.is_err());

  // Connect to dev.
  fidl::InterfaceHandle<fuchsia::io::Node> dev;
  zx_status_t status = realm.component().exposed()->Open(fuchsia::io::OpenFlags::RIGHT_READABLE, 0,
                                                         "dev", dev.NewRequest());
  ASSERT_EQ(status, ZX_OK);

  fbl::unique_fd root_fd;
  status = fdio_fd_create(dev.TakeChannel().release(), root_fd.reset_and_get_address());
  ASSERT_EQ(status, ZX_OK);

  // Wait for driver.
  zx::result dev_channel =
      device_watcher::RecursiveWaitForFile(root_fd.get(), "sys/test/sample_driver");
  ASSERT_EQ(dev_channel.status_value(), ZX_OK);

  // Connect to the controller.
  auto endpoints = fidl::CreateEndpoints<fuchsia_device::Controller>();
  ASSERT_EQ(endpoints.status_value(), ZX_OK);

  fdio_cpp::UnownedFdioCaller caller(root_fd);
  zx::result channel = component::ConnectAt<fuchsia_device::Controller>(
      caller.directory(), "sys/test/sample_driver/device_controller");
  ASSERT_EQ(ZX_OK, channel.status_value());

  auto client = fidl::WireSyncClient(std::move(channel.value()));

  auto result = client->GetTopologicalPath();
  ASSERT_EQ(result->value()->path.get(), "/dev/sys/test/sample_driver");

  // Get the underlying device connection.
  {
    auto endpoints = fidl::CreateEndpoints<fuchsia_hardware_sample::Echo>();
    ASSERT_EQ(client->ConnectToDeviceFidl(endpoints->server.TakeChannel()).status(), ZX_OK);

    auto echo = fidl::WireSyncClient(std::move(endpoints->client));

    std::string_view sent_string = "hello";
    auto result = echo->EchoString(fidl::StringView::FromExternal(sent_string));
    ASSERT_EQ(ZX_OK, result.status());
    ASSERT_EQ(sent_string, result.value().response.get());
  }

  // Check the Echo API through the device protocol connector.
  {
    zx::result channel = component::ConnectAt<fuchsia_hardware_sample::Echo>(
        caller.directory(), "sys/test/sample_driver/device_protocol");
    ASSERT_EQ(ZX_OK, channel.status_value());

    auto echo = fidl::WireSyncClient(std::move(channel.value()));

    std::string_view sent_string = "hello";
    auto result = echo->EchoString(fidl::StringView::FromExternal(sent_string));
    ASSERT_EQ(ZX_OK, result.status());
    ASSERT_EQ(sent_string, result.value().response.get());
  }
}

TEST_F(DeviceControllerFidl, ControllerTestDfv2) {
  // Create and build the realm.
  auto realm_builder = component_testing::RealmBuilder::Create();
  driver_test_realm::Setup(realm_builder);
  auto realm = realm_builder.Build(dispatcher());

  // Start DriverTestRealm.
  fidl::SynchronousInterfacePtr<fuchsia::driver::test::Realm> driver_test_realm;
  ASSERT_EQ(ZX_OK, realm.component().Connect(driver_test_realm.NewRequest()));
  fuchsia::driver::test::Realm_Start_Result realm_result;
  auto args = fuchsia::driver::test::RealmArgs();
  args.set_use_driver_framework_v2(true);
  ASSERT_EQ(ZX_OK, driver_test_realm->Start(std::move(args), &realm_result));
  ASSERT_FALSE(realm_result.is_err());

  // Connect to dev.
  fidl::InterfaceHandle<fuchsia::io::Node> dev;
  zx_status_t status = realm.component().exposed()->Open(fuchsia::io::OpenFlags::RIGHT_READABLE, 0,
                                                         "dev", dev.NewRequest());
  ASSERT_EQ(status, ZX_OK);

  fbl::unique_fd root_fd;
  status = fdio_fd_create(dev.TakeChannel().release(), root_fd.reset_and_get_address());
  ASSERT_EQ(status, ZX_OK);

  // Wait for driver.
  zx::result dev_channel =
      device_watcher::RecursiveWaitForFile(root_fd.get(), "sys/test/sample_driver");
  ASSERT_EQ(dev_channel.status_value(), ZX_OK);

  auto endpoints = fidl::CreateEndpoints<fuchsia_device::Controller>();
  ASSERT_EQ(endpoints.status_value(), ZX_OK);

  fdio_cpp::UnownedFdioCaller caller(root_fd);
  zx::result channel = component::ConnectAt<fuchsia_device::Controller>(
      caller.directory(), "sys/test/sample_driver/device_controller");
  ASSERT_EQ(ZX_OK, channel.status_value());

  auto client = fidl::WireSyncClient(std::move(channel.value()));

  auto result = client->GetTopologicalPath();
  ASSERT_EQ(result->value()->path.get(), "/dev/sys/test/sample_driver");

  // Get the underlying device connection.
  {
    auto endpoints = fidl::CreateEndpoints<fuchsia_hardware_sample::Echo>();
    ASSERT_EQ(client->ConnectToDeviceFidl(endpoints->server.TakeChannel()).status(), ZX_OK);

    auto echo = fidl::WireSyncClient(std::move(endpoints->client));

    std::string_view sent_string = "hello";
    auto result = echo->EchoString(fidl::StringView::FromExternal(sent_string));
    ASSERT_EQ(ZX_OK, result.status());
    ASSERT_EQ(sent_string, result.value().response.get());
  }

  // Check the Echo API through the device protocol connector.
  {
    zx::result channel = component::ConnectAt<fuchsia_hardware_sample::Echo>(
        caller.directory(), "sys/test/sample_driver/device_protocol");
    ASSERT_EQ(ZX_OK, channel.status_value());

    auto echo = fidl::WireSyncClient(std::move(channel.value()));

    std::string_view sent_string = "hello";
    auto result = echo->EchoString(fidl::StringView::FromExternal(sent_string));
    ASSERT_EQ(ZX_OK, result.status());
    ASSERT_EQ(sent_string, result.value().response.get());
  }
}
