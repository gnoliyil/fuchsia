// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Utilities for interacting with DHCPv4 client/server during integration
//! tests.

#![deny(missing_docs)]

use dhcpv4::protocol::IntoFidlExt as _;
use fuchsia_zircon as zx;
use futures::StreamExt as _;
use net_declare::{fidl_ip_v4, std_ip_v4};

/// Encapsulates a minimal configuration needed to test a DHCP client/server combination.
pub struct TestConfig {
    /// Server IP address.
    pub server_addr: fidl_fuchsia_net::Ipv4Address,

    /// Address pool for the DHCP server.
    pub managed_addrs: dhcpv4::configuration::ManagedAddresses,
}

impl TestConfig {
    /// The IPv4 address a client will acquire from the server.
    pub fn expected_acquired(&self) -> fidl_fuchsia_net::Subnet {
        let Self {
            server_addr: _,
            managed_addrs:
                dhcpv4::configuration::ManagedAddresses { mask, pool_range_start, pool_range_stop: _ },
        } = self;
        fidl_fuchsia_net::Subnet {
            addr: fidl_fuchsia_net::IpAddress::Ipv4(pool_range_start.into_fidl()),
            prefix_len: mask.ones(),
        }
    }

    /// The IPv4 address and prefix the server will assign to itself.
    pub fn server_addr_with_prefix(&self) -> fidl_fuchsia_net::Ipv4AddressWithPrefix {
        let Self {
            server_addr,
            managed_addrs:
                dhcpv4::configuration::ManagedAddresses {
                    mask,
                    pool_range_start: _,
                    pool_range_stop: _,
                },
        } = self;
        fidl_fuchsia_net::Ipv4AddressWithPrefix { addr: *server_addr, prefix_len: mask.ones() }
    }

    /// The FIDL parameters a DHCPv4 server should be configured with.
    pub fn dhcp_parameters(&self) -> Vec<fidl_fuchsia_net_dhcp::Parameter> {
        let Self { server_addr, managed_addrs } = self;
        vec![
            fidl_fuchsia_net_dhcp::Parameter::IpAddrs(vec![*server_addr]),
            fidl_fuchsia_net_dhcp::Parameter::AddressPool(managed_addrs.into_fidl()),
        ]
    }
}

/// Default configuration.
pub const DEFAULT_TEST_CONFIG: TestConfig = TestConfig {
    server_addr: fidl_ip_v4!("192.168.0.1"),
    managed_addrs: dhcpv4::configuration::ManagedAddresses {
        // We know this is safe because 25 is less than the size of an IPv4 address in bits.
        mask: const_unwrap::const_unwrap_option(dhcpv4::configuration::SubnetMask::new(25)),
        pool_range_start: std_ip_v4!("192.168.0.2"),
        pool_range_stop: std_ip_v4!("192.168.0.5"),
    },
};

/// Set DHCPv4 server settings.
pub async fn set_server_settings(
    dhcp_server: &fidl_fuchsia_net_dhcp::Server_Proxy,
    parameters: impl IntoIterator<Item = fidl_fuchsia_net_dhcp::Parameter>,
    options: impl IntoIterator<Item = fidl_fuchsia_net_dhcp::Option_>,
) {
    let parameters = futures::stream::iter(parameters.into_iter()).for_each_concurrent(
        None,
        |mut parameter| async move {
            dhcp_server
                .set_parameter(&mut parameter)
                .await
                .expect("failed to call dhcp/Server.SetParameter")
                .map_err(zx::Status::from_raw)
                .unwrap_or_else(|e| {
                    panic!("dhcp/Server.SetParameter({:?}) returned error: {:?}", parameter, e)
                })
        },
    );
    let options = futures::stream::iter(options.into_iter()).for_each_concurrent(
        None,
        |mut option| async move {
            dhcp_server
                .set_option(&mut option)
                .await
                .expect("failed to call dhcp/Server.SetOption")
                .map_err(zx::Status::from_raw)
                .unwrap_or_else(|e| {
                    panic!("dhcp/Server.SetOption({:?}) returned error: {:?}", option, e)
                })
        },
    );
    let ((), ()) = futures::future::join(parameters, options).await;
}
