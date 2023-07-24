// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>

#include <bind/fuchsia/cpp/bind.h>
#include <bind/fuchsia/hardware/gpio/cpp/bind.h>
#include <ddk/metadata/buttons.h>
#include <ddktl/device.h>

#include "src/devices/board/drivers/as370/as370-gpio.h"
#include "src/devices/board/drivers/as370/as370.h"

namespace board_as370 {

constexpr buttons_button_config_t mute_button{BUTTONS_TYPE_DIRECT, BUTTONS_ID_MIC_MUTE, 0, 0, 0};

constexpr buttons_gpio_config_t mute_gpio{
    BUTTONS_GPIO_TYPE_INTERRUPT,
    0,
    {.interrupt = {GPIO_NO_PULL}},
};

constexpr device_metadata_t available_buttons_metadata[] = {
    {
        .type = DEVICE_METADATA_BUTTONS_BUTTONS,
        .data = &mute_button,
        .length = sizeof(mute_button),
    },
    {
        .type = DEVICE_METADATA_BUTTONS_GPIOS,
        .data = &mute_gpio,
        .length = sizeof(mute_gpio),
    }};

zx_status_t As370::ButtonsInit() {
  const ddk::BindRule kMicPrivacyRules[] = {
      ddk::MakeAcceptBindRule(bind_fuchsia::FIDL_PROTOCOL,
                              bind_fuchsia_hardware_gpio::BIND_FIDL_PROTOCOL_SERVICE),
      ddk::MakeAcceptBindRule(bind_fuchsia::GPIO_PIN, static_cast<uint32_t>(GPIO_MIC_MUTE_STATUS))};
  const device_bind_prop_t kMicPrivacyProps[] = {
      ddk::MakeProperty(bind_fuchsia::FIDL_PROTOCOL,
                        bind_fuchsia_hardware_gpio::BIND_FIDL_PROTOCOL_SERVICE),
      ddk::MakeProperty(bind_fuchsia_hardware_gpio::FUNCTION,
                        bind_fuchsia_hardware_gpio::FUNCTION_MIC_MUTE),
  };

  const ddk::CompositeNodeSpec buttonComposite =
      ddk::CompositeNodeSpec(kMicPrivacyRules, kMicPrivacyProps)
          .set_metadata(available_buttons_metadata);

  zx_status_t status = DdkAddCompositeNodeSpec("as370-buttons", buttonComposite);

  if (status != ZX_OK) {
    zxlogf(ERROR, "AddCompositeNodeSpec failed: %s", zx_status_get_string(status));
  }

  return status;
}

}  // namespace board_as370
