// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sysmem-proxy-device.h"

#include <fidl/fuchsia.sysmem/cpp/wire.h>
#include <fidl/fuchsia.sysmem2/cpp/wire.h>
#include <inttypes.h>
#include <lib/async/cpp/task.h>
#include <lib/async/dispatcher.h>
#include <lib/ddk/device.h>
#include <lib/ddk/platform-defs.h>
#include <lib/fdio/directory.h>
#include <lib/sync/completion.h>
#include <lib/sysmem-version/sysmem-version.h>
#include <lib/zx/channel.h>
#include <lib/zx/event.h>
#include <zircon/assert.h>

#include <memory>

namespace display {

// severity can be ERROR, WARN, INFO, DEBUG, TRACE.  See ddk/debug.h.
//
// Using ## __VA_ARGS__ instead of __VA_OPT__(,) __VA_ARGS__ for now, since
// __VA_OPT__ doesn't seem to be available yet.
#define LOG(severity, fmt, ...) \
  zxlogf(severity, "[%s:%s:%d] " fmt "\n", "display", __func__, __LINE__, ##__VA_ARGS__)

SysmemProxyDevice::SysmemProxyDevice(zx_device_t* parent_device,
                                     sysmem_driver::Driver* parent_driver)
    : DdkDeviceType2(parent_device),
      parent_driver_(parent_driver),
      loop_(&kAsyncLoopConfigNeverAttachToThread),
      in_proc_sysmem_protocol_{.ops = &sysmem_protocol_ops_, .ctx = this} {
  ZX_DEBUG_ASSERT(parent_);
  ZX_DEBUG_ASSERT(parent_driver_);
  zx_status_t status = loop_.StartThread("sysmem", &loop_thrd_);
  ZX_ASSERT(status == ZX_OK);
}

void SysmemProxyDevice::ConnectV1(ConnectV1RequestView request,
                                  ConnectV1Completer::Sync& completer) {
  zx_status_t status = SysmemConnect(request->allocator_request.TakeChannel());
  if (status != ZX_OK) {
    LOG(INFO, "SysmemConnect() failed");
    return;
  }
}

void SysmemProxyDevice::ConnectV2(ConnectV2RequestView request,
                                  ConnectV2Completer::Sync& completer) {
  zx_status_t status = SysmemConnectV2(request->allocator_request.TakeChannel());
  if (status != ZX_OK) {
    LOG(INFO, "SysmemConnect() failed");
    return;
  }
}

void SysmemProxyDevice::SetAuxServiceDirectory(SetAuxServiceDirectoryRequestView request,
                                               SetAuxServiceDirectoryCompleter::Sync& completer) {
  LOG(INFO, "SysmemProxyDevice::SetAuxServiceDirectory() not supported");
}

zx_status_t SysmemProxyDevice::SysmemConnect(zx::channel allocator_request) {
  const char* kSvcPath = "/svc/fuchsia.sysmem.Allocator";
  LOG(INFO, "fdio_service_connect to service service: %s", kSvcPath);
  return fdio_service_connect(kSvcPath, allocator_request.release());
}

zx_status_t SysmemProxyDevice::SysmemConnectV2(zx::channel allocator_request) {
  const char* kSvcPath = "/svc/fuchsia.sysmem2.Allocator";
  LOG(INFO, "fdio_service_connect to service service: %s", kSvcPath);
  return fdio_service_connect(kSvcPath, allocator_request.release());
}

zx_status_t SysmemProxyDevice::SysmemRegisterHeap(uint64_t heap, zx::channel heap_connection) {
  ZX_ASSERT(false);
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t SysmemProxyDevice::SysmemRegisterSecureMem(zx::channel tee_connection) {
  ZX_ASSERT(false);
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t SysmemProxyDevice::SysmemUnregisterSecureMem() {
  ZX_ASSERT(false);
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t SysmemProxyDevice::Bind() {
  auto pdev_client = DdkConnectFidlProtocol<fuchsia_hardware_platform_device::Service::Device>();
  if (pdev_client.is_error()) {
    LOG(ERROR, "Failed DdkConnectFidlProtocol() for fuchsia.hardware.platform.device - status: %s",
        pdev_client.status_string());
    return pdev_client.status_value();
  }

  pdev_ = fidl::SyncClient(std::move(*pdev_client));

  zx_status_t status = DdkAdd(ddk::DeviceAddArgs("sysmem")
                                  .set_flags(DEVICE_ADD_ALLOW_MULTI_COMPOSITE)
                                  .set_inspect_vmo(inspector_.DuplicateVmo()));
  if (status != ZX_OK) {
    LOG(ERROR, "Failed to bind device");
    return status;
  }

  return ZX_OK;
}

void SysmemProxyDevice::DdkUnbind(ddk::UnbindTxn txn) {
  // Ensure all tasks started before this call finish before shutting down the loop.
  async::PostTask(loop_.dispatcher(), [this]() { loop_.Quit(); });
  // JoinThreads waits for the Quit() to execute and cause the thread to exit.
  loop_.JoinThreads();
  loop_.Shutdown();
  // After this point the FIDL servers should have been shutdown and all DDK and other protocol
  // methods will error out because posting tasks to the dispatcher fails.
  txn.Reply();
}

}  // namespace display
