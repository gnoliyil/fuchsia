// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/create/goldens/my-driver-cpp/my_driver_cpp.h"

#include <lib/ddk/binding_driver.h>

namespace my_driver_cpp {

zx_status_t MyDriverCpp::Bind(void* ctx, zx_device_t* dev) {
  auto driver = std::make_unique<MyDriverCpp>(dev);
  zx_status_t status = driver->Bind();
  if (status != ZX_OK) {
    return status;
  }
  // The DriverFramework now owns driver.
  [[maybe_unused]] auto ptr = driver.release();
  return ZX_OK;
}

zx_status_t MyDriverCpp::Bind() {
  is_bound.Set(true);
  return DdkAdd(ddk::DeviceAddArgs("my_driver_cpp").set_inspect_vmo(inspect_.DuplicateVmo()));
}

void MyDriverCpp::DdkInit(ddk::InitTxn txn) { txn.Reply(ZX_OK); }

void MyDriverCpp::DdkRelease() { delete this; }

static zx_driver_ops_t my_driver_cpp_driver_ops = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = MyDriverCpp::Bind;
  return ops;
}();

}  // namespace my_driver_cpp

ZIRCON_DRIVER(MyDriverCpp, my_driver_cpp::my_driver_cpp_driver_ops, "zircon", "0.1");
