// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/board/drivers/nelson/nelson.h"

#include <assert.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <fuchsia/hardware/gpio/c/banjo.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>

#include "src/devices/board/drivers/nelson/nelson-bind.h"
#include "src/devices/board/drivers/nelson/nelson-gpios.h"

namespace nelson {
namespace fpbus = fuchsia_hardware_platform_bus;

uint32_t Nelson::GetBoardRev() {
  if (!board_rev_) {
    uint32_t board_rev;
    uint8_t id0, id1, id2;

    gpio_impl_.ConfigIn(GPIO_HW_ID0, GPIO_NO_PULL);
    gpio_impl_.ConfigIn(GPIO_HW_ID1, GPIO_NO_PULL);
    gpio_impl_.ConfigIn(GPIO_HW_ID2, GPIO_NO_PULL);
    gpio_impl_.Read(GPIO_HW_ID0, &id0);
    gpio_impl_.Read(GPIO_HW_ID1, &id1);
    gpio_impl_.Read(GPIO_HW_ID2, &id2);
    board_rev = id0 + (id1 << 1) + (id2 << 2);

    if (board_rev >= MAX_SUPPORTED_REV) {
      // We have detected a new board rev. Print this warning just in case the
      // new board rev requires additional support that we were not aware of
      zxlogf(INFO, "Unsupported board revision detected (%d)", board_rev);
    }

    board_rev_.emplace(board_rev);
  }

  return *board_rev_;
}

uint32_t Nelson::GetBoardOption() {
  if (!board_option_) {
    uint8_t id3, id4;

    gpio_impl_.ConfigIn(GPIO_HW_ID3, GPIO_NO_PULL);
    gpio_impl_.ConfigIn(GPIO_HW_ID4, GPIO_NO_PULL);
    gpio_impl_.Read(GPIO_HW_ID3, &id3);
    gpio_impl_.Read(GPIO_HW_ID4, &id4);

    board_option_.emplace(id3 + (id4 << 1));
  }

  return *board_option_;
}

uint32_t Nelson::GetDisplayId() {
  if (!display_id_) {
    uint8_t id0, id1;
    gpio_impl_.ConfigIn(GPIO_DISP_SOC_ID0, GPIO_NO_PULL);
    gpio_impl_.ConfigIn(GPIO_DISP_SOC_ID1, GPIO_NO_PULL);
    gpio_impl_.Read(GPIO_DISP_SOC_ID0, &id0);
    gpio_impl_.Read(GPIO_DISP_SOC_ID1, &id1);
    display_id_.emplace((id1 << 1) | id0);
  }

  return *display_id_;
}

int Nelson::Thread() {
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
  info.board_revision() = GetBoardRev();
  fidl::Arena<> fidl_arena;
  auto result = pbus_.buffer(fdf::Arena('INFO'))->SetBoardInfo(fidl::ToWire(fidl_arena, info));
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
  zxlogf(INFO, "Detected board rev 0x%x", info.board_revision().value_or(-1));

  if ((info.board_revision() != BOARD_REV_P1) && (info.board_revision() != BOARD_REV_P2) &&
      (info.board_revision() != BOARD_REV_EVT) && (info.board_revision() != BOARD_REV_DVT) &&
      (info.board_revision() != BOARD_REV_DVT2)) {
    zxlogf(ERROR, "Unsupported board revision %u. Booting will not continue",
           info.board_revision().value_or(-1));
    return -1;
  }

  // This is tightly coupled to the u-boot supplied metadata and the GT6853 touch driver.
  size_t metadata_size;
  uint32_t bootloader_display_id = 0;
  status = DdkGetMetadataSize(DEVICE_METADATA_BOARD_PRIVATE, &metadata_size);
  if (status == ZX_OK) {
    if (metadata_size != sizeof(uint32_t)) {
      zxlogf(ERROR, "%s: bootloader board metadata is the wrong size, got %zu want %zu", __func__,
             metadata_size, sizeof(uint32_t));
    } else {
      size_t actual;
      status = DdkGetMetadata(DEVICE_METADATA_BOARD_PRIVATE, &bootloader_display_id, metadata_size,
                              &actual);
      if (status != ZX_OK || actual != metadata_size) {
        zxlogf(ERROR, "%s: bootloader board metadata could not be read (%d, size=%zu)", __func__,
               status, actual);
        bootloader_display_id = 0;
      }
    }
  } else {
    zxlogf(ERROR, "%s: no panel type metadata (%d), falling back to GPIO inspection", __func__,
           status);
  }

  if ((status = RegistersInit()) != ZX_OK) {
    zxlogf(ERROR, "RegistersInit failed: %d", status);
  }

  if ((status = ClkInit()) != ZX_OK) {
    zxlogf(ERROR, "ClkInit failed: %d", status);
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

  if ((status = SpiInit()) != ZX_OK) {
    zxlogf(ERROR, "SpiInit failed: %d", status);
  }

  if ((status = SelinaInit()) != ZX_OK) {
    zxlogf(ERROR, "SelinaInit failed: %d\n", status);
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

  if ((status = DisplayInit(bootloader_display_id)) != ZX_OK) {
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

  if ((status = EmmcInit()) != ZX_OK) {
    zxlogf(ERROR, "EmmcInit() failed: %d", status);
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

  if ((status = AudioInit()) != ZX_OK) {
    zxlogf(ERROR, "AudioInit failed: %d", status);
  }

  if ((status = SecureMemInit()) != ZX_OK) {
    zxlogf(ERROR, "SecureMemInit failed: %d", status);
  }

  if ((status = BacklightInit()) != ZX_OK) {
    zxlogf(ERROR, "BacklightInit failed: %d", status);
  }

  if ((status = PowerInit()) != ZX_OK) {
    zxlogf(ERROR, "PowerInit failed: %d", status);
  }

  if ((status = NnaInit()) != ZX_OK) {
    zxlogf(ERROR, "NnaInit failed: %d", status);
  }

  if (RamCtlInit() != ZX_OK) {
    zxlogf(ERROR, "RamCtlInit failed");
  }

  if (ThermistorInit() != ZX_OK) {
    zxlogf(ERROR, "ThermistorInit failed");
  }

  if (OtRadioInit() != ZX_OK) {
    zxlogf(ERROR, "OtRadioInit failed");
  }

  // This function includes some non-trivial delays, so lets run this last
  // to avoid slowing down the rest of the boot.
  if ((status = BluetoothInit()) != ZX_OK) {
    zxlogf(ERROR, "BluetoothInit failed: %d", status);
  }

  root_ = inspector_.GetRoot().CreateChild("nelson_board_driver");
  board_rev_property_ = root_.CreateUint("board_build", GetBoardRev());
  board_option_property_ = root_.CreateUint("board_option", GetBoardOption());
  display_id_property_ = root_.CreateUint("display_id", GetDisplayId());

  return ZX_OK;
}

zx_status_t Nelson::Start() {
  int rc = thrd_create_with_name(
      &thread_, [](void* arg) -> int { return reinterpret_cast<Nelson*>(arg)->Thread(); }, this,
      "nelson-start-thread");
  if (rc != thrd_success) {
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

void Nelson::DdkRelease() { delete this; }

zx_status_t Nelson::Create(void* ctx, zx_device_t* parent) {
  iommu_protocol_t iommu;
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

  status = device_get_protocol(parent, ZX_PROTOCOL_IOMMU, &iommu);
  if (status != ZX_OK) {
    return status;
  }

  fbl::AllocChecker ac;
  auto board = fbl::make_unique_checked<Nelson>(&ac, parent, std::move(endpoints->client), &iommu);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }

  status = board->DdkAdd(ddk::DeviceAddArgs("nelson")
                             .set_flags(DEVICE_ADD_NON_BINDABLE)
                             .set_inspect_vmo(board->inspector_.DuplicateVmo()));
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

static zx_driver_ops_t nelson_driver_ops = []() {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = Nelson::Create;
  return ops;
}();

}  // namespace nelson

ZIRCON_DRIVER(nelson, nelson::nelson_driver_ops, "zircon", "0.1");
