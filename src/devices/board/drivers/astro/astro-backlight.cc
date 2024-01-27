// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/driver/component/cpp/node_add_args.h>
#include <lib/driver/component/cpp/node_group.h>
#include <zircon/compiler.h>

#include <bind/fuchsia/cpp/bind.h>
#include <bind/fuchsia/i2c/cpp/bind.h>
#include <soc/aml-s905d2/s905d2-hw.h>

#include "astro.h"
#include "src/devices/bus/lib/platform-bus-composites/platform-bus-composite.h"
#include "src/ui/backlight/drivers/ti-lp8556/ti-lp8556Metadata.h"

namespace astro {
namespace fpbus = fuchsia_hardware_platform_bus;

static const std::vector<fpbus::Mmio> backlight_mmios{
    {{
        .base = S905D2_GPIO_AO_BASE,
        .length = S905D2_GPIO_AO_LENGTH,
    }},
};

constexpr double kMaxBrightnessInNits = 400.0;

TiLp8556Metadata kDeviceMetadata = {
    .panel_id = 0,
    .allow_set_current_scale = false,
    .registers =
        {
            // Registers
            0x01, 0x85,  // Device Control
                         // EPROM
            0xa2, 0x30,  // CFG2
            0xa3, 0x32,  // CFG3
            0xa5, 0x54,  // CFG5
            0xa7, 0xf4,  // CFG7
            0xa9, 0x60,  // CFG9
            0xae, 0x09,  // CFGE
        },
    .register_count = 14,
};

static const std::vector<fpbus::Metadata> backlight_metadata{
    {{
        .type = DEVICE_METADATA_BACKLIGHT_MAX_BRIGHTNESS_NITS,
        .data = std::vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(&kMaxBrightnessInNits),
            reinterpret_cast<const uint8_t*>(&kMaxBrightnessInNits) + sizeof(kMaxBrightnessInNits)),
    }},
    {{
        .type = DEVICE_METADATA_PRIVATE,
        .data = std::vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(&kDeviceMetadata),
            reinterpret_cast<const uint8_t*>(&kDeviceMetadata) + sizeof(kDeviceMetadata)),
    }},
};

static const fpbus::Node backlight_dev = []() {
  fpbus::Node dev = {};
  dev.name() = "backlight";
  dev.vid() = PDEV_VID_TI;
  dev.pid() = PDEV_PID_TI_LP8556;
  dev.did() = PDEV_DID_TI_BACKLIGHT;
  dev.metadata() = backlight_metadata;
  dev.mmio() = backlight_mmios;
  return dev;
}();

zx_status_t Astro::BacklightInit() {
  fidl::Arena<> fidl_arena;
  fdf::Arena arena('BACK');

  auto bind_rules = std::vector{
      fdf::MakeAcceptBindRule(bind_fuchsia::FIDL_PROTOCOL,
                              bind_fuchsia_i2c::BIND_FIDL_PROTOCOL_DEVICE),
      fdf::MakeAcceptBindRule(bind_fuchsia::I2C_BUS_ID, bind_fuchsia_i2c::BIND_I2C_BUS_ID_I2C_3),
      fdf::MakeAcceptBindRule(bind_fuchsia::I2C_ADDRESS,
                              bind_fuchsia_i2c::BIND_I2C_ADDRESS_BACKLIGHT),
  };

  auto properties = std::vector{
      fdf::MakeProperty(bind_fuchsia::FIDL_PROTOCOL, bind_fuchsia_i2c::BIND_FIDL_PROTOCOL_DEVICE),
  };

  auto nodes = std::vector{
      fuchsia_driver_framework::NodeRepresentation{{
          .bind_rules = bind_rules,
          .properties = properties,
      }},
  };

  auto node_group = fuchsia_driver_framework::NodeGroup{{.name = "backlight", .nodes = nodes}};

  auto result = pbus_.buffer(arena)->AddNodeGroup(fidl::ToWire(fidl_arena, backlight_dev),
                                                  fidl::ToWire(fidl_arena, node_group));

  if (!result.ok()) {
    zxlogf(ERROR, "%s: AddNodeGroup Backlight(backlight_dev) request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: AddNodeGroup Backlight(backlight_dev) failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

}  // namespace astro
