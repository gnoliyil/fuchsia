// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "usb-harriet.h"

#include <fuchsia/hardware/usb/c/banjo.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>

#include <fbl/alloc_checker.h>
#include <usb/usb.h>

namespace usb_harriet {

zx_status_t Harriet::Bind() {
  zx_status_t status = DdkAdd("usb-harriet");
  if (status != ZX_OK) {
    return status;
  }
  return ZX_OK;
}

// static
zx_status_t Harriet::Create(zx_device_t* parent) {
  usb::UsbDevice usb(parent);
  if (!usb.is_valid()) {
    return ZX_ERR_PROTOCOL_NOT_SUPPORTED;
  }

  std::optional<usb::InterfaceList> intfs;
  zx_status_t status = usb::InterfaceList::Create(usb, true, &intfs);
  if (status != ZX_OK) {
    return status;
  }
  auto intf = intfs->begin();
  const usb_interface_descriptor_t* intf_desc = intf->descriptor();
  if (intf == intfs->end()) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  uint8_t intf_num = intf_desc->b_interface_number;
  zxlogf(DEBUG, "found intf %u", intf_num);

  for (auto& intf : *intfs) {
    auto ep_itr = intf.GetEndpointList().cbegin();
    do {
      uint8_t ep_type = usb_ep_type(ep_itr->descriptor());
      switch (ep_type) {
        case USB_ENDPOINT_BULK:
        case USB_ENDPOINT_INTERRUPT:
          zxlogf(DEBUG, "%s %s EP 0x%x", ep_type == USB_ENDPOINT_BULK ? "BULK" : "INTERRUPT",
                 usb_ep_direction(ep_itr->descriptor()) == USB_ENDPOINT_OUT ? "OUT" : "IN",
                 ep_itr->descriptor()->b_endpoint_address);
          break;
        default:
          zxlogf(DEBUG, "found additional unexpected EP, type: %u addr 0x%x", ep_type,
                 ep_itr->descriptor()->b_endpoint_address);
      }
    } while (ep_itr++ != intf.GetEndpointList().end());
  }

  fbl::AllocChecker ac;
  auto dev = fbl::make_unique_checked<Harriet>(&ac, parent, usb);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  status = dev->Bind();
  if (status == ZX_OK) {
    // Intentionally leak as it is now held by DevMgr.
    [[maybe_unused]] auto ptr = dev.release();
  }
  return status;
}

zx_status_t harriet_bind(void* ctx, zx_device_t* parent) {
  zxlogf(DEBUG, "harriet_bind");
  return usb_harriet::Harriet::Create(parent);
}

static constexpr zx_driver_ops_t harriet_driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = harriet_bind;
  return ops;
}();

}  // namespace usb_harriet

ZIRCON_DRIVER(usb_harriet, usb_harriet::harriet_driver_ops, "zircon", "0.1");
