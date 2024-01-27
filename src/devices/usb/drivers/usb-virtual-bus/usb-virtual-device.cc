// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/usb/drivers/usb-virtual-bus/usb-virtual-device.h"

#include <assert.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fbl/auto_lock.h>
#include <usb/usb-request.h>

#include "src/devices/usb/drivers/usb-virtual-bus/usb-virtual-bus.h"

namespace usb_virtual_bus {

void UsbVirtualDevice::DdkRelease() { delete this; }

void UsbVirtualDevice::UsbDciRequestQueue(usb_request_t* req,
                                          const usb_request_complete_callback_t* complete_cb) {
  bus_->UsbDciRequestQueue(req, complete_cb);
}

zx_status_t UsbVirtualDevice::UsbDciSetInterface(const usb_dci_interface_protocol_t* dci_intf) {
  return bus_->UsbDciSetInterface(dci_intf);
}

zx_status_t UsbVirtualDevice::UsbDciConfigEp(const usb_endpoint_descriptor_t* ep_desc,
                                             const usb_ss_ep_comp_descriptor_t* ss_comp_desc) {
  return bus_->UsbDciConfigEp(ep_desc, ss_comp_desc);
}

zx_status_t UsbVirtualDevice::UsbDciDisableEp(uint8_t ep_address) {
  return bus_->UsbDciDisableEp(ep_address);
}

zx_status_t UsbVirtualDevice::UsbDciCancelAll(uint8_t ep) { return bus_->UsbDciCancelAll(ep); }

zx_status_t UsbVirtualDevice::UsbDciEpSetStall(uint8_t ep_address) {
  return bus_->UsbDciEpSetStall(ep_address);
}

zx_status_t UsbVirtualDevice::UsbDciEpClearStall(uint8_t ep_address) {
  return bus_->UsbDciEpClearStall(ep_address);
}

size_t UsbVirtualDevice::UsbDciGetRequestSize() { return bus_->UsbDciGetRequestSize(); }

void UsbVirtualDevice::ConnectToEndpoint(ConnectToEndpointRequest& request,
                                         ConnectToEndpointCompleter::Sync& completer) {
  completer.Reply(fit::as_error(ZX_ERR_NOT_SUPPORTED));
}

}  // namespace usb_virtual_bus
