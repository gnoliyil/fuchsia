// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/tests/ddk-fidl-test/fidl-async-llcpp-driver.h"

#include <lib/async/cpp/task.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/platform-defs.h>

#include <memory>
#include <optional>

#include <ddktl/device.h>
#include <fbl/alloc_checker.h>

namespace fidl {

zx_status_t DdkFidlDevice::Create(void* ctx, zx_device_t* dev) {
  fbl::AllocChecker ac;
  std::unique_ptr<DdkFidlDevice> test_dev(new (&ac) DdkFidlDevice(dev));

  if (!ac.check()) {
    zxlogf(ERROR, "DdkFidlDevice::Create: no memory to allocate device!");
    return ZX_ERR_NO_MEMORY;
  }

  zx_status_t status;
  if ((status = test_dev->Bind()) != ZX_OK) {
    zxlogf(ERROR, "DdkFidlDevice::Create: Bind failed");
    test_dev.release()->DdkRelease();
    return status;
  }

  // devmgr is now in charge of the device.
  [[maybe_unused]] auto* dummy = test_dev.release();

  return ZX_OK;
}

void DdkFidlDevice::GetChannel(GetChannelCompleter::Sync& completer) {
  ZX_ASSERT(ZX_OK ==
            async::PostTask(loop_.dispatcher(), [completer = completer.ToAsync()]() mutable {
              zx::channel local;
              zx::channel remote;
              zx::channel::create(0, &local, &remote);
              [[maybe_unused]] auto dummy = local.release();
              completer.Reply(std::move(remote));
            }));
}

zx_status_t DdkFidlDevice::Bind() {
  auto status = loop_.StartThread();
  if (status != ZX_OK) {
    return status;
  }
  return DdkAdd("ddk-async-fidl");
}

void DdkFidlDevice::DdkRelease() { delete this; }

constexpr zx_driver_ops_t driver_ops = []() {
  zx_driver_ops_t driver_ops = {};
  driver_ops.version = DRIVER_OPS_VERSION;
  driver_ops.bind = DdkFidlDevice::Create;
  return driver_ops;
}();

}  // namespace fidl

ZIRCON_DRIVER(ddk_fidl, fidl::driver_ops, "zircon", "0.1");
