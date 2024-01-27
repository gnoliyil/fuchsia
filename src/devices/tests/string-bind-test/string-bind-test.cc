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
#include <lib/fdio/directory.h>
#include <lib/sys/cpp/component_context.h>
#include <fbl/unique_fd.h>

#include <gtest/gtest.h>

#include "src/lib/testing/loop_fixture/test_loop_fixture.h"

const std::string kDriverBaseUrl = "fuchsia-boot:///#driver";
const std::string kStringBindDriverLibPath = kDriverBaseUrl + "/string-bind-child.so";
const std::string kChildDevicePath = "/dev/sys/test/parent";

class StringBindTest : public gtest::TestLoopFixture {
 protected:
  void SetUp() override {
    auto realm_builder = component_testing::RealmBuilder::Create();
    driver_test_realm::Setup(realm_builder);
    realm_ = std::make_unique<component_testing::RealmRoot>(realm_builder.Build(dispatcher()));

    fidl::SynchronousInterfacePtr<fuchsia::driver::test::Realm> driver_test_realm;
    ASSERT_EQ(realm_->component().Connect(driver_test_realm.NewRequest()), ZX_OK);
    fuchsia::driver::test::Realm_Start_Result realm_result;
    fuchsia::driver::test::RealmArgs args;
    args.set_use_driver_framework_v2(false);
    ASSERT_EQ(driver_test_realm->Start(std::move(args), &realm_result), ZX_OK);
    ASSERT_FALSE(realm_result.is_err());

    fidl::InterfaceHandle<fuchsia::io::Node> dev;
    zx_status_t status =
        realm_->component().Connect("dev-topological", dev.NewRequest().TakeChannel());
    ASSERT_EQ(status, ZX_OK);

    fbl::unique_fd root_fd;
    status = fdio_fd_create(dev.TakeChannel().release(), root_fd.reset_and_get_address());
    ASSERT_EQ(status, ZX_OK);

    // Wait for the child device to bind and appear. The child device should bind with its string
    // properties.
    zx::result channel =
        device_watcher::RecursiveWaitForFile(root_fd.get(), "sys/test/parent/child");
    ASSERT_EQ(ZX_OK, channel.status_value());

    // Connect to the DriverDevelopment service.
    status = realm_->component().Connect(driver_dev_.NewRequest());
    ASSERT_EQ(status, ZX_OK);
  }

  fuchsia::driver::development::DriverDevelopmentSyncPtr driver_dev_;
  std::unique_ptr<component_testing::RealmRoot> realm_;
};

// Get the bind program of the test driver and check that it has the expected instructions.
TEST_F(StringBindTest, DriverBytecode) {
  fuchsia::driver::development::DriverInfoIteratorSyncPtr iterator;
  ASSERT_EQ(ZX_OK, driver_dev_->GetDriverInfo({kStringBindDriverLibPath}, iterator.NewRequest()));

  std::vector<fuchsia::driver::development::DriverInfo> drivers;
  ASSERT_EQ(iterator->GetNext(&drivers), ZX_OK);
  ASSERT_EQ(drivers.size(), 1u);
  auto bytecode = drivers[0].bind_rules().bytecode_v2();

  const uint8_t kExpectedBytecode[] = {
      0x42, 0x49, 0x4E, 0x44, 0x02, 0x0,  0x0,  0x0,               // Bind header
      0x0,                                                         // Debug flag
      0x53, 0x59, 0x4E, 0x42, 0x7A, 0x00, 0x00, 0x00,              // Symbol table header
      0x01, 0x0,  0x0,  0x0,                                       // "stringbind.lib.kinglet" ID
      0x73, 0x74, 0x72, 0x69, 0x6e, 0x67, 0x62, 0x69, 0x6e, 0x64,  // "stringbind"
      0x2e, 0x6c, 0x69, 0x62, 0x2e, 0x6b, 0x69, 0x6e, 0x67, 0x6c,  // ".lib.kingl"
      0x65, 0x74, 0x00,                                            // "et"
      0x02, 0x00, 0x00, 0x00,                                      // "firecrest" ID
      0x66, 0x69, 0x72, 0x65, 0x63, 0x72, 0x65, 0x73, 0x74, 0x00,  // "firecrest"
      0x03, 0x00, 0x00, 0x00,                                      // "stringbind.lib.bobolink" ID
      0x73, 0x74, 0x72, 0x69, 0x6e, 0x67, 0x62, 0x69, 0x6e, 0x64,  // "stringbind"
      0x2e, 0x6c, 0x69, 0x62, 0x2e, 0x62, 0x6f, 0x62, 0x6f, 0x6c,  // ".lib.bobol"
      0x69, 0x6e, 0x6b, 0x00,                                      // "ink"
      0x04, 0x00, 0x00, 0x00,                                      // "stringbind.lib.Moon" ID
      0x73, 0x74, 0x72, 0x69, 0x6e, 0x67, 0x62, 0x69, 0x6e, 0x64,  // "stringbind"
      0x2e, 0x6c, 0x69, 0x62, 0x2e, 0x4d, 0x6f, 0x6f, 0x6e, 0x00,  // ".lib.Moon"
      0x05, 0x00, 0x00, 0x00,                                      // "stringbind.lib.Moon.Half" ID
      0x73, 0x74, 0x72, 0x69, 0x6e, 0x67, 0x62, 0x69, 0x6e, 0x64,  // "stringbind"
      0x2e, 0x6c, 0x69, 0x62, 0x2e, 0x4d, 0x6f, 0x6f, 0x6e,        // ".lib.Moon"
      0x2e, 0x48, 0x61, 0x6c, 0x66, 0x00,                          // ".Half"
      0x49, 0x4E, 0x53, 0x54, 0x2C, 0x00, 0x00, 0x00,              // Instruction header
      0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00,
      0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x0a,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x05, 0x00, 0x00, 0x00,
  };

  ASSERT_EQ(std::size(kExpectedBytecode), bytecode.size());
  for (size_t i = 0; i < bytecode.size(); i++) {
    ASSERT_EQ(kExpectedBytecode[i], bytecode[i]);
  }
}

TEST_F(StringBindTest, DeviceProperties) {
  fuchsia::driver::development::DeviceInfoIteratorSyncPtr iterator;
  ASSERT_EQ(ZX_OK, driver_dev_->GetDeviceInfo({kChildDevicePath}, iterator.NewRequest(),
                                              /* exact_match= */ true));

  std::vector<fuchsia::driver::development::DeviceInfo> devices;
  ASSERT_EQ(iterator->GetNext(&devices), ZX_OK);

  constexpr zx_device_prop_t kExpectedProps[] = {
      {BIND_PROTOCOL, 0, 3},
      {BIND_PCI_VID, 0, 1234},
      {BIND_PCI_DID, 0, 1234},
  };

  ASSERT_EQ(devices.size(), 1u);
  auto props = devices[0].property_list().props;
  ASSERT_EQ(props.size(), std::size(kExpectedProps));
  for (size_t i = 0; i < props.size(); i++) {
    ASSERT_EQ(props[i].id, kExpectedProps[i].id);
    ASSERT_EQ(props[i].reserved, kExpectedProps[i].reserved);
    ASSERT_EQ(props[i].value, kExpectedProps[i].value);
  }

  auto& str_props = devices[0].property_list().str_props;
  ASSERT_EQ(static_cast<size_t>(3), str_props.size());

  ASSERT_STREQ("stringbind.lib.kinglet", str_props[0].key.data());
  ASSERT_TRUE(str_props[0].value.is_str_value());
  ASSERT_STREQ("firecrest", str_props[0].value.str_value().data());

  ASSERT_STREQ("stringbind.lib.Moon", str_props[1].key.data());
  ASSERT_TRUE(str_props[1].value.is_enum_value());
  ASSERT_STREQ("stringbind.lib.Moon.Half", str_props[1].value.enum_value().data());

  ASSERT_STREQ("stringbind.lib.bobolink", str_props[2].key.data());
  ASSERT_TRUE(str_props[2].value.is_int_value());
  ASSERT_EQ(static_cast<uint32_t>(10), str_props[2].value.int_value());
}
