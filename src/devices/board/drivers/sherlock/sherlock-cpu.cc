// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <fidl/fuchsia.hardware.thermal/cpp/wire.h>
#include <lib/driver/component/cpp/composite_node_spec.h>
#include <lib/driver/component/cpp/node_add_args.h>

#include <bind/fuchsia/amlogic/platform/cpp/bind.h>
#include <bind/fuchsia/cpp/bind.h>
#include <bind/fuchsia/google/platform/cpp/bind.h>
#include <bind/fuchsia/thermal/cpp/bind.h>
#include <soc/aml-common/aml-cpu-metadata.h>
#include <soc/aml-t931/t931-hw.h>

#include "sherlock.h"

namespace {
namespace fpbus = fuchsia_hardware_platform_bus;

const std::vector<fpbus::Mmio> cpu_mmios{
    {{
        // AOBUS
        .base = T931_AOBUS_BASE,
        .length = T931_AOBUS_LENGTH,
    }},
};

constexpr amlogic_cpu::legacy_cluster_size_t cluster_sizes[] = {
    {.pd_id =
         static_cast<uint32_t>(fuchsia_hardware_thermal::wire::PowerDomain::kBigClusterPowerDomain),
     .core_count = 4},
    {.pd_id = static_cast<uint32_t>(
         fuchsia_hardware_thermal::wire::PowerDomain::kLittleClusterPowerDomain),
     .core_count = 2},
};

const std::vector<fpbus::Metadata> cpu_metadata{
    {{
        .type = DEVICE_METADATA_CLUSTER_SIZE_LEGACY,
        .data = std::vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(&cluster_sizes),
            reinterpret_cast<const uint8_t*>(&cluster_sizes) + sizeof(cluster_sizes)),
    }},
};

const fpbus::Node cpu_dev = []() {
  fpbus::Node result = {};
  result.name() = "aml-cpu";
  result.vid() = bind_fuchsia_google_platform::BIND_PLATFORM_DEV_VID_GOOGLE;
  result.pid() = bind_fuchsia_google_platform::BIND_PLATFORM_DEV_PID_SHERLOCK;
  result.did() = bind_fuchsia_google_platform::BIND_PLATFORM_DEV_DID_GOOGLE_AMLOGIC_CPU;
  result.metadata() = cpu_metadata;
  result.mmio() = cpu_mmios;
  return result;
}();

}  // namespace

namespace sherlock {

zx_status_t Sherlock::CpuInit() {
  fidl::Arena<> fidl_arena;
  fdf::Arena arena('SHER');

  auto aml_cpu_legacy_thermal_node = fuchsia_driver_framework::ParentSpec{{
      .bind_rules =
          {
              fdf::MakeAcceptBindRule(bind_fuchsia::PROTOCOL,
                                      bind_fuchsia_thermal::BIND_PROTOCOL_DEVICE),
              fdf::MakeAcceptBindRule(
                  bind_fuchsia::PLATFORM_DEV_DID,
                  bind_fuchsia_amlogic_platform::BIND_PLATFORM_DEV_DID_THERMAL_PLL),
          },
      .properties =
          {
              fdf::MakeProperty(bind_fuchsia::PROTOCOL, bind_fuchsia_thermal::BIND_PROTOCOL_DEVICE),
              fdf::MakeProperty(bind_fuchsia::PLATFORM_DEV_DID,
                                bind_fuchsia_amlogic_platform::BIND_PLATFORM_DEV_DID_THERMAL_PLL),
          },
  }};

  auto composite_spec = fuchsia_driver_framework::CompositeNodeSpec{{
      .name = "aml_cpu_legacy_spec",
      .parents = {{aml_cpu_legacy_thermal_node}},
  }};

  fdf::WireUnownedResult result = pbus_.buffer(arena)->AddCompositeNodeSpec(
      fidl::ToWire(fidl_arena, cpu_dev), fidl::ToWire(fidl_arena, composite_spec));
  if (!result.ok()) {
    zxlogf(ERROR, "%s: AddCompositeNodeSpec SherlockCpu(cpu_dev) request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: AddCompositeNodeSpec SherlockCpu(cpu_dev) failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

}  // namespace sherlock
