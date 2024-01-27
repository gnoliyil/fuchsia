// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_USB_DRIVERS_USB_PERIPHERAL_TEST_DRIVER_H_
#define SRC_DEVICES_USB_DRIVERS_USB_PERIPHERAL_TEST_DRIVER_H_

#include <fuchsia/hardware/usb/function/cpp/banjo.h>

#include <atomic>

#include <ddktl/device.h>
#include <ddktl/suspend-txn.h>
#include <fbl/mutex.h>
#include <usb/request-cpp.h>
#include <usb/usb-request.h>

static constexpr size_t BULK_TX_COUNT = 16;
static constexpr size_t BULK_RX_COUNT = 16;
static constexpr size_t INTR_COUNT = 8;

static constexpr size_t BULK_MAX_PACKET = 512;  // FIXME(voydanoff) USB 3.0 support.
// FIXME(voydanoff) Increase this when DCI drivers support
// non-contiguous DMA buffers.
static constexpr size_t BULK_REQ_SIZE = 512;
static constexpr size_t INTR_REQ_SIZE = 64;
static constexpr size_t INTR_MAX_PACKET = 64;

namespace usb_function_test {

class UsbTest;
using UsbTestType = ddk::Device<UsbTest, ddk::Unbindable, ddk::Suspendable>;
class UsbTest : public UsbTestType, public ddk::UsbFunctionInterfaceProtocol<UsbTest> {
 public:
  explicit UsbTest(zx_device_t* parent) : UsbTestType(parent) {}
  zx_status_t Init();

  size_t UsbFunctionInterfaceGetDescriptorsSize() { return sizeof(descriptors_); }

  void UsbFunctionInterfaceGetDescriptors(uint8_t* buffer, size_t buffer_size, size_t* out_actual);

  zx_status_t UsbFunctionInterfaceControl(const usb_setup_t* setup, const uint8_t* write_buffer,
                                          size_t write_size, uint8_t* read_buffer, size_t read_size,
                                          size_t* out_read_actual);

  zx_status_t UsbFunctionInterfaceSetConfigured(bool configured, usb_speed_t speed);

  zx_status_t UsbFunctionInterfaceSetInterface(uint8_t interface, uint8_t alt_setting);

  void DdkUnbind(ddk::UnbindTxn txn);

  void DdkSuspend(ddk::SuspendTxn txn);

  size_t UsbFunctionGetRequestSize() { return parent_req_size_ + sizeof(usb_req_internal_t); }

  void DdkRelease();
  static zx_status_t Create(void* ctx, zx_device_t* parent);

 private:
  void TestIntrInComplete(usb_request_t* req);
  void TestIntrOutComplete(usb_request_t* req);
  void TestBulkOutComplete(usb_request_t* req);
  void TestBulkInComplete(usb_request_t* req);
  ddk::UsbFunctionProtocolClient function_;

  // These are lists of usb_request_t.
  usb::RequestQueue<void> bulk_out_reqs_ __TA_GUARDED(lock_);
  usb::RequestQueue<void> bulk_in_reqs_ __TA_GUARDED(lock_);
  usb::RequestQueue<void> intr_in_reqs_ __TA_GUARDED(lock_);
  usb::RequestQueue<void> intr_out_reqs_ __TA_GUARDED(lock_);
  usb::RequestQueue<void> requests_;

  uint8_t test_data_[INTR_REQ_SIZE];
  size_t test_data_length_;

  bool configured_;

  fbl::Mutex lock_;

  std::atomic_bool suspending_ = false;
  uint8_t bulk_out_addr_;
  uint8_t bulk_in_addr_;
  uint8_t intr_out_addr_;
  uint8_t intr_in_addr_;
  size_t parent_req_size_;

  struct {
    usb_interface_descriptor_t intf;
    usb_endpoint_descriptor_t intr_out_ep;
    usb_endpoint_descriptor_t intr_in_ep;
    usb_endpoint_descriptor_t bulk_out_ep;
    usb_endpoint_descriptor_t bulk_in_ep;
  } descriptors_ = {
      .intf =
          {
              .b_length = sizeof(usb_interface_descriptor_t),
              .b_descriptor_type = USB_DT_INTERFACE,
              .b_interface_number = 0,  // set later
              .b_alternate_setting = 0,
              .b_num_endpoints = 4,
              .b_interface_class = USB_CLASS_VENDOR,
              .b_interface_sub_class = 0,
              .b_interface_protocol = 0,
              .i_interface = 0,
          },
      .intr_out_ep =
          {
              .b_length = sizeof(usb_endpoint_descriptor_t),
              .b_descriptor_type = USB_DT_ENDPOINT,
              .b_endpoint_address = 0,  // set later
              .bm_attributes = USB_ENDPOINT_INTERRUPT,
              .w_max_packet_size = htole16(INTR_MAX_PACKET),
              .b_interval = 8,
          },
      .intr_in_ep =
          {
              .b_length = sizeof(usb_endpoint_descriptor_t),
              .b_descriptor_type = USB_DT_ENDPOINT,
              .b_endpoint_address = 0,  // set later
              .bm_attributes = USB_ENDPOINT_INTERRUPT,
              .w_max_packet_size = htole16(INTR_MAX_PACKET),
              .b_interval = 8,
          },
      .bulk_out_ep =
          {
              .b_length = sizeof(usb_endpoint_descriptor_t),
              .b_descriptor_type = USB_DT_ENDPOINT,
              .b_endpoint_address = 0,  // set later
              .bm_attributes = USB_ENDPOINT_BULK,
              .w_max_packet_size = htole16(BULK_MAX_PACKET),
              .b_interval = 0,
          },
      .bulk_in_ep =
          {
              .b_length = sizeof(usb_endpoint_descriptor_t),
              .b_descriptor_type = USB_DT_ENDPOINT,
              .b_endpoint_address = 0,  // set later
              .bm_attributes = USB_ENDPOINT_BULK,
              .w_max_packet_size = htole16(BULK_MAX_PACKET),
              .b_interval = 0,
          },
  };
};

}  // namespace usb_function_test

#endif  // SRC_DEVICES_USB_DRIVERS_USB_PERIPHERAL_TEST_DRIVER_H_
