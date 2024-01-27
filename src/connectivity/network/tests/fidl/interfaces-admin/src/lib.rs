// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use assert_matches::assert_matches;
use fidl_fuchsia_net as fnet;
use fidl_fuchsia_net_debug as fnet_debug;
use fidl_fuchsia_net_ext as fnet_ext;
use fidl_fuchsia_net_ext::IntoExt;
use fidl_fuchsia_net_interfaces_admin as finterfaces_admin;
use fidl_fuchsia_net_interfaces_ext as fnet_interfaces_ext;
use fidl_fuchsia_net_routes as fnet_routes;
use fidl_fuchsia_net_routes_ext as fnet_routes_ext;
use fidl_fuchsia_posix_socket as fposix_socket;
use fuchsia_async::{
    self as fasync,
    net::{DatagramSocket, UdpSocket},
    TimeoutExt as _,
};
use fuchsia_zircon as zx;
use fuchsia_zircon_status as zx_status;
use futures::{FutureExt as _, StreamExt as _, TryFutureExt as _, TryStreamExt as _};
use net_declare::{fidl_ip, fidl_mac, fidl_subnet, std_ip_v6, std_socket_addr};
use net_types::ip::{IpAddress as _, IpVersion, Ipv4};
use netemul::RealmUdpSocket as _;
use netstack_testing_common::{
    devices::create_tun_device,
    interfaces,
    realms::{Netstack, NetstackVersion, TestRealmExt as _, TestSandboxExt as _},
    ASYNC_EVENT_POSITIVE_CHECK_TIMEOUT,
};
use netstack_testing_macros::netstack_test;
use std::collections::{HashMap, HashSet};
use std::convert::TryInto as _;
use std::ops::Not as _;
use test_case::test_case;

#[netstack_test]
async fn address_deprecation<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let device = sandbox.create_endpoint(name).await.expect("create endpoint");
    let interface = device.into_interface_in_realm(&realm).await.expect("add endpoint to Netstack");
    assert!(interface.control().enable().await.expect("send enable").expect("enable"));
    let () = interface.set_link_up(true).await.expect("bring device up");

    const ADDR1: std::net::Ipv6Addr = std_ip_v6!("abcd::1");
    const ADDR2: std::net::Ipv6Addr = std_ip_v6!("abcd::2");
    // Cannot be const because `std::net::SocketAddrV6:new` isn't const.
    let sock_addr = std_socket_addr!("[abcd::3]:12345");
    // Note that the absence of the preferred_lifetime_info field implies infinite
    // preferred lifetime.
    let preferred_properties = fidl_fuchsia_net_interfaces_admin::AddressProperties::default();
    let deprecated_properties = fidl_fuchsia_net_interfaces_admin::AddressProperties {
        preferred_lifetime_info: Some(
            fidl_fuchsia_net_interfaces::PreferredLifetimeInfo::Deprecated(
                fidl_fuchsia_net_interfaces::Empty,
            ),
        ),
        ..Default::default()
    };
    let addr1_state_provider = interfaces::add_subnet_address_and_route_wait_assigned(
        &interface,
        fidl_fuchsia_net::Subnet {
            addr: fidl_fuchsia_net::IpAddress::Ipv6(fidl_fuchsia_net::Ipv6Address {
                addr: ADDR1.octets(),
            }),
            prefix_len: 16,
        },
        // Note that an empty AddressParameters means that the address has
        // infinite preferred lifetime.
        fidl_fuchsia_net_interfaces_admin::AddressParameters {
            initial_properties: Some(preferred_properties.clone()),
            ..Default::default()
        },
    )
    .await
    .expect("failed to add preferred address");

    let addr2_state_provider = interfaces::add_address_wait_assigned(
        interface.control(),
        fidl_fuchsia_net::Subnet {
            addr: fidl_fuchsia_net::IpAddress::Ipv6(fidl_fuchsia_net::Ipv6Address {
                addr: ADDR2.octets(),
            }),
            prefix_len: (ADDR2.octets().len() * 8).try_into().unwrap(),
        },
        fidl_fuchsia_net_interfaces_admin::AddressParameters {
            initial_properties: Some(deprecated_properties.clone()),
            ..Default::default()
        },
    )
    .await
    .expect("failed to add deprecated address");

    let get_source_addr = || async {
        let sock = realm
            .datagram_socket(
                fidl_fuchsia_posix_socket::Domain::Ipv6,
                fidl_fuchsia_posix_socket::DatagramSocketProtocol::Udp,
            )
            .await
            .expect("failed to create UDP socket");
        sock.connect(&socket2::SockAddr::from(sock_addr)).expect("failed to connect with socket");
        *sock
            .local_addr()
            .expect("failed to get socket local addr")
            .as_socket_ipv6()
            .expect("socket local addr not IPv6")
            .ip()
    };
    assert_eq!(get_source_addr().await, ADDR1);

    addr1_state_provider
        .update_address_properties(&deprecated_properties)
        .await
        .expect("FIDL error deprecating address");
    addr2_state_provider
        .update_address_properties(&preferred_properties)
        .await
        .expect("FIDL error setting address to preferred");

    assert_eq!(get_source_addr().await, ADDR2);
}

#[netstack_test]
async fn update_address_lifetimes<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let device = sandbox.create_endpoint(name).await.expect("create endpoint");
    let interface = realm
        .install_endpoint(device, Default::default())
        .await
        .expect("install endpoint into Netstack");

    const ADDR: fidl_fuchsia_net::Subnet = fidl_subnet!("abcd::1/64");
    let addr_state_provider = interfaces::add_subnet_address_and_route_wait_assigned(
        &interface,
        ADDR,
        fidl_fuchsia_net_interfaces_admin::AddressParameters::default(),
    )
    .await
    .expect("failed to add preferred address");

    let interface_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");
    let event_stream = fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)
        .expect("event stream from state")
        .fuse();
    futures::pin_mut!(event_stream);
    let mut if_state = fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(interface.id());
    async fn wait_for_lifetimes(
        event_stream: impl futures::Stream<
            Item = Result<fidl_fuchsia_net_interfaces::Event, fidl::Error>,
        >,
        if_state: &mut fidl_fuchsia_net_interfaces_ext::InterfaceState,
        valid_until: zx::sys::zx_time_t,
    ) -> Result<(), anyhow::Error> {
        fidl_fuchsia_net_interfaces_ext::wait_interface_with_id(
            event_stream,
            if_state,
            move |fidl_fuchsia_net_interfaces_ext::Properties {
                      addresses,
                      online: _,
                      id: _,
                      name: _,
                      device_class: _,
                      has_default_ipv4_route: _,
                      has_default_ipv6_route: _,
                  }: &fidl_fuchsia_net_interfaces_ext::Properties| {
                addresses
                    .contains(&fidl_fuchsia_net_interfaces_ext::Address { addr: ADDR, valid_until })
                    .then_some(())
            },
        )
        .await
        .map_err(Into::into)
    }
    wait_for_lifetimes(event_stream.by_ref(), &mut if_state, zx::sys::ZX_TIME_INFINITE)
        .await
        .expect("failed to observe address with default (infinite) lifetimes");

    {
        const VALID_UNTIL: zx::sys::zx_time_t = 123_000_000_000;
        addr_state_provider
            .update_address_properties(&fidl_fuchsia_net_interfaces_admin::AddressProperties {
                preferred_lifetime_info: None,
                valid_lifetime_end: Some(VALID_UNTIL),
                ..Default::default()
            })
            .await
            .expect("FIDL error updating address lifetimes");

        wait_for_lifetimes(event_stream.by_ref(), &mut if_state, VALID_UNTIL)
            .await
            .expect("failed to observe address with updated lifetimes");
    }
}

#[netstack_test]
async fn add_address_errors<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");

    let fidl_fuchsia_net_interfaces_ext::Properties {
        id: loopback_id,
        addresses,
        name: _,
        device_class: _,
        online: _,
        has_default_ipv4_route: _,
        has_default_ipv6_route: _,
    } = realm
        .loopback_properties()
        .await
        .expect("failed to get loopback properties")
        .expect("loopback not found");

    let control = realm
        .interface_control(loopback_id.get())
        .expect("failed to get loopback interface control client proxy");

    let valid_address_parameters = fidl_fuchsia_net_interfaces_admin::AddressParameters::default();

    // Removing non-existent address.
    {
        let mut address = fidl_subnet!("1.1.1.1/32");
        let did_remove = control
            .remove_address(&mut address)
            .await
            .expect("FIDL error calling fuchsia.net.interfaces.admin/Control.RemoveAddress")
            .expect("RemoveAddress failed");
        assert!(!did_remove);
    }

    let (control, v4_addr, v6_addr) = futures::stream::iter(addresses).fold((control, None, None), |(control, v4, v6), fidl_fuchsia_net_interfaces_ext::Address {
        addr,
        valid_until: _,
    }| {
        let (v4, v6) = {
            let fidl_fuchsia_net::Subnet { addr, prefix_len } = addr;
            match addr {
                fidl_fuchsia_net::IpAddress::Ipv4(addr) => {
                    let nt_addr = net_types::ip::Ipv4Addr::new(addr.addr);
                    assert!(nt_addr.is_loopback(), "{} is not a loopback address", nt_addr);
                    let addr = fidl_fuchsia_net::Ipv4AddressWithPrefix {
                        addr,
                        prefix_len,
                    };
                    assert_eq!(v4, None, "v4 address already present, found {:?}", addr);
                    (Some(addr), v6)
                }
                fidl_fuchsia_net::IpAddress::Ipv6(addr) => {
                    let nt_addr = net_types::ip::Ipv6Addr::from_bytes(addr.addr);
                    assert!(nt_addr.is_loopback(), "{} is not a loopback address", nt_addr);
                    assert_eq!(v6, None, "v6 address already present, found {:?}", addr);
                    let addr = fidl_fuchsia_net::Ipv6AddressWithPrefix {
                        addr,
                        prefix_len,
                    };
                    (v4, Some(addr))
                }
            }
        };
        let valid_address_parameters = valid_address_parameters.clone();
        async move {
            assert_matches::assert_matches!(
                interfaces::add_address_wait_assigned(&control, addr.clone(), valid_address_parameters).await,
                Err(fidl_fuchsia_net_interfaces_ext::admin::AddressStateProviderError::AddressRemoved(
                    fidl_fuchsia_net_interfaces_admin::AddressRemovalReason::AlreadyAssigned
                )));
            (control, v4, v6)
        }
    }).await;
    assert_ne!(v4_addr, None, "expected v4 address");
    assert_ne!(v6_addr, None, "expected v6 address");

    // Adding an invalid address returns error.
    {
        // NB: fidl_subnet! doesn't allow invalid prefix lengths.
        let invalid_address =
            fidl_fuchsia_net::Subnet { addr: fidl_ip!("1.1.1.1"), prefix_len: 33 };
        assert_matches::assert_matches!(
            interfaces::add_address_wait_assigned(
                &control,
                invalid_address,
                valid_address_parameters
            )
            .await,
            Err(fidl_fuchsia_net_interfaces_ext::admin::AddressStateProviderError::AddressRemoved(
                fidl_fuchsia_net_interfaces_admin::AddressRemovalReason::Invalid
            ))
        );
    }
}

#[netstack_test]
async fn add_address_removal<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("new sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let device = sandbox.create_endpoint(name).await.expect("create endpoint");
    let interface = device.into_interface_in_realm(&realm).await.expect("add endpoint to Netstack");
    let id = interface.id();
    assert!(interface.control().enable().await.expect("send enable").expect("enable"));
    let () = interface.set_link_up(true).await.expect("bring device up");

    let debug_control = realm
        .connect_to_protocol::<fidl_fuchsia_net_debug::InterfacesMarker>()
        .expect(<fidl_fuchsia_net_debug::InterfacesMarker as fidl::endpoints::DiscoverableProtocolMarker>::PROTOCOL_NAME);

    let (control, server) = fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints()
        .expect("create Control proxy");
    let () = debug_control.get_admin(id, server).expect("get admin");

    let valid_address_parameters = fidl_fuchsia_net_interfaces_admin::AddressParameters::default();

    // Adding a valid address and observing the address removal.
    {
        let mut address = fidl_subnet!("3.3.3.3/32");

        let address_state_provider = interfaces::add_address_wait_assigned(
            &control,
            address,
            valid_address_parameters.clone(),
        )
        .await
        .expect("add address failed unexpectedly");

        let did_remove = control
            .remove_address(&mut address)
            .await
            .expect("FIDL error calling Control.RemoveAddress")
            .expect("error calling Control.RemoveAddress");
        assert!(did_remove);

        let fidl_fuchsia_net_interfaces_admin::AddressStateProviderEvent::OnAddressRemoved {
            error: reason,
        } = address_state_provider
            .take_event_stream()
            .try_next()
            .await
            .expect("read AddressStateProvider event")
            .expect("AddressStateProvider event stream ended unexpectedly");
        assert_eq!(reason, fidl_fuchsia_net_interfaces_admin::AddressRemovalReason::UserRemoved);
    }

    // Adding a valid address and removing the interface.
    {
        let address = fidl_subnet!("4.4.4.4/32");

        let address_state_provider = interfaces::add_address_wait_assigned(
            &control,
            address,
            valid_address_parameters.clone(),
        )
        .await
        .expect("add address failed unexpectedly");

        let (_netemul_endpoint, _device_control_handle) =
            interface.remove().await.expect("failed to remove interface");
        let fidl_fuchsia_net_interfaces_admin::AddressStateProviderEvent::OnAddressRemoved {
            error: reason,
        } = address_state_provider
            .take_event_stream()
            .try_next()
            .await
            .expect("read AddressStateProvider event")
            .expect("AddressStateProvider event stream ended unexpectedly");
        assert_eq!(
            reason,
            fidl_fuchsia_net_interfaces_admin::AddressRemovalReason::InterfaceRemoved
        );

        assert_matches::assert_matches!(
            control.wait_termination().await,
            fidl_fuchsia_net_interfaces_ext::admin::TerminalError::Terminal(
                fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::User
            )
        );
    }
}

// Add an address while the interface is offline, bring the interface online and ensure that the
// assignment state is set correctly.
#[netstack_test]
async fn add_address_offline<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("new sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let device = sandbox.create_endpoint(name).await.expect("create endpoint");
    let interface = device.into_interface_in_realm(&realm).await.expect("add endpoint to Netstack");
    let id = interface.id();

    let debug_control = realm
        .connect_to_protocol::<fidl_fuchsia_net_debug::InterfacesMarker>()
        .expect(<fidl_fuchsia_net_debug::InterfacesMarker as fidl::endpoints::DiscoverableProtocolMarker>::PROTOCOL_NAME);

    let (control, server) =
        fidl::endpoints::create_proxy::<fidl_fuchsia_net_interfaces_admin::ControlMarker>()
            .expect("create Control proxy");
    let () = debug_control.get_admin(id, server).expect("get admin");

    let valid_address_parameters = fidl_fuchsia_net_interfaces_admin::AddressParameters::default();

    // Adding a valid address and observing the address removal.
    let mut address = fidl_subnet!("5.5.5.5/32");

    let (address_state_provider, server) = fidl::endpoints::create_proxy::<
        fidl_fuchsia_net_interfaces_admin::AddressStateProviderMarker,
    >()
    .expect("create AddressStateProvider proxy");
    let () = control
        .add_address(&mut address, &valid_address_parameters, server)
        .expect("Control.AddAddress FIDL error");

    let state_stream = fidl_fuchsia_net_interfaces_ext::admin::assignment_state_stream(
        address_state_provider.clone(),
    );
    futures::pin_mut!(state_stream);
    let () = fidl_fuchsia_net_interfaces_ext::admin::wait_assignment_state(
        &mut state_stream,
        fidl_fuchsia_net_interfaces_admin::AddressAssignmentState::Unavailable,
    )
    .await
    .expect("wait for UNAVAILABLE address assignment state");

    let did_enable = interface.control().enable().await.expect("send enable").expect("enable");
    assert!(did_enable);
    let () = interface.set_link_up(true).await.expect("bring device up");

    let () = fidl_fuchsia_net_interfaces_ext::admin::wait_assignment_state(
        &mut state_stream,
        fidl_fuchsia_net_interfaces_admin::AddressAssignmentState::Assigned,
    )
    .await
    .expect("wait for ASSIGNED address assignment state");
}

// Verify that a request to `WatchAddressAssignmentState` while an existing
// request is pending causes the `AddressStateProvider` protocol to close,
// regardless of whether the protocol is detached.
#[netstack_test]
#[test_case(false; "no_detach")]
#[test_case(true; "detach")]
async fn duplicate_watch_address_assignment_state<N: Netstack>(name: &str, detach: bool) {
    let sandbox = netemul::TestSandbox::new().expect("new sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let device = sandbox.create_endpoint(name).await.expect("create endpoint");
    let interface = device.into_interface_in_realm(&realm).await.expect("add endpoint to Netstack");
    assert!(interface.control().enable().await.expect("send enable").expect("enable"));
    let () = interface.set_link_up(true).await.expect("bring device up");

    let valid_address_parameters = fidl_fuchsia_net_interfaces_admin::AddressParameters::default();
    let address = fidl_subnet!("1.1.1.1/32");
    let address_state_provider = interfaces::add_address_wait_assigned(
        interface.control(),
        address,
        valid_address_parameters,
    )
    .await
    .expect("add address failed unexpectedly");

    if detach {
        address_state_provider.detach().expect("failed to detach");
    }

    // Invoke `WatchAddressAssignmentState` twice and assert that the two
    // requests and the AddressStateProvider protocol are closed.
    assert_matches!(
        futures::future::join(
            address_state_provider.watch_address_assignment_state(),
            address_state_provider.watch_address_assignment_state(),
        )
        .await,
        (
            Err(fidl::Error::ClientChannelClosed { status: zx_status::Status::PEER_CLOSED, .. }),
            Err(fidl::Error::ClientChannelClosed { status: zx_status::Status::PEER_CLOSED, .. }),
        )
    );
    assert_matches!(
        address_state_provider
            .take_event_stream()
            .try_next()
            .await
            .expect("read AddressStateProvider event"),
        None
    );
}

/// Creates a realm in the provided sandbox and an interface in that realm.
async fn create_realm_and_interface<'a, N: Netstack>(
    name: &'a str,
    sandbox: &'a netemul::TestSandbox,
) -> (
    netemul::TestRealm<'a>,
    fidl_fuchsia_net_interfaces::StateProxy,
    u64,
    fidl_fuchsia_net_interfaces_ext::admin::Control,
) {
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");

    let interface_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect(<fidl_fuchsia_net_interfaces::StateMarker as fidl::endpoints::DiscoverableProtocolMarker>::PROTOCOL_NAME);

    let interfaces = fidl_fuchsia_net_interfaces_ext::existing(
        fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)
            .expect("create watcher event stream"),
        HashMap::new(),
    )
    .await
    .expect("initial");
    assert_eq!(interfaces.len(), 1);
    let id = *interfaces
        .keys()
        .next()
        .expect("interface properties map unexpectedly does not include loopback");

    let debug_control = realm
        .connect_to_protocol::<fidl_fuchsia_net_debug::InterfacesMarker>()
        .expect(<fidl_fuchsia_net_debug::InterfacesMarker as fidl::endpoints::DiscoverableProtocolMarker>::PROTOCOL_NAME);

    let (control, server) = fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints()
        .expect("create Control proxy");
    debug_control.get_admin(id, server).expect("get admin");

    (realm, interface_state, id, control)
}

async fn ipv4_routing_table(
    realm: &netemul::TestRealm<'_>,
) -> Vec<fnet_routes_ext::InstalledRoute<Ipv4>> {
    let state_v4 =
        realm.connect_to_protocol::<fnet_routes::StateV4Marker>().expect("connect to protocol");
    let stream = fnet_routes_ext::event_stream_from_state::<Ipv4>(&state_v4)
        .expect("failed to connect to watcher");
    futures::pin_mut!(stream);
    fnet_routes_ext::collect_routes_until_idle::<_, Vec<_>>(stream)
        .await
        .expect("failed to get routing table")
}

enum AddressRemoval {
    DropHandle,
    CallRemove,
}
use AddressRemoval::*;

#[netstack_test]
#[test_case(None, false, CallRemove; "default add_subnet_route explicit remove")]
#[test_case(None, false, DropHandle; "default add_subnet_route implicit remove")]
#[test_case(Some(false), false, CallRemove; "add_subnet_route is false explicit remove")]
#[test_case(Some(false), false, DropHandle; "add_subnet_route is false implicit remove")]
#[test_case(Some(true), true, CallRemove; "add_subnet_route is true explicit remove")]
#[test_case(Some(true), true, DropHandle; "add_subnet_route is true implicit remove")]
async fn add_address_and_remove<N: Netstack>(
    name: &str,
    add_subnet_route: Option<bool>,
    expect_subnet_route: bool,
    remove_address: AddressRemoval,
) {
    let sandbox = netemul::TestSandbox::new().expect("new sandbox");
    let (realm, interface_state, id, control) =
        create_realm_and_interface::<N>(name, &sandbox).await;

    // Adding a valid address succeeds.
    let subnet = fidl_subnet!("1.1.1.1/32");
    let address_state_provider = interfaces::add_address_wait_assigned(
        &control,
        subnet,
        fidl_fuchsia_net_interfaces_admin::AddressParameters {
            add_subnet_route,
            ..Default::default()
        },
    )
    .await
    .expect("add address failed unexpectedly");

    // Ensure that a subnet route was added if requested.
    let subnet_route_is_present = ipv4_routing_table(&realm).await.iter().any(|route| {
        <net_types::ip::Subnet<net_types::ip::Ipv4Addr> as IntoExt<fnet::Subnet>>::into_ext(
            route.route.destination,
        ) == subnet
    });
    assert_eq!(subnet_route_is_present, expect_subnet_route);

    let event_stream = fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)
        .expect("event stream from state");
    futures::pin_mut!(event_stream);
    let mut properties = fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(id);
    let () = fidl_fuchsia_net_interfaces_ext::wait_interface_with_id(
        event_stream.by_ref(),
        &mut properties,
        |fidl_fuchsia_net_interfaces_ext::Properties {
             id: _,
             name: _,
             device_class: _,
             online: _,
             addresses,
             has_default_ipv4_route: _,
             has_default_ipv6_route: _,
         }| {
            addresses
                .iter()
                .any(|&fidl_fuchsia_net_interfaces_ext::Address { addr, valid_until: _ }| {
                    addr == subnet
                })
                .then(|| ())
        },
    )
    .await
    .expect("wait for address presence");

    match remove_address {
        AddressRemoval::DropHandle => {
            // Explicitly drop the AddressStateProvider channel to cause address deletion.
            std::mem::drop(address_state_provider);

            let () = fidl_fuchsia_net_interfaces_ext::wait_interface_with_id(
                event_stream.by_ref(),
                &mut properties,
                |fidl_fuchsia_net_interfaces_ext::Properties {
                     id: _,
                     name: _,
                     device_class: _,
                     online: _,
                     addresses,
                     has_default_ipv4_route: _,
                     has_default_ipv6_route: _,
                 }| {
                    addresses
                        .iter()
                        .all(
                            |&fidl_fuchsia_net_interfaces_ext::Address { addr, valid_until: _ }| {
                                addr != subnet
                            },
                        )
                        .then(|| ())
                },
            )
            .await
            .expect("wait for address absence");
        }
        AddressRemoval::CallRemove => {
            assert_eq!(
                control.remove_address(&mut subnet.clone()).await.expect("fidl success"),
                Ok(true)
            )
        }
    }

    // In either case, there should be no route for the subnet after the
    // address is removed.
    let routes = ipv4_routing_table(&realm).await;
    let subnet_route_is_present = routes.iter().any(|route| {
        <net_types::ip::Subnet<net_types::ip::Ipv4Addr> as IntoExt<fnet::Subnet>>::into_ext(
            route.route.destination,
        ) == subnet
    });
    assert_eq!(subnet_route_is_present, false, "found subnet route in {:?}", routes);
}

#[netstack_test]
#[test_case(None, false; "default add_subnet_route")]
#[test_case(Some(false), false; "add_subnet_route is false")]
#[test_case(Some(true), true; "add_subnet_route is true")]
async fn add_address_and_detach<N: Netstack>(
    name: &str,
    add_subnet_route: Option<bool>,
    expect_subnet_route: bool,
) {
    let sandbox = netemul::TestSandbox::new().expect("new sandbox");
    let (realm, interface_state, id, control) =
        create_realm_and_interface::<N>(name, &sandbox).await;

    // Adding a valid address and detaching does not cause the address (or the
    // subnet, if one was requested) to be removed.
    let subnet = fidl_subnet!("2.2.2.2/32");
    let address_state_provider = interfaces::add_address_wait_assigned(
        &control,
        subnet,
        fidl_fuchsia_net_interfaces_admin::AddressParameters {
            add_subnet_route,
            ..Default::default()
        },
    )
    .await
    .expect("add address failed unexpectedly");

    let () = address_state_provider
        .detach()
        .expect("FIDL error calling fuchsia.net.interfaces.admin/Control.Detach");

    std::mem::drop(address_state_provider);

    let mut properties = fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(id);
    let () = fidl_fuchsia_net_interfaces_ext::wait_interface_with_id(
        fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)
            .expect("get interface event stream"),
        &mut properties,
        |fidl_fuchsia_net_interfaces_ext::Properties {
             id: _,
             name: _,
             device_class: _,
             online: _,
             addresses,
             has_default_ipv4_route: _,
             has_default_ipv6_route: _,
         }| {
            addresses
                .iter()
                .all(|&fidl_fuchsia_net_interfaces_ext::Address { addr, valid_until: _ }| {
                    addr != subnet
                })
                .then(|| ())
        },
    )
    .map_ok(|()| panic!("address deleted after detaching and closing channel"))
    .on_timeout(fuchsia_async::Time::after(fuchsia_zircon::Duration::from_millis(100)), || Ok(()))
    .await
    .expect("wait for address to not be removed");

    let subnet_route_is_still_present = ipv4_routing_table(&realm).await.iter().any(|route| {
        <net_types::ip::Subnet<net_types::ip::Ipv4Addr> as IntoExt<fnet::Subnet>>::into_ext(
            route.route.destination,
        ) == subnet
    });
    assert_eq!(subnet_route_is_still_present, expect_subnet_route);
}

#[netstack_test]
async fn device_control_create_interface<N: Netstack>(name: &str) {
    // NB: interface names are limited to fuchsia.net.interfaces/INTERFACE_NAME_LENGTH.
    const IF_NAME: &'static str = "ctrl_create_if";

    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let endpoint = sandbox.create_endpoint(name).await.expect("create endpoint");
    let installer = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces_admin::InstallerMarker>()
        .expect("connect to protocol");

    let (device, port_id) = endpoint.get_netdevice().await.expect("get netdevice");
    let (device_control, device_control_server_end) =
        fidl::endpoints::create_proxy::<fidl_fuchsia_net_interfaces_admin::DeviceControlMarker>()
            .expect("create proxy");
    let () = installer.install_device(device, device_control_server_end).expect("install device");

    let (control, control_server_end) =
        fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints().expect("create proxy");
    let () = device_control
        .create_interface(
            &port_id,
            control_server_end,
            &fidl_fuchsia_net_interfaces_admin::Options {
                name: Some(IF_NAME.to_string()),
                metric: None,
                ..Default::default()
            },
        )
        .expect("create interface");

    let iface_id = control.get_id().await.expect("get id");

    let interfaces_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");
    let interface_state = fidl_fuchsia_net_interfaces_ext::existing(
        fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interfaces_state)
            .expect("create watcher event stream"),
        fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(iface_id),
    )
    .await
    .expect("get interface state");
    let properties = match interface_state {
        fidl_fuchsia_net_interfaces_ext::InterfaceState::Known(properties) => properties,
        fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(id) => {
            panic!("failed to retrieve new interface with id {}", id)
        }
    };
    assert_eq!(
        properties,
        fidl_fuchsia_net_interfaces_ext::Properties {
            id: iface_id.try_into().expect("should be nonzero"),
            name: IF_NAME.to_string(),
            device_class: fidl_fuchsia_net_interfaces::DeviceClass::Device(
                fidl_fuchsia_hardware_network::DeviceClass::Virtual
            ),
            online: false,
            // We haven't enabled the interface, it mustn't have any addresses assigned
            // to it yet.
            addresses: vec![],
            has_default_ipv4_route: false,
            has_default_ipv6_route: false
        }
    );
}

// Tests that when a DeviceControl instance is dropped, all interfaces created
// from it are dropped as well.
#[netstack_test]
#[test_case(false; "no_detach")]
#[test_case(true; "detach")]
async fn device_control_owns_interfaces_lifetimes<N: Netstack>(name: &str, detach: bool) {
    let detach_str = if detach { "detach" } else { "no_detach" };
    let name = format!("{name}_{detach_str}");
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");

    // Create tun interfaces directly to attach ports to different interfaces.
    let (tun_dev, netdevice_client_end) = create_tun_device();

    let (device_control, device_control_server_end) =
        fidl::endpoints::create_proxy::<fidl_fuchsia_net_interfaces_admin::DeviceControlMarker>()
            .expect("create proxy");
    let installer = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces_admin::InstallerMarker>()
        .expect("connect to protocol");
    let () = installer
        .install_device(netdevice_client_end, device_control_server_end)
        .expect("install device");

    let interfaces_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");
    let watcher = fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interfaces_state)
        .expect("create event stream")
        .map(|r| r.expect("watcher error"))
        .fuse();
    futures::pin_mut!(watcher);

    // Consume the watcher until we see the idle event.
    let existing = fidl_fuchsia_net_interfaces_ext::existing(
        watcher.by_ref().map(Result::<_, fidl::Error>::Ok),
        HashMap::<u64, _>::new(),
    )
    .await
    .expect("existing");
    // Only loopback should exist.
    assert_eq!(existing.len(), 1, "unexpected interfaces in existing: {:?}", existing);

    const PORT_COUNT: u8 = 5;
    let mut interfaces = HashSet::new();
    let mut ports_detached_stream = futures::stream::FuturesUnordered::new();
    let mut control_proxies = Vec::new();
    // NB: For loop here is much more friendly to lifetimes than a closure
    // chain.
    for index in 1..=PORT_COUNT {
        let (iface_id, port, control) = async {
            let (port, port_server_end) =
                fidl::endpoints::create_proxy::<fidl_fuchsia_net_tun::PortMarker>()
                    .expect("create proxy");
            let () = tun_dev
                .add_port(
                    &fidl_fuchsia_net_tun::DevicePortConfig {
                        base: Some(fidl_fuchsia_net_tun::BasePortConfig {
                            id: Some(index),
                            rx_types: Some(vec![
                                fidl_fuchsia_hardware_network::FrameType::Ethernet,
                            ]),
                            tx_types: Some(vec![fidl_fuchsia_hardware_network::FrameTypeSupport {
                                type_: fidl_fuchsia_hardware_network::FrameType::Ethernet,
                                features: fidl_fuchsia_hardware_network::FRAME_FEATURES_RAW,
                                supported_flags: fidl_fuchsia_hardware_network::TxFlags::empty(),
                            }]),
                            mtu: Some(netemul::DEFAULT_MTU.into()),
                            ..Default::default()
                        }),
                        mac: Some(fidl_mac!("02:03:04:05:06:07")),
                        ..Default::default()
                    },
                    port_server_end,
                )
                .expect("add port");
            let port_id = {
                let (device_port, server) =
                    fidl::endpoints::create_proxy::<fidl_fuchsia_hardware_network::PortMarker>()
                        .expect("create endpoints");
                let () = port.get_port(server).expect("get port");
                device_port.get_info().await.expect("get info").id.expect("missing port id")
            };

            let (control, control_server_end) =
                fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints()
                    .expect("create proxy");

            let () = device_control
                .create_interface(
                    &port_id,
                    control_server_end,
                    &fidl_fuchsia_net_interfaces_admin::Options::default(),
                )
                .expect("create interface");

            let iface_id = control.get_id().await.expect("get id");

            // Observe interface creation in watcher.
            let event = watcher.select_next_some().await;
            assert_matches::assert_matches!(
                event,
                fidl_fuchsia_net_interfaces::Event::Added(
                    fidl_fuchsia_net_interfaces::Properties { id: Some(id), .. }
                ) if id == iface_id
            );

            (iface_id, port, control)
        }
        .await;
        assert!(
            interfaces.insert(iface_id),
            "unexpected duplicate interface iface_id: {}, interfaces={:?}",
            iface_id,
            interfaces
        );
        // Enable the interface and wait for port to be attached.
        assert!(control.enable().await.expect("calling enable").expect("enable failed"));
        let mut port_has_session_stream = futures::stream::unfold(port, |port| {
            port.watch_state().map(move |state| {
                let fidl_fuchsia_net_tun::InternalState { mac: _, has_session, .. } =
                    state.expect("calling watch_state");
                Some((has_session.expect("has_session missing from table"), port))
            })
        });
        loop {
            if port_has_session_stream.next().await.expect("port stream ended unexpectedly") {
                break;
            }
        }
        let port_detached = port_has_session_stream
            .filter_map(move |has_session| {
                futures::future::ready((!has_session).then(move || index))
            })
            .into_future()
            .map(|(i, _stream)| i.expect("port stream ended unexpectedly"));
        let () = ports_detached_stream.push(port_detached);
        let () = control_proxies.push(control);
    }

    let mut control_wait_termination_stream = control_proxies
        .into_iter()
        .map(|control| control.wait_termination())
        .collect::<futures::stream::FuturesUnordered<_>>();

    if detach {
        // Drop detached device_control and ensure none of the futures resolve.
        let () = device_control.detach().expect("detach");
        std::mem::drop(device_control);

        let watcher_fut = watcher.next().map(|e| panic!("unexpected watcher event {:?}", e));
        let ports_fut = ports_detached_stream
            .next()
            .map(|item| panic!("session detached from port unexpectedly {:?}", item));
        let control_closed_fut = control_wait_termination_stream
            .next()
            .map(|termination| panic!("unexpected control termination event {:?}", termination));

        let ((), (), ()) = futures::future::join3(watcher_fut, ports_fut, control_closed_fut)
            .on_timeout(
                fuchsia_async::Time::after(
                    netstack_testing_common::ASYNC_EVENT_NEGATIVE_CHECK_TIMEOUT,
                ),
                || ((), (), ()),
            )
            .await;
    } else {
        // Drop device_control and wait for futures to resolve.
        std::mem::drop(device_control);

        let interfaces_removed_fut = async_utils::fold::fold_while(
            watcher,
            interfaces,
            |mut interfaces, event| match event {
                fidl_fuchsia_net_interfaces::Event::Removed(id) => {
                    assert!(interfaces.remove(&id));
                    futures::future::ready(if interfaces.is_empty() {
                        async_utils::fold::FoldWhile::Done(())
                    } else {
                        async_utils::fold::FoldWhile::Continue(interfaces)
                    })
                }
                event => panic!("unexpected event {:?}", event),
            },
        )
        .map(|fold_result| fold_result.short_circuited().expect("watcher ended"));

        let ports_are_detached_fut =
            ports_detached_stream.map(|_port_index: u8| ()).collect::<()>();
        let control_closed_fut = control_wait_termination_stream.for_each(|termination| {
            assert_matches::assert_matches!(
                termination,
                fidl_fuchsia_net_interfaces_ext::admin::TerminalError::Terminal(
                    fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::PortClosed
                )
            );
            futures::future::ready(())
        });

        let ((), (), ()) = futures::future::join3(
            interfaces_removed_fut,
            ports_are_detached_fut,
            control_closed_fut,
        )
        .await;
    }
}

#[netstack_test]
#[test_case(
fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::DuplicateName;
"DuplicateName"
)]
#[test_case(
fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::PortAlreadyBound;
"PortAlreadyBound"
)]
#[test_case(fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::BadPort; "BadPort")]
#[test_case(fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::PortClosed; "PortClosed")]
#[test_case(fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::User; "User")]
async fn control_terminal_events<N: Netstack>(
    name: &str,
    reason: fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason,
) {
    let name = format!("{}_{:?}", name, reason);

    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(&name).expect("create realm");

    let installer = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces_admin::InstallerMarker>()
        .expect("connect to protocol");

    let (tun_dev, device) = create_tun_device();

    const BASE_PORT_ID: u8 = 13;
    let base_port_config = fidl_fuchsia_net_tun::BasePortConfig {
        id: Some(BASE_PORT_ID),
        rx_types: Some(vec![fidl_fuchsia_hardware_network::FrameType::Ethernet]),
        tx_types: Some(vec![fidl_fuchsia_hardware_network::FrameTypeSupport {
            type_: fidl_fuchsia_hardware_network::FrameType::Ethernet,
            features: fidl_fuchsia_hardware_network::FRAME_FEATURES_RAW,
            supported_flags: fidl_fuchsia_hardware_network::TxFlags::empty(),
        }]),
        mtu: Some(netemul::DEFAULT_MTU.into()),
        ..Default::default()
    };

    let create_port = |config: fidl_fuchsia_net_tun::BasePortConfig| {
        let (port, port_server_end) =
            fidl::endpoints::create_proxy::<fidl_fuchsia_net_tun::PortMarker>()
                .expect("create proxy");
        let () = tun_dev
            .add_port(
                &fidl_fuchsia_net_tun::DevicePortConfig {
                    base: Some(config),
                    mac: Some(fidl_mac!("02:aa:bb:cc:dd:ee")),
                    ..Default::default()
                },
                port_server_end,
            )
            .expect("add port");
        async move {
            // Interact with port to make sure it's installed.
            let () = port.set_online(false).await.expect("calling set_online");

            let (device_port, server) =
                fidl::endpoints::create_proxy::<fidl_fuchsia_hardware_network::PortMarker>()
                    .expect("create endpoints");
            let () = port.get_port(server).expect("get port");
            let id = device_port.get_info().await.expect("get info").id.expect("missing port id");

            (port, id)
        }
    };

    let (device_control, device_control_server_end) =
        fidl::endpoints::create_proxy::<fidl_fuchsia_net_interfaces_admin::DeviceControlMarker>()
            .expect("create proxy");
    let () = installer.install_device(device, device_control_server_end).expect("install device");

    let create_interface = |port_id, options| {
        let (control, control_server_end) =
            fidl::endpoints::create_proxy::<fidl_fuchsia_net_interfaces_admin::ControlMarker>()
                .expect("create proxy");
        let () = device_control
            .create_interface(&port_id, control_server_end, &options)
            .expect("create interface");
        control
    };

    enum KeepResource {
        Control(fidl_fuchsia_net_interfaces_ext::admin::Control),
        Port(fidl_fuchsia_net_tun::PortProxy),
    }

    let (control, _keep_alive): (_, Vec<KeepResource>) = match reason {
        fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::PortAlreadyBound => {
            let (port, port_id) = create_port(base_port_config).await;
            let control1 = {
                let control =
                    fidl_fuchsia_net_interfaces_ext::admin::Control::new(create_interface(
                        port_id.clone(),
                        fidl_fuchsia_net_interfaces_admin::Options::default(),
                    ));
                // Verify that interface was created.
                let _: u64 = control.get_id().await.expect("get id");
                control
            };

            // Create a new interface with the same port identifier.
            let control2 =
                create_interface(port_id, fidl_fuchsia_net_interfaces_admin::Options::default());
            (control2, vec![KeepResource::Control(control1), KeepResource::Port(port)])
        }
        fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::DuplicateName => {
            let (port1, port1_id) = create_port(base_port_config.clone()).await;
            let if_name = "test_same_name";
            let control1 = {
                let control =
                    fidl_fuchsia_net_interfaces_ext::admin::Control::new(create_interface(
                        port1_id,
                        fidl_fuchsia_net_interfaces_admin::Options {
                            name: Some(if_name.to_string()),
                            ..Default::default()
                        },
                    ));
                // Verify that interface was created.
                let _: u64 = control.get_id().await.expect("get id");
                control
            };

            // Create a new interface with the same name.
            let (port2, port2_id) = create_port(fidl_fuchsia_net_tun::BasePortConfig {
                id: Some(BASE_PORT_ID + 1),
                ..base_port_config
            })
            .await;

            let control2 = create_interface(
                port2_id,
                fidl_fuchsia_net_interfaces_admin::Options {
                    name: Some(if_name.to_string()),
                    ..Default::default()
                },
            );
            (
                control2,
                vec![
                    KeepResource::Control(control1),
                    KeepResource::Port(port1),
                    KeepResource::Port(port2),
                ],
            )
        }
        fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::BadPort => {
            let (port, port_id) = create_port(fidl_fuchsia_net_tun::BasePortConfig {
                // netdevice/client.go only accepts IP devices that support both
                // IPv4 and IPv6.
                rx_types: Some(vec![fidl_fuchsia_hardware_network::FrameType::Ipv4]),
                ..base_port_config
            })
            .await;
            let control =
                create_interface(port_id, fidl_fuchsia_net_interfaces_admin::Options::default());
            (control, vec![KeepResource::Port(port)])
        }
        fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::PortClosed => {
            // Port closed is equivalent to port doesn't exist.
            let control = create_interface(
                fidl_fuchsia_hardware_network::PortId { base: BASE_PORT_ID, salt: 0 },
                fidl_fuchsia_net_interfaces_admin::Options::default(),
            );
            (control, vec![])
        }
        fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::User => {
            let (port, port_id) = create_port(base_port_config).await;
            let control =
                create_interface(port_id, fidl_fuchsia_net_interfaces_admin::Options::default());
            let interface_id = control.get_id().await.expect("get id");
            // Setup a control handle via the debug API, and drop the original control handle.
            let debug_interfaces = realm
                .connect_to_protocol::<fnet_debug::InterfacesMarker>()
                .expect("connect to protocol");
            let (debug_control, server_end) =
                fidl::endpoints::create_proxy::<finterfaces_admin::ControlMarker>()
                    .expect("create proxy");
            debug_interfaces.get_admin(interface_id, server_end).expect("get admin failed");
            // Wait for the debug handle to be fully installed by synchronizing on `get_id`.
            assert_eq!(debug_control.get_id().await.expect("get id"), interface_id);
            (debug_control, vec![KeepResource::Port(port)])
        }
        unknown_reason => panic!("unknown reason {:?}", unknown_reason),
    };

    // Observe a terminal event and channel closure.
    let got_reason = control
        .take_event_stream()
        .map_ok(|fidl_fuchsia_net_interfaces_admin::ControlEvent::OnInterfaceRemoved { reason }| {
            reason
        })
        .try_collect::<Vec<_>>()
        .await
        .expect("waiting for terminal event");
    assert_eq!(got_reason, [reason]);
}

// Test that destroying a device causes device control instance to close.
#[netstack_test]
async fn device_control_closes_on_device_close<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let endpoint = sandbox.create_endpoint(name).await.expect("create endpoint");

    // Create a watcher, we'll use it to ensure the Netstack didn't crash.
    let interfaces_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");
    let watcher = fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interfaces_state)
        .expect("create watcher");
    futures::pin_mut!(watcher);

    let installer = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces_admin::InstallerMarker>()
        .expect("connect to protocol");

    let (device, port_id) = endpoint.get_netdevice().await.expect("get netdevice");
    let (device_control, device_control_server_end) =
        fidl::endpoints::create_proxy::<fidl_fuchsia_net_interfaces_admin::DeviceControlMarker>()
            .expect("create proxy");
    let () = installer.install_device(device, device_control_server_end).expect("install device");

    // Create an interface and get its identifier to ensure the device is
    // installed.
    let (control, control_server_end) =
        fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints().expect("create proxy");
    let () = device_control
        .create_interface(
            &port_id,
            control_server_end,
            &fidl_fuchsia_net_interfaces_admin::Options::default(),
        )
        .expect("create interface");
    let _iface_id: u64 = control.get_id().await.expect("get id");

    // Drop the device and observe the control channel closing because the
    // device was destroyed.
    std::mem::drop(endpoint);
    assert_matches::assert_matches!(device_control.take_event_stream().next().await, None);

    // The channel could've been closed by a Netstack crash, consume from the
    // watcher to ensure that's not the case.
    let _: fidl_fuchsia_net_interfaces::Event =
        watcher.try_next().await.expect("watcher error").expect("watcher ended uexpectedly");
}

// TODO(https://fxbug.dev/110470) Remove this trait once the source of the
// timeout-induced-flake has been identified.
/// A wrapper for a [`futures::future::Future`] that panics if the future has
/// not resolved within [`ASYNC_EVENT_POSITIVE_CHECK_TIME`].
trait PanicOnTimeout: fasync::TimeoutExt {
    /// Wraps the [`futures::future::Future`] in an [`fasync::OnTimeout`] that
    /// panics if the future has not resolved within
    /// [`ASYNC_EVENT_POSITIVE_CHECK_TIME`].
    fn panic_on_timeout<S: std::fmt::Display + 'static>(
        self,
        name: S,
    ) -> fasync::OnTimeout<Self, Box<dyn FnOnce() -> Self::Output>> {
        self.on_timeout(
            ASYNC_EVENT_POSITIVE_CHECK_TIMEOUT,
            Box::new(move || {
                panic!("{}: Timed out after {:?}", name, ASYNC_EVENT_POSITIVE_CHECK_TIMEOUT)
            }),
        )
    }
}

impl<T: fasync::TimeoutExt> PanicOnTimeout for T {}

// Tests that interfaces created through installer have a valid datapath.
#[netstack_test]
async fn installer_creates_datapath<N: Netstack, I: net_types::ip::Ip>(test_name: &str) {
    const SUBNET: fidl_fuchsia_net::Subnet = fidl_subnet!("192.168.0.0/24");
    const ALICE_IP_V4: fidl_fuchsia_net::Subnet = fidl_subnet!("192.168.0.1/24");
    const BOB_IP_V4: fidl_fuchsia_net::Subnet = fidl_subnet!("192.168.0.2/24");

    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let network = sandbox
        .create_network("net")
        .panic_on_timeout("creating network")
        .await
        .expect("create network");

    struct RealmInfo<'a> {
        realm: netemul::TestRealm<'a>,
        endpoint: netemul::TestEndpoint<'a>,
        addr: std::net::IpAddr,
        iface_id: u64,
        device_control: fidl_fuchsia_net_interfaces_admin::DeviceControlProxy,
        control: fidl_fuchsia_net_interfaces_ext::admin::Control,
        address_state_provider:
            Option<fidl_fuchsia_net_interfaces_admin::AddressStateProviderProxy>,
    }

    let realms_stream = futures::stream::iter([("alice", ALICE_IP_V4), ("bob", BOB_IP_V4)]).then(
        |(name, ipv4_addr)| {
            let sandbox = &sandbox;
            let network = &network;
            async move {
                let test_name = format!("{}_{}", test_name, name);
                let realm =
                    sandbox.create_netstack_realm::<N, _>(test_name.clone()).expect("create realm");
                let endpoint = network
                    .create_endpoint(test_name)
                    .panic_on_timeout(format!("create {} endpoint", name))
                    .await
                    .expect("create endpoint");
                let () = endpoint
                    .set_link_up(true)
                    .panic_on_timeout(format!("set {} link up", name))
                    .await
                    .expect("set link up");
                let installer = realm
                    .connect_to_protocol::<fidl_fuchsia_net_interfaces_admin::InstallerMarker>()
                    .expect("connect to protocol");

                let (device, port_id) = endpoint
                    .get_netdevice()
                    .panic_on_timeout(format!("get {} netdevice", name))
                    .await
                    .expect("get netdevice");
                let (device_control, device_control_server_end) = fidl::endpoints::create_proxy::<
                    fidl_fuchsia_net_interfaces_admin::DeviceControlMarker,
                >()
                .expect("create proxy");
                let () = installer
                    .install_device(device, device_control_server_end)
                    .expect("install device");

                let (control, control_server_end) =
                    fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints()
                        .expect("create proxy");
                let () = device_control
                    .create_interface(
                        &port_id,
                        control_server_end,
                        &fidl_fuchsia_net_interfaces_admin::Options {
                            name: Some(name.to_string()),
                            metric: None,
                            ..Default::default()
                        },
                    )
                    .expect("create interface");
                let iface_id = control
                    .get_id()
                    .panic_on_timeout(format!("get {} interface id", name))
                    .await
                    .expect("get id");

                let did_enable = control
                    .enable()
                    .panic_on_timeout(format!("enable {} interface", name))
                    .await
                    .expect("calling enable")
                    .expect("enable failed");
                assert!(did_enable);

                let (addr, address_state_provider) = match I::VERSION {
                    net_types::ip::IpVersion::V4 => {
                        let address_state_provider = interfaces::add_address_wait_assigned(
                            &control,
                            ipv4_addr,
                            fidl_fuchsia_net_interfaces_admin::AddressParameters::default(),
                        )
                        .panic_on_timeout(format!("add {} ipv4 address", name))
                        .await
                        .expect("add address");

                        // Adding addresses through Control does not add the subnet
                        // routes.
                        let stack = realm
                            .connect_to_protocol::<fidl_fuchsia_net_stack::StackMarker>()
                            .expect("connect to protocol");
                        let () = stack
                            .add_forwarding_entry(&mut fidl_fuchsia_net_stack::ForwardingEntry {
                                subnet: SUBNET,
                                device_id: iface_id,
                                next_hop: None,
                                metric: 0,
                            })
                            .panic_on_timeout(format!("add {} route", name))
                            .await
                            .expect("send add route")
                            .expect("add route");
                        let fidl_fuchsia_net_ext::IpAddress(addr) = ipv4_addr.addr.into();
                        (addr, Some(address_state_provider))
                    }
                    net_types::ip::IpVersion::V6 => {
                        let ipv6 = netstack_testing_common::interfaces::wait_for_v6_ll(
                            &realm
                                .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
                                .expect("connect to protocol"),
                            iface_id,
                        )
                        .panic_on_timeout(format!("wait for {} ipv6 address", name))
                        .await
                        .expect("get ipv6 link local");
                        (net_types::ip::IpAddr::V6(ipv6).into(), None)
                    }
                };
                RealmInfo {
                    realm,
                    addr,
                    iface_id,
                    endpoint,
                    device_control,
                    control,
                    address_state_provider,
                }
            }
        },
    );
    futures::pin_mut!(realms_stream);

    // Can't drop any of the fields of RealmInfo to maintain objects alive.
    let RealmInfo {
        realm: alice_realm,
        endpoint: _alice_endpoint,
        addr: alice_addr,
        iface_id: alice_iface_id,
        device_control: _alice_device_control,
        control: _alice_control,
        address_state_provider: _alice_asp,
    } = realms_stream
        .next()
        .panic_on_timeout("setup alice fixture")
        .await
        .expect("create alice realm");
    let RealmInfo {
        realm: bob_realm,
        endpoint: _bob_endpoint,
        addr: bob_addr,
        iface_id: _,
        device_control: _bob_device_control,
        control: _bob_control,
        address_state_provider: _bob_asp,
    } = realms_stream
        .next()
        .panic_on_timeout("setup bob fixture")
        .await
        .expect("create alice realm");

    const PORT: u16 = 8080;
    let (bob_addr, bind_ip) = match bob_addr {
        std::net::IpAddr::V4(addr) => {
            (std::net::SocketAddrV4::new(addr, PORT).into(), std::net::Ipv4Addr::UNSPECIFIED.into())
        }
        std::net::IpAddr::V6(addr) => (
            std::net::SocketAddrV6::new(
                addr,
                PORT,
                0,
                alice_iface_id.try_into().expect("doesn't fit scope id"),
            )
            .into(),
            std::net::Ipv6Addr::UNSPECIFIED.into(),
        ),
    };
    let alice_sock = fuchsia_async::net::UdpSocket::bind_in_realm(
        &alice_realm,
        std::net::SocketAddr::new(bind_ip, 0),
    )
    .panic_on_timeout("bind alice socket")
    .await
    .expect("bind alice sock");
    let bob_sock = fuchsia_async::net::UdpSocket::bind_in_realm(
        &bob_realm,
        std::net::SocketAddr::new(bind_ip, PORT),
    )
    .panic_on_timeout("bind bob socket")
    .await
    .expect("bind bob sock");

    const PAYLOAD: &'static str = "hello bob";
    let payload_bytes = PAYLOAD.as_bytes();
    assert_eq!(
        alice_sock
            .send_to(payload_bytes, bob_addr)
            .panic_on_timeout("alice sendto bob")
            .await
            .expect("sendto"),
        payload_bytes.len()
    );

    let mut buff = [0; PAYLOAD.len() + 1];
    let (read, from) = bob_sock
        .recv_from(&mut buff[..])
        .panic_on_timeout("alice recvfrom bob")
        .await
        .expect("recvfrom");
    assert_eq!(from.ip(), alice_addr);

    assert_eq!(read, payload_bytes.len());
    assert_eq!(&buff[..read], payload_bytes);
}

#[netstack_test]
async fn control_enable_disable<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let endpoint = sandbox.create_endpoint(name).await.expect("create endpoint");
    let () = endpoint.set_link_up(true).await.expect("set link up");
    let installer = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces_admin::InstallerMarker>()
        .expect("connect to protocol");

    let (device, port_id) = endpoint.get_netdevice().await.expect("get netdevice");
    let (device_control, device_control_server_end) =
        fidl::endpoints::create_proxy::<fidl_fuchsia_net_interfaces_admin::DeviceControlMarker>()
            .expect("create proxy");
    let () = installer.install_device(device, device_control_server_end).expect("install device");

    let (control, control_server_end) =
        fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints().expect("create proxy");

    let interfaces_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");
    let watcher = fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interfaces_state)
        .expect("create event stream")
        .map(|r| r.expect("watcher error"))
        .fuse();
    futures::pin_mut!(watcher);

    // Consume the watcher until we see the idle event.
    let existing = fidl_fuchsia_net_interfaces_ext::existing(
        watcher.by_ref().map(Result::<_, fidl::Error>::Ok),
        HashMap::<u64, _>::new(),
    )
    .await
    .expect("existing");
    // Only loopback should exist.
    assert_eq!(existing.len(), 1, "unexpected interfaces in existing: {:?}", existing);

    let () = device_control
        .create_interface(
            &port_id,
            control_server_end,
            &fidl_fuchsia_net_interfaces_admin::Options::default(),
        )
        .expect("create interface");
    let iface_id = control.get_id().await.expect("get id");

    // Expect the added event.
    let event = watcher.select_next_some().await;
    assert_matches::assert_matches!(event,
        fidl_fuchsia_net_interfaces::Event::Added(
                fidl_fuchsia_net_interfaces::Properties {
                    id: Some(id), online: Some(online), ..
                },
        ) if id == iface_id && !online
    );

    // Starts disabled, it's a no-op.
    let did_disable = control.disable().await.expect("calling disable").expect("disable failed");
    assert!(!did_disable);

    // Enable and observe online.
    let did_enable = control.enable().await.expect("calling enable").expect("enable failed");
    assert!(did_enable);
    let () = watcher
        .by_ref()
        .filter_map(|event| match event {
            fidl_fuchsia_net_interfaces::Event::Changed(
                fidl_fuchsia_net_interfaces::Properties { id: Some(id), online, .. },
            ) if id == iface_id => {
                futures::future::ready(online.and_then(|online| online.then(|| ())))
            }
            event => panic!("unexpected event {:?}", event),
        })
        .select_next_some()
        .await;

    // Enable again should be no-op.
    let did_enable = control.enable().await.expect("calling enable").expect("enable failed");
    assert!(!did_enable);

    // Disable again, expect offline.
    let did_disable = control.disable().await.expect("calling disable").expect("disable failed");
    assert!(did_disable);
    let () = watcher
        .filter_map(|event| match event {
            fidl_fuchsia_net_interfaces::Event::Changed(
                fidl_fuchsia_net_interfaces::Properties { id: Some(id), online, .. },
            ) if id == iface_id => {
                futures::future::ready(online.and_then(|online| (!online).then(|| ())))
            }
            event => panic!("unexpected event {:?}", event),
        })
        .select_next_some()
        .await;
}

#[netstack_test]
async fn link_state_interface_state_interaction<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("new sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let device = sandbox.create_endpoint(name).await.expect("create endpoint");
    let interface = device.into_interface_in_realm(&realm).await.expect("add endpoint to Netstack");
    let iface_id = interface.id();

    interface.set_link_up(false).await.expect("bring device ");

    // Setup the interface watcher.
    let interfaces_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");
    let watcher = fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interfaces_state)
        .expect("create event stream")
        .map(|r| r.expect("watcher error"))
        .fuse();
    futures::pin_mut!(watcher);
    // Consume the watcher until we see the idle event.
    let existing = fidl_fuchsia_net_interfaces_ext::existing(
        watcher.by_ref().map(Result::<_, fidl::Error>::Ok),
        HashMap::<u64, _>::new(),
    )
    .await
    .expect("existing");
    assert_matches!(
        existing.get(&iface_id),
        Some(fidl_fuchsia_net_interfaces_ext::Properties { online: false, .. })
    );

    // Map the `watcher` to only produce `Events` when `online` changes.
    let watcher =
        watcher.filter_map(|event| match event {
            fidl_fuchsia_net_interfaces::Event::Changed(
                fidl_fuchsia_net_interfaces::Properties { id: Some(id), online, .. },
            ) if id == iface_id => futures::future::ready(online),
            event => panic!("unexpected event {:?}", event),
        });
    futures::pin_mut!(watcher);

    // Helper function that polls the watcher and panics if `online` changes.
    async fn expect_online_not_changed<S: futures::Stream<Item = bool> + std::marker::Unpin>(
        mut watcher: S,
        iface_id: u64,
    ) -> S {
        watcher
            .next()
            .map(|online: Option<bool>| match online {
                None => panic!("stream unexpectedly ended"),
                Some(online) => {
                    panic!("online unexpectedly changed to {} for {}", online, iface_id)
                }
            })
            .on_timeout(
                fuchsia_async::Time::after(
                    netstack_testing_common::ASYNC_EVENT_NEGATIVE_CHECK_TIMEOUT,
                ),
                || (),
            )
            .await;
        watcher
    }

    // Set the link up, and observe no change in interface state (because the
    // interface is still disabled).
    interface.set_link_up(true).await.expect("bring device up");
    let watcher = expect_online_not_changed(watcher, iface_id).await;
    // Set the link down, and observe no change in interface state.
    interface.set_link_up(false).await.expect("bring device down");
    let watcher = expect_online_not_changed(watcher, iface_id).await;
    // Enable the interface, and observe no change in interface state (because
    // the link is still down).
    assert!(interface.control().enable().await.expect("send enable").expect("enable"));
    let mut watcher = expect_online_not_changed(watcher, iface_id).await;
    // Set the link up, and observe the interface is online.
    interface.set_link_up(true).await.expect("bring device up");
    let online = watcher.next().await.expect("stream unexpectedly ended");
    assert!(online);
    // Set the link down and observe the interface is offline.
    interface.set_link_up(false).await.expect("bring device down");
    let online = watcher.next().await.expect("stream unexpectedly ended");
    assert!(!online);
}

enum SetupOrder {
    PreferredInterfaceFirst,
    PreferredInterfaceLast,
}

#[netstack_test]
#[test_case(SetupOrder::PreferredInterfaceFirst; "setup_preferred_interface_first")]
#[test_case(SetupOrder::PreferredInterfaceLast; "setup_preferred_interface_last")]
// Verify that interfaces with lower routing metrics are preferred.
//
// Setup two test realms (Netstacks) connected by an underlying network. On the
// send side, install two interfaces with differing metrics. On the receive side
// install a single interface. Configure all interfaces with differing IPs
// within the same subnet (and add subnet routes). Finally, verify (by checking
// the source addr) that sending from the send side to the receive side uses the
// preferred send interface.
async fn interface_routing_metric<N: Netstack, I: net_types::ip::Ip>(
    name: &str,
    order: SetupOrder,
) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let send_realm =
        sandbox.create_netstack_realm::<N, _>(format!("{name}_send")).expect("create realm");
    let recv_realm =
        sandbox.create_netstack_realm::<N, _>(format!("{name}_recv")).expect("create realm");
    let network = sandbox.create_network(name).await.expect("create network");

    async fn setup_interface_in_realm<'a>(
        metric: Option<u32>,
        addr_subnet: fnet::Subnet,
        name: &'static str,
        realm: &netemul::TestRealm<'a>,
        network: &netemul::TestNetwork<'a>,
    ) -> netemul::TestInterface<'a> {
        let interface = realm
            .join_network_with_if_config(
                network,
                name,
                netemul::InterfaceConfig { name: None, metric },
            )
            .await
            .expect("install interface");
        interface.add_address_and_subnet_route(addr_subnet).await.expect("configure address");

        interface
    }

    const LESS_PREFERRED_METRIC: Option<u32> = Some(100);
    const MORE_PREFERRED_METRIC: Option<u32> = Some(50);
    let (send_addr_sub1, send_addr_sub2, recv_addr_sub) = match I::VERSION {
        IpVersion::V4 => (
            fidl_subnet!("192.168.0.1/24"),
            fidl_subnet!("192.168.0.2/24"),
            fidl_subnet!("192.168.0.3/24"),
        ),
        IpVersion::V6 => {
            (fidl_subnet!("ff::1/64"), fidl_subnet!("ff::2/64"), fidl_subnet!("ff::3/64"))
        }
    };
    let (metric1, metric2, expected_src_addr) = match order {
        SetupOrder::PreferredInterfaceFirst => {
            (MORE_PREFERRED_METRIC, LESS_PREFERRED_METRIC, send_addr_sub1.addr)
        }
        SetupOrder::PreferredInterfaceLast => {
            (LESS_PREFERRED_METRIC, MORE_PREFERRED_METRIC, send_addr_sub2.addr)
        }
    };

    let _send_interface1: netemul::TestInterface<'_> =
        setup_interface_in_realm(metric1, send_addr_sub1, "send-interface1", &send_realm, &network)
            .await;
    let _send_interface2: netemul::TestInterface<'_> =
        setup_interface_in_realm(metric2, send_addr_sub2, "send-interface2", &send_realm, &network)
            .await;
    let _recv_interface: netemul::TestInterface<'_> = setup_interface_in_realm(
        None,
        recv_addr_sub.clone(),
        "recv-interface",
        &recv_realm,
        &network,
    )
    .await;

    async fn create_socket_in_realm<I: net_types::ip::Ip>(
        realm: &netemul::TestRealm<'_>,
        port: u16,
    ) -> UdpSocket {
        let (domain, addr) = match I::VERSION {
            IpVersion::V4 => (
                fposix_socket::Domain::Ipv4,
                std::net::SocketAddr::from((std::net::Ipv4Addr::UNSPECIFIED, port)),
            ),
            IpVersion::V6 => (
                fposix_socket::Domain::Ipv6,
                std::net::SocketAddr::from((std::net::Ipv6Addr::UNSPECIFIED, port)),
            ),
        };
        let socket = realm
            .datagram_socket(domain, fposix_socket::DatagramSocketProtocol::Udp)
            .await
            .expect("failed to create socket");
        socket.bind(&addr.into()).expect("failed to bind socket");
        UdpSocket::from_datagram(DatagramSocket::new_from_socket(socket).unwrap()).unwrap()
    }
    const PORT: u16 = 999;
    let send_socket = create_socket_in_realm::<I>(&send_realm, PORT).await;
    let recv_socket = create_socket_in_realm::<I>(&recv_realm, PORT).await;

    let to_socket_addr = |addr: fnet::IpAddress| -> std::net::SocketAddr {
        let fnet_ext::IpAddress(addr) = addr.into();
        std::net::SocketAddr::from((addr, PORT))
    };

    const BUF: &str = "HELLO WORLD";
    assert_eq!(
        send_socket
            .send_to(BUF.as_bytes(), to_socket_addr(recv_addr_sub.addr))
            .await
            .expect("send failed"),
        BUF.len()
    );

    let mut buffer = [0u8; BUF.len() + 1];
    let (len, source_addr) = recv_socket.recv_from(&mut buffer).await.expect("receive failed");
    assert_eq!(source_addr, to_socket_addr(expected_src_addr));
    assert_eq!(len, BUF.len());
    assert_eq!(&buffer[..BUF.len()], BUF.as_bytes());
}

#[netstack_test]
// Test add/remove address and observe the events in InterfaceWatcher.
async fn control_add_remove_address<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let device = sandbox.create_endpoint(name).await.expect("create endpoint");
    let interface = device.into_interface_in_realm(&realm).await.expect("add endpoint to Netstack");
    assert!(interface.control().enable().await.expect("send enable").expect("enable"));
    interface.set_link_up(true).await.expect("bring device up");
    let id = interface.control().get_id().await.expect("failed to get interface id");
    let interface_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");

    async fn add_address(
        address: &mut fnet::Subnet,
        control: &fidl_fuchsia_net_interfaces_ext::admin::Control,
        id: u64,
        interface_state: &fidl_fuchsia_net_interfaces::StateProxy,
    ) -> finterfaces_admin::AddressStateProviderProxy {
        let (address_state_provider, server_end) =
            fidl::endpoints::create_proxy::<finterfaces_admin::AddressStateProviderMarker>()
                .expect("create AddressStateProvider proxy");
        control
            .add_address(
                address,
                fidl_fuchsia_net_interfaces_admin::AddressParameters::default(),
                server_end,
            )
            .expect("failed to add_address");
        interfaces::wait_for_addresses(&interface_state, id, |addresses| {
            addresses
                .iter()
                .any(|&fidl_fuchsia_net_interfaces_ext::Address { addr, valid_until: _ }| {
                    addr == *address
                })
                .then(|| ())
        })
        .await
        .expect("wait for address presence");
        address_state_provider
    }

    let addresses = [fidl_subnet!("1.1.1.1/32"), fidl_subnet!("3ffe::1/128")];
    for mut address in addresses {
        // Add an address and explicitly remove it.
        let _address_state_provider =
            add_address(&mut address, &interface.control(), id, &interface_state).await;
        let did_remove = interface
            .control()
            .remove_address(&mut address)
            .await
            .expect("failed to send remove address request")
            .expect("failed to remove address");
        assert!(did_remove);
        interfaces::wait_for_addresses(&interface_state, id, |addresses| {
            addresses
                .iter()
                .all(|&fidl_fuchsia_net_interfaces_ext::Address { addr, valid_until: _ }| {
                    addr != address
                })
                .then(|| ())
        })
        .await
        .expect("wait for address absence");

        // Add an address and drop the AddressStateProvider handle, verifying
        // the address was removed.
        let address_state_provider =
            add_address(&mut address, &interface.control(), id, &interface_state).await;
        std::mem::drop(address_state_provider);
        interfaces::wait_for_addresses(&interface_state, id, |addresses| {
            addresses
                .iter()
                .all(|&fidl_fuchsia_net_interfaces_ext::Address { addr, valid_until: _ }| {
                    addr != address
                })
                .then(|| ())
        })
        .await
        .expect("wait for address absence");
    }
}

#[netstack_test]
#[test_case(false; "no_detach")]
#[test_case(true; "detach")]
async fn control_owns_interface_lifetime<N: Netstack>(name: &str, detach: bool) {
    let detach_str = if detach { "detach" } else { "no_detach" };
    let name = format!("{}_{}", name, detach_str);

    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(&name).expect("create realm");
    let endpoint = sandbox.create_endpoint(&name).await.expect("create endpoint");
    let installer = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces_admin::InstallerMarker>()
        .expect("connect to protocol");

    let (device, port_id) = endpoint.get_netdevice().await.expect("get netdevice");
    let (device_control, device_control_server_end) =
        fidl::endpoints::create_proxy::<fidl_fuchsia_net_interfaces_admin::DeviceControlMarker>()
            .expect("create proxy");
    let () = installer.install_device(device, device_control_server_end).expect("install device");

    let (control, control_server_end) =
        fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints().expect("create proxy");

    let interfaces_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");
    let watcher = fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interfaces_state)
        .expect("create event stream")
        .map(|r| r.expect("watcher error"))
        .fuse();
    futures::pin_mut!(watcher);

    // Consume the watcher until we see the idle event.
    let existing = fidl_fuchsia_net_interfaces_ext::existing(
        watcher.by_ref().map(Result::<_, fidl::Error>::Ok),
        HashMap::<u64, _>::new(),
    )
    .await
    .expect("existing");
    // Only loopback should exist.
    assert_eq!(existing.len(), 1, "unexpected interfaces in existing: {:?}", existing);

    let () = device_control
        .create_interface(
            &port_id,
            control_server_end,
            &fidl_fuchsia_net_interfaces_admin::Options::default(),
        )
        .expect("create interface");
    let iface_id = control.get_id().await.expect("get id");

    // Expect the added event.
    let event = watcher.select_next_some().await;
    assert_matches::assert_matches!(event,
        fidl_fuchsia_net_interfaces::Event::Added(
                fidl_fuchsia_net_interfaces::Properties {
                    id: Some(id), ..
                },
        ) if id == iface_id
    );

    let debug = realm
        .connect_to_protocol::<fidl_fuchsia_net_debug::InterfacesMarker>()
        .expect("connect to protocol");
    let (debug_control, control_server_end) =
        fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints().expect("create proxy");
    let () = debug.get_admin(iface_id, control_server_end).expect("get admin");
    let same_iface_id = debug_control.get_id().await.expect("get id");
    assert_eq!(same_iface_id, iface_id);

    if detach {
        let () = control.detach().expect("detach");
        // Drop control and expect the interface to NOT be removed.
        std::mem::drop(control);
        let watcher_fut =
            watcher.select_next_some().map(|event| panic!("unexpected event {:?}", event));

        let debug_control_fut = debug_control
            .wait_termination()
            .map(|event| panic!("unexpected termination {:?}", event));

        let ((), ()) = futures::future::join(watcher_fut, debug_control_fut)
            .on_timeout(
                fuchsia_async::Time::after(
                    netstack_testing_common::ASYNC_EVENT_NEGATIVE_CHECK_TIMEOUT,
                ),
                || ((), ()),
            )
            .await;
    } else {
        // Drop control and expect the interface to be removed.
        std::mem::drop(control);

        let event = watcher.select_next_some().await;
        assert_matches::assert_matches!(event,
            fidl_fuchsia_net_interfaces::Event::Removed(id) if id == iface_id
        );

        // The debug control channel is a weak ref, it didn't prevent destruction,
        // but is closed now.
        assert_matches::assert_matches!(
            debug_control.wait_termination().await,
            fidl_fuchsia_net_interfaces_ext::admin::TerminalError::Terminal(
                fidl_fuchsia_net_interfaces_admin::InterfaceRemovedReason::User
            )
        );
    }
}

#[derive(Default, Debug, PartialEq)]
struct IpForwarding {
    v4: Option<bool>,
    v4_multicast: Option<bool>,
    v6: Option<bool>,
    v6_multicast: Option<bool>,
}

impl IpForwarding {
    // Returns the expected response when calling `get_forwarding` on
    // an interface that was previously configured using the given config.
    fn expected_next_get_forwarding_response<N: Netstack>(&self) -> IpForwarding {
        const fn false_if_none(val: Option<bool>) -> Option<bool> {
            // Manual implementation of `Option::and` since it is not yet
            // stable as a const fn.
            match val {
                None => Some(false),
                Some(v) => Some(v),
            }
        }
        match N::VERSION {
            // TODO(https://fxbug.dev/124237): Implement multicast forwarding in Netstack3.
            NetstackVersion::Netstack3 => IpForwarding {
                v4: false_if_none(self.v4),
                v4_multicast: None,
                v6: false_if_none(self.v6),
                v6_multicast: None,
            },
            NetstackVersion::Netstack2 => IpForwarding {
                v4: false_if_none(self.v4),
                v4_multicast: false_if_none(self.v4_multicast),
                v6: false_if_none(self.v6),
                v6_multicast: false_if_none(self.v6_multicast),
            },
            v @ (NetstackVersion::Netstack2WithFastUdp
            | NetstackVersion::ProdNetstack2
            | NetstackVersion::ProdNetstack3) => {
                panic!("netstack_test should only be parameterized with Netstack2 or Netstack3: got {:?}", v);
            }
        }
    }

    // Returns the expected response when calling `set_forwarding` for the first
    // time with the given config.
    fn expected_first_set_forwarding_response(&self) -> IpForwarding {
        fn false_if_some(val: Option<bool>) -> Option<bool> {
            val.and(Some(false))
        }
        IpForwarding {
            v4: false_if_some(self.v4),
            v4_multicast: false_if_some(self.v4_multicast),
            v6: false_if_some(self.v6),
            v6_multicast: false_if_some(self.v6_multicast),
        }
    }

    fn as_configuration(&self) -> finterfaces_admin::Configuration {
        let IpForwarding { v4, v4_multicast, v6, v6_multicast } = *self;
        finterfaces_admin::Configuration {
            ipv4: Some(finterfaces_admin::Ipv4Configuration {
                forwarding: v4,
                multicast_forwarding: v4_multicast,
                ..Default::default()
            }),
            ipv6: Some(finterfaces_admin::Ipv6Configuration {
                forwarding: v6,
                multicast_forwarding: v6_multicast,
                ..Default::default()
            }),
            ..Default::default()
        }
    }
}

async fn get_ip_forwarding(iface: &fnet_interfaces_ext::admin::Control) -> IpForwarding {
    let finterfaces_admin::Configuration { ipv4: ipv4_config, ipv6: ipv6_config, .. } = iface
        .get_configuration()
        .await
        .expect("get_configuration FIDL error")
        .expect("error getting configuration");

    let finterfaces_admin::Ipv4Configuration {
        forwarding: v4,
        multicast_forwarding: v4_multicast,
        ..
    } = ipv4_config.expect("IPv4 configuration should be populated");
    let finterfaces_admin::Ipv6Configuration {
        forwarding: v6,
        multicast_forwarding: v6_multicast,
        ..
    } = ipv6_config.expect("IPv6 configuration should be populated");

    IpForwarding { v4, v4_multicast, v6, v6_multicast }
}

#[netstack_test]
#[test_case(
    IpForwarding { v4: None, v4_multicast: None, v6: None, v6_multicast: None },
    None
; "set_none")]
#[test_case(
    IpForwarding { v4: Some(false), v4_multicast: None, v6: Some(false), v6_multicast: None },
    None
; "set_ip_false")]
#[test_case(
    IpForwarding { v4: Some(true), v4_multicast: None, v6: Some(false), v6_multicast: None },
    Some(finterfaces_admin::ControlSetConfigurationError::Ipv4ForwardingUnsupported)
; "set_ipv4_true")]
#[test_case(
    IpForwarding { v4: Some(false), v4_multicast: None, v6: Some(true), v6_multicast: None },
    Some(finterfaces_admin::ControlSetConfigurationError::Ipv6ForwardingUnsupported)
; "set_ipv6_true")]
#[test_case(
    IpForwarding { v4: Some(true), v4_multicast: None, v6: Some(true), v6_multicast: None },
    Some(finterfaces_admin::ControlSetConfigurationError::Ipv4ForwardingUnsupported)
; "set_ip_true")]
#[test_case(
    IpForwarding { v4: None, v4_multicast: Some(false), v6: None, v6_multicast: Some(false) },
    None
; "set_multicast_ip_false")]
#[test_case(
    IpForwarding { v4: None, v4_multicast: Some(true), v6: None, v6_multicast: Some(false) },
    Some(finterfaces_admin::ControlSetConfigurationError::Ipv4MulticastForwardingUnsupported)
; "set_multicast_ipv4_true")]
#[test_case(
    IpForwarding { v4: None, v4_multicast: Some(false), v6: None, v6_multicast: Some(true) },
    Some(finterfaces_admin::ControlSetConfigurationError::Ipv6MulticastForwardingUnsupported)
; "set_multicast_ipv6_true")]
#[test_case(
    IpForwarding { v4: None, v4_multicast: Some(true), v6: None, v6_multicast: Some(true) },
    Some(finterfaces_admin::ControlSetConfigurationError::Ipv4MulticastForwardingUnsupported)
; "set_multicast_ip_true")]
#[test_case(
    IpForwarding { v4: Some(true), v4_multicast: Some(false), v6: Some(true), v6_multicast: Some(false) },
    Some(finterfaces_admin::ControlSetConfigurationError::Ipv4ForwardingUnsupported)
; "set_ip_true_and_multicast_ip_false")]
#[test_case(
    IpForwarding { v4: Some(false), v4_multicast: Some(true), v6: Some(false), v6_multicast: Some(true) },
    Some(finterfaces_admin::ControlSetConfigurationError::Ipv4MulticastForwardingUnsupported)
; "set_ip_false_and_multicast_ip_true")]
#[test_case(
    IpForwarding { v4: Some(true), v4_multicast: Some(true), v6: Some(true), v6_multicast: Some(true) },
    Some(finterfaces_admin::ControlSetConfigurationError::Ipv4ForwardingUnsupported)
; "set_ip_and_multicast_ip_true")]
async fn get_set_forwarding_loopback<N: Netstack>(
    name: &str,
    forwarding_config: IpForwarding,
    expected_err: Option<finterfaces_admin::ControlSetConfigurationError>,
) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create netstack realm");
    let loopback_control = realm
        .interface_control(assert_matches::assert_matches!(
            realm.loopback_properties().await,
            Ok(Some(fnet_interfaces_ext::Properties {
                id,
                name: _,
                device_class: _,
                online: _,
                addresses: _,
                has_default_ipv4_route: _,
                has_default_ipv6_route: _,
            })) => id.get()
        ))
        .unwrap();

    let expected_get_forwarding_response_when_previously_unset =
        IpForwarding::default().expected_next_get_forwarding_response::<N>();
    // Initially, interfaces have IP forwarding disabled.
    assert_eq!(
        get_ip_forwarding(&loopback_control).await,
        expected_get_forwarding_response_when_previously_unset
    );

    assert_eq!(
        loopback_control
            .set_configuration(forwarding_config.as_configuration())
            .await
            .expect("set_configuration FIDL error"),
        expected_err.map_or_else(
            || Ok(forwarding_config.expected_first_set_forwarding_response().as_configuration()),
            Err,
        ),
    );
    // The configuration should not have changed.
    assert_eq!(
        get_ip_forwarding(&loopback_control).await,
        expected_get_forwarding_response_when_previously_unset,
    );
}

#[netstack_test]
#[test_case(IpForwarding { v4: None, v4_multicast: None, v6: None, v6_multicast: None }; "set_none")]
#[test_case(IpForwarding { v4: Some(false), v4_multicast: None, v6: Some(false), v6_multicast: None }; "set_ip_false")]
#[test_case(IpForwarding { v4: Some(true), v4_multicast: None, v6: Some(false), v6_multicast: None }; "set_ipv4_true")]
#[test_case(IpForwarding { v4: Some(false), v4_multicast: None, v6: Some(true), v6_multicast: None }; "set_ipv6_true")]
#[test_case(IpForwarding { v4: Some(true), v4_multicast: None, v6: Some(true), v6_multicast: None }; "set_ip_true")]
#[test_case(IpForwarding { v4: None, v4_multicast: Some(false), v6: None, v6_multicast: Some(false) }; "set_multicast_ip_false")]
#[test_case(IpForwarding { v4: None, v4_multicast: Some(true), v6: None, v6_multicast: Some(false) }; "set_multicast_ipv4_true")]
#[test_case(IpForwarding { v4: None, v4_multicast: Some(false), v6: None, v6_multicast: Some(true) }; "set_multicast_ipv6_true")]
#[test_case(IpForwarding { v4: None, v4_multicast: Some(true), v6: None, v6_multicast: Some(true) }; "set_multicast_ip_true")]
#[test_case(IpForwarding { v4: Some(true), v4_multicast: Some(false), v6: Some(true), v6_multicast: Some(false) }; "set_ip_true_and_multicast_ip_false")]
#[test_case(IpForwarding { v4: Some(false), v4_multicast: Some(true), v6: Some(false), v6_multicast: Some(true) }; "set_ip_false_and_multicast_ip_true")]
#[test_case(IpForwarding { v4: Some(true), v4_multicast: Some(true), v6: Some(true), v6_multicast: Some(true) }; "set_ip_and_multicast_ip_true")]
async fn get_set_forwarding<N: Netstack>(name: &str, forwarding_config: IpForwarding) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create netstack realm");
    let net = sandbox.create_network("net").await.expect("create network");
    let iface1 = realm.join_network(&net, "iface1").await.expect("create iface1");
    let iface2 = realm.join_network(&net, "iface2").await.expect("create iface1");

    let expected_get_forwarding_response_when_previously_unset =
        IpForwarding::default().expected_next_get_forwarding_response::<N>();
    // Initially, interfaces have IP forwarding disabled.
    assert_eq!(
        get_ip_forwarding(iface1.control()).await,
        expected_get_forwarding_response_when_previously_unset
    );
    assert_eq!(
        get_ip_forwarding(iface2.control()).await,
        expected_get_forwarding_response_when_previously_unset
    );

    /// Sets the forwarding configuration and checks the configuration before
    /// the update was applied.
    async fn set_ip_forwarding(
        iface: &netemul::TestInterface<'_>,
        enable: &IpForwarding,
        expected_previous: &IpForwarding,
    ) {
        let configuration = iface
            .control()
            .set_configuration(enable.as_configuration())
            .await
            .expect("set_configuration FIDL error")
            .expect("error setting configuration");

        assert_eq!(configuration, expected_previous.as_configuration())
    }

    set_ip_forwarding(
        &iface1,
        &forwarding_config,
        &forwarding_config.expected_first_set_forwarding_response(),
    )
    .await;
    let expected_get_forwarding_response_after_set =
        forwarding_config.expected_next_get_forwarding_response::<N>();
    assert_eq!(
        get_ip_forwarding(iface1.control()).await,
        expected_get_forwarding_response_after_set
    );
    assert_eq!(
        get_ip_forwarding(iface2.control()).await,
        expected_get_forwarding_response_when_previously_unset
    );

    // Setting the same config should be a no-op.
    set_ip_forwarding(&iface1, &forwarding_config, &forwarding_config).await;
    assert_eq!(
        get_ip_forwarding(iface1.control()).await,
        expected_get_forwarding_response_after_set,
    );
    assert_eq!(
        get_ip_forwarding(iface2.control()).await,
        expected_get_forwarding_response_when_previously_unset
    );

    // Modifying an interface's IP forwarding should not affect another
    // interface/protocol.
    let reverse_if_some = |val: Option<bool>| val.map(bool::not);
    let reversed_forwarding_config = IpForwarding {
        v4: reverse_if_some(forwarding_config.v4),
        v4_multicast: reverse_if_some(forwarding_config.v4_multicast),
        v6: reverse_if_some(forwarding_config.v6),
        v6_multicast: reverse_if_some(forwarding_config.v6_multicast),
    };
    set_ip_forwarding(
        &iface2,
        &reversed_forwarding_config,
        &reversed_forwarding_config.expected_first_set_forwarding_response(),
    )
    .await;
    let expected_get_forwarding_response_after_reverse =
        reversed_forwarding_config.expected_next_get_forwarding_response::<N>();
    assert_eq!(
        get_ip_forwarding(iface2.control()).await,
        expected_get_forwarding_response_after_reverse,
    );
    assert_eq!(
        get_ip_forwarding(iface1.control()).await,
        expected_get_forwarding_response_after_set
    );

    // Unset forwarding.
    set_ip_forwarding(&iface1, &reversed_forwarding_config, &forwarding_config).await;
    assert_eq!(
        get_ip_forwarding(iface1.control()).await,
        expected_get_forwarding_response_after_reverse
    );
}

// Test that reinstalling a port with the same base port identifier works.
#[netstack_test]
async fn reinstall_same_port<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");

    let installer = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces_admin::InstallerMarker>()
        .expect("connect to protocol");

    let (tun_dev, device) = create_tun_device();

    let (device_control, device_control_server_end) =
        fidl::endpoints::create_proxy::<fidl_fuchsia_net_interfaces_admin::DeviceControlMarker>()
            .expect("create proxy");
    let () = installer.install_device(device, device_control_server_end).expect("install device");

    const PORT_ID: u8 = 15;

    for index in 0..3 {
        let (tun_port, port_server_end) =
            fidl::endpoints::create_proxy::<fidl_fuchsia_net_tun::PortMarker>()
                .expect("create proxy");
        let () = tun_dev
            .add_port(
                &fidl_fuchsia_net_tun::DevicePortConfig {
                    base: Some(fidl_fuchsia_net_tun::BasePortConfig {
                        id: Some(PORT_ID),
                        rx_types: Some(vec![fidl_fuchsia_hardware_network::FrameType::Ethernet]),
                        tx_types: Some(vec![fidl_fuchsia_hardware_network::FrameTypeSupport {
                            type_: fidl_fuchsia_hardware_network::FrameType::Ethernet,
                            features: fidl_fuchsia_hardware_network::FRAME_FEATURES_RAW,
                            supported_flags: fidl_fuchsia_hardware_network::TxFlags::empty(),
                        }]),
                        mtu: Some(netemul::DEFAULT_MTU.into()),
                        ..Default::default()
                    }),
                    mac: Some(fidl_mac!("02:03:04:05:06:07")),
                    ..Default::default()
                },
                port_server_end,
            )
            .expect("add port");

        let (dev_port, port_server_end) =
            fidl::endpoints::create_proxy::<fidl_fuchsia_hardware_network::PortMarker>()
                .expect("create proxy");

        tun_port.get_port(port_server_end).expect("get port");
        let port_id = dev_port.get_info().await.expect("get info").id.expect("missing port id");

        let (control, control_server_end) =
            fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints()
                .expect("create proxy");
        let () = device_control
            .create_interface(
                &port_id,
                control_server_end,
                &fidl_fuchsia_net_interfaces_admin::Options {
                    name: Some(format!("test{}", index)),
                    metric: None,
                    ..Default::default()
                },
            )
            .expect("create interface");

        let did_enable = control.enable().await.expect("calling enable").expect("enable");
        assert!(did_enable);

        {
            // Give the stream a clone of the port proxy. We do this in a closed
            // scope to make sure we have no references to the proxy anymore
            // when we decide to drop it to delete the port.
            let attached_stream = futures::stream::unfold(tun_port.clone(), |port| async move {
                let fidl_fuchsia_net_tun::InternalState { has_session, .. } =
                    port.watch_state().await.expect("watch state");
                Some((has_session.expect("missing session information"), port))
            });
            futures::pin_mut!(attached_stream);
            attached_stream
                .by_ref()
                .filter_map(|attached| futures::future::ready(attached.then(|| ())))
                .next()
                .await
                .expect("stream ended");

            // Drop the interface control handle.
            drop(control);

            // Wait for the session to detach.
            attached_stream
                .filter_map(|attached| futures::future::ready((!attached).then(|| ())))
                .next()
                .await
                .expect("stream ended");
        }

        tun_port.remove().expect("triggered port removal");
        // Wait for the port to close, ensuring we can safely add the port again
        // with the same ID in the next iteration.
        assert_matches::assert_matches!(
            tun_port.take_event_stream().try_next().await.expect("failed to read next event"),
            None
        );
    }
}

#[netstack_test]
async fn synchronous_remove<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create netstack realm");
    let ep = sandbox.create_endpoint(name).await.expect("create endpoint");
    let iface = ep.into_interface_in_realm(&realm).await.expect("install interface");
    iface.control().remove().await.expect("remove completes").expect("remove succeeds");

    let reason = iface.wait_removal().await.expect("wait removal");
    assert_eq!(reason, finterfaces_admin::InterfaceRemovedReason::User);
}

#[netstack_test]
async fn no_remove_loopback<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create netstack realm");
    let fidl_fuchsia_net_interfaces_ext::Properties { id, .. } = realm
        .loopback_properties()
        .await
        .expect("fetching loopback properties")
        .expect("no loopback interface");

    let control = realm.interface_control(id.get()).expect("get interface control");

    assert_eq!(
        control.remove().await.expect("remove"),
        Err(finterfaces_admin::ControlRemoveError::NotAllowed)
    );

    // Reach out again to ensure the interface hasn't been removed and that the
    // channel is still open.
    assert_eq!(control.get_id().await.expect("get id"), id.get());
}

#[netstack_test]
async fn epitaph_is_sent_after_interface_removal<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create netstack realm");
    let ep = sandbox.create_endpoint(name).await.expect("create endpoint");

    let (device, port_id) = ep.get_netdevice().await.expect("get netdevice");
    let installer = realm
        .connect_to_protocol::<finterfaces_admin::InstallerMarker>()
        .expect("connect to protocol");
    let device_control = {
        let (control, server_end) =
            fidl::endpoints::create_proxy::<finterfaces_admin::DeviceControlMarker>()
                .expect("create proxy");
        installer.install_device(device, server_end).expect("install device");
        control
    };

    // Remove and reinstall the interface from the same port on the same device
    // multiple times to race observing the epitaph with adding the new
    // interface. If the epitaph is sent early, creating the interface will fail
    // with `PortAlreadyBound`.
    for _ in 0..10 {
        let (control, server_end) =
            fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints()
                .expect("create endpoints");
        device_control
            .create_interface(
                &port_id,
                server_end,
                &finterfaces_admin::Options {
                    name: Some("testif".to_string()),
                    ..Default::default()
                },
            )
            .expect("create interface");

        control.remove().await.expect("remove completes").expect("remove succeeds");

        assert_matches!(
            control.wait_termination().await,
            fidl_fuchsia_net_interfaces_ext::admin::TerminalError::Terminal(
                finterfaces_admin::InterfaceRemovedReason::User
            )
        );
    }
}
