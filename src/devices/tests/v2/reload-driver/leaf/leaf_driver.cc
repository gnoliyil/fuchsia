// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/devices/tests/v2/reload-driver/driver_helpers.h"

namespace helpers = reload_test_driver_helpers;

namespace {

class LeafDriver : public fdf::DriverBase {
 public:
  LeafDriver(fdf::DriverStartArgs start_args, fdf::UnownedSynchronizedDispatcher driver_dispatcher)
      : fdf::DriverBase("leaf", std::move(start_args), std::move(driver_dispatcher)) {}

  zx::result<> Start() override {
    return helpers::SendAck(logger(), node_name().value_or("None"), context(), name());
  }
};

}  // namespace

FUCHSIA_DRIVER_LIFECYCLE_CPP_V3(fdf::Lifecycle<LeafDriver>);
