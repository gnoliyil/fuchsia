// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.device.restarttest/cpp/wire.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>

#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <ddktl/protocol/empty-protocol.h>
#include <fbl/alloc_checker.h>

#include "src/devices/bin/driver_host/tests/test-metadata.h"

using fuchsia_device_restarttest::TestDevice;

class TestDevhostDriver;
using DeviceType =
    ddk::Device<TestDevhostDriver, ddk::Initializable, ddk::Messageable<TestDevice>::Mixin>;
class TestDevhostDriver : public DeviceType, public ddk::EmptyProtocol<ZX_PROTOCOL_DEVHOST_TEST> {
 public:
  TestDevhostDriver(zx_device_t* parent) : DeviceType(parent) {}
  zx_status_t Bind();
  void DdkInit(ddk::InitTxn txn);
  void DdkRelease() { delete this; }

 private:
  struct devhost_test_metadata metadata_;
  size_t metadata_size_;

  void GetPid(GetPidCompleter::Sync& _completer) override;
};

void TestDevhostDriver::GetPid(GetPidCompleter::Sync& completer) {
  zx_koid_t koid;
  auto self = zx_process_self();
  zx_info_handle_basic_t info;
  zx_status_t status =
      zx_object_get_info(self, ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  if (status != ZX_OK) {
    koid = ZX_KOID_INVALID;
  } else {
    koid = info.koid;
  }
  completer.ReplySuccess(koid);
}

zx_status_t TestDevhostDriver::Bind() {
  zxlogf(INFO, "test-devhost-parent bind");
  zxlogf_tag(INFO, "my-tag-2", "log with some tags");
  size_t size;
  zx_status_t status = DdkGetMetadataSize(DEVICE_METADATA_TEST, &size);
  if (status != ZX_OK) {
    return status;
  }

  if (size != sizeof(struct devhost_test_metadata)) {
    zxlogf(ERROR, "Unable to get the metadata correctly. size is %lu\n", size);
    return ZX_ERR_INTERNAL;
  }

  status = DdkGetMetadata(DEVICE_METADATA_TEST, &metadata_, size, &metadata_size_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "Unable to get the metadata. size is %lu\n", size);
    return status;
  }

  return DdkAdd("devhost-test-parent");
}

void TestDevhostDriver::DdkInit(ddk::InitTxn txn) {
  zx_status_t status = DdkAddMetadata(DEVICE_METADATA_PRIVATE, &metadata_, metadata_size_);
  txn.Reply(status);
}

zx_status_t TestDevhostDriverBind(void* ctx, zx_device_t* device) {
  fbl::AllocChecker ac;
  auto dev = fbl::make_unique_checked<TestDevhostDriver>(&ac, device);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  auto status = dev->Bind();
  if (status == ZX_OK) {
    // devmgr is now in charge of the memory for dev
    [[maybe_unused]] auto ptr = dev.release();
  }
  return status;
}

static zx_driver_ops_t test_devhost_driver_ops = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = TestDevhostDriverBind;
  return ops;
}();

ZIRCON_DRIVER(test_devhost_parent, test_devhost_driver_ops, "zircon", "0.1");
