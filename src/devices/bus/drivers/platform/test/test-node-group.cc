// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.hardware.platform.bus/cpp/driver/fidl.h>
#include <fidl/fuchsia.hardware.platform.bus/cpp/fidl.h>
#include <lib/ddk/binding.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/driver/component/cpp/node_add_args.h>
#include <lib/driver/component/cpp/node_group.h>

#include <bind/fuchsia/cpp/bind.h>
#include <bind/fuchsia/test/cpp/bind.h>
#include <bind/fuchsia/test/platform/cpp/bind.h>

#include "test.h"

namespace board_test {

zx_status_t TestBoard::NodeGroupInit() {
  fuchsia_hardware_platform_bus::Node dev;
  dev.name() = "node_a";
  dev.vid() = bind_fuchsia_test_platform::BIND_PLATFORM_DEV_VID_TEST;
  dev.pid() = bind_fuchsia_test_platform::BIND_PLATFORM_DEV_PID_PBUS_TEST;
  dev.did() = bind_fuchsia_test_platform::BIND_PLATFORM_DEV_DID_NODE_GROUP;

  fidl::Arena<> fidl_arena;
  fdf::Arena arena('DVGP');

  auto bind_rules = std::vector{
      fdf::MakeAcceptBindRule(bind_fuchsia::PLATFORM_DEV_VID,
                              bind_fuchsia_test_platform::BIND_PLATFORM_DEV_VID_TEST),
      fdf::MakeAcceptBindRule(bind_fuchsia::PLATFORM_DEV_PID,
                              bind_fuchsia_test_platform::BIND_PLATFORM_DEV_PID_PBUS_TEST),
      fdf::MakeAcceptBindRule(
          bind_fuchsia::PLATFORM_DEV_DID,
          bind_fuchsia_test_platform::BIND_PLATFORM_DEV_DID_NODE_REPRESENTATION),
  };

  auto properties = std::vector{
      fdf::MakeProperty(bind_fuchsia::PROTOCOL, bind_fuchsia_test::BIND_PROTOCOL_DEVICE),
  };

  auto nodes = std::vector{
      fuchsia_driver_framework::NodeRepresentation{{
          .bind_rules = bind_rules,
          .properties = properties,
      }},
  };

  auto node_group = fuchsia_driver_framework::NodeGroup{{.name = "node_group", .nodes = nodes}};
  auto result = pbus_.buffer(arena)->AddNodeGroup(fidl::ToWire(fidl_arena, dev),
                                                  fidl::ToWire(fidl_arena, node_group));

  if (!result.ok()) {
    zxlogf(ERROR, "%s: AddNodeGroup node_group(dev) request failed: %s", __func__,
           result.FormatDescription().data());
    return result.status();
  }
  if (result->is_error()) {
    zxlogf(ERROR, "%s: AddNodeGroup node_group(dev) failed: %s", __func__,
           zx_status_get_string(result->error_value()));
    return result->error_value();
  }

  return ZX_OK;
}

}  // namespace board_test
