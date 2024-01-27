// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.rebind.test/cpp/fidl.h>
#include <lib/driver/compat/cpp/compat.h>
#include <lib/driver/component/cpp/driver_cpp.h>
#include <lib/driver/devfs/cpp/connector.h>

#include "fidl/fuchsia.rebind.test/cpp/markers.h"

namespace rebind_child {

const std::string kDriverName = "rebind-child";

class RebindChildServer : public fidl::Server<fuchsia_rebind_test::RebindChild> {};

class RebindChild : public fdf::DriverBase {
 public:
  RebindChild(fdf::DriverStartArgs start_args, fdf::UnownedSynchronizedDispatcher driver_dispatcher)
      : DriverBase(kDriverName, std::move(start_args), std::move(driver_dispatcher)),
        devfs_connector_(fit::bind_member<&RebindChild::Serve>(this)) {}

  zx::result<> Start() override { return ExportToDevfs(); }

 private:
  zx::result<> ExportToDevfs() {
    // Create a node for devfs.
    fidl::Arena arena;
    zx::result connector = devfs_connector_.Bind(dispatcher());
    if (connector.is_error()) {
      return connector.take_error();
    }

    auto devfs = fuchsia_driver_framework::wire::DevfsAddArgs::Builder(arena).connector(
        std::move(connector.value()));

    auto args = fuchsia_driver_framework::wire::NodeAddArgs::Builder(arena)
                    .name(arena, name())
                    .devfs_args(devfs.Build())
                    .Build();

    // Create endpoints of the `NodeController` for the node.
    zx::result controller_endpoints =
        fidl::CreateEndpoints<fuchsia_driver_framework::NodeController>();
    ZX_ASSERT_MSG(controller_endpoints.is_ok(), "Failed: %s", controller_endpoints.status_string());

    zx::result node_endpoints = fidl::CreateEndpoints<fuchsia_driver_framework::Node>();
    ZX_ASSERT_MSG(node_endpoints.is_ok(), "Failed: %s", node_endpoints.status_string());

    fidl::WireResult result = fidl::WireCall(node())->AddChild(
        args, std::move(controller_endpoints->server), std::move(node_endpoints->server));
    if (!result.ok()) {
      FDF_SLOG(ERROR, "Failed to add child", KV("status", result.status_string()));
      return zx::error(result.status());
    }
    controller_.Bind(std::move(controller_endpoints->client));
    node_.Bind(std::move(node_endpoints->client));
    return zx::ok();
  }

  void Serve(fidl::ServerEnd<fuchsia_rebind_test::RebindChild> server) {
    fidl::BindServer(dispatcher(), std::move(server), &server_);
  }

  RebindChildServer server_;

  fidl::WireSyncClient<fuchsia_driver_framework::Node> node_;
  fidl::WireSyncClient<fuchsia_driver_framework::NodeController> controller_;
  driver_devfs::Connector<fuchsia_rebind_test::RebindChild> devfs_connector_;
};

}  // namespace rebind_child

FUCHSIA_DRIVER_LIFECYCLE_CPP_V3(fdf::Lifecycle<rebind_child::RebindChild>);
