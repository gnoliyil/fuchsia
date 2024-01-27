// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/tests/runtime-composite-test/drivers/leaf-driver.h"

#include <lib/ddk/metadata.h>

#include <bind/composite/test/lib/cpp/bind.h>
#include <bind/fuchsia/cpp/bind.h>

#include "src/devices/tests/runtime-composite-test/drivers/composite-driver.h"
#include "src/devices/tests/runtime-composite-test/drivers/leaf-driver-bind.h"

namespace bind_test = bind_composite_test_lib;

namespace leaf_driver {

// static
zx_status_t LeafDriver::Bind(void* ctx, zx_device_t* device) {
  auto dev = std::make_unique<LeafDriver>(device);

  auto status = dev->DdkAdd("leaf");
  if (status != ZX_OK) {
    return status;
  }

  // Add the spec.
  const uint32_t node_1_bind_rule_values[] = {10, 3};
  const ddk::BindRule node_1_bind_rules[] = {
      ddk::MakeAcceptBindRuleList(50, node_1_bind_rule_values),
      ddk::MakeRejectBindRule(bind_test::FLAG, true),
  };

  const device_bind_prop_t node_1_properties[] = {
      ddk::MakeProperty(bind_fuchsia::PROTOCOL, bind_test::BIND_PROTOCOL_VALUE_1),
      ddk::MakeProperty(bind_fuchsia::USB_VID, bind_test::BIND_USB_VID_VALUE),
  };

  const char* node_2_props_values[] = {bind_test::TEST_PROP_VALUE_1.c_str(),
                                       bind_test::TEST_PROP_VALUE_2.c_str()};
  const ddk::BindRule node_2_bind_rules[] = {
      ddk::MakeAcceptBindRuleList(bind_test::TEST_PROP, node_2_props_values),
      ddk::MakeRejectBindRule(20, 10),
  };

  const device_bind_prop_t node_2_properties[] = {
      ddk::MakeProperty(bind_fuchsia::PROTOCOL, bind_test::BIND_PROTOCOL_VALUE_2),
  };

  const device_metadata_t metadata[] = {
      {
          .type = DEVICE_METADATA_PRIVATE,
          .data = const_cast<char*>(composite_driver::kMetadataStr),
          .length = strlen(composite_driver::kMetadataStr) + 1,
      },
  };

  status = dev->DdkAddCompositeNodeSpec("test_composite_1",
                                        ddk::CompositeNodeSpec(node_1_bind_rules, node_1_properties)
                                            .AddParentSpec(node_2_bind_rules, node_2_properties)
                                            .set_metadata(metadata));
  if (status != ZX_OK) {
    return status;
  }

  [[maybe_unused]] auto ptr = dev.release();

  return ZX_OK;
}

static zx_driver_ops_t kDriverOps = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = LeafDriver::Bind;
  return ops;
}();

}  // namespace leaf_driver

ZIRCON_DRIVER(LeafDriver, leaf_driver::kDriverOps, "zircon", "0.1");
