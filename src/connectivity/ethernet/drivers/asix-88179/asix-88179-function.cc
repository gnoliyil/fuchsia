// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.ax88179/cpp/wire.h>
#include <fuchsia/hardware/ethernet/cpp/banjo.h>
#include <fuchsia/hardware/usb/function/cpp/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/zircon-internal/thread_annotations.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <ddktl/protocol/empty-protocol.h>
#include <fbl/auto_lock.h>
#include <fbl/condition_variable.h>
#include <fbl/mutex.h>
#include <usb/cdc.h>
#include <usb/hid.h>
#include <usb/peripheral.h>
#include <usb/request-cpp.h>
#include <usb/usb-request.h>
#include <usb/usb.h>

#include "asix-88179-regs.h"

namespace fake_usb_ax88179_function {
namespace {

constexpr int BULK_MAX_PACKET = 512;
constexpr size_t INTR_MAX_PACKET = 64;

// Acts as a fake USB device for asix-88179 tests. Currently only partially
// implemented for initialization order regression test.

class FakeUsbAx88179Function;

using DeviceType = ddk::Device<FakeUsbAx88179Function, ddk::Unbindable,
                               ddk::Messageable<fuchsia_hardware_ax88179::Hooks>::Mixin>;

class FakeUsbAx88179Function : public DeviceType,
                               public ddk::UsbFunctionInterfaceProtocol<FakeUsbAx88179Function>,
                               public ddk::EmptyProtocol<ZX_PROTOCOL_TEST_ASIX_FUNCTION> {
 public:
  explicit FakeUsbAx88179Function(zx_device_t* parent) : DeviceType(parent), function_(parent) {}

  zx_status_t Bind();

  // |ddk::Device| mix-in implementations.
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();

  // UsbFunctionInterface:
  size_t UsbFunctionInterfaceGetDescriptorsSize();
  void UsbFunctionInterfaceGetDescriptors(uint8_t* out_descriptors_buffer, size_t descriptors_size,
                                          size_t* out_descriptors_actual);
  zx_status_t UsbFunctionInterfaceControl(const usb_setup_t* setup, const uint8_t* write_buffer,
                                          size_t write_size, uint8_t* out_read_buffer,
                                          size_t read_size, size_t* out_read_actual);
  zx_status_t UsbFunctionInterfaceSetConfigured(bool configured, usb_speed_t speed);
  zx_status_t UsbFunctionInterfaceSetInterface(uint8_t interface, uint8_t alt_setting);

  // Hooks:
  void SetOnline(SetOnlineRequestView request, SetOnlineCompleter::Sync& completer) override;

 private:
  void RequestQueue(usb_request_t* req, const usb_request_complete_callback_t* completion);

  ddk::UsbFunctionProtocolClient function_;

  struct {
    usb_interface_descriptor_t interface;
    usb_endpoint_descriptor_t bulk_in;
    usb_endpoint_descriptor_t bulk_out;
    usb_endpoint_descriptor_t intr_ep;
  } __PACKED descriptor_;

  size_t descriptor_size_ = 0;
  size_t parent_req_size_ = 0;
  uint8_t intr_addr_ = 0;

  std::optional<usb::Request<>> intr_req_ TA_GUARDED(mtx_);

  fbl::Mutex mtx_;

  bool configured_ = false;
};

void FakeUsbAx88179Function::SetOnline(SetOnlineRequestView request,
                                       SetOnlineCompleter::Sync& completer) {
  fbl::AutoLock lock(&mtx_);

  constexpr size_t kInterruptRequestSize = 8;
  uint8_t status[kInterruptRequestSize];
  memset(&status, 0, sizeof(status));
  status[2] = request->online;

  usb_request_complete_callback_t complete = {
      .callback = [](void* ctx, usb_request_t* req) {},
      .ctx = nullptr,
  };

  intr_req_->request()->header.length = sizeof(status);
  intr_req_->request()->header.ep_address = intr_addr_;
  size_t copy_result = intr_req_->CopyTo(status, sizeof(status), 0);
  ZX_ASSERT(copy_result == sizeof(status));
  RequestQueue(intr_req_->request(), &complete);

  completer.Reply(ZX_OK);
}

zx_status_t FakeUsbAx88179Function::Bind() {
  fbl::AutoLock lock(&mtx_);

  descriptor_size_ = sizeof(descriptor_);
  descriptor_.interface = {
      .b_length = sizeof(usb_interface_descriptor_t),
      .b_descriptor_type = USB_DT_INTERFACE,
      .b_interface_number = 0,
      .b_alternate_setting = 0,
      .b_num_endpoints = 3,
      .b_interface_class = USB_CLASS_COMM,
      .b_interface_sub_class = USB_CDC_SUBCLASS_ETHERNET,
      .b_interface_protocol = 1,
      .i_interface = 0,
  };
  descriptor_.bulk_in = {
      .b_length = sizeof(usb_endpoint_descriptor_t),
      .b_descriptor_type = USB_DT_ENDPOINT,
      .b_endpoint_address = USB_ENDPOINT_IN,  // set later
      .bm_attributes = USB_ENDPOINT_BULK,
      .w_max_packet_size = htole16(BULK_MAX_PACKET),
      .b_interval = 0,
  };
  descriptor_.bulk_out = {
      .b_length = sizeof(usb_endpoint_descriptor_t),
      .b_descriptor_type = USB_DT_ENDPOINT,
      .b_endpoint_address = USB_ENDPOINT_OUT,  // set later
      .bm_attributes = USB_ENDPOINT_BULK,
      .w_max_packet_size = htole16(BULK_MAX_PACKET),
      .b_interval = 0,
  };
  descriptor_.intr_ep = {
      .b_length = sizeof(usb_endpoint_descriptor_t),
      .b_descriptor_type = USB_DT_ENDPOINT,
      .b_endpoint_address = 0,  // set later
      .bm_attributes = USB_ENDPOINT_INTERRUPT,
      .w_max_packet_size = htole16(INTR_MAX_PACKET),
      .b_interval = 8,
  };

  parent_req_size_ = function_.GetRequestSize();

  zx_status_t status = function_.AllocInterface(&descriptor_.interface.b_interface_number);
  if (status != ZX_OK) {
    zxlogf(ERROR, "FakeUsbAx88179Function: usb_function_alloc_interface failed");
    return status;
  }
  status = function_.AllocEp(USB_DIR_IN, &descriptor_.bulk_in.b_endpoint_address);
  if (status != ZX_OK) {
    zxlogf(ERROR, "FakeUsbAx88179Function: usb_function_alloc_ep failed");
    return status;
  }
  status = function_.AllocEp(USB_DIR_OUT, &descriptor_.bulk_out.b_endpoint_address);
  if (status != ZX_OK) {
    zxlogf(ERROR, "FakeUsbAx88179Function: usb_function_alloc_ep failed");
    return status;
  }
  status = function_.AllocEp(USB_DIR_IN, &descriptor_.intr_ep.b_endpoint_address);
  if (status != ZX_OK) {
    zxlogf(ERROR, "FakeUsbAx88179Function: usb_function_alloc_ep failed");
    return status;
  }

  intr_addr_ = descriptor_.intr_ep.b_endpoint_address;

  status = usb::Request<>::Alloc(&intr_req_, INTR_MAX_PACKET, intr_addr_, parent_req_size_);
  if (status != ZX_OK) {
    return status;
  }

  status = DdkAdd("usb-ax88179-function");
  if (status != ZX_OK) {
    return status;
  }
  function_.SetInterface(this, &usb_function_interface_protocol_ops_);

  return ZX_OK;
}

void FakeUsbAx88179Function::RequestQueue(usb_request_t* req,
                                          const usb_request_complete_callback_t* completion) {
  function_.RequestQueue(req, completion);
}

size_t FakeUsbAx88179Function::UsbFunctionInterfaceGetDescriptorsSize() { return descriptor_size_; }
void FakeUsbAx88179Function::UsbFunctionInterfaceGetDescriptors(uint8_t* out_descriptors_buffer,
                                                                size_t descriptors_size,
                                                                size_t* out_descriptors_actual) {
  memcpy(out_descriptors_buffer, &descriptor_, std::min(descriptors_size, descriptor_size_));
  *out_descriptors_actual = descriptor_size_;
}

zx_status_t FakeUsbAx88179Function::UsbFunctionInterfaceControl(
    const usb_setup_t* setup, const uint8_t* write_buffer, size_t write_size,
    uint8_t* out_read_buffer, size_t read_size, size_t* out_read_actual) {
  if (out_read_actual) {
    *out_read_actual = 0;
  }
  return ZX_OK;
}

zx_status_t FakeUsbAx88179Function::UsbFunctionInterfaceSetConfigured(bool configured,
                                                                      usb_speed_t speed) {
  fbl::AutoLock lock(&mtx_);
  zx_status_t status;

  if (configured) {
    if (configured_) {
      return ZX_OK;
    }
    configured_ = true;

    if ((status = function_.ConfigEp(&descriptor_.intr_ep, nullptr)) != ZX_OK) {
      zxlogf(ERROR, "usb-ax88179-function: usb_function_config_ep failed");
    }
  } else {
    configured_ = false;
  }
  return ZX_OK;
}

zx_status_t FakeUsbAx88179Function::UsbFunctionInterfaceSetInterface(uint8_t interface,
                                                                     uint8_t alt_setting) {
  return ZX_OK;
}

void FakeUsbAx88179Function::DdkUnbind(ddk::UnbindTxn txn) {
  fbl::AutoLock lock(&mtx_);
  txn.Reply();
}

void FakeUsbAx88179Function::DdkRelease() { delete this; }

zx_status_t bind(void* ctx, zx_device_t* parent) {
  zxlogf(INFO, "FakeUsbAx88179Function: binding driver");
  auto dev = std::make_unique<FakeUsbAx88179Function>(parent);
  zx_status_t status = dev->Bind();
  if (status == ZX_OK) {
    dev.release();
  }
  return ZX_OK;
}

constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = bind;
  return ops;
}();

}  // namespace
}  // namespace fake_usb_ax88179_function

ZIRCON_DRIVER(fake_usb_ax88179, fake_usb_ax88179_function::driver_ops, "zircon", "0.1");
