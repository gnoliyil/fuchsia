// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.driver.transport.test/cpp/driver/wire.h>
#include <fidl/fuchsia.driver.transport.test/cpp/markers.h>
#include <fidl/fuchsia.driver.transport.test/cpp/wire.h>
#include <lib/ddk/binding_driver.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/fdf/dispatcher.h>
#include <lib/fidl/cpp/wire/arena.h>
#include <lib/fidl/cpp/wire/connect_service.h>
#include <lib/fidl_driver/cpp/wire_messaging_declarations.h>

#include <ddktl/device.h>

namespace fdtt = fuchsia_driver_transport_test;

class Device;
using DeviceType =
    ddk::Device<Device, ddk::Unbindable, ddk::Messageable<fdtt::TestDeviceChild>::Mixin>;

class Device : public DeviceType {
 public:
  static zx_status_t Bind(void* ctx, zx_device_t* device);

  Device(zx_device_t* parent, fdf::ClientEnd<fdtt::DriverTransportProtocol> client,
         fdf_dispatcher_t* dispatcher)
      : DeviceType(parent), client_(std::move(client), dispatcher) {}

  // fdtt::TestDeviceChild protocol implementation.
  void GetParentDataOverDriverTransport(
      GetParentDataOverDriverTransportCompleter::Sync& completer) override;

  // Device protocol implementation.
  void DdkUnbind(ddk::UnbindTxn txn) { txn.Reply(); }
  void DdkRelease() { delete this; }

 private:
  fdf::WireSharedClient<fdtt::DriverTransportProtocol> client_;
};

void Device::GetParentDataOverDriverTransport(
    GetParentDataOverDriverTransportCompleter::Sync& completer) {
  fdf::Arena arena('TDAT');

  // Send a request to the parent driver over the driver transport.
  client_.buffer(std::move(arena))
      ->TransmitData()
      .ThenExactlyOnce(
          [completer = completer.ToAsync()](
              fdf::WireUnownedResult<fdtt::DriverTransportProtocol::TransmitData>& result) mutable {
            if (!result.ok()) {
              zxlogf(ERROR, "%s", result.FormatDescription().c_str());
              completer.ReplyError(result.status());
              return;
            }
            if (result->is_error()) {
              zxlogf(ERROR, "TransmitData failed with status: %d", result->error_value());
              completer.ReplyError(result->error_value());
              return;
            }

            // Reply to the test's fidl request with the data.
            completer.ReplySuccess(result->value()->out);
          });
}

// static
zx_status_t Device::Bind(void* ctx, zx_device_t* device) {
  // Connect to our parent driver.
  auto client_end = DdkConnectRuntimeProtocol<fdtt::Service::DriverTransportProtocol>(device);
  if (client_end.is_error()) {
    zxlogf(ERROR, "DdkConnectRuntimeProtocol Failed =(");
    return client_end.status_value();
  }

  auto* dispatcher = fdf_dispatcher_get_current_dispatcher();
  auto dev = std::make_unique<Device>(device, std::move(*client_end), dispatcher);

  zx_status_t status = dev->DdkAdd("child");
  if (status == ZX_OK) {
    // devmgr is now in charge of the memory for dev
    [[maybe_unused]] auto ptr = dev.release();
  }
  return status;
}

static zx_driver_ops_t kDriverOps = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = Device::Bind;
  return ops;
}();

ZIRCON_DRIVER(driver_transport_test_child, kDriverOps, "zircon", "0.1");
