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
#include <soc/aml-a5/a5-gpio.h>
#include <soc/aml-a5/a5-hw.h>

#include "av400.h"
#include "src/devices/bus/lib/platform-bus-composites/platform-bus-composite.h"

namespace av400 {
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
    DECL_GPIO_PIN(A5_GPIOB(12)), DECL_GPIO_PIN(A5_GPIOB(13)),    DECL_GPIO_PIN(A5_GPIOB(9)),
    DECL_GPIO_PIN(A5_GPIOT(10)), DECL_GPIO_PIN(A5_GPIOX(16)),    DECL_GPIO_PIN(A5_GPIOX(17)),
    DECL_GPIO_PIN(A5_GPIOX(6)),  DECL_GPIO_PIN(A5_ETH_MAC_INTR), DECL_GPIO_PIN(A5_GPIOD(9)),
    DECL_GPIO_PIN(A5_GPIOD(3)),
};

zx_status_t Av400::GpioInit() {
  fuchsia_hardware_gpio_init::wire::GpioInitMetadata metadata;
  metadata.steps = fidl::VectorView<fuchsia_hardware_gpio_init::wire::GpioInitStep>::FromExternal(
      gpio_init_steps_.data(), gpio_init_steps_.size());

  fit::result encoded = fidl::Persist(metadata);
  if (!encoded.is_ok()) {
    zxlogf(ERROR, "Failed to encode GPIO init metadata: %s",
           encoded.error_value().FormatDescription().c_str());
    return encoded.error_value().status();
  }

  static const std::vector<fpbus::Metadata> gpio_metadata{
      {{
          .type = DEVICE_METADATA_GPIO_PINS,
          .data = std::vector<uint8_t>(
              reinterpret_cast<const uint8_t*>(&gpio_pins),
              reinterpret_cast<const uint8_t*>(&gpio_pins) + sizeof(gpio_pins)),
      }},
      {{
          .type = DEVICE_METADATA_GPIO_INIT_STEPS,
          .data = std::move(encoded.value()),
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

  fidl::Arena<> fidl_arena;
  fdf::Arena arena('GPIO');
  auto result = pbus_.buffer(arena)->NodeAdd(fidl::ToWire(fidl_arena, gpio_dev));
  if (!result.ok()) {
    zxlogf(ERROR, "NodeAdd Gpio(gpio_dev) request failed: %s", result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "NodeAdd Gpio(gpio_dev) failed: %s", zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

}  // namespace av400
