// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/test/stub_device.h"

#include <zircon/types.h>

#include <ddktl/init-txn.h>

namespace wlan {
namespace brcmfmac {

StubDevice::StubDevice() : Device(nullptr) {}

StubDevice::~StubDevice() = default;

void StubDevice::Shutdown() {}

async_dispatcher_t* StubDevice::GetDispatcher() { return nullptr; }

DeviceInspect* StubDevice::GetInspect() { return nullptr; }

zx_status_t StubDevice::DeviceInit() { return ZX_ERR_NOT_SUPPORTED; }

zx_status_t StubDevice::DeviceAdd(device_add_args_t* args, zx_device_t** out_device) {
  return ZX_ERR_NOT_SUPPORTED;
}

void StubDevice::DeviceAsyncRemove(zx_device_t* dev) {}

zx_status_t StubDevice::LoadFirmware(const char* path, zx_handle_t* fw, size_t* size) {
  return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t StubDevice::DeviceGetMetadata(uint32_t type, void* buf, size_t buflen, size_t* actual) {
  return ZX_ERR_NOT_SUPPORTED;
}

}  // namespace brcmfmac
}  // namespace wlan
