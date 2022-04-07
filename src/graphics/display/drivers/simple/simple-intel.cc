// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>

#include "simple-display.h"
#include "src/graphics/display/drivers/simple/simple-intel-bind.h"

static zx_status_t intel_disp_bind(void* ctx, zx_device_t* dev) {
  return bind_simple_pci_display_bootloader(dev, "intel", 2u);
}

static zx_driver_ops_t intel_disp_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = intel_disp_bind,
};

ZIRCON_DRIVER(intel_disp, intel_disp_driver_ops, "zircon", "*0.1");
