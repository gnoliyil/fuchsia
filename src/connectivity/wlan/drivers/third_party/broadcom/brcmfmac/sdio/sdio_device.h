// Copyright (c) 2019 The Fuchsia Authors
//
// Permission to use, copy, modify, and/or distribute this software for any purpose with or without
// fee is hereby granted, provided that the above copyright notice and this permission notice
// appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
// SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
// AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
// NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
// OF THIS SOFTWARE.

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SDIO_SDIO_DEVICE_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SDIO_SDIO_DEVICE_H_

#include <lib/async-loop/cpp/loop.h>
#include <lib/async/dispatcher.h>
#include <lib/ddk/device.h>
#include <zircon/types.h>

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/device.h"

namespace wlan::brcmfmac {

class DeviceInspect;

// This class uses the DDKTL classes to manage the lifetime of a brcmfmac driver instance.
class SdioDevice : public Device {
 public:
  SdioDevice(const SdioDevice& device) = delete;
  SdioDevice& operator=(const SdioDevice& other) = delete;
  ~SdioDevice() override;

  // Static factory function for SdioDevice instances. This factory does not return an owned
  // instance, as on successful invocation the instance will have its lifecycle managed by the
  // devhost.
  static zx_status_t Create(zx_device_t* parent_device);

  // Virtual state accessor implementation.
  async_dispatcher_t* GetDispatcher() override;
  DeviceInspect* GetInspect() override;

  // Trampolines for DDK functions, for platforms that support them
  zx_status_t DeviceInit() override;
  zx_status_t DeviceAdd(device_add_args_t* args, zx_device_t** out_device) override;
  void DeviceAsyncRemove(zx_device_t* dev) override;
  zx_status_t LoadFirmware(const char* path, zx_handle_t* fw, size_t* size) override;
  zx_status_t DeviceGetMetadata(uint32_t type, void* buf, size_t buflen, size_t* actual) override;

 protected:
  void Shutdown() override;

 private:
  explicit SdioDevice(zx_device_t* parent);

  std::unique_ptr<async::Loop> async_loop_;
  std::unique_ptr<DeviceInspect> inspect_;
  std::unique_ptr<brcmf_bus> brcmf_bus_;
};

}  // namespace wlan::brcmfmac

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SDIO_SDIO_DEVICE_H_
