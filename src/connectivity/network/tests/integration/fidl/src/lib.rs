// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use fidl_fuchsia_net as fnet;
use fidl_fuchsia_net_ext::{IntoExt as _, NetTypesIpAddressExt};
use fidl_fuchsia_net_interfaces_admin as finterfaces_admin;
use fidl_fuchsia_net_stack as fnet_stack;
use fidl_fuchsia_netemul as fnetemul;
use fuchsia_async::{DurationExt as _, TimeoutExt as _};

use futures::{FutureExt as _, StreamExt as _, TryStreamExt as _};
use net_declare::{fidl_mac, fidl_subnet, net_mac, std_ip_v4, std_ip_v6, std_socket_addr};
use netemul::RealmUdpSocket as _;
use netstack_testing_common::{
    get_component_moniker,
    interfaces::add_address_wait_assigned,
    realms::{
        constants, KnownServiceProvider, Netstack, NetstackVersion, TestRealmExt as _,
        TestSandboxExt as _,
    },
    ASYNC_EVENT_NEGATIVE_CHECK_TIMEOUT, ASYNC_EVENT_POSITIVE_CHECK_TIMEOUT,
};
use netstack_testing_macros::netstack_test;
use packet::{serialize::Serializer as _, ParsablePacket as _};
use packet_formats::{
    error::ParseError,
    ethernet::{EthernetFrame, EthernetFrameBuilder, EthernetFrameLengthCheck},
    icmp::{
        IcmpEchoRequest, IcmpIpExt, IcmpMessage, IcmpPacket, IcmpPacketBuilder, IcmpParseArgs,
        IcmpUnusedCode, MessageBody as _, OriginalPacket,
    },
    ip::{IpExt, IpPacketBuilder as _},
};
use test_case::test_case;

#[netstack_test]
async fn log_packets<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    // Modify debug netstack args so that it does not log packets.
    let (realm, stack_log) = {
        let mut netstack = fnetemul::ChildDef::from(&KnownServiceProvider::Netstack(N::VERSION));
        let fnetemul::ChildDef { program_args, .. } = &mut netstack;
        assert_eq!(
            std::mem::replace(program_args, Some(vec!["--verbosity=debug".to_string()])),
            None,
        );
        let realm = sandbox.create_realm(name, [netstack]).expect("create realm");

        let netstack_proxy =
            realm.connect_to_protocol::<fnet_stack::LogMarker>().expect("connect to netstack");
        (realm, netstack_proxy)
    };
    let () = stack_log.set_log_packets(true).await.expect("enable packet logging");

    let sock =
        fuchsia_async::net::UdpSocket::bind_in_realm(&realm, std_socket_addr!("127.0.0.1:0"))
            .await
            .expect("create socket");
    let addr = sock.local_addr().expect("get bound socket address");
    const PAYLOAD: [u8; 4] = [1u8, 2, 3, 4];
    let sent = sock.send_to(&PAYLOAD[..], addr).await.expect("send_to failed");
    assert_eq!(sent, PAYLOAD.len());

    let patterns = ["send", "recv"]
        .iter()
        .map(|t| format!("{} udp {} -> {} len:{}", t, addr, addr, PAYLOAD.len()))
        .collect::<Vec<_>>();

    let netstack_moniker = get_component_moniker(&realm, constants::netstack::COMPONENT_NAME)
        .await
        .expect("get netstack moniker");
    let stream = diagnostics_reader::ArchiveReader::new()
        .select_all_for_moniker(&netstack_moniker)
        .snapshot_then_subscribe()
        .expect("subscribe to snapshot");

    let () = async_utils::fold::try_fold_while(stream, patterns, |mut patterns, data| {
        let () = patterns
            .retain(|pattern| !data.msg().map(|msg| msg.contains(pattern)).unwrap_or(false));
        futures::future::ok(if patterns.is_empty() {
            async_utils::fold::FoldWhile::Done(())
        } else {
            async_utils::fold::FoldWhile::Continue(patterns)
        })
    })
    .await
    .expect("observe expected patterns")
    .short_circuited()
    .unwrap_or_else(|patterns| {
        panic!("log stream ended while still waiting for patterns {:?}", patterns)
    });
}

const IPV4_LOOPBACK: fidl_fuchsia_net::Subnet = fidl_subnet!("127.0.0.1/8");
const IPV6_LOOPBACK: fidl_fuchsia_net::Subnet = fidl_subnet!("::1/128");

#[netstack_test]
async fn add_remove_address_on_loopback<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");

    let (loopback_id, addresses) = assert_matches::assert_matches!(
        realm.loopback_properties().await,
        Ok(Some(
            fidl_fuchsia_net_interfaces_ext::Properties {
                id,
                online: true,
                addresses,
                ..
            },
        )) => (id, addresses)
    );
    let addresses: Vec<_> = addresses
        .into_iter()
        .map(|fidl_fuchsia_net_interfaces_ext::Address { addr, .. }| addr)
        .collect();
    assert_eq!(addresses[..], [IPV4_LOOPBACK, IPV6_LOOPBACK]);

    let debug = realm
        .connect_to_protocol::<fidl_fuchsia_net_debug::InterfacesMarker>()
        .expect("connect to protocol");

    let (control, server_end) =
        fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints().expect("create proxy");
    let () = debug.get_admin(loopback_id, server_end).expect("get admin");

    futures::stream::iter([IPV4_LOOPBACK, IPV6_LOOPBACK].into_iter())
        .for_each_concurrent(None, |mut addr| {
            let control = &control;
            async move {
                let did_remove = control
                    .remove_address(&mut addr)
                    .await
                    .expect("remove_address")
                    .expect("remove address");
                // Netstack3 does not allow addresses to be removed from the loopback device, for
                // some reason?
                if N::VERSION == NetstackVersion::Netstack3 {
                    assert!(!did_remove, "{:?}", addr);
                } else {
                    assert!(did_remove, "{:?}", addr);
                }
            }
        })
        .await;

    futures::stream::iter([fidl_subnet!("1.1.1.1/24"), fidl_subnet!("a::1/64")].into_iter())
        .for_each_concurrent(None, |addr| {
            add_address_wait_assigned(&control, addr, finterfaces_admin::AddressParameters::EMPTY)
                .map(|res| {
                    let _: finterfaces_admin::AddressStateProviderProxy = res.expect("add address");
                })
        })
        .await;
}

#[netstack_test]
async fn disable_interface_loopback<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");

    let interface_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");

    let stream = fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)
        .expect("get interface event stream");
    futures::pin_mut!(stream);

    let loopback_id = assert_matches::assert_matches!(
        stream.try_next().await,
        Ok(Some(fidl_fuchsia_net_interfaces::Event::Existing(
            fidl_fuchsia_net_interfaces::Properties {
                id: Some(id),
                device_class:
                    Some(fidl_fuchsia_net_interfaces::DeviceClass::Loopback(
                        fidl_fuchsia_net_interfaces::Empty {},
                    )),
                online: Some(true),
                ..
            },
        ))) => id
    );

    let () = assert_matches::assert_matches!(
        stream.try_next().await,
        Ok(Some(fidl_fuchsia_net_interfaces::Event::Idle(
            fidl_fuchsia_net_interfaces::Empty {},
        ))) => ()
    );

    let debug = realm
        .connect_to_protocol::<fidl_fuchsia_net_debug::InterfacesMarker>()
        .expect("connect to protocol");

    let (control, server_end) =
        fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints().expect("create proxy");
    let () = debug.get_admin(loopback_id, server_end).expect("get admin");

    let did_disable = control.disable().await.expect("send disable").expect("disable");
    assert!(did_disable);

    // N2 emits Changed events for ::1 disappearing from the loopback
    // interface first, so consume from the stream until a Changed event
    // indicating interface offline is observed.
    stream.filter_map(|event| {
        let online = assert_matches::assert_matches!(
            event,
            Ok(fidl_fuchsia_net_interfaces::Event::Changed(fidl_fuchsia_net_interfaces::Properties {
                id: Some(id),
                online,
                ..
            })) if id == loopback_id => online
        );
        futures::future::ready(online.map(|online| if online {
            panic!("online changed unexpectedly to true");
        }))
    })
    .next()
    .await
    .expect("interface watcher stream ended unexpectedly")
}

#[netstack_test]
async fn reject_multicast_mac_address<N: Netstack>(name: &str) {
    const BAD_MAC_ADDRESS: net_types::ethernet::Mac = net_mac!("CF:AA:BB:CC:DD:EE");
    assert_eq!(net_types::UnicastAddr::new(BAD_MAC_ADDRESS), None);

    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let net = sandbox.create_network("net").await.expect("created network");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let result = realm
        .join_network_with(
            &net,
            "host",
            netemul::new_endpoint_config(
                netemul::DEFAULT_MTU,
                Some(fnet::MacAddress { octets: BAD_MAC_ADDRESS.bytes() }),
            ),
            Default::default(),
        )
        .await;
    assert_matches::assert_matches!(result, Err(_));
}

enum ForwardingConfiguration {
    BothIfaces(fidl_fuchsia_net::IpVersion),
    Iface1Only(fidl_fuchsia_net::IpVersion),
    Iface2Only(fidl_fuchsia_net::IpVersion),
}

struct ForwardingTestCase<I: IcmpIpExt> {
    iface1_addr: fidl_fuchsia_net::Subnet,
    iface2_addr: fidl_fuchsia_net::Subnet,
    forwarding_config: Option<ForwardingConfiguration>,
    src_ip: I::Addr,
    dst_ip: I::Addr,
    expect_forward: bool,
}

fn test_forwarding_v4(
    forwarding_config: Option<ForwardingConfiguration>,
    expect_forward: bool,
) -> ForwardingTestCase<net_types::ip::Ipv4> {
    ForwardingTestCase {
        iface1_addr: fidl_subnet!("192.168.1.1/24"),
        iface2_addr: fidl_subnet!("192.168.2.1/24"),
        forwarding_config,
        // TODO(https://fxbug.dev/77901): Use `std_ip_v4!(..).into()`.
        // TODO(https://fxbug.dev/77965): Use `net_declare` macros to create
        // `net_types` addresses.
        src_ip: net_types::ip::Ipv4Addr::new(std_ip_v4!("192.168.1.2").octets()),
        dst_ip: net_types::ip::Ipv4Addr::new(std_ip_v4!("192.168.2.2").octets()),
        expect_forward,
    }
}

fn test_forwarding_v6(
    forwarding_config: Option<ForwardingConfiguration>,
    expect_forward: bool,
) -> ForwardingTestCase<net_types::ip::Ipv6> {
    ForwardingTestCase {
        iface1_addr: fidl_subnet!("a::1/64"),
        iface2_addr: fidl_subnet!("b::1/64"),
        forwarding_config,
        // TODO(https://fxbug.dev/77901): Use `std_ip_v6!(..).into()`.
        // TODO(https://fxbug.dev/77965): Use `net_declare` macros to create
        // `net_types` addresses.
        src_ip: net_types::ip::Ipv6Addr::from_bytes(std_ip_v6!("a::2").octets()),
        dst_ip: net_types::ip::Ipv6Addr::from_bytes(std_ip_v6!("b::2").octets()),
        expect_forward,
    }
}

#[netstack_test]
#[test_case(
    "v4_none_forward_icmp_v4",
    test_forwarding_v4(
        None,
        false,
    ); "v4_none_forward_icmp_v4")]
#[test_case(
    "v4_all_forward_icmp_v4",
    test_forwarding_v4(
        Some(ForwardingConfiguration::BothIfaces(fidl_fuchsia_net::IpVersion::V4)),
        true,
    ); "v4_all_forward_icmp_v4")]
#[test_case(
    "v4_iface1_forward_v4_icmp_v4",
    test_forwarding_v4(
        Some(ForwardingConfiguration::Iface1Only(fidl_fuchsia_net::IpVersion::V4)),
        true,
    ); "v4_iface1_forward_v4_icmp_v4")]
#[test_case(
    "v4_iface1_forward_v6_icmp_v4",
    test_forwarding_v4(
        Some(ForwardingConfiguration::Iface1Only(fidl_fuchsia_net::IpVersion::V6)),
        false,
    ); "v4_iface1_forward_v6_icmp_v4")]
#[test_case(
    "v4_iface2_forward_v4_icmp_v4",
    test_forwarding_v4(
        Some(ForwardingConfiguration::Iface2Only(fidl_fuchsia_net::IpVersion::V4)),
        false,
    ); "v4_iface2_forward_v4_icmp_v4")]
#[test_case(
    "v6_none_forward_icmp_v6",
    test_forwarding_v6(
        None,
        false,
    ); "v6_none_forward_icmp_v6")]
#[test_case(
    "v6_all_forward_icmp_v6",
    test_forwarding_v6(
        Some(ForwardingConfiguration::BothIfaces(fidl_fuchsia_net::IpVersion::V6)),
        true,
    ); "v6_all_forward_icmp_v6")]
#[test_case(
    "v6_iface1_forward_v6_icmp_v6",
    test_forwarding_v6(
        Some(ForwardingConfiguration::Iface1Only(fidl_fuchsia_net::IpVersion::V6)),
        true,
    ); "v6_iface1_forward_v6_icmp_v6")]
#[test_case(
    "v6_iface1_forward_v4_icmp_v6",
    test_forwarding_v6(
        Some(ForwardingConfiguration::Iface1Only(fidl_fuchsia_net::IpVersion::V4)),
        false,
    ); "v6_iface1_forward_v4_icmp_v6")]
#[test_case(
    "v6_iface2_forward_v6_icmp_v6",
    test_forwarding_v6(
        Some(ForwardingConfiguration::Iface2Only(fidl_fuchsia_net::IpVersion::V6)),
        false,
    ); "v6_iface2_forward_v6_icmp_v6")]
async fn test_forwarding<I: IpExt + IcmpIpExt, N: Netstack>(
    test_name: &str,
    sub_test_name: &str,
    test_case: ForwardingTestCase<I>,
) where
    IcmpEchoRequest:
        for<'a> IcmpMessage<I, &'a [u8], Code = IcmpUnusedCode, Body = OriginalPacket<&'a [u8]>>,
    I::Addr: NetTypesIpAddressExt,
{
    const TTL: u8 = 64;
    const ECHO_ID: u16 = 1;
    const ECHO_SEQ: u16 = 2;
    const MAC: fidl_fuchsia_net::MacAddress = fidl_mac!("02:0A:0B:0C:0D:0E");

    let ForwardingTestCase {
        iface1_addr,
        iface2_addr,
        forwarding_config,
        src_ip,
        dst_ip,
        expect_forward,
    } = test_case;

    let name = format!("{}_{}", test_name, sub_test_name);
    let name = name.as_str();

    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let sandbox = &sandbox;
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create netstack realm");
    let realm = &realm;

    let net_ep_iface = |net_num: u8, addr: fidl_fuchsia_net::Subnet| async move {
        let net = sandbox.create_network(format!("net{}", net_num)).await.expect("create network");
        let fake_ep = net.create_fake_endpoint().expect("create fake endpoint");
        let iface = realm
            .join_network(&net, format!("iface{}", net_num))
            .await
            .expect("configure networking");
        iface.add_address_and_subnet_route(addr).await.expect("configure address");

        (net, fake_ep, iface)
    };

    let (_net1, fake_ep1, iface1) = net_ep_iface(1, iface1_addr).await;
    let (_net2, fake_ep2, iface2) = net_ep_iface(2, iface2_addr).await;

    async fn enable_ip_forwarding(iface: &netemul::TestInterface<'_>, ip_version: fnet::IpVersion) {
        let config_with_ip_forwarding_set = |ip_version, forwarding| match ip_version {
            fnet::IpVersion::V4 => finterfaces_admin::Configuration {
                ipv4: Some(finterfaces_admin::Ipv4Configuration {
                    forwarding: Some(forwarding),
                    ..finterfaces_admin::Ipv4Configuration::EMPTY
                }),
                ..finterfaces_admin::Configuration::EMPTY
            },
            fnet::IpVersion::V6 => finterfaces_admin::Configuration {
                ipv6: Some(finterfaces_admin::Ipv6Configuration {
                    forwarding: Some(forwarding),
                    ..finterfaces_admin::Ipv6Configuration::EMPTY
                }),
                ..finterfaces_admin::Configuration::EMPTY
            },
        };

        let configuration = iface
            .control()
            .set_configuration(config_with_ip_forwarding_set(ip_version, true))
            .await
            .expect("set_configuration FIDL error")
            .expect("error setting configuration");

        assert_eq!(configuration, config_with_ip_forwarding_set(ip_version, false))
    }

    if let Some(config) = forwarding_config {
        match config {
            ForwardingConfiguration::BothIfaces(ip_version) => {
                enable_ip_forwarding(&iface1, ip_version).await;
                enable_ip_forwarding(&iface2, ip_version).await;
            }
            ForwardingConfiguration::Iface1Only(ip_version) => {
                enable_ip_forwarding(&iface1, ip_version).await;
            }
            ForwardingConfiguration::Iface2Only(ip_version) => {
                enable_ip_forwarding(&iface2, ip_version).await;
            }
        }
    }

    let neighbor_controller = realm
        .connect_to_protocol::<fidl_fuchsia_net_neighbor::ControllerMarker>()
        .expect("connect to protocol");
    let dst_ip_fidl: <I::Addr as NetTypesIpAddressExt>::Fidl = dst_ip.into_ext();
    let () = neighbor_controller
        .add_entry(iface2.id(), &mut dst_ip_fidl.into_ext(), &mut MAC.clone())
        .await
        .expect("add_entry FIDL error")
        .expect("error adding static entry");

    let mut icmp_body = [1, 2, 3, 4, 5, 6, 7, 8];

    let ser = packet::Buf::new(&mut icmp_body, ..)
        .encapsulate(IcmpPacketBuilder::<I, _, _>::new(
            src_ip,
            dst_ip,
            IcmpUnusedCode,
            IcmpEchoRequest::new(ECHO_ID, ECHO_SEQ),
        ))
        .encapsulate(<I as IpExt>::PacketBuilder::new(src_ip, dst_ip, TTL, I::ICMP_IP_PROTO))
        .encapsulate(EthernetFrameBuilder::new(
            net_types::ethernet::Mac::new([1, 2, 3, 4, 5, 6]),
            net_types::ethernet::Mac::BROADCAST,
            I::ETHER_TYPE,
        ))
        .serialize_vec_outer()
        .expect("serialize ICMP packet")
        .unwrap_b();

    let duration = if expect_forward {
        ASYNC_EVENT_POSITIVE_CHECK_TIMEOUT
    } else {
        ASYNC_EVENT_NEGATIVE_CHECK_TIMEOUT
    };

    let ((), forwarded) = futures::future::join(
        fake_ep1.write(ser.as_ref()).map(|r| r.expect("write to fake endpoint #1")),
        fake_ep2
            .frame_stream()
            .map(|r| r.expect("error getting OnData event"))
            .filter_map(|(data, dropped)| {
                assert_eq!(dropped, 0);

                let mut data = &data[..];

                let eth = EthernetFrame::parse(&mut data, EthernetFrameLengthCheck::NoCheck)
                    .expect("error parsing ethernet frame");

                if eth.ethertype() != Some(I::ETHER_TYPE) {
                    // Ignore other IP packets.
                    return futures::future::ready(None);
                }

                let (mut payload, src_ip, dst_ip, proto, got_ttl) =
                    packet_formats::testutil::parse_ip_packet::<I>(&data)
                        .expect("error parsing IP packet");

                if proto != I::ICMP_IP_PROTO {
                    // Ignore non-ICMP packets.
                    return futures::future::ready(None);
                }

                let icmp = match IcmpPacket::<I, _, IcmpEchoRequest>::parse(
                    &mut payload,
                    IcmpParseArgs::new(src_ip, dst_ip),
                ) {
                    Ok(o) => o,
                    Err(ParseError::NotExpected) => {
                        // Ignore non-echo request packets.
                        return futures::future::ready(None);
                    }
                    Err(e) => {
                        panic!("error parsing ICMP echo request packet: {}", e)
                    }
                };

                let echo_request = icmp.message();
                assert_eq!(echo_request.id(), ECHO_ID);
                assert_eq!(echo_request.seq(), ECHO_SEQ);
                assert_eq!(icmp.body().bytes(), icmp_body);
                assert_eq!(got_ttl, TTL - 1);

                // Our packet was forwarded.
                futures::future::ready(Some(true))
            })
            .next()
            .map(|r| r.expect("stream unexpectedly ended"))
            .on_timeout(duration.after_now(), || {
                // The packet was not forwarded.
                false
            }),
    )
    .await;

    assert_eq!(expect_forward, forwarded);
}
