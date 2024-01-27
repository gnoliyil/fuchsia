// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;
use zerocopy::{AsBytes, FromBytes};

use crate::device::{DeviceListener, DeviceListenerKey};
use crate::fs::buffers::*;
use crate::fs::*;
use crate::lock::Mutex;
use crate::logging::not_implemented;
use crate::mm::MemoryAccessorExt;
use crate::task::*;
use crate::types::*;
use std::sync::Arc;

// From netlink/socket.go in gVisor.
pub const SOCKET_MIN_SIZE: usize = 4 << 10;
pub const SOCKET_DEFAULT_SIZE: usize = 16 * 1024;
pub const SOCKET_MAX_SIZE: usize = 4 << 20;

pub fn new_netlink_socket(
    kernel: &Arc<Kernel>,
    socket_type: SocketType,
    family: NetlinkFamily,
) -> Result<Box<dyn SocketOps>, Errno> {
    if socket_type != SocketType::Datagram && socket_type != SocketType::Raw {
        return error!(ESOCKTNOSUPPORT);
    }

    let ops: Box<dyn SocketOps> = match family {
        NetlinkFamily::KobjectUevent => Box::new(UEventNetlinkSocket::new(kernel)),
        _ => Box::new(BaseNetlinkSocket::new(family)),
    };
    Ok(ops)
}

#[derive(Default, Debug, Clone, PartialEq, Eq)]
#[repr(C)]
pub struct NetlinkAddress {
    pid: u32,
    groups: u32,
}

impl NetlinkAddress {
    pub fn new(pid: u32, groups: u32) -> Self {
        NetlinkAddress { pid, groups }
    }

    pub fn set_pid_if_zero(&mut self, pid: i32) {
        if self.pid == 0 {
            self.pid = pid as u32;
        }
    }

    pub fn to_bytes(&self) -> Vec<u8> {
        sockaddr_nl { nl_family: AF_NETLINK, nl_pid: self.pid, nl_pad: 0, nl_groups: self.groups }
            .as_bytes()
            .to_vec()
    }
}

#[derive(Debug, Hash, Eq, PartialEq, Clone)]
pub enum NetlinkFamily {
    Unsupported,
    Route,
    Usersock,
    Firewall,
    SockDiag,
    Nflog,
    Xfrm,
    Selinux,
    Iscsi,
    Audit,
    FibLookup,
    Connector,
    Netfilter,
    Ip6Fw,
    Dnrtmsg,
    KobjectUevent,
    Generic,
    Scsitransport,
    Ecryptfs,
    Rdma,
    Crypto,
    Smc,
}

impl NetlinkFamily {
    pub fn from_raw(family: u32) -> Self {
        match family {
            NETLINK_ROUTE => NetlinkFamily::Route,
            NETLINK_USERSOCK => NetlinkFamily::Usersock,
            NETLINK_FIREWALL => NetlinkFamily::Firewall,
            NETLINK_SOCK_DIAG => NetlinkFamily::SockDiag,
            NETLINK_NFLOG => NetlinkFamily::Nflog,
            NETLINK_XFRM => NetlinkFamily::Xfrm,
            NETLINK_SELINUX => NetlinkFamily::Selinux,
            NETLINK_ISCSI => NetlinkFamily::Iscsi,
            NETLINK_AUDIT => NetlinkFamily::Audit,
            NETLINK_FIB_LOOKUP => NetlinkFamily::FibLookup,
            NETLINK_CONNECTOR => NetlinkFamily::Connector,
            NETLINK_NETFILTER => NetlinkFamily::Netfilter,
            NETLINK_IP6_FW => NetlinkFamily::Ip6Fw,
            NETLINK_DNRTMSG => NetlinkFamily::Dnrtmsg,
            NETLINK_KOBJECT_UEVENT => NetlinkFamily::KobjectUevent,
            NETLINK_GENERIC => NetlinkFamily::Generic,
            NETLINK_SCSITRANSPORT => NetlinkFamily::Scsitransport,
            NETLINK_ECRYPTFS => NetlinkFamily::Ecryptfs,
            NETLINK_RDMA => NetlinkFamily::Rdma,
            NETLINK_CRYPTO => NetlinkFamily::Crypto,
            NETLINK_SMC => NetlinkFamily::Smc,
            _ => NetlinkFamily::Unsupported,
        }
    }

    pub fn as_raw(&self) -> u32 {
        match self {
            NetlinkFamily::Route => NETLINK_ROUTE,
            NetlinkFamily::KobjectUevent => NETLINK_KOBJECT_UEVENT,
            _ => 0,
        }
    }
}

#[allow(dead_code)]
struct NetlinkSocketInner {
    /// The specific type of netlink socket.
    family: NetlinkFamily,

    /// The `MessageQueue` that contains messages sent to this socket.
    messages: MessageQueue,

    /// This queue will be notified on reads, writes, disconnects etc.
    waiters: WaitQueue,

    /// The address of this socket.
    address: Option<NetlinkAddress>,

    /// See SO_PASSCRED.
    pub passcred: bool,

    /// See SO_TIMESTAMP.
    pub timestamp: bool,
}

impl NetlinkSocketInner {
    fn bind(
        &mut self,
        current_task: &CurrentTask,
        socket_address: SocketAddress,
    ) -> Result<(), Errno> {
        if self.address.is_some() {
            return error!(EINVAL);
        }

        let netlink_address = match socket_address {
            SocketAddress::Netlink(mut netlink_address) => {
                // TODO: Support distinct IDs for processes with multiple netlink sockets.
                netlink_address.set_pid_if_zero(current_task.get_pid());
                netlink_address
            }
            _ => return error!(EINVAL),
        };

        self.address = Some(netlink_address);
        Ok(())
    }

    fn connect(&mut self, current_task: &CurrentTask, peer: SocketPeer) -> Result<(), Errno> {
        let address = match peer {
            SocketPeer::Address(address) => address,
            _ => return error!(EINVAL),
        };
        // Connect is equivalent to bind, but error are ignored.
        let _ = self.bind(current_task, address);
        Ok(())
    }

    fn set_capacity(&mut self, requested_capacity: usize) {
        let capacity = requested_capacity.clamp(SOCKET_MIN_SIZE, SOCKET_MAX_SIZE);
        let capacity = std::cmp::max(capacity, self.messages.len());
        // We have validated capacity sufficiently that set_capacity should always succeed.
        self.messages.set_capacity(capacity).unwrap();
    }

    fn read_message(&mut self) -> Option<Message> {
        let message = self.messages.read_message();
        if message.is_some() {
            self.waiters.notify_events(FdEvents::POLLOUT);
        }
        message
    }

    fn read_datagram(&mut self, data: &mut dyn OutputBuffer) -> Result<MessageReadInfo, Errno> {
        let info = self.messages.read_datagram(data)?;
        if info.message_length == 0 {
            return error!(EAGAIN);
        }
        Ok(info)
    }

    fn write_to_queue(
        &mut self,
        data: &mut dyn InputBuffer,
        address: Option<NetlinkAddress>,
        ancillary_data: &mut Vec<AncillaryData>,
    ) -> Result<usize, Errno> {
        let socket_address = match address {
            Some(addr) => Some(SocketAddress::Netlink(addr)),
            None => self.address.as_ref().map(|addr| SocketAddress::Netlink(addr.clone())),
        };
        let bytes_written = self.messages.write_datagram(data, socket_address, ancillary_data)?;
        if bytes_written > 0 {
            self.waiters.notify_events(FdEvents::POLLIN);
        }
        Ok(bytes_written)
    }

    fn wait_async(
        &mut self,
        waiter: &Waiter,
        events: FdEvents,
        handler: EventHandler,
    ) -> WaitCanceler {
        self.waiters.wait_async_mask(waiter, events.bits(), handler)
    }

    fn query_events(&self) -> FdEvents {
        self.messages.query_events()
    }

    fn getsockname(&self) -> Vec<u8> {
        match &self.address {
            Some(addr) => addr.to_bytes(),
            _ => vec![],
        }
    }

    fn getpeername(&self) -> Result<Vec<u8>, Errno> {
        match &self.address {
            Some(addr) => Ok(addr.to_bytes()),
            None => {
                let addr = sockaddr_nl { nl_family: AF_NETLINK, ..Default::default() };
                Ok(addr.as_bytes().to_vec())
            }
        }
    }

    fn getsockopt(&self, level: u32, optname: u32) -> Result<Vec<u8>, Errno> {
        let opt_value = match level {
            SOL_SOCKET => match optname {
                SO_PASSCRED => (self.passcred as u32).as_bytes().to_vec(),
                SO_TIMESTAMP => (self.timestamp as u32).as_bytes().to_vec(),
                SO_SNDBUF => (self.messages.capacity() as socklen_t).to_ne_bytes().to_vec(),
                SO_RCVBUF => (self.messages.capacity() as socklen_t).to_ne_bytes().to_vec(),
                SO_SNDBUFFORCE => (self.messages.capacity() as socklen_t).to_ne_bytes().to_vec(),
                SO_RCVBUFFORCE => (self.messages.capacity() as socklen_t).to_ne_bytes().to_vec(),
                SO_PROTOCOL => self.family.as_raw().as_bytes().to_vec(),
                _ => return error!(ENOSYS),
            },
            _ => vec![],
        };

        Ok(opt_value)
    }

    fn setsockopt(
        &mut self,
        task: &Task,
        level: u32,
        optname: u32,
        user_opt: UserBuffer,
    ) -> Result<(), Errno> {
        fn read<T: Default + AsBytes + FromBytes>(
            task: &Task,
            user_opt: UserBuffer,
        ) -> Result<T, Errno> {
            let user_ref = UserRef::<T>::from_buf(user_opt).ok_or_else(|| errno!(EINVAL))?;
            task.mm.read_object(user_ref)
        }

        match level {
            SOL_SOCKET => match optname {
                SO_SNDBUF => {
                    let requested_capacity = read::<socklen_t>(task, user_opt)? as usize;
                    self.set_capacity(requested_capacity * 2);
                }
                SO_RCVBUF => {
                    let requested_capacity = read::<socklen_t>(task, user_opt)? as usize;
                    self.set_capacity(requested_capacity);
                }
                SO_PASSCRED => {
                    let passcred = read::<u32>(task, user_opt)?;
                    self.passcred = passcred != 0;
                }
                SO_TIMESTAMP => {
                    let timestamp = read::<u32>(task, user_opt)?;
                    self.timestamp = timestamp != 0;
                }
                SO_SNDBUFFORCE => {
                    if !task.creds().has_capability(CAP_NET_ADMIN) {
                        return error!(EPERM);
                    }
                    let requested_capacity = read::<socklen_t>(task, user_opt)? as usize;
                    self.set_capacity(requested_capacity * 2);
                }
                SO_RCVBUFFORCE => {
                    if !task.creds().has_capability(CAP_NET_ADMIN) {
                        return error!(EPERM);
                    }
                    let requested_capacity = read::<socklen_t>(task, user_opt)? as usize;
                    self.set_capacity(requested_capacity);
                }
                _ => return error!(ENOSYS),
            },
            _ => return error!(ENOSYS),
        }

        Ok(())
    }
}

struct BaseNetlinkSocket {
    inner: Mutex<NetlinkSocketInner>,
}

impl BaseNetlinkSocket {
    pub fn new(family: NetlinkFamily) -> Self {
        BaseNetlinkSocket {
            inner: Mutex::new(NetlinkSocketInner {
                family,
                messages: MessageQueue::new(SOCKET_DEFAULT_SIZE),
                waiters: WaitQueue::default(),
                address: None,
                passcred: false,
                timestamp: false,
            }),
        }
    }

    /// Locks and returns the inner state of the Socket.
    fn lock(&self) -> crate::lock::MutexGuard<'_, NetlinkSocketInner> {
        self.inner.lock()
    }
}

impl SocketOps for BaseNetlinkSocket {
    fn connect(
        &self,
        _socket: &SocketHandle,
        current_task: &CurrentTask,
        peer: SocketPeer,
    ) -> Result<(), Errno> {
        self.lock().connect(current_task, peer)
    }

    fn listen(&self, _socket: &Socket, _backlog: i32, _credentials: ucred) -> Result<(), Errno> {
        error!(EOPNOTSUPP)
    }

    fn accept(&self, _socket: &Socket) -> Result<SocketHandle, Errno> {
        error!(EOPNOTSUPP)
    }

    fn remote_connection(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        _file: FileHandle,
    ) -> Result<(), Errno> {
        error!(EOPNOTSUPP)
    }

    fn bind(
        &self,
        _socket: &Socket,
        current_task: &CurrentTask,
        socket_address: SocketAddress,
    ) -> Result<(), Errno> {
        self.lock().bind(current_task, socket_address)
    }

    fn read(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        data: &mut dyn OutputBuffer,
        _flags: SocketMessageFlags,
    ) -> Result<MessageReadInfo, Errno> {
        let msg = self.lock().read_message();
        match msg {
            Some(message) => {
                // Mark the message as complete and return it.
                let mut nl_msg = nlmsghdr::read_from_prefix(message.data.bytes())
                    .ok_or_else(|| errno!(EINVAL))?;
                nl_msg.nlmsg_type = NLMSG_DONE as u16;
                nl_msg.nlmsg_flags &= NLM_F_MULTI as u16;
                let msg_bytes = nl_msg.as_bytes();
                let bytes_read = data.write(msg_bytes)?;

                let info = MessageReadInfo {
                    bytes_read,
                    message_length: msg_bytes.len(),
                    address: Some(SocketAddress::Netlink(NetlinkAddress::default())),
                    ancillary_data: vec![],
                };
                Ok(info)
            }
            None => Ok(MessageReadInfo::default()),
        }
    }

    fn write(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        data: &mut dyn InputBuffer,
        dest_address: &mut Option<SocketAddress>,
        ancillary_data: &mut Vec<AncillaryData>,
    ) -> Result<usize, Errno> {
        let inner = self.lock();
        let mut local_address = inner.address.clone();
        drop(inner);

        let destination = match dest_address {
            Some(SocketAddress::Netlink(addr)) => addr,
            _ => match &mut local_address {
                Some(addr) => addr,
                _ => {
                    return Ok(data.drain());
                }
            },
        };

        if destination.groups != 0 {
            not_implemented!("?", "BaseNetlinkSockets multicasting is stubbed");
            return Ok(data.drain());
        }

        self.lock().write_to_queue(data, Some(NetlinkAddress::default()), ancillary_data)
    }

    fn wait_async(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        waiter: &Waiter,
        events: FdEvents,
        handler: EventHandler,
    ) -> WaitCanceler {
        self.lock().wait_async(waiter, events, handler)
    }

    fn query_events(&self, _socket: &Socket, _current_task: &CurrentTask) -> FdEvents {
        self.lock().query_events() & FdEvents::POLLIN
    }

    fn shutdown(&self, _socket: &Socket, _how: SocketShutdownFlags) -> Result<(), Errno> {
        not_implemented!("?", "BaseNetlinkSocket::shutdown is stubbed");
        Ok(())
    }

    fn close(&self, _socket: &Socket) {}

    fn getsockname(&self, _socket: &Socket) -> Vec<u8> {
        self.lock().getsockname()
    }

    fn getpeername(&self, _socket: &Socket) -> Result<Vec<u8>, Errno> {
        self.lock().getpeername()
    }

    fn getsockopt(
        &self,
        _socket: &Socket,
        level: u32,
        optname: u32,
        _optlen: u32,
    ) -> Result<Vec<u8>, Errno> {
        self.lock().getsockopt(level, optname)
    }

    fn setsockopt(
        &self,
        _socket: &Socket,
        task: &Task,
        level: u32,
        optname: u32,
        user_opt: UserBuffer,
    ) -> Result<(), Errno> {
        self.lock().setsockopt(task, level, optname, user_opt)
    }
}

/// Socket implementation for the NETLINK_KOBJECT_UEVENT family of netlink sockets.
struct UEventNetlinkSocket {
    kernel: Arc<Kernel>,
    inner: Arc<Mutex<NetlinkSocketInner>>,
    device_listener_key: Mutex<Option<DeviceListenerKey>>,
}

impl UEventNetlinkSocket {
    #[allow(clippy::let_and_return)]
    pub fn new(kernel: &Arc<Kernel>) -> Self {
        let result = Self {
            kernel: Arc::clone(kernel),
            inner: Arc::new(Mutex::new(NetlinkSocketInner {
                family: NetlinkFamily::KobjectUevent,
                messages: MessageQueue::new(SOCKET_DEFAULT_SIZE),
                waiters: WaitQueue::default(),
                address: None,
                passcred: false,
                timestamp: false,
            })),
            device_listener_key: Default::default(),
        };
        #[cfg(any(test, debug_assertions))]
        {
            let _l1 = result.device_listener_key.lock();
            let _l2 = kernel.device_registry.read();
            let _l3 = result.lock();
        }
        result
    }

    /// Locks and returns the inner state of the Socket.
    fn lock(&self) -> crate::lock::MutexGuard<'_, NetlinkSocketInner> {
        self.inner.lock()
    }

    fn register_listener(&self, state: crate::lock::MutexGuard<'_, NetlinkSocketInner>) {
        if state.address.is_none() {
            return;
        }
        std::mem::drop(state);
        let mut key_state = self.device_listener_key.lock();
        if key_state.is_none() {
            *key_state =
                Some(self.kernel.device_registry.write().register_listener(self.inner.clone()));
        }
    }
}

impl SocketOps for UEventNetlinkSocket {
    fn connect(
        &self,
        _socket: &SocketHandle,
        current_task: &CurrentTask,
        peer: SocketPeer,
    ) -> Result<(), Errno> {
        let mut state = self.lock();
        state.connect(current_task, peer)?;
        self.register_listener(state);
        Ok(())
    }

    fn listen(&self, _socket: &Socket, _backlog: i32, _credentials: ucred) -> Result<(), Errno> {
        error!(EOPNOTSUPP)
    }

    fn accept(&self, _socket: &Socket) -> Result<SocketHandle, Errno> {
        error!(EOPNOTSUPP)
    }

    fn remote_connection(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        _file: FileHandle,
    ) -> Result<(), Errno> {
        error!(EOPNOTSUPP)
    }

    fn bind(
        &self,
        _socket: &Socket,
        current_task: &CurrentTask,
        socket_address: SocketAddress,
    ) -> Result<(), Errno> {
        let mut state = self.lock();
        state.bind(current_task, socket_address)?;
        self.register_listener(state);
        Ok(())
    }

    fn read(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        data: &mut dyn OutputBuffer,
        _flags: SocketMessageFlags,
    ) -> Result<MessageReadInfo, Errno> {
        self.lock().read_datagram(data)
    }

    fn write(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        _data: &mut dyn InputBuffer,
        _dest_address: &mut Option<SocketAddress>,
        _ancillary_data: &mut Vec<AncillaryData>,
    ) -> Result<usize, Errno> {
        error!(EOPNOTSUPP)
    }

    fn wait_async(
        &self,
        _socket: &Socket,
        _current_task: &CurrentTask,
        waiter: &Waiter,
        events: FdEvents,
        handler: EventHandler,
    ) -> WaitCanceler {
        self.lock().wait_async(waiter, events, handler)
    }

    fn query_events(&self, _socket: &Socket, _current_task: &CurrentTask) -> FdEvents {
        self.lock().query_events() & FdEvents::POLLIN
    }

    fn shutdown(&self, _socket: &Socket, _how: SocketShutdownFlags) -> Result<(), Errno> {
        not_implemented!("?", "BaseNetlinkSocket::shutdown is stubbed");
        Ok(())
    }

    fn close(&self, _socket: &Socket) {
        let id = self.device_listener_key.lock().take();
        if let Some(id) = id {
            self.kernel.device_registry.write().unregister_listener(&id);
        }
    }

    fn getsockname(&self, _socket: &Socket) -> Vec<u8> {
        self.lock().getsockname()
    }

    fn getpeername(&self, _socket: &Socket) -> Result<Vec<u8>, Errno> {
        self.lock().getpeername()
    }

    fn getsockopt(
        &self,
        _socket: &Socket,
        level: u32,
        optname: u32,
        _optlen: u32,
    ) -> Result<Vec<u8>, Errno> {
        self.lock().getsockopt(level, optname)
    }

    fn setsockopt(
        &self,
        _socket: &Socket,
        task: &Task,
        level: u32,
        optname: u32,
        user_opt: UserBuffer,
    ) -> Result<(), Errno> {
        self.lock().setsockopt(task, level, optname, user_opt)
    }
}

impl DeviceListener for Arc<Mutex<NetlinkSocketInner>> {
    fn on_device_event(&self, seqnum: u64, device: DeviceType) {
        let message = match (device, device.major(), device.minor()) {
            (DeviceType::DEVICE_MAPPER, _, _) => format!(
                "add@/devices/virtual/misc/device-mapper\0ACTION=add\0DEVPATH=/devices/virtual/misc/device-mapper\0SUBSYSTEM=misc\0SYNTH_UUID=0\0MAJOR=10\0MINOR=236\0DEVNAME=mapper/control\0SEQNUM={seqnum}\0"
            ),
            (_, major @ INPUT_MAJOR, minor) => format!(
                "add@/devices/virtual/input/event{minor}\0\
                ACTION=add\0\
                DEVPATH=/devices/virtual/input/event{minor}\0\
                SUBSYSTEM=input\0\
                SYNTH_UUID=0\0\
                MAJOR={major}\0\
                MINOR={minor}\0\
                DEVNAME=input/event{minor}\0\
                SEQNUM={seqnum}",
            ),
            _ => {
                crate::logging::not_implemented!(
                    "?",
                    "Device event for {} is not implemented!",
                    device,
                );
                return;
            }
        };
        let ancillary_data = AncillaryData::Unix(UnixControlData::Credentials(Default::default()));
        let mut ancillary_data = vec![ancillary_data];
        // Ignore write errors
        let _ = self.lock().write_to_queue(
            &mut VecInputBuffer::new(message.as_bytes()),
            Some(NetlinkAddress { pid: 0, groups: 1 }),
            &mut ancillary_data,
        );
    }
}
