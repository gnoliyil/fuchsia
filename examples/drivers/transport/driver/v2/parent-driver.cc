// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.examples.gizmo/cpp/driver/wire.h>
#include <fidl/fuchsia.gizmo.protocol/cpp/wire.h>
#include <lib/driver/compat/cpp/compat.h>
#include <lib/driver/compat/cpp/context.h>
#include <lib/driver/component/cpp/driver_cpp.h>
#include <lib/driver/devfs/cpp/connector.h>

#include <bind/fuchsia/examples/gizmo/cpp/bind.h>

namespace driver_transport {

// Protocol served to child driver components over the Driver transport.
class DriverTransportServer : public fdf::WireServer<fuchsia_examples_gizmo::Device> {
 public:
  explicit DriverTransportServer() {}

  void GetHardwareId(fdf::Arena& arena, GetHardwareIdCompleter::Sync& completer) override {
    completer.buffer(arena).ReplySuccess(0x1234ABCD);
  }
  void GetFirmwareVersion(fdf::Arena& arena,
                          GetFirmwareVersionCompleter::Sync& completer) override {
    completer.buffer(arena).ReplySuccess(0x0, 0x1);
  }
};

// Protocol served to client components over devfs.
class TestProtocolServer : public fidl::WireServer<fuchsia_gizmo_protocol::TestingProtocol> {
 public:
  explicit TestProtocolServer() {}

  void GetValue(GetValueCompleter::Sync& completer) { completer.Reply(0x1234); }
};

class ParentDriverTransportDriver : public fdf::DriverBase {
 public:
  ParentDriverTransportDriver(fdf::DriverStartArgs start_args,
                              fdf::UnownedSynchronizedDispatcher driver_dispatcher)
      : DriverBase("transport-parent", std::move(start_args), std::move(driver_dispatcher)),
        devfs_connector_(fit::bind_member<&ParentDriverTransportDriver::Serve>(this)) {}

  zx::result<> Start() override {
    node_.Bind(std::move(node()));

    // Publish `fuchsia.examples.gizmo.Service` to the outgoing directory.
    auto protocol_handler = [this](fdf::ServerEnd<fuchsia_examples_gizmo::Device> request) -> void {
      auto server_impl = std::make_unique<DriverTransportServer>();
      fdf::BindServer(driver_dispatcher()->get(), std::move(request), std::move(server_impl));
    };
    fuchsia_examples_gizmo::Service::InstanceHandler handler(
        {.device = std::move(protocol_handler)});

    auto result =
        context().outgoing()->AddService<fuchsia_examples_gizmo::Service>(std::move(handler));
    if (result.is_error()) {
      FDF_SLOG(ERROR, "Failed to add service", KV("status", result.status_string()));
      return result.take_error();
    }

    // Initialize driver compat context
    compat::Context::ConnectAndCreate(
        &context(), dispatcher(),
        [this](zx::result<std::shared_ptr<compat::Context>> compat_result) {
          if (!compat_result.is_ok()) {
            FDF_LOG(ERROR, "Call to Context::ConnectAndCreate failed: %s",
                    compat_result.status_string());
            node().reset();
            return;
          }
          compat_context_ = std::move(*compat_result);

          auto result = ExportService(name());
          if (result.is_error()) {
            FDF_SLOG(ERROR, "Failed to export to services", KV("status", result.status_string()));
            node().reset();
            return;
          }

          result = AddChild(name());
          if (result.is_error()) {
            FDF_SLOG(ERROR, "Failed to add child node", KV("status", result.status_string()));
            node().reset();
            return;
          }
        });

    return zx::ok();
  }

  // Add a child device node and offer the service capabilities.
  zx::result<> AddChild(std::string_view node_name) {
    fidl::Arena arena;
    // Offer `fuchsia.driver.compat.Service` to the driver that binds to the node.
    auto offers = child_->CreateOffers(arena);
    // Offer `fuchsia.examples.gizmo.Service` to the driver that binds to the node.
    auto service_offer = fuchsia_component_decl::wire::OfferService::Builder(arena)
                             .source_name(arena, fuchsia_examples_gizmo::Service::Name)
                             .target_name(arena, fuchsia_examples_gizmo::Service::Name)
                             .Build();
    offers.push_back(fuchsia_component_decl::wire::Offer::WithService(arena, service_offer));

    auto properties = fidl::VectorView<fuchsia_driver_framework::wire::NodeProperty>(arena, 1);
    properties[0] = fuchsia_driver_framework::wire::NodeProperty::Builder(arena)
                        .key(fuchsia_driver_framework::wire::NodePropertyKey::WithStringValue(
                            arena, bind_fuchsia_examples_gizmo::DEVICE))
                        .value(fuchsia_driver_framework::wire::NodePropertyValue::WithEnumValue(
                            arena, bind_fuchsia_examples_gizmo::DEVICE_DRIVERTRANSPORT))
                        .Build();

    auto args = fuchsia_driver_framework::wire::NodeAddArgs::Builder(arena)
                    .name(arena, node_name)
                    .offers(offers)
                    .properties(properties)
                    .Build();

    // Create endpoints of the `NodeController` for the node.
    auto endpoints = fidl::CreateEndpoints<fuchsia_driver_framework::NodeController>();
    if (endpoints.is_error()) {
      FDF_SLOG(ERROR, "Failed to create endpoint", KV("status", endpoints.status_string()));
      return zx::error(endpoints.status_value());
    }
    auto result = node_->AddChild(args, std::move(endpoints->server), {});
    if (!result.ok()) {
      FDF_SLOG(ERROR, "Failed to add child", KV("status", result.status_string()));
      return zx::error(result.status());
    }
    controller_.Bind(std::move(endpoints->client));

    return zx::ok();
  }

  // Publish offered services for client components.
  zx::result<> ExportService(std::string_view node_name) {
    // Publish `fuchsia.gizmo.protocol.Service` to the outgoing directory.
    fuchsia_gizmo_protocol::Service::InstanceHandler handler({
        .testing = fit::bind_member<&ParentDriverTransportDriver::Serve>(this),
    });

    auto result =
        context().outgoing()->AddService<fuchsia_gizmo_protocol::Service>(std::move(handler));
    if (result.is_error()) {
      FDF_SLOG(ERROR, "Failed to add service", KV("status", result.status_string()));
      return result.take_error();
    }

    // Publish `fuchsia.driver.compat.Service` to the outgoing directory.
    child_ = compat::DeviceServer(std::string(node_name), 0,
                                  compat_context_->TopologicalPath(node_name));
    auto status = child_->Serve(dispatcher(), &context().outgoing()->component());
    if (status != ZX_OK) {
      FDF_LOG(ERROR, "Failed to serve compat device server: %s", zx_status_get_string(status));
      return zx::error(status);
    }

    // Export to devfs.
    if (zx::result status = ExportToDevfs(child_->topological_path()); status.is_error()) {
      FDF_LOG(ERROR, "Failed to export to devfs: %s", status.status_string());
      return status.take_error();
    }
    return zx::ok();
  }

 private:
  void Serve(fidl::ServerEnd<fuchsia_gizmo_protocol::TestingProtocol> server) {
    auto server_impl = std::make_unique<TestProtocolServer>();
    fidl::BindServer(dispatcher(), std::move(server), std::move(server_impl));
  }

  zx::result<> ExportToDevfs(std::string_view devfs_path) {
    zx::result connection = context().incoming()->Connect<fuchsia_device_fs::Exporter>();
    if (connection.is_error()) {
      return connection.take_error();
    }
    fidl::WireSyncClient devfs_exporter{std::move(connection.value())};

    zx::result connector = devfs_connector_.Bind(dispatcher());
    if (connector.is_error()) {
      return connector.take_error();
    }
    fidl::WireResult result = devfs_exporter->ExportV2(
        std::move(connector.value()), fidl::StringView::FromExternal(devfs_path),
        fidl::StringView(), fuchsia_device_fs::ExportOptions());
    if (!result.ok()) {
      return zx::error(result.status());
    }
    if (result.value().is_error()) {
      return result.value().take_error();
    }
    return zx::ok();
  }

  std::optional<compat::DeviceServer> child_;
  driver_devfs::Connector<fuchsia_gizmo_protocol::TestingProtocol> devfs_connector_;
  std::shared_ptr<compat::Context> compat_context_;
  fidl::WireSyncClient<fuchsia_driver_framework::Node> node_;
  fidl::WireSyncClient<fuchsia_driver_framework::NodeController> controller_;
};

}  // namespace driver_transport

FUCHSIA_DRIVER_LIFECYCLE_CPP_V3(fdf::Lifecycle<driver_transport::ParentDriverTransportDriver>);
