// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use std::collections::HashMap;
use std::fmt::Debug;
use std::hash::Hash;

use anyhow::Context as _;
use assert_matches::assert_matches;
use fidl::endpoints::Proxy;
use fidl_fuchsia_net_ext::IntoExt;
use fidl_fuchsia_net_routes as fnet_routes;
use fidl_fuchsia_net_routes_ext as fnet_routes_ext;
use fuchsia_async::TimeoutExt;
use fuchsia_zircon_status as zx_status;
use futures::{FutureExt, StreamExt};
use net_declare::{fidl_ip, fidl_ip_v4, fidl_mac, fidl_subnet, net_subnet_v4, net_subnet_v6};
use net_types::{
    self,
    ip::{GenericOverIp, Ip, IpInvariant, Ipv4Addr, Ipv6Addr},
};
use netstack_testing_common::{
    interfaces,
    realms::{Netstack, Netstack2, TestRealmExt as _, TestSandboxExt as _},
};
use netstack_testing_macros::netstack_test;

async fn resolve(
    routes: &fidl_fuchsia_net_routes::StateProxy,
    mut remote: fidl_fuchsia_net::IpAddress,
) -> fidl_fuchsia_net_routes::Resolved {
    routes
        .resolve(&mut remote)
        .await
        .expect("routes/State.Resolve FIDL error")
        .map_err(fuchsia_zircon::Status::from_raw)
        .context("routes/State.Resolve error")
        .expect("failed to resolve remote")
}

#[netstack_test]
async fn resolve_loopback_route(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("failed to create sandbox");
    let realm =
        sandbox.create_netstack_realm::<Netstack2, _>(name).expect("failed to create realm");
    let routes = realm
        .connect_to_protocol::<fidl_fuchsia_net_routes::StateMarker>()
        .expect("failed to connect to routes/State");
    let routes = &routes;

    let test = |remote: fidl_fuchsia_net::IpAddress, source: fidl_fuchsia_net::IpAddress| async move {
        assert_eq!(
            resolve(routes, remote).await,
            fidl_fuchsia_net_routes::Resolved::Direct(fidl_fuchsia_net_routes::Destination {
                address: Some(remote),
                mac: None,
                interface_id: Some(1),
                source_address: Some(source),
                ..fidl_fuchsia_net_routes::Destination::EMPTY
            }),
        );
    };

    test(fidl_ip!("127.0.0.1"), fidl_ip!("127.0.0.1")).await;
    test(fidl_ip!("::1"), fidl_ip!("::1")).await;
}

#[netstack_test]
async fn resolve_route(name: &str) {
    const GATEWAY_IP_V4: fidl_fuchsia_net::Subnet = fidl_subnet!("192.168.0.1/24");
    const GATEWAY_IP_V6: fidl_fuchsia_net::Subnet = fidl_subnet!("3080::1/64");
    const GATEWAY_MAC: fidl_fuchsia_net::MacAddress = fidl_mac!("02:01:02:03:04:05");
    const HOST_IP_V4: fidl_fuchsia_net::Subnet = fidl_subnet!("192.168.0.2/24");
    const HOST_IP_V6: fidl_fuchsia_net::Subnet = fidl_subnet!("3080::2/64");

    let sandbox = netemul::TestSandbox::new().expect("failed to create sandbox");
    let net = sandbox.create_network("net").await.expect("failed to create network");

    // Configure a host.
    let host = sandbox
        .create_netstack_realm::<Netstack2, _>(format!("{}_host", name))
        .expect("failed to create client realm");

    let host_stack = host
        .connect_to_protocol::<fidl_fuchsia_net_stack::StackMarker>()
        .expect("failed to connect to netstack");

    let host_ep = host.join_network(&net, "host").await.expect("host failed to join network");
    host_ep.add_address_and_subnet_route(HOST_IP_V4).await.expect("configure address");
    let _host_address_state_provider = interfaces::add_subnet_address_and_route_wait_assigned(
        &host_ep,
        HOST_IP_V6,
        fidl_fuchsia_net_interfaces_admin::AddressParameters::EMPTY,
    )
    .await
    .expect("add subnet address and route");

    // Configure a gateway.
    let gateway = sandbox
        .create_netstack_realm::<Netstack2, _>(format!("{}_gateway", name))
        .expect("failed to create server realm");

    let gateway_ep = gateway
        .join_network_with(
            &net,
            "gateway",
            netemul::new_endpoint_config(netemul::DEFAULT_MTU, Some(GATEWAY_MAC)),
            Default::default(),
        )
        .await
        .expect("gateway failed to join network");
    gateway_ep.add_address_and_subnet_route(GATEWAY_IP_V4).await.expect("configure address");
    let _gateway_address_state_provider = interfaces::add_subnet_address_and_route_wait_assigned(
        &gateway_ep,
        GATEWAY_IP_V6,
        fidl_fuchsia_net_interfaces_admin::AddressParameters::EMPTY,
    )
    .await
    .expect("add subnet address and route");

    let routes = host
        .connect_to_protocol::<fidl_fuchsia_net_routes::StateMarker>()
        .expect("failed to connect to routes/State");
    let routes = &routes;

    let resolve_fails = move |mut remote: fidl_fuchsia_net::IpAddress| async move {
        assert_eq!(
            routes
                .resolve(&mut remote)
                .await
                .expect("resolve FIDL error")
                .map_err(fuchsia_zircon::Status::from_raw),
            Err(fuchsia_zircon::Status::ADDRESS_UNREACHABLE)
        )
    };

    let interface_id = host_ep.id();
    let host_stack = &host_stack;

    let do_test = |gateway: fidl_fuchsia_net::IpAddress,
                   unreachable_peer: fidl_fuchsia_net::IpAddress,
                   unspecified: fidl_fuchsia_net::IpAddress,
                   public_ip: fidl_fuchsia_net::IpAddress,
                   source_address: fidl_fuchsia_net::IpAddress| async move {
        let gateway_node = || fidl_fuchsia_net_routes::Destination {
            address: Some(gateway),
            mac: Some(GATEWAY_MAC),
            interface_id: Some(interface_id),
            source_address: Some(source_address),
            ..fidl_fuchsia_net_routes::Destination::EMPTY
        };

        // Start asking for a route for something that is directly accessible on the
        // network.
        let resolved = resolve(routes, gateway).await;
        assert_eq!(resolved, fidl_fuchsia_net_routes::Resolved::Direct(gateway_node()));
        // Fails if MAC unreachable.
        resolve_fails(unreachable_peer).await;
        // Fails if route unreachable.
        resolve_fails(public_ip).await;

        // Install a default route and try to resolve through the gateway.
        let () = host_stack
            .add_forwarding_entry(&mut fidl_fuchsia_net_stack::ForwardingEntry {
                subnet: fidl_fuchsia_net::Subnet { addr: unspecified, prefix_len: 0 },
                device_id: interface_id,
                next_hop: Some(Box::new(gateway)),
                metric: 100,
            })
            .await
            .expect("call add_route")
            .expect("add route");

        // Resolve a public IP again and check that we get the gateway response.
        let resolved = resolve(routes, public_ip).await;
        assert_eq!(resolved, fidl_fuchsia_net_routes::Resolved::Gateway(gateway_node()));
        // And that the unspecified address resolves to the gateway node as well.
        let resolved = resolve(routes, unspecified).await;
        assert_eq!(resolved, fidl_fuchsia_net_routes::Resolved::Gateway(gateway_node()));
    };

    do_test(
        GATEWAY_IP_V4.addr,
        fidl_ip!("192.168.0.3"),
        fidl_ip!("0.0.0.0"),
        fidl_ip!("8.8.8.8"),
        HOST_IP_V4.addr,
    )
    .await;

    do_test(
        GATEWAY_IP_V6.addr,
        fidl_ip!("3080::3"),
        fidl_ip!("::"),
        fidl_ip!("2001:4860:4860::8888"),
        HOST_IP_V6.addr,
    )
    .await;
}

#[netstack_test]
async fn resolve_default_route_while_dhcp_is_running(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("failed to create sandbox");
    let net = sandbox.create_network("net").await.expect("failed to create network");

    // Configure a host.
    let realm =
        sandbox.create_netstack_realm::<Netstack2, _>(name).expect("failed to create client realm");

    let stack = realm
        .connect_to_protocol::<fidl_fuchsia_net_stack::StackMarker>()
        .expect("failed to connect to netstack");

    let ep = realm.join_network(&net, "host").await.expect("host failed to join network");
    ep.start_dhcp().await.expect("failed to start DHCP");

    let routes = realm
        .connect_to_protocol::<fidl_fuchsia_net_routes::StateMarker>()
        .expect("failed to connect to routes/State");

    let resolved = routes
        .resolve(&mut fidl_ip!("0.0.0.0"))
        .await
        .expect("routes/State.Resolve FIDL error")
        .map_err(fuchsia_zircon::Status::from_raw);

    assert_eq!(resolved, Err(fuchsia_zircon::Status::ADDRESS_UNREACHABLE));

    const EP_ADDR: fidl_fuchsia_net::Ipv4Address = fidl_ip_v4!("192.168.0.3");
    const PREFIX_LEN: u8 = 24;
    const GATEWAY_ADDR: fidl_fuchsia_net::IpAddress = fidl_ip!("192.168.0.1");
    const GATEWAY_MAC: fidl_fuchsia_net::MacAddress = fidl_mac!("02:01:02:03:04:05");
    const UNSPECIFIED_IP: fidl_fuchsia_net::IpAddress = fidl_ip!("0.0.0.0");

    // Configure stack statically with an address and a default route while DHCP is still running.
    let _host_address_state_provider = interfaces::add_address_wait_assigned(
        ep.control(),
        fidl_fuchsia_net::Subnet {
            addr: fidl_fuchsia_net::IpAddress::Ipv4(EP_ADDR),
            prefix_len: PREFIX_LEN,
        },
        fidl_fuchsia_net_interfaces_admin::AddressParameters::EMPTY,
    )
    .await
    .expect("add address");

    let neigh = realm
        .connect_to_protocol::<fidl_fuchsia_net_neighbor::ControllerMarker>()
        .expect("failed to connect to neighbor API");
    let () = neigh
        .add_entry(ep.id(), &mut GATEWAY_ADDR.clone(), &mut GATEWAY_MAC.clone())
        .await
        .expect("add_entry FIDL error")
        .map_err(fuchsia_zircon::Status::from_raw)
        .expect("add_entry error");

    // Install a default route and try to resolve through the gateway.
    let () = stack
        .add_forwarding_entry(&mut fidl_fuchsia_net_stack::ForwardingEntry {
            subnet: fidl_fuchsia_net::Subnet { addr: UNSPECIFIED_IP, prefix_len: 0 },
            device_id: ep.id(),
            next_hop: Some(Box::new(GATEWAY_ADDR)),
            metric: 100,
        })
        .await
        .expect("call add_route")
        .expect("add route");

    let resolved = routes
        .resolve(&mut UNSPECIFIED_IP.clone())
        .await
        .expect("routes/State.Resolve FIDL error")
        .map_err(fuchsia_zircon::Status::from_raw);

    assert_eq!(
        resolved,
        Ok(fidl_fuchsia_net_routes::Resolved::Gateway(fidl_fuchsia_net_routes::Destination {
            address: Some(GATEWAY_ADDR),
            mac: Some(GATEWAY_MAC),
            interface_id: Some(ep.id()),
            source_address: Some(fidl_fuchsia_net::IpAddress::Ipv4(EP_ADDR)),
            ..fidl_fuchsia_net_routes::Destination::EMPTY
        }))
    );
}

fn new_installed_route<I: fnet_routes_ext::FidlRouteIpExt>(
    subnet: net_types::ip::Subnet<I::Addr>,
    interface: u64,
    metric: u32,
    metric_is_inherited: bool,
) -> fnet_routes_ext::InstalledRoute<I> {
    let specified_metric = if metric_is_inherited {
        fnet_routes::SpecifiedMetric::InheritedFromInterface(fnet_routes::Empty)
    } else {
        fnet_routes::SpecifiedMetric::ExplicitMetric(metric)
    };
    fnet_routes_ext::InstalledRoute {
        route: fnet_routes_ext::Route {
            destination: subnet,
            action: fnet_routes_ext::RouteAction::Forward(fnet_routes_ext::RouteTarget {
                outbound_interface: interface,
                next_hop: None,
            }),
            properties: fnet_routes_ext::RouteProperties {
                specified_properties: fnet_routes_ext::SpecifiedRouteProperties {
                    metric: specified_metric,
                },
            },
        },
        effective_properties: fnet_routes_ext::EffectiveRouteProperties { metric: metric },
    }
}

// Asserts that two vectors contain the same entries, order independent.
fn assert_eq_unordered<T: Debug + Eq + Hash + PartialEq>(a: Vec<T>, b: Vec<T>) {
    // Converts a `Vec<T>` into a `HashMap` where the key is `T` and the value
    // is the count of occurrences of `T` in the vec.
    fn into_counted_set<T: Eq + Hash>(set: Vec<T>) -> HashMap<T, usize> {
        set.into_iter().fold(HashMap::new(), |mut map, entry| {
            *map.entry(entry).or_default() += 1;
            map
        })
    }
    assert_eq!(into_counted_set(a), into_counted_set(b));
}

// Default metric values used by the netstack when creating implicit routes.
// See `src/connectivity/network/netstack/netstack.go`.
const DEFAULT_INTERFACE_METRIC: u32 = 100;
const DEFAULT_LOW_PRIORITY_METRIC: u32 = 99999;
const DEFAULT_UNSET_METRIC: u32 = 0;

// Verifies the startup behavior of the watcher protocols; including the
// expected preinstalled routes.
#[netstack_test]
async fn watcher_existing<N: Netstack, I: net_types::ip::Ip + fnet_routes_ext::FidlRouteIpExt>(
    name: &str,
) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");

    // The routes we expected to be installed in the netstack by default.
    let loopback_id = realm
        .loopback_properties()
        .await
        .expect("failed to get loopback properties")
        .expect("loopback properties unexpectedly None")
        .id;
    #[derive(GenericOverIp)]
    struct RoutesHolder<I: Ip + fnet_routes_ext::FidlRouteIpExt>(
        Vec<fnet_routes_ext::InstalledRoute<I>>,
    );
    let RoutesHolder(expected_routes) = I::map_ip(
        IpInvariant(loopback_id),
        |IpInvariant(loopback_id)| {
            RoutesHolder(vec![
                new_installed_route(
                    net_subnet_v4!("127.0.0.0/8"),
                    loopback_id,
                    DEFAULT_INTERFACE_METRIC,
                    true,
                ),
                new_installed_route(
                    net_subnet_v4!("255.255.255.255/32"),
                    loopback_id,
                    DEFAULT_LOW_PRIORITY_METRIC,
                    false,
                ),
            ])
        },
        |IpInvariant(loopback_id)| {
            RoutesHolder(vec![
                new_installed_route(
                    net_subnet_v6!("::1/128"),
                    loopback_id,
                    DEFAULT_INTERFACE_METRIC,
                    true,
                ),
                // TODO(https://fxbug.dev/120250): Once IPv6 link-local subnet
                // routes have the correct metric, switch this to
                // `DEFAULT_INTERFACE_METRIC`.
                new_installed_route(
                    net_subnet_v6!("fe80::/64"),
                    loopback_id,
                    DEFAULT_UNSET_METRIC,
                    true,
                ),
            ])
        },
    );

    let state_proxy =
        realm.connect_to_protocol::<I::StateMarker>().expect("failed to connect to routes/State");
    let event_stream = fnet_routes_ext::event_stream_from_state::<I>(&state_proxy)
        .expect("failed to connect to routes watcher");

    futures::pin_mut!(event_stream);
    let existing = fnet_routes_ext::collect_routes_until_idle::<I, Vec<_>>(event_stream.by_ref())
        .await
        .expect("failed to collect existing routes");

    // Assert that the existing routes contain exactly the expected routes.
    assert_eq_unordered(existing, expected_routes);

    // Verify that there are no more pending events in the stream.
    event_stream
        .next()
        .map(Err)
        .on_timeout(
            fuchsia_async::Time::after(netstack_testing_common::ASYNC_EVENT_NEGATIVE_CHECK_TIMEOUT),
            || Ok(()),
        )
        .await
        .expect("Unexpected event in event stream");
}

// Declare subnet routes for tests to add/delete. These are known to not collide
// with routes implicitly installed by the Netstack.
const TEST_SUBNET_V4: net_types::ip::Subnet<Ipv4Addr> = net_subnet_v4!("192.168.0.0/24");
const TEST_SUBNET_V6: net_types::ip::Subnet<Ipv6Addr> = net_subnet_v6!("fd::/64");

// Verifies that a client-installed route is observed as `existing` if added
// before the watcher client connects.
#[netstack_test]
async fn watcher_add_before_watch<
    N: Netstack,
    I: net_types::ip::Ip + fnet_routes_ext::FidlRouteIpExt,
>(
    name: &str,
) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let device = sandbox.create_endpoint(name).await.expect("create endpoint");
    let interface = device.into_interface_in_realm(&realm).await.expect("add endpoint to Netstack");

    let subnet: net_types::ip::Subnet<I::Addr> =
        I::map_ip((), |()| TEST_SUBNET_V4, |()| TEST_SUBNET_V6);

    // Add a test route.
    interface.add_subnet_route(subnet.into_ext()).await.expect("failed to add route");

    // Connect to the watcher protocol.
    let state_proxy =
        realm.connect_to_protocol::<I::StateMarker>().expect("failed to connect to routes/State");
    let event_stream = fnet_routes_ext::event_stream_from_state::<I>(&state_proxy)
        .expect("failed to connect to routes watcher");

    // Verify that the previously added route is observed as `existing`.
    futures::pin_mut!(event_stream);
    let existing = fnet_routes_ext::collect_routes_until_idle::<I, Vec<_>>(event_stream)
        .await
        .expect("failed to collect existing routes");
    let expected_route =
        new_installed_route(subnet, interface.id(), DEFAULT_INTERFACE_METRIC, true);
    assert!(
        existing.contains(&expected_route),
        "route: {:?}, existing: {:?}",
        expected_route,
        existing
    )
}

// Verifies the watcher protocols correctly report `added` and `removed` events.
#[netstack_test]
async fn watcher_add_remove<N: Netstack, I: net_types::ip::Ip + fnet_routes_ext::FidlRouteIpExt>(
    name: &str,
) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let device = sandbox.create_endpoint(name).await.expect("create endpoint");
    let interface = device.into_interface_in_realm(&realm).await.expect("add endpoint to Netstack");

    let subnet: net_types::ip::Subnet<I::Addr> =
        I::map_ip((), |()| TEST_SUBNET_V4, |()| TEST_SUBNET_V6);

    // Connect to the watcher protocol and consume all existing events.
    let state_proxy =
        realm.connect_to_protocol::<I::StateMarker>().expect("failed to connect to routes/State");
    let event_stream = fnet_routes_ext::event_stream_from_state::<I>(&state_proxy)
        .expect("failed to connect to routes watcher");
    futures::pin_mut!(event_stream);

    // Skip all `existing` events.
    let _existing_routes =
        fnet_routes_ext::collect_routes_until_idle::<I, Vec<_>>(event_stream.by_ref())
            .await
            .expect("failed to collect existing routes");

    // Add a test route.
    interface.add_subnet_route(subnet.into_ext()).await.expect("failed to add route");

    // Verify the `Added` event is observed.
    let added_route = assert_matches!(
        event_stream.next().await,
        Some(Ok(fnet_routes_ext::Event::<I>::Added(route))) => route
    );
    let expected_route =
        new_installed_route(subnet, interface.id(), DEFAULT_INTERFACE_METRIC, true);
    assert_eq!(added_route, expected_route);

    // Remove the test route.
    interface.del_subnet_route(subnet.into_ext()).await.expect("failed to remove route");

    // Verify the removed event is observed.
    let removed_route = assert_matches!(
        event_stream.next().await,
        Some(Ok(fnet_routes_ext::Event::<I>::Removed(route))) => route
    );
    assert_eq!(removed_route, expected_route);
}

// Verifies the watcher protocols close if the client incorrectly calls `Watch()`
// while there is already a pending `Watch()` call parked in the server.
#[netstack_test]
async fn watcher_already_pending<
    N: Netstack,
    I: net_types::ip::Ip + fnet_routes_ext::FidlRouteIpExt,
>(
    name: &str,
) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let state_proxy =
        realm.connect_to_protocol::<I::StateMarker>().expect("failed to connect to routes/State");
    let watcher_proxy = fnet_routes_ext::get_watcher::<I>(&state_proxy)
        .expect("failed to connect to watcher protocol");

    // Call `Watch` in a loop until the idle event is observed.
    while fnet_routes_ext::watch::<I>(&watcher_proxy)
        .map(|event_batch| {
            event_batch.expect("error while calling watch").into_iter().all(|event| {
                use fnet_routes_ext::Event::*;
                match event.try_into().expect("failed to process event") {
                    Existing(_) => true,
                    Idle => false,
                    e @ Unknown | e @ Added(_) | e @ Removed(_) => {
                        panic!("unexpected event received from the routes watcher: {e:?}")
                    }
                }
            })
        })
        .await
    {}

    // Call `Watch` twice and observe the protocol close.
    assert_matches!(
        futures::future::join(
            fnet_routes_ext::watch::<I>(&watcher_proxy),
            fnet_routes_ext::watch::<I>(&watcher_proxy),
        )
        .await,
        (
            Err(fidl::Error::ClientChannelClosed { status: zx_status::Status::PEER_CLOSED, .. }),
            Err(fidl::Error::ClientChannelClosed { status: zx_status::Status::PEER_CLOSED, .. }),
        )
    );
    assert!(watcher_proxy.is_closed());
}

// Verifies the watcher protocol does not get torn down when the `State`
// protocol is closed.
#[netstack_test]
async fn watcher_outlives_state<
    N: Netstack,
    I: net_types::ip::Ip + fnet_routes_ext::FidlRouteIpExt,
>(
    name: &str,
) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");

    // Connect to the watcher protocol and consume all existing events.
    let state_proxy =
        realm.connect_to_protocol::<I::StateMarker>().expect("failed to connect to routes/State");
    let event_stream = fnet_routes_ext::event_stream_from_state::<I>(&state_proxy)
        .expect("failed to connect to routes watcher");
    futures::pin_mut!(event_stream);

    // Skip all `existing` events.
    let _existing_routes =
        fnet_routes_ext::collect_routes_until_idle::<I, Vec<_>>(event_stream.by_ref())
            .await
            .expect("failed to collect existing routes");

    // Drop the state proxy and verify the event_stream stays open
    drop(state_proxy);
    event_stream
        .next()
        .map(Err)
        .on_timeout(
            fuchsia_async::Time::after(netstack_testing_common::ASYNC_EVENT_NEGATIVE_CHECK_TIMEOUT),
            || Ok(()),
        )
        .await
        .expect("Unexpected event in event stream");
}

/// Verifies several instantiations of the watcher protocol can exist independent
/// of one another.
#[netstack_test]
async fn watcher_multiple_instances<
    N: Netstack,
    I: net_types::ip::Ip + fnet_routes_ext::FidlRouteIpExt,
>(
    name: &str,
) {
    const NUM_INSTANCES: u8 = 10;

    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let device = sandbox.create_endpoint(name).await.expect("create endpoint");
    let interface = device.into_interface_in_realm(&realm).await.expect("add endpoint to Netstack");

    let state_proxy =
        realm.connect_to_protocol::<I::StateMarker>().expect("failed to connect to routes/State");

    let mut watchers = Vec::new();
    let mut expected_existing_routes = Vec::new();

    // For each iteration, instantiate a watcher and add a unique route. The
    // route will appear as `added` for all "already-existing" watcher clients,
    // but `existing` for all "not-yet-instantiated" watcher clients in future
    // iterations. This ensures that each client is operating over a unique
    // event stream.
    for i in 0..NUM_INSTANCES {
        // Connect to the watcher protocol and observe all expected existing
        // events
        let mut event_stream = fnet_routes_ext::event_stream_from_state::<I>(&state_proxy)
            .expect("failed to connect to routes watcher")
            .boxed_local();
        let existing =
            fnet_routes_ext::collect_routes_until_idle::<I, Vec<_>>(event_stream.by_ref())
                .await
                .expect("failed to collect existing routes");
        for route in &expected_existing_routes {
            assert!(existing.contains(&route), "route: {:?}, existing: {:?}", route, existing)
        }
        watchers.push(event_stream);

        // Add a test route whose subnet is unique based on `i`.
        let subnet: net_types::ip::Subnet<I::Addr> = I::map_ip(
            IpInvariant(i),
            |IpInvariant(i)| {
                net_types::ip::Subnet::new(net_types::ip::Ipv4Addr::new([192, 168, i, 0]), 24)
                    .unwrap()
            },
            |IpInvariant(i)| {
                net_types::ip::Subnet::new(
                    net_types::ip::Ipv6Addr::from_bytes([
                        0xfd, 0, i, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    ]),
                    64,
                )
                .unwrap()
            },
        );
        interface.add_subnet_route(subnet.into_ext()).await.expect("failed to add route");
        let expected_route =
            new_installed_route(subnet, interface.id(), DEFAULT_INTERFACE_METRIC, true);
        expected_existing_routes.push(expected_route);

        // Observe an `added` event on all connected watchers.
        for event_stream in watchers.iter_mut() {
            let added_route = assert_matches!(
                event_stream.next().await,
                Some(Ok(fnet_routes_ext::Event::<I>::Added(route))) => route
            );
            assert_eq!(added_route, expected_route);
        }
    }
}
