// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.compat.nodegroup.test/cpp/wire.h>
#include <lib/async/cpp/task.h>
#include <lib/driver/component/cpp/driver_cpp.h>

namespace fcdt = fuchsia_compat_nodegroup_test;

namespace {

class NodeGroupDriver : public fdf::DriverBase {
 public:
  NodeGroupDriver(fdf::DriverStartArgs start_args,
                  fdf::UnownedSynchronizedDispatcher driver_dispatcher)
      : fdf::DriverBase("node_group", std::move(start_args), std::move(driver_dispatcher)) {}

  zx::result<> Start() override {
    auto connect_result = context().incoming()->Connect<fcdt::Waiter>();
    if (connect_result.is_error()) {
      FDF_LOG(ERROR, "Failed to start node-group driver: %s", connect_result.status_string());
      node().reset();
      return connect_result.take_error();
    }

    const fidl::WireSharedClient<fcdt::Waiter> client{std::move(connect_result.value()),
                                                      dispatcher()};
    [[maybe_unused]] auto result = client->Ack(ZX_OK);

    return zx::ok();
  }
};

}  // namespace

FUCHSIA_DRIVER_LIFECYCLE_CPP_V2(fdf::Lifecycle<NodeGroupDriver>);
