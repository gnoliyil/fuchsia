// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/lib/fragment-irq/dfv2/fragment-irq.h"

#include <fidl/fuchsia.hardware.interrupt/cpp/fidl.h>

namespace fragment_irq {
namespace fint = fuchsia_hardware_interrupt;

zx::result<zx::interrupt> GetInterrupt(const fdf::Namespace& ns, std::string_view instance_name) {
  auto result = ns.Connect<fint::Service::Provider>(instance_name);
  if (result.is_error()) {
    return result.take_error();
  }

  auto call_result = fidl::WireCall(*result)->Get();
  if (!call_result.ok()) {
    return zx::error(call_result.status());
  }
  if (call_result->is_error()) {
    return zx::error(call_result->error_value());
  }

  return zx::ok(std::move(call_result->value()->interrupt));
}

zx::result<zx::interrupt> GetInterrupt(const fdf::Namespace& ns, uint32_t which) {
  char buffer[] = "irqXXX";
  snprintf(buffer, sizeof(buffer), "irq%03u", which);
  return GetInterrupt(ns, buffer);
}

}  // namespace fragment_irq
