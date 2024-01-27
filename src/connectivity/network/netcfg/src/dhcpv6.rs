// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::{
    boxed::Box,
    collections::{HashMap, HashSet},
    pin::Pin,
};

use fidl_fuchsia_net as fnet;
use fidl_fuchsia_net_dhcpv6 as fnet_dhcpv6;
use fidl_fuchsia_net_name as fnet_name;
use fuchsia_zircon as zx;

use anyhow::Context as _;
use async_utils::{
    hanging_get::client::HangingGetStream,
    stream::{StreamMap, Tagged},
};
use dns_server_watcher::{DnsServers, DnsServersUpdateSource};
use futures::{
    future::TryFutureExt as _,
    stream::{BoxStream, Stream, TryStreamExt as _},
};

use crate::errors::{self, ContextExt as _};
use crate::{dns, DnsServerWatchers};

#[derive(Copy, Clone, Debug, PartialEq)]
pub(super) struct PrefixOnInterface {
    interface_id: u64,
    prefix: net_types::ip::Subnet<net_types::ip::Ipv6Addr>,
    lifetimes: Lifetimes,
}

pub(super) type Prefixes = HashMap<net_types::ip::Subnet<net_types::ip::Ipv6Addr>, Lifetimes>;
pub(super) type InterfaceIdTaggedPrefixesStream = Tagged<u64, PrefixesStream>;
pub(super) type PrefixesStreamMap = StreamMap<u64, InterfaceIdTaggedPrefixesStream>;

#[derive(Debug)]
pub(super) struct ClientState {
    pub(super) sockaddr: fnet::Ipv6SocketAddress,
    pub(super) prefixes: Prefixes,
}

impl ClientState {
    pub(super) fn new(sockaddr: fnet::Ipv6SocketAddress) -> Self {
        Self { sockaddr, prefixes: Default::default() }
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub(super) struct Lifetimes {
    preferred_until: zx::Time,
    valid_until: zx::Time,
}

impl Into<fnet_dhcpv6::Lifetimes> for Lifetimes {
    fn into(self) -> fnet_dhcpv6::Lifetimes {
        let Self { preferred_until, valid_until } = self;
        fnet_dhcpv6::Lifetimes {
            preferred_until: preferred_until.into_nanos(),
            valid_until: valid_until.into_nanos(),
        }
    }
}

pub(super) type PrefixesStream =
    HangingGetStream<fnet_dhcpv6::ClientProxy, Vec<fnet_dhcpv6::Prefix>>;

pub(super) fn from_fidl_prefixes(
    fidl_prefixes: &[fnet_dhcpv6::Prefix],
) -> Result<Prefixes, anyhow::Error> {
    let prefixes = fidl_prefixes
        .iter()
        .map(
            |&fnet_dhcpv6::Prefix {
                 prefix:
                     fnet::Ipv6AddressWithPrefix { addr: fnet::Ipv6Address { addr }, prefix_len },
                 lifetimes: fnet_dhcpv6::Lifetimes { valid_until, preferred_until },
             }| {
                let subnet = net_types::ip::Subnet::new(
                    net_types::ip::Ipv6Addr::from_bytes(addr),
                    prefix_len,
                )
                .map_err(|e| anyhow::anyhow!("subnet parsing error: {:?}", e))?;
                if valid_until == 0 {
                    return Err(anyhow::anyhow!(
                        "received DHCPv6 prefix {:?} with valid-until time of 0",
                        subnet
                    ));
                }
                if preferred_until == 0 {
                    return Err(anyhow::anyhow!(
                        "received DHCPv6 prefix {:?} with preferred-until time of 0",
                        subnet
                    ));
                }
                Ok((
                    subnet,
                    Lifetimes {
                        valid_until: zx::Time::from_nanos(valid_until),
                        preferred_until: zx::Time::from_nanos(preferred_until),
                    },
                ))
            },
        )
        .collect::<Result<Prefixes, _>>()?;
    if prefixes.len() != fidl_prefixes.len() {
        return Err(anyhow::anyhow!(
            "DHCPv6 prefixes {:?} contains duplicate prefix",
            fidl_prefixes
        ));
    }
    Ok(prefixes)
}

/// Start a DHCPv6 client for the specified host interface.
pub(super) fn start_client(
    dhcpv6_client_provider: &fnet_dhcpv6::ClientProviderProxy,
    interface_id: u64,
    sockaddr: fnet::Ipv6SocketAddress,
    prefix_delegation_config: Option<fnet_dhcpv6::PrefixDelegationConfig>,
) -> Result<
    (impl Stream<Item = Result<Vec<fnet_name::DnsServer_>, fidl::Error>>, PrefixesStream),
    errors::Error,
> {
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

    let dns_servers_stream = futures::stream::try_unfold(client.clone(), move |proxy| {
        proxy.watch_servers().map_ok(move |s| Some((s, proxy)))
    });
    let prefixes_stream =
        PrefixesStream::new_eager_with_fn_ptr(client, fnet_dhcpv6::ClientProxy::watch_prefixes);

    Ok((dns_servers_stream, prefixes_stream))
}

fn get_suitable_dhcpv6_prefix(
    current_prefix: Option<PrefixOnInterface>,
    interface_states: &HashMap<u64, crate::InterfaceState>,
    allowed_upstream_device_classes: &HashSet<crate::DeviceClass>,
    interface_config: AcquirePrefixInterfaceConfig,
) -> Option<PrefixOnInterface> {
    if let Some(PrefixOnInterface { interface_id, prefix, lifetimes: _ }) = current_prefix {
        let crate::InterfaceState { config, control: _, device_class: _ } =
            interface_states.get(&interface_id).unwrap_or_else(|| {
                panic!(
                    "interface {} cannot be found but provides current prefix = {:?}",
                    interface_id, current_prefix,
                )
            });
        match config {
            crate::InterfaceConfigState::Host(crate::HostInterfaceState {
                dhcpv6_client_state,
                dhcpv6_pd_config: _,
            }) => {
                let ClientState { prefixes, sockaddr: _ } =
                    dhcpv6_client_state.as_ref().unwrap_or_else(|| {
                        panic!(
                            "interface {} doesn't have DHCPv6 client \
                            but provides prefix returned to PrefixControl",
                            interface_id
                        )
                    });
                if let Some(lifetimes) = prefixes.get(&prefix) {
                    return Some(PrefixOnInterface { interface_id, prefix, lifetimes: *lifetimes });
                }
            }
            crate::InterfaceConfigState::WlanAp(wlan_ap_state) => {
                panic!(
                    "interface {} not expected to be WLAN AP with state: {:?}",
                    interface_id, wlan_ap_state,
                );
            }
        }
    }

    interface_states
        .iter()
        .filter_map(|(interface_id, crate::InterfaceState { config, device_class, control: _ })| {
            let prefixes = match config {
                crate::InterfaceConfigState::Host(crate::HostInterfaceState {
                    dhcpv6_client_state,
                    dhcpv6_pd_config: _,
                }) => {
                    if let Some(ClientState { prefixes, sockaddr: _ }) = dhcpv6_client_state {
                        prefixes
                    } else {
                        return None;
                    }
                }
                crate::InterfaceConfigState::WlanAp(crate::WlanApInterfaceState {}) => {
                    return None;
                }
            };
            match interface_config {
                AcquirePrefixInterfaceConfig::Upstreams => {
                    allowed_upstream_device_classes.contains(&device_class)
                }
                AcquirePrefixInterfaceConfig::Id(want_id) => *interface_id == want_id,
            }
            .then(|| {
                prefixes.iter().map(|(&prefix, &lifetimes)| PrefixOnInterface {
                    interface_id: *interface_id,
                    prefix,
                    lifetimes,
                })
            })
        })
        .flatten()
        .max_by(
            |PrefixOnInterface {
                 interface_id: _,
                 prefix: _,
                 lifetimes:
                     Lifetimes { preferred_until: preferred_until1, valid_until: valid_until1 },
             },
             PrefixOnInterface {
                 interface_id: _,
                 prefix: _,
                 lifetimes:
                     Lifetimes { preferred_until: preferred_until2, valid_until: valid_until2 },
             }| {
                // Prefer prefixes with the highest preferred lifetime then
                // valid lifetime.
                (preferred_until1, valid_until1).cmp(&(preferred_until2, valid_until2))
            },
        )
}

pub(super) fn maybe_send_watch_prefix_response(
    interface_states: &HashMap<u64, crate::InterfaceState>,
    allowed_upstream_device_classes: &HashSet<crate::DeviceClass>,
    prefix_provider_handler: Option<&mut PrefixProviderHandler>,
) -> Result<(), anyhow::Error> {
    let PrefixProviderHandler {
        current_prefix,
        interface_config,
        preferred_prefix_len: _,
        watch_prefix_responder,
        prefix_control_request_stream: _,
    } = if let Some(handler) = prefix_provider_handler {
        handler
    } else {
        return Ok(());
    };

    let new_prefix = get_suitable_dhcpv6_prefix(
        *current_prefix,
        interface_states,
        allowed_upstream_device_classes,
        *interface_config,
    );
    if new_prefix == *current_prefix {
        return Ok(());
    }

    if let Some(responder) = watch_prefix_responder.take() {
        responder
            .send(&mut new_prefix.map_or(
                fnet_dhcpv6::PrefixEvent::Unassigned(fnet_dhcpv6::Empty),
                |PrefixOnInterface { interface_id: _, prefix, lifetimes }| {
                    fnet_dhcpv6::PrefixEvent::Assigned(fnet_dhcpv6::Prefix {
                        prefix: fnet::Ipv6AddressWithPrefix {
                            addr: fnet::Ipv6Address { addr: prefix.network().ipv6_bytes() },
                            prefix_len: prefix.prefix(),
                        },
                        lifetimes: lifetimes.into(),
                    })
                },
            ))
            .context("failed to send PrefixControl.WatchPrefix response")?;
        *current_prefix = new_prefix;
    }

    Ok(())
}

/// Stops the DHCPv6 client running on the specified host interface.
///
/// Any DNS servers learned by the client will be cleared.
pub(super) async fn stop_client(
    lookup_admin: &fnet_name::LookupAdminProxy,
    dns_servers: &mut DnsServers,
    interface_id: u64,
    watchers: &mut DnsServerWatchers<'_>,
    prefixes_streams: &mut PrefixesStreamMap,
) -> Result<(), errors::Error> {
    let source = DnsServersUpdateSource::Dhcpv6 { interface_id };

    // Dropping all fuchsia.net.dhcpv6/Client proxies will stop the DHCPv6 client.
    let _: Pin<Box<BoxStream<'_, _>>> = watchers.remove(&source).unwrap_or_else(|| {
        unreachable!(
            "DNS server watchers must contain key {:?}; interface_id={}",
            source, interface_id
        )
    });
    let _: Pin<Box<InterfaceIdTaggedPrefixesStream>> =
        prefixes_streams.remove(&interface_id).unwrap_or_else(|| {
            unreachable!(
                "DHCPv6 prefixes streams must hold stream for interface_id={}",
                interface_id
            )
        });

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
    /// Interfaces configured to perform PD on.
    pub(super) interface_config: AcquirePrefixInterfaceConfig,
    pub(super) current_prefix: Option<PrefixOnInterface>,
}

impl PrefixProviderHandler {
    pub(super) fn try_next_prefix_control_request(
        &mut self,
    ) -> futures::stream::TryNext<'_, fnet_dhcpv6::PrefixControlRequestStream> {
        self.prefix_control_request_stream.try_next()
    }
}

#[cfg(test)]
mod tests {
    use fuchsia_zircon as zx;

    use net_declare::{fidl_socket_addr_v6, net_subnet_v6};
    use test_case::test_case;

    use crate::{DeviceClass, HostInterfaceState, InterfaceConfigState, InterfaceState};

    use super::*;

    const ALLOWED_UPSTREAM_DEVICE_CLASS: crate::DeviceClass = crate::DeviceClass::Ethernet;
    const DISALLOWED_UPSTREAM_DEVICE_CLASS: crate::DeviceClass = crate::DeviceClass::Virtual;
    const LIFETIMES: Lifetimes = Lifetimes {
        preferred_until: zx::Time::from_nanos(123_000_000_000),
        valid_until: zx::Time::from_nanos(456_000_000_000),
    };
    const RENEWED_LIFETIMES: Lifetimes = Lifetimes {
        preferred_until: zx::Time::from_nanos(777_000_000_000),
        valid_until: zx::Time::from_nanos(888_000_000_000),
    };

    impl InterfaceState {
        fn new_host_with_state(
            control: fidl_fuchsia_net_interfaces_ext::admin::Control,
            device_class: DeviceClass,
            dhcpv6_pd_config: Option<fnet_dhcpv6::PrefixDelegationConfig>,
            dhcpv6_client_state: Option<ClientState>,
        ) -> Self {
            Self {
                control,
                config: InterfaceConfigState::Host(HostInterfaceState {
                    dhcpv6_client_state,
                    dhcpv6_pd_config,
                }),
                device_class,
            }
        }
    }

    #[test_case(
        None,
        [
            (
                DISALLOWED_UPSTREAM_DEVICE_CLASS,
                Some(HashMap::from([(net_subnet_v6!("abcd::/64"), LIFETIMES)])),
            )
        ].into_iter(),
        AcquirePrefixInterfaceConfig::Upstreams,
        None;
        "not_upstream"
    )]
    #[test_case(
        None,
        [
            (
                ALLOWED_UPSTREAM_DEVICE_CLASS,
                Some(HashMap::from([(net_subnet_v6!("abcd::/64"), LIFETIMES)])),
            )
        ].into_iter(),
        AcquirePrefixInterfaceConfig::Upstreams,
        Some(PrefixOnInterface {
            interface_id: 1,
            prefix: net_subnet_v6!("abcd::/64"),
            lifetimes: LIFETIMES,
        });
        "none_to_some"
    )]
    #[test_case(
        Some(PrefixOnInterface {
            interface_id: 1,
            prefix: net_subnet_v6!("abcd::/64"),
            lifetimes: LIFETIMES,
        }),
        [
            (
                ALLOWED_UPSTREAM_DEVICE_CLASS,
                Some(HashMap::from([(net_subnet_v6!("abcd::/64"), LIFETIMES)])),
            )
        ].into_iter(),
        AcquirePrefixInterfaceConfig::Upstreams,
        Some(PrefixOnInterface {
            interface_id: 1,
            prefix: net_subnet_v6!("abcd::/64"),
            lifetimes: LIFETIMES,
        });
        "same"
    )]
    #[test_case(
        Some(PrefixOnInterface {
            interface_id: 1,
            prefix: net_subnet_v6!("abcd::/64"),
            lifetimes: LIFETIMES,
        }),
        [
            (
                ALLOWED_UPSTREAM_DEVICE_CLASS,
                Some(HashMap::from([(net_subnet_v6!("abcd::/64"), RENEWED_LIFETIMES)])),
            )
        ].into_iter(),
        AcquirePrefixInterfaceConfig::Upstreams,
        Some(PrefixOnInterface {
            interface_id: 1,
            prefix: net_subnet_v6!("abcd::/64"),
            lifetimes: RENEWED_LIFETIMES,
        });
        "lifetime_changed"
    )]
    #[test_case(
        Some(PrefixOnInterface {
            interface_id: 1,
            prefix: net_subnet_v6!("abcd::/64"),
            lifetimes: LIFETIMES,
        }),
        [
            (
                ALLOWED_UPSTREAM_DEVICE_CLASS,
                Some(HashMap::new()),
            ),
            (
                ALLOWED_UPSTREAM_DEVICE_CLASS,
                Some(HashMap::from([(net_subnet_v6!("efff::/64"), RENEWED_LIFETIMES)])),
            )
        ].into_iter(),
        AcquirePrefixInterfaceConfig::Upstreams,
        Some(PrefixOnInterface {
            interface_id: 2,
            prefix: net_subnet_v6!("efff::/64"),
            lifetimes: RENEWED_LIFETIMES,
        });
        "different_interface"
    )]
    #[fuchsia_async::run_singlethreaded(test)]
    async fn get_suitable_dhcpv6_prefix(
        current_prefix: Option<PrefixOnInterface>,
        interface_state_iter: impl IntoIterator<Item = (crate::DeviceClass, Option<Prefixes>)>,
        interface_config: AcquirePrefixInterfaceConfig,
        want: Option<PrefixOnInterface>,
    ) {
        let interface_states = (1..)
            .zip(interface_state_iter.into_iter().map(|(device_class, prefixes)| {
                let (control, _control_server_end) =
                    fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints()
                        .expect("create endpoints");
                InterfaceState::new_host_with_state(
                    control,
                    device_class,
                    None,
                    prefixes.map(|prefixes| ClientState {
                        sockaddr: fidl_socket_addr_v6!("[fe80::1]:546"),
                        prefixes: prefixes,
                    }),
                )
            }))
            .collect();
        let allowed_upstream_device_classes = HashSet::from([ALLOWED_UPSTREAM_DEVICE_CLASS]);
        assert_eq!(
            super::get_suitable_dhcpv6_prefix(
                current_prefix,
                &interface_states,
                &allowed_upstream_device_classes,
                interface_config,
            ),
            want
        );
    }
}
