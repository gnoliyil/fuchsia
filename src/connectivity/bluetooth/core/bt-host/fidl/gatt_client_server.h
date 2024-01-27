// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_FIDL_GATT_CLIENT_SERVER_H_
#define SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_FIDL_GATT_CLIENT_SERVER_H_

#include <fuchsia/bluetooth/gatt/cpp/fidl.h>

#include "lib/fidl/cpp/binding.h"
#include "src/connectivity/bluetooth/core/bt-host/common/macros.h"
#include "src/connectivity/bluetooth/core/bt-host/fidl/gatt_remote_service_server.h"
#include "src/connectivity/bluetooth/core/bt-host/fidl/server_base.h"

namespace bthost {

// Implements the gatt::Client FIDL interface.
class GattClientServer : public GattServerBase<fuchsia::bluetooth::gatt::Client> {
 public:
  GattClientServer(bt::gatt::PeerId peer_id, bt::gatt::GATT::WeakPtr gatt,
                   fidl::InterfaceRequest<fuchsia::bluetooth::gatt::Client> request);
  ~GattClientServer() override = default;

 private:
  // bluetooth::gatt::Client overrides:
  void ListServices(::fidl::VectorPtr<::std::string> uuids, ListServicesCallback callback) override;
  void ConnectToService(
      uint64_t id,
      ::fidl::InterfaceRequest<fuchsia::bluetooth::gatt::RemoteService> request) override;

  // The ID of the peer that this client is attached to.
  bt::gatt::PeerId peer_id_;

  // Remote GATT services that were connected through this client. The value can
  // be null while a ConnectToService request is in progress.
  std::unordered_map<uint64_t, std::unique_ptr<GattRemoteServiceServer>> connected_services_;

  WeakSelf<GattClientServer> weak_self_;

  BT_DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(GattClientServer);
};

}  // namespace bthost

#endif  // SRC_CONNECTIVITY_BLUETOOTH_CORE_BT_HOST_FIDL_GATT_CLIENT_SERVER_H_
