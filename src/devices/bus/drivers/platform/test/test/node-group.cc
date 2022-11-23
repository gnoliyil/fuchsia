// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/ddk/debug.h>
#include <lib/ddk/driver.h>

#include <memory>

#include <ddktl/device.h>

#include "src/devices/bus/drivers/platform/test/test-node-group-bind.h"

namespace node_group {

class TestNodeGroup;
using DeviceType = ddk::Device<TestNodeGroup>;

class TestNodeGroup : public DeviceType {
 public:
  static zx_status_t Create(void* ctx, zx_device_t* parent) {
    auto dev = std::make_unique<TestNodeGroup>(parent);

    zxlogf(INFO, "TestNodeGroup::Create: test-node-group");

    auto status = dev->DdkAdd("test-node-group");
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: DdkAdd failed: %d", __func__, status);
      return status;
    }

    __UNUSED auto ptr = dev.release();

    return ZX_OK;
  }

  explicit TestNodeGroup(zx_device_t* parent) : DeviceType(parent) {}

  void DdkRelease() { delete this; }
};

constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t driver_ops = {};
  driver_ops.version = DRIVER_OPS_VERSION;
  driver_ops.bind = TestNodeGroup::Create;
  return driver_ops;
}();

}  // namespace node_group

ZIRCON_DRIVER(test_node_group, node_group::driver_ops, "zircon", "0.1");
