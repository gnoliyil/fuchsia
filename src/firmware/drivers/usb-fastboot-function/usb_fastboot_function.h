// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_FIRMWARE_DRIVERS_USB_FASTBOOT_FUNCTION_USB_FASTBOOT_FUNCTION_H_
#define SRC_FIRMWARE_DRIVERS_USB_FASTBOOT_FUNCTION_USB_FASTBOOT_FUNCTION_H_

#include <fidl/fuchsia.hardware.fastboot/cpp/wire.h>
#include <fuchsia/hardware/usb/function/cpp/banjo.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/ddk/driver.h>
#include <lib/fzl/owned-vmo-mapper.h>
#include <lib/inspect/cpp/inspect.h>
#include <zircon/compiler.h>

#include <mutex>

#include <ddktl/device.h>
#include <ddktl/protocol/empty-protocol.h>
#include <usb/request-cpp.h>
#include <usb/usb-request.h>

namespace usb_fastboot_function {

// The higher the value of `kBulkReqSize`, the higher the speed. But if set too high, the driver
// will start crashing more often due to memory error. Set to 4k for now which is stable and gives
// decent speed.
constexpr uint32_t kBulkReqSize = 4 * 1024;
constexpr uint16_t kBulkMaxPacketSize = 512;

class UsbFastbootFunction;
using DeviceType = ddk::Device<UsbFastbootFunction, ddk::Initializable,
                               ddk::Messageable<fuchsia_hardware_fastboot::FastbootImpl>::Mixin>;
class UsbFastbootFunction : public DeviceType,
                            public ddk::EmptyProtocol<ZX_PROTOCOL_FASTBOOT>,
                            public ddk::UsbFunctionInterfaceProtocol<UsbFastbootFunction> {
 public:
  explicit UsbFastbootFunction(zx_device_t* parent) : DeviceType(parent), function_(parent) {}

  virtual ~UsbFastbootFunction() = default;

  static zx_status_t Bind(void* ctx, zx_device_t* dev);
  zx_status_t Bind();
  void DdkInit(ddk::InitTxn txn);
  void DdkRelease();

  // For inspect test.
  zx::vmo inspect_vmo() { return inspect_.DuplicateVmo(); }

  // UsbFunctionInterface methods.
  size_t UsbFunctionInterfaceGetDescriptorsSize();
  void UsbFunctionInterfaceGetDescriptors(uint8_t* out_descriptors_buffer, size_t descriptors_size,
                                          size_t* out_descriptors_actual);
  zx_status_t UsbFunctionInterfaceControl(const usb_setup_t* setup, const uint8_t* write_buffer,
                                          size_t write_size, uint8_t* out_read_buffer,
                                          size_t read_size, size_t* out_read_actual);
  zx_status_t UsbFunctionInterfaceSetConfigured(bool configured, usb_speed_t speed);
  zx_status_t UsbFunctionInterfaceSetInterface(uint8_t interface, uint8_t alt_setting);

  void Send(::fuchsia_hardware_fastboot::wire::FastbootImplSendRequest* request,
            SendCompleter::Sync& completer) override;
  void Receive(::fuchsia_hardware_fastboot::wire::FastbootImplReceiveRequest* request,
               ReceiveCompleter::Sync& completer) override;

 private:
  uint8_t bulk_out_addr() const { return descriptors_.bulk_out_ep.b_endpoint_address; }
  uint8_t bulk_in_addr() const { return descriptors_.bulk_in_ep.b_endpoint_address; }
  zx_status_t ConfigureEndpoints(bool enable);

  std::atomic<bool> configured_ = false;

  inspect::Inspector inspect_;
  inspect::BoolProperty is_bound = inspect_.GetRoot().CreateBool("is_bound", false);
  ddk::UsbFunctionProtocolClient function_;
  size_t parent_request_size_ = 0;
  // Size of a usb request taking into account parent request size + alignment + internal
  // bookkeeping. This is calculated by Usb::Request<>::RequestSize() method.
  size_t usb_request_size_ = 0;

  std::mutex send_lock_;
  size_t total_to_send_ __TA_GUARDED(send_lock_) = 0;
  size_t sent_size_ __TA_GUARDED(send_lock_) = 0;
  fzl::OwnedVmoMapper send_vmo_ __TA_GUARDED(send_lock_);
  std::optional<SendCompleter::Async> send_completer_ __TA_GUARDED(send_lock_);
  usb::RequestPool<> bulk_in_reqs_ __TA_GUARDED(send_lock_){};

  std::mutex receive_lock_;
  fzl::OwnedVmoMapper receive_vmo_ __TA_GUARDED(receive_lock_);
  size_t received_size_ __TA_GUARDED(receive_lock_) = 0;
  size_t requested_size_ __TA_GUARDED(receive_lock_) = 0;
  std::optional<ReceiveCompleter::Async> receive_completer_ __TA_GUARDED(receive_lock_);
  usb::RequestPool<> bulk_out_reqs_ __TA_GUARDED(receive_lock_){};

  // USB request completion callback methods.
  void TxComplete(usb_request_t* req);
  void RxComplete(usb_request_t* req);

  // Completion callbacks.
  const usb_request_complete_callback_t rx_complete_ = {
      .callback =
          [](void* ctx, usb_request_t* req) {
            ZX_DEBUG_ASSERT(ctx != nullptr);
            reinterpret_cast<UsbFastbootFunction*>(ctx)->RxComplete(req);
          },
      .ctx = this,
  };
  const usb_request_complete_callback_t tx_complete_ = {
      .callback =
          [](void* ctx, usb_request_t* req) {
            ZX_DEBUG_ASSERT(ctx != nullptr);
            reinterpret_cast<UsbFastbootFunction*>(ctx)->TxComplete(req);
          },
      .ctx = this,
  };

  void CleanUpRx(zx_status_t status, usb::Request<> req) __TA_REQUIRES(receive_lock_);
  void CleanUpTx(zx_status_t status, usb::Request<> req) __TA_REQUIRES(send_lock_);
  zx_status_t PrepareSendRequest(usb::Request<>& req) __TA_REQUIRES(send_lock_);

  // USB Fastboot interface descriptor.
  struct {
    usb_interface_descriptor_t fastboot_intf;
    usb_endpoint_descriptor_t bulk_out_ep;
    usb_endpoint_descriptor_t bulk_in_ep;

    // Fastboot tool checks only up to `bNumInterfaces` of interfaces in each USB device's
    // descriptor to see if it supports fastboot. One issue is that `alternate` interfaces don't
    // count in  `bNumInterfaces`. But it will still be listed in device descriptors. That is, when
    // device has `alternate` interfaces, total number of interfaces in device descriptor is
    // greater than `bNumInterfaces`. Fastboot tool doesn't know to skip them and therefore might
    // miss some of the interfaces.
    //
    // This is the case when fastboot is used with CDC Ethernet, which has an `alternate` interface
    // causing the fastboot tool to miss the fastboot interface. Therefore, we add a placeholder
    // interface as a workaround to increase `bNumInterfaces` by 1 so that it can cover right
    // at `fastboot_intf`.
    //
    // This should be changed if/when the fastboot CLI logic (ffx and upstream fastboot tool) knows
    // how to handle interface alt-configs.
    usb_interface_descriptor_t placehodler_intf;
  } descriptors_ = {
      .fastboot_intf =
          {
              .b_length = sizeof(usb_interface_descriptor_t),
              .b_descriptor_type = USB_DT_INTERFACE,
              .b_interface_number = 0,  // set later
              .b_alternate_setting = 0,
              .b_num_endpoints = 2,
              .b_interface_class = USB_CLASS_VENDOR,
              .b_interface_sub_class = USB_SUBCLASS_FASTBOOT,
              .b_interface_protocol = USB_PROTOCOL_FASTBOOT,
              .i_interface = 0,
          },
      .bulk_out_ep =
          {
              .b_length = sizeof(usb_endpoint_descriptor_t),
              .b_descriptor_type = USB_DT_ENDPOINT,
              .b_endpoint_address = 0,  // set later during AllocEp
              .bm_attributes = USB_ENDPOINT_BULK,
              .w_max_packet_size = htole16(kBulkMaxPacketSize),
              .b_interval = 0,
          },
      .bulk_in_ep =
          {
              .b_length = sizeof(usb_endpoint_descriptor_t),
              .b_descriptor_type = USB_DT_ENDPOINT,
              .b_endpoint_address = 0,  // set later during AllocEp
              .bm_attributes = USB_ENDPOINT_BULK,
              .w_max_packet_size = htole16(kBulkMaxPacketSize),
              .b_interval = 0,
          },
      .placehodler_intf =
          {
              .b_length = sizeof(usb_interface_descriptor_t),
              .b_descriptor_type = USB_DT_INTERFACE,
              .b_interface_number = 0,
              .b_alternate_setting = 0,
              .b_num_endpoints = 0,
              .b_interface_class = USB_CLASS_VENDOR,
              .b_interface_sub_class = 0,
              .b_interface_protocol = 0,
              .i_interface = 0,
          },
  };
};

}  // namespace usb_fastboot_function

#endif  // SRC_FIRMWARE_DRIVERS_USB_FASTBOOT_FUNCTION_USB_FASTBOOT_FUNCTION_H_
