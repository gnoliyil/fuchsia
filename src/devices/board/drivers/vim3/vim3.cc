// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/board/drivers/vim3/vim3.h"

#include <assert.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/markers.h>
#include <fuchsia/hardware/gpio/c/banjo.h>
#include <lib/async/cpp/task.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/platform-defs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>

#include "src/devices/board/drivers/vim3/vim3-bind.h"

namespace vim3 {

zx_status_t Vim3::Create(void* ctx, zx_device_t* parent) {
  iommu_protocol_t iommu;

  auto endpoints = fdf::CreateEndpoints<fuchsia_hardware_platform_bus::PlatformBus>();
  if (endpoints.is_error()) {
    return endpoints.error_value();
  }

  zx_status_t status = device_connect_runtime_protocol(
      parent, fuchsia_hardware_platform_bus::Service::PlatformBus::ServiceName,
      fuchsia_hardware_platform_bus::Service::PlatformBus::Name,
      endpoints->server.TakeHandle().release());
  if (status != ZX_OK) {
    return status;
  }

  status = device_get_protocol(parent, ZX_PROTOCOL_IOMMU, &iommu);
  if (status != ZX_OK) {
    return status;
  }

  fbl::AllocChecker ac;
  auto board = fbl::make_unique_checked<Vim3>(&ac, parent, std::move(endpoints->client), &iommu);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  status = board->DdkAdd("vim3");
  if (status != ZX_OK) {
    return status;
  }

  if (status == ZX_OK) {
    // devmgr is now in charge of the device.
    [[maybe_unused]] auto* dummy = board.release();
  }

  return status;
}

int Vim3::Thread() {
  // Load protocol implementation drivers first.
  zx_status_t status;
  if ((status = SysmemInit()) != ZX_OK) {
    zxlogf(ERROR, "SysmemInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = GpioInit()) != ZX_OK) {
    zxlogf(ERROR, "GpioInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = RegistersInit()) != ZX_OK) {
    zxlogf(ERROR, "RegistersInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = ClkInit()) != ZX_OK) {
    zxlogf(ERROR, "ClkInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = I2cInit()) != ZX_OK) {
    zxlogf(ERROR, "I2cInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = EthInit()) != ZX_OK) {
    zxlogf(ERROR, "EthInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = EmmcInit()) != ZX_OK) {
    zxlogf(ERROR, "EmmcInit() failed: %d\n", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = SdInit()) != ZX_OK) {
    zxlogf(ERROR, "SdInit() failed: %d\n", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = SdioInit()) != ZX_OK) {
    zxlogf(ERROR, "SdioInit() failed: %d\n", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = MaliInit()) != ZX_OK) {
    zxlogf(ERROR, "MaliInit() failed: %d\n", status);
  }
  if ((status = NnaInit()) != ZX_OK) {
    zxlogf(ERROR, "NnaInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = UsbInit()) != ZX_OK) {
    zxlogf(ERROR, "UsbInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = CanvasInit()) != ZX_OK) {
    zxlogf(ERROR, "CanvasInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = DsiInit()) != ZX_OK) {
    zxlogf(ERROR, "DsiInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = HdmiInit()) != ZX_OK) {
    zxlogf(ERROR, "HdmiInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = DisplayInit()) != ZX_OK) {
    zxlogf(ERROR, "DisplayInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = PwmInit()) != ZX_OK) {
    zxlogf(ERROR, "PwmInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = PowerInit()) != ZX_OK) {
    zxlogf(ERROR, "PowerInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = CpuInit()) != ZX_OK) {
    zxlogf(ERROR, "CpuInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = ThermalInit()) != ZX_OK) {
    zxlogf(ERROR, "ThermalInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  if ((status = VideoInit()) != ZX_OK) {
    zxlogf(ERROR, "VideoInit() failed: %d", status);
    init_txn_->Reply(ZX_ERR_INTERNAL);
    return status;
  }
  init_txn_->Reply(status);
  return ZX_OK;
}

void Vim3::DdkInit(ddk::InitTxn txn) {
  init_txn_ = std::move(txn);
  async::PostTask(fdf::Dispatcher::GetCurrent()->async_dispatcher(), [this]() { Thread(); });
}

static constexpr zx_driver_ops_t vim3_driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = Vim3::Create;
  return ops;
}();

}  // namespace vim3

ZIRCON_DRIVER(vim3, vim3::vim3_driver_ops, "zircon", "0.1");
