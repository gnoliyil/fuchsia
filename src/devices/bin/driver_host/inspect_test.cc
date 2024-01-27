// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "inspect.h"

#include <sdk/lib/inspect/testing/cpp/zxtest/inspect.h>
#include <zxtest/zxtest.h>

#include "driver_host.h"
#include "src/lib/storage/vfs/cpp/dir_test_util.h"

namespace {
using inspect::InspectTestHelper;
class DriverHostInspectTestCase : public InspectTestHelper, public zxtest::Test {
 public:
  DriverHostInspectTestCase() : loop_(&kAsyncLoopConfigNoAttachToCurrentThread) {
    loop_.StartThread("dh_inspect_test_thread");
  }

  DriverHostInspect& inspect() { return inspect_; }

 private:
  DriverHostInspect inspect_;
  async::Loop loop_;
};
}  // namespace

TEST_F(DriverHostInspectTestCase, DirectoryEntries) {
  // Check that root inspect is created
  uint8_t buffer[4096];
  size_t length;
  {
    fs::VdirCookie cookie;
    EXPECT_EQ(inspect().diagnostics_dir().Readdir(&cookie, buffer, sizeof(buffer), &length), ZX_OK);
    fs::DirentChecker dc(buffer, length);
    dc.ExpectEntry(".", V_TYPE_DIR);
    dc.ExpectEntry("root.inspect", V_TYPE_FILE);
    dc.ExpectEnd();
  }
}

class DriverInspectTestCase : public InspectTestHelper, public zxtest::Test {
 public:
  DriverInspectTestCase() : driver_host_(&kAsyncLoopConfigNoAttachToCurrentThread) {}

  DriverHostContext& driver_host() { return driver_host_; }

 private:
  DriverHostContext driver_host_;
};

TEST_F(DriverInspectTestCase, DriverProperties) {
  fbl::RefPtr<zx_driver> driver;
  ASSERT_OK(zx_driver::Create("test-driver", driver_host().inspect().drivers(), &driver));
  driver->set_name("test");
  driver->set_status(ZX_OK);
  ReadInspect(driver_host().inspect().inspector());

  // Check properties of test-driver
  auto* test_driver = hierarchy().GetByPath({"drivers", "test-driver"});
  ASSERT_TRUE(test_driver);

  // name: "test"
  ASSERT_NO_FATAL_FAILURE(
      CheckProperty(test_driver->node(), "name", inspect::StringPropertyValue("test")));

  // status: 0
  ASSERT_NO_FATAL_FAILURE(
      CheckProperty(test_driver->node(), "status", inspect::IntPropertyValue(ZX_OK)));
}

TEST_F(DriverInspectTestCase, AddRemoveDriver) {
  // Get the initial driver count
  ReadInspect(driver_host().inspect().inspector());
  const auto* driver_count =
      hierarchy().node().get_property<inspect::UintPropertyValue>("driver_count");
  ASSERT_TRUE(driver_count);
  auto initial_count = driver_count->value();

  // Add test-driver
  fbl::RefPtr<zx_driver> driver;
  ASSERT_OK(zx_driver::Create("test-driver", driver_host().inspect().drivers(), &driver));

  // Check count is incremented and driver is listed
  ReadInspect(driver_host().inspect().inspector());
  const auto* current_count =
      hierarchy().node().get_property<inspect::UintPropertyValue>("driver_count");
  ASSERT_TRUE(current_count);
  EXPECT_EQ(initial_count + 1, current_count->value());

  auto* test_driver = hierarchy().GetByPath({"drivers", "test-driver"});
  ASSERT_TRUE(test_driver);

  // Destroy driver
  driver.reset();

  // Check count is decremented and device is not listed
  ReadInspect(driver_host().inspect().inspector());
  current_count = hierarchy().node().get_property<inspect::UintPropertyValue>("driver_count");
  ASSERT_TRUE(current_count);
  EXPECT_EQ(initial_count, current_count->value());

  test_driver = hierarchy().GetByPath({"drivers", "test-driver"});
  ASSERT_FALSE(test_driver);
}

class DeviceInspectTestCase : public InspectTestHelper, public zxtest::Test {
 public:
  DeviceInspectTestCase() : driver_host_(&kAsyncLoopConfigNoAttachToCurrentThread) {
    zx_driver::Create("test-driver", driver_host_.inspect().drivers(), &drv_);
    auto driver = Driver::Create(drv_.get());
    ASSERT_OK(driver.status_value());
    driver_obj_ = *std::move(driver);
  }

  DriverHostContext& driver_host() { return driver_host_; }

  fbl::RefPtr<Driver> driver() { return driver_obj_; }

 private:
  DriverHostContext driver_host_;
  fbl::RefPtr<zx_driver> drv_;
  fbl::RefPtr<Driver> driver_obj_;
};

TEST_F(DeviceInspectTestCase, DeviceProperties) {
  fbl::RefPtr<zx_device> device;
  ASSERT_OK(zx_device::Create(&(driver_host()), "test-device", driver(), &device));
  device->set_local_id(1);
  device->set_flag(DEV_FLAG_UNBINDABLE | DEV_FLAG_INITIALIZING);

  ReadInspect(driver_host().inspect().inspector());

  // Check properties of test-device
  auto* test_device = hierarchy().GetByPath({"drivers", "test-driver", "devices", "test-device"});
  ASSERT_TRUE(test_device);
  ASSERT_NO_FATAL_FAILURE(
      CheckProperty(test_device->node(), "local_id", inspect::UintPropertyValue(1)));
  ASSERT_NO_FATAL_FAILURE(CheckProperty(test_device->node(), "flags",
                                        inspect::StringPropertyValue("initializing unbindable ")));

  device->set_local_id(0);
  device->vnode.reset();
}

TEST_F(DeviceInspectTestCase, AddRemoveDevice) {
  fbl::RefPtr<zx_device> device;

  ASSERT_OK(zx_device::Create(&(driver_host()), std::string("test-device"), driver(), &device));

  // Check the device count and check that device is listed
  ReadInspect(driver_host().inspect().inspector());
  auto* test_driver = hierarchy().GetByPath({"drivers", "test-driver"});
  ASSERT_TRUE(test_driver);
  const auto* device_count =
      test_driver->node().get_property<inspect::UintPropertyValue>("device_count");
  ASSERT_TRUE(device_count);
  auto initial_count = device_count->value();
  ASSERT_EQ(initial_count, 1);

  auto* test_device = hierarchy().GetByPath({"drivers", "test-driver", "devices", "test-device"});
  ASSERT_TRUE(test_device);

  // Destroy the device
  // Note: This only makes the device to be marked as dead; driver_host holds onto the list of dead
  // devices
  device->vnode.reset();
  device.reset();

  // Check count decremented and device is not listed
  ReadInspect(driver_host().inspect().inspector());
  test_driver = hierarchy().GetByPath({"drivers", "test-driver"});
  ASSERT_TRUE(test_driver);
  device_count = test_driver->node().get_property<inspect::UintPropertyValue>("device_count");
  ASSERT_TRUE(device_count);
  EXPECT_EQ(device_count->value(), 0);

  test_device = hierarchy().GetByPath({"drivers", "test-driver", "devices", "test-device"});
  ASSERT_FALSE(test_device);
}

TEST_F(DeviceInspectTestCase, CallStats) {
  fbl::RefPtr<zx_device> device;
  ASSERT_OK(zx_device::Create(&(driver_host()), "test-device", driver(), &device));
  device->set_ops(&internal::kDeviceDefaultOps);
  device->vnode.reset();

  // Make op calls
  fidl_incoming_msg_t dummy_msg = {};
  fidl_message_header_t dummy_hdr = {};
  dummy_msg.bytes = static_cast<void*>(&dummy_hdr);
  static_cast<devfs_fidl::DeviceInterface*>(device.get())->MessageOp(&dummy_msg, nullptr);

  {
    // Test InspectCallStats::Update() method
    driver_host().inspect().DeviceCreateStats().Update();
  }

  // Check call stats
  ReadInspect(driver_host().inspect().inspector());
  auto* call_stats =
      hierarchy().GetByPath({"drivers", "test-driver", "devices", "test-device", "call_stats"});
  ASSERT_TRUE(call_stats);

  auto* message_op_stat = call_stats->GetByPath({"message_op"});
  ASSERT_TRUE(message_op_stat);
  ASSERT_NO_FATAL_FAILURE(
      CheckProperty(message_op_stat->node(), "count", inspect::UintPropertyValue(1)));

  auto* device_create_stat = hierarchy().GetByPath({"call_stats", "device_create"});
  ASSERT_TRUE(device_create_stat);
  ASSERT_NO_FATAL_FAILURE(
      CheckProperty(device_create_stat->node(), "count", inspect::UintPropertyValue(1)));
}

TEST_F(DeviceInspectTestCase, ParentChild) {
  fbl::RefPtr<zx_device> parent;
  ASSERT_OK(zx_device::Create(&(driver_host()), "test-parent", driver(), &parent));
  parent->set_local_id(2);

  fbl::RefPtr<zx_device> child;
  ASSERT_OK(zx_device::Create(&(driver_host()), "test-child", driver(), &child));
  child->set_local_id(3);
  child->set_parent(parent);
  parent->add_child(child.get());

  // Check parent-child fields in inspect
  ReadInspect(driver_host().inspect().inspector());
  auto* parent_data = hierarchy().GetByPath({"drivers", "test-driver", "devices", "test-parent"});
  ASSERT_TRUE(parent_data);
  auto* child_data = hierarchy().GetByPath({"drivers", "test-driver", "devices", "test-child"});
  ASSERT_TRUE(child_data);

  ASSERT_NO_FATAL_FAILURE(
      CheckProperty(parent_data->node(), "child_count", inspect::UintPropertyValue(1)));

  ASSERT_NO_FATAL_FAILURE(CheckProperty(child_data->node(), "parent",
                                        inspect::StringPropertyValue("test-parent (local-id:2)")));

  child->set_local_id(0);
  child->vnode.reset();
  parent->set_local_id(0);
  parent->vnode.reset();
}
