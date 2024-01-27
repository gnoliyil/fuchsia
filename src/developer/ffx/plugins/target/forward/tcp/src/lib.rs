// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{Context as _, Result};
use errors::ffx_bail;
use ffx_config::keys::TARGET_DEFAULT_KEY;
use ffx_core::ffx_plugin;
use ffx_target_forward_tcp_args::TcpCommand;
use fidl_fuchsia_developer_ffx as ffx;

#[ffx_plugin(ffx::TunnelProxy = "daemon::protocol")]
pub async fn forward_tcp(forward_port: ffx::TunnelProxy, cmd: TcpCommand) -> Result<()> {
    let target: Option<String> =
        ffx_config::get(TARGET_DEFAULT_KEY).await.context("getting default target from config")?;
    let target = if let Some(target) = target {
        target
    } else {
        ffx_bail!("Specify a target or configure a default target")
    };

    forward_tcp_impl(&target, forward_port, cmd).await
}

pub async fn forward_tcp_impl(
    target: &str,
    forward_port: ffx::TunnelProxy,
    cmd: TcpCommand,
) -> Result<()> {
    match forward_port.forward_port(target, &cmd.host_address, &cmd.target_address).await? {
        Ok(()) => Ok(()),
        Err(ffx::TunnelError::CouldNotListen) => {
            ffx_bail!("Could not listen on address {:?}", cmd.host_address)
        }
        Err(e) => Err(anyhow::anyhow!("Unexpected error: {:?}", e)),
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use fidl_fuchsia_net as net;

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_forward() {
        let host_address_test = net::SocketAddress::Ipv4(net::Ipv4SocketAddress {
            address: net::Ipv4Address { addr: [1, 2, 3, 4] },
            port: 1234,
        });
        let target_address_test = net::SocketAddress::Ipv4(net::Ipv4SocketAddress {
            address: net::Ipv4Address { addr: [5, 6, 7, 8] },
            port: 5678,
        });

        let forward_port_proxy = setup_fake_forward_port({
            let host_address_test = host_address_test.clone();
            let target_address_test = target_address_test.clone();
            move |req| match req {
                ffx::TunnelRequest::ForwardPort {
                    target,
                    host_address,
                    target_address,
                    responder,
                } => {
                    assert_eq!("dummy_target", target);
                    assert_eq!(host_address_test, host_address);
                    assert_eq!(target_address_test, target_address);
                    responder.send(Ok(())).context("sending response").expect("should send")
                }
                other => panic!("Unexpected request: {:?}", other),
            }
        });

        forward_tcp_impl(
            "dummy_target",
            forward_port_proxy,
            TcpCommand {
                host_address: host_address_test.clone(),
                target_address: target_address_test.clone(),
            },
        )
        .await
        .expect("should succeed");
    }
}
