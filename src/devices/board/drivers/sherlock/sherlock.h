// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVICES_BOARD_DRIVERS_SHERLOCK_SHERLOCK_H_
#define SRC_DEVICES_BOARD_DRIVERS_SHERLOCK_SHERLOCK_H_

#include <fidl/fuchsia.hardware.clockimpl/cpp/wire.h>
#include <fidl/fuchsia.hardware.gpioimpl/cpp/wire.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fuchsia/hardware/iommu/cpp/banjo.h>
#include <lib/ddk/device.h>
#include <zircon/types.h>

#include <ddktl/device.h>
#include <fbl/macros.h>
#include <soc/aml-t931/t931-hw.h>

#include "sdk/lib/driver/outgoing/cpp/outgoing_directory.h"
#include "src/devices/board/drivers/sherlock/sherlock-btis.h"
namespace sherlock {

// MAC address metadata indices
enum {
  MACADDR_WIFI = 0,
  MACADDR_BLUETOOTH = 1,
};

// These should match the mmio table defined in sherlock-i2c.c
enum {
  SHERLOCK_I2C_A0_0,
  SHERLOCK_I2C_2,
  SHERLOCK_I2C_3,
};

// These should match the mmio table defined in sherlock-spi.c
enum { SHERLOCK_SPICC0, SHERLOCK_SPICC1 };

class Sherlock;
using SherlockType = ddk::Device<Sherlock, ddk::Initializable>;

// This is the main class for the platform bus driver.
class Sherlock : public SherlockType {
 public:
  explicit Sherlock(zx_device_t* parent,
                    fdf::ClientEnd<fuchsia_hardware_platform_bus::PlatformBus> pbus,
                    iommu_protocol_t* iommu)
      : SherlockType(parent),
        pbus_(std::move(pbus)),
        iommu_(iommu),
        outgoing_(fdf::Dispatcher::GetCurrent()->get()) {}

  static zx_status_t Create(void* ctx, zx_device_t* parent);

  // Device protocol implementation.
  void DdkInit(ddk::InitTxn txn);
  void DdkRelease();

 private:
  DISALLOW_COPY_ASSIGN_AND_MOVE(Sherlock);

  void Serve(fdf::ServerEnd<fuchsia_hardware_platform_bus::PlatformBus> request) {
    device_connect_runtime_protocol(
        parent(), fuchsia_hardware_platform_bus::Service::PlatformBus::ServiceName,
        fuchsia_hardware_platform_bus::Service::PlatformBus::Name, request.TakeChannel().release());
  }

  zx_status_t Start();
  zx::result<> AdcInit();
  zx_status_t SysmemInit();
  zx_status_t GpioInit();
  zx_status_t RegistersInit();
  zx_status_t CanvasInit();
  zx_status_t I2cInit();
  zx_status_t SpiInit();
  zx_status_t UsbInit();
  zx_status_t EmmcInit();
  zx_status_t BCM43458LpoClockInit();  // required for BCM43458 wifi/bluetooth chip.
  zx_status_t SdioInit();
  zx_status_t BluetoothInit();
  zx_status_t ClkInit();
  zx_status_t CameraInit();
  zx_status_t MaliInit();
  zx_status_t TeeInit();
  zx_status_t VideoInit();
  zx_status_t VideoEncInit();
  zx_status_t HevcEncInit();
  zx_status_t ButtonsInit();
  zx_status_t AudioInit();
  zx_status_t ThermalInit();
  zx_status_t LightInit();
  zx_status_t OtRadioInit();
  zx_status_t BacklightInit();
  zx_status_t NnaInit();
  zx_status_t SecureMemInit();
  zx_status_t PwmInit();
  zx_status_t RamCtlInit();
  zx_status_t CpuInit();
  zx_status_t ThermistorInit();
  zx_status_t DsiInit();
  zx_status_t AddPostInitDevice();
  int Thread();

  zx_status_t EnableWifi32K(void);

  static fuchsia_hardware_gpioimpl::wire::InitCall GpioConfigIn(
      fuchsia_hardware_gpio::GpioFlags flags) {
    return fuchsia_hardware_gpioimpl::wire::InitCall::WithInputFlags(flags);
  }

  static fuchsia_hardware_gpioimpl::wire::InitCall GpioConfigOut(uint8_t initial_value) {
    return fuchsia_hardware_gpioimpl::wire::InitCall::WithOutputValue(initial_value);
  }

  fuchsia_hardware_gpioimpl::wire::InitCall GpioSetAltFunction(uint64_t function) {
    return fuchsia_hardware_gpioimpl::wire::InitCall::WithAltFunction(init_arena_, function);
  }

  fuchsia_hardware_gpioimpl::wire::InitCall GpioSetDriveStrength(uint64_t ds_ua) {
    return fuchsia_hardware_gpioimpl::wire::InitCall::WithDriveStrengthUa(init_arena_, ds_ua);
  }

  fdf::WireSyncClient<fuchsia_hardware_platform_bus::PlatformBus> pbus_;
  ddk::IommuProtocolClient iommu_;
  fidl::Arena<> init_arena_;
  std::vector<fuchsia_hardware_gpioimpl::wire::InitStep> gpio_init_steps_;
  std::vector<fuchsia_hardware_clockimpl::wire::InitStep> clock_init_steps_;

  fdf::OutgoingDirectory outgoing_;
};

}  // namespace sherlock

#endif  // SRC_DEVICES_BOARD_DRIVERS_SHERLOCK_SHERLOCK_H_
