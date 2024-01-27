// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED. DO NOT EDIT.
// Generated from the fuchsia.hardware.gpio banjo file

#ifndef SRC_DEVICES_GPIO_LIB_FUCHSIA_HARDWARE_GPIO_INCLUDE_C_FUCHSIA_HARDWARE_GPIO_C_BANJO_H_
#define SRC_DEVICES_GPIO_LIB_FUCHSIA_HARDWARE_GPIO_INCLUDE_C_FUCHSIA_HARDWARE_GPIO_C_BANJO_H_

#include <zircon/compiler.h>
#include <zircon/types.h>

__BEGIN_CDECLS

// Forward declarations
typedef uint32_t gpio_polarity_t;
#define GPIO_POLARITY_LOW UINT32_C(0x0)
#define GPIO_POLARITY_HIGH UINT32_C(0x1)
typedef struct gpio_protocol gpio_protocol_t;
typedef struct gpio_protocol_ops gpio_protocol_ops_t;
#define GPIO_PULL_UP UINT32_C(0x1)
#define GPIO_PULL_MASK UINT32_C(0x3)
// Flags for `ConfigIn`.
#define GPIO_PULL_DOWN UINT32_C(0x0)
#define GPIO_NO_PULL UINT32_C(0x2)

// Declarations
struct gpio_protocol_ops {
  zx_status_t (*config_in)(void* ctx, uint32_t flags);
  zx_status_t (*config_out)(void* ctx, uint8_t initial_value);
  zx_status_t (*set_alt_function)(void* ctx, uint64_t function);
  zx_status_t (*read)(void* ctx, uint8_t* out_value);
  zx_status_t (*write)(void* ctx, uint8_t value);
  zx_status_t (*get_interrupt)(void* ctx, uint32_t flags, zx_handle_t* out_irq);
  zx_status_t (*release_interrupt)(void* ctx);
  zx_status_t (*set_polarity)(void* ctx, gpio_polarity_t polarity);
  zx_status_t (*set_drive_strength)(void* ctx, uint64_t ds_ua, uint64_t* out_actual_ds_ua);
  zx_status_t (*get_drive_strength)(void* ctx, uint64_t* out_value);
};

struct gpio_protocol {
  const gpio_protocol_ops_t* ops;
  void* ctx;
};

// Helpers
// Configures a GPIO for input.
static inline zx_status_t gpio_config_in(const gpio_protocol_t* proto, uint32_t flags) {
  return proto->ops->config_in(proto->ctx, flags);
}

// Configures a GPIO for output.
static inline zx_status_t gpio_config_out(const gpio_protocol_t* proto, uint8_t initial_value) {
  return proto->ops->config_out(proto->ctx, initial_value);
}

// Configures the GPIO pin for an alternate function (I2C, SPI, etc)
// the interpretation of "function" is platform dependent.
static inline zx_status_t gpio_set_alt_function(const gpio_protocol_t* proto, uint64_t function) {
  return proto->ops->set_alt_function(proto->ctx, function);
}

// Reads the current value of a GPIO (0 or 1).
static inline zx_status_t gpio_read(const gpio_protocol_t* proto, uint8_t* out_value) {
  return proto->ops->read(proto->ctx, out_value);
}

// Sets the current value of the GPIO (any non-zero value maps to 1).
static inline zx_status_t gpio_write(const gpio_protocol_t* proto, uint8_t value) {
  return proto->ops->write(proto->ctx, value);
}

// Gets an interrupt object pertaining to a particular GPIO pin.
static inline zx_status_t gpio_get_interrupt(const gpio_protocol_t* proto, uint32_t flags,
                                             zx_handle_t* out_irq) {
  return proto->ops->get_interrupt(proto->ctx, flags, out_irq);
}

// Release the interrupt.
static inline zx_status_t gpio_release_interrupt(const gpio_protocol_t* proto) {
  return proto->ops->release_interrupt(proto->ctx);
}

// Set GPIO polarity.
static inline zx_status_t gpio_set_polarity(const gpio_protocol_t* proto,
                                            gpio_polarity_t polarity) {
  return proto->ops->set_polarity(proto->ctx, polarity);
}

// Set GPIO drive strength.
static inline zx_status_t gpio_set_drive_strength(const gpio_protocol_t* proto, uint64_t ds_ua,
                                                  uint64_t* out_actual_ds_ua) {
  return proto->ops->set_drive_strength(proto->ctx, ds_ua, out_actual_ds_ua);
}

// Get GPIO drive strength.
static inline zx_status_t gpio_get_drive_strength(const gpio_protocol_t* proto,
                                                  uint64_t* out_value) {
  return proto->ops->get_drive_strength(proto->ctx, out_value);
}

__END_CDECLS

#endif  // SRC_DEVICES_GPIO_LIB_FUCHSIA_HARDWARE_GPIO_INCLUDE_C_FUCHSIA_HARDWARE_GPIO_C_BANJO_H_
