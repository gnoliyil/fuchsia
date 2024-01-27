// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BOARD_DRIVERS_VIM3_VIM3_H_
#define SRC_DEVICES_BOARD_DRIVERS_VIM3_VIM3_H_

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/markers.h>
#include <fuchsia/hardware/clockimpl/cpp/banjo.h>
#include <fuchsia/hardware/gpioimpl/cpp/banjo.h>
#include <fuchsia/hardware/iommu/cpp/banjo.h>
#include <lib/ddk/device.h>
#include <threads.h>

#include <optional>

#include <ddktl/device.h>
#include <fbl/macros.h>
#include <soc/aml-a311d/a311d-hw.h>

namespace vim3 {

// BTI IDs for our devices
enum {
  BTI_CANVAS,
  BTI_DISPLAY,
  BTI_EMMC,
  BTI_ETHERNET,
  BTI_SD,
  BTI_SDIO,
  BTI_SYSMEM,
  BTI_NNA,
  BTI_USB,
  BTI_MALI,
  BTI_VIDEO,
};

// MAC address metadata indices.
// Currently the bootloader only sets up a single MAC zbi entry, we'll use it for both the WiFi and
// BT radio MACs.
enum {
  MACADDR_WIFI = 0,
  MACADDR_BLUETOOTH = 0,
};

class Vim3;
using Vim3Type = ddk::Device<Vim3, ddk::Initializable>;

// This is the main class for the platform bus driver.
class Vim3 : public Vim3Type {
 public:
  Vim3(zx_device_t* parent, fdf::ClientEnd<fuchsia_hardware_platform_bus::PlatformBus> pbus,
       iommu_protocol_t* iommu)
      : Vim3Type(parent), pbus_(std::move(pbus)), iommu_(iommu) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  // Device protocol implementation.
  void DdkInit(ddk::InitTxn txn);
  void DdkRelease() {}

 private:
  DISALLOW_COPY_ASSIGN_AND_MOVE(Vim3);

  zx_status_t CanvasInit();
  zx_status_t ClkInit();
  zx_status_t CpuInit();
  zx_status_t DsiInit();
  zx_status_t DisplayInit();
  zx_status_t EmmcInit();
  zx_status_t EthInit();
  zx_status_t GpioInit();
  zx_status_t HdmiInit();
  zx_status_t I2cInit();
  zx_status_t PowerInit();
  zx_status_t PwmInit();
  zx_status_t RegistersInit();
  zx_status_t SdInit();
  zx_status_t SdioInit();
  zx_status_t Start();
  zx_status_t SysmemInit();
  zx_status_t ThermalInit();
  zx_status_t NnaInit();
  zx_status_t UsbInit();
  zx_status_t MaliInit();
  zx_status_t VideoInit();

  int Thread();

  // TODO(fxbug.dev/108070): migrate to fdf::SyncClient when it is available.
  fdf::WireSyncClient<fuchsia_hardware_platform_bus::PlatformBus> pbus_;
  std::optional<ddk::InitTxn> init_txn_;
  ddk::IommuProtocolClient iommu_;
  ddk::GpioImplProtocolClient gpio_impl_;
  ddk::ClockImplProtocolClient clk_impl_;
  thrd_t thread_;
};

}  // namespace vim3

#endif  // SRC_DEVICES_BOARD_DRIVERS_VIM3_VIM3_H_
