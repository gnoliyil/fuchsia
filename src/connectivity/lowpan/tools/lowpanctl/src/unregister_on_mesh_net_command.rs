// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::context::LowpanCtlContext;
use crate::prelude::*;
use fidl_fuchsia_net::Ipv6Address;
use fidl_fuchsia_net::Ipv6AddressWithPrefix as Ipv6Subnet;

/// Contains the arguments decoded for the `unregister-on-mesh-net` command.
#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "unregister-on-mesh-net")]
pub struct UnregisterOnMeshNetCommand {
    /// ipv6 prefix (always a /64)
    #[argh(positional)]
    pub addr: std::net::Ipv6Addr,
}

impl UnregisterOnMeshNetCommand {
    pub async fn exec(&self, context: &mut LowpanCtlContext) -> Result<(), Error> {
        let device_route = context.get_default_device_route_proxy().await?;
        let prefix_len = 64;
        let subnet = Ipv6Subnet { addr: Ipv6Address { addr: self.addr.octets() }, prefix_len };

        device_route
            .unregister_on_mesh_prefix(&subnet)
            .await
            .context("Unable to send unregister_on_mesh_prefix command")
    }
}
