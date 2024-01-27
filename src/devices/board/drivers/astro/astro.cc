// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/board/drivers/astro/astro.h"

#include <assert.h>
#include <fidl/fuchsia.hardware.gpio/cpp/wire.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/platform-defs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>

#include "src/devices/board/drivers/astro/astro-bind.h"

namespace astro {
namespace fpbus = fuchsia_hardware_platform_bus;
namespace fhgpio = fuchsia_hardware_gpio;

uint32_t Astro::GetBoardRev() {
  uint32_t board_rev;
  uint8_t id0, id1, id2;

  gpio_impl_.ConfigIn(GPIO_HW_ID0, static_cast<uint32_t>(fhgpio::GpioFlags::kNoPull));
  gpio_impl_.ConfigIn(GPIO_HW_ID1, static_cast<uint32_t>(fhgpio::GpioFlags::kNoPull));
  gpio_impl_.ConfigIn(GPIO_HW_ID2, static_cast<uint32_t>(fhgpio::GpioFlags::kNoPull));
  gpio_impl_.Read(GPIO_HW_ID0, &id0);
  gpio_impl_.Read(GPIO_HW_ID1, &id1);
  gpio_impl_.Read(GPIO_HW_ID2, &id2);
  board_rev = id0 + (id1 << 1) + (id2 << 2);

  if (board_rev >= MAX_SUPPORTED_REV) {
    // We have detected a new board rev. Print this warning just in case the
    // new board rev requires additional support that we were not aware of
    zxlogf(INFO, "Unsupported board revision detected (%d)", board_rev);
  }

  return board_rev;
}

int Astro::Thread() {
  zx_status_t status;

  // Sysmem is started early so zx_vmo_create_contiguous() works.
  if ((status = SysmemInit()) != ZX_OK) {
    zxlogf(ERROR, "%s: SysmemInit() failed: %d", __func__, status);
    return status;
  }

  if ((status = GpioInit()) != ZX_OK) {
    zxlogf(ERROR, "%s: GpioInit() failed: %d", __func__, status);
    return status;
  }

  // Once gpio is up and running, let's populate board revision
  fpbus::BoardInfo info = {};
  fidl::Arena<> fidl_arena;
  info.board_revision() = GetBoardRev();
  auto result = pbus_.buffer(fdf::Arena('ASTR'))->SetBoardInfo(fidl::ToWire(fidl_arena, info));
  if (!result.ok()) {
    zxlogf(ERROR, "%s: SetBoardInfo request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: SetBoardInfo failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }
  zxlogf(INFO, "Detected board rev 0x%x", info.board_revision().value());

  if (*info.board_revision() != BOARD_REV_DVT && *info.board_revision() != BOARD_REV_PVT) {
    zxlogf(ERROR, "Unsupported board revision %u. Booting will not continue",
           *info.board_revision());
    return -1;
  }

  if ((status = RegistersInit()) != ZX_OK) {
    zxlogf(ERROR, "%s: RegistersInit() failed: %d", __func__, status);
    return status;
  }

  if ((status = ClkInit()) != ZX_OK) {
    zxlogf(ERROR, "ClkInit failed: %d", status);
  }

  if ((status = PowerInit()) != ZX_OK) {
    zxlogf(ERROR, "PowerInit failed: %d", status);
  }

  if ((status = ButtonsInit()) != ZX_OK) {
    zxlogf(ERROR, "ButtonsInit failed: %d", status);
  }

  if ((status = I2cInit()) != ZX_OK) {
    zxlogf(ERROR, "I2cInit failed: %d", status);
  }

  if ((status = CpuInit()) != ZX_OK) {
    zxlogf(ERROR, "CpuInit failed: %d", status);
  }

  if ((status = MaliInit()) != ZX_OK) {
    zxlogf(ERROR, "MaliInit failed: %d", status);
  }

  if ((status = UsbInit()) != ZX_OK) {
    zxlogf(ERROR, "UsbInit failed: %d", status);
  }

  if ((status = TouchInit()) != ZX_OK) {
    zxlogf(ERROR, "TouchInit failed: %d", status);
  }

  if ((status = DsiInit()) != ZX_OK) {
    zxlogf(ERROR, "DsiInit failed: %d", status);
  }

  if ((status = DisplayInit()) != ZX_OK) {
    zxlogf(ERROR, "DisplayInit failed: %d", status);
  }

  if ((status = CanvasInit()) != ZX_OK) {
    zxlogf(ERROR, "CanvasInit failed: %d", status);
  }

  if ((status = PwmInit()) != ZX_OK) {
    zxlogf(ERROR, "PwmInit failed: %d", status);
  }

  if ((status = TeeInit()) != ZX_OK) {
    zxlogf(ERROR, "TeeInit failed: %d", status);
  }

  if ((status = VideoInit()) != ZX_OK) {
    zxlogf(ERROR, "VideoInit failed: %d", status);
  }

  if ((status = RawNandInit()) != ZX_OK) {
    zxlogf(ERROR, "RawNandInit failed: %d", status);
  }

  if ((status = SdioInit()) != ZX_OK) {
    zxlogf(ERROR, "SdioInit failed: %d", status);
  }

  if ((status = LightInit()) != ZX_OK) {
    zxlogf(ERROR, "LightInit failed: %d", status);
  }

  if ((status = ThermalInit()) != ZX_OK) {
    zxlogf(ERROR, "ThermalInit failed: %d", status);
  }

  if ((status = ThermistorInit()) != ZX_OK) {
    zxlogf(ERROR, "ThermistorInit failed: %d", status);
  }

  if ((status = AudioInit()) != ZX_OK) {
    zxlogf(ERROR, "AudioInit failed: %d", status);
  }

  if ((status = SecureMemInit()) != ZX_OK) {
    zxlogf(ERROR, "SecureMemInit failed: %d", status);
  }

  if ((status = BacklightInit()) != ZX_OK) {
    zxlogf(ERROR, "BacklightInit failed: %d", status);
  }

  if ((status = RamCtlInit()) != ZX_OK) {
    zxlogf(ERROR, "RamCtlInit failed: %d", status);
  }

  // This function includes some non-trivial delays, so lets run this last
  // to avoid slowing down the rest of the boot.
  if ((status = BluetoothInit()) != ZX_OK) {
    zxlogf(ERROR, "BluetoothInit failed: %d", status);
  }

  return ZX_OK;
}

zx_status_t Astro::Start() {
  int rc = thrd_create_with_name(
      &thread_, [](void* arg) -> int { return reinterpret_cast<Astro*>(arg)->Thread(); }, this,
      "astro-start-thread");
  if (rc != thrd_success) {
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

void Astro::DdkRelease() { delete this; }

zx_status_t Astro::Create(void* ctx, zx_device_t* parent) {
  auto endpoints = fdf::CreateEndpoints<fuchsia_hardware_platform_bus::PlatformBus>();
  if (endpoints.is_error()) {
    return endpoints.error_value();
  }

  zx_status_t status = device_connect_runtime_protocol(
      parent, fpbus::Service::PlatformBus::ServiceName, fpbus::Service::PlatformBus::Name,
      endpoints->server.TakeHandle().release());
  if (status != ZX_OK) {
    return status;
  }

  iommu_protocol_t iommu;

  status = device_get_protocol(parent, ZX_PROTOCOL_IOMMU, &iommu);
  if (status != ZX_OK) {
    return status;
  }

  fbl::AllocChecker ac;
  auto board = fbl::make_unique_checked<Astro>(&ac, parent, std::move(endpoints->client), &iommu);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  status = board->DdkAdd("astro", DEVICE_ADD_NON_BINDABLE);
  if (status != ZX_OK) {
    return status;
  }

  // Start up our protocol helpers and platform devices.
  status = board->Start();
  if (status == ZX_OK) {
    // devmgr is now in charge of the device.
    [[maybe_unused]] auto* dummy = board.release();
  }
  return status;
}

static zx_driver_ops_t astro_driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = Astro::Create;
  return ops;
}();

}  // namespace astro

ZIRCON_DRIVER(aml_bus, astro::astro_driver_ops, "zircon", "0.1");
