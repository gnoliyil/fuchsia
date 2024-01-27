// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/ddk/binding_driver.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>

#include "simple-display.h"

// simple framebuffer device to match against an AMD Kaveri R7 device already
// initialized from EFI

static zx_status_t kaveri_disp_bind(void* ctx, zx_device_t* dev) {
  // framebuffer bar seems to be 0
  return bind_simple_pci_display_bootloader(dev, "amd", 0u, false);
}

static zx_driver_ops_t kaveri_disp_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = kaveri_disp_bind,
};

ZIRCON_DRIVER(kaveri_disp, kaveri_disp_driver_ops, "zircon", "0.1");
