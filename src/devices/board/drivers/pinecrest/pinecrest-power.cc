// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/metadata.h>
#include <lib/ddk/platform-defs.h>

#include <ddk/metadata/power.h>
#include <soc/as370/as370-power.h>

#include "pinecrest.h"
#include "src/devices/board/drivers/pinecrest/pinecrest-bind.h"

namespace board_pinecrest {
namespace fpbus = fuchsia_hardware_platform_bus;

namespace {

static const zx_bind_inst_t power_impl_driver_match[] = {
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_POWER_IMPL),
};

constexpr device_fragment_part_t power_impl_fragment[] = {
    {std::size(power_impl_driver_match), power_impl_driver_match},
};

zx_device_prop_t power_domain_kBuckSoC_props[] = {
    {BIND_POWER_DOMAIN_COMPOSITE, 0, PDEV_DID_POWER_DOMAIN_COMPOSITE},
};

constexpr device_fragment_t power_domain_kBuckSoC_fragments[] = {
    {"power-impl", std::size(power_impl_fragment), power_impl_fragment},
};

static const power_domain_t power_domain_kBuckSoC[] = {
    {kBuckSoC},
};

static const device_metadata_t power_domain_kBuckSoC_metadata[] = {
    {
        .type = DEVICE_METADATA_POWER_DOMAINS,
        .data = &power_domain_kBuckSoC,
        .length = sizeof(power_domain_kBuckSoC),
    },
};

const composite_device_desc_t power_domain_kBuckSoC_desc = {
    .props = power_domain_kBuckSoC_props,
    .props_count = std::size(power_domain_kBuckSoC_props),
    .fragments = power_domain_kBuckSoC_fragments,
    .fragments_count = std::size(power_domain_kBuckSoC_fragments),
    .primary_fragment = "power-impl",
    .spawn_colocated = true,
    .metadata_list = power_domain_kBuckSoC_metadata,
    .metadata_count = std::size(power_domain_kBuckSoC_metadata),
};

static const zx_bind_inst_t i2c_match[] = {
    BI_ABORT_IF(NE, BIND_FIDL_PROTOCOL, ZX_FIDL_PROTOCOL_I2C),
    BI_ABORT_IF(NE, BIND_I2C_BUS_ID, 0x0),
    BI_MATCH_IF(EQ, BIND_I2C_ADDRESS, 0x61),
};

static const device_fragment_part_t i2c_fragment[] = {
    {std::size(i2c_match), i2c_match},
};

static const device_fragment_t fragments[] = {
    {"i2c", std::size(i2c_fragment), i2c_fragment},
};

constexpr zx_device_prop_t props[] = {
    {BIND_PLATFORM_DEV_VID, 0, PDEV_VID_SYNAPTICS},
    {BIND_PLATFORM_DEV_DID, 0, PDEV_DID_AS370_POWER},
};

const composite_device_desc_t comp_desc = {
    .props = props,
    .props_count = std::size(props),
    .fragments = fragments,
    .fragments_count = std::size(fragments),
    .primary_fragment = "i2c",
    .spawn_colocated = false,
    .metadata_list = nullptr,
    .metadata_count = 0,
};

}  // namespace

zx_status_t Pinecrest::PowerInit() {
  zx_status_t status;

  status = DdkAddComposite("power", &comp_desc);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: CompositeDeviceAdd for powerimpl failed %d", __FUNCTION__, status);
    return status;
  }

  status = DdkAddComposite("composite-pd-kBuckSoC", &power_domain_kBuckSoC_desc);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: CompositeDeviceAdd for power domain kBuckSoC failed %d", __FUNCTION__,
           status);
    return status;
  }

  return ZX_OK;
}

}  // namespace board_pinecrest
