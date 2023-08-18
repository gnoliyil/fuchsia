// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>

#include <memory>
#include <vector>

#include <soc/aml-common/aml-registers.h>
#include <soc/aml-s905d2/s905d2-hw.h>

#include "astro.h"
#include "src/devices/lib/fidl-metadata/registers.h"

namespace astro {
namespace fpbus = fuchsia_hardware_platform_bus;

namespace {

enum MmioMetadataIdx {
  RESET_MMIO,

  MMIO_COUNT,
};

static const std::vector<fpbus::Mmio> registers_mmios{
    {{
        .base = S905D2_RESET_BASE,
        .length = S905D2_RESET_LENGTH,
    }},
};

static const fidl_metadata::registers::Register<uint32_t> kRegisters[]{
    {
        .bind_id = aml_registers::REGISTER_USB_PHY_V2_RESET,
        .mmio_id = RESET_MMIO,
        .masks =
            {
                {
                    .value = aml_registers::USB_RESET1_REGISTER_UNKNOWN_1_MASK |
                             aml_registers::USB_RESET1_REGISTER_UNKNOWN_2_MASK,
                    .mmio_offset = S905D2_RESET1_REGISTER,
                },
                {
                    .value = aml_registers::USB_RESET1_LEVEL_MASK,
                    .mmio_offset = S905D2_RESET1_LEVEL,
                },
            },
    },
    {
        .bind_id = aml_registers::REGISTER_MALI_RESET,
        .mmio_id = RESET_MMIO,
        .masks =
            {
                {
                    .value = aml_registers::MALI_RESET0_MASK,
                    .mmio_offset = S905D2_RESET0_MASK,
                },
                {
                    .value = aml_registers::MALI_RESET0_MASK,
                    .mmio_offset = S905D2_RESET0_LEVEL,
                },
                {
                    .value = aml_registers::MALI_RESET2_MASK,
                    .mmio_offset = S905D2_RESET2_MASK,
                },
                {
                    .value = aml_registers::MALI_RESET2_MASK,
                    .mmio_offset = S905D2_RESET2_LEVEL,
                },
            },
    },
};

}  // namespace

zx_status_t Astro::RegistersInit() {
  auto metadata_bytes = fidl_metadata::registers::RegistersMetadataToFidl(kRegisters);
  if (!metadata_bytes.is_ok()) {
    zxlogf(ERROR, "%s: Could not build metadata %s\n", __func__, metadata_bytes.status_string());
    return metadata_bytes.error_value();
  }

  std::vector<fpbus::Metadata> registers_metadata{
      {{
          .type = DEVICE_METADATA_REGISTERS,
          .data = metadata_bytes.value(),
      }},
  };

  fpbus::Node registers_dev;
  registers_dev.name() = "registers";
  registers_dev.vid() = PDEV_VID_GENERIC;
  registers_dev.pid() = PDEV_PID_GENERIC;
  registers_dev.did() = PDEV_DID_REGISTERS;
  registers_dev.mmio() = registers_mmios;
  registers_dev.metadata() = registers_metadata;

  fidl::Arena<> fidl_arena;
  fdf::Arena arena('REGI');
  auto result = pbus_.buffer(arena)->NodeAdd(fidl::ToWire(fidl_arena, registers_dev));
  if (!result.ok()) {
    zxlogf(ERROR, "%s: NodeAdd Registers(registers_dev) request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: NodeAdd Registers(registers_dev) failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

}  // namespace astro
