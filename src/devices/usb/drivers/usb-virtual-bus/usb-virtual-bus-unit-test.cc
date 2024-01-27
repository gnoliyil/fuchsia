// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/usb/dci/cpp/banjo.h>

#include <thread>

#include <zxtest/zxtest.h>

#include "src/devices/testing/mock-ddk/mock-device.h"
#include "src/devices/usb/drivers/usb-virtual-bus/usb-virtual-bus.h"

namespace usb_virtual_bus {

TEST(VirtualBusUnitTest, DdkLifecycle) {
  auto fake_parent = MockDevice::FakeRootParent();

  auto bus = new UsbVirtualBus(fake_parent.get());
  ASSERT_NOT_NULL(bus);

  ASSERT_OK(bus->DdkAdd("usb-virtual-bus"));
  ASSERT_EQ(1, fake_parent->child_count());
  auto* child = fake_parent->GetLatestChild();

  child->InitOp();
  ASSERT_OK(child->WaitUntilInitReplyCalled());
  EXPECT_TRUE(child->InitReplyCalled());

  device_async_remove(bus->zxdev());
  mock_ddk::ReleaseFlaggedDevices(fake_parent.get());
}

class FakeDci : public ddk::UsbDciInterfaceProtocol<FakeDci> {
 public:
  explicit FakeDci()
      : UsbDciInterfaceProtocol(), protocol_{&usb_dci_interface_protocol_ops_, this} {}

  usb_dci_interface_protocol_t* get_proto() { return &protocol_; }

  // UsbDciInterface implementation.
  // This will block until the test calls |CompleteControlRequest|.
  zx_status_t UsbDciInterfaceControl(const usb_setup_t* setup, const uint8_t* write_buffer,
                                     size_t write_size, uint8_t* out_read_buffer, size_t read_size,
                                     size_t* out_read_actual) {
    sync_completion_signal(&control_start_sync_);
    sync_completion_wait(&control_complete_sync_, ZX_TIME_INFINITE);
    return ZX_OK;
  }

  // Blocks until FakeDci has received the control request.
  void WaitForControlRequestStart() {
    sync_completion_wait(&control_start_sync_, ZX_TIME_INFINITE);
  }

  // Signals FakeDci to complete the control request.
  void CompleteControlRequest() { sync_completion_signal(&control_complete_sync_); }

  void UsbDciInterfaceSetConnected(bool connected) {}
  void UsbDciInterfaceSetSpeed(usb_speed_t speed) {}

 private:
  usb_dci_interface_protocol_t protocol_;
  sync_completion_t control_start_sync_;
  sync_completion_t control_complete_sync_;
};

// Tests unbinding the usb virtual bus while a control request is in progress.
TEST(VirtualBusUnitTest, UnbindDuringControlRequest) {
  auto fake_parent = MockDevice::FakeRootParent();

  auto bus = new UsbVirtualBus(fake_parent.get());
  ASSERT_NOT_NULL(bus);

  ASSERT_OK(bus->DdkAdd("usb-virtual-bus"));
  ASSERT_EQ(1, fake_parent->child_count());
  auto* child = fake_parent->GetLatestChild();

  child->InitOp();
  ASSERT_OK(child->WaitUntilInitReplyCalled());
  EXPECT_TRUE(child->InitReplyCalled());

  // This needs to be true, otherwise requests will fail to be queued.
  bus->SetConnected(true);

  FakeDci fake_dci;
  ASSERT_OK(bus->UsbDciSetInterface(fake_dci.get_proto()));

  // This will be signalled by the control request completion callback.
  sync_completion_t usb_req_sync;
  // Start the control request before unbinding the device.
  // Do this in a new thread as it is a blocking operation, and we will not
  // request it be completed until after we begin unbinding.
  std::thread req_thread([&] {
    usb_request_complete_callback_t callback = {
        .callback =
            [](void* ctx, usb_request_t* req) {
              sync_completion_t* sync = static_cast<sync_completion_t*>(ctx);
              sync_completion_signal(sync);
              usb_request_release(req);
            },
        .ctx = &usb_req_sync,
    };
    size_t parent_req_size = bus->UsbHciGetRequestSize();
    usb_request_t* fake_req;
    ASSERT_OK(usb_request_alloc(&fake_req, zx_system_get_page_size(), 0 /* ep_address */,
                                parent_req_size));

    bus->UsbHciRequestQueue(fake_req, &callback);
  });

  fake_dci.WaitForControlRequestStart();

  // Request the device begin unbinding.
  // This should wake up the worker thread, which will block until the control request completes.
  child->UnbindOp();

  fake_dci.CompleteControlRequest();

  // Wait for the control request to complete.
  sync_completion_wait(&usb_req_sync, ZX_TIME_INFINITE);
  req_thread.join();

  // Check that unbind has replied.
  EXPECT_EQ(ZX_OK, child->WaitUntilUnbindReplyCalled());
  EXPECT_TRUE(child->UnbindReplyCalled());
}

}  // namespace usb_virtual_bus
