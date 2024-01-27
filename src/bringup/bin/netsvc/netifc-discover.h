// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_BRINGUP_BIN_NETSVC_NETIFC_DISCOVER_H_
#define SRC_BRINGUP_BIN_NETSVC_NETIFC_DISCOVER_H_

#include <fidl/fuchsia.hardware.network/cpp/wire.h>
#include <lib/stdcompat/string_view.h>
#include <lib/zx/channel.h>
#include <lib/zx/result.h>

#include "src/bringup/bin/netsvc/inet6.h"

struct NetdeviceInterface {
  fidl::ClientEnd<fuchsia_hardware_network::Device> device;
  fuchsia_hardware_network::wire::PortId port_id;
  mac_addr_t mac;
};

zx::result<NetdeviceInterface> netifc_discover(const std::string& devdir,
                                               cpp17::string_view topological_path);

#endif  // SRC_BRINGUP_BIN_NETSVC_NETIFC_DISCOVER_H_
