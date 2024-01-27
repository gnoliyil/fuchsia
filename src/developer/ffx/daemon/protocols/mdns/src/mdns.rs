use std::os::unix::prelude::AsRawFd;

use mdns::protocol::Type;

// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::MdnsProtocolInner;
use anyhow::{Context as _, Result};
use async_io::Async;
use async_lock::Mutex;
use async_net::UdpSocket;
use ffx_config::get;
use fidl_fuchsia_developer_ffx as ffx;
use fidl_fuchsia_net::{IpAddress, Ipv4Address, Ipv6Address};
use fuchsia_async::{Task, Timer};
use futures::FutureExt;
use mdns::protocol as dns;
use netext::{get_mcast_interfaces, IsLocalAddr};
use packet::{InnerPacketBuilder, ParseBuffer};
use std::{
    collections::{HashMap, HashSet},
    fmt::Write,
    net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr},
    rc::{Rc, Weak},
    time::Duration,
};
use zerocopy::ByteSlice;

const MDNS_MCAST_V4: Ipv4Addr = Ipv4Addr::new(224, 0, 0, 251);
const MDNS_MCAST_V6: Ipv6Addr = Ipv6Addr::new(0xff02, 0, 0, 0, 0, 0, 0, 0x00fb);

#[cfg(not(test))]
const MDNS_PORT: u16 = 5353;
#[cfg(test)]
const MDNS_PORT: u16 = 0;

pub(crate) struct DiscoveryConfig {
    pub socket_tasks: Rc<Mutex<HashMap<IpAddr, Task<()>>>>,
    pub mdns_protocol: Weak<MdnsProtocolInner>,
    pub discovery_interval: Duration,
    pub query_interval: Duration,
    pub ttl: u32,
}

async fn propagate_bind_event(sock: &UdpSocket, svc: &Weak<MdnsProtocolInner>) -> u16 {
    let port = match sock.local_addr().unwrap() {
        SocketAddr::V4(s) => s.port(),
        SocketAddr::V6(s) => s.port(),
    };
    if let Some(svc) = svc.upgrade() {
        svc.publish_event(ffx::MdnsEventType::SocketBound(ffx::MdnsBindEvent {
            port: Some(port),
            ..ffx::MdnsBindEvent::EMPTY
        }))
        .await;
    }
    port
}

async fn is_mdns_enabled() -> bool {
    get("discovery.mdns.enabled").await.unwrap_or(true)
}

// discovery_loop iterates over all multicast interfaces and adds them to
// the socket_tasks if there is not already a task for that interface.
pub(crate) async fn discovery_loop(config: DiscoveryConfig) {
    let DiscoveryConfig { socket_tasks, mdns_protocol, discovery_interval, query_interval, ttl } =
        config;
    // See fxbug.dev/62617#c10 for details. A macOS system can end up in
    // a situation where the default routes for protocols are on
    // non-functional interfaces, and under such conditions the wildcard
    // listen socket binds will fail. We will repeat attempting to bind
    // them, as newly added interfaces later may unstick the issue, if
    // they introduce new routes. These boolean flags are used to
    // suppress the production of a log output in every interface
    // iteration.
    // In order to manually reproduce these conditions on a macOS
    // system, open Network.prefpane, and for each connection in the
    // list select Advanced... > TCP/IP > Configure IPv6 > Link-local
    // only. Click apply, then restart the ffx daemon.
    let mut should_log_v4_listen_error = true;
    let mut should_log_v6_listen_error = true;

    let mut v4_listen_socket: Weak<UdpSocket> = Weak::new();
    let mut v6_listen_socket: Weak<UdpSocket> = Weak::new();

    loop {
        if is_mdns_enabled().await {
            if v4_listen_socket.upgrade().is_none() {
                match make_listen_socket((MDNS_MCAST_V4, MDNS_PORT).into())
                    .context("make_listen_socket for IPv4")
                {
                    Ok(sock) => {
                        // TODO(awdavies): Networking tests appear to fail when
                        // using IPv6. Only propagates the port binding event for
                        // IPv4.
                        let _ = propagate_bind_event(&sock, &mdns_protocol).await;
                        let sock = Rc::new(sock);
                        v4_listen_socket = Rc::downgrade(&sock);
                        Task::local(recv_loop(sock, mdns_protocol.clone())).detach();
                        should_log_v4_listen_error = true;
                    }
                    Err(err) => {
                        if should_log_v4_listen_error {
                            tracing::error!(
                                "unable to bind IPv4 listen socket: {}. Discovery may fail.",
                                err
                            );
                            should_log_v4_listen_error = false;
                        }
                    }
                }
            }

            if v6_listen_socket.upgrade().is_none() {
                match make_listen_socket((MDNS_MCAST_V6, MDNS_PORT).into())
                    .context("make_listen_socket for IPv6")
                {
                    Ok(sock) => {
                        let sock = Rc::new(sock);
                        v6_listen_socket = Rc::downgrade(&sock);
                        Task::local(recv_loop(sock, mdns_protocol.clone())).detach();
                        should_log_v6_listen_error = true;
                    }
                    Err(err) => {
                        if should_log_v6_listen_error {
                            tracing::error!(
                                "unable to bind IPv6 listen socket: {}. Discovery may fail.",
                                err
                            );
                            should_log_v6_listen_error = false;
                        }
                    }
                }
            }

            // As some operating systems will not error sendmsg/recvmsg for UDP
            // sockets bound to addresses that no longer exist, they must be removed
            // by ensuring that they still exist, otherwise we may be sending out
            // unanswerable queries.
            let mut to_delete = HashSet::<IpAddr>::new();
            for ip in socket_tasks.lock().await.keys() {
                to_delete.insert(*ip);
            }

            for iface in get_mcast_interfaces().unwrap_or_default() {
                match iface.id() {
                    Ok(id) => {
                        if let Some(sock) = v6_listen_socket.upgrade() {
                            let _ = sock.join_multicast_v6(&MDNS_MCAST_V6, id);
                        }
                    }
                    Err(err) => {
                        tracing::warn!("{}", err);
                    }
                }

                for addr in iface.addrs.iter() {
                    to_delete.remove(&addr.ip());

                    let mut addr = *addr;
                    addr.set_port(0);

                    // TODO(raggi): remove duplicate joins, log unexpected errors
                    if let SocketAddr::V4(addr) = addr {
                        if let Some(sock) = v4_listen_socket.upgrade() {
                            let _ = sock.join_multicast_v4(MDNS_MCAST_V4, *addr.ip());
                        }
                    }

                    if socket_tasks.lock().await.get(&addr.ip()).is_some() {
                        continue;
                    }

                    let sock = iface
                        .id()
                        .map(|id| match make_sender_socket(id, addr, ttl) {
                            Ok(sock) => Some(sock),
                            Err(err) => {
                                tracing::error!("mdns: failed to bind {}: {}", &addr, err);
                                None
                            }
                        })
                        .ok()
                        .flatten();

                    if sock.is_some() {
                        socket_tasks.lock().await.insert(
                            addr.ip(),
                            Task::local(query_recv_loop(
                                Rc::new(sock.unwrap()),
                                mdns_protocol.clone(),
                                query_interval,
                                socket_tasks.clone(),
                            )),
                        );
                    }
                }
            }

            // Drop tasks for IP addresses no longer found on the system.
            {
                let mut tasks = socket_tasks.lock().await;
                for ip in to_delete {
                    if let Some(handle) = tasks.remove(&ip) {
                        handle.cancel().await;
                    }
                }
            }
        }

        Timer::new(discovery_interval).await;
    }
}

fn make_target<B: ByteSlice + Clone>(
    src: SocketAddr,
    msg: dns::Message<B>,
) -> Option<(ffx::TargetInfo, u32)> {
    let mut nodename = String::new();
    let mut ttl = 0u32;
    let mut ssh_port: u16 = 0;
    let mut ssh_address = None;
    let mut src = ffx::TargetAddrInfo::Ip(ffx::TargetIp {
        ip: match &src {
            SocketAddr::V6(s) => IpAddress::Ipv6(Ipv6Address { addr: s.ip().octets() }),
            SocketAddr::V4(s) => IpAddress::Ipv4(Ipv4Address { addr: s.ip().octets() }),
        },
        scope_id: if let SocketAddr::V6(s) = &src { s.scope_id() } else { 0 },
    });
    let fastboot_interface = is_fastboot_response(&msg);

    for record in msg.additional.iter() {
        match record.rtype {
            // Emulator adds Txt records to share the user mode networking configuration. This information
            // should override any A record information.
            dns::Type::Txt => {
                if let Some(data) = record.rdata.bytes() {
                    let txt_lines: Vec<String> = decode_txt_rdata(data).unwrap_or_default();
                    let mut ip_addr: Option<IpAddress> = None;
                    for txt in &txt_lines {
                        if let Some((name, value)) = txt.split_once(':') {
                            match name {
                                "host" => {
                                    if let Ok(addr) = value.parse::<Ipv4Addr>() {
                                        let ip =
                                            IpAddress::Ipv4(Ipv4Address { addr: addr.octets() });
                                        src = ffx::TargetAddrInfo::Ip(ffx::TargetIp {
                                            ip,
                                            scope_id: 0,
                                        });
                                        ip_addr = Some(ip);
                                    }
                                }
                                "ssh" => {
                                    ssh_port = value.parse().unwrap_or(22);
                                }
                                _ => {}
                            };
                        }
                    }
                    if let Some(ip) = ip_addr {
                        ssh_address = Some(ffx::TargetAddrInfo::IpPort(ffx::TargetIpPort {
                            ip,
                            scope_id: 0,
                            port: ssh_port,
                        }));
                    }
                    tracing::debug!("emulator mdns txt {:?} {:?}", &txt_lines, record.domain);
                } else {
                    tracing::debug!("no data in txt record {:?}", record.domain);
                }
            }
            dns::Type::A | dns::Type::Aaaa => {
                if nodename.is_empty() {
                    write!(nodename, "{}", record.domain).unwrap();
                    nodename = nodename.trim_end_matches(".local").into();
                }
                if ttl == 0 {
                    ttl = record.ttl;
                }
            }
            _ => {}
        };
    }

    if nodename.is_empty() || ttl == 0 {
        return None;
    }
    Some((
        ffx::TargetInfo {
            nodename: Some(nodename),
            addresses: Some(vec![src]),
            target_state: fastboot_interface.map(|_| ffx::TargetState::Fastboot),
            fastboot_interface,
            ssh_address,
            ..ffx::TargetInfo::EMPTY
        },
        ttl,
    ))
}

/// Read the bytes from the txt record. These are encoded
/// as <len><string> where len is u8. multiple strings
/// can be in encoded.
fn decode_txt_rdata(data: &[u8]) -> Result<Vec<String>> {
    // Each text element is preceded by the length
    let mut ret: Vec<String> = vec![];
    let mut pos = 0;
    while pos < data.len() {
        let l: usize = data[pos].into();
        if l == 0 {
            break;
        }
        let s = std::str::from_utf8(&data[pos + 1..pos + l + 1])?;
        ret.push(s.to_string());
        pos = pos + l + 1;
    }
    Ok(ret)
}

// recv_loop reads packets from sock. If the packet is a Fuchsia mdns packet, a
// corresponding mdns event is published to the queue. All other packets are
// silently discarded.
async fn recv_loop(sock: Rc<UdpSocket>, mdns_protocol: Weak<MdnsProtocolInner>) {
    if !is_mdns_enabled().await {
        return;
    }

    loop {
        let mut buf = &mut [0u8; 1500][..];
        let addr = match sock.recv_from(buf).await {
            Ok((sz, addr)) => {
                buf = &mut buf[..sz];
                addr
            }
            Err(err) => {
                tracing::error!("listen socket recv error: {}, mdns listener closed", err);
                return;
            }
        };

        // Note: important, otherwise non-local responders could add themselves.
        if !addr.ip().is_local_addr() {
            continue;
        }

        let msg = match buf.parse::<dns::Message<_>>() {
            Ok(msg) => msg,
            Err(e) => {
                tracing::trace!(
                    "unable to parse message received on {} from {}: {:?}",
                    sock.local_addr()
                        .map(|s| format!("{}", s))
                        .unwrap_or(format!("fd:{}", sock.as_raw_fd())),
                    addr,
                    e
                );
                continue;
            }
        };

        // Only interested in fuchsia services or fastboot.
        if !is_fuchsia_response(&msg) && is_fastboot_response(&msg).is_none() {
            continue;
        }
        // Source addresses need to be present in the response, or be a TXT record which
        // contains address information about user mode networking being used by an emulator
        // instance.
        if !contains_source_address(&addr, &msg) && !contains_txt_response(&msg) {
            continue;
        }

        if let Some(mdns_protocol) = mdns_protocol.upgrade() {
            if let Some((t, ttl)) = make_target(addr, msg) {
                tracing::trace!(
                    "packet from {} ({}) on {}",
                    addr,
                    t.nodename.as_ref().unwrap_or(&"<unknown>".to_string()),
                    sock.local_addr().unwrap()
                );
                mdns_protocol.handle_target(t, ttl).await;
            }
        } else {
            return;
        }
    }
}

fn construct_query_buf(service: &str) -> Box<[u8]> {
    let question = dns::QuestionBuilder::new(
        dns::DomainBuilder::from_str(service).unwrap(),
        dns::Type::Ptr,
        dns::Class::In,
        true,
    );

    let mut message = dns::MessageBuilder::new(0, true);
    message.add_question(question);

    let mut buf = vec![0; message.bytes_len()];
    message.serialize(buf.as_mut_slice());
    buf.into_boxed_slice()
}

lazy_static::lazy_static! {
    static ref QUERY_BUF: [Box<[u8]>; 2] =
    [
        (construct_query_buf("_fuchsia._udp.local")),
        (construct_query_buf("_fastboot._tcp.local")),
    ];
}

// query_loop broadcasts an mdns query on sock every interval.
async fn query_loop(sock: Rc<UdpSocket>, interval: Duration) {
    let to_addr: SocketAddr = match sock.local_addr() {
        Ok(SocketAddr::V4(_)) => (MDNS_MCAST_V4, MDNS_PORT).into(),
        Ok(SocketAddr::V6(_)) => (MDNS_MCAST_V6, MDNS_PORT).into(),
        Err(err) => {
            tracing::error!("resolving local socket addr failed with: {}", err);
            return;
        }
    };

    loop {
        for query_buf in QUERY_BUF.iter() {
            if let Err(err) = sock.send_to(query_buf, to_addr).await {
                tracing::error!(
                    "mdns query failed from {}: {}",
                    sock.local_addr()
                        .map(|a| a.to_string())
                        .unwrap_or_else(|_| "unknown".to_string()),
                    err
                );
                return;
            }
        }
        Timer::new(interval).await;
    }
}

// sock is dispatched with a recv_loop, as well as broadcasting an
// mdns query to discover Fuchsia devices every interval.
async fn query_recv_loop(
    sock: Rc<UdpSocket>,
    mdns_protocol: Weak<MdnsProtocolInner>,
    interval: Duration,
    tasks: Rc<Mutex<HashMap<IpAddr, Task<()>>>>,
) {
    let mut recv = recv_loop(sock.clone(), mdns_protocol).boxed_local().fuse();
    let mut query = query_loop(sock.clone(), interval).boxed_local().fuse();

    let addr = match sock.local_addr() {
        Ok(addr) => addr,
        Err(err) => {
            tracing::error!("mdns: failed to shutdown: {:?}", err);
            return;
        }
    };

    tracing::debug!("mdns: started query socket {}", &addr);

    futures::select!(
        _ = recv => {},
        _ = query => {},
    );

    drop(recv);
    drop(query);

    if let Some(a) = tasks.lock().await.remove(&addr.ip()) {
        drop(a)
    }
    tracing::debug!("mdns: shut down query socket {}", &addr);
}

// Exclude any mdns packets received where the source address of the packet does not appear in any
// of the answers in the advert/response, as this likely means the target was NAT'd in some way, and
// the return path is likely not viable. In particular this filters out multicast that QEMU SLIRP
// has invalidly pumped onto the network that would cause us to attempt to connect to the host
// machine as if it was a Fuchsia target.
fn contains_source_address<B: zerocopy::ByteSlice + Clone>(
    addr: &SocketAddr,
    msg: &dns::Message<B>,
) -> bool {
    for answer in msg.answers.iter().chain(msg.additional.iter()) {
        if answer.rtype != Type::A && answer.rtype != Type::Aaaa {
            continue;
        }

        if answer.rdata.ip_addr() == Some(addr.ip()) {
            return true;
        }
    }
    tracing::warn!(
        "Dubious mdns from: {:?} does not contain an answer that includes the source address, therefore it is ignored.",
        addr
    );
    false
}

fn contains_txt_response<B: zerocopy::ByteSlice + Clone>(m: &dns::Message<B>) -> bool {
    m.answers.iter().any(|a| a.rtype == dns::Type::Txt)
}
fn is_fuchsia_response<B: zerocopy::ByteSlice + Clone>(m: &dns::Message<B>) -> bool {
    m.answers.iter().any(|a| a.domain == "_fuchsia._udp.local")
}

fn is_fastboot_response<B: zerocopy::ByteSlice + Clone>(
    m: &dns::Message<B>,
) -> Option<ffx::FastbootInterface> {
    if m.answers.is_empty() {
        None
    } else if m.answers[0].domain == "_fastboot._udp.local" {
        Some(ffx::FastbootInterface::Udp)
    } else if m.answers[0].domain == "_fastboot._tcp.local" {
        Some(ffx::FastbootInterface::Tcp)
    } else {
        None
    }
}

fn make_listen_socket(listen_addr: SocketAddr) -> Result<UdpSocket> {
    let socket: std::net::UdpSocket = match listen_addr {
        SocketAddr::V4(_) => {
            let socket = socket2::Socket::new(
                socket2::Domain::IPV4,
                socket2::Type::DGRAM,
                Some(socket2::Protocol::UDP),
            )
            .context("construct datagram socket")?;
            socket.set_multicast_loop_v4(false).context("set_multicast_loop_v4")?;
            socket.set_reuse_address(true).context("set_reuse_address")?;
            socket.set_reuse_port(true).context("set_reuse_port")?;
            socket
                .bind(
                    &SocketAddr::new(IpAddr::V4(Ipv4Addr::UNSPECIFIED), listen_addr.port()).into(),
                )
                .context("bind")?;
            socket
                .join_multicast_v4(&MDNS_MCAST_V4, &Ipv4Addr::UNSPECIFIED)
                .context("join_multicast_v4")?;
            socket
        }
        SocketAddr::V6(_) => {
            let socket = socket2::Socket::new(
                socket2::Domain::IPV6,
                socket2::Type::DGRAM,
                Some(socket2::Protocol::UDP),
            )
            .context("construct datagram socket")?;
            socket.set_only_v6(true).context("set_only_v6")?;
            socket.set_multicast_loop_v6(false).context("set_multicast_loop_v6")?;
            socket.set_reuse_address(true).context("set_reuse_address")?;
            socket.set_reuse_port(true).context("set_reuse_port")?;
            socket
                .bind(
                    &SocketAddr::new(IpAddr::V6(Ipv6Addr::UNSPECIFIED), listen_addr.port()).into(),
                )
                .context("bind")?;
            // For some reason this often fails to bind on Mac, so avoid it and
            // use the interface binding loop to get multicast group joining to
            // work.
            #[cfg(not(target_os = "macos"))]
            socket.join_multicast_v6(&MDNS_MCAST_V6, 0).context("join_multicast_v6")?;
            socket
        }
    }
    .into();
    Ok(Async::new(socket)?.into())
}

fn make_sender_socket(interface_id: u32, addr: SocketAddr, ttl: u32) -> Result<UdpSocket> {
    let socket: std::net::UdpSocket = match addr {
        SocketAddr::V4(ref saddr) => {
            let socket = socket2::Socket::new(
                socket2::Domain::IPV4,
                socket2::Type::DGRAM,
                Some(socket2::Protocol::UDP),
            )
            .context("construct datagram socket")?;
            socket.set_ttl(ttl).context("set_ttl")?;
            socket.set_multicast_if_v4(saddr.ip()).context("set_multicast_if_v4")?;
            socket.set_multicast_ttl_v4(ttl).context("set_multicast_ttl_v4")?;
            socket.bind(&addr.into()).context("bind")?;
            socket
        }
        SocketAddr::V6(ref _saddr) => {
            let socket = socket2::Socket::new(
                socket2::Domain::IPV6,
                socket2::Type::DGRAM,
                Some(socket2::Protocol::UDP),
            )
            .context("construct datagram socket")?;
            socket.set_only_v6(true).context("set_only_v6")?;
            socket.set_multicast_if_v6(interface_id).context("set_multicast_if_v6")?;
            socket.set_unicast_hops_v6(ttl).context("set_unicast_hops_v6")?;
            socket.set_multicast_hops_v6(ttl).context("set_multicast_hops_v6")?;
            socket.bind(&addr.into()).context("bind")?;
            socket
        }
    }
    .into();
    Ok(Async::new(socket)?.into())
}

#[cfg(test)]
mod tests {

    use super::*;
    use ::mdns::protocol::{
        Class, DomainBuilder, EmbeddedPacketBuilder, Message, MessageBuilder, RecordBuilder, Type,
    };
    use fidl_fuchsia_developer_ffx::{TargetAddrInfo::IpPort, TargetIpPort};
    use fidl_fuchsia_net::IpAddress::Ipv4;
    use packet::{InnerPacketBuilder, ParseBuffer, Serializer};
    use std::io::Write;

    #[test]
    fn test_make_target() {
        let nodename = DomainBuilder::from_str("foo._fuchsia._udp.local").unwrap();
        let record = RecordBuilder::new(nodename, Type::A, Class::Any, true, 4500, &[8, 8, 8, 8]);
        let mut message = MessageBuilder::new(0, true);
        message.add_additional(record);
        let mut msg_bytes = message
            .into_serializer()
            .serialize_vec_outer()
            .unwrap_or_else(|_| panic!("failed to serialize"));
        let parsed = msg_bytes.parse::<Message<_>>().expect("failed to parse");
        let addr: SocketAddr = (MDNS_MCAST_V4, 12).into();
        let (t, ttl) = make_target(addr, parsed).unwrap();
        assert_eq!(ttl, 4500);
        assert_eq!(
            t.addresses.as_ref().unwrap()[0],
            ffx::TargetAddrInfo::Ip(ffx::TargetIp {
                ip: IpAddress::Ipv4(Ipv4Address { addr: MDNS_MCAST_V4.octets() }),
                scope_id: 0
            })
        );
        assert_eq!(t.nodename.unwrap(), "foo._fuchsia._udp");
    }

    #[test]
    fn test_make_target_from_txt() -> Result<()> {
        let nodename = DomainBuilder::from_str("foo._fuchsia._udp.local").unwrap();
        let mut emu_data: Vec<u8> = vec![];
        let emu_strings = ["host:123.11.22.33", "ssh:54321", "debug:1111"];
        for d in emu_strings {
            emu_data.write_all(&[d.len() as u8])?;
            emu_data.write_all(d.as_bytes())?;
        }
        let record =
            RecordBuilder::new(nodename.clone(), Type::A, Class::Any, true, 4500, &[8, 8, 8, 8]);
        let text_record =
            RecordBuilder::new(nodename, Type::Txt, Class::Any, true, 4500, &emu_data);
        let mut message = MessageBuilder::new(0, true);
        message.add_additional(record);
        message.add_additional(text_record);
        let mut msg_bytes = message
            .into_serializer()
            .serialize_vec_outer()
            .unwrap_or_else(|_| panic!("failed to serialize"));
        let parsed = msg_bytes.parse::<Message<_>>().expect("failed to parse");
        let addr: SocketAddr = (MDNS_MCAST_V4, 12).into();
        let (t, ttl) = make_target(addr, parsed).unwrap();
        assert_eq!(ttl, 4500);
        assert_eq!(
            t.addresses.as_ref().unwrap()[0],
            ffx::TargetAddrInfo::Ip(ffx::TargetIp {
                ip: IpAddress::Ipv4(Ipv4Address { addr: [123, 11, 22, 33] }),
                scope_id: 0
            })
        );
        assert_eq!(
            t.ssh_address,
            Some(IpPort(TargetIpPort {
                ip: Ipv4(Ipv4Address { addr: [123, 11, 22, 33] }),
                scope_id: 0,
                port: 54321
            }))
        );
        assert_eq!(t.nodename.unwrap(), "foo._fuchsia._udp");
        Ok(())
    }

    #[test]
    fn test_make_target_no_valid_record() {
        let nodename = DomainBuilder::from_str("foo._fuchsia._udp.local").unwrap();
        let record = RecordBuilder::new(
            nodename,
            Type::Ptr,
            Class::Any,
            true,
            4500,
            &[0x03, b'f', b'o', b'o', 0],
        );
        let mut message = MessageBuilder::new(0, true);
        message.add_additional(record);
        let mut msg_bytes = message
            .into_serializer()
            .serialize_vec_outer()
            .unwrap_or_else(|_| panic!("failed to serialize"));
        let parsed = msg_bytes.parse::<Message<_>>().expect("failed to parse");
        let addr: SocketAddr = (MDNS_MCAST_V4, 12).into();
        assert!(make_target(addr, parsed).is_none());
    }

    /// Create an mdns advertisement packet as network bytes
    fn create_mdns_advert(nodename: &str, address: IpAddr) -> Vec<u8> {
        let domain = DomainBuilder::from_str(&format!("{}._fuchsia._udp.local", nodename)).unwrap();
        let rdata = DomainBuilder::from_str("_fuchsia._udp.local").unwrap().bytes();
        let record = RecordBuilder::new(domain, Type::Ptr, Class::Any, true, 1, &rdata);
        let mut message = MessageBuilder::new(0, true);
        message.add_additional(record);

        let domain = DomainBuilder::from_str(&format!("{}.local", nodename)).unwrap();
        let rdata = match &address {
            IpAddr::V4(addr) => Vec::from(addr.octets()),
            IpAddr::V6(addr) => Vec::from(addr.octets()),
        };
        let record = RecordBuilder::new(
            domain,
            match address {
                IpAddr::V4(_) => Type::A,
                IpAddr::V6(_) => Type::Aaaa,
            },
            Class::Any,
            true,
            1,
            &rdata,
        );
        message.add_additional(record);

        message
            .into_serializer()
            .serialize_vec_outer()
            .unwrap_or_else(|_| panic!("failed to serialize"))
            .unwrap_b()
            .as_ref()
            .to_vec()
    }

    #[test]
    fn test_contains_source_address() {
        assert!(contains_source_address(
            &"127.0.0.1:0".parse().unwrap(),
            &create_mdns_advert("fuchsia-foo-1234", IpAddr::from([127, 0, 0, 1]))
                .as_slice()
                .parse::<Message<_>>()
                .unwrap(),
        ));

        assert!(!contains_source_address(
            &"127.0.0.1:0".parse().unwrap(),
            &create_mdns_advert("fuchsia-foo-1234", IpAddr::from([127, 0, 0, 2]))
                .as_slice()
                .parse::<Message<_>>()
                .unwrap(),
        ));
    }
}
