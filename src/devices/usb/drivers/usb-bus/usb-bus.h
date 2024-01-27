// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_USB_DRIVERS_USB_BUS_USB_BUS_H_
#define SRC_DEVICES_USB_DRIVERS_USB_BUS_USB_BUS_H_

#include <fuchsia/hardware/usb/bus/cpp/banjo.h>
#include <fuchsia/hardware/usb/hci/cpp/banjo.h>
#include <lib/sync/cpp/completion.h>

#include <ddktl/device.h>
#include <fbl/array.h>
#include <fbl/ref_ptr.h>
#include <usb/usb-request.h>
#include <usb/usb.h>

namespace usb_bus {

class UsbBus;
class UsbDevice;
using UsbBusType = ddk::Device<UsbBus, ddk::Unbindable, ddk::ChildPreReleaseable>;

class UsbBus : public UsbBusType,
               public ddk::UsbBusProtocol<UsbBus, ddk::base_protocol>,
               public ddk::UsbBusInterfaceProtocol<UsbBus> {
 public:
  UsbBus(zx_device_t* parent) : UsbBusType(parent), hci_(parent) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  // Device protocol implementation.
  void DdkChildPreRelease(void* child_ctx);
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();

  // USB Bus protocol implementation.
  zx_status_t UsbBusConfigureHub(/* zx_device_t* */ uint64_t hub_device, usb_speed_t speed,
                                 const usb_hub_descriptor_t* desc, bool multi_tt);
  zx_status_t UsbBusDeviceAdded(/* zx_device_t* */ uint64_t hub_device, uint32_t port,
                                usb_speed_t speed);
  zx_status_t UsbBusDeviceRemoved(/* zx_device_t* */ uint64_t hub_device, uint32_t port);
  zx_status_t UsbBusSetHubInterface(/* zx_device_t* */ uint64_t usb_device,
                                    const usb_hub_interface_protocol_t* hub);
  void UsbBusRequestQueue(usb_request_t* usb_request,
                          const usb_request_complete_callback_t* complete_cb) {
    usb_request_complete(usb_request, ZX_ERR_NOT_SUPPORTED, 0, complete_cb);
  }

  // USB Bus interface implementation.
  zx_status_t UsbBusInterfaceAddDevice(uint32_t device_id, uint32_t hub_id, usb_speed_t speed);
  zx_status_t UsbBusInterfaceRemoveDevice(uint32_t device_id);
  zx_status_t UsbBusInterfaceResetPort(uint32_t hub_id, uint32_t port, bool enumerating);
  zx_status_t UsbBusInterfaceReinitializeDevice(uint32_t device_id);

 private:
  DISALLOW_COPY_ASSIGN_AND_MOVE(UsbBus);

  zx_status_t Init();

  zx_status_t GetDeviceId(/* zx_device_t* */ uint64_t device, uint32_t* out);

  // Our parent's HCI protocol.
  const ddk::UsbHciProtocolClient hci_;
  // Array of all our USB devices.
  fbl::Array<fbl::RefPtr<UsbDevice>> devices_;
  // Map for storing sync completion objects for all USB devices.
  // This will be used to signal USB device remove completion.
  // The USBDevice* is used just as a key and will not be dereferenced.
  std::map<UsbDevice*, libsync::Completion> remove_completion_;
};

}  // namespace usb_bus

#endif  // SRC_DEVICES_USB_DRIVERS_USB_BUS_USB_BUS_H_
