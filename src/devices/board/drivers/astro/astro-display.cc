// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <lib/device-protocol/display-panel.h>
#include <lib/driver/component/cpp/composite_node_spec.h>
#include <lib/driver/component/cpp/node_add_args.h>

#include <bind/fuchsia/amlogic/platform/cpp/bind.h>
#include <bind/fuchsia/amlogic/platform/s905d2/cpp/bind.h>
#include <bind/fuchsia/cpp/bind.h>
#include <bind/fuchsia/gpio/cpp/bind.h>
#include <bind/fuchsia/hardware/dsi/cpp/bind.h>
#include <bind/fuchsia/sysmem/cpp/bind.h>
#include <ddk/metadata/display.h>
#include <soc/aml-s905d2/s905d2-gpio.h>
#include <soc/aml-s905d2/s905d2-hw.h>

#include "astro-gpios.h"
#include "astro.h"
#include "src/devices/bus/lib/platform-bus-composites/platform-bus-composite.h"

namespace astro {
namespace fpbus = fuchsia_hardware_platform_bus;

static const std::vector<fpbus::Mmio> display_mmios{
    {{
        // VBUS/VPU
        .base = S905D2_VPU_BASE,
        .length = S905D2_VPU_LENGTH,
    }},
    {{
        // TOP DSI Host Controller (Amlogic Specific)
        .base = S905D2_MIPI_TOP_DSI_BASE,
        .length = S905D2_MIPI_TOP_DSI_LENGTH,
    }},
    {{
        // DSI PHY
        .base = S905D2_DSI_PHY_BASE,
        .length = S905D2_DSI_PHY_LENGTH,
    }},
    {{
        // HHI
        .base = S905D2_HIU_BASE,
        .length = S905D2_HIU_LENGTH,
    }},
    {{
        // AOBUS
        .base = S905D2_AOBUS_BASE,
        .length = S905D2_AOBUS_LENGTH,
    }},
    {{
        // CBUS
        .base = S905D2_CBUS_BASE,
        .length = S905D2_CBUS_LENGTH,
    }},
};

static const std::vector<fpbus::Irq> display_irqs{
    {{
        .irq = S905D2_VIU1_VSYNC_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    }},
    {{
        .irq = S905D2_RDMA_DONE,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    }},
    {{
        .irq = S905D2_VID1_WR,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    }},
};

static std::vector<fpbus::Metadata> display_panel_metadata{
    {{
        .type = DEVICE_METADATA_DISPLAY_CONFIG,
        // No metadata for this item.
    }},
};

static const std::vector<fpbus::Bti> display_btis{
    {{
        .iommu_index = 0,
        .bti_id = BTI_DISPLAY,
    }},
};

zx_status_t Astro::DisplayInit() {
  display_panel_t display_panel_info[] = {
      {
          .width = 600,
          .height = 1024,
      },
  };

  uint8_t pt;
  gpio_impl_.ConfigIn(GPIO_PANEL_DETECT, GPIO_NO_PULL);
  gpio_impl_.Read(GPIO_PANEL_DETECT, &pt);
  if (pt) {
    display_panel_info[0].panel_type = PANEL_P070ACB_FT;
  } else {
    display_panel_info[0].panel_type = PANEL_TV070WSM_FT;
  }
  display_panel_metadata[0].data() =
      std::vector(reinterpret_cast<uint8_t*>(&display_panel_info),
                  reinterpret_cast<uint8_t*>(&display_panel_info) + sizeof(display_panel_info));

  const fpbus::Node display_dev = []() {
    fpbus::Node dev = {};
    dev.name() = "display";
    dev.vid() = PDEV_VID_AMLOGIC;
    dev.pid() = PDEV_PID_AMLOGIC_S905D2;
    dev.did() = PDEV_DID_AMLOGIC_DISPLAY;
    dev.metadata() = display_panel_metadata;
    dev.mmio() = display_mmios;
    dev.irq() = display_irqs;
    dev.bti() = display_btis;
    return dev;
  }();

  auto dsi_bind_rules = std::vector{
      fdf::MakeAcceptBindRule(bind_fuchsia::PROTOCOL,
                              bind_fuchsia_hardware_dsi::BIND_PROTOCOL_IMPL),
  };

  auto dsi_properties = std::vector{
      fdf::MakeProperty(bind_fuchsia::PROTOCOL, bind_fuchsia_hardware_dsi::BIND_PROTOCOL_IMPL),
  };

  auto gpio_bind_rules = std::vector{
      fdf::MakeAcceptBindRule(bind_fuchsia::PROTOCOL, bind_fuchsia_gpio::BIND_PROTOCOL_DEVICE),
      fdf::MakeAcceptBindRule(bind_fuchsia::GPIO_PIN,
                              bind_fuchsia_amlogic_platform_s905d2::GPIOH_PIN_ID_PIN_6),
  };

  auto gpio_properties = std::vector{
      fdf::MakeProperty(bind_fuchsia::PROTOCOL, bind_fuchsia_gpio::BIND_PROTOCOL_DEVICE),
      fdf::MakeProperty(bind_fuchsia_gpio::FUNCTION, bind_fuchsia_gpio::FUNCTION_LCD_RESET),
  };

  auto sysmem_bind_rules = std::vector{
      fdf::MakeAcceptBindRule(bind_fuchsia::PROTOCOL, bind_fuchsia_sysmem::BIND_PROTOCOL_DEVICE),
  };

  auto sysmem_properties = std::vector{
      fdf::MakeProperty(bind_fuchsia::PROTOCOL, bind_fuchsia_sysmem::BIND_PROTOCOL_DEVICE),
  };

  auto canvas_bind_rules = std::vector{
      fdf::MakeAcceptBindRule(bind_fuchsia::PROTOCOL,
                              bind_fuchsia_amlogic_platform::BIND_PROTOCOL_CANVAS),
  };

  auto canvas_properties = std::vector{
      fdf::MakeProperty(bind_fuchsia::PROTOCOL,
                        bind_fuchsia_amlogic_platform::BIND_PROTOCOL_CANVAS),
  };

  auto parents = std::vector{
      fuchsia_driver_framework::ParentSpec{{
          .bind_rules = dsi_bind_rules,
          .properties = dsi_properties,
      }},
      fuchsia_driver_framework::ParentSpec{{
          .bind_rules = gpio_bind_rules,
          .properties = gpio_properties,
      }},
      fuchsia_driver_framework::ParentSpec{{
          .bind_rules = sysmem_bind_rules,
          .properties = sysmem_properties,
      }},
      fuchsia_driver_framework::ParentSpec{{
          .bind_rules = canvas_bind_rules,
          .properties = canvas_properties,
      }},
  };

  auto node_group =
      fuchsia_driver_framework::CompositeNodeSpec{{.name = "display", .parents = parents}};

  // TODO(payamm): Change from "dsi" to nullptr to separate DSI and Display into two different
  // driver hosts once support for it lands.
  fidl::Arena<> fidl_arena;
  fdf::Arena arena('DISP');
  auto result = pbus_.buffer(arena)->AddCompositeNodeSpec(fidl::ToWire(fidl_arena, display_dev),
                                                          fidl::ToWire(fidl_arena, node_group));
  if (!result.ok()) {
    zxlogf(ERROR, "%s: AddCompositeSpec Display(display_dev) request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: AddCompositeSpec Display(display_dev) failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

}  // namespace astro
