// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <bind/fuchsia/reloaddriverbind/test/cpp/bind.h>

#include "src/devices/tests/v2/reload-driver/driver_helpers.h"

namespace bindlib = bind_fuchsia_reloaddriverbind_test;
namespace helpers = reload_test_driver_helpers;

namespace {

class RightParentDriver : public fdf::DriverBase {
 public:
  RightParentDriver(fdf::DriverStartArgs start_args,
                    fdf::UnownedSynchronizedDispatcher driver_dispatcher)
      : fdf::DriverBase("right_parent", std::move(start_args), std::move(driver_dispatcher)) {}

  zx::result<> Start() override {
    node_client_.Bind(std::move(node()));

    zx::result result = helpers::AddChild(logger(), "E", node_client_,
                                          bindlib::TEST_BIND_PROPERTY_COMPOSITE_PARENT_E);
    if (result.is_error()) {
      return result.take_error();
    }
    node_controller_1_.Bind(std::move(result.value()));

    result = helpers::AddChild(logger(), "F", node_client_,
                               bindlib::TEST_BIND_PROPERTY_COMPOSITE_PARENT_F);
    if (result.is_error()) {
      return result.take_error();
    }
    node_controller_2_.Bind(std::move(result.value()));

    result = helpers::AddChild(logger(), "G", node_client_, bindlib::TEST_BIND_PROPERTY_TARGET_1);
    if (result.is_error()) {
      return result.take_error();
    }
    node_controller_3_.Bind(std::move(result.value()));

    return helpers::SendAck(logger(), node_name().value_or("None"), incoming(), name());
  }

 private:
  fidl::SyncClient<fuchsia_driver_framework::Node> node_client_;
  fidl::SyncClient<fuchsia_driver_framework::NodeController> node_controller_1_;
  fidl::SyncClient<fuchsia_driver_framework::NodeController> node_controller_2_;
  fidl::SyncClient<fuchsia_driver_framework::NodeController> node_controller_3_;
};

}  // namespace

FUCHSIA_DRIVER_EXPORT(RightParentDriver);
