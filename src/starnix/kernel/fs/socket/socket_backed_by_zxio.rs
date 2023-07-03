// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;

use crate::{
    fs::{buffers::*, fuchsia::*, *},
    logging::{log_warn, not_implemented},
    mm::MemoryAccessorExt,
    syscalls::*,
    task::*,
    types::*,
};

use fidl::endpoints::DiscoverableProtocolMarker as _;
use fidl_fuchsia_posix_socket as fposix_socket;
use fidl_fuchsia_posix_socket_packet as fposix_socket_packet;
use fidl_fuchsia_posix_socket_raw as fposix_socket_raw;
use fuchsia_zircon as zx;
use net_types::ip::IpAddress as _;
use netlink_packet_core::{NetlinkHeader, NetlinkMessage, NetlinkPayload};
use netlink_packet_route::{
    rtnl::{address::nlas::Nla as AddressNla, link::nlas::Nla as LinkNla},
    AddressMessage, LinkMessage, RtnlMessage,
};
use static_assertions::{const_assert, const_assert_eq};
use std::{
    ffi::CStr,
    mem::size_of,
    sync::{Arc, OnceLock},
};
use syncio::{ControlMessage, RecvMessageInfo, ServiceConnector, Zxio};
use zerocopy::{AsBytes as _, ByteOrder as _, FromBytes as _, NativeEndian};

/// The size of a buffer suitable to carry netlink route messages.
const NETLINK_ROUTE_BUF_SIZE: usize = 1024;

/// Connects to the appropriate `fuchsia_posix_socket_*::Provider` protocol.
struct SocketProviderServiceConnector;

impl ServiceConnector for SocketProviderServiceConnector {
    fn connect(service_name: &str) -> Result<&'static zx::Channel, zx::Status> {
        match service_name {
            fposix_socket::ProviderMarker::PROTOCOL_NAME => {
                static mut CHANNEL: OnceLock<Result<zx::Channel, zx::Status>> = OnceLock::new();
                unsafe { &CHANNEL }
            }
            fposix_socket_packet::ProviderMarker::PROTOCOL_NAME => {
                static mut CHANNEL: OnceLock<Result<zx::Channel, zx::Status>> = OnceLock::new();
                unsafe { &CHANNEL }
            }
            fposix_socket_raw::ProviderMarker::PROTOCOL_NAME => {
                static mut CHANNEL: OnceLock<Result<zx::Channel, zx::Status>> = OnceLock::new();
                unsafe { &CHANNEL }
            }
            _ => return Err(zx::Status::INTERNAL),
        }
        .get_or_init(|| {
            let (client, server) = zx::Channel::create();
            let protocol_path = format!("/svc/{service_name}");
            fdio::service_connect(&protocol_path, server)?;
            Ok(client)
        })
        .as_ref()
        .map_err(|status| *status)
    }
}

/// A socket backed by an underlying Zircon I/O object.
pub struct ZxioBackedSocket {
    /// The underlying Zircon I/O object.
    zxio: Arc<syncio::Zxio>,
}

impl ZxioBackedSocket {
    pub fn new(
        domain: SocketDomain,
        socket_type: SocketType,
        protocol: SocketProtocol,
    ) -> Result<ZxioBackedSocket, Errno> {
        let zxio = Zxio::new_socket::<SocketProviderServiceConnector>(
            domain.as_raw() as c_int,
            socket_type.as_raw() as c_int,
            protocol.as_raw() as c_int,
        )
        .map_err(|status| from_status_like_fdio!(status))?
        .map_err(|out_code| errno_from_zxio_code!(out_code))?;

        Ok(ZxioBackedSocket { zxio: Arc::new(zxio) })
    }

    pub fn sendmsg(
        &self,
        addr: &Option<SocketAddress>,
        data: &mut dyn InputBuffer,
        cmsgs: Vec<ControlMessage>,
        flags: SocketMessageFlags,
    ) -> Result<usize, Errno> {
        let addr = match addr {
            Some(sockaddr) => match sockaddr {
                SocketAddress::Inet(sockaddr)
                | SocketAddress::Inet6(sockaddr)
                | SocketAddress::Packet(sockaddr) => sockaddr.clone(),
                _ => return error!(EINVAL),
            },
            None => vec![],
        };

        let bytes = data.peek_all()?;
        let sent_bytes = self
            .zxio
            .sendmsg(addr, bytes, cmsgs, flags.bits() & !MSG_DONTWAIT)
            .map_err(|status| from_status_like_fdio!(status))?
            .map_err(|out_code| errno_from_zxio_code!(out_code))?;
        data.advance(sent_bytes)?;
        Ok(sent_bytes)
    }

    pub fn recvmsg(
        &self,
        iovec_length: usize,
        flags: SocketMessageFlags,
    ) -> Result<RecvMessageInfo, Errno> {
        self.zxio
            .recvmsg(iovec_length, flags.bits() & !MSG_DONTWAIT & !MSG_WAITALL)
            .map_err(|status| from_status_like_fdio!(status))?
            .map_err(|out_code| errno_from_zxio_code!(out_code))
    }
}

impl SocketOps for ZxioBackedSocket {
    fn connect(
        &self,
        _socket: &SocketHandle,
        _current_task: &CurrentTask,
        peer: SocketPeer,
    ) -> Result<(), Errno> {
        match peer {
            SocketPeer::Address(SocketAddress::Inet(addr))
            | SocketPeer::Address(SocketAddress::Inet6(addr))
            | SocketPeer::Address(SocketAddress::Packet(addr)) => self
                .zxio
                .connect(&addr)
                .map_err(|status| from_status_like_fdio!(status))?
                .map_err(|out_code| errno_from_zxio_code!(out_code)),
            _ => error!(EINVAL),
        }
    }

    fn listen(&self, _socket: &Socket, backlog: i32, _credentials: ucred) -> Result<(), Errno> {
        self.zxio
            .listen(backlog)
            .map_err(|status| from_status_like_fdio!(status))?
            .map_err(|out_code| errno_from_zxio_code!(out_code))
    }

    fn accept(&self, socket: &Socket) -> Result<SocketHandle, Errno> {
        let zxio = self
            .zxio
            .accept()
            .map_err(|status| from_status_like_fdio!(status))?
            .map_err(|out_code| errno_from_zxio_code!(out_code))?;

        Ok(Socket::new_with_ops(
            socket.domain,
            socket.socket_type,
            socket.protocol,
            Box::new(ZxioBackedSocket { zxio: Arc::new(zxio) }),
        ))
    }

    fn bind(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        socket_address: SocketAddress,
    ) -> Result<(), Errno> {
        match socket_address {
            SocketAddress::Inet(addr)
            | SocketAddress::Inet6(addr)
            | SocketAddress::Packet(addr) => self
                .zxio
                .bind(&addr)
                .map_err(|status| from_status_like_fdio!(status))?
                .map_err(|out_code| errno_from_zxio_code!(out_code)),
            _ => error!(EINVAL),
        }
    }

    fn read(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        data: &mut dyn OutputBuffer,
        flags: SocketMessageFlags,
    ) -> Result<MessageReadInfo, Errno> {
        let iovec_length = data.available();
        let mut info = self.recvmsg(iovec_length, flags)?;

        let bytes_read = data.write_all(&info.message)?;

        let address = if !info.address.is_empty() {
            Some(SocketAddress::from_bytes(info.address)?)
        } else {
            None
        };

        Ok(MessageReadInfo {
            bytes_read,
            message_length: info.message_length,
            address,
            ancillary_data: info.control_messages.drain(..).map(AncillaryData::Ip).collect(),
        })
    }

    fn write(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        data: &mut dyn InputBuffer,
        dest_address: &mut Option<SocketAddress>,
        ancillary_data: &mut Vec<AncillaryData>,
    ) -> Result<usize, Errno> {
        let mut cmsgs = vec![];
        for d in ancillary_data.drain(..) {
            match d {
                AncillaryData::Ip(msg) => cmsgs.push(msg),
                _ => return error!(EINVAL),
            }
        }

        self.sendmsg(dest_address, data, cmsgs, SocketMessageFlags::empty())
    }

    fn wait_async(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        waiter: &Waiter,
        events: FdEvents,
        handler: EventHandler,
    ) -> WaitCanceler {
        zxio_wait_async(&self.zxio, waiter, events, handler)
    }

    fn query_events(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
    ) -> Result<FdEvents, Errno> {
        Ok(zxio_query_events(&self.zxio))
    }

    fn shutdown(&self, _socket: &Socket, how: SocketShutdownFlags) -> Result<(), Errno> {
        self.zxio
            .shutdown(how)
            .map_err(|status| from_status_like_fdio!(status))?
            .map_err(|out_code| errno_from_zxio_code!(out_code))
    }

    fn close(&self, _socket: &Socket) {}

    fn getsockname(&self, socket: &Socket) -> Vec<u8> {
        match self.zxio.getsockname() {
            Err(_) | Ok(Err(_)) => SocketAddress::default_for_domain(socket.domain).to_bytes(),
            Ok(Ok(addr)) => addr,
        }
    }

    fn getpeername(&self, _socket: &Socket) -> Result<Vec<u8>, Errno> {
        self.zxio
            .getpeername()
            .map_err(|status| from_status_like_fdio!(status))?
            .map_err(|out_code| errno_from_zxio_code!(out_code))
    }

    fn setsockopt(
        &self,
        _socket: &Socket,
        task: &Task,
        level: u32,
        optname: u32,
        user_opt: UserBuffer,
    ) -> Result<(), Errno> {
        let optval = task.mm.read_buffer(&user_opt)?;

        if level == SOL_SOCKET && optname == SO_ATTACH_FILTER {
            not_implemented!(
                "TODO(https://fxbug.dev/129596): `SOL_SOCKET` -> `SO_ATTACH_FILTER` unsupported; returning success anyways"
            );
            return Ok(());
        }

        self.zxio
            .setsockopt(level as i32, optname as i32, &optval)
            .map_err(|status| from_status_like_fdio!(status))?
            .map_err(|out_code| errno_from_zxio_code!(out_code))
    }

    fn getsockopt(
        &self,
        _socket: &Socket,
        level: u32,
        optname: u32,
        optlen: u32,
    ) -> Result<Vec<u8>, Errno> {
        self.zxio
            .getsockopt(level, optname, optlen)
            .map_err(|status| from_status_like_fdio!(status))?
            .map_err(|out_code| errno_from_zxio_code!(out_code))
    }

    fn ioctl(
        &self,
        _socket: &Socket,
        file: &FileObject,
        current_task: &CurrentTask,
        request: u32,
        arg: SyscallArg,
    ) -> Result<SyscallResult, Errno> {
        let user_addr = UserAddress::from(arg);

        // TODO(https://fxbug.dev/129059): Share this implementation with `fdio`
        // by moving things to `zxio`.
        match request {
            SIOCGIFADDR => {
                let in_ifreq: ifreq = current_task.mm.read_object(UserRef::new(user_addr))?;
                let mut read_buf = VecOutputBuffer::new(NETLINK_ROUTE_BUF_SIZE);
                let (_socket, address_msgs, _if_index) =
                    get_netlink_ipv4_addresses(current_task, &in_ifreq, &mut read_buf)?;

                let ifru_addr = {
                    let mut addr = sockaddr::default();
                    let s_addr = address_msgs
                        .into_iter()
                        .next()
                        .and_then(|msg| {
                            msg.nlas.into_iter().find_map(|nla| {
                                if let AddressNla::Address(bytes) = nla {
                                    // The bytes are held in network-endian
                                    // order and `in_addr_t` is documented to
                                    // hold values in network order as well. Per
                                    // POSIX specifications for `sockaddr_in`
                                    // https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/netinet_in.h.html.
                                    //
                                    //   The sin_port and sin_addr members shall
                                    //   be in network byte order.
                                    //
                                    // Because of this, we read the bytes in
                                    // native endian which is effectively a
                                    // `core::mem::transmute` to `u32`.
                                    Some(NativeEndian::read_u32(&bytes[..]))
                                } else {
                                    None
                                }
                            })
                        })
                        .unwrap_or(0);
                    sockaddr_in {
                        sin_family: AF_INET,
                        sin_port: 0,
                        sin_addr: in_addr { s_addr },
                        __pad: Default::default(),
                    }
                    .write_to_prefix(addr.as_bytes_mut());
                    addr
                };

                let out_ifreq: [u8; std::mem::size_of::<ifreq>()] = struct_with_union_into_bytes!(ifreq {
                    ifr_ifrn.ifrn_name: unsafe { in_ifreq.ifr_ifrn.ifrn_name },
                    ifr_ifru.ifru_addr: ifru_addr,
                });
                current_task.mm.write_object(UserRef::new(user_addr), &out_ifreq)?;
                Ok(SUCCESS)
            }
            SIOCSIFADDR => {
                let in_ifreq: ifreq = current_task.mm.read_object(UserRef::new(user_addr))?;
                let mut read_buf = VecOutputBuffer::new(NETLINK_ROUTE_BUF_SIZE);
                let (socket, address_msgs, if_index) =
                    get_netlink_ipv4_addresses(current_task, &in_ifreq, &mut read_buf)?;

                let request_header = {
                    let mut header = NetlinkHeader::default();
                    // Always request the ACK response so that we know the
                    // request has been handled before we return from this
                    // operation.
                    header.flags =
                        netlink_packet_core::NLM_F_REQUEST | netlink_packet_core::NLM_F_ACK;
                    header
                };

                // First remove all IPv4 addresses for the requested interface.
                for addr in address_msgs.into_iter() {
                    send_netlink_msg_and_wait_response(
                        current_task,
                        &socket,
                        NetlinkMessage::new(
                            request_header,
                            NetlinkPayload::InnerMessage(RtnlMessage::DelAddress(addr)),
                        ),
                        &mut read_buf,
                        SIOCSIFADDR,
                    )?;
                }

                // Next, add the requested address.
                const_assert!(size_of::<sockaddr_in>() <= size_of::<sockaddr>());
                let addr = sockaddr_in::read_from_prefix(
                    unsafe { in_ifreq.ifr_ifru.ifru_addr }.as_bytes(),
                )
                .expect("sockaddr_in is smaller than sockaddr")
                .sin_addr
                .s_addr;
                if addr != 0 {
                    send_netlink_msg_and_wait_response(
                        current_task,
                        &socket,
                        NetlinkMessage::new(
                            request_header,
                            NetlinkPayload::InnerMessage(RtnlMessage::NewAddress({
                                let mut msg = AddressMessage::default();
                                msg.header.family =
                                    AF_INET.try_into().expect("AF_INET should fit in u8");
                                msg.header.index = if_index;
                                let addr = addr.to_be_bytes();
                                // The request does not include the prefix
                                // length so we use the default prefix for the
                                // address's class.
                                msg.header.prefix_len = net_types::ip::Ipv4Addr::new(addr)
                                    .class()
                                    .default_prefix_len()
                                    .unwrap_or(net_types::ip::Ipv4Addr::BYTES * 8);
                                msg.nlas = vec![AddressNla::Address(addr.to_vec())];
                                msg
                            })),
                        ),
                        &mut read_buf,
                        SIOCSIFADDR,
                    )?;
                }

                Ok(SUCCESS)
            }
            SIOCGIFHWADDR => {
                let user_addr = UserAddress::from(arg);
                let in_ifreq: ifreq = current_task.mm.read_object(UserRef::new(user_addr))?;
                let mut read_buf = VecOutputBuffer::new(NETLINK_ROUTE_BUF_SIZE);
                let (_socket, link_msg) =
                    get_netlink_interface_info(current_task, &in_ifreq, &mut read_buf)?;

                let hw_addr_and_type = {
                    let hw_type = link_msg.header.link_layer_type;
                    link_msg.nlas.into_iter().find_map(|nla| {
                        if let LinkNla::Address(addr) = nla {
                            Some((addr, hw_type))
                        } else {
                            None
                        }
                    })
                };

                let out_ifreq: [u8; std::mem::size_of::<ifreq>()] = struct_with_union_into_bytes!(
                    ifreq {
                        // Safety: The `ifr_ifrn` union only has one field, so it
                        // must be `ifrn_name`.
                        ifr_ifrn.ifrn_name: unsafe { in_ifreq.ifr_ifrn.ifrn_name },
                        ifr_ifru.ifru_hwaddr: hw_addr_and_type.map(|(addr_bytes, sa_family)| {
                            let mut addr = sockaddr {
                                sa_family,
                                sa_data: Default::default(),
                            };
                            // We need to manually assign from one to the other
                            // because we may be copying a vector of `u8` into
                            // an array of `i8` and regular `copy_from_slice`
                            // expects both src/dst slices to have the same
                            // element type.
                            //
                            // See /src/starnix/lib/linux_uapi/src/types.rs,
                            // `c_char` is an `i8` on `x86_64` and a `u8` on
                            // `arm64` and `riscv`.
                            addr.sa_data.iter_mut().zip(addr_bytes.into_iter())
                                .for_each(|(sa_data_byte, link_addr_byte): (&mut c_char, u8)| {
                                    *sa_data_byte = link_addr_byte as c_char;
                                });
                            addr
                        }).unwrap_or_else(Default::default),
                    }
                );
                current_task.mm.write_object(UserRef::new(user_addr), &out_ifreq)?;
                Ok(SUCCESS)
            }
            SIOCGIFINDEX => {
                let in_ifreq: ifreq = current_task.mm.read_object(UserRef::new(user_addr))?;
                let mut read_buf = VecOutputBuffer::new(NETLINK_ROUTE_BUF_SIZE);
                let (_socket, link_msg) =
                    get_netlink_interface_info(current_task, &in_ifreq, &mut read_buf)?;
                let out_ifreq: [u8; std::mem::size_of::<ifreq>()] = struct_with_union_into_bytes!(ifreq {
                    ifr_ifrn.ifrn_name: unsafe { in_ifreq.ifr_ifrn.ifrn_name },
                    ifr_ifru.ifru_ivalue: {
                        let index: u32 = link_msg.header.index;
                        i32::try_from(index).expect("interface ID should fit in an i32")
                    },
                });
                current_task.mm.write_object(UserRef::new(user_addr), &out_ifreq)?;
                Ok(SUCCESS)
            }
            SIOCGIFMTU => {
                let in_ifreq: ifreq = current_task.mm.read_object(UserRef::new(user_addr))?;
                let out_ifreq: [u8; std::mem::size_of::<ifreq>()] = struct_with_union_into_bytes!(ifreq {
                    ifr_ifrn.ifrn_name: unsafe { in_ifreq.ifr_ifrn.ifrn_name },
                    // TODO(https://fxbug.dev/129165): Return the actual MTU instead
                    // of this hard-coded value.
                    ifr_ifru.ifru_mtu: 1280 /* IPv6 MIN MTU */,
                });
                current_task.mm.write_object(UserRef::new(user_addr), &out_ifreq)?;
                Ok(SUCCESS)
            }
            _ => default_ioctl(file, current_task, request, arg),
        }
    }
}

/// Creates a netlink socket and performs an `RTM_GETLINK` request for the
/// requested interface requested in `in_ifreq`.
///
/// Returns the netlink socket and the interface's information, or an [`Errno`]
/// if the operation failed.
fn get_netlink_interface_info(
    current_task: &CurrentTask,
    in_ifreq: &ifreq,
    read_buf: &mut VecOutputBuffer,
) -> Result<(FileHandle, LinkMessage), Errno> {
    let iface_name = unsafe { CStr::from_ptr(in_ifreq.ifr_ifrn.ifrn_name.as_ptr()) }
        .to_str()
        .map_err(|std::str::Utf8Error { .. }| errno!(EINVAL))?;
    let socket = new_socket_file(
        current_task,
        SocketDomain::Netlink,
        SocketType::Datagram,
        OpenFlags::RDWR,
        SocketProtocol::from_raw(NetlinkFamily::Route.as_raw()),
    )?;

    // Send the request to get the link details with the requested
    // interface name.
    {
        let mut msg = NetlinkMessage::new(
            {
                let mut header = NetlinkHeader::default();
                header.flags = netlink_packet_core::NLM_F_REQUEST;
                header
            },
            NetlinkPayload::InnerMessage(RtnlMessage::GetLink({
                let mut msg = LinkMessage::default();
                msg.nlas = vec![LinkNla::IfName(iface_name.to_string())];
                msg
            })),
        );
        msg.finalize();
        let mut buf = vec![0; msg.buffer_len()];
        msg.serialize(&mut buf[..]);
        assert_eq!(
            socket.write(current_task, &mut VecInputBuffer::from(buf))?,
            msg.buffer_len(),
            "netlink sockets do not support partial writes",
        );
    }

    // Read the response to get the link-layer info, or error.
    let link_msg = {
        read_buf.reset();
        let n = socket.read(current_task, read_buf)?;

        let msg = NetlinkMessage::<RtnlMessage>::deserialize(&read_buf.data()[..n])
            .expect("netlink should always send well-formed messages");
        match msg.payload {
            NetlinkPayload::Error(e) => {
                // `e.code` is an `i32` and may hold negative values so
                // we need to do an `as u64` cast instead of `try_into`.
                // Note that `ErrnoCode::from_return_value` will
                // cast the value to an `i64` to check that it is a
                // valid (negative) errno value.
                let code = ErrnoCode::from_return_value(e.code as u64);
                return Err(Errno::new(code, "error code from RTM_GETLINK", None));
            }
            NetlinkPayload::InnerMessage(RtnlMessage::NewLink(msg)) => msg,
            // netlink is only expected to return an error or
            // RTM_NEWLINK response for our RTM_GETLINK request.
            payload => panic!("unexpected message = {:?}", payload),
        }
    };

    Ok((socket, link_msg))
}

/// Creates a netlink socket and performs an `RTM_GETADDR` dump request for the
/// requested interface requested in `in_ifreq`.
///
/// Returns the netlink socket, the list of addresses and interface index, or an
/// [`Errno`] if the operation failed.
fn get_netlink_ipv4_addresses(
    current_task: &CurrentTask,
    in_ifreq: &ifreq,
    read_buf: &mut VecOutputBuffer,
) -> Result<(FileHandle, Vec<AddressMessage>, u32), Errno> {
    let sockaddr { sa_family, sa_data: _ } = unsafe { in_ifreq.ifr_ifru.ifru_addr };
    if sa_family != AF_INET {
        return error!(EINVAL);
    }

    let (socket, link_msg) = get_netlink_interface_info(current_task, in_ifreq, read_buf)?;
    let if_index = link_msg.header.index;

    // Send the request to dump all IPv4 addresses.
    {
        let mut msg = NetlinkMessage::new(
            {
                let mut header = NetlinkHeader::default();
                header.flags = netlink_packet_core::NLM_F_DUMP | netlink_packet_core::NLM_F_REQUEST;
                header
            },
            NetlinkPayload::InnerMessage(RtnlMessage::GetAddress({
                let mut msg = AddressMessage::default();
                msg.header.family = AF_INET.try_into().expect("AF_INET should fit in u8");
                msg
            })),
        );
        msg.finalize();
        let mut buf = vec![0; msg.buffer_len()];
        msg.serialize(&mut buf[..]);
        assert_eq!(socket.write(current_task, &mut VecInputBuffer::from(buf))?, msg.buffer_len());
    }

    // Collect all the addresses.
    let mut addrs = Vec::new();
    loop {
        read_buf.reset();
        let n = socket.read(current_task, read_buf)?;

        let msg = NetlinkMessage::<RtnlMessage>::deserialize(&read_buf.data()[..n])
            .expect("netlink should always send well-formed messages");
        match msg.payload {
            NetlinkPayload::Done => break,
            NetlinkPayload::InnerMessage(RtnlMessage::NewAddress(msg)) => {
                if msg.header.index == if_index {
                    addrs.push(msg);
                }
            }
            payload => panic!("unexpected message = {:?}", payload),
        }
    }

    Ok((socket, addrs, if_index))
}

fn send_netlink_msg_and_wait_response(
    current_task: &CurrentTask,
    socket: &FileHandle,
    mut msg: NetlinkMessage<RtnlMessage>,
    read_buf: &mut VecOutputBuffer,
    req: u32,
) -> Result<(), Errno> {
    msg.finalize();
    let mut buf = vec![0; msg.buffer_len()];
    msg.serialize(&mut buf[..]);
    assert_eq!(socket.write(current_task, &mut VecInputBuffer::from(buf))?, msg.buffer_len());

    read_buf.reset();
    let n = socket.read(current_task, read_buf)?;
    let msg = NetlinkMessage::<RtnlMessage>::deserialize(&read_buf.data()[..n])
        .expect("netlink should always send well-formed messages");
    match msg.payload {
        NetlinkPayload::Ack(_) => {}
        NetlinkPayload::Error(msg) => {
            // Don't propagate the error up because its not the fault of the
            // caller - the stack state can change underneath the caller.
            log_warn!(
                "got NACK netlink route response when handling ioctl(_, {:#x}, _): {}",
                req,
                msg.code
            );
        }
        payload => panic!("unexpected message = {:?}", payload),
    }

    Ok(())
}

// Check that values that are passed to and from ZXIO have the same meaning.
const_assert_eq!(syncio::zxio::AF_UNSPEC, uapi::AF_UNSPEC as u32);
const_assert_eq!(syncio::zxio::AF_UNIX, uapi::AF_UNIX as u32);
const_assert_eq!(syncio::zxio::AF_INET, uapi::AF_INET as u32);
const_assert_eq!(syncio::zxio::AF_INET6, uapi::AF_INET6 as u32);
const_assert_eq!(syncio::zxio::AF_NETLINK, uapi::AF_NETLINK as u32);
const_assert_eq!(syncio::zxio::AF_PACKET, uapi::AF_PACKET as u32);
const_assert_eq!(syncio::zxio::AF_VSOCK, uapi::AF_VSOCK as u32);

const_assert_eq!(syncio::zxio::SO_DEBUG, uapi::SO_DEBUG);
const_assert_eq!(syncio::zxio::SO_REUSEADDR, uapi::SO_REUSEADDR);
const_assert_eq!(syncio::zxio::SO_TYPE, uapi::SO_TYPE);
const_assert_eq!(syncio::zxio::SO_ERROR, uapi::SO_ERROR);
const_assert_eq!(syncio::zxio::SO_DONTROUTE, uapi::SO_DONTROUTE);
const_assert_eq!(syncio::zxio::SO_BROADCAST, uapi::SO_BROADCAST);
const_assert_eq!(syncio::zxio::SO_SNDBUF, uapi::SO_SNDBUF);
const_assert_eq!(syncio::zxio::SO_RCVBUF, uapi::SO_RCVBUF);
const_assert_eq!(syncio::zxio::SO_KEEPALIVE, uapi::SO_KEEPALIVE);
const_assert_eq!(syncio::zxio::SO_OOBINLINE, uapi::SO_OOBINLINE);
const_assert_eq!(syncio::zxio::SO_NO_CHECK, uapi::SO_NO_CHECK);
const_assert_eq!(syncio::zxio::SO_PRIORITY, uapi::SO_PRIORITY);
const_assert_eq!(syncio::zxio::SO_LINGER, uapi::SO_LINGER);
const_assert_eq!(syncio::zxio::SO_BSDCOMPAT, uapi::SO_BSDCOMPAT);
const_assert_eq!(syncio::zxio::SO_REUSEPORT, uapi::SO_REUSEPORT);
const_assert_eq!(syncio::zxio::SO_PASSCRED, uapi::SO_PASSCRED);
const_assert_eq!(syncio::zxio::SO_PEERCRED, uapi::SO_PEERCRED);
const_assert_eq!(syncio::zxio::SO_RCVLOWAT, uapi::SO_RCVLOWAT);
const_assert_eq!(syncio::zxio::SO_SNDLOWAT, uapi::SO_SNDLOWAT);
const_assert_eq!(syncio::zxio::SO_ACCEPTCONN, uapi::SO_ACCEPTCONN);
const_assert_eq!(syncio::zxio::SO_PEERSEC, uapi::SO_PEERSEC);
const_assert_eq!(syncio::zxio::SO_SNDBUFFORCE, uapi::SO_SNDBUFFORCE);
const_assert_eq!(syncio::zxio::SO_RCVBUFFORCE, uapi::SO_RCVBUFFORCE);
const_assert_eq!(syncio::zxio::SO_PROTOCOL, uapi::SO_PROTOCOL);
const_assert_eq!(syncio::zxio::SO_DOMAIN, uapi::SO_DOMAIN);
const_assert_eq!(syncio::zxio::SO_RCVTIMEO, uapi::SO_RCVTIMEO);
const_assert_eq!(syncio::zxio::SO_SNDTIMEO, uapi::SO_SNDTIMEO);
const_assert_eq!(syncio::zxio::SO_TIMESTAMP, uapi::SO_TIMESTAMP);
const_assert_eq!(syncio::zxio::SO_TIMESTAMPNS, uapi::SO_TIMESTAMPNS);
const_assert_eq!(syncio::zxio::SO_TIMESTAMPING, uapi::SO_TIMESTAMPING);
const_assert_eq!(syncio::zxio::SO_SECURITY_AUTHENTICATION, uapi::SO_SECURITY_AUTHENTICATION);
const_assert_eq!(
    syncio::zxio::SO_SECURITY_ENCRYPTION_TRANSPORT,
    uapi::SO_SECURITY_ENCRYPTION_TRANSPORT
);
const_assert_eq!(
    syncio::zxio::SO_SECURITY_ENCRYPTION_NETWORK,
    uapi::SO_SECURITY_ENCRYPTION_NETWORK
);
const_assert_eq!(syncio::zxio::SO_BINDTODEVICE, uapi::SO_BINDTODEVICE);
const_assert_eq!(syncio::zxio::SO_ATTACH_FILTER, uapi::SO_ATTACH_FILTER);
const_assert_eq!(syncio::zxio::SO_DETACH_FILTER, uapi::SO_DETACH_FILTER);
const_assert_eq!(syncio::zxio::SO_GET_FILTER, uapi::SO_GET_FILTER);
const_assert_eq!(syncio::zxio::SO_PEERNAME, uapi::SO_PEERNAME);
const_assert_eq!(syncio::zxio::SO_PASSSEC, uapi::SO_PASSSEC);
const_assert_eq!(syncio::zxio::SO_MARK, uapi::SO_MARK);
const_assert_eq!(syncio::zxio::SO_RXQ_OVFL, uapi::SO_RXQ_OVFL);
const_assert_eq!(syncio::zxio::SO_WIFI_STATUS, uapi::SO_WIFI_STATUS);
const_assert_eq!(syncio::zxio::SO_PEEK_OFF, uapi::SO_PEEK_OFF);
const_assert_eq!(syncio::zxio::SO_NOFCS, uapi::SO_NOFCS);
const_assert_eq!(syncio::zxio::SO_LOCK_FILTER, uapi::SO_LOCK_FILTER);
const_assert_eq!(syncio::zxio::SO_SELECT_ERR_QUEUE, uapi::SO_SELECT_ERR_QUEUE);
const_assert_eq!(syncio::zxio::SO_BUSY_POLL, uapi::SO_BUSY_POLL);
const_assert_eq!(syncio::zxio::SO_MAX_PACING_RATE, uapi::SO_MAX_PACING_RATE);
const_assert_eq!(syncio::zxio::SO_BPF_EXTENSIONS, uapi::SO_BPF_EXTENSIONS);
const_assert_eq!(syncio::zxio::SO_INCOMING_CPU, uapi::SO_INCOMING_CPU);
const_assert_eq!(syncio::zxio::SO_ATTACH_BPF, uapi::SO_ATTACH_BPF);
const_assert_eq!(syncio::zxio::SO_DETACH_BPF, uapi::SO_DETACH_BPF);
const_assert_eq!(syncio::zxio::SO_ATTACH_REUSEPORT_CBPF, uapi::SO_ATTACH_REUSEPORT_CBPF);
const_assert_eq!(syncio::zxio::SO_ATTACH_REUSEPORT_EBPF, uapi::SO_ATTACH_REUSEPORT_EBPF);
const_assert_eq!(syncio::zxio::SO_CNX_ADVICE, uapi::SO_CNX_ADVICE);
const_assert_eq!(syncio::zxio::SO_MEMINFO, uapi::SO_MEMINFO);
const_assert_eq!(syncio::zxio::SO_INCOMING_NAPI_ID, uapi::SO_INCOMING_NAPI_ID);
const_assert_eq!(syncio::zxio::SO_COOKIE, uapi::SO_COOKIE);
const_assert_eq!(syncio::zxio::SO_PEERGROUPS, uapi::SO_PEERGROUPS);
const_assert_eq!(syncio::zxio::SO_ZEROCOPY, uapi::SO_ZEROCOPY);
const_assert_eq!(syncio::zxio::SO_TXTIME, uapi::SO_TXTIME);
const_assert_eq!(syncio::zxio::SO_BINDTOIFINDEX, uapi::SO_BINDTOIFINDEX);
const_assert_eq!(syncio::zxio::SO_DETACH_REUSEPORT_BPF, uapi::SO_DETACH_REUSEPORT_BPF);

const_assert_eq!(syncio::zxio::MSG_WAITALL, uapi::MSG_WAITALL);
const_assert_eq!(syncio::zxio::MSG_PEEK, uapi::MSG_PEEK);
const_assert_eq!(syncio::zxio::MSG_DONTROUTE, uapi::MSG_DONTROUTE);
const_assert_eq!(syncio::zxio::MSG_CTRUNC, uapi::MSG_CTRUNC);
const_assert_eq!(syncio::zxio::MSG_PROXY, uapi::MSG_PROXY);
const_assert_eq!(syncio::zxio::MSG_TRUNC, uapi::MSG_TRUNC);
const_assert_eq!(syncio::zxio::MSG_DONTWAIT, uapi::MSG_DONTWAIT);
const_assert_eq!(syncio::zxio::MSG_EOR, uapi::MSG_EOR);
const_assert_eq!(syncio::zxio::MSG_WAITALL, uapi::MSG_WAITALL);
const_assert_eq!(syncio::zxio::MSG_FIN, uapi::MSG_FIN);
const_assert_eq!(syncio::zxio::MSG_SYN, uapi::MSG_SYN);
const_assert_eq!(syncio::zxio::MSG_CONFIRM, uapi::MSG_CONFIRM);
const_assert_eq!(syncio::zxio::MSG_RST, uapi::MSG_RST);
const_assert_eq!(syncio::zxio::MSG_ERRQUEUE, uapi::MSG_ERRQUEUE);
const_assert_eq!(syncio::zxio::MSG_NOSIGNAL, uapi::MSG_NOSIGNAL);
const_assert_eq!(syncio::zxio::MSG_MORE, uapi::MSG_MORE);
const_assert_eq!(syncio::zxio::MSG_WAITFORONE, uapi::MSG_WAITFORONE);
const_assert_eq!(syncio::zxio::MSG_BATCH, uapi::MSG_BATCH);
const_assert_eq!(syncio::zxio::MSG_FASTOPEN, uapi::MSG_FASTOPEN);
const_assert_eq!(syncio::zxio::MSG_CMSG_CLOEXEC, uapi::MSG_CMSG_CLOEXEC);

const_assert_eq!(syncio::zxio::IP_TOS, uapi::IP_TOS);
const_assert_eq!(syncio::zxio::IP_TTL, uapi::IP_TTL);
const_assert_eq!(syncio::zxio::IP_HDRINCL, uapi::IP_HDRINCL);
const_assert_eq!(syncio::zxio::IP_OPTIONS, uapi::IP_OPTIONS);
const_assert_eq!(syncio::zxio::IP_ROUTER_ALERT, uapi::IP_ROUTER_ALERT);
const_assert_eq!(syncio::zxio::IP_RECVOPTS, uapi::IP_RECVOPTS);
const_assert_eq!(syncio::zxio::IP_RETOPTS, uapi::IP_RETOPTS);
const_assert_eq!(syncio::zxio::IP_PKTINFO, uapi::IP_PKTINFO);
const_assert_eq!(syncio::zxio::IP_PKTOPTIONS, uapi::IP_PKTOPTIONS);
const_assert_eq!(syncio::zxio::IP_MTU_DISCOVER, uapi::IP_MTU_DISCOVER);
const_assert_eq!(syncio::zxio::IP_RECVERR, uapi::IP_RECVERR);
const_assert_eq!(syncio::zxio::IP_RECVTTL, uapi::IP_RECVTTL);
const_assert_eq!(syncio::zxio::IP_RECVTOS, uapi::IP_RECVTOS);
const_assert_eq!(syncio::zxio::IP_MTU, uapi::IP_MTU);
const_assert_eq!(syncio::zxio::IP_FREEBIND, uapi::IP_FREEBIND);
const_assert_eq!(syncio::zxio::IP_IPSEC_POLICY, uapi::IP_IPSEC_POLICY);
const_assert_eq!(syncio::zxio::IP_XFRM_POLICY, uapi::IP_XFRM_POLICY);
const_assert_eq!(syncio::zxio::IP_PASSSEC, uapi::IP_PASSSEC);
const_assert_eq!(syncio::zxio::IP_TRANSPARENT, uapi::IP_TRANSPARENT);
const_assert_eq!(syncio::zxio::IP_ORIGDSTADDR, uapi::IP_ORIGDSTADDR);
const_assert_eq!(syncio::zxio::IP_RECVORIGDSTADDR, uapi::IP_RECVORIGDSTADDR);
const_assert_eq!(syncio::zxio::IP_MINTTL, uapi::IP_MINTTL);
const_assert_eq!(syncio::zxio::IP_NODEFRAG, uapi::IP_NODEFRAG);
const_assert_eq!(syncio::zxio::IP_CHECKSUM, uapi::IP_CHECKSUM);
const_assert_eq!(syncio::zxio::IP_BIND_ADDRESS_NO_PORT, uapi::IP_BIND_ADDRESS_NO_PORT);
const_assert_eq!(syncio::zxio::IP_MULTICAST_IF, uapi::IP_MULTICAST_IF);
const_assert_eq!(syncio::zxio::IP_MULTICAST_TTL, uapi::IP_MULTICAST_TTL);
const_assert_eq!(syncio::zxio::IP_MULTICAST_LOOP, uapi::IP_MULTICAST_LOOP);
const_assert_eq!(syncio::zxio::IP_ADD_MEMBERSHIP, uapi::IP_ADD_MEMBERSHIP);
const_assert_eq!(syncio::zxio::IP_DROP_MEMBERSHIP, uapi::IP_DROP_MEMBERSHIP);
const_assert_eq!(syncio::zxio::IP_UNBLOCK_SOURCE, uapi::IP_UNBLOCK_SOURCE);
const_assert_eq!(syncio::zxio::IP_BLOCK_SOURCE, uapi::IP_BLOCK_SOURCE);
const_assert_eq!(syncio::zxio::IP_ADD_SOURCE_MEMBERSHIP, uapi::IP_ADD_SOURCE_MEMBERSHIP);
const_assert_eq!(syncio::zxio::IP_DROP_SOURCE_MEMBERSHIP, uapi::IP_DROP_SOURCE_MEMBERSHIP);
const_assert_eq!(syncio::zxio::IP_MSFILTER, uapi::IP_MSFILTER);
const_assert_eq!(syncio::zxio::IP_MULTICAST_ALL, uapi::IP_MULTICAST_ALL);
const_assert_eq!(syncio::zxio::IP_UNICAST_IF, uapi::IP_UNICAST_IF);
const_assert_eq!(syncio::zxio::IP_RECVRETOPTS, uapi::IP_RECVRETOPTS);
const_assert_eq!(syncio::zxio::IP_PMTUDISC_DONT, uapi::IP_PMTUDISC_DONT);
const_assert_eq!(syncio::zxio::IP_PMTUDISC_WANT, uapi::IP_PMTUDISC_WANT);
const_assert_eq!(syncio::zxio::IP_PMTUDISC_DO, uapi::IP_PMTUDISC_DO);
const_assert_eq!(syncio::zxio::IP_PMTUDISC_PROBE, uapi::IP_PMTUDISC_PROBE);
const_assert_eq!(syncio::zxio::IP_PMTUDISC_INTERFACE, uapi::IP_PMTUDISC_INTERFACE);
const_assert_eq!(syncio::zxio::IP_PMTUDISC_OMIT, uapi::IP_PMTUDISC_OMIT);
const_assert_eq!(syncio::zxio::IP_DEFAULT_MULTICAST_TTL, uapi::IP_DEFAULT_MULTICAST_TTL);
const_assert_eq!(syncio::zxio::IP_DEFAULT_MULTICAST_LOOP, uapi::IP_DEFAULT_MULTICAST_LOOP);

const_assert_eq!(syncio::zxio::IPV6_ADDRFORM, uapi::IPV6_ADDRFORM);
const_assert_eq!(syncio::zxio::IPV6_2292PKTINFO, uapi::IPV6_2292PKTINFO);
const_assert_eq!(syncio::zxio::IPV6_2292HOPOPTS, uapi::IPV6_2292HOPOPTS);
const_assert_eq!(syncio::zxio::IPV6_2292DSTOPTS, uapi::IPV6_2292DSTOPTS);
const_assert_eq!(syncio::zxio::IPV6_2292RTHDR, uapi::IPV6_2292RTHDR);
const_assert_eq!(syncio::zxio::IPV6_2292PKTOPTIONS, uapi::IPV6_2292PKTOPTIONS);
const_assert_eq!(syncio::zxio::IPV6_CHECKSUM, uapi::IPV6_CHECKSUM);
const_assert_eq!(syncio::zxio::IPV6_2292HOPLIMIT, uapi::IPV6_2292HOPLIMIT);
const_assert_eq!(syncio::zxio::IPV6_NEXTHOP, uapi::IPV6_NEXTHOP);
const_assert_eq!(syncio::zxio::IPV6_AUTHHDR, uapi::IPV6_AUTHHDR);
const_assert_eq!(syncio::zxio::IPV6_UNICAST_HOPS, uapi::IPV6_UNICAST_HOPS);
const_assert_eq!(syncio::zxio::IPV6_MULTICAST_IF, uapi::IPV6_MULTICAST_IF);
const_assert_eq!(syncio::zxio::IPV6_MULTICAST_HOPS, uapi::IPV6_MULTICAST_HOPS);
const_assert_eq!(syncio::zxio::IPV6_MULTICAST_LOOP, uapi::IPV6_MULTICAST_LOOP);
const_assert_eq!(syncio::zxio::IPV6_ROUTER_ALERT, uapi::IPV6_ROUTER_ALERT);
const_assert_eq!(syncio::zxio::IPV6_MTU_DISCOVER, uapi::IPV6_MTU_DISCOVER);
const_assert_eq!(syncio::zxio::IPV6_MTU, uapi::IPV6_MTU);
const_assert_eq!(syncio::zxio::IPV6_RECVERR, uapi::IPV6_RECVERR);
const_assert_eq!(syncio::zxio::IPV6_V6ONLY, uapi::IPV6_V6ONLY);
const_assert_eq!(syncio::zxio::IPV6_JOIN_ANYCAST, uapi::IPV6_JOIN_ANYCAST);
const_assert_eq!(syncio::zxio::IPV6_LEAVE_ANYCAST, uapi::IPV6_LEAVE_ANYCAST);
const_assert_eq!(syncio::zxio::IPV6_IPSEC_POLICY, uapi::IPV6_IPSEC_POLICY);
const_assert_eq!(syncio::zxio::IPV6_XFRM_POLICY, uapi::IPV6_XFRM_POLICY);
const_assert_eq!(syncio::zxio::IPV6_HDRINCL, uapi::IPV6_HDRINCL);
const_assert_eq!(syncio::zxio::IPV6_RECVPKTINFO, uapi::IPV6_RECVPKTINFO);
const_assert_eq!(syncio::zxio::IPV6_PKTINFO, uapi::IPV6_PKTINFO);
const_assert_eq!(syncio::zxio::IPV6_RECVHOPLIMIT, uapi::IPV6_RECVHOPLIMIT);
const_assert_eq!(syncio::zxio::IPV6_HOPLIMIT, uapi::IPV6_HOPLIMIT);
const_assert_eq!(syncio::zxio::IPV6_RECVHOPOPTS, uapi::IPV6_RECVHOPOPTS);
const_assert_eq!(syncio::zxio::IPV6_HOPOPTS, uapi::IPV6_HOPOPTS);
const_assert_eq!(syncio::zxio::IPV6_RTHDRDSTOPTS, uapi::IPV6_RTHDRDSTOPTS);
const_assert_eq!(syncio::zxio::IPV6_RECVRTHDR, uapi::IPV6_RECVRTHDR);
const_assert_eq!(syncio::zxio::IPV6_RTHDR, uapi::IPV6_RTHDR);
const_assert_eq!(syncio::zxio::IPV6_RECVDSTOPTS, uapi::IPV6_RECVDSTOPTS);
const_assert_eq!(syncio::zxio::IPV6_DSTOPTS, uapi::IPV6_DSTOPTS);
const_assert_eq!(syncio::zxio::IPV6_RECVPATHMTU, uapi::IPV6_RECVPATHMTU);
const_assert_eq!(syncio::zxio::IPV6_PATHMTU, uapi::IPV6_PATHMTU);
const_assert_eq!(syncio::zxio::IPV6_DONTFRAG, uapi::IPV6_DONTFRAG);
const_assert_eq!(syncio::zxio::IPV6_RECVTCLASS, uapi::IPV6_RECVTCLASS);
const_assert_eq!(syncio::zxio::IPV6_TCLASS, uapi::IPV6_TCLASS);
const_assert_eq!(syncio::zxio::IPV6_AUTOFLOWLABEL, uapi::IPV6_AUTOFLOWLABEL);
const_assert_eq!(syncio::zxio::IPV6_ADDR_PREFERENCES, uapi::IPV6_ADDR_PREFERENCES);
const_assert_eq!(syncio::zxio::IPV6_MINHOPCOUNT, uapi::IPV6_MINHOPCOUNT);
const_assert_eq!(syncio::zxio::IPV6_ORIGDSTADDR, uapi::IPV6_ORIGDSTADDR);
const_assert_eq!(syncio::zxio::IPV6_RECVORIGDSTADDR, uapi::IPV6_RECVORIGDSTADDR);
const_assert_eq!(syncio::zxio::IPV6_TRANSPARENT, uapi::IPV6_TRANSPARENT);
const_assert_eq!(syncio::zxio::IPV6_UNICAST_IF, uapi::IPV6_UNICAST_IF);
const_assert_eq!(syncio::zxio::IPV6_ADD_MEMBERSHIP, uapi::IPV6_ADD_MEMBERSHIP);
const_assert_eq!(syncio::zxio::IPV6_DROP_MEMBERSHIP, uapi::IPV6_DROP_MEMBERSHIP);
const_assert_eq!(syncio::zxio::IPV6_PMTUDISC_DONT, uapi::IPV6_PMTUDISC_DONT);
const_assert_eq!(syncio::zxio::IPV6_PMTUDISC_WANT, uapi::IPV6_PMTUDISC_WANT);
const_assert_eq!(syncio::zxio::IPV6_PMTUDISC_DO, uapi::IPV6_PMTUDISC_DO);
const_assert_eq!(syncio::zxio::IPV6_PMTUDISC_PROBE, uapi::IPV6_PMTUDISC_PROBE);
const_assert_eq!(syncio::zxio::IPV6_PMTUDISC_INTERFACE, uapi::IPV6_PMTUDISC_INTERFACE);
const_assert_eq!(syncio::zxio::IPV6_PMTUDISC_OMIT, uapi::IPV6_PMTUDISC_OMIT);
const_assert_eq!(syncio::zxio::IPV6_PREFER_SRC_TMP, uapi::IPV6_PREFER_SRC_TMP);
const_assert_eq!(syncio::zxio::IPV6_PREFER_SRC_PUBLIC, uapi::IPV6_PREFER_SRC_PUBLIC);
const_assert_eq!(
    syncio::zxio::IPV6_PREFER_SRC_PUBTMP_DEFAULT,
    uapi::IPV6_PREFER_SRC_PUBTMP_DEFAULT
);
const_assert_eq!(syncio::zxio::IPV6_PREFER_SRC_COA, uapi::IPV6_PREFER_SRC_COA);
const_assert_eq!(syncio::zxio::IPV6_PREFER_SRC_HOME, uapi::IPV6_PREFER_SRC_HOME);
const_assert_eq!(syncio::zxio::IPV6_PREFER_SRC_CGA, uapi::IPV6_PREFER_SRC_CGA);
const_assert_eq!(syncio::zxio::IPV6_PREFER_SRC_NONCGA, uapi::IPV6_PREFER_SRC_NONCGA);
