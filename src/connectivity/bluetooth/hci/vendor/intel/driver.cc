// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/bt/hci/c/banjo.h>
#include <fuchsia/hardware/usb/c/banjo.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <zircon/status.h>

#include <cstdint>
#include <cstdio>
#include <future>
#include <thread>

#include "device.h"
#include "logging.h"

// USB Product IDs that use the "secure" firmware method.
constexpr uint16_t sfi_product_ids[] = {0x0025, 0x0a2b, 0x0aaa};

zx_status_t btintel_bind(void* ctx, zx_device_t* device) {
  tracef("bind\n");

  usb_protocol_t usb;
  zx_status_t result = device_get_protocol(device, ZX_PROTOCOL_USB, &usb);
  if (result != ZX_OK) {
    errorf("couldn't get USB protocol: %s\n", zx_status_get_string(result));
    return result;
  }

  usb_device_descriptor_t dev_desc;
  usb_get_device_descriptor(&usb, &dev_desc);

  // Whether this device uses the "secure" firmware method.
  bool secure = false;
  for (uint16_t id : sfi_product_ids) {
    if (dev_desc.id_product == id) {
      secure = true;
      break;
    }
  }

  bt_hci_protocol_t hci;
  result = device_get_protocol(device, ZX_PROTOCOL_BT_HCI, &hci);
  if (result != ZX_OK) {
    errorf("couldn't get BT_HCI protocol: %s\n", zx_status_get_string(result));
    return result;
  }

  auto btdev = new btintel::Device(device, &hci, secure);
  result = btdev->Bind();
  if (result != ZX_OK) {
    errorf("failed binding device: %s\n", zx_status_get_string(result));
    delete btdev;
    return result;
  }
  // Bind succeeded and devmgr is now responsible for releasing |btdev|
  // The device's init hook will load the firmware.
  return ZX_OK;
}

static constexpr zx_driver_ops_t btintel_driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = btintel_bind;
  return ops;
}();

// clang-format off
ZIRCON_DRIVER(bt_hci_intel, btintel_driver_ops, "fuchsia", "0.1");

