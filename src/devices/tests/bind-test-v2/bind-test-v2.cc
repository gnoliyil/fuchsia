// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.device.test/cpp/wire.h>
#include <fidl/fuchsia.device/cpp/wire.h>
#include <fuchsia/driver/development/cpp/fidl.h>
#include <fuchsia/driver/test/cpp/fidl.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/driver.h>
#include <lib/device-watcher/cpp/device-watcher.h>
#include <lib/driver_test_realm/realm_builder/cpp/lib.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/sys/cpp/component_context.h>

#include "lib/fidl/cpp/synchronous_interface_ptr.h"
#include "src/lib/testing/loop_fixture/test_loop_fixture.h"

const char kDriverUrl[] = "fuchsia-boot:///#driver/bind-test-v2-driver.so";
const char kDriverLibname[] = "bind-test-v2-driver.cm";
const char kChildDeviceName[] = "child";

class BindCompilerV2Test : public gtest::TestLoopFixture {
 protected:
  void SetUp() override {
    auto realm_builder = component_testing::RealmBuilder::Create();
    driver_test_realm::Setup(realm_builder);
    realm_ = std::make_unique<component_testing::RealmRoot>(realm_builder.Build(dispatcher()));

    fidl::SynchronousInterfacePtr<fuchsia::driver::test::Realm> driver_test_realm;
    ASSERT_EQ(realm_->component().Connect(driver_test_realm.NewRequest()), ZX_OK);
    fuchsia::driver::test::Realm_Start_Result realm_result;
    ASSERT_EQ(driver_test_realm->Start(fuchsia::driver::test::RealmArgs(), &realm_result), ZX_OK);
    ASSERT_FALSE(realm_result.is_err());

    fidl::InterfaceHandle<fuchsia::io::Node> dev;
    zx_status_t status =
        realm_->component().Connect("dev-topological", dev.NewRequest().TakeChannel());
    ASSERT_EQ(status, ZX_OK);

    fbl::unique_fd root_fd;
    status = fdio_fd_create(dev.TakeChannel().release(), root_fd.reset_and_get_address());
    ASSERT_EQ(status, ZX_OK);

    // Wait for /dev/sys/test/test to appear, then create an endpoint to it.
    zx::result channel = device_watcher::RecursiveWaitForFile(
        root_fd.get(), fuchsia_device_test::wire::kControlDevice);
    ASSERT_EQ(channel.status_value(), ZX_OK);

    fidl::ClientEnd<fuchsia_device_test::RootDevice> client_end(std::move(channel.value()));
    fidl::WireSyncClient root_device{std::move(client_end)};

    // Create the root test device in /dev/sys/test/test, and get its relative path from /dev.
    auto result = root_device->CreateDevice(fidl::StringView::FromExternal(kChildDeviceName));

    ASSERT_EQ(result.status(), ZX_OK);
    ASSERT_TRUE(result->is_ok()) << "CreateDevice failed "
                                 << zx_status_get_string(result->error_value());

    device_path_.clear();
    device_path_.append(fuchsia_device_test::wire::kControlDevice);
    device_path_.append("/");
    device_path_.append(kChildDeviceName);

    // Connect to the child device and bind a test driver to it.
    {
      zx::result channel =
          device_watcher::RecursiveWaitForFile(root_fd.get(), device_path_.c_str());
      ASSERT_EQ(channel.status_value(), ZX_OK);

      fidl::ClientEnd<fuchsia_device::Controller> client_end(std::move(channel.value()));
      fidl::WireSyncClient child{std::move(client_end)};

      auto response = child->Bind(::fidl::StringView::FromExternal(kDriverLibname));
      status = response.status();
      if (status == ZX_OK) {
        if (response->is_error()) {
          status = response->error_value();
        }
      }
      ASSERT_EQ(status, ZX_OK);
    }

    // Connect to the DriverDevelopment service.
    status = realm_->component().Connect(driver_dev_.NewRequest());
    ASSERT_EQ(status, ZX_OK);
  }

  fuchsia::driver::development::DriverDevelopmentSyncPtr driver_dev_;
  std::unique_ptr<component_testing::RealmRoot> realm_;
  std::string device_path_;
};

// Check that calling GetDriverInfo with an invalid driver path returns ZX_ERR_NOT_FOUND.
TEST_F(BindCompilerV2Test, InvalidDriver) {
  fuchsia::driver::development::DriverInfoIteratorSyncPtr iterator;
  ASSERT_EQ(driver_dev_->GetDriverInfo({"abc"}, iterator.NewRequest()), ZX_OK);

  std::vector<fuchsia::driver::development::DriverInfo> drivers;
  ASSERT_NE(iterator->GetNext(&drivers), ZX_OK);
}

// Get the bind program of the test driver and check that it has the expected instructions.
TEST_F(BindCompilerV2Test, ValidDriver) {
  fuchsia::driver::development::DriverInfoIteratorSyncPtr iterator;
  ASSERT_EQ(driver_dev_->GetDriverInfo({kDriverUrl}, iterator.NewRequest()), ZX_OK);

  std::vector<fuchsia::driver::development::DriverInfo> drivers;
  ASSERT_EQ(iterator->GetNext(&drivers), ZX_OK);

  ASSERT_EQ(drivers.size(), 1u);
  auto bytecode = drivers[0].bind_rules().bytecode_v2();

  uint8_t expected_bytecode[] = {
      0x42, 0x49, 0x4e, 0x44, 0x2,  0x0,  0x0,  0x0,  // Bind header
      0x0,                                            // Debug flag
      0x53, 0x59, 0x4e, 0x42, 0x36, 0x0,  0x0,  0x0,  // Symbol header
      0x1,  0x0,  0x0,  0x0,                          // fuchsia.compat.LIBNAME symbol key
      0x66, 0x75, 0x63, 0x68, 0x73, 0x69, 0x61, 0x2e, 0x63, 0x6f, 0x6d, 0x70,  // fuchsia.comp
      0x61, 0x74, 0x2e, 0x4c, 0x49, 0x42, 0x4e, 0x41, 0x4d, 0x45, 0x0,         // at.LIBNAME
      0x2,  0x0,  0x0,  0x0,  // bind-test-v2-driver.so symbol key
      0x62, 0x69, 0x6e, 0x64, 0x2d, 0x74, 0x65, 0x73, 0x74, 0x2d, 0x76,       // bind-test-v
      0x32, 0x2d, 0x64, 0x72, 0x69, 0x76, 0x65, 0x72, 0x2e, 0x63, 0x6d, 0x0,  // 2-driver.cm

      0x49, 0x4e, 0x53, 0x54, 0x16, 0x0,  0x0,  0x0,                    // Instruction header
      0x1,  0x1,  0x1,  0x0,  0x0,  0x0,  0x1,  0x50, 0x0,  0x0,  0x0,  // Device protocol condition
      0x1,  0x0,  0x1,  0x0,  0x0,  0x0,  0x2,  0x2,  0x0,  0x0,  0x0,  // fuchsia.compat.LIBNAME ==
                                                                        // bind-test-v2-driver.cm
  };

  ASSERT_EQ(std::size(expected_bytecode), bytecode.size());
  for (size_t i = 0; i < bytecode.size(); i++) {
    ASSERT_EQ(expected_bytecode[i], bytecode[i]);
  }
}

// Check that calling GetDeviceInfo with a non-existent device path returns an empty vector.
TEST_F(BindCompilerV2Test, InvalidDevice) {
  fuchsia::driver::development::DeviceInfoIteratorSyncPtr iterator;
  ASSERT_EQ(driver_dev_->GetDeviceInfo({"abc"}, iterator.NewRequest(), /* exact_match= */ true),
            ZX_OK);
  std::vector<fuchsia::driver::development::DeviceInfo> devices;
  ASSERT_EQ(iterator->GetNext(&devices), ZX_OK);
  ASSERT_EQ(devices.size(), 0u);
}

// Get the properties of the test driver's child device and check that they are as expected.
TEST_F(BindCompilerV2Test, ValidDevice) {
  std::string child_device_path("/dev/" + device_path_ + "/" + kChildDeviceName);

  fuchsia::driver::development::DeviceInfoIteratorSyncPtr iterator;
  ASSERT_EQ(driver_dev_->GetDeviceInfo({child_device_path}, iterator.NewRequest(),
                                       /* exact_match= */ true),
            ZX_OK);

  std::vector<fuchsia::driver::development::DeviceInfo> devices;
  ASSERT_EQ(iterator->GetNext(&devices), ZX_OK);
  ASSERT_EQ(devices.size(), 1u);
  auto props = devices[0].property_list().props;

  zx_device_prop_t expected_props[] = {
      {BIND_PROTOCOL, 0, ZX_PROTOCOL_PCI},
      {BIND_PCI_VID, 0, 1234},
      {BIND_PCI_DID, 0, 1234},
  };

  ASSERT_EQ(props.size(), std::size(expected_props));
  for (size_t i = 0; i < props.size(); i++) {
    ASSERT_EQ(props[i].id, expected_props[i].id);
    ASSERT_EQ(props[i].reserved, expected_props[i].reserved);
    ASSERT_EQ(props[i].value, expected_props[i].value);
  }
}
