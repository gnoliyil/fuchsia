// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use assert_matches::assert_matches;
use fidl_fuchsia_hardware_network as fhardware_network;
use fidl_fuchsia_net as fnet;
use fidl_fuchsia_net_debug as fnet_debug;
use fidl_fuchsia_net_interfaces_admin as fnet_interfaces_admin;
use fidl_fuchsia_net_interfaces_ext as fnet_interfaces_ext;
use futures::TryStreamExt as _;
use net_declare::fidl_mac;
use netstack_testing_common::{
    devices::{create_tun_device, create_tun_port, install_device},
    realms::{Netstack, Netstack2, TestRealmExt as _, TestSandboxExt as _},
};
use netstack_testing_macros::variants_test;

async fn get_loopback_id(realm: &netemul::TestRealm<'_>) -> u64 {
    let fnet_interfaces_ext::Properties {
        id,
        name: _,
        device_class: _,
        online: _,
        addresses: _,
        has_default_ipv4_route: _,
        has_default_ipv6_route: _,
    } = realm.loopback_properties().await.expect("loopback properties").expect("loopback missing");
    id
}

#[fuchsia::test]
async fn get_admin_unknown() {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm =
        sandbox.create_netstack_realm::<Netstack2, _>("get_admin_unknown").expect("create realm");

    let id = get_loopback_id(&realm).await;

    let debug_interfaces =
        realm.connect_to_protocol::<fnet_debug::InterfacesMarker>().expect("connect to protocol");

    // Request unknown NIC ID, expect request channel to be closed.
    let (admin_control, server_end) =
        fidl::endpoints::create_proxy::<fnet_interfaces_admin::ControlMarker>()
            .expect("create proxy");
    let () = debug_interfaces.get_admin(id + 1, server_end).expect("get admin failed");
    assert_matches!(
        admin_control.take_event_stream().try_collect::<Vec<_>>().await.as_ref().map(Vec::as_slice),
        // TODO(https://fxbug.dev/8018): Sending epitaphs not supported in Go.
        Ok([])
    );
}

// Retrieve the MAC address for the given device id, expecting no FIDL errors.
//
// This helper extracts the MAC from its `Box` making matching easier. See
// https://doc.rust-lang.org/beta/unstable-book/language-features/box-patterns.html.
async fn get_mac(
    id: u64,
    debug_interfaces: &fnet_debug::InterfacesProxy,
) -> Result<Option<fnet::MacAddress>, fnet_debug::InterfacesGetMacError> {
    let mac = debug_interfaces.get_mac(id).await.expect("get mac");
    mac.map(|option| option.map(|box_| *box_))
}

#[variants_test]
async fn get_mac_not_found<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let debug_interfaces =
        realm.connect_to_protocol::<fnet_debug::InterfacesMarker>().expect("connect to protocol");

    let loopback_id = get_loopback_id(&realm).await;
    // Unknown device ID produces an error.
    assert_matches!(
        get_mac(loopback_id + 1, &debug_interfaces).await,
        Err(fnet_debug::InterfacesGetMacError::NotFound)
    );
}

#[variants_test]
async fn get_mac_loopback<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let debug_interfaces =
        realm.connect_to_protocol::<fnet_debug::InterfacesMarker>().expect("connect to protocol");

    let loopback_id = get_loopback_id(&realm).await;
    // Loopback has the all-zero MAC address.
    assert_matches!(
        get_mac(loopback_id, &debug_interfaces).await,
        Ok(Some(fnet::MacAddress { octets: [0, 0, 0, 0, 0, 0] }))
    );
}

// Add a pure IP interface to the given device/port, returning the created
// `fuchsia.net.interfaces.admin/Control` handle.
async fn add_pure_ip_interface(
    network_port: &fhardware_network::PortProxy,
    admin_device_control: &fnet_interfaces_admin::DeviceControlProxy,
    interface_name: &str,
) -> fnet_interfaces_admin::ControlProxy {
    let fhardware_network::PortInfo { id, .. } = network_port.get_info().await.expect("get info");
    let mut port_id = id.expect("port id");

    let (admin_control, server_end) =
        fidl::endpoints::create_proxy::<fnet_interfaces_admin::ControlMarker>()
            .expect("create proxy");

    let () = admin_device_control
        .create_interface(
            &mut port_id,
            server_end,
            fnet_interfaces_admin::Options {
                name: Some(interface_name.to_string()),
                ..fnet_interfaces_admin::Options::EMPTY
            },
        )
        .expect("create interface");
    admin_control
}

// TODO(https://fxbug.dev/100871): Parameterize by Netstack to test NS3.
#[fuchsia::test]
async fn get_mac_pure_ip() {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<Netstack2, _>("get_mac").expect("create realm");
    let debug_interfaces =
        realm.connect_to_protocol::<fnet_debug::InterfacesMarker>().expect("connect to protocol");

    const PORT_ID: u8 = 7; // Arbitrary nonzero to avoid masking default value assumptions.
    const INTERFACE_NAME: &str = "ihazmac";
    let (tun_device, network_device) = create_tun_device();
    let admin_device_control = install_device(&realm, network_device);
    // Retain `_tun_port` to keep the FIDL channel open.
    let (_tun_port, network_port) = create_tun_port(&tun_device, Some(PORT_ID)).await;
    let admin_control =
        add_pure_ip_interface(&network_port, &admin_device_control, INTERFACE_NAME).await;
    let virtual_id = admin_control.get_id().await.expect("get id");
    // Pure IP interfaces do not have MAC addresses.
    assert_matches!(get_mac(virtual_id, &debug_interfaces).await, Ok(None));
}

#[variants_test]
async fn get_mac_netemul_endpoint<N: Netstack, E: netemul::Endpoint>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let debug_interfaces =
        realm.connect_to_protocol::<fnet_debug::InterfacesMarker>().expect("connect to protocol");

    const DEFAULT_MAC: fnet::MacAddress = fidl_mac!("00:03:00:00:00:00");
    let device = sandbox
        .create_endpoint_with("get_mac", E::make_config(netemul::DEFAULT_MTU, Some(DEFAULT_MAC)))
        .await
        .expect("create netemul endpoint");
    // Retain `_control` and `_device_control` to keep the FIDL channel open.
    let (id, _control, _device_control) = device
        .add_to_stack(&realm, netemul::InterfaceConfig::default())
        .await
        .expect("add to stack");
    assert_matches!(get_mac(id.into(), &debug_interfaces).await, Ok(Some(DEFAULT_MAC)));
}
