// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This header has to come first, and we define our ZX_PROTOCOL, so that
// we don't have to edit protodefs.h to add this test protocol.
#include <bind/fuchsia/lifecycle/cpp/bind.h>
#define ZX_PROTOCOL_PARENT bind_fuchsia_lifecycle::BIND_PROTOCOL_PARENT

#include <fidl/fuchsia.device.fs/cpp/fidl.h>
#include <fidl/fuchsia.driver.compat/cpp/fidl.h>
#include <fidl/fuchsia.driver.framework/cpp/wire.h>
#include <fidl/fuchsia.lifecycle.test/cpp/wire.h>
#include <fuchsia/lifecycle/test/cpp/banjo.h>
#include <lib/driver/compat/cpp/context.h>
#include <lib/driver/compat/cpp/device_server.h>
#include <lib/driver/compat/cpp/symbols.h>
#include <lib/driver/component/cpp/driver_cpp.h>

namespace fdf {
using namespace fuchsia_driver_framework;
}  // namespace fdf

namespace ft = fuchsia_lifecycle_test;

namespace {

class LifecycleDriver : public fdf::DriverBase, public fidl::WireServer<ft::Device> {
 public:
  LifecycleDriver(fdf::DriverStartArgs start_args,
                  fdf::UnownedSynchronizedDispatcher driver_dispatcher)
      : DriverBase("lifeycle-driver", std::move(start_args), std::move(driver_dispatcher)) {}

  zx::result<> Start() override {
    FDF_LOG(INFO, "Starting lifecycle driver");

    // Get our parent banjo symbol.
    auto parent_symbol = fdf::GetSymbol<compat::device_t*>(symbols(), compat::kDeviceSymbol);
    if (parent_symbol->proto_ops.id != ZX_PROTOCOL_PARENT) {
      FDF_LOG(ERROR, "Didn't find PARENT banjo protocol, found protocol id: %d",
              parent_symbol->proto_ops.id);
      return zx::error(ZX_ERR_NOT_FOUND);
    }

    parent_protocol_t proto = {};
    proto.ctx = parent_symbol->context;
    proto.ops = reinterpret_cast<const parent_protocol_ops_t*>(parent_symbol->proto_ops.ops);
    parent_client_ = ddk::ParentProtocolClient(&proto);
    if (!parent_client_.is_valid()) {
      FDF_LOG(ERROR, "Failed to create parent client");
      return zx::error(ZX_ERR_INTERNAL);
    }

    // Serve our Service.
    ft::Service::InstanceHandler handler(
        {.device = [this](fidl::ServerEnd<ft::Device> request) -> void {
          fidl::BindServer(dispatcher(), std::move(request), this);
        }});

    auto result = context().outgoing()->AddService<ft::Service>(std::move(handler));
    if (result.is_error()) {
      FDF_SLOG(ERROR, "Failed to add Demo service", KV("status", result.status_string()));
      return result.take_error();
    }

    // Create our compat context, and serve our device when it's created.
    compat::Context::ConnectAndCreate(
        &context(), dispatcher(), [this](zx::result<std::shared_ptr<compat::Context>> context) {
          if (!context.is_ok()) {
            FDF_LOG(ERROR, "Call to Context::ConnectAndCreate failed: %s", context.status_string());
            node().reset();
            return;
          }
          compat_context_ = std::move(*context);
          const auto kDeviceName = "lifecycle-device";
          child_ =
              compat::DeviceServer(kDeviceName, 0, compat_context_->TopologicalPath(kDeviceName));
          const auto kServicePath =
              std::string(ft::Service::Name) + "/" + component::kDefaultInstance + "/device";
          child_->ExportToDevfs(
              compat_context_->devfs_exporter(), kServicePath, [this](zx_status_t status) {
                if (status != ZX_OK) {
                  FDF_LOG(WARNING, "Failed to export to devfs: %s", zx_status_get_string(status));
                  node().reset();
                }
              });
        });
    return zx::ok();
  }

  // fidl::WireServer<ft::Device>
  void Ping(PingCompleter::Sync& completer) override { completer.Reply(); }

  // fidl::WireServer<ft::Device>
  void GetString(GetStringCompleter::Sync& completer) override {
    char str[100];
    parent_client_.GetString(str, 100);
    completer.Reply(fidl::StringView::FromExternal(std::string(str)));
  }

  // fidl::WireServer<ft::Device>
  void Stop(StopCompleter::Sync& completer) override {
    stop_completer_ = completer.ToAsync();
    // Resetting the node handle will result in the driver being stopped.
    node().reset();
  }

  void PrepareStop(PrepareStopContext* context) override {
    ZX_ASSERT(stop_completer_);
    ZX_ASSERT(stop_called_ == false);
    stop_completer_->Reply();
    context->complete(context, ZX_OK);
  }

  void Stop() override { stop_called_ = true; }

 private:
  std::optional<compat::DeviceServer> child_;
  std::shared_ptr<compat::Context> compat_context_;

  std::optional<StopCompleter::Async> stop_completer_;
  bool stop_called_ = false;

  ddk::ParentProtocolClient parent_client_;
};

}  // namespace

FUCHSIA_DRIVER_RECORD_CPP_V3(fdf::Record<LifecycleDriver>);
