// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/bringup/bin/netsvc/netifc.h"

#include <dirent.h>
#include <lib/fzl/vmo-mapper.h>
#include <stdio.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/time.h>
#include <zircon/types.h>

#include <thread>

#include "src/bringup/bin/netsvc/inet6.h"
#include "src/bringup/bin/netsvc/netifc-discover.h"

namespace {

struct NetdeviceIfc {
  NetdeviceIfc(fidl::ClientEnd<fuchsia_hardware_network::Device> device,
               async_dispatcher_t* dispatcher, fit::callback<void(zx_status_t)> on_error,
               fuchsia_hardware_network::wire::PortId port_id)
      : client(std::move(device), dispatcher), port_id(port_id), on_error(std::move(on_error)) {}

  zx::result<DeviceBuffer> GetBuffer(size_t len, bool block) {
    network::client::NetworkDeviceClient::Buffer tx = client.AllocTx();
    if (!tx.is_valid()) {
      // Be loud in case the caller expects this to be synchronous, we can
      // change strategies if this proves a problem.
      if (block) {
        printf("netifc: netdevice does not block for new buffers, transfer will fail\n");
      }
      return zx::error(ZX_ERR_NO_RESOURCES);
    }
    if (len > tx.data().part(0).len()) {
      printf("netifc: can't allocate %zu bytes, buffer is %d\n", len, tx.data().part(0).len());
      return zx::error(ZX_ERR_INVALID_ARGS);
    }
    return zx::ok(std::move(tx));
  }

  network::client::NetworkDeviceClient client;
  const fuchsia_hardware_network::wire::PortId port_id;
  fit::callback<void(zx_status_t)> on_error;
};

std::unique_ptr<NetdeviceIfc> g_state;

}  // namespace

DeviceBuffer::DeviceBuffer(network::client::NetworkDeviceClient::Buffer contents)
    : contents_(std::move(contents)) {}
cpp20::span<uint8_t> DeviceBuffer::data() { return contents_.data().part(0).data(); }

zx::result<DeviceBuffer> DeviceBuffer::Get(size_t len, bool block) {
  if (g_state == nullptr) {
    return zx::error(ZX_ERR_BAD_STATE);
  }
  return g_state->GetBuffer(len, block);
}

zx_status_t DeviceBuffer::Send(size_t len) {
  if (g_state == nullptr) {
    printf("%s: no state?\n", __func__);
    return ZX_ERR_BAD_STATE;
  }

  contents_.data().part(0).CapLength(static_cast<uint32_t>(len));
  contents_.data().SetFrameType(fuchsia_hardware_network::wire::FrameType::kEthernet);
  contents_.data().SetPortId(static_cast<NetdeviceIfc&>(*g_state).port_id);
  zx_status_t ret = contents_.Send();
  if (ret != ZX_OK) {
    printf("%s: Send failed: %s", __func__, zx_status_get_string(ret));
  }
  return ret;
}

int eth_add_mcast_filter(const mac_addr_t* addr) { return 0; }

zx::result<> open_netdevice(async_dispatcher_t* dispatcher,
                            fidl::ClientEnd<fuchsia_hardware_network::Device> device,
                            fuchsia_hardware_network::wire::PortId port_id,
                            fit::callback<void(zx_status_t)> on_error) {
  std::unique_ptr state =
      std::make_unique<NetdeviceIfc>(std::move(device), dispatcher, std::move(on_error), port_id);
  NetdeviceIfc& ifc = *state;
  ifc.client.SetErrorCallback([&ifc](zx_status_t status) {
    printf("netsvc: netdevice error %s\n", zx_status_get_string(status));
    ifc.on_error(status);
  });
  ifc.client.SetRxCallback([dispatcher](network::client::NetworkDeviceClient::Buffer buffer) {
    ZX_ASSERT_MSG(buffer.data().parts() == 1, "received fragmented buffer with %d parts",
                  buffer.data().parts());
    cpp20::span data = buffer.data().part(0).data();
    netifc_recv(dispatcher, data.begin(), data.size());
  });
  ifc.client.OpenSession("netsvc", [&ifc](zx_status_t status) {
    if (status != ZX_OK) {
      printf("netsvc: netdevice failed to open session: %s\n", zx_status_get_string(status));
      ifc.on_error(status);
      return;
    }
    ifc.client.AttachPort(ifc.port_id, {fuchsia_hardware_network::wire::FrameType::kEthernet},
                          [&ifc](zx_status_t status) {
                            if (status != ZX_OK) {
                              printf("netsvc: failed to attach port: %s\n",
                                     zx_status_get_string(status));
                              ifc.on_error(status);
                              return;
                            }
                          });
  });
  g_state = std::move(state);
  return zx::ok();
}

zx::result<> netifc_open(async_dispatcher_t* dispatcher, cpp17::string_view interface,
                         fit::callback<void(zx_status_t)> on_error) {
  zx::result status = netifc_discover("/dev", interface);
  if (status.is_error()) {
    printf("netifc: failed to discover interface %s\n", status.status_string());
    return status.take_error();
  }
  NetdeviceInterface& netdevice = status.value();

  {
    zx::result status = open_netdevice(dispatcher, std::move(netdevice.device), netdevice.port_id,
                                       std::move(on_error));
    if (status.is_error()) {
      return status.take_error();
    }
  }
  ip6_init(netdevice.mac, false);
  return zx::ok();
}

void netifc_close() { g_state = nullptr; }
