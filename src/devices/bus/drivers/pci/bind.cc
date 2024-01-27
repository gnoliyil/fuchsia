// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <fuchsia/hardware/pciroot/cpp/banjo.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/device.h>
#include <lib/ddk/platform-defs.h>

#include "src/devices/bus/drivers/pci/bus.h"

static constexpr zx_driver_ops_t pci_driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = pci::pci_bus_bind;
  return ops;
}();

ZIRCON_DRIVER(pci, pci_driver_ops, "zircon", "0.1");
