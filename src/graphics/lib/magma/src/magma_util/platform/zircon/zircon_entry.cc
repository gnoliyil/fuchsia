// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.gpu.magma/cpp/wire.h>
#include <fuchsia/hardware/platform/device/c/banjo.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/platform-defs.h>
#include <lib/fidl/cpp/wire/arena.h>
#include <lib/fit/thread_safety.h>
#include <zircon/process.h>
#include <zircon/time.h>
#include <zircon/types.h>

#include <memory>

#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <ddktl/protocol/empty-protocol.h>

#include "magma_util/short_macros.h"
#include "platform_handle.h"
#include "platform_logger.h"
#include "platform_trace_provider.h"
#include "platform_trace_provider_with_fdio.h"
#include "src/graphics/lib/magma/src/sys_driver/magma_device_impl.h"
#include "sys_driver/magma_driver.h"
#include "sys_driver/magma_system_device.h"
#include "zircon_platform_status.h"

#if MAGMA_TEST_DRIVER
zx_status_t magma_indriver_test(zx_device_t* device);
#endif

class GpuDevice;

using DdkDeviceType =
    ddk::Device<GpuDevice, magma::MagmaDeviceImpl, ddk::Unbindable, ddk::Initializable>;

class GpuDevice : public DdkDeviceType, public ddk::EmptyProtocol<ZX_PROTOCOL_GPU> {
 public:
  explicit GpuDevice(zx_device_t* parent_device) : DdkDeviceType(parent_device) {}

  void DdkInit(ddk::InitTxn txn);
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();

  zx_status_t Init();

 private:
  zx_status_t MagmaStart() FIT_REQUIRES(magma_mutex());
};

zx_status_t GpuDevice::MagmaStart() {
  set_magma_system_device(magma_driver()->CreateDevice(parent()));
  if (!magma_system_device())
    return DRET_MSG(ZX_ERR_NO_RESOURCES, "Failed to create device");
  InitSystemDevice();
  return ZX_OK;
}

void GpuDevice::DdkInit(ddk::InitTxn txn) {
  set_zx_device(zxdev());
  txn.Reply(InitChildDevices());
}

void GpuDevice::DdkUnbind(ddk::UnbindTxn txn) {
  // This will tear down client connections and cause them to return errors.
  MagmaStop();
  txn.Reply();
}

void GpuDevice::DdkRelease() {
  MAGMA_LOG(INFO, "Starting device_release");

  delete this;
  MAGMA_LOG(INFO, "Finished device_release");

  magma::PlatformTraceProvider::Shutdown();
}

zx_status_t GpuDevice::Init() {
  std::lock_guard<std::mutex> lock(magma_mutex());
  set_magma_driver(MagmaDriver::Create());
#if MAGMA_TEST_DRIVER
  DLOG("running magma indriver test");
  set_unit_test_status(magma_indriver_test(parent()));
#endif

  zx_status_t status = MagmaStart();
  if (status != ZX_OK)
    return status;

  status = DdkAdd(ddk::DeviceAddArgs("magma_gpu")
                      .set_flags(DEVICE_ADD_NON_BINDABLE)
                      .set_inspect_vmo(zx::vmo(magma_driver()->DuplicateInspectVmo())));
  if (status != ZX_OK)
    return DRET_MSG(status, "device_add failed");
  return ZX_OK;
}

static zx_status_t driver_bind(void* context, zx_device_t* parent) {
  MAGMA_LOG(INFO, "driver_bind: binding\n");
  auto gpu = std::make_unique<GpuDevice>(parent);
  if (!gpu)
    return ZX_ERR_NO_MEMORY;

  if (magma::PlatformTraceProvider::Get())
    magma::InitializeTraceProviderWithFdio(magma::PlatformTraceProvider::Get());

  zx_status_t status = gpu->Init();
  if (status != ZX_OK) {
    return status;
  }
  // DdkAdd in Init took ownership of device.
  [[maybe_unused]] GpuDevice* ptr = gpu.release();
  return ZX_OK;
}

zx_driver_ops_t msd_driver_ops = []() constexpr {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = driver_bind;
  return ops;
}();
