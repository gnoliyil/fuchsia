// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>

#include <ddk/metadata/power.h>

#include "test.h"

namespace {

// Composite binding rules for power domain 1
static const zx_bind_inst_t power_impl_driver_match[] = {
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_POWER_IMPL),
};
constexpr device_fragment_part_t power_impl_fragment[] = {
    {std::size(power_impl_driver_match), power_impl_driver_match},
};
zx_device_prop_t props[] = {
    {BIND_POWER_DOMAIN_COMPOSITE, 0, PDEV_DID_POWER_DOMAIN_COMPOSITE},
};

constexpr device_fragment_t power_domain_1_fragments[] = {
    {"power-impl", std::size(power_impl_fragment), power_impl_fragment},
};
static const power_domain_t power_domain_1[] = {
    {1},
};

static const device_metadata_t power_metadata_1[] = {{
    .type = DEVICE_METADATA_POWER_DOMAINS,
    .data = &power_domain_1,
    .length = sizeof(power_domain_1),
}};
const composite_device_desc_t power_domain_1_desc = {
    .props = props,
    .props_count = std::size(props),
    .fragments = power_domain_1_fragments,
    .fragments_count = std::size(power_domain_1_fragments),
    .primary_fragment = "power-impl",
    .spawn_colocated = true,
    .metadata_list = power_metadata_1,
    .metadata_count = std::size(power_metadata_1),
};

// Composite binding rules for power domain 3
static const zx_bind_inst_t parent_domain_match[] = {
    BI_ABORT_IF(NE, BIND_FIDL_PROTOCOL, ZX_FIDL_PROTOCOL_POWER),
    BI_MATCH_IF(EQ, BIND_POWER_DOMAIN, 1),
};
constexpr device_fragment_part_t parent_domain_fragment[] = {
    {std::size(parent_domain_match), parent_domain_match},
};

constexpr device_fragment_t power_domain_3_fragments[] = {
    {"power-impl", std::size(power_impl_fragment), power_impl_fragment},
    {"power-domain", std::size(parent_domain_fragment), parent_domain_fragment},
};
static const power_domain_t power_domain_3[] = {
    {3},
};

static const device_metadata_t power_metadata_3[] = {{
    .type = DEVICE_METADATA_POWER_DOMAINS,
    .data = &power_domain_3,
    .length = sizeof(power_domain_3),
}};
const composite_device_desc_t power_domain_3_desc = {
    .props = props,
    .props_count = std::size(props),
    .fragments = power_domain_3_fragments,
    .fragments_count = std::size(power_domain_3_fragments),
    .primary_fragment = "power-impl",
    .spawn_colocated = true,
    .metadata_list = power_metadata_3,
    .metadata_count = std::size(power_metadata_3),
};

}  // namespace

namespace board_test {
namespace fpbus = fuchsia_hardware_platform_bus;

zx_status_t TestBoard::PowerInit() {
  fpbus::Node power_dev;
  power_dev.name() = "power";
  power_dev.vid() = PDEV_VID_TEST;
  power_dev.pid() = PDEV_PID_PBUS_TEST;
  power_dev.did() = PDEV_DID_TEST_POWER;

  fidl::Arena<> fidl_arena;
  fdf::Arena arena('TPWR');
  auto result = pbus_.buffer(arena)->NodeAdd(fidl::ToWire(fidl_arena, power_dev));
  if (!result.ok()) {
    zxlogf(ERROR, "%s: DeviceAdd Power request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: DeviceAdd Power failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }
  zx_status_t status = DdkAddComposite("composite-pd-1", &power_domain_1_desc);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: DdkAddComposite for power domain 1 failed: %d ", __FUNCTION__, status);
    return status;
  }

  status = DdkAddComposite("composite-pd-3", &power_domain_3_desc);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: DdkAddComposite for power domain 3 failed: %d", __FUNCTION__, status);
    return status;
  }

  return ZX_OK;
}

}  // namespace board_test
