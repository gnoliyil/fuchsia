// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/clock/c/banjo.h>
#include <fuchsia/hardware/gpio/c/banjo.h>
#include <fuchsia/hardware/platform/device/c/banjo.h>
#include <fuchsia/hardware/power/c/banjo.h>
#include <fuchsia/hardware/pwm/c/banjo.h>
#include <fuchsia/hardware/spi/c/banjo.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>
#include <stdlib.h>
#include <string.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>
#include <zircon/types.h>

#include "src/devices/bus/drivers/platform/test/test-metadata.h"

#define DRIVER_NAME "test-composite"
#define GOLDFISH_TEST_HEAP (0x100000000000fffful)

enum Fragments_1 {
  FRAGMENT_PDEV_1, /* Should be 1st fragment */
  FRAGMENT_GPIO_1,
  FRAGMENT_CLOCK_1,
  FRAGMENT_I2C_1,
  FRAGMENT_POWER_1,
  FRAGMENT_CHILD4_1,
  FRAGMENT_COUNT_1,
};

enum Fragments_2 {
  FRAGMENT_PDEV_2, /* Should be 1st fragment */
  FRAGMENT_CLOCK_2,
  FRAGMENT_POWER_2,
  FRAGMENT_CHILD4_2,
  FRAGMENT_SPI_2,
  FRAGMENT_PWM_2,
  FRAGMENT_COUNT_2,
};

enum Fragments_GoldfishControl {
  FRAGMENT_PDEV_GOLDFISH_CTRL, /* Should be 1st fragment */
  FRAGMENT_COUNT_GOLDFISH_CTRL,
};

typedef struct {
  zx_device_t* zxdev;
} test_t;

typedef struct {
  uint32_t magic;
} mode_config_magic_t;

typedef struct {
  uint32_t mode;
  union {
    mode_config_magic_t magic;
  };
} mode_config_t;

static void test_release(void* ctx) { free(ctx); }

static zx_protocol_device_t test_device_protocol = {
    .version = DEVICE_OPS_VERSION,
    .release = test_release,
};

static zx_status_t test_gpio(gpio_protocol_t* gpio) {
  zx_status_t status;
  uint8_t value;

  if ((status = gpio_config_out(gpio, 0)) != ZX_OK) {
    return status;
  }
  if ((status = gpio_read(gpio, &value)) != ZX_OK || value != 0) {
    return status;
  }
  if ((status = gpio_write(gpio, 1)) != ZX_OK) {
    return status;
  }
  if ((status = gpio_read(gpio, &value)) != ZX_OK || value != 1) {
    return status;
  }

  return ZX_OK;
}

static zx_status_t test_clock(clock_protocol_t* clock) {
  zx_status_t status;
  const uint64_t kOneMegahertz = 1000000;
  const uint32_t kBad = 0xDEADBEEF;
  uint64_t out_rate = 0;

  if ((status = clock_enable(clock)) != ZX_OK) {
    return status;
  }
  if ((status = clock_disable(clock)) != ZX_OK) {
    return status;
  }

  bool is_enabled = false;
  if ((status = clock_is_enabled(clock, &is_enabled)) != ZX_OK) {
    return status;
  }

  if ((status = clock_set_rate(clock, kOneMegahertz)) != ZX_OK) {
    return status;
  }

  if ((status = clock_query_supported_rate(clock, kOneMegahertz, &out_rate)) != ZX_OK) {
    return status;
  }

  if ((status = clock_get_rate(clock, &out_rate)) != ZX_OK) {
    return status;
  }

  if ((status = clock_set_input(clock, 0)) != ZX_OK) {
    return status;
  }

  uint32_t num_inputs = kBad;
  if ((status = clock_get_num_inputs(clock, &num_inputs)) != ZX_OK) {
    return status;
  }

  uint32_t current_input = kBad;
  if ((status = clock_get_input(clock, &current_input)) != ZX_OK) {
    return status;
  }

  // Make sure that the input value was actually set.
  if (num_inputs == kBad || current_input == kBad) {
    // The above calls returned ZX_OK but the out value was unchanged?
    return ZX_ERR_BAD_STATE;
  }

  return ZX_OK;
}

static zx_status_t test_spi(spi_protocol_t* spi) {
  uint8_t txbuf[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  uint8_t rxbuf[sizeof txbuf];

  // tx should just succeed
  zx_status_t status = spi_transmit(spi, txbuf, sizeof txbuf);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: spi_transmit failed %d", DRIVER_NAME, status);
    return status;
  }

  // rx should return pattern
  size_t actual;
  memset(rxbuf, 0, sizeof rxbuf);
  status = spi_receive(spi, sizeof rxbuf, rxbuf, sizeof rxbuf, &actual);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: spi_receive failed %d (", DRIVER_NAME, status);
    return status;
  }

  if (actual != sizeof rxbuf) {
    zxlogf(ERROR, "%s: spi_receive returned incomplete %zu/%zu (", DRIVER_NAME, actual,
           sizeof rxbuf);
    return ZX_ERR_INTERNAL;
  }

  for (size_t i = 0; i < actual; i++) {
    if (rxbuf[i] != (i & 0xff)) {
      zxlogf(ERROR, "%s: spi_receive returned bad pattern rxbuf[%zu] = 0x%02x, should be 0x%02x(",
             DRIVER_NAME, i, rxbuf[i], (uint8_t)(i & 0xff));
      return ZX_ERR_INTERNAL;
    }
  }

  // exchange copies input
  memset(rxbuf, 0, sizeof rxbuf);
  status = spi_exchange(spi, txbuf, sizeof txbuf, rxbuf, sizeof rxbuf, &actual);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: spi_exchange failed %d (", DRIVER_NAME, status);
    return status;
  }

  if (actual != sizeof rxbuf) {
    zxlogf(ERROR, "%s: spi_exchange returned incomplete %zu/%zu (", DRIVER_NAME, actual,
           sizeof rxbuf);
    return ZX_ERR_INTERNAL;
  }

  for (size_t i = 0; i < actual; i++) {
    if (rxbuf[i] != txbuf[i]) {
      zxlogf(ERROR, "%s: spi_exchange returned bad result rxbuf[%zu] = 0x%02x, should be 0x%02x(",
             DRIVER_NAME, i, rxbuf[i], txbuf[i]);
      return ZX_ERR_INTERNAL;
    }
  }

  // SPI FIDL communication should work, but there is no way to synchronize this with the
  // enumeration test that also attempts to open the device. FIDL communication is the
  // responsibility of the SPI core driver, so checking Banjo only is good enough here.
  return ZX_OK;
}

static zx_status_t test_power(power_protocol_t* power) {
  zx_status_t status;
  uint32_t value;

  uint32_t min_voltage = 0, max_voltage = 0;
  if ((status = power_get_supported_voltage_range(power, &min_voltage, &max_voltage)) != ZX_OK) {
    // Not a fixed power domain.
    zxlogf(ERROR, "Unable to get supported voltage from power domain");
    return status;
  }

  // These are the limits in the test power-impl driver
  if (min_voltage != 10 && max_voltage != 1000) {
    zxlogf(ERROR, "%s: Got wrong supported voltages", __func__);
    return ZX_ERR_INTERNAL;
  }

  if ((status = power_register_power_domain(power, 50, 800)) != ZX_OK) {
    zxlogf(ERROR, "Unable to register for power domain");
    return status;
  }

  power_domain_status_t out_status;
  if ((status = power_get_power_domain_status(power, &out_status) != ZX_OK)) {
    zxlogf(ERROR, "Unable to power domain status");
    return status;
  }

  if (out_status != POWER_DOMAIN_STATUS_ENABLED) {
    zxlogf(ERROR, "power domain should have been enabled after registration");
    return ZX_ERR_INTERNAL;
  }

  uint32_t out_actual_voltage = 0;
  if ((status = power_request_voltage(power, 30, &out_actual_voltage)) != ZX_OK) {
    zxlogf(ERROR, "Unable to request a particular voltage. Got out_voltage as %d",
           out_actual_voltage);
    return status;
  }

  // We registered to the domain with voltage range 50-800. 30 will be rounded to 50.
  if (out_actual_voltage != 50) {
    zxlogf(ERROR, "Generic power driver failed to set correct voltage. Got out_voltage as %d",
           out_actual_voltage);
    return ZX_ERR_INTERNAL;
  }

  // Write a register and read it back
  if ((status = power_write_pmic_ctrl_reg(power, 0x1234, 6)) != ZX_OK) {
    return status;
  }
  if ((status = power_read_pmic_ctrl_reg(power, 0x1234, &value)) != ZX_OK || value != 6) {
    return status;
  }

  if ((status = power_unregister_power_domain(power) != ZX_OK)) {
    zxlogf(ERROR, "Unable to unregister for power domain");
    return status;
  }

  return ZX_OK;
}

static zx_status_t test_pwm(pwm_protocol_t* pwm) {
  zx_status_t status = ZX_OK;
  mode_config_t mode_cfg = {.mode = 0, .magic = {12345}};
  pwm_config_t cfg = {
      false, 1000, 39.0, (uint8_t*)&mode_cfg, sizeof(mode_cfg),
  };
  if ((status = pwm_set_config(pwm, &cfg)) != ZX_OK) {
    return status;
  }
  mode_config_t out_mode_cfg = {.mode = 0, .magic = {0}};
  pwm_config_t out_config = {
      false, 0, 0.0, (uint8_t*)&out_mode_cfg, sizeof(out_mode_cfg),
  };
  pwm_get_config(pwm, &out_config);
  if (cfg.polarity != out_config.polarity || cfg.period_ns != out_config.period_ns ||
      cfg.duty_cycle != out_config.duty_cycle ||
      cfg.mode_config_size != out_config.mode_config_size ||
      memcmp(cfg.mode_config_buffer, out_config.mode_config_buffer, cfg.mode_config_size)) {
    return ZX_ERR_INTERNAL;
  }
  if ((status = pwm_enable(pwm)) != ZX_OK) {
    return status;
  }
  if ((status = pwm_disable(pwm)) != ZX_OK) {
    return status;
  }
  return ZX_OK;
}

static zx_status_t test_bind(void* ctx, zx_device_t* parent) {
  zx_status_t status;

  zxlogf(INFO, "test_bind: %s ", DRIVER_NAME);

  uint32_t count = device_get_fragment_count(parent);
  size_t actual;
  composite_device_fragment_t fragments[FRAGMENT_COUNT_2] = {};
  device_get_fragments(parent, fragments, count, &actual);
  if (count != actual) {
    zxlogf(ERROR, "%s: got the wrong number of fragments (%u, %zu)", DRIVER_NAME, count, actual);
    return ZX_ERR_BAD_STATE;
  }

  pdev_protocol_t pdev;
  if (strncmp(fragments[FRAGMENT_PDEV_1].name, "pdev", 32)) {
    zxlogf(ERROR, "%s: Unexpected name: %s", DRIVER_NAME, fragments[FRAGMENT_PDEV_1].name);
    return ZX_ERR_INTERNAL;
  }

  status = device_get_protocol(fragments[FRAGMENT_PDEV_1].device, ZX_PROTOCOL_PDEV, &pdev);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: could not get protocol ZX_PROTOCOL_PDEV", DRIVER_NAME);
    return status;
  }

  size_t size;
  composite_test_metadata metadata;
  status =
      device_get_metadata_size(fragments[FRAGMENT_PDEV_1].device, DEVICE_METADATA_PRIVATE, &size);
  if (status != ZX_OK || size != sizeof(composite_test_metadata)) {
    zxlogf(ERROR, "%s: device_get_metadata_size failed: %d", DRIVER_NAME, status);
    return ZX_ERR_INTERNAL;
  }
  status = device_get_metadata(fragments[FRAGMENT_PDEV_1].device, DEVICE_METADATA_PRIVATE,
                               &metadata, sizeof(metadata), &size);
  if (status != ZX_OK || size != sizeof(composite_test_metadata)) {
    zxlogf(ERROR, "%s: device_get_metadata failed: %d", DRIVER_NAME, status);
    return ZX_ERR_INTERNAL;
  }

  if (metadata.metadata_value != 12345) {
    zxlogf(ERROR, "%s: device_get_metadata failed: %d", DRIVER_NAME, status);
    return ZX_ERR_INTERNAL;
  }

  clock_protocol_t clock;
  power_protocol_t power;
  clock_protocol_t child4;
  gpio_protocol_t gpio;
  spi_protocol_t spi;
  pwm_protocol_t pwm;

  if (metadata.composite_device_id == PDEV_DID_TEST_COMPOSITE_1) {
    if (count != FRAGMENT_COUNT_1) {
      zxlogf(ERROR, "%s: got the wrong number of fragments (%u, %d)", DRIVER_NAME, count,
             FRAGMENT_COUNT_1);
      return ZX_ERR_BAD_STATE;
    }

    if (strncmp(fragments[FRAGMENT_CLOCK_1].name, "clock", 32)) {
      zxlogf(ERROR, "%s: Unexpected name: %s", DRIVER_NAME, fragments[FRAGMENT_CLOCK_1].name);
      return ZX_ERR_INTERNAL;
    }
    status = device_get_protocol(fragments[FRAGMENT_CLOCK_1].device, ZX_PROTOCOL_CLOCK, &clock);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: could not get protocol ZX_PROTOCOL_CLOCK", DRIVER_NAME);
      return status;
    }
    if (strncmp(fragments[FRAGMENT_POWER_1].name, "power", 32)) {
      zxlogf(ERROR, "%s: Unexpected name: %s", DRIVER_NAME, fragments[FRAGMENT_POWER_1].name);
      return ZX_ERR_INTERNAL;
    }
    status = device_get_protocol(fragments[FRAGMENT_POWER_1].device, ZX_PROTOCOL_POWER, &power);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: could not get protocol ZX_PROTOCOL_POWER", DRIVER_NAME);
      return status;
    }
    if (strncmp(fragments[FRAGMENT_CHILD4_1].name, "child4", 32)) {
      zxlogf(ERROR, "%s: Unexpected name: %s", DRIVER_NAME, fragments[FRAGMENT_CHILD4_1].name);
      return ZX_ERR_INTERNAL;
    }
    status = device_get_protocol(fragments[FRAGMENT_CHILD4_1].device, ZX_PROTOCOL_CLOCK, &child4);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: could not get protocol from child4", DRIVER_NAME);
      return status;
    }
    if (strncmp(fragments[FRAGMENT_GPIO_1].name, "gpio", 32)) {
      zxlogf(ERROR, "%s: Unexpected name: %s", DRIVER_NAME, fragments[FRAGMENT_GPIO_1].name);
      return ZX_ERR_INTERNAL;
    }
    status = device_get_protocol(fragments[FRAGMENT_GPIO_1].device, ZX_PROTOCOL_GPIO, &gpio);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: could not get protocol ZX_PROTOCOL_GPIO", DRIVER_NAME);
      return status;
    }
    if (strncmp(fragments[FRAGMENT_I2C_1].name, "i2c", 32)) {
      zxlogf(ERROR, "%s: Unexpected name: %s", DRIVER_NAME, fragments[FRAGMENT_I2C_1].name);
      return ZX_ERR_INTERNAL;
    }
    if ((status = test_clock(&clock)) != ZX_OK) {
      zxlogf(ERROR, "%s: test_clock failed: %d", DRIVER_NAME, status);
      return status;
    }
    if ((status = test_power(&power)) != ZX_OK) {
      zxlogf(ERROR, "%s: test_power failed: %d", DRIVER_NAME, status);
      return status;
    }
    if ((status = test_gpio(&gpio)) != ZX_OK) {
      zxlogf(ERROR, "%s: test_gpio failed: %d", DRIVER_NAME, status);
      return status;
    }
  } else if (metadata.composite_device_id == PDEV_DID_TEST_COMPOSITE_2) {
    if (count != FRAGMENT_COUNT_2) {
      zxlogf(ERROR, "%s: got the wrong number of components (%u, %d)", DRIVER_NAME, count,
             FRAGMENT_COUNT_2);
      return ZX_ERR_BAD_STATE;
    }

    if (strncmp(fragments[FRAGMENT_CLOCK_2].name, "clock", 32)) {
      zxlogf(ERROR, "%s: Unexpected name: %s", DRIVER_NAME, fragments[FRAGMENT_CLOCK_2].name);
      return ZX_ERR_INTERNAL;
    }
    status = device_get_protocol(fragments[FRAGMENT_CLOCK_2].device, ZX_PROTOCOL_CLOCK, &clock);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: could not get protocol ZX_PROTOCOL_CLOCK", DRIVER_NAME);
      return status;
    }
    if (strncmp(fragments[FRAGMENT_POWER_2].name, "power", 32)) {
      zxlogf(ERROR, "%s: Unexpected name: %s", DRIVER_NAME, fragments[FRAGMENT_POWER_2].name);
      return ZX_ERR_INTERNAL;
    }
    status = device_get_protocol(fragments[FRAGMENT_POWER_2].device, ZX_PROTOCOL_POWER, &power);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: could not get protocol ZX_PROTOCOL_POWER", DRIVER_NAME);
      return status;
    }
    if (strncmp(fragments[FRAGMENT_CHILD4_2].name, "child4", 32)) {
      zxlogf(ERROR, "%s: Unexpected name: %s", DRIVER_NAME, fragments[FRAGMENT_CHILD4_2].name);
      return ZX_ERR_INTERNAL;
    }
    status = device_get_protocol(fragments[FRAGMENT_CHILD4_2].device, ZX_PROTOCOL_CLOCK, &child4);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: could not get protocol from child4", DRIVER_NAME);
      return status;
    }
    if (strncmp(fragments[FRAGMENT_SPI_2].name, "spi", 32)) {
      zxlogf(ERROR, "%s: Unexpected name: %s", DRIVER_NAME, fragments[FRAGMENT_SPI_2].name);
      return ZX_ERR_INTERNAL;
    }
    status = device_get_protocol(fragments[FRAGMENT_SPI_2].device, ZX_PROTOCOL_SPI, &spi);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: could not get protocol ZX_PROTOCOL_SPI", DRIVER_NAME);
      return status;
    }
    if (strncmp(fragments[FRAGMENT_PWM_2].name, "pwm", 32)) {
      zxlogf(ERROR, "%s: Unexpected name: %s", DRIVER_NAME, fragments[FRAGMENT_PWM_2].name);
      return ZX_ERR_INTERNAL;
    }
    status = device_get_protocol(fragments[FRAGMENT_PWM_2].device, ZX_PROTOCOL_PWM, &pwm);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: could not get protocol ZX_PROTOCOL_PWM", DRIVER_NAME);
      return status;
    }
    if ((status = test_clock(&clock)) != ZX_OK) {
      zxlogf(ERROR, "%s: test_clock failed: %d", DRIVER_NAME, status);
      return status;
    }
    if ((status = test_power(&power)) != ZX_OK) {
      zxlogf(ERROR, "%s: test_power failed: %d", DRIVER_NAME, status);
      return status;
    }
    if ((status = test_spi(&spi)) != ZX_OK) {
      zxlogf(ERROR, "%s: test_spi failed: %d", DRIVER_NAME, status);
      return status;
    }
    if ((status = test_pwm(&pwm)) != ZX_OK) {
      zxlogf(ERROR, "%s: test_pwm failed: %d", DRIVER_NAME, status);
      return status;
    }
  } else if (metadata.composite_device_id == PDEV_DID_TEST_GOLDFISH_CONTROL_COMPOSITE) {
    if (count != FRAGMENT_COUNT_GOLDFISH_CTRL) {
      zxlogf(ERROR, "%s: got the wrong number of fragments (%u, %d)", DRIVER_NAME, count,
             FRAGMENT_COUNT_GOLDFISH_CTRL);
      return ZX_ERR_BAD_STATE;
    }
  }

  test_t* test = calloc(1, sizeof(test_t));
  if (!test) {
    return ZX_ERR_NO_MEMORY;
  }

  device_add_args_t args = {
      .version = DEVICE_ADD_ARGS_VERSION,
      .name = "composite",
      .ctx = test,
      .ops = &test_device_protocol,
      .flags = DEVICE_ADD_NON_BINDABLE,
  };

  status = device_add(parent, &args, &test->zxdev);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: device_add failed: %d", DRIVER_NAME, status);
    free(test);
    return status;
  }

  // Make sure we can read metadata added to a fragment.
  status = device_get_metadata_size(test->zxdev, DEVICE_METADATA_PRIVATE, &size);
  if (status != ZX_OK || size != sizeof(composite_test_metadata)) {
    zxlogf(ERROR, "%s: device_get_metadata_size failed: %d", DRIVER_NAME, status);
    device_async_remove(test->zxdev);
    return ZX_ERR_INTERNAL;
  }
  status =
      device_get_metadata(test->zxdev, DEVICE_METADATA_PRIVATE, &metadata, sizeof(metadata), &size);
  if (status != ZX_OK || size != sizeof(composite_test_metadata)) {
    zxlogf(ERROR, "%s: device_get_metadata failed: %d", DRIVER_NAME, status);
    device_async_remove(test->zxdev);
    return ZX_ERR_INTERNAL;
  }

  if (metadata.metadata_value != 12345) {
    zxlogf(ERROR, "%s: device_get_metadata failed: %d", DRIVER_NAME, status);
    device_async_remove(test->zxdev);
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

static zx_driver_ops_t test_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = test_bind,
};

ZIRCON_DRIVER(test_composite, test_driver_ops, "zircon", "0.1");
