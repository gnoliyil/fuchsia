// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "root-mock-device.h"

#include <fcntl.h>
#include <fuchsia/device/cpp/fidl.h>
#include <fuchsia/device/test/cpp/fidl.h>
#include <lib/devmgr-integration-test/fixture.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/unsafe.h>
#include <lib/fit/defer.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include <zircon/assert.h>

#define DRIVER_TEST_DIR "/boot/driver"
#define MOCK_DEVICE_LIB "/boot/driver/mock-device.so"

namespace libdriver_integration_test {

RootMockDevice::RootMockDevice(std::unique_ptr<MockDeviceHooks> hooks,
                               fidl::InterfacePtr<fuchsia::device::test::Device> test_device,
                               fidl::InterfaceRequest<fuchsia::device::mock::MockDevice> controller,
                               async_dispatcher_t* dispatcher, std::string path)
    : test_device_(std::move(test_device)),
      path_(std::move(path)),
      mock_(std::move(controller), dispatcher, "") {
  mock_.set_hooks(std::move(hooks));
}

RootMockDevice::~RootMockDevice() {
  // This will trigger unbind() to be called on any device that was added in
  // the bind hook.
  test_device_->Destroy();
}

// |*test_device_out| will be a channel to the test device that the mock device
// bound to.  This is provided so we can trigger unbinding of the mock device.
// |*control_out| will be a channel for fulfilling requests from the mock
// device.
zx_status_t RootMockDevice::Create(const IsolatedDevmgr& devmgr, async_dispatcher_t* dispatcher,
                                   std::unique_ptr<MockDeviceHooks> hooks,
                                   std::unique_ptr<RootMockDevice>* mock_out) {
  // Wait for /dev/sys/test/test to appear
  zx::result channel =
      device_watcher::RecursiveWaitForFile(devmgr.devfs_root().get(), "sys/test/test");
  if (channel.is_error()) {
    return channel.status_value();
  }

  zx::channel test_root_chan = std::move(channel.value());

  fidl::SynchronousInterfacePtr<fuchsia::device::test::RootDevice> test_root;
  test_root.Bind(std::move(test_root_chan));

  return CreateFromTestRoot(devmgr, dispatcher, std::move(test_root), std::move(hooks), mock_out);
}

zx_status_t RootMockDevice::CreateFromTestRoot(
    const IsolatedDevmgr& devmgr, async_dispatcher_t* dispatcher,
    fidl::SynchronousInterfacePtr<fuchsia::device::test::RootDevice> test_root,
    std::unique_ptr<MockDeviceHooks> hooks, std::unique_ptr<RootMockDevice>* mock_out) {
  fuchsia::device::test::RootDevice_CreateDevice_Result create_result;
  zx_status_t status = test_root->CreateDevice("mock", &create_result);
  if (status != ZX_OK) {
    return status;
  }
  if (create_result.is_err()) {
    return create_result.err();
  }

  // Connect to the created device.
  zx::result channel = device_watcher::RecursiveWaitForFile(devmgr.devfs_root().get(),
                                                            create_result.response().path.c_str());
  if (channel.is_error()) {
    return channel.error_value();
  }
  fidl::SynchronousInterfacePtr<fuchsia::device::test::Device> test_dev;
  test_dev.Bind(std::move(channel.value()));

  auto destroy_device = fit::defer([&test_dev] { test_dev->Destroy(); });

  fidl::InterfaceHandle<fuchsia::device::mock::MockDevice> client;
  fidl::InterfaceRequest<fuchsia::device::mock::MockDevice> server(client.NewRequest());
  if (!server.is_valid()) {
    return ZX_ERR_BAD_STATE;
  }

  status = test_dev->SetChannel(client.TakeChannel());
  if (status != ZX_OK) {
    return status;
  }

  // Ignore the |devpath| return and construct it ourselves, since the test
  // driver makes an assumption about where it's bound which isn't true in the
  // case where we're testing composite devices
  fidl::SynchronousInterfacePtr<fuchsia::device::Controller> test_root_controller;
  test_root_controller.Bind(test_root.Unbind().TakeChannel());
  fuchsia::device::Controller_GetTopologicalPath_Result result;
  status = test_root_controller->GetTopologicalPath(&result);
  if (status != ZX_OK) {
    return status;
  }
  if (result.is_err()) {
    return status;
  }
  fidl::StringPtr devpath = result.response().path;
  test_root.Bind(test_root_controller.Unbind().TakeChannel());

  const char* kDevPrefix = "/dev/";
  if (!devpath.has_value() || devpath.value().find(kDevPrefix) != 0) {
    return ZX_ERR_BAD_STATE;
  }
  std::string relative_devpath(devpath.value(), strlen(kDevPrefix));
  relative_devpath += "/mock";

  // Open a new connection to the test device to return.  We do to simplify
  // handling around the blocking nature of fuchsia.device.Controller/Bind.  Needs to
  // happen before the bind(), since bind() will cause us to get blocked in the mock device
  // driver waiting for input on what to do.
  //
  // TODO(https://fxbug.dev/112484): this relies on multiplexing.
  fidl::InterfacePtr<fuchsia::device::test::Device> test_device;
  {
    fidl::SynchronousInterfacePtr node =
        fidl::InterfaceHandle<fuchsia::io::Node>(test_dev.Unbind().TakeChannel()).BindSync();
    if (zx_status_t status = node->Clone(fuchsia::io::OpenFlags::CLONE_SAME_RIGHTS,
                                         fidl::InterfaceRequest<fuchsia::io::Node>(
                                             test_device.NewRequest(dispatcher).TakeChannel()));
        status != ZX_OK) {
      return status;
    }
    test_dev.Bind(node.Unbind().TakeChannel());
  }

  // Bind the mock device driver in a separate thread, since this call is
  // synchronous.
  thrd_t thrd;
  int ret = thrd_create(
      &thrd,
      [](void* ctx) {
        zx::channel test_dev(static_cast<zx_handle_t>(reinterpret_cast<uintptr_t>(ctx)));
        fidl::SynchronousInterfacePtr<fuchsia::device::Controller> controller;
        controller.Bind(std::move(test_dev));
        fuchsia::device::Controller_Bind_Result result;
        controller->Bind(MOCK_DEVICE_LIB, &result);
        return 0;
      },
      reinterpret_cast<void*>(static_cast<uintptr_t>(test_dev.Unbind().TakeChannel().release())));
  ZX_ASSERT(ret == thrd_success);
  thrd_detach(thrd);

  destroy_device.cancel();

  auto mock =
      std::make_unique<RootMockDevice>(std::move(hooks), std::move(test_device), std::move(server),
                                       dispatcher, std::move(relative_devpath));
  *mock_out = std::move(mock);
  return ZX_OK;
}

}  // namespace libdriver_integration_test
