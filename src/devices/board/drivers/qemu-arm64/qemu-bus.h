// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_DEVICES_BOARD_DRIVERS_QEMU_ARM64_QEMU_BUS_H_
#define SRC_DEVICES_BOARD_DRIVERS_QEMU_ARM64_QEMU_BUS_H_

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <lib/pci/root_host.h>
#include <threads.h>

#include <ddktl/device.h>

namespace board_qemu_arm64 {

// BTI IDs for our devices
enum {
  BTI_SYSMEM,
};

class QemuArm64 : public ddk::Device<QemuArm64> {
 public:
  QemuArm64(zx_device_t* parent, fdf::ClientEnd<fuchsia_hardware_platform_bus::PlatformBus> pbus)
      : ddk::Device<QemuArm64>(parent),
        pbus_(std::move(pbus)),
        pci_root_host_(zx::unowned_resource(get_root_resource()), PCI_ADDRESS_SPACE_MEMORY) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  void DdkRelease() { delete this; }

 private:
  zx_status_t Start();
  int Thread();

  zx_status_t PciInit();
  zx_status_t PciAdd();
  zx_status_t RtcInit();
  zx_status_t SysmemInit();

  // TODO(fxbug.dev/108070): Switch to fdf::SyncClient once it's supported.
  fdf::WireSyncClient<fuchsia_hardware_platform_bus::PlatformBus> pbus_;
  PciRootHost pci_root_host_;
  thrd_t thread_;
};

}  // namespace board_qemu_arm64

#endif  // SRC_DEVICES_BOARD_DRIVERS_QEMU_ARM64_QEMU_BUS_H_
