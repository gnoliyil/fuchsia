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
#include <lib/driver/component/cpp/node_add_args.h>
#include <lib/driver/component/cpp/node_group.h>

#include <bind/fuchsia/amlogic/platform/s905d2/cpp/bind.h>
#include <bind/fuchsia/ams/platform/cpp/bind.h>
#include <bind/fuchsia/cpp/bind.h>
#include <bind/fuchsia/gpio/cpp/bind.h>
#include <bind/fuchsia/pwm/cpp/bind.h>
#include <ddk/metadata/lights.h>
#include <ddktl/metadata/light-sensor.h>
#include <soc/aml-s905d2/s905d2-gpio.h>
#include <soc/aml-s905d2/s905d2-pwm.h>

#include "astro-gpios.h"
#include "astro.h"
#include "src/devices/board/drivers/astro/tcs3400-light-bind.h"
#include "src/devices/bus/lib/platform-bus-composites/platform-bus-composite.h"

namespace astro {
namespace fpbus = fuchsia_hardware_platform_bus;

zx_status_t Astro::LightInit() {
  metadata::LightSensorParams params = {};
  // TODO(kpt): Insert the right parameters here.
  params.integration_time_us = 711'680;
  params.gain = 16;
  params.polling_time_us = 100'000;
  device_metadata_t metadata[] = {
      {
          .type = DEVICE_METADATA_PRIVATE,
          .data = reinterpret_cast<uint8_t*>(&params),
          .length = sizeof(params),
      },
  };
  constexpr zx_device_prop_t props[] = {
      {BIND_PLATFORM_DEV_VID, 0, bind_fuchsia_ams_platform::BIND_PLATFORM_DEV_VID_AMS},
      {BIND_PLATFORM_DEV_PID, 0, bind_fuchsia_ams_platform::BIND_PLATFORM_DEV_PID_TCS3400},
      {BIND_PLATFORM_DEV_DID, 0, bind_fuchsia_ams_platform::BIND_PLATFORM_DEV_DID_LIGHT},
  };

  const composite_device_desc_t comp_desc = {
      .props = props,
      .props_count = std::size(props),
      .fragments = tcs3400_light_fragments,
      .fragments_count = std::size(tcs3400_light_fragments),
      .primary_fragment = "i2c",
      .spawn_colocated = false,
      .metadata_list = metadata,
      .metadata_count = std::size(metadata),
  };

  zx_status_t status = DdkAddComposite("tcs3400-light", &comp_desc);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s(tcs-3400): DdkAddComposite failed: %d", __func__, status);
    return status;
  }

  // Lights
  // Instructions: include fragments in this order
  //     GPIO fragment
  //     BRIGHTNESS capable--include PWM fragment
  //     RGB capable--include RGB fragment
  //   Set GPIO alternative function here!
  using LightName = char[ZX_MAX_NAME_LEN];
  constexpr LightName kLightNames[] = {"AMBER_LED"};
  constexpr LightsConfig kConfigs[] = {
      {.brightness = true, .rgb = false, .init_on = true, .group_id = -1},
  };
  std::vector<fpbus::Metadata> light_metadata{
      {{
          .type = DEVICE_METADATA_NAME,
          .data = std::vector<uint8_t>(
              reinterpret_cast<const uint8_t*>(&kLightNames),
              reinterpret_cast<const uint8_t*>(&kLightNames) + sizeof(kLightNames)),
      }},
      {{
          .type = DEVICE_METADATA_LIGHTS,
          .data =
              std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(&kConfigs),
                                   reinterpret_cast<const uint8_t*>(&kConfigs) + sizeof(kConfigs)),
      }},
  };

  fpbus::Node light_dev;
  light_dev.name() = "gpio-light";
  light_dev.vid() = PDEV_VID_AMLOGIC;
  light_dev.pid() = PDEV_PID_GENERIC;
  light_dev.did() = PDEV_DID_GPIO_LIGHT;
  light_dev.metadata() = light_metadata;

  // Enable the Amber LED so it will be controlled by PWM.
  status = gpio_impl_.SetAltFunction(GPIO_AMBER_LED, 3);  // Set as PWM.
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Configure mute LED GPIO failed %d", __func__, status);
  }

  // GPIO must be set to default out otherwise could cause light to not work
  // on certain reboots.
  status = gpio_impl_.ConfigOut(GPIO_AMBER_LED, 1);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Configure mute LED GPIO on failed %d", __func__, status);
  }

  auto amber_led_gpio_bind_rules = std::vector{
      fdf::MakeAcceptBindRule(bind_fuchsia::PROTOCOL, bind_fuchsia_gpio::BIND_PROTOCOL_DEVICE),
      fdf::MakeAcceptBindRule(bind_fuchsia::GPIO_PIN,
                              bind_fuchsia_amlogic_platform_s905d2::GPIOAO_PIN_ID_PIN_11),
  };

  auto amber_led_gpio_properties = std::vector{
      fdf::MakeProperty(bind_fuchsia::PROTOCOL, bind_fuchsia_gpio::BIND_PROTOCOL_DEVICE),
      fdf::MakeProperty(bind_fuchsia_gpio::FUNCTION, bind_fuchsia_gpio::FUNCTION_GPIO_AMBER_LED),
  };

  auto amber_led_pwm_bind_rules = std::vector{
      fdf::MakeAcceptBindRule(bind_fuchsia::PROTOCOL, bind_fuchsia_pwm::BIND_PROTOCOL_PWM),
      fdf::MakeAcceptBindRule(bind_fuchsia::PWM_ID,
                              bind_fuchsia_amlogic_platform_s905d2::BIND_PWM_ID_PWM_AO_A),
  };

  auto amber_led_pwm_properties = std::vector{
      fdf::MakeProperty(bind_fuchsia::PROTOCOL, bind_fuchsia_pwm::BIND_PROTOCOL_PWM),
      fdf::MakeProperty(bind_fuchsia_pwm::PWM_ID_FUNCTION,
                        bind_fuchsia_pwm::PWM_ID_FUNCTION_AMBER_LED),
  };

  auto nodes = std::vector{
      fuchsia_driver_framework::NodeRepresentation{{
          .bind_rules = amber_led_gpio_bind_rules,
          .properties = amber_led_gpio_properties,
      }},
      fuchsia_driver_framework::NodeRepresentation{{
          .bind_rules = amber_led_pwm_bind_rules,
          .properties = amber_led_pwm_properties,
      }},
  };

  fidl::Arena<> fidl_arena;
  fdf::Arena arena('LIGH');

  auto node_group = fuchsia_driver_framework::NodeGroup{{.name = "light_dev", .nodes = nodes}};

  auto result = pbus_.buffer(arena)->AddNodeGroup(fidl::ToWire(fidl_arena, light_dev),
                                                  fidl::ToWire(fidl_arena, node_group));

  if (!result.ok()) {
    zxlogf(ERROR, "%s: AddNodeGroup Light(light_dev) request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: AddNodeGroup Light(light_dev) failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

}  // namespace astro
