// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.device.firmware.test/cpp/wire.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/platform-defs.h>
#include <zircon/assert.h>
#include <zircon/errors.h>

#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <fbl/alloc_checker.h>

#include "src/devices/tests/ddk-firmware-test/test-driver-bind.h"

namespace {

// Keep in sync with test-firmware.txt
constexpr const char TEST_FIRMWARE_CONTENTS[] = "this is some firmware\n";

using fuchsia_device_firmware_test::TestDevice;

class TestFirmwareDriver;
using DeviceType = ddk::Device<TestFirmwareDriver, ddk::Messageable<TestDevice>::Mixin>;

class TestFirmwareDriver : public DeviceType {
 public:
  explicit TestFirmwareDriver(zx_device_t* parent) : DeviceType(parent) {}

  zx_status_t Bind() {
    // Generate a unique child device name in case the driver is bound multiple
    // times.
    std::string name = std::string("ddk-firmware-test-device-").append(std::to_string(counter_));
    counter_++;
    return DdkAdd(name.c_str());
  }

  // Device protocol implementation.
  void DdkRelease() { delete this; }

  // Device message ops implementation.
  void LoadFirmware(LoadFirmwareRequestView request,
                    LoadFirmwareCompleter::Sync& completer) override;

 private:
  static zx_status_t CheckFirmware(zx_handle_t fw, size_t size);

  // A counter to generate unique child device names
  inline static int counter_ = 0;
};

void TestFirmwareDriver::LoadFirmware(LoadFirmwareRequestView request,
                                      LoadFirmwareCompleter::Sync& completer) {
  zx_handle_t fw;
  size_t size;
  std::string str_path(request->path.begin(), request->path.size());
  auto status = load_firmware(zxdev(), str_path.c_str(), &fw, &size);
  if (status != ZX_OK) {
    completer.ReplyError(status);
    ZX_DEBUG_ASSERT(completer.result_of_reply().status() == ZX_OK);
    return;
  }

  TestFirmwareDriver::CheckFirmware(fw, size);
  completer.ReplySuccess();
}

zx_status_t TestFirmwareDriver::CheckFirmware(zx_handle_t fw, size_t size) {
  zx::vmo vmo(fw);
  if (!vmo) {
    return ZX_ERR_INVALID_ARGS;
  }

  if (size != strlen(TEST_FIRMWARE_CONTENTS)) {
    return ZX_ERR_FILE_BIG;
  }
  auto buf = std::make_unique<char[]>(size);
  vmo.read(buf.get(), 0, size);

  if (memcmp(buf.get(), TEST_FIRMWARE_CONTENTS, size) != 0) {
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

zx_status_t TestFirmwareBind(void* ctx, zx_device_t* device) {
  fbl::AllocChecker ac;
  auto dev = fbl::make_unique_checked<TestFirmwareDriver>(&ac, device);
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

zx_driver_ops_t driver_ops = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = TestFirmwareBind;
  return ops;
}();

}  // namespace

ZIRCON_DRIVER(TestFirmware, driver_ops, "zircon", "0.1");
