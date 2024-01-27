// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>

#include <ddk/metadata/gpio.h>
#include <soc/aml-a5/a5-hw.h>

#include "buckeye-gpios.h"
#include "buckeye.h"
#include "src/devices/bus/lib/platform-bus-composites/platform-bus-composite.h"

namespace buckeye {
namespace fpbus = fuchsia_hardware_platform_bus;

static const std::vector<fpbus::Mmio> gpio_mmios{
    {{
        .base = A5_GPIO_BASE,
        .length = A5_GPIO_LENGTH,
    }},
    {{
        .base = A5_GPIO_BASE,
        .length = A5_GPIO_LENGTH,
    }},
    {{
        .base = A5_GPIO_INTERRUPT_BASE,
        .length = A5_GPIO_INTERRUPT_LENGTH,
    }},
};

static const std::vector<fpbus::Irq> gpio_irqs{
    {{
        .irq = A5_GPIO_IRQ_0,
        .mode = ZX_INTERRUPT_MODE_DEFAULT,
    }},
    {{
        .irq = A5_GPIO_IRQ_1,
        .mode = ZX_INTERRUPT_MODE_DEFAULT,
    }},
    {{
        .irq = A5_GPIO_IRQ_2,
        .mode = ZX_INTERRUPT_MODE_DEFAULT,
    }},
    {{
        .irq = A5_GPIO_IRQ_3,
        .mode = ZX_INTERRUPT_MODE_DEFAULT,
    }},
    {{
        .irq = A5_GPIO_IRQ_4,
        .mode = ZX_INTERRUPT_MODE_DEFAULT,
    }},
    {{
        .irq = A5_GPIO_IRQ_5,
        .mode = ZX_INTERRUPT_MODE_DEFAULT,
    }},
    {{
        .irq = A5_GPIO_IRQ_6,
        .mode = ZX_INTERRUPT_MODE_DEFAULT,
    }},
    {{
        .irq = A5_GPIO_IRQ_7,
        .mode = ZX_INTERRUPT_MODE_DEFAULT,
    }},
    {{
        .irq = A5_GPIO_IRQ_8,
        .mode = ZX_INTERRUPT_MODE_DEFAULT,
    }},
    {{
        .irq = A5_GPIO_IRQ_9,
        .mode = ZX_INTERRUPT_MODE_DEFAULT,
    }},
    {{
        .irq = A5_GPIO_IRQ_10,
        .mode = ZX_INTERRUPT_MODE_DEFAULT,
    }},
    {{
        .irq = A5_GPIO_IRQ_11,
        .mode = ZX_INTERRUPT_MODE_DEFAULT,
    }},
};

// GPIOs to expose from generic GPIO driver.
static const gpio_pin_t gpio_pins[] = {
    DECL_GPIO_PIN(A5_GPIOB(12)),         DECL_GPIO_PIN(A5_GPIOB(13)),
    DECL_GPIO_PIN(GPIO_EMMC_RST_L),      DECL_GPIO_PIN(GPIO_PANEL_SPI_SS0),
    DECL_GPIO_PIN(GPIO_SOC_WIFI_32k768), DECL_GPIO_PIN(GPIO_SOC_BT_REG_ON),
    DECL_GPIO_PIN(GPIO_WL_PDN),          DECL_GPIO_PIN(GPIO_SOC_AMP_EN_H),
    DECL_GPIO_PIN(GPIO_MIC_MUTE),        DECL_GPIO_PIN(GPIO_VOL_UP_L),
    DECL_GPIO_PIN(GPIO_VOL_DN_L),        DECL_GPIO_PIN(GPIO_UNKNOWN_SPI_SS0),
    DECL_GPIO_PIN(GPIO_SNS_SPI1_PD0N),   DECL_GPIO_PIN(GPIO_RF_SPI_RESETN),
    DECL_GPIO_PIN(GPIO_AMP_EN),          DECL_GPIO_PIN(GPIO_UWB_SOC_INT),
    DECL_GPIO_PIN(GPIO_UWB_SPI_WAKEUP),
};

static const std::vector<fpbus::Metadata> gpio_metadata{
    {{
        .type = DEVICE_METADATA_GPIO_PINS,
        .data =
            std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&gpio_pins),
                                 reinterpret_cast<const uint8_t*>(&gpio_pins) + sizeof(gpio_pins)),
    }},
};

static const fpbus::Node gpio_dev = []() {
  fpbus::Node dev = {};
  dev.name() = "gpio";
  dev.vid() = PDEV_VID_AMLOGIC;
  dev.pid() = PDEV_PID_AMLOGIC_A5;
  dev.did() = PDEV_DID_AMLOGIC_GPIO;
  dev.mmio() = gpio_mmios;
  dev.irq() = gpio_irqs;
  dev.metadata() = gpio_metadata;
  return dev;
}();

zx_status_t Buckeye::GpioInit() {
  fidl::Arena<> fidl_arena;
  fdf::Arena arena('GPIO');
  auto result = pbus_.buffer(arena)->ProtocolNodeAdd(ZX_PROTOCOL_GPIO_IMPL,
                                                     fidl::ToWire(fidl_arena, gpio_dev));
  if (!result.ok()) {
    zxlogf(ERROR, "%s: ProtocolNodeAdd Gpio(gpio_dev) request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: ProtocolNodeAdd Gpio(gpio_dev) failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  gpio_impl_ = ddk::GpioImplProtocolClient(parent());
  if (!gpio_impl_.is_valid()) {
    zxlogf(ERROR, "%s: device_get_protocol failed", __func__);
    return ZX_ERR_INTERNAL;
  }

  return ZX_OK;
}

}  // namespace buckeye
