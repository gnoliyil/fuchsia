// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BOARD_DRIVERS_CLOVER_CLOVER_H_
#define SRC_DEVICES_BOARD_DRIVERS_CLOVER_CLOVER_H_

#include <fidl/fuchsia.hardware.gpio.init/cpp/wire.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <fuchsia/hardware/iommu/cpp/banjo.h>
#include <lib/ddk/device.h>
#include <threads.h>

#include <optional>

#include <ddktl/device.h>
#include <fbl/macros.h>

#include "src/devices/bus/lib/platform-bus-composites/platform-bus-composite.h"

namespace clover {
namespace fpbus = fuchsia_hardware_platform_bus;

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
  BTI_SPI0,
  BTI_SPI1,
  BTI_AUDIO_OUT,
  BTI_AUDIO_IN,
  BTI_TEE,
};

// Clover SPI bus arbiters (should match spi_channels[] in clover-spi.cc  ).
enum {
  CLOVER_SPICC0,
};

class Clover;
using CloverType = ddk::Device<Clover, ddk::Initializable>;

// This is the main class for the platform bus driver.
class Clover : public CloverType {
 public:
  Clover(zx_device_t* parent, fdf::ClientEnd<fuchsia_hardware_platform_bus::PlatformBus> pbus,
         iommu_protocol_t* iommu)
      : CloverType(parent), pbus_(std::move(pbus)), iommu_(iommu) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  // Device protocol implementation.
  void DdkInit(ddk::InitTxn txn);
  void DdkRelease() {}

 private:
  Clover(const Clover&) = delete;
  Clover& operator=(const Clover&) = delete;
  Clover(Clover&&) = delete;
  Clover& operator=(Clover&&) = delete;

  int Thread();
  zx_status_t GpioInit();
  zx_status_t ClkInit();
  zx_status_t SysmemInit();
  zx_status_t TeeInit();
  zx_status_t ThermalInit();
  zx_status_t DmcInit();
  zx_status_t I2cInit();
  zx_status_t SpiInit();
  zx_status_t RegistersInit();
  zx_status_t PwmInit();
  zx_status_t SpiNandInit();
  zx_status_t CpuInit();
  zx_status_t SdioInit();
  zx_status_t MailboxInit();

  fdf::WireSyncClient<fuchsia_hardware_platform_bus::PlatformBus> pbus_;
  std::optional<ddk::InitTxn> init_txn_;
  ddk::IommuProtocolClient iommu_;
  thrd_t thread_;

  fidl::Arena<> gpio_init_arena_;
  std::vector<fuchsia_hardware_gpio_init::wire::GpioInitStep> gpio_init_steps_;
};

}  // namespace clover

#endif  // SRC_DEVICES_BOARD_DRIVERS_CLOVER_CLOVER_H_
