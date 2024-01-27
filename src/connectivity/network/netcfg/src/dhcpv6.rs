// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_net as fnet;
use fidl_fuchsia_net_dhcpv6 as fnet_dhcpv6;
use fidl_fuchsia_net_name as fnet_name;

use anyhow::Context as _;
use dns_server_watcher::{DnsServers, DnsServersUpdateSource};
use futures::future::TryFutureExt as _;
use futures::stream::Stream;

use crate::errors::{self, ContextExt as _};
use crate::{dns, DnsServerWatchers};

/// Start a DHCPv6 client for the specified host interface.
pub(super) fn start_client(
    dhcpv6_client_provider: &fnet_dhcpv6::ClientProviderProxy,
    interface_id: u64,
    sockaddr: fnet::Ipv6SocketAddress,
    prefix_delegation_config: Option<fnet_dhcpv6::PrefixDelegationConfig>,
) -> Result<impl Stream<Item = Result<Vec<fnet_name::DnsServer_>, fidl::Error>>, errors::Error> {
    let params = fnet_dhcpv6::NewClientParams {
        interface_id: Some(interface_id),
        address: Some(sockaddr),
        config: Some(fnet_dhcpv6::ClientConfig {
            information_config: Some(fnet_dhcpv6::InformationConfig {
                dns_servers: Some(true),
                ..fnet_dhcpv6::InformationConfig::EMPTY
            }),
            prefix_delegation_config,
            ..fnet_dhcpv6::ClientConfig::EMPTY
        }),
        ..fnet_dhcpv6::NewClientParams::EMPTY
    };
    let (client, server) = fidl::endpoints::create_proxy::<fnet_dhcpv6::ClientMarker>()
        .context("error creating DHCPv6 client fidl endpoints")
        .map_err(errors::Error::Fatal)?;

    // Not all environments may have a DHCPv6 client service so we consider this a
    // non-fatal error.
    let () = dhcpv6_client_provider
        .new_client(params, server)
        .context("error creating new DHCPv6 client")
        .map_err(errors::Error::NonFatal)?;

    let dns_servers_stream = futures::stream::try_unfold(client, move |proxy| {
        proxy.watch_servers().map_ok(move |s| Some((s, proxy)))
    });

    Ok(dns_servers_stream)
}

/// Stops the DHCPv6 client running on the specified host interface.
///
/// Any DNS servers learned by the client will be cleared.
pub(super) async fn stop_client(
    lookup_admin: &fnet_name::LookupAdminProxy,
    dns_servers: &mut DnsServers,
    interface_id: u64,
    watchers: &mut DnsServerWatchers<'_>,
) -> Result<(), errors::Error> {
    let source = DnsServersUpdateSource::Dhcpv6 { interface_id };

    // Dropping the client end of the Client interface should stop the
    // DHCPv6 client.
    if watchers.remove(&source).is_none() {
        // Should never happen as we only set the DHCPv6 client
        // socket address if we successfully create a client.
        return Err(errors::Error::Fatal(anyhow::anyhow!(
            "expected to remove a DNS watcher for host interface with id={}",
            interface_id
        )));
    }

    dns::update_servers(lookup_admin, dns_servers, source, vec![])
        .await
        .context("error clearing DNS servers")
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) enum AcquirePrefixInterfaceConfig {
    Upstreams,
    Id(u64),
}

pub(super) struct PrefixProviderHandler {
    pub(super) prefix_control_request_stream: fnet_dhcpv6::PrefixControlRequestStream,
    pub(super) watch_prefix_responder: Option<fnet_dhcpv6::PrefixControlWatchPrefixResponder>,
    pub(super) preferred_prefix_len: Option<u8>,
    // TODO(https://fxbug.dev/114770): Use the interface config to know the
    // interfaces over which prefixes acquired over DHCPv6 PD should be
    // returned to the client.
    /// Interfaces configured to perform PD on.
    pub(super) _interface_config: AcquirePrefixInterfaceConfig,
}
