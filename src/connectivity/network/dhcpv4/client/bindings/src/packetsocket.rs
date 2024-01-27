// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use async_trait::async_trait;
use dhcp_client_core::PacketSocketProvider;
use fidl_fuchsia_posix as fposix;
use fidl_fuchsia_posix_socket_packet as fpacket;
use fuchsia_async as fasync;
use sockaddr::{IntoSockAddr as _, TryToSockaddrLl as _};

pub(crate) struct PacketSocket {
    interface_id: u64,
    inner: fasync::net::DatagramSocket,
}

fn translate_io_error(e: std::io::Error) -> dhcp_client_core::SocketError {
    use fposix::Errno as E;
    match e.raw_os_error().and_then(fposix::Errno::from_primitive) {
        None => dhcp_client_core::SocketError::Other(e),
        // TODO(https://fxbug.dev/124593): better indicate recoverable
        // vs. non-recoverable error types.
        Some(errno) => match errno {
            // ============
            // These errors are documented in `man packet`.
            E::Eaddrnotavail => panic!(
                "got EADDRNOTAVAIL, which shouldn't be possible because we \
                 aren't sending traffic to multicast groups: {:?}",
                e
            ),
            E::Efault => panic!("passed invalid memory address to socket call"),
            E::Enobufs => panic!("out of memory: {:?}", e),
            E::Enodev => dhcp_client_core::SocketError::NoInterface,
            E::Einval
            | E::Emsgsize
            | E::Enetdown
            | E::Enoent
            | E::Enotconn
            | E::Enxio
            | E::Eperm => dhcp_client_core::SocketError::Other(e),

            // ============
            // These errors are documented in `man bind`.
            E::Enotsock => {
                panic!("tried to perform socket operation with non-socket file descriptor: {:?}", e)
            }
            E::Ebadf => {
                panic!("tried to perform socket operation with bad file descriptor: {:?}", e)
            }
            E::Eacces | E::Eaddrinuse => dhcp_client_core::SocketError::Other(e),

            // ============
            // These errors can be returned from `sendto()` at the socket layer.
            E::Eagain => {
                panic!("got EAGAIN, should be handled by lower-level library: {:?}", e)
            }
            E::Eintr => panic!("got EINTR, should be handled by lower-level library: {:?}", e),
            E::Ealready => panic!("got EALREADY, but we aren't using TCP: {:?}", e),
            E::Econnreset => panic!("got ECONNRESET, but we aren't using TCP: {:?}", e),
            E::Edestaddrreq => panic!("got EDESTADDRREQ, but we only use sendto: {:?}", e),
            E::Eisconn => {
                panic!("got EISCONN, but we aren't using connection-mode sockets: {:?}", e)
            }
            E::Epipe => {
                panic!("got EPIPE, but we aren't using connection-mode sockets: {:?}", e)
            }
            E::Enomem => panic!("out of memory: {:?}", e),
            E::Eopnotsupp => dhcp_client_core::SocketError::Other(e),

            // ============
            // These errors are documented in `man recvfrom`.
            E::Econnrefused => {
                panic!("got ECONNREFUSED, but we aren't using connection-mode sockets: {:?}", e)
            }

            // ============
            // The following errors aren't expected to be returned by any of the
            // socket operations we use.
            E::Enetunreach
            | E::Esrch
            | E::Eio
            | E::E2Big
            | E::Enoexec
            | E::Echild
            | E::Enotblk
            | E::Ebusy
            | E::Eexist
            | E::Exdev
            | E::Enotdir
            | E::Eisdir
            | E::Enfile
            | E::Emfile
            | E::Enotty
            | E::Etxtbsy
            | E::Efbig
            | E::Enospc
            | E::Espipe
            | E::Erofs
            | E::Emlink
            | E::Edom
            | E::Erange
            | E::Edeadlk
            | E::Enametoolong
            | E::Enolck
            | E::Enosys
            | E::Enotempty
            | E::Eloop
            | E::Enomsg
            | E::Eidrm
            | E::Echrng
            | E::El2Nsync
            | E::El3Hlt
            | E::El3Rst
            | E::Elnrng
            | E::Eunatch
            | E::Enocsi
            | E::El2Hlt
            | E::Ebade
            | E::Ebadr
            | E::Exfull
            | E::Enoano
            | E::Ebadrqc
            | E::Ebadslt
            | E::Ebfont
            | E::Enostr
            | E::Enodata
            | E::Etime
            | E::Enosr
            | E::Enonet
            | E::Enopkg
            | E::Eremote
            | E::Enolink
            | E::Eadv
            | E::Esrmnt
            | E::Ecomm
            | E::Eproto
            | E::Emultihop
            | E::Edotdot
            | E::Ebadmsg
            | E::Eoverflow
            | E::Enotuniq
            | E::Ebadfd
            | E::Eremchg
            | E::Elibacc
            | E::Elibbad
            | E::Elibscn
            | E::Elibmax
            | E::Elibexec
            | E::Eilseq
            | E::Erestart
            | E::Estrpipe
            | E::Eusers
            | E::Eprototype
            | E::Enoprotoopt
            | E::Eprotonosupport
            | E::Esocktnosupport
            | E::Epfnosupport
            | E::Eafnosupport
            | E::Enetreset
            | E::Econnaborted
            | E::Eshutdown
            | E::Etoomanyrefs
            | E::Etimedout
            | E::Ehostdown
            | E::Ehostunreach
            | E::Einprogress
            | E::Estale
            | E::Euclean
            | E::Enotnam
            | E::Enavail
            | E::Eisnam
            | E::Eremoteio
            | E::Edquot
            | E::Enomedium
            | E::Emediumtype
            | E::Ecanceled
            | E::Enokey
            | E::Ekeyexpired
            | E::Ekeyrevoked
            | E::Ekeyrejected
            | E::Eownerdead
            | E::Enotrecoverable
            | E::Erfkill
            | E::Ehwpoison => panic!("unexpected error from socket: {:?}", e),
        },
    }
}

/// Specifies only the fields that need to vary within our `PacketSocket` implementation.
struct SockaddrLl {
    interface_id: u64,

    // This DHCP client is assumed to operate only on Ethernet links.
    // TODO(https://fxbug.dev/124899): Generalize to other link types (like PPP).
    addr: net_types::ethernet::Mac,
}

impl From<SockaddrLl> for libc::sockaddr_ll {
    fn from(value: SockaddrLl) -> Self {
        let SockaddrLl { interface_id, addr } = value;

        let mut sll_addr = [0; 8];
        sll_addr[..addr.bytes().len()].copy_from_slice(&addr.bytes());

        let ethertype_ipv4 = u16::from(packet_formats::ethernet::EtherType::Ipv4);
        libc::sockaddr_ll {
            sll_family: libc::AF_PACKET
                .try_into()
                .expect("libc::AF_PACKET should fit in sll_family field"),
            sll_ifindex: (interface_id)
                .try_into()
                .expect("interface_id should fit in sll_ifindex field"),
            // Network order is big endian.
            sll_protocol: ethertype_ipv4.to_be(),
            sll_halen: addr
                .bytes()
                .len()
                .try_into()
                .expect("hardware address length should fit in sll_halen field"),
            sll_addr,
            sll_hatype: 0,  // unused by sendto() or bind()
            sll_pkttype: 0, // unused by sendto() or bind()
        }
    }
}

// TODO(https://fxbug.dev/124913): expose this from `libc` crate on fuchsia.
const ARPHRD_ETHER: libc::c_ushort = 1;

#[async_trait(?Send)]
impl dhcp_client_core::Socket<net_types::ethernet::Mac> for PacketSocket {
    async fn send_to(
        &self,
        buf: &[u8],
        addr: net_types::ethernet::Mac,
    ) -> Result<(), dhcp_client_core::SocketError> {
        let Self { interface_id, inner } = self;

        let sockaddr_ll = libc::sockaddr_ll::from(SockaddrLl { interface_id: *interface_id, addr });

        let n =
            inner.send_to(buf, sockaddr_ll.into_sockaddr()).await.map_err(translate_io_error)?;
        // Packet sockets never have short sends.
        assert_eq!(n, buf.len());
        Ok(())
    }

    async fn recv_from(
        &self,
        buf: &mut [u8],
    ) -> Result<
        dhcp_client_core::DatagramInfo<net_types::ethernet::Mac>,
        dhcp_client_core::SocketError,
    > {
        let Self { interface_id: _, inner } = self;
        let (length, sock_addr) = inner.recv_from(buf).await.map_err(translate_io_error)?;
        let libc::sockaddr_ll {
            sll_family: _,
            sll_ifindex: _,
            sll_protocol: _,
            sll_halen,
            sll_addr,
            sll_hatype,
            sll_pkttype: _,
        } = sock_addr.try_to_sockaddr_ll().expect("addr from packet socket must be sockaddr_ll");

        if sll_hatype != ARPHRD_ETHER {
            tracing::error!("unsupported sll_hatype: {}", sll_hatype);
            return Err(dhcp_client_core::SocketError::UnsupportedHardwareType);
        }

        assert_eq!(usize::from(sll_halen), net_types::ethernet::Mac::BYTES);
        let address = {
            let mut address = net_types::ethernet::Mac::new([0; net_types::ethernet::Mac::BYTES]);
            address.as_mut().copy_from_slice(&sll_addr[..net_types::ethernet::Mac::BYTES]);
            address
        };
        Ok(dhcp_client_core::DatagramInfo { length, address })
    }
}

pub(crate) struct PacketSocketProviderImpl {
    provider: fpacket::ProviderProxy,
    interface_id: u64,
}

impl PacketSocketProviderImpl {
    pub(crate) fn new(provider: fpacket::ProviderProxy, interface_id: u64) -> Self {
        PacketSocketProviderImpl { provider, interface_id }
    }

    pub(crate) async fn get_mac(
        &self,
    ) -> Result<net_types::ethernet::Mac, dhcp_client_core::SocketError> {
        let PacketSocket { interface_id: _, inner } = self.get_packet_socket().await?;
        let sock_addr = inner.local_addr().map_err(translate_io_error)?;
        let libc::sockaddr_ll {
            sll_family: _,
            sll_ifindex: _,
            sll_protocol: _,
            sll_halen,
            sll_addr,
            sll_hatype,
            sll_pkttype: _,
        } = sock_addr.try_to_sockaddr_ll().expect("addr from packet socket must be sockaddr_ll");
        if sll_hatype != ARPHRD_ETHER {
            tracing::error!("unsupported sll_hatype: {}", sll_hatype);
            return Err(dhcp_client_core::SocketError::UnsupportedHardwareType);
        }

        assert_eq!(usize::from(sll_halen), net_types::ethernet::Mac::BYTES);
        let mut address = net_types::ethernet::Mac::new([0; net_types::ethernet::Mac::BYTES]);
        address.as_mut().copy_from_slice(&sll_addr[..net_types::ethernet::Mac::BYTES]);
        Ok(address)
    }
}

#[async_trait(?Send)]
impl dhcp_client_core::PacketSocketProvider for PacketSocketProviderImpl {
    type Sock = PacketSocket;

    async fn get_packet_socket(&self) -> Result<Self::Sock, dhcp_client_core::SocketError> {
        let PacketSocketProviderImpl { provider, interface_id } = self;

        let sock = provider
            .socket(fpacket::Kind::Network)
            .await
            .map_err(|e: fidl::Error| dhcp_client_core::SocketError::FailedToOpen(e.into()))?
            .map_err(|errno| {
                translate_io_error(std::io::Error::from_raw_os_error(errno.into_primitive()))
            })?;
        let socket: socket2::Socket = fdio::create_fd(sock.into()).unwrap();

        let sockaddr_ll = libc::sockaddr_ll::from(SockaddrLl {
            interface_id: *interface_id,
            addr: net_types::ethernet::Mac::UNSPECIFIED, // unused by bind()
        });

        socket.bind(&sockaddr_ll.into_sockaddr()).map_err(translate_io_error)?;
        let socket = fasync::net::DatagramSocket::new_from_socket(socket)
            .expect("failed to wrap into fuchsia-async DatagramSocket");
        Ok(PacketSocket { interface_id: *interface_id, inner: socket })
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use dhcp_client_core::{DatagramInfo, Socket as _};
    use fidl_fuchsia_net_ext as fnet_ext;
    use fidl_fuchsia_netemul_network as fnetemul_network;
    use fidl_fuchsia_posix_socket_packet as fpacket;
    use fuchsia_async as fasync;
    use futures::{join, FutureExt as _};
    use netstack_testing_common::realms::TestSandboxExt as _;
    use packet::{InnerPacketBuilder as _, Serializer as _};

    #[fasync::run_singlethreaded(test)]
    async fn packet_socket_provider_impl_send_receive() {
        let sandbox: netemul::TestSandbox = netemul::TestSandbox::new().unwrap();

        let network = sandbox.create_network("dhcp-test-network").await.expect("create network");
        let realm_a: netemul::TestRealm<'_> = sandbox
            .create_netstack_realm::<netstack_testing_common::realms::Netstack2, _>(
                "dhcp-test-realm-a",
            )
            .expect("create realm");
        let realm_b: netemul::TestRealm<'_> = sandbox
            .create_netstack_realm::<netstack_testing_common::realms::Netstack2, _>(
                "dhcp-test-realm-b",
            )
            .expect("create realm");

        const MAC_A: net_types::ethernet::Mac = net_declare::net_mac!("00:00:00:00:00:01");
        const MAC_B: net_types::ethernet::Mac = net_declare::net_mac!("00:00:00:00:00:02");

        let iface_a = realm_a
            .join_network_with(
                &network,
                "iface_a",
                fnetemul_network::EndpointConfig {
                    mtu: netemul::DEFAULT_MTU,
                    mac: Some(Box::new(fnet_ext::MacAddress { octets: MAC_A.bytes() }.into())),
                },
                netemul::InterfaceConfig { name: Some("iface_a".into()), metric: None },
            )
            .await
            .expect("join network with realm_a");
        let iface_b = realm_b
            .join_network_with(
                &network,
                "iface_b",
                fnetemul_network::EndpointConfig {
                    mtu: netemul::DEFAULT_MTU,
                    mac: Some(Box::new(fnet_ext::MacAddress { octets: MAC_B.bytes() }.into())),
                },
                netemul::InterfaceConfig { name: Some("iface_b".into()), metric: None },
            )
            .await
            .expect("join network with realm_b");

        let socket_a = PacketSocketProviderImpl {
            interface_id: iface_a.id(),
            provider: realm_a.connect_to_protocol::<fpacket::ProviderMarker>().unwrap(),
        }
        .get_packet_socket()
        .await
        .expect("get packet socket");
        let socket_b = PacketSocketProviderImpl {
            interface_id: iface_b.id(),
            provider: realm_b.connect_to_protocol::<fpacket::ProviderMarker>().unwrap(),
        }
        .get_packet_socket()
        .await
        .expect("get packet socket");

        let mut buf = [0u8; netemul::DEFAULT_MTU as usize];

        // We can only receive IPv4-formatted packets because our packet sockets
        // only bind to the IPv4 protocol.
        let payload = packet_formats::ipv4::Ipv4PacketBuilder::new(
            net_types::ip::Ipv4Addr::default(),
            net_types::ip::Ipv4Addr::default(),
            0,
            packet_formats::ip::Ipv4Proto::Other(0),
        );
        let payload = b"hello world!"
            .into_serializer()
            .encapsulate(payload)
            .serialize_vec_outer()
            .expect("serialize");

        let DatagramInfo { length, address } = {
            let send_fut = async {
                socket_a
                    // The packet sockets are all on the same link, so the
                    // destination MAC address doesn't matter.
                    .send_to(payload.as_ref(), net_declare::net_mac!("00:00:00:00:00:00"))
                    .await
                    .expect("send_to");
            }
            .fuse();

            let receive_fut =
                async { socket_b.recv_from(&mut buf).await.expect("recv_from") }.fuse();

            let ((), datagram_info) = join!(send_fut, receive_fut);
            datagram_info
        };

        assert_eq!(&buf[..length], payload.as_ref());
        assert_eq!(address, MAC_A);
    }

    #[fasync::run_singlethreaded(test)]
    async fn packet_socket_provider_impl_get_mac() {
        let sandbox: netemul::TestSandbox = netemul::TestSandbox::new().unwrap();

        let network = sandbox.create_network("dhcp-test-network").await.expect("create network");
        let realm: netemul::TestRealm<'_> = sandbox
            .create_netstack_realm::<netstack_testing_common::realms::Netstack2, _>(
                "dhcp-test-realm-a",
            )
            .expect("create realm");

        const MAC: net_types::ethernet::Mac = net_declare::net_mac!("00:00:00:00:00:01");
        let iface = realm
            .join_network_with(
                &network,
                "iface",
                fnetemul_network::EndpointConfig {
                    mtu: netemul::DEFAULT_MTU,
                    mac: Some(Box::new(fnet_ext::MacAddress { octets: MAC.bytes() }.into())),
                },
                netemul::InterfaceConfig { name: Some("iface".into()), metric: None },
            )
            .await
            .expect("join network with realm");

        let provider = PacketSocketProviderImpl {
            interface_id: iface.id(),
            provider: realm.connect_to_protocol::<fpacket::ProviderMarker>().unwrap(),
        };
        assert_eq!(provider.get_mac().await.expect("get mac"), MAC);
    }
}
