// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Defines how TCP state machines are used for TCP sockets.
//!
//! TCP state machine implemented in the parent module aims to only implement
//! RFC 793 which lacks posix semantics.
//!
//! To actually support posix-style sockets:
//! We would need two kinds of active sockets, listeners/connections (or
//! server sockets/client sockets; both are not very accurate terms, the key
//! difference is that the former has only local addresses but the later has
//! remote addresses in addition). [`Connection`]s are backed by a state
//! machine, however the state can be in any state. [`Listener`]s don't have
//! state machines, but they create [`Connection`]s that are backed by
//! [`State::Listen`] an incoming SYN and keep track of whether the connection
//! is established.

pub(crate) mod demux;
mod icmp;
pub(crate) mod isn;

use alloc::{collections::VecDeque, vec::Vec};
use core::{
    convert::Infallible as Never,
    fmt::Debug,
    marker::PhantomData,
    num::{NonZeroU16, NonZeroUsize},
    ops::RangeInclusive,
};

use assert_matches::assert_matches;
use derivative::Derivative;
use log::warn;
use net_types::{
    ip::{
        GenericOverIp, Ip, IpAddr, IpAddress, IpInvariant, IpVersion, IpVersionMarker, Ipv4,
        Ipv4Addr, Ipv6, Ipv6Addr,
    },
    AddrAndZone, SpecifiedAddr, ZonedAddr,
};
use nonzero_ext::nonzero;
use packet::Buf;
use packet_formats::ip::IpProto;
use rand::RngCore;

use crate::{
    algorithm::{PortAlloc, PortAllocImpl},
    context::TimerContext,
    data_structures::{
        id_map::{self, Entry as IdMapEntry, IdMap},
        id_map_collection::IdMapCollectionKey,
        socketmap::{IterShadows as _, SocketMap, Tagged},
    },
    error::{ExistsError, LocalAddressError},
    ip::{
        socket::{
            BufferIpSocketHandler as _, DefaultSendOptions, IpSock, IpSockCreationError,
            IpSocketHandler as _,
        },
        BufferTransportIpContext, IpDeviceId, IpDeviceIdContext, IpExt, TransportIpContext as _,
    },
    socket::{
        address::{ConnAddr, ConnIpAddr, IpPortSpec, ListenerAddr, ListenerIpAddr},
        AddrVec, Bound, BoundSocketMap, IncompatibleError, InsertError, RemoveResult,
        SocketAddrTypeTag, SocketMapAddrStateSpec, SocketMapConflictPolicy, SocketMapStateSpec,
        SocketTypeState as _, SocketTypeStateEntry as _, SocketTypeStateMut as _,
    },
    transport::tcp::{
        buffer::{IntoBuffers, ReceiveBuffer, SendBuffer},
        socket::{demux::tcp_serialize_segment, isn::IsnGenerator},
        state::{CloseError, Closed, Initial, State, Takeable},
        BufferSizes, KeepAlive, DEFAULT_MAXIMUM_SEGMENT_SIZE,
    },
    DeviceId, Instant, SyncCtx,
};

/// Timer ID for TCP connections.
#[derive(Debug, PartialEq, Eq, Clone, Copy, Hash, GenericOverIp)]
#[allow(missing_docs)]
pub enum TimerId {
    V4(MaybeClosedConnectionId<Ipv4>),
    V6(MaybeClosedConnectionId<Ipv6>),
}

impl TimerId {
    fn new<I: Ip>(id: MaybeClosedConnectionId<I>) -> Self {
        I::map_ip(id, TimerId::V4, TimerId::V6)
    }
}

/// Non-sync context for TCP.
///
/// The relationship between buffers defined in the context is as follows:
///
/// The Bindings will receive the `ReturnedBuffers` so that it can: 1. give the
/// application a handle to read/write data; 2. Observe whatever signal required
/// from the application so that it can inform Core. The peer end of returned
/// handle will be held by the state machine inside the netstack. Specialized
/// receive/send buffers will be derived from `ProvidedBuffers` from Bindings.
///
/// +-------------------------------+
/// |       +--------------+        |
/// |       |   returned   |        |
/// |       |    buffers   |        |
/// |       +------+-------+        |
/// |              |     application|
/// +--------------+----------------+
///                |
/// +--------------+----------------+
/// |              |        netstack|
/// |   +---+------+-------+---+    |
/// |   |   |  provided    |   |    |
/// |   | +-+-  buffers   -+-+ |    |
/// |   +-+-+--------------+-+-+    |
/// |     v                  v      |
/// |receive buffer     send buffer |
/// +-------------------------------+
pub trait NonSyncContext: TimerContext<TimerId> {
    /// Receive buffer used by TCP.
    type ReceiveBuffer: ReceiveBuffer;
    /// Send buffer used by TCP.
    type SendBuffer: SendBuffer;
    /// The object that will be returned by the state machine when a passive
    /// open connection becomes established. The bindings can use this object
    /// to read/write bytes from/into the created buffers.
    type ReturnedBuffers: Debug;
    /// The object that is needed from the bindings to initiate a connection,
    /// it is provided by the bindings and will be later used to construct
    /// buffers when the connection becomes established.
    type ProvidedBuffers: Debug + Takeable + IntoBuffers<Self::ReceiveBuffer, Self::SendBuffer>;

    /// A new connection is ready to be accepted on the listener.
    fn on_new_connection<I: Ip>(&mut self, listener: ListenerId<I>);
    /// Creates new buffers and returns the object that Bindings need to
    /// read/write from/into the created buffers.
    fn new_passive_open_buffers(
        buffer_sizes: BufferSizes,
    ) -> (Self::ReceiveBuffer, Self::SendBuffer, Self::ReturnedBuffers);
}

/// Sync context for TCP.
pub(crate) trait SyncContext<I: IpExt, C: NonSyncContext>: IpDeviceIdContext<I> {
    type IpTransportCtx: BufferTransportIpContext<I, C, Buf<Vec<u8>>, DeviceId = Self::DeviceId>;

    /// Calls the function with a `Self::IpTransportCtx`, immutable reference to
    /// an initial sequence number generator and a mutable reference to TCP
    /// socket state.
    fn with_ip_transport_ctx_isn_generator_and_tcp_sockets_mut<
        O,
        F: FnOnce(
            &mut Self::IpTransportCtx,
            &IsnGenerator<C::Instant>,
            &mut Sockets<I, Self::DeviceId, C>,
        ) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O;

    /// Calls the function with a `Self::IpTransportCtx` and a mutable reference
    /// to TCP socket state.
    fn with_ip_transport_ctx_and_tcp_sockets_mut<
        O,
        F: FnOnce(&mut Self::IpTransportCtx, &mut Sockets<I, Self::DeviceId, C>) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O {
        self.with_ip_transport_ctx_isn_generator_and_tcp_sockets_mut(|ctx, _isn, sockets| {
            cb(ctx, sockets)
        })
    }

    /// Calls the function with a mutable reference to TCP socket state.
    fn with_tcp_sockets_mut<O, F: FnOnce(&mut Sockets<I, Self::DeviceId, C>) -> O>(
        &mut self,
        cb: F,
    ) -> O {
        self.with_ip_transport_ctx_isn_generator_and_tcp_sockets_mut(|_ctx, _isn, sockets| {
            cb(sockets)
        })
    }

    /// Calls the function with an immutable reference to TCP socket state.
    fn with_tcp_sockets<O, F: FnOnce(&Sockets<I, Self::DeviceId, C>) -> O>(&self, cb: F) -> O;
}

/// Socket address includes the ip address and the port number.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, GenericOverIp)]
pub struct SocketAddr<A: IpAddress> {
    /// The IP component of the address.
    pub ip: SpecifiedAddr<A>,
    /// The port component of the address.
    pub port: NonZeroU16,
}

impl<A: IpAddress> From<SocketAddr<A>> for IpAddr<SocketAddr<Ipv4Addr>, SocketAddr<Ipv6Addr>> {
    fn from(
        SocketAddr { ip, port }: SocketAddr<A>,
    ) -> IpAddr<SocketAddr<Ipv4Addr>, SocketAddr<Ipv6Addr>> {
        match ip.into() {
            IpAddr::V4(ip) => IpAddr::V4(SocketAddr { ip, port }),
            IpAddr::V6(ip) => IpAddr::V6(SocketAddr { ip, port }),
        }
    }
}

/// An implementation of [`IpTransportContext`] for TCP.
pub(crate) enum TcpIpTransportContext {}

/// Uninstantiatable type for implementing [`SocketMapStateSpec`].
struct TcpSocketSpec<Ip, Device, NonSyncContext>(PhantomData<(Ip, Device, NonSyncContext)>, Never);

impl<I: IpExt, D: IpDeviceId, C: NonSyncContext> SocketMapStateSpec for TcpSocketSpec<I, D, C> {
    type ListenerId = MaybeListenerId<I>;
    type ConnId = MaybeClosedConnectionId<I>;

    type ListenerState = MaybeListener<I, C::ReturnedBuffers>;
    type ConnState =
        Connection<I, D, C::Instant, C::ReceiveBuffer, C::SendBuffer, C::ProvidedBuffers>;

    type ListenerSharingState = ();
    type ConnSharingState = ();
    type AddrVecTag = SocketAddrTypeTag<()>;

    type ListenerAddrState = MaybeListenerId<I>;
    type ConnAddrState = MaybeClosedConnectionId<I>;
}

impl<I: IpExt, D: IpDeviceId, C: NonSyncContext>
    SocketMapConflictPolicy<ListenerAddr<I::Addr, D, NonZeroU16>, (), IpPortSpec<I, D>>
    for TcpSocketSpec<I, D, C>
{
    fn check_for_conflicts(
        (): &(),
        addr: &ListenerAddr<I::Addr, D, NonZeroU16>,
        socketmap: &SocketMap<AddrVec<IpPortSpec<I, D>>, Bound<Self>>,
    ) -> Result<(), InsertError> {
        let addr = AddrVec::Listen(addr.clone());
        // Check if any shadow address is present, specifically, if
        // there is an any-listener with the same port.
        if addr.iter_shadows().any(|a| socketmap.get(&a).is_some()) {
            return Err(InsertError::ShadowAddrExists);
        }

        // Check if shadower exists. Note: Listeners do conflict
        // with existing connections.
        if socketmap.descendant_counts(&addr).len() > 0 {
            return Err(InsertError::ShadowerExists);
        }
        Ok(())
    }
}

impl<I: IpExt, D: IpDeviceId, C: NonSyncContext>
    SocketMapConflictPolicy<ConnAddr<I::Addr, D, NonZeroU16, NonZeroU16>, (), IpPortSpec<I, D>>
    for TcpSocketSpec<I, D, C>
{
    fn check_for_conflicts(
        (): &(),
        _addr: &ConnAddr<I::Addr, D, NonZeroU16, NonZeroU16>,
        _socketmap: &SocketMap<AddrVec<IpPortSpec<I, D>>, Bound<Self>>,
    ) -> Result<(), InsertError> {
        // Connections don't conflict with existing listeners. If there
        // are connections with the same local and remote address, it
        // will be decided by the socket sharing options.
        Ok(())
    }
}

impl<I: Ip, D, LI> Tagged<ListenerAddr<I::Addr, D, LI>> for MaybeListenerId<I> {
    type Tag = SocketAddrTypeTag<()>;
    fn tag(&self, address: &ListenerAddr<I::Addr, D, LI>) -> Self::Tag {
        (address, ()).into()
    }
}

impl<I: Ip, D, LI, RI> Tagged<ConnAddr<I::Addr, D, LI, RI>> for MaybeClosedConnectionId<I> {
    type Tag = SocketAddrTypeTag<()>;
    fn tag(&self, address: &ConnAddr<I::Addr, D, LI, RI>) -> Self::Tag {
        (address, ()).into()
    }
}

#[derive(Debug, Derivative, Clone)]
#[derivative(Default(bound = ""))]
#[cfg_attr(test, derive(PartialEq))]
struct Unbound<D> {
    bound_device: Option<D>,
    buffer_sizes: BufferSizes,
    keep_alive: KeepAlive,
}

/// Holds all the TCP socket states.
pub(crate) struct Sockets<I: IpExt, D: IpDeviceId, C: NonSyncContext> {
    port_alloc: PortAlloc<BoundSocketMap<IpPortSpec<I, D>, TcpSocketSpec<I, D, C>>>,
    inactive: IdMap<Unbound<D>>,
    socketmap: BoundSocketMap<IpPortSpec<I, D>, TcpSocketSpec<I, D, C>>,
}

impl<I: IpExt, D: IpDeviceId, C: NonSyncContext> PortAllocImpl
    for BoundSocketMap<IpPortSpec<I, D>, TcpSocketSpec<I, D, C>>
{
    const TABLE_SIZE: NonZeroUsize = nonzero!(20usize);
    const EPHEMERAL_RANGE: RangeInclusive<u16> = 49152..=65535;
    type Id = I::Addr;

    fn is_port_available(&self, addr: &I::Addr, port: u16) -> bool {
        // We can safely unwrap here, because the ports received in
        // `is_port_available` are guaranteed to be in `EPHEMERAL_RANGE`.
        let port = NonZeroU16::new(port).unwrap();
        let root_addr = AddrVec::from(ListenerAddr {
            ip: ListenerIpAddr { addr: SpecifiedAddr::new(*addr), identifier: port },
            device: None,
        });

        // A port is free if there are no sockets currently using it, and if
        // there are no sockets that are shadowing it.

        root_addr.iter_shadows().chain(core::iter::once(root_addr.clone())).all(|a| match &a {
            AddrVec::Listen(l) => self.listeners().get_by_addr(&l).is_none(),
            AddrVec::Conn(_c) => {
                unreachable!("no connection shall be included in an iteration from a listener")
            }
        }) && self.get_shadower_counts(&root_addr) == 0
    }
}

impl<I: IpExt, D: IpDeviceId, C: NonSyncContext> Sockets<I, D, C> {
    fn get_listener_by_id_mut(
        &mut self,
        id: ListenerId<I>,
    ) -> Option<&mut Listener<I, C::ReturnedBuffers>> {
        self.socketmap.listeners_mut().get_by_id_mut(&MaybeListenerId::from(id)).map(
            |(maybe_listener, _sharing, _local_addr)| match maybe_listener {
                MaybeListener::Bound(_) => {
                    unreachable!("contract violated: ListenerId points to an inactive entry")
                }
                MaybeListener::Listener(l) => l,
            },
        )
    }

    pub(crate) fn new(rng: &mut impl RngCore) -> Self {
        Self {
            port_alloc: PortAlloc::new(rng),
            inactive: IdMap::new(),
            socketmap: Default::default(),
        }
    }
}

/// A link stored in each passively created connections that points back to the
/// parent listener.
///
/// The link is an [`Acceptor::Pending`] iff the acceptee is in the pending
/// state; The link is an [`Acceptor::Ready`] iff the acceptee is ready and has
/// an established connection.
#[derive(Debug)]
enum Acceptor<I: Ip> {
    Pending(ListenerId<I>),
    Ready(ListenerId<I>),
}

/// The Connection state.
///
/// Note: the `state` is not guaranteed to be [`State::Established`]. The
/// connection can be in any state as long as both the local and remote socket
/// addresses are specified.
#[derive(Debug)]
struct Connection<I: IpExt, D: IpDeviceId, II: Instant, R: ReceiveBuffer, S: SendBuffer, ActiveOpen>
{
    acceptor: Option<Acceptor<I>>,
    state: State<II, R, S, ActiveOpen>,
    ip_sock: IpSock<I, D, DefaultSendOptions>,
    /// The user has indicated that this connection will never be used again, we
    /// keep the connection in the socketmap to perform the shutdown but it will
    /// be auto removed once the state reaches Closed.
    defunct: bool,
    keep_alive: KeepAlive,
}

/// The Listener state.
///
/// State for sockets that participate in the passive open. Contrary to
/// [`Connection`], only the local address is specified.
#[derive(Debug)]
#[cfg_attr(test, derive(PartialEq, Eq))]
struct Listener<I: Ip, PassiveOpen> {
    backlog: NonZeroUsize,
    ready: VecDeque<(ConnectionId<I>, PassiveOpen)>,
    pending: Vec<ConnectionId<I>>,
    buffer_sizes: BufferSizes,
    keep_alive: KeepAlive,
    // If ip sockets can be half-specified so that only the local address
    // is needed, we can construct an ip socket here to be reused.
}

impl<I: Ip, PassiveOpen> Listener<I, PassiveOpen> {
    fn new(backlog: NonZeroUsize, buffer_sizes: BufferSizes, keep_alive: KeepAlive) -> Self {
        Self { backlog, ready: VecDeque::new(), pending: Vec::new(), buffer_sizes, keep_alive }
    }
}

#[derive(Clone, Debug)]
#[cfg_attr(test, derive(Eq, PartialEq))]
struct BoundState {
    buffer_sizes: BufferSizes,
    keep_alive: KeepAlive,
}

/// Represents either a bound socket or a listener socket.
#[derive(Debug)]
#[cfg_attr(test, derive(PartialEq))]
enum MaybeListener<I: Ip, PassiveOpen> {
    Bound(BoundState),
    Listener(Listener<I, PassiveOpen>),
}

impl<I: Ip, PassiveOpen: core::fmt::Debug> MaybeListener<I, PassiveOpen> {
    fn maybe_shutdown(&mut self) -> Option<Listener<I, PassiveOpen>> {
        let (buffer_sizes, keep_alive) = match self {
            Self::Bound(_) => return None,
            Self::Listener(Listener {
                backlog: _,
                ready: _,
                pending: _,
                buffer_sizes,
                keep_alive,
            }) => (buffer_sizes.clone(), keep_alive.clone()),
        };
        assert_matches!(
            core::mem::replace(self, Self::Bound(BoundState { buffer_sizes, keep_alive })),
            Self::Listener(listener) => Some(listener)
        )
    }
}

// TODO(https://fxbug.dev/38297): The following IDs are all `Clone + Copy`,
// which makes it possible for the client to keep them for longer than they are
// valid and cause panics. Find a way to make it harder to misuse.
/// The ID to an unbound socket.
#[derive(Clone, Copy, Debug, PartialEq, Eq, GenericOverIp)]
pub struct UnboundId<I: Ip>(usize, IpVersionMarker<I>);
/// The ID to a bound socket.
#[derive(Clone, Copy, Debug, PartialEq, Eq, GenericOverIp)]
pub struct BoundId<I: Ip>(usize, IpVersionMarker<I>);
/// The ID to a listener socket.
#[derive(Clone, Copy, Debug, PartialEq, Eq, GenericOverIp)]
pub struct ListenerId<I: Ip>(usize, IpVersionMarker<I>);
/// The ID to a connection socket that might have been defunct.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, GenericOverIp)]
pub struct MaybeClosedConnectionId<I: Ip>(usize, IpVersionMarker<I>);
/// The ID to a connection socket that has never been closed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, GenericOverIp)]
pub struct ConnectionId<I: Ip>(usize, IpVersionMarker<I>);

impl<I: Ip> IdMapCollectionKey for ListenerId<I> {
    const VARIANT_COUNT: usize = 2;

    fn get_variant(&self) -> usize {
        match I::VERSION {
            IpVersion::V4 => 0,
            IpVersion::V6 => 1,
        }
    }

    fn get_id(&self) -> usize {
        (*self).into()
    }
}

impl<I: IpExt> ConnectionId<I> {
    fn get_from_socketmap<D: IpDeviceId, C: NonSyncContext>(
        self,
        socketmap: &BoundSocketMap<IpPortSpec<I, D>, TcpSocketSpec<I, D, C>>,
    ) -> (
        &Connection<I, D, C::Instant, C::ReceiveBuffer, C::SendBuffer, C::ProvidedBuffers>,
        (),
        &ConnAddr<I::Addr, D, NonZeroU16, NonZeroU16>,
    ) {
        let (conn, (), addr) =
            socketmap.conns().get_by_id(&self.into()).expect("invalid ConnectionId: not found");
        assert!(!conn.defunct, "invalid ConnectionId: already defunct");
        (conn, (), addr)
    }

    fn get_from_socketmap_mut<D: IpDeviceId, C: NonSyncContext>(
        self,
        socketmap: &mut BoundSocketMap<IpPortSpec<I, D>, TcpSocketSpec<I, D, C>>,
    ) -> (
        &mut Connection<I, D, C::Instant, C::ReceiveBuffer, C::SendBuffer, C::ProvidedBuffers>,
        (),
        &ConnAddr<I::Addr, D, NonZeroU16, NonZeroU16>,
    ) {
        let (conn, (), addr) = socketmap
            .conns_mut()
            .get_by_id_mut(&self.into())
            .expect("invalid ConnectionId: not found");
        assert!(!conn.defunct, "invalid ConnectionId: already defunct");
        (conn, (), addr)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, GenericOverIp)]
pub(crate) struct MaybeListenerId<I: Ip>(usize, IpVersionMarker<I>);

#[derive(Clone, Copy, Debug, PartialEq, Eq, GenericOverIp)]
/// Possible socket IDs for TCP.
pub enum SocketId<I: Ip> {
    /// Unbound socket.
    Unbound(UnboundId<I>),
    /// Bound socket.
    Bound(BoundId<I>),
    /// Listener socket.
    Listener(ListenerId<I>),
    /// Connection socket.
    Connection(ConnectionId<I>),
}

impl<I: Ip> From<ConnectionId<I>> for SocketId<I> {
    fn from(connection: ConnectionId<I>) -> Self {
        Self::Connection(connection)
    }
}

impl<I: Ip> From<ListenerId<I>> for SocketId<I> {
    fn from(listener: ListenerId<I>) -> Self {
        Self::Listener(listener)
    }
}

impl<I: Ip> From<UnboundId<I>> for SocketId<I> {
    fn from(unbound: UnboundId<I>) -> Self {
        Self::Unbound(unbound)
    }
}

impl<I: Ip> From<BoundId<I>> for SocketId<I> {
    fn from(bound: BoundId<I>) -> Self {
        Self::Bound(bound)
    }
}

impl<I: Ip> SocketMapAddrStateSpec for MaybeListenerId<I> {
    type Id = MaybeListenerId<I>;
    type SharingState = ();

    fn new((): &(), id: MaybeListenerId<I>) -> Self {
        id
    }

    fn remove_by_id(&mut self, id: MaybeListenerId<I>) -> RemoveResult {
        assert_eq!(self, &id);
        RemoveResult::IsLast
    }

    fn try_get_dest<'a, 'b>(
        &'b mut self,
        (): &'a (),
    ) -> Result<&'b mut Vec<MaybeListenerId<I>>, IncompatibleError> {
        // TODO(https://fxbug.dev/101596): Support sharing for TCP sockets.
        Err(IncompatibleError)
    }

    fn could_insert(
        &self,
        _new_sharing_state: &Self::SharingState,
    ) -> Result<(), IncompatibleError> {
        // TODO(https://fxbug.dev/101596): Support sharing for TCP sockets.
        Err(IncompatibleError)
    }
}

impl<I: Ip> SocketMapAddrStateSpec for MaybeClosedConnectionId<I> {
    type Id = MaybeClosedConnectionId<I>;
    type SharingState = ();

    fn new((): &(), id: MaybeClosedConnectionId<I>) -> Self {
        id
    }

    fn remove_by_id(&mut self, id: MaybeClosedConnectionId<I>) -> RemoveResult {
        assert_eq!(self, &id);
        RemoveResult::IsLast
    }

    fn try_get_dest<'a, 'b>(
        &'b mut self,
        (): &'a (),
    ) -> Result<&'b mut Vec<MaybeClosedConnectionId<I>>, IncompatibleError> {
        Err(IncompatibleError)
    }

    fn could_insert(
        &self,
        _new_sharing_state: &Self::SharingState,
    ) -> Result<(), IncompatibleError> {
        Err(IncompatibleError)
    }
}

pub(crate) trait SocketHandler<I: Ip, C: NonSyncContext>: IpDeviceIdContext<I> {
    fn create_socket(&mut self, ctx: &mut C) -> UnboundId<I>;

    fn bind(
        &mut self,
        ctx: &mut C,
        id: UnboundId<I>,
        local_ip: I::Addr,
        port: Option<NonZeroU16>,
    ) -> Result<BoundId<I>, LocalAddressError>;

    fn listen(&mut self, _ctx: &mut C, id: BoundId<I>, backlog: NonZeroUsize) -> ListenerId<I>;

    fn accept(
        &mut self,
        _ctx: &mut C,
        id: ListenerId<I>,
    ) -> Result<(ConnectionId<I>, SocketAddr<I::Addr>, C::ReturnedBuffers), AcceptError>;

    fn connect_bound(
        &mut self,
        ctx: &mut C,
        id: BoundId<I>,
        remote: SocketAddr<I::Addr>,
        netstack_buffers: C::ProvidedBuffers,
    ) -> Result<ConnectionId<I>, ConnectError>;

    fn connect_unbound(
        &mut self,
        ctx: &mut C,
        id: UnboundId<I>,
        remote: SocketAddr<I::Addr>,
        netstack_buffers: C::ProvidedBuffers,
    ) -> Result<ConnectionId<I>, ConnectError>;

    fn shutdown_conn(&mut self, ctx: &mut C, id: ConnectionId<I>) -> Result<(), NoConnection>;
    fn close_conn(&mut self, ctx: &mut C, id: ConnectionId<I>);
    fn remove_unbound(&mut self, id: UnboundId<I>);
    fn remove_bound(&mut self, id: BoundId<I>);
    fn shutdown_listener(&mut self, ctx: &mut C, id: ListenerId<I>) -> BoundId<I>;

    fn get_unbound_info(&self, id: UnboundId<I>) -> UnboundInfo<Self::DeviceId>;
    fn get_bound_info(&self, id: BoundId<I>) -> BoundInfo<I::Addr, Self::DeviceId>;
    fn get_listener_info(&self, id: ListenerId<I>) -> BoundInfo<I::Addr, Self::DeviceId>;
    fn get_connection_info(&self, id: ConnectionId<I>) -> ConnectionInfo<I::Addr, Self::DeviceId>;
    fn do_send(&mut self, ctx: &mut C, conn_id: MaybeClosedConnectionId<I>);
    fn handle_timer(&mut self, ctx: &mut C, conn_id: MaybeClosedConnectionId<I>);

    fn set_unbound_device(&mut self, ctx: &mut C, id: UnboundId<I>, device: Option<Self::DeviceId>);
    fn set_bound_device(
        &mut self,
        ctx: &mut C,
        id: impl Into<MaybeListenerId<I>>,
        device: Option<Self::DeviceId>,
    ) -> Result<(), SetDeviceError>;
    fn set_connection_device(
        &mut self,
        ctx: &mut C,
        id: ConnectionId<I>,
        device: Option<Self::DeviceId>,
    ) -> Result<(), SetDeviceError>;
    fn with_keep_alive_mut<R, F: FnOnce(&mut KeepAlive) -> R, Id: Into<SocketId<I>>>(
        &mut self,
        ctx: &mut C,
        id: Id,
        f: F,
    ) -> R;
    fn with_keep_alive<R, F: FnOnce(&KeepAlive) -> R, Id: Into<SocketId<I>>>(
        &self,
        id: Id,
        f: F,
    ) -> R;

    fn set_send_buffer_size<Id: Into<SocketId<I>>>(&mut self, ctx: &mut C, id: Id, size: usize);
    fn send_buffer_size<Id: Into<SocketId<I>>>(&self, ctx: &mut C, id: Id) -> usize;
}

impl<I: IpExt, C: NonSyncContext, SC: SyncContext<I, C>> SocketHandler<I, C> for SC {
    fn create_socket(&mut self, _ctx: &mut C) -> UnboundId<I> {
        let unbound = Unbound::default();
        UnboundId(
            self.with_tcp_sockets_mut(move |sockets| sockets.inactive.push(unbound)),
            IpVersionMarker::default(),
        )
    }

    fn bind(
        &mut self,
        _ctx: &mut C,
        id: UnboundId<I>,
        local_ip: I::Addr,
        port: Option<NonZeroU16>,
    ) -> Result<BoundId<I>, LocalAddressError> {
        // TODO(https://fxbug.dev/104300): Check if local_ip is a unicast address.
        self.with_ip_transport_ctx_and_tcp_sockets_mut(
            |ip_transport_ctx, Sockets { port_alloc, inactive, socketmap }| {
                let port = match port {
                    None => match port_alloc.try_alloc(&local_ip, &socketmap) {
                        Some(port) => {
                            NonZeroU16::new(port).expect("ephemeral ports must be non-zero")
                        }
                        None => return Err(LocalAddressError::FailedToAllocateLocalPort),
                    },
                    Some(port) => port,
                };

                let local_ip = SpecifiedAddr::new(local_ip);
                if let Some(ip) = local_ip {
                    if ip_transport_ctx.get_devices_with_assigned_addr(ip).next().is_none() {
                        return Err(LocalAddressError::AddressMismatch);
                    }
                }

                let inactive_entry = match inactive.entry(id.into()) {
                    IdMapEntry::Vacant(_) => panic!("invalid unbound ID"),
                    IdMapEntry::Occupied(o) => o,
                };

                let Unbound { bound_device, buffer_sizes, keep_alive } = &inactive_entry.get();
                let bound_state = BoundState {
                    buffer_sizes: buffer_sizes.clone(),
                    keep_alive: keep_alive.clone(),
                };
                let bound = socketmap
                    .listeners_mut()
                    .try_insert(
                        ListenerAddr {
                            ip: ListenerIpAddr { addr: local_ip, identifier: port },
                            device: bound_device.clone(),
                        },
                        MaybeListener::Bound(bound_state),
                        // TODO(https://fxbug.dev/101596): Support sharing for TCP sockets.
                        (),
                    )
                    .map(|entry| {
                        let MaybeListenerId(x, marker) = entry.id();
                        BoundId(x, marker)
                    })
                    .map_err(|_: (InsertError, MaybeListener<_, _>, ())| {
                        LocalAddressError::AddressInUse
                    })?;
                let _: Unbound<_> = inactive_entry.remove();
                Ok(bound)
            },
        )
    }

    fn listen(&mut self, _ctx: &mut C, id: BoundId<I>, backlog: NonZeroUsize) -> ListenerId<I> {
        let id = MaybeListenerId::from(id);
        self.with_tcp_sockets_mut(|sockets| {
            let (listener, _, _): (_, &(), &ListenerAddr<_, _, _>) =
                sockets.socketmap.listeners_mut().get_by_id_mut(&id).expect("invalid listener id");

            match listener {
                MaybeListener::Bound(BoundState { buffer_sizes, keep_alive }) => {
                    *listener = MaybeListener::Listener(Listener::new(
                        backlog,
                        buffer_sizes.clone(),
                        keep_alive.clone(),
                    ));
                }
                MaybeListener::Listener(_) => {
                    unreachable!("invalid bound id that points to a listener entry")
                }
            }
        });

        ListenerId(id.into(), IpVersionMarker::default())
    }

    fn accept(
        &mut self,
        _ctx: &mut C,
        id: ListenerId<I>,
    ) -> Result<(ConnectionId<I>, SocketAddr<I::Addr>, C::ReturnedBuffers), AcceptError> {
        self.with_tcp_sockets_mut(|sockets| {
            let listener = sockets.get_listener_by_id_mut(id).expect("invalid listener id");
            let (conn_id, client_buffers) =
                listener.ready.pop_front().ok_or(AcceptError::WouldBlock)?;
            let (conn, (), conn_addr) = conn_id.get_from_socketmap_mut(&mut sockets.socketmap);
            conn.acceptor = None;
            let (remote_ip, remote_port) = conn_addr.ip.remote;
            Ok((conn_id, SocketAddr { ip: remote_ip, port: remote_port }, client_buffers))
        })
    }

    fn connect_bound(
        &mut self,
        ctx: &mut C,
        id: BoundId<I>,
        remote: SocketAddr<I::Addr>,
        netstack_buffers: C::ProvidedBuffers,
    ) -> Result<ConnectionId<I>, ConnectError> {
        let bound_id = MaybeListenerId::from(id);
        self.with_ip_transport_ctx_isn_generator_and_tcp_sockets_mut(
            |ip_transport_ctx, isn, sockets| {
                let (bound, (), bound_addr) =
                    sockets.socketmap.listeners().get_by_id(&bound_id).expect("invalid socket id");
                let bound = assert_matches!(bound, MaybeListener::Bound(b) => b);
                let BoundState { buffer_sizes, keep_alive } = bound.clone();
                let ListenerAddr { ip, device } = bound_addr;
                let ListenerIpAddr { addr: local_ip, identifier: local_port } = *ip;

                let ip_sock = ip_transport_ctx
                    .new_ip_socket(
                        ctx,
                        device.as_ref(),
                        local_ip,
                        remote.ip,
                        IpProto::Tcp.into(),
                        DefaultSendOptions,
                    )
                    .map_err(|(err, DefaultSendOptions {})| match err {
                        IpSockCreationError::Route(_) => ConnectError::NoRoute,
                    })?;

                let device = device.clone();
                let conn_id = connect_inner(
                    isn,
                    &mut sockets.socketmap,
                    ip_transport_ctx,
                    ctx,
                    ip_sock,
                    device,
                    local_port,
                    remote.port,
                    netstack_buffers,
                    buffer_sizes.clone(),
                    keep_alive.clone(),
                )?;
                let _: Option<_> = sockets.socketmap.listeners_mut().remove(&bound_id);
                Ok(conn_id)
            },
        )
    }

    fn connect_unbound(
        &mut self,
        ctx: &mut C,
        id: UnboundId<I>,
        remote: SocketAddr<I::Addr>,
        netstack_buffers: C::ProvidedBuffers,
    ) -> Result<ConnectionId<I>, ConnectError> {
        self.with_ip_transport_ctx_isn_generator_and_tcp_sockets_mut(
            |ip_transport_ctx, isn, sockets| {
                let inactive = match sockets.inactive.entry(id.into()) {
                    id_map::Entry::Vacant(_) => panic!("invalid unbound ID {:?}", id),
                    id_map::Entry::Occupied(o) => o,
                };
                let Unbound { bound_device, buffer_sizes: _, keep_alive: _ } = inactive.get();

                let ip_sock = ip_transport_ctx
                    .new_ip_socket(
                        ctx,
                        bound_device.as_ref(),
                        None,
                        remote.ip,
                        IpProto::Tcp.into(),
                        DefaultSendOptions,
                    )
                    .map_err(|(err, DefaultSendOptions)| match err {
                        IpSockCreationError::Route(_) => ConnectError::NoRoute,
                    })?;

                let local_port = match sockets
                    .port_alloc
                    .try_alloc(&*ip_sock.local_ip(), &sockets.socketmap)
                {
                    Some(port) => NonZeroU16::new(port).expect("ephemeral ports must be non-zero"),
                    None => return Err(ConnectError::NoPort),
                };

                let bound_device = bound_device.clone();
                let entry = sockets.inactive.entry(id.into());
                let inactive = match entry {
                    IdMapEntry::Vacant(_v) => panic!("invalid unbound ID"),
                    IdMapEntry::Occupied(o) => o,
                };
                let Unbound { buffer_sizes, bound_device: _, keep_alive } = inactive.get();

                let conn_id = connect_inner(
                    isn,
                    &mut sockets.socketmap,
                    ip_transport_ctx,
                    ctx,
                    ip_sock,
                    bound_device,
                    local_port,
                    remote.port,
                    netstack_buffers,
                    buffer_sizes.clone(),
                    keep_alive.clone(),
                )?;
                let _: Unbound<_> = inactive.remove();
                Ok(conn_id)
            },
        )
    }

    fn shutdown_conn(&mut self, ctx: &mut C, id: ConnectionId<I>) -> Result<(), NoConnection> {
        self.with_ip_transport_ctx_and_tcp_sockets_mut(|ip_transport_ctx, sockets| {
            let (conn, (), addr) = id.get_from_socketmap_mut(&mut sockets.socketmap);
            match conn.state.close() {
                Ok(()) => Ok(do_send_inner(id.into(), conn, addr, ip_transport_ctx, ctx)),
                Err(CloseError::NoConnection) => Err(NoConnection),
                Err(CloseError::Closing) => Ok(()),
            }
        })
    }

    fn close_conn(&mut self, ctx: &mut C, id: ConnectionId<I>) {
        self.with_ip_transport_ctx_and_tcp_sockets_mut(|ip_transport_ctx, sockets| {
            let (conn, (), addr) = id.get_from_socketmap_mut(&mut sockets.socketmap);
            conn.defunct = true;
            let already_closed = match conn.state.close() {
                Err(CloseError::NoConnection) => true,
                Err(CloseError::Closing) => return,
                Ok(()) => matches!(conn.state, State::Closed(_)),
            };
            if already_closed {
                assert_matches!(sockets.socketmap.conns_mut().remove(&id.into()), Some(_));
                let _: Option<_> = ctx.cancel_timer(TimerId::new::<I>(id.into()));
                return;
            }
            do_send_inner(id.into(), conn, addr, ip_transport_ctx, ctx)
        })
    }

    fn remove_unbound(&mut self, id: UnboundId<I>) {
        self.with_tcp_sockets_mut(|Sockets { socketmap: _, inactive, port_alloc: _ }| {
            assert_matches!(inactive.remove(id.into()), Some(_));
        });
    }

    fn remove_bound(&mut self, id: BoundId<I>) {
        self.with_tcp_sockets_mut(|Sockets { socketmap, inactive: _, port_alloc: _ }| {
            assert_matches!(socketmap.listeners_mut().remove(&id.into()), Some(_));
        });
    }

    fn shutdown_listener(&mut self, ctx: &mut C, id: ListenerId<I>) -> BoundId<I> {
        self.with_ip_transport_ctx_and_tcp_sockets_mut(
            |ip_transport_ctx, Sockets { socketmap, inactive: _, port_alloc: _ }| {
                let (maybe_listener, (), _addr) = socketmap
                    .listeners_mut()
                    .get_by_id_mut(&id.into())
                    .expect("invalid listener ID");
                let Listener { backlog: _, pending, ready, buffer_sizes: _, keep_alive: _ } =
                    maybe_listener.maybe_shutdown().expect("must be a listener");
                for conn_id in pending.into_iter().chain(
                    ready
                        .into_iter()
                        .map(|(conn_id, _passive_open): (_, C::ReturnedBuffers)| conn_id),
                ) {
                    let _: Option<C::Instant> = ctx.cancel_timer(TimerId::new::<I>(conn_id.into()));
                    let (mut conn, (), conn_addr) =
                        socketmap.conns_mut().remove(&conn_id.into()).unwrap();
                    if let Some(reset) = conn.state.abort() {
                        let ConnAddr { ip, device: _ } = conn_addr;
                        let ser = tcp_serialize_segment(reset, ip);
                        ip_transport_ctx
                            .send_ip_packet(ctx, &conn.ip_sock, ser, None)
                            .unwrap_or_else(|(body, err)| {
                                log::debug!(
                                    "failed to reset connection to {:?}, body: {:?}, err: {:?}",
                                    ip,
                                    body,
                                    err
                                )
                            });
                    }
                }
                BoundId(id.into(), IpVersionMarker::default())
            },
        )
    }

    fn set_unbound_device(
        &mut self,
        _ctx: &mut C,
        id: UnboundId<I>,
        device: Option<Self::DeviceId>,
    ) {
        self.with_tcp_sockets_mut(|sockets| {
            let Sockets { inactive, port_alloc: _, socketmap: _ } = sockets;
            let Unbound { bound_device, buffer_sizes: _, keep_alive: _ } =
                inactive.get_mut(id.into()).expect("invalid unbound socket ID");
            *bound_device = device;
        })
    }

    fn set_bound_device(
        &mut self,
        _ctx: &mut C,
        id: impl Into<MaybeListenerId<I>>,
        device: Option<Self::DeviceId>,
    ) -> Result<(), SetDeviceError> {
        self.with_tcp_sockets_mut(|sockets| {
            let Sockets { socketmap, inactive: _, port_alloc: _ } = sockets;
            let entry = socketmap.listeners_mut().entry(&id.into()).expect("invalid ID");
            let (_, _, addr): &(MaybeListener<_, _>, (), _) = entry.get();
            let ListenerAddr { device: old_device, ip: ip_addr } = addr;
            let ListenerIpAddr { identifier: _, addr: ip } = ip_addr;
            if let Some(ip) = ip {
                // TODO(https://fxbug.dev/115524): add an assert for
                // `must_have_zone = true` implying `device != None` once that's
                // enforced as an invariant for bind & connect.
                if crate::socket::must_have_zone(ip) && &device != old_device {
                    return Err(SetDeviceError::ZoneChange);
                }
            }
            let ip = *ip_addr;
            match entry.try_update_addr(ListenerAddr { device, ip }) {
                Ok(_entry) => Ok(()),
                Err((ExistsError, _entry)) => Err(SetDeviceError::Conflict),
            }
        })
    }

    fn set_connection_device(
        &mut self,
        ctx: &mut C,
        id: ConnectionId<I>,
        new_device: Option<Self::DeviceId>,
    ) -> Result<(), SetDeviceError> {
        self.with_ip_transport_ctx_and_tcp_sockets_mut(
            |ip_transport_ctx, Sockets { socketmap, inactive: _, port_alloc: _ }| {
                let entry = socketmap.conns_mut().entry(&id.into()).expect("invalid conn ID");
                let (_, _, addr): &(Connection<_, _, _, _, _, _>, (), _) = entry.get();
                let ConnAddr {
                    device,
                    ip: ConnIpAddr { local: (local_ip, _), remote: (remote_ip, _) },
                } = addr;

                // TODO(https://fxbug.dev/115524): add an assert for
                // `must_have_zone = true` implying `device != None` once that's
                // enforced as an invariant for bind & connect.
                if crate::socket::must_have_zone(local_ip)
                    || crate::socket::must_have_zone(remote_ip)
                {
                    if &new_device != device {
                        return Err(SetDeviceError::ZoneChange);
                    }
                }

                let new_socket = ip_transport_ctx
                    .new_ip_socket(
                        ctx,
                        new_device.as_ref(),
                        Some(*local_ip),
                        *remote_ip,
                        IpProto::Tcp.into(),
                        Default::default(),
                    )
                    .map_err(|_: (IpSockCreationError, DefaultSendOptions)| {
                        SetDeviceError::Unroutable
                    })?;

                let addr = addr.clone();
                let mut new_entry =
                    match entry.try_update_addr(ConnAddr { device: new_device.clone(), ..addr }) {
                        Ok(entry) => Ok(entry),
                        Err((ExistsError, _entry)) => Err(SetDeviceError::Conflict),
                    }?;
                let Connection { ip_sock, acceptor: _, state: _, defunct: _, keep_alive: _ } =
                    new_entry.get_state_mut();
                *ip_sock = new_socket;
                Ok(())
            },
        )
    }

    fn get_unbound_info(&self, id: UnboundId<I>) -> UnboundInfo<SC::DeviceId> {
        self.with_tcp_sockets(|sockets| {
            let Sockets { socketmap: _, inactive, port_alloc: _ } = sockets;
            inactive.get(id.into()).expect("invalid unbound ID").into()
        })
    }

    fn get_bound_info(&self, id: BoundId<I>) -> BoundInfo<I::Addr, SC::DeviceId> {
        self.with_tcp_sockets(|sockets| {
            let (bound, (), bound_addr) =
                sockets.socketmap.listeners().get_by_id(&id.into()).expect("invalid bound ID");
            assert_matches!(bound, MaybeListener::Bound(_));
            bound_addr.clone()
        })
        .into()
    }

    fn get_listener_info(&self, id: ListenerId<I>) -> BoundInfo<I::Addr, SC::DeviceId> {
        self.with_tcp_sockets(|sockets| {
            let (listener, (), addr) =
                sockets.socketmap.listeners().get_by_id(&id.into()).expect("invalid listener ID");
            assert_matches!(listener, MaybeListener::Listener(_));
            addr.clone()
        })
        .into()
    }

    fn get_connection_info(&self, id: ConnectionId<I>) -> ConnectionInfo<I::Addr, SC::DeviceId> {
        self.with_tcp_sockets(|sockets| {
            let (_conn, (), addr) = id.get_from_socketmap(&sockets.socketmap);
            addr.clone()
        })
        .into()
    }

    fn do_send(&mut self, ctx: &mut C, conn_id: MaybeClosedConnectionId<I>) {
        self.with_ip_transport_ctx_and_tcp_sockets_mut(|ip_transport_ctx, sockets| {
            if let Some((conn, (), addr)) = sockets.socketmap.conns_mut().get_by_id_mut(&conn_id) {
                do_send_inner(conn_id, conn, addr, ip_transport_ctx, ctx);
            }
        })
    }

    fn handle_timer(&mut self, ctx: &mut C, conn_id: MaybeClosedConnectionId<I>) {
        self.with_ip_transport_ctx_and_tcp_sockets_mut(|ip_transport_ctx, sockets| {
            let (conn, (), addr) = sockets
                .socketmap
                .conns_mut()
                .get_by_id_mut(&conn_id)
                .expect("invalid connection ID");
            do_send_inner(conn_id, conn, addr, ip_transport_ctx, ctx);
            if conn.defunct && matches!(conn.state, State::Closed(_)) {
                assert_matches!(sockets.socketmap.conns_mut().remove(&conn_id), Some(_));
                let _: Option<_> = ctx.cancel_timer(TimerId::new::<I>(conn_id));
            }
        })
    }

    fn with_keep_alive_mut<R, F: FnOnce(&mut KeepAlive) -> R, Id: Into<SocketId<I>>>(
        &mut self,
        ctx: &mut C,
        id: Id,
        f: F,
    ) -> R {
        self.with_ip_transport_ctx_and_tcp_sockets_mut(|ip_transport_ctx, sockets| {
            let maybe_listener_id: MaybeListenerId<I> = match id.into() {
                SocketId::Unbound(unbound_id) => {
                    return f(&mut sockets
                        .inactive
                        .get_mut(unbound_id.into())
                        .expect("invalid unbound ID")
                        .keep_alive);
                }
                SocketId::Bound(bound_id) => bound_id.into(),
                SocketId::Listener(listener_id) => listener_id.into(),
                SocketId::Connection(conn_id) => {
                    let (conn, (), addr) = conn_id.get_from_socketmap_mut(&mut sockets.socketmap);
                    let old = conn.keep_alive;
                    let result = f(&mut conn.keep_alive);
                    if old != conn.keep_alive {
                        do_send_inner(conn_id.into(), conn, addr, ip_transport_ctx, ctx);
                    }
                    return result;
                }
            };
            let (maybe_listener, (), _bound_addr) = sockets
                .socketmap
                .listeners_mut()
                .get_by_id_mut(&maybe_listener_id)
                .expect("invalid ID");
            match maybe_listener {
                MaybeListener::Bound(bound) => f(&mut bound.keep_alive),
                MaybeListener::Listener(listener) => f(&mut listener.keep_alive),
            }
        })
    }

    fn with_keep_alive<R, F: FnOnce(&KeepAlive) -> R, Id: Into<SocketId<I>>>(
        &self,
        id: Id,
        f: F,
    ) -> R {
        self.with_tcp_sockets(|sockets| {
            let maybe_listener_id: MaybeListenerId<I> = match id.into() {
                SocketId::Unbound(unbound_id) => {
                    return f(&sockets
                        .inactive
                        .get(unbound_id.into())
                        .expect("invalid unbound ID")
                        .keep_alive);
                }
                SocketId::Bound(bound_id) => bound_id.into(),
                SocketId::Listener(listener_id) => listener_id.into(),
                SocketId::Connection(conn_id) => {
                    let (conn, (), _addr) = conn_id.get_from_socketmap(&sockets.socketmap);
                    return f(&conn.keep_alive);
                }
            };
            let (maybe_listener, (), _bound_addr) =
                sockets.socketmap.listeners().get_by_id(&maybe_listener_id).expect("invalid ID");
            match maybe_listener {
                MaybeListener::Bound(bound) => f(&bound.keep_alive),
                MaybeListener::Listener(listener) => f(&listener.keep_alive),
            }
        })
    }

    fn set_send_buffer_size<Id: Into<SocketId<I>>>(&mut self, _ctx: &mut C, id: Id, size: usize) {
        self.with_tcp_sockets_mut(|sockets| {
            let Sockets { port_alloc: _, inactive, socketmap } = sockets;
            let get_listener = match id.into() {
                SocketId::Unbound(id) => {
                    let Unbound { bound_device: _, buffer_sizes, keep_alive: _ } =
                        inactive.get_mut(id.into()).expect("invalid unbound ID");
                    let BufferSizes { send } = buffer_sizes;
                    return *send = size;
                }
                SocketId::Connection(id) => {
                    let (conn, _, _): (_, &(), &ConnAddr<_, _, _, _>) =
                        socketmap.conns_mut().get_by_id_mut(&id.into()).expect("invalid ID");
                    let Connection { acceptor: _, state, ip_sock: _, defunct: _, keep_alive: _ } =
                        conn;
                    return state.set_send_buffer_size(size);
                }
                SocketId::Bound(id) => socketmap.listeners_mut().get_by_id_mut(&id.into()),
                SocketId::Listener(id) => socketmap.listeners_mut().get_by_id_mut(&id.into()),
            };

            let (state, _, _): (_, &(), &ListenerAddr<_, _, _>) =
                get_listener.expect("invalid socket ID");
            let BufferSizes { send } = match state {
                MaybeListener::Bound(BoundState { buffer_sizes, keep_alive: _ }) => buffer_sizes,
                MaybeListener::Listener(Listener {
                    backlog: _,
                    ready: _,
                    pending: _,
                    buffer_sizes,
                    keep_alive: _,
                }) => buffer_sizes,
            };
            *send = size;
        })
    }

    fn send_buffer_size<Id: Into<SocketId<I>>>(&self, _ctx: &mut C, id: Id) -> usize {
        self.with_tcp_sockets(|sockets| {
            let Sockets { port_alloc: _, inactive, socketmap } = sockets;
            let get_listener = match id.into() {
                SocketId::Unbound(id) => {
                    let Unbound { bound_device: _, buffer_sizes, keep_alive: _ } =
                        inactive.get(id.into()).expect("invalid unbound ID");
                    let BufferSizes { send } = buffer_sizes;
                    return *send;
                }
                SocketId::Connection(id) => {
                    let (conn, _, _): &(_, (), ConnAddr<_, _, _, _>) =
                        socketmap.conns().get_by_id(&id.into()).expect("invalid ID");
                    let Connection { acceptor: _, state, ip_sock: _, defunct: _, keep_alive: _ } =
                        conn;
                    return state.send_buffer_size();
                }
                SocketId::Bound(id) => socketmap.listeners().get_by_id(&id.into()),
                SocketId::Listener(id) => socketmap.listeners().get_by_id(&id.into()),
            };

            let (state, _, _): &(_, (), ListenerAddr<_, _, _>) =
                get_listener.expect("invalid socket ID");
            let BufferSizes { send } = match state {
                MaybeListener::Bound(BoundState { buffer_sizes, keep_alive: _ }) => buffer_sizes,
                MaybeListener::Listener(Listener {
                    backlog: _,
                    ready: _,
                    pending: _,
                    buffer_sizes,
                    keep_alive: _,
                }) => buffer_sizes,
            };
            *send
        })
    }
}

fn do_send_inner<I, SC, C>(
    conn_id: MaybeClosedConnectionId<I>,
    conn: &mut Connection<
        I,
        SC::DeviceId,
        C::Instant,
        C::ReceiveBuffer,
        C::SendBuffer,
        C::ProvidedBuffers,
    >,
    addr: &ConnAddr<I::Addr, SC::DeviceId, NonZeroU16, NonZeroU16>,
    ip_transport_ctx: &mut SC,
    ctx: &mut C,
) where
    I: IpExt,
    C: NonSyncContext,
    SC: BufferTransportIpContext<I, C, Buf<Vec<u8>>>,
{
    while let Some(seg) =
        conn.state.poll_send(DEFAULT_MAXIMUM_SEGMENT_SIZE, ctx.now(), &conn.keep_alive)
    {
        let ser = tcp_serialize_segment(seg, addr.ip.clone());
        ip_transport_ctx.send_ip_packet(ctx, &conn.ip_sock, ser, None).unwrap_or_else(
            |(body, err)| {
                // Currently there are a few call sites to `do_send_inner` and they
                // don't really care about the error, with Rust's strict
                // `unused_result` lint, not returning an error that no one
                // would care makes the code less cumbersome to write. So We do
                // not return the error to caller but just log it instead. If
                // we find a case where the caller is interested in the error,
                // then we can always come back and change this.
                log::debug!(
                    "failed to send an ip packet on {:?}, body: {:?}, err: {:?}",
                    conn_id,
                    body,
                    err
                )
            },
        );
    }

    if let Some(instant) = conn.state.poll_send_at() {
        let _: Option<_> = ctx.schedule_timer_instant(instant, TimerId::new::<I>(conn_id));
    }
}

/// Creates a new socket in unbound state.
pub fn create_socket<I, C>(mut sync_ctx: &SyncCtx<C>, ctx: &mut C) -> UnboundId<I>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        IpInvariant((&mut sync_ctx, ctx)),
        |IpInvariant((sync_ctx, ctx))| SocketHandler::create_socket(sync_ctx, ctx),
        |IpInvariant((sync_ctx, ctx))| SocketHandler::create_socket(sync_ctx, ctx),
    )
}

/// Sets the device to which a socket should be bound.
///
/// Sets the device on which the socket (once bound or connected) should send
/// and receive packets, or `None` to clear the bound device.
pub fn set_unbound_device<I, C>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: UnboundId<I>,
    device: Option<DeviceId<C::Instant>>,
) where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, device)), id),
        |(IpInvariant((sync_ctx, ctx, device)), id)| {
            SocketHandler::set_unbound_device(sync_ctx, ctx, id, device)
        },
        |(IpInvariant((sync_ctx, ctx, device)), id)| {
            SocketHandler::set_unbound_device(sync_ctx, ctx, id, device)
        },
    )
}

/// Error returned when failing to set the bound device for a socket.
#[derive(Debug, GenericOverIp)]
pub enum SetDeviceError {
    /// The socket would conflict with another socket.
    Conflict,
    /// The socket would become unroutable.
    Unroutable,
    /// The socket has an address with a different zone.
    ZoneChange,
}

/// Sets the device on which a listening socket will receive new connections.
///
/// Sets the device on which the given socket will listen for new incoming
/// connections. Passing `None` clears the bound device.
pub fn set_listener_device<I, C>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: ListenerId<I>,
    device: Option<DeviceId<C::Instant>>,
) -> Result<(), SetDeviceError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, device)), id),
        |(IpInvariant((sync_ctx, ctx, device)), id)| {
            SocketHandler::set_bound_device(sync_ctx, ctx, id, device)
        },
        |(IpInvariant((sync_ctx, ctx, device)), id)| {
            SocketHandler::set_bound_device(sync_ctx, ctx, id, device)
        },
    )
}

/// Sets the device on which a bound socket will eventually receive traffic.
///
/// Sets the device on which the given socket will either (if turned into a
/// listening socket) accept connections or (if connected to a remote address)
/// or send and receive packets. Passing `None` clears the bound device.
pub fn set_bound_device<I, C>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: BoundId<I>,
    device: Option<DeviceId<C::Instant>>,
) -> Result<(), SetDeviceError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, device)), id),
        |(IpInvariant((sync_ctx, ctx, device)), id)| {
            SocketHandler::set_bound_device(sync_ctx, ctx, id, device)
        },
        |(IpInvariant((sync_ctx, ctx, device)), id)| {
            SocketHandler::set_bound_device(sync_ctx, ctx, id, device)
        },
    )
}

/// Sets the device on which a connected socket sends and receives traffic.
///
/// Sets the device on which the connected socket sends and receives packets.
/// Passing `None` clears the bound device.
pub fn set_connection_device<I, C>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: ConnectionId<I>,
    device: Option<DeviceId<C::Instant>>,
) -> Result<(), SetDeviceError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, device)), id),
        |(IpInvariant((sync_ctx, ctx, device)), id)| {
            SocketHandler::set_connection_device(sync_ctx, ctx, id, device)
        },
        |(IpInvariant((sync_ctx, ctx, device)), id)| {
            SocketHandler::set_connection_device(sync_ctx, ctx, id, device)
        },
    )
}

/// Binds an unbound socket to a local socket address.
pub fn bind<I, C>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: UnboundId<I>,
    local_ip: I::Addr,
    port: Option<NonZeroU16>,
) -> Result<BoundId<I>, LocalAddressError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, port)), id, local_ip),
        |(IpInvariant((sync_ctx, ctx, port)), id, local_ip)| {
            SocketHandler::bind(sync_ctx, ctx, id, local_ip, port)
        },
        |(IpInvariant((sync_ctx, ctx, port)), id, local_ip)| {
            SocketHandler::bind(sync_ctx, ctx, id, local_ip, port)
        },
    )
}

/// Listens on an already bound socket.
pub fn listen<I, C>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: BoundId<I>,
    backlog: NonZeroUsize,
) -> ListenerId<I>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, backlog)), id),
        |(IpInvariant((sync_ctx, ctx, backlog)), id)| {
            SocketHandler::listen(sync_ctx, ctx, id, backlog)
        },
        |(IpInvariant((sync_ctx, ctx, backlog)), id)| {
            SocketHandler::listen(sync_ctx, ctx, id, backlog)
        },
    )
}

/// Possible errors for accept operation.
#[derive(Debug, GenericOverIp)]
pub enum AcceptError {
    /// There is no established socket currently.
    WouldBlock,
}

/// Possible error for calling `shutdown` on a not-yet connected socket.
#[derive(Debug, GenericOverIp)]
pub struct NoConnection;

/// Accepts an established socket from the queue of a listener socket.
pub fn accept<I: Ip, C>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: ListenerId<I>,
) -> Result<(ConnectionId<I>, SocketAddr<I::Addr>, C::ReturnedBuffers), AcceptError>
where
    C: crate::NonSyncContext,
{
    I::map_ip::<_, Result<_, _>>(
        (IpInvariant((&mut sync_ctx, ctx)), id),
        |(IpInvariant((sync_ctx, ctx)), id)| {
            SocketHandler::accept(sync_ctx, ctx, id).map(|(a, b, c)| (a, b, IpInvariant(c)))
        },
        |(IpInvariant((sync_ctx, ctx)), id)| {
            SocketHandler::accept(sync_ctx, ctx, id).map(|(a, b, c)| (a, b, IpInvariant(c)))
        },
    )
    .map(|(a, b, IpInvariant(c))| (a, b, c))
}

/// Possible errors when connecting a socket.
#[derive(Debug, GenericOverIp)]
pub enum ConnectError {
    /// Cannot allocate a local port for the connection.
    NoPort,
    /// Cannot find a route to the remote host.
    NoRoute,
}

/// Connects a socket that has been bound locally.
///
/// When the method returns, the connection is not guaranteed to be established.
/// It is up to the caller (Bindings) to determine when the connection has been
/// established. Bindings are free to use anything available on the platform to
/// check, for instance, signals.
pub fn connect_bound<I, C>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: BoundId<I>,
    remote: SocketAddr<I::Addr>,
    netstack_buffers: C::ProvidedBuffers,
) -> Result<ConnectionId<I>, ConnectError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, netstack_buffers)), id, remote),
        |(IpInvariant((sync_ctx, ctx, netstack_buffers)), id, remote)| {
            SocketHandler::connect_bound(sync_ctx, ctx, id, remote, netstack_buffers)
        },
        |(IpInvariant((sync_ctx, ctx, netstack_buffers)), id, remote)| {
            SocketHandler::connect_bound(sync_ctx, ctx, id, remote, netstack_buffers)
        },
    )
}

/// Connects a socket that is in unbound state.
///
/// When the method returns, the connection is not guaranteed to be established.
/// It is up to the caller (Bindings) to determine when the connection has been
/// established. Bindings are free to use anything available on the platform to
/// check, for instance, signals.
pub fn connect_unbound<I, C>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: UnboundId<I>,
    remote: SocketAddr<I::Addr>,
    netstack_buffers: C::ProvidedBuffers,
) -> Result<ConnectionId<I>, ConnectError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, netstack_buffers)), id, remote),
        |(IpInvariant((sync_ctx, ctx, netstack_buffers)), id, remote)| {
            SocketHandler::connect_unbound(sync_ctx, ctx, id, remote, netstack_buffers)
        },
        |(IpInvariant((sync_ctx, ctx, netstack_buffers)), id, remote)| {
            SocketHandler::connect_unbound(sync_ctx, ctx, id, remote, netstack_buffers)
        },
    )
}

fn connect_inner<I, SC, C>(
    isn: &IsnGenerator<C::Instant>,
    socketmap: &mut BoundSocketMap<IpPortSpec<I, SC::DeviceId>, TcpSocketSpec<I, SC::DeviceId, C>>,
    ip_transport_ctx: &mut SC,
    ctx: &mut C,
    ip_sock: IpSock<I, SC::DeviceId, DefaultSendOptions>,
    device: Option<SC::DeviceId>,
    local_port: NonZeroU16,
    remote_port: NonZeroU16,
    netstack_buffers: C::ProvidedBuffers,
    buffer_sizes: BufferSizes,
    keep_alive: KeepAlive,
) -> Result<ConnectionId<I>, ConnectError>
where
    I: IpExt,
    C: NonSyncContext,
    SC: BufferTransportIpContext<I, C, Buf<Vec<u8>>>,
{
    let isn = isn.generate(
        ctx.now(),
        SocketAddr { ip: ip_sock.local_ip().clone(), port: local_port },
        SocketAddr { ip: ip_sock.remote_ip().clone(), port: remote_port },
    );
    let conn_addr = ConnAddr {
        ip: ConnIpAddr {
            local: (ip_sock.local_ip().clone(), local_port),
            remote: (ip_sock.remote_ip().clone(), remote_port),
        },
        device,
    };
    let now = ctx.now();
    let (syn_sent, syn) = Closed::<Initial>::connect(isn, now, netstack_buffers, buffer_sizes);
    let state = State::SynSent(syn_sent);
    let poll_send_at = state.poll_send_at().expect("no retrans timer");
    let conn_id = socketmap
        .conns_mut()
        .try_insert(
            conn_addr.clone(),
            Connection {
                acceptor: None,
                state,
                ip_sock: ip_sock.clone(),
                defunct: false,
                keep_alive,
            },
            // TODO(https://fxbug.dev/101596): Support sharing for TCP sockets.
            (),
        )
        .expect("failed to insert connection")
        .id();

    ip_transport_ctx
        .send_ip_packet(ctx, &ip_sock, tcp_serialize_segment(syn, conn_addr.ip), None)
        .map_err(|(body, err)| {
            warn!("tcp: failed to send ip packet {:?}: {:?}", body, err);
            assert_matches!(socketmap.conns_mut().remove(&conn_id), Some(_));
            ConnectError::NoRoute
        })?;
    assert_eq!(ctx.schedule_timer_instant(poll_send_at, TimerId::new::<I>(conn_id)), None);
    // This conversion Ok because `conn_id` is newly created; No one should
    // have called close on it.
    let MaybeClosedConnectionId(id, marker) = conn_id;
    Ok(ConnectionId(id, marker))
}

/// Closes the connection. The user has promised that they will not use `id`
/// again, we can reclaim the connection after the connection becomes `Closed`.
pub fn close_conn<I, C>(mut sync_ctx: &SyncCtx<C>, ctx: &mut C, id: ConnectionId<I>)
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx)), id),
        |(IpInvariant((sync_ctx, ctx)), id)| SocketHandler::close_conn(sync_ctx, ctx, id),
        |(IpInvariant((sync_ctx, ctx)), id)| SocketHandler::close_conn(sync_ctx, ctx, id),
    )
}

/// Shuts down the write-half of the connection. Calling this function signals
/// the other side of the connection that we will not be sending anything over
/// the connection; The connection will still stay in the socketmap even after
/// reaching `Closed` state. The user needs to call `close_conn` in order to
/// remove it.
pub fn shutdown_conn<I, C>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: ConnectionId<I>,
) -> Result<(), NoConnection>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx)), id),
        |(IpInvariant((sync_ctx, ctx)), id)| SocketHandler::shutdown_conn(sync_ctx, ctx, id),
        |(IpInvariant((sync_ctx, ctx)), id)| SocketHandler::shutdown_conn(sync_ctx, ctx, id),
    )
}

/// Removes an unbound socket.
pub fn remove_unbound<I, C>(mut sync_ctx: &SyncCtx<C>, id: UnboundId<I>)
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::remove_unbound(sync_ctx, id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::remove_unbound(sync_ctx, id),
    )
}

/// Removes a bound socket.
pub fn remove_bound<I, C>(mut sync_ctx: &SyncCtx<C>, id: BoundId<I>)
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::remove_bound(sync_ctx, id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::remove_bound(sync_ctx, id),
    )
}

/// Shuts down a listener socket.
///
/// The socket remains in the socket map as a bound socket, taking the port
/// that the socket has been using. Returns the id of that bound socket.
pub fn shutdown_listener<I, C>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: ListenerId<I>,
) -> BoundId<I>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx)), id),
        |(IpInvariant((sync_ctx, ctx)), id)| SocketHandler::shutdown_listener(sync_ctx, ctx, id),
        |(IpInvariant((sync_ctx, ctx)), id)| SocketHandler::shutdown_listener(sync_ctx, ctx, id),
    )
}

/// Information about an unbound socket.
#[derive(Clone, Debug, Eq, PartialEq, GenericOverIp)]
pub struct UnboundInfo<D> {
    /// The device the socket will be bound to.
    pub device: Option<D>,
}

/// Information about a bound socket's address.
#[derive(Clone, Debug, Eq, PartialEq, GenericOverIp)]
pub struct BoundInfo<A: IpAddress, D> {
    /// The IP address the socket is bound to, or `None` for all local IPs.
    pub addr: Option<ZonedAddr<A, D>>,
    /// The port number the socket is bound to.
    pub port: NonZeroU16,
    /// The device the socket is bound to.
    pub device: Option<D>,
}

/// Information about a connected socket's address.
#[derive(Clone, Debug, Eq, PartialEq, GenericOverIp)]
pub struct ConnectionInfo<A: IpAddress, D> {
    /// The local address the socket is bound to.
    pub local_addr: (ZonedAddr<A, D>, NonZeroU16),
    /// The remote address the socket is connected to.
    pub remote_addr: (ZonedAddr<A, D>, NonZeroU16),
    /// The device the socket is bound to.
    pub device: Option<D>,
}

impl<D: Clone> From<&'_ Unbound<D>> for UnboundInfo<D> {
    fn from(unbound: &Unbound<D>) -> Self {
        let Unbound { bound_device: device, buffer_sizes: _, keep_alive: _ } = unbound;
        Self { device: device.clone() }
    }
}

fn maybe_zoned<A: IpAddress, D: Clone>(
    ip: SpecifiedAddr<A>,
    device: &Option<D>,
) -> ZonedAddr<A, D> {
    device
        .as_ref()
        .and_then(|device| {
            AddrAndZone::new(*ip, device).map(|az| ZonedAddr::Zoned(az.map_zone(Clone::clone)))
        })
        .unwrap_or(ZonedAddr::Unzoned(ip))
}

impl<A: IpAddress, D: Clone> From<ListenerAddr<A, D, NonZeroU16>> for BoundInfo<A, D> {
    fn from(addr: ListenerAddr<A, D, NonZeroU16>) -> Self {
        let ListenerAddr { ip: ListenerIpAddr { addr, identifier }, device } = addr;
        let addr = addr.map(|ip| maybe_zoned(ip, &device));
        BoundInfo { addr, port: identifier, device }
    }
}

impl<A: IpAddress, D: Clone> From<ConnAddr<A, D, NonZeroU16, NonZeroU16>> for ConnectionInfo<A, D> {
    fn from(addr: ConnAddr<A, D, NonZeroU16, NonZeroU16>) -> Self {
        let ConnAddr { ip: ConnIpAddr { local, remote }, device } = addr;
        let convert = |(ip, port): (SpecifiedAddr<A>, NonZeroU16)| (maybe_zoned(ip, &device), port);
        Self { local_addr: convert(local), remote_addr: convert(remote), device }
    }
}

/// Get information for unbound TCP socket.
pub fn get_unbound_info<I: Ip, C: crate::NonSyncContext>(
    mut sync_ctx: &SyncCtx<C>,
    id: UnboundId<I>,
) -> UnboundInfo<DeviceId<C::Instant>> {
    I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv4, _>::get_unbound_info(sync_ctx, id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv6, _>::get_unbound_info(sync_ctx, id),
    )
}

/// Get information for bound TCP socket.
pub fn get_bound_info<I: Ip, C: crate::NonSyncContext>(
    mut sync_ctx: &SyncCtx<C>,
    id: BoundId<I>,
) -> BoundInfo<I::Addr, DeviceId<C::Instant>> {
    I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv4, _>::get_bound_info(sync_ctx, id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv6, _>::get_bound_info(sync_ctx, id),
    )
}

/// Get information for listener TCP socket.
pub fn get_listener_info<I: Ip, C: crate::NonSyncContext>(
    mut sync_ctx: &SyncCtx<C>,
    id: ListenerId<I>,
) -> BoundInfo<I::Addr, DeviceId<C::Instant>> {
    I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv4, _>::get_listener_info(sync_ctx, id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv6, _>::get_listener_info(sync_ctx, id),
    )
}

/// Get information for connection TCP socket.
pub fn get_connection_info<I: Ip, C: crate::NonSyncContext>(
    mut sync_ctx: &SyncCtx<C>,
    id: ConnectionId<I>,
) -> ConnectionInfo<I::Addr, DeviceId<C::Instant>> {
    I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv4, _>::get_connection_info(sync_ctx, id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv6, _>::get_connection_info(sync_ctx, id),
    )
}

/// Access keep-alive options mutably for a TCP socket.
pub fn with_keep_alive_mut<
    I: Ip,
    C: crate::NonSyncContext,
    R,
    F: FnOnce(&mut KeepAlive) -> R,
    Id: Into<SocketId<I>>,
>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: Id,
    f: F,
) -> R {
    let IpInvariant(r) = I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, f)), id.into()),
        |(IpInvariant((sync_ctx, ctx, f)), id)| {
            IpInvariant(SocketHandler::with_keep_alive_mut(sync_ctx, ctx, id, f))
        },
        |(IpInvariant((sync_ctx, ctx, f)), id)| {
            IpInvariant(SocketHandler::with_keep_alive_mut(sync_ctx, ctx, id, f))
        },
    );
    r
}

/// Access keep-alive options immutably for a TCP socket.
pub fn with_keep_alive<
    I: Ip,
    C: crate::NonSyncContext,
    R,
    F: FnOnce(&KeepAlive) -> R,
    Id: Into<SocketId<I>>,
>(
    sync_ctx: &SyncCtx<C>,
    id: Id,
    f: F,
) -> R {
    let IpInvariant(r) = I::map_ip(
        (IpInvariant((&sync_ctx, f)), id.into()),
        |(IpInvariant((sync_ctx, f)), id)| {
            IpInvariant(SocketHandler::with_keep_alive(sync_ctx, id, f))
        },
        |(IpInvariant((sync_ctx, f)), id)| {
            IpInvariant(SocketHandler::with_keep_alive(sync_ctx, id, f))
        },
    );
    r
}

/// Set the size of the send buffer for this socket and future derived sockets.
pub fn set_send_buffer_size<I: Ip, C: crate::NonSyncContext, Id: Into<SocketId<I>>>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: Id,
    size: usize,
) {
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, size)), id.into()),
        |(IpInvariant((sync_ctx, ctx, size)), id)| {
            SocketHandler::set_send_buffer_size(sync_ctx, ctx, id, size)
        },
        |(IpInvariant((sync_ctx, ctx, size)), id)| {
            SocketHandler::set_send_buffer_size(sync_ctx, ctx, id, size)
        },
    )
}

/// Get the size of the send buffer for this socket and future derived sockets.
pub fn send_buffer_size<I: Ip, C: crate::NonSyncContext, Id: Into<SocketId<I>>>(
    mut sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: Id,
) -> usize {
    let IpInvariant(size) = I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx)), id.into()),
        |(IpInvariant((sync_ctx, ctx)), id)| {
            IpInvariant(SocketHandler::send_buffer_size(sync_ctx, ctx, id))
        },
        |(IpInvariant((sync_ctx, ctx)), id)| {
            IpInvariant(SocketHandler::send_buffer_size(sync_ctx, ctx, id))
        },
    );
    size
}

/// Call this function whenever a socket can push out more data. That means either:
///
/// - A retransmission timer fires.
/// - An ack received from peer so that our send window is enlarged.
/// - The user puts data into the buffer and we are notified.
pub fn do_send<I, C>(mut sync_ctx: &SyncCtx<C>, ctx: &mut C, conn_id: MaybeClosedConnectionId<I>)
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx)), conn_id),
        |(IpInvariant((sync_ctx, ctx)), conn_id)| SocketHandler::do_send(sync_ctx, ctx, conn_id),
        |(IpInvariant((sync_ctx, ctx)), conn_id)| SocketHandler::do_send(sync_ctx, ctx, conn_id),
    )
}

pub(crate) fn handle_timer<SC, C>(sync_ctx: &mut SC, ctx: &mut C, timer_id: TimerId)
where
    C: NonSyncContext,
    SC: SyncContext<Ipv4, C> + SyncContext<Ipv6, C>,
{
    match timer_id {
        TimerId::V4(conn_id) => SocketHandler::<Ipv4, _>::handle_timer(sync_ctx, ctx, conn_id),
        TimerId::V6(conn_id) => SocketHandler::<Ipv6, _>::handle_timer(sync_ctx, ctx, conn_id),
    }
}

impl<I: Ip> From<ListenerId<I>> for MaybeListenerId<I> {
    fn from(ListenerId(x, marker): ListenerId<I>) -> Self {
        Self(x, marker)
    }
}

impl<I: Ip> From<BoundId<I>> for MaybeListenerId<I> {
    fn from(BoundId(x, marker): BoundId<I>) -> Self {
        Self(x, marker)
    }
}

impl<I: Ip> From<usize> for MaybeListenerId<I> {
    fn from(x: usize) -> Self {
        Self(x, IpVersionMarker::default())
    }
}

impl<I: Ip> Into<usize> for MaybeListenerId<I> {
    fn into(self) -> usize {
        let Self(x, _marker) = self;
        x
    }
}

impl<I: Ip> From<usize> for MaybeClosedConnectionId<I> {
    fn from(x: usize) -> Self {
        Self(x, IpVersionMarker::default())
    }
}

impl<I: Ip> Into<usize> for ListenerId<I> {
    fn into(self) -> usize {
        let Self(x, _marker) = self;
        x
    }
}

impl<I: Ip> Into<usize> for MaybeClosedConnectionId<I> {
    fn into(self) -> usize {
        let Self(x, _marker) = self;
        x
    }
}

impl<I: Ip> From<ConnectionId<I>> for MaybeClosedConnectionId<I> {
    fn from(ConnectionId(id, marker): ConnectionId<I>) -> Self {
        Self(id, marker)
    }
}

impl<I: Ip> Into<usize> for UnboundId<I> {
    fn into(self) -> usize {
        let Self(x, _marker) = self;
        x
    }
}

#[cfg(test)]
mod tests {
    use core::{cell::RefCell, fmt::Debug};
    use fakealloc::{rc::Rc, vec};

    use const_unwrap::const_unwrap_option;
    use ip_test_macro::ip_test;
    use net_declare::net_ip_v6;
    use net_types::{
        ip::{AddrSubnet, Ip, Ipv4, Ipv6, Ipv6SourceAddr},
        LinkLocalAddr,
    };
    use packet::ParseBuffer as _;
    use packet_formats::tcp::{TcpParseArgs, TcpSegment};
    use rand::Rng as _;
    use test_case::test_case;

    use crate::{
        context::testutil::{
            FakeCtxWithSyncCtx, FakeFrameCtx, FakeInstant, FakeNetwork, FakeNetworkContext,
            FakeNonSyncCtx, FakeSyncCtx, InstantAndData, PendingFrameData, StepResult,
            WrappedFakeSyncCtx,
        },
        ip::{
            device::state::{
                AddrConfig, AddressState, IpDeviceState, IpDeviceStateIpExt, Ipv6AddressEntry,
            },
            socket::testutil::{FakeBufferIpSocketCtx, FakeDeviceConfig, FakeIpSocketCtx},
            testutil::{FakeDeviceId, MultipleDevicesId},
            BufferIpTransportContext as _, SendIpPacketMeta,
        },
        testutil::{new_rng, run_with_many_seeds, set_logger_for_test, FakeCryptoRng, TestIpExt},
        transport::tcp::{
            buffer::{Buffer, RingBuffer, SendPayload},
            segment::Payload,
            UserError,
        },
    };

    use super::*;

    trait TcpTestIpExt: IpExt + TestIpExt + IpDeviceStateIpExt {
        fn recv_src_addr(addr: Self::Addr) -> Self::RecvSrcAddr;

        fn new_device_state(addr: Self::Addr, prefix: u8) -> IpDeviceState<FakeInstant, Self>;
    }

    type FakeBufferIpTransportCtx<I, D> = FakeSyncCtx<
        FakeBufferIpSocketCtx<I, D>,
        SendIpPacketMeta<I, D, SpecifiedAddr<<I as Ip>::Addr>>,
        D,
    >;

    struct FakeTcpState<I: TcpTestIpExt, D: IpDeviceId> {
        isn_generator: IsnGenerator<FakeInstant>,
        sockets: Sockets<I, D, TcpNonSyncCtx>,
    }

    impl<I: TcpTestIpExt, D: IpDeviceId> Default for FakeTcpState<I, D> {
        fn default() -> Self {
            Self {
                isn_generator: Default::default(),
                sockets: Sockets {
                    inactive: IdMap::new(),
                    socketmap: BoundSocketMap::default(),
                    port_alloc: PortAlloc::new(&mut FakeCryptoRng::new_xorshift(0)),
                },
            }
        }
    }

    type TcpSyncCtx<I, D> = WrappedFakeSyncCtx<
        FakeTcpState<I, D>,
        FakeBufferIpSocketCtx<I, D>,
        SendIpPacketMeta<I, D, SpecifiedAddr<<I as Ip>::Addr>>,
        D,
    >;

    type TcpCtx<I, D> = FakeCtxWithSyncCtx<TcpSyncCtx<I, D>, TimerId, (), ()>;

    impl<I: TcpTestIpExt, D: IpDeviceId>
        AsMut<FakeFrameCtx<SendIpPacketMeta<I, D, SpecifiedAddr<<I as Ip>::Addr>>>>
        for TcpCtx<I, D>
    {
        fn as_mut(
            &mut self,
        ) -> &mut FakeFrameCtx<SendIpPacketMeta<I, D, SpecifiedAddr<<I as Ip>::Addr>>> {
            self.sync_ctx.inner.as_mut()
        }
    }

    impl<I: TcpTestIpExt, D: IpDeviceId> FakeNetworkContext for TcpCtx<I, D> {
        type TimerId = TimerId;
        type SendMeta = SendIpPacketMeta<I, FakeDeviceId, SpecifiedAddr<<I as Ip>::Addr>>;
    }

    type TcpNonSyncCtx = FakeNonSyncCtx<TimerId, (), ()>;

    impl Buffer for Rc<RefCell<RingBuffer>> {
        fn len(&self) -> usize {
            self.borrow().len()
        }

        fn cap(&self) -> usize {
            self.borrow().cap()
        }
    }

    impl ReceiveBuffer for Rc<RefCell<RingBuffer>> {
        fn write_at<P: Payload>(&mut self, offset: usize, data: &P) -> usize {
            self.borrow_mut().write_at(offset, data)
        }

        fn make_readable(&mut self, count: usize) {
            self.borrow_mut().make_readable(count)
        }
    }

    #[derive(Debug, Default)]
    pub struct TestSendBuffer {
        fake_stream: Rc<RefCell<Vec<u8>>>,
        ring: RingBuffer,
    }
    impl TestSendBuffer {
        fn new(fake_stream: Rc<RefCell<Vec<u8>>>, ring: RingBuffer) -> TestSendBuffer {
            Self { fake_stream, ring }
        }
    }

    impl Buffer for TestSendBuffer {
        fn len(&self) -> usize {
            let Self { fake_stream, ring } = self;
            ring.len() + fake_stream.borrow().len()
        }

        fn cap(&self) -> usize {
            let Self { fake_stream, ring } = self;
            ring.cap() + fake_stream.borrow().capacity()
        }
    }

    impl SendBuffer for TestSendBuffer {
        fn request_capacity(&mut self, size: usize) {
            let Self { fake_stream: _, ring } = self;
            ring.request_capacity(size)
        }

        fn mark_read(&mut self, count: usize) {
            let Self { fake_stream: _, ring } = self;
            ring.mark_read(count)
        }

        fn peek_with<'a, F, R>(&'a mut self, offset: usize, f: F) -> R
        where
            F: FnOnce(SendPayload<'a>) -> R,
        {
            let Self { fake_stream, ring } = self;
            if !fake_stream.borrow().is_empty() {
                // Pull from the fake stream into the ring if there is capacity.
                let len = (ring.cap() - ring.len()).min(fake_stream.borrow().len());
                let rest = fake_stream.borrow_mut().split_off(len);
                let first = fake_stream.replace(rest);
                assert_eq!(ring.enqueue_data(&first[..]), len);
            }
            ring.peek_with(offset, f)
        }
    }

    #[derive(Clone, Debug, Default, Eq, PartialEq)]
    pub(crate) struct ClientBuffers {
        receive: Rc<RefCell<RingBuffer>>,
        send: Rc<RefCell<Vec<u8>>>,
    }

    impl ClientBuffers {
        fn new(buffer_sizes: BufferSizes) -> Self {
            let BufferSizes { send } = buffer_sizes;
            Self {
                receive: Default::default(),
                send: Rc::new(RefCell::new(Vec::with_capacity(send))),
            }
        }
    }

    impl NonSyncContext for TcpNonSyncCtx {
        type ReceiveBuffer = Rc<RefCell<RingBuffer>>;
        type SendBuffer = TestSendBuffer;
        type ReturnedBuffers = ClientBuffers;
        type ProvidedBuffers = WriteBackClientBuffers;

        fn on_new_connection<I: Ip>(&mut self, _listener: ListenerId<I>) {}
        fn new_passive_open_buffers(
            buffer_sizes: BufferSizes,
        ) -> (Self::ReceiveBuffer, Self::SendBuffer, Self::ReturnedBuffers) {
            let client = ClientBuffers::new(buffer_sizes);
            (
                Rc::clone(&client.receive),
                TestSendBuffer::new(Rc::clone(&client.send), RingBuffer::default()),
                client,
            )
        }
    }

    type WriteBackClientBuffers = Rc<RefCell<Option<ClientBuffers>>>;

    impl IntoBuffers<Rc<RefCell<RingBuffer>>, TestSendBuffer> for WriteBackClientBuffers {
        fn into_buffers(
            self,
            buffer_sizes: BufferSizes,
        ) -> (Rc<RefCell<RingBuffer>>, TestSendBuffer) {
            let buffers = ClientBuffers::new(buffer_sizes);
            *self.as_ref().borrow_mut() = Some(buffers.clone());
            let ClientBuffers { receive, send } = buffers;
            (receive, TestSendBuffer::new(send, Default::default()))
        }
    }

    impl<I: TcpTestIpExt, D: IpDeviceId + 'static> SyncContext<I, TcpNonSyncCtx> for TcpSyncCtx<I, D> {
        type IpTransportCtx = FakeBufferIpTransportCtx<I, D>;

        fn with_ip_transport_ctx_isn_generator_and_tcp_sockets_mut<
            O,
            F: FnOnce(
                &mut FakeBufferIpTransportCtx<I, D>,
                &IsnGenerator<FakeInstant>,
                &mut Sockets<I, D, TcpNonSyncCtx>,
            ) -> O,
        >(
            &mut self,
            cb: F,
        ) -> O {
            let WrappedFakeSyncCtx {
                outer: FakeTcpState { isn_generator, sockets },
                inner: ip_transport_ctx,
            } = self;
            cb(ip_transport_ctx, isn_generator, sockets)
        }

        fn with_tcp_sockets<O, F: FnOnce(&Sockets<I, D, TcpNonSyncCtx>) -> O>(&self, cb: F) -> O {
            let WrappedFakeSyncCtx { outer: FakeTcpState { isn_generator: _, sockets }, inner: _ } =
                self;
            cb(sockets)
        }
    }

    impl<I: TcpTestIpExt> TcpSyncCtx<I, FakeDeviceId> {
        fn new(addr: SpecifiedAddr<I::Addr>, peer: SpecifiedAddr<I::Addr>, prefix: u8) -> Self {
            Self::with_inner_and_outer_state(
                FakeBufferIpSocketCtx::with_ctx(FakeIpSocketCtx::<I, _>::with_devices_state(
                    core::iter::once((
                        FakeDeviceId,
                        I::new_device_state(*addr, prefix),
                        alloc::vec![peer],
                    )),
                )),
                FakeTcpState {
                    isn_generator: Default::default(),
                    sockets: Sockets {
                        inactive: IdMap::new(),
                        socketmap: BoundSocketMap::default(),
                        port_alloc: PortAlloc::new(&mut FakeCryptoRng::new_xorshift(0)),
                    },
                },
            )
        }
    }

    impl<I: TcpTestIpExt> TcpSyncCtx<I, MultipleDevicesId> {
        fn new_multiple_devices() -> Self {
            Self::with_inner_and_outer_state(
                FakeBufferIpSocketCtx::with_ctx(FakeIpSocketCtx::<I, _>::with_devices_state(
                    core::iter::empty(),
                )),
                Default::default(),
            )
        }
    }

    const LOCAL: &'static str = "local";
    const REMOTE: &'static str = "remote";
    const PORT_1: NonZeroU16 = const_unwrap_option(NonZeroU16::new(42));
    const PORT_2: NonZeroU16 = const_unwrap_option(NonZeroU16::new(43));

    impl TcpTestIpExt for Ipv4 {
        fn recv_src_addr(addr: Self::Addr) -> Self::RecvSrcAddr {
            addr
        }

        fn new_device_state(addr: Self::Addr, prefix: u8) -> IpDeviceState<FakeInstant, Self> {
            let mut device_state = IpDeviceState::default();
            device_state
                .add_addr(AddrSubnet::new(addr, prefix).unwrap())
                .expect("failed to add address");
            device_state
        }
    }

    impl TcpTestIpExt for Ipv6 {
        fn recv_src_addr(addr: Self::Addr) -> Self::RecvSrcAddr {
            Ipv6SourceAddr::new(addr).unwrap()
        }

        fn new_device_state(addr: Self::Addr, prefix: u8) -> IpDeviceState<FakeInstant, Self> {
            let mut device_state = IpDeviceState::default();
            device_state
                .add_addr(Ipv6AddressEntry::new(
                    AddrSubnet::new(addr, prefix).unwrap(),
                    AddressState::Assigned,
                    AddrConfig::Manual,
                ))
                .expect("failed to add address");
            device_state
        }
    }

    type TcpTestNetwork<I> = FakeNetwork<
        &'static str,
        SendIpPacketMeta<I, FakeDeviceId, SpecifiedAddr<<I as Ip>::Addr>>,
        TcpCtx<I, FakeDeviceId>,
        fn(
            &'static str,
            SendIpPacketMeta<I, FakeDeviceId, SpecifiedAddr<<I as Ip>::Addr>>,
        ) -> Vec<(
            &'static str,
            SendIpPacketMeta<I, FakeDeviceId, SpecifiedAddr<<I as Ip>::Addr>>,
            Option<core::time::Duration>,
        )>,
    >;

    fn new_test_net<I: TcpTestIpExt>() -> TcpTestNetwork<I> {
        FakeNetwork::new(
            [
                (
                    LOCAL,
                    TcpCtx::with_sync_ctx(TcpSyncCtx::new(
                        I::FAKE_CONFIG.local_ip,
                        I::FAKE_CONFIG.remote_ip,
                        I::FAKE_CONFIG.subnet.prefix(),
                    )),
                ),
                (
                    REMOTE,
                    TcpCtx::with_sync_ctx(TcpSyncCtx::new(
                        I::FAKE_CONFIG.remote_ip,
                        I::FAKE_CONFIG.local_ip,
                        I::FAKE_CONFIG.subnet.prefix(),
                    )),
                ),
            ],
            move |net, meta: SendIpPacketMeta<I, _, _>| {
                if net == LOCAL {
                    alloc::vec![(REMOTE, meta, None)]
                } else {
                    alloc::vec![(LOCAL, meta, None)]
                }
            },
        )
    }

    fn handle_frame<I: TcpTestIpExt>(
        TcpCtx { sync_ctx, non_sync_ctx }: &mut TcpCtx<I, FakeDeviceId>,
        meta: SendIpPacketMeta<I, FakeDeviceId, SpecifiedAddr<I::Addr>>,
        buffer: Buf<Vec<u8>>,
    ) {
        TcpIpTransportContext::receive_ip_packet(
            sync_ctx,
            non_sync_ctx,
            &FakeDeviceId,
            I::recv_src_addr(*meta.src_ip),
            meta.dst_ip,
            buffer,
        )
        .expect("failed to deliver bytes");
    }

    impl<I: TcpTestIpExt, D: IpDeviceId, NewIp: TcpTestIpExt> GenericOverIp<NewIp> for TcpCtx<I, D> {
        type Type = TcpCtx<NewIp, D>;
    }

    fn handle_timer<I: Ip + TcpTestIpExt, D: IpDeviceId + 'static>(
        ctx: &mut TcpCtx<I, D>,
        _: &mut (),
        timer_id: TimerId,
    ) {
        I::map_ip(
            (ctx, timer_id),
            |(ctx, timer_id)| {
                let FakeCtxWithSyncCtx { sync_ctx, non_sync_ctx } = ctx;
                let conn_id = assert_matches!(timer_id, TimerId::V4(conn_id) => conn_id);
                SocketHandler::handle_timer(sync_ctx, non_sync_ctx, conn_id)
            },
            |(ctx, timer_id)| {
                let FakeCtxWithSyncCtx { sync_ctx, non_sync_ctx } = ctx;
                let conn_id = assert_matches!(timer_id, TimerId::V6(conn_id) => conn_id);
                SocketHandler::handle_timer(sync_ctx, non_sync_ctx, conn_id)
            },
        )
    }

    /// The following test sets up two connected testing context - one as the
    /// server and the other as the client. Tests if a connection can be
    /// established using `bind`, `listen`, `connect` and `accept`.
    ///
    /// # Arguments
    ///
    /// * `listen_addr` - The address to listen on.
    /// * `bind_client` - Whether to bind the client before connecting.
    fn bind_listen_connect_accept_inner<I: Ip + TcpTestIpExt>(
        listen_addr: I::Addr,
        bind_client: bool,
        seed: u128,
        drop_rate: f64,
    ) -> (TcpTestNetwork<I>, ConnectionId<I>, ConnectionId<I>) {
        let mut net = new_test_net::<I>();
        let mut rng = new_rng(seed);

        let mut maybe_drop_frame =
            |ctx: &mut TcpCtx<I, _>,
             meta: SendIpPacketMeta<I, FakeDeviceId, SpecifiedAddr<<I as Ip>::Addr>>,
             buffer: Buf<Vec<u8>>| {
                let x: f64 = rng.gen();
                if x > drop_rate {
                    handle_frame(ctx, meta, buffer);
                }
            };

        let backlog = NonZeroUsize::new(1).unwrap();
        let server = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let conn = SocketHandler::create_socket(sync_ctx, non_sync_ctx);
            let bound =
                SocketHandler::bind(sync_ctx, non_sync_ctx, conn, listen_addr, Some(PORT_1))
                    .expect("failed to bind the server socket");
            SocketHandler::listen(sync_ctx, non_sync_ctx, bound, backlog)
        });

        let client_ends = WriteBackClientBuffers::default();
        let client = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let conn = SocketHandler::create_socket(sync_ctx, non_sync_ctx);
            if bind_client {
                let conn = SocketHandler::bind(
                    sync_ctx,
                    non_sync_ctx,
                    conn,
                    *I::FAKE_CONFIG.local_ip,
                    Some(PORT_1),
                )
                .expect("failed to bind the client socket");
                SocketHandler::connect_bound(
                    sync_ctx,
                    non_sync_ctx,
                    conn,
                    SocketAddr { ip: I::FAKE_CONFIG.remote_ip, port: PORT_1 },
                    client_ends.clone(),
                )
                .expect("failed to connect")
            } else {
                SocketHandler::connect_unbound(
                    sync_ctx,
                    non_sync_ctx,
                    conn,
                    SocketAddr { ip: I::FAKE_CONFIG.remote_ip, port: PORT_1 },
                    client_ends.clone(),
                )
                .expect("failed to connect")
            }
        });
        // If drop rate is 0, the SYN is guaranteed to be delivered, so we can
        // look at the SYN queue deterministically.
        if drop_rate == 0.0 {
            // Step once for the SYN packet to be sent.
            let _: StepResult = net.step(handle_frame, handle_timer);
            // The listener should create a pending socket.
            assert_matches!(
                net.sync_ctx(REMOTE).outer.sockets.get_listener_by_id_mut(server),
                Some(Listener { backlog: _, ready, pending, buffer_sizes: _, keep_alive: _ }) => {
                    assert_eq!(ready.len(), 0);
                    assert_eq!(pending.len(), 1);
                }
            );
            // The handshake is not done, calling accept here should not succeed.
            net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
                assert_matches!(
                    SocketHandler::accept(sync_ctx, non_sync_ctx, server),
                    Err(AcceptError::WouldBlock)
                );
            });
        }

        // Step the test network until the handshake is done.
        net.run_until_idle(&mut maybe_drop_frame, handle_timer);
        let (accepted, addr, accepted_ends) =
            net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
                SocketHandler::accept(sync_ctx, non_sync_ctx, server).expect("failed to accept")
            });
        if bind_client {
            assert_eq!(addr, SocketAddr { ip: I::FAKE_CONFIG.local_ip, port: PORT_1 });
        } else {
            assert_eq!(addr.ip, I::FAKE_CONFIG.local_ip);
        }

        let mut assert_connected = |name: &'static str, conn_id: ConnectionId<I>| {
            let (conn, (), _): (_, _, &ConnAddr<_, _, _, _>) =
                conn_id.get_from_socketmap(&net.sync_ctx(name).outer.sockets.socketmap);
            assert_matches!(
                conn,
                Connection {
                    acceptor: None,
                    state: State::Established(_),
                    ip_sock: _,
                    defunct: false,
                    keep_alive: _,
                }
            )
        };

        assert_connected(LOCAL, client);
        assert_connected(REMOTE, accepted);

        let ClientBuffers { send: client_snd_end, receive: client_rcv_end } =
            client_ends.as_ref().borrow_mut().take().unwrap();
        let ClientBuffers { send: accepted_snd_end, receive: accepted_rcv_end } = accepted_ends;
        for snd_end in [client_snd_end, accepted_snd_end] {
            snd_end.borrow_mut().extend_from_slice(b"Hello");
        }

        for (c, id) in [(LOCAL, client), (REMOTE, accepted)] {
            net.with_context(c, |TcpCtx { sync_ctx, non_sync_ctx }| {
                SocketHandler::<I, _>::do_send(sync_ctx, non_sync_ctx, id.into())
            })
        }
        net.run_until_idle(&mut maybe_drop_frame, handle_timer);

        for rcv_end in [client_rcv_end, accepted_rcv_end] {
            assert_eq!(
                rcv_end.borrow_mut().read_with(|avail| {
                    let avail = avail.concat();
                    assert_eq!(avail, b"Hello");
                    avail.len()
                }),
                5
            );
        }

        // Check the listener is in correct state.
        assert_eq!(
            net.sync_ctx(REMOTE).outer.sockets.get_listener_by_id_mut(server),
            Some(&mut Listener::new(backlog, BufferSizes::default(), KeepAlive::default())),
        );

        (net, client, accepted)
    }

    #[ip_test]
    fn bind_listen_connect_accept<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        for bind_client in [true, false] {
            for listen_addr in [I::UNSPECIFIED_ADDRESS, *I::FAKE_CONFIG.remote_ip] {
                let (_net, _client, _accepted) =
                    bind_listen_connect_accept_inner::<I>(listen_addr, bind_client, 0, 0.0);
            }
        }
    }

    #[ip_test]
    #[test_case(*<I as TestIpExt>::FAKE_CONFIG.local_ip; "same addr")]
    #[test_case(I::UNSPECIFIED_ADDRESS; "any addr")]
    fn bind_conflict<I: Ip + TcpTestIpExt>(conflict_addr: I::Addr) {
        set_logger_for_test();
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<I, _>::with_sync_ctx(TcpSyncCtx::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));
        let s1 = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);
        let s2 = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);

        let _b1 = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            s1,
            *I::FAKE_CONFIG.local_ip,
            Some(PORT_1),
        )
        .expect("first bind should succeed");
        assert_matches!(
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, s2, conflict_addr, Some(PORT_1)),
            Err(LocalAddressError::AddressInUse)
        );
        let _b2 =
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, s2, conflict_addr, Some(PORT_2))
                .expect("able to rebind to a free address");
    }
    #[ip_test]
    #[test_case(nonzero!(u16::MAX), Ok(nonzero!(u16::MAX)); "ephemeral available")]
    #[test_case(nonzero!(100u16), Err(LocalAddressError::FailedToAllocateLocalPort);
                "no ephemeral available")]
    fn bind_picked_port_all_others_taken<I: Ip + TcpTestIpExt>(
        available_port: NonZeroU16,
        expected_result: Result<NonZeroU16, LocalAddressError>,
    ) {
        set_logger_for_test();
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<I, _>::with_sync_ctx(TcpSyncCtx::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));
        for port in 1..=u16::MAX {
            let port = NonZeroU16::new(port).unwrap();
            if port == available_port {
                continue;
            }
            let unbound = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);
            let bound = SocketHandler::bind(
                &mut sync_ctx,
                &mut non_sync_ctx,
                unbound,
                I::UNSPECIFIED_ADDRESS,
                Some(port),
            )
            .expect("uncontested bind");
            let _listener =
                SocketHandler::listen(&mut sync_ctx, &mut non_sync_ctx, bound, nonzero!(1usize));
        }

        // Now that all but the LOCAL_PORT are occupied, ask the stack to
        // select a port.
        let unbound = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);
        let result = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            I::UNSPECIFIED_ADDRESS,
            None,
        )
        .map(|bound| SocketHandler::get_bound_info(&sync_ctx, bound).port);
        assert_eq!(result, expected_result);
    }

    #[ip_test]
    fn bind_to_non_existent_address<I: Ip + TcpTestIpExt>() {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<I, _>::with_sync_ctx(TcpSyncCtx::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));
        let unbound = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);
        assert_matches!(
            SocketHandler::bind(
                &mut sync_ctx,
                &mut non_sync_ctx,
                unbound,
                *I::FAKE_CONFIG.remote_ip,
                None
            ),
            Err(LocalAddressError::AddressMismatch)
        );

        sync_ctx.with_tcp_sockets(|sockets| {
            assert_matches!(sockets.inactive.get(unbound.into()), Some(_));
        });
    }

    // The test verifies that if client tries to connect to a closed port on
    // server, the connection is aborted and RST is received.
    #[ip_test]
    fn connect_reset<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let mut net = new_test_net::<I>();

        let client = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let conn = SocketHandler::create_socket(sync_ctx, non_sync_ctx);
            let conn = SocketHandler::bind(
                sync_ctx,
                non_sync_ctx,
                conn,
                *I::FAKE_CONFIG.local_ip,
                Some(PORT_1),
            )
            .expect("failed to bind the client socket");
            SocketHandler::connect_bound(
                sync_ctx,
                non_sync_ctx,
                conn,
                SocketAddr { ip: I::FAKE_CONFIG.remote_ip, port: PORT_1 },
                Default::default(),
            )
            .expect("failed to connect")
        });

        // Step one time for SYN packet to be delivered.
        let _: StepResult = net.step(handle_frame, handle_timer);
        // Assert that we got a RST back.
        net.collect_frames();
        assert_matches!(
            &net.iter_pending_frames().collect::<Vec<_>>()[..],
            [InstantAndData(_instant, PendingFrameData {
                dst_context: _,
                meta,
                frame,
            })] => {
            let mut buffer = Buf::new(frame, ..);
            let parsed = buffer.parse_with::<_, TcpSegment<_>>(
                TcpParseArgs::new(*meta.src_ip, *meta.dst_ip)
            ).expect("failed to parse");
            assert!(parsed.rst())
        });

        net.run_until_idle(handle_frame, handle_timer);
        // Finally, the connection should be reset.
        let (conn, (), _): (_, _, &ConnAddr<_, _, _, _>) =
            client.get_from_socketmap(&net.sync_ctx(LOCAL).outer.sockets.socketmap);
        assert_matches!(
            conn,
            Connection {
                acceptor: None,
                state: State::Closed(Closed { reason: UserError::ConnectionReset }),
                ip_sock: _,
                defunct: false,
                keep_alive: _,
            }
        );
    }

    #[ip_test]
    fn retransmission<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        run_with_many_seeds(|seed| {
            let (_net, _client, _accepted) =
                bind_listen_connect_accept_inner::<I>(I::UNSPECIFIED_ADDRESS, false, seed, 0.2);
        });
    }

    const LOCAL_PORT: NonZeroU16 = nonzero!(1845u16);

    #[ip_test]
    fn listener_with_bound_device_conflict<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::with_sync_ctx(TcpSyncCtx::<I, _>::new_multiple_devices());

        let bound_a = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);
        SocketHandler::set_unbound_device(
            &mut sync_ctx,
            &mut non_sync_ctx,
            bound_a,
            Some(MultipleDevicesId::A),
        );
        let bound_a = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            bound_a,
            I::UNSPECIFIED_ADDRESS,
            Some(LOCAL_PORT),
        )
        .expect("bind should succeed");
        let _bound_a =
            SocketHandler::listen(&mut sync_ctx, &mut non_sync_ctx, bound_a, nonzero!(10usize));

        let s = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);
        // Binding `s` to the unspecified address should fail since the address
        // is shadowed by `bound_a`.
        assert_matches!(
            SocketHandler::bind(
                &mut sync_ctx,
                &mut non_sync_ctx,
                s,
                I::UNSPECIFIED_ADDRESS,
                Some(LOCAL_PORT)
            ),
            Err(LocalAddressError::AddressInUse)
        );

        // Once `s` is bound to a different device, though, it no longer
        // conflicts.
        SocketHandler::set_unbound_device(
            &mut sync_ctx,
            &mut non_sync_ctx,
            s,
            Some(MultipleDevicesId::B),
        );
        let _: BoundId<_> = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            s,
            I::UNSPECIFIED_ADDRESS,
            Some(LOCAL_PORT),
        )
        .expect("no conflict");
    }

    #[test_case(None)]
    #[test_case(Some(MultipleDevicesId::B); "other")]
    fn set_bound_device_listener_on_zoned_addr(set_device: Option<MultipleDevicesId>) {
        set_logger_for_test();
        let ll_addr = LinkLocalAddr::new(Ipv6::LINK_LOCAL_UNICAST_SUBNET.network()).unwrap();

        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::with_sync_ctx(TcpSyncCtx::<Ipv6, _>::with_inner_and_outer_state(
                FakeBufferIpSocketCtx::with_ctx(FakeIpSocketCtx::new(
                    MultipleDevicesId::all().into_iter().map(|device| FakeDeviceConfig {
                        device,
                        local_ips: vec![ll_addr.into_specified()],
                        remote_ips: vec![ll_addr.into_specified()],
                    }),
                )),
                Default::default(),
            ));

        let unbound = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);
        // TODO(https://fxbug.dev/115524): Use a local address with a zone
        // instead of setting the device manually.
        SocketHandler::set_unbound_device(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            Some(MultipleDevicesId::A),
        );
        let bound = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            *ll_addr,
            Some(LOCAL_PORT),
        )
        .expect("bind should succeed");

        assert_matches!(
            SocketHandler::set_bound_device(&mut sync_ctx, &mut non_sync_ctx, bound, set_device),
            Err(SetDeviceError::ZoneChange)
        );
    }

    #[test_case(None)]
    #[test_case(Some(MultipleDevicesId::B); "other")]
    fn set_bound_device_connected_to_zoned_addr(set_device: Option<MultipleDevicesId>) {
        set_logger_for_test();
        let ll_addr = LinkLocalAddr::new(Ipv6::LINK_LOCAL_UNICAST_SUBNET.network()).unwrap();

        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::with_sync_ctx(TcpSyncCtx::<Ipv6, _>::with_inner_and_outer_state(
                FakeBufferIpSocketCtx::with_ctx(FakeIpSocketCtx::new(
                    MultipleDevicesId::all().into_iter().map(|device| FakeDeviceConfig {
                        device,
                        local_ips: vec![ll_addr.into_specified()],
                        remote_ips: vec![ll_addr.into_specified()],
                    }),
                )),
                Default::default(),
            ));

        let unbound = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);
        // TODO(https://fxbug.dev/115524): Use a remote address with a zone
        // instead of setting the device manually.
        SocketHandler::set_unbound_device(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            Some(MultipleDevicesId::A),
        );
        let bound = SocketHandler::connect_unbound(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            SocketAddr { ip: ll_addr.into_specified(), port: LOCAL_PORT },
            Default::default(),
        )
        .expect("connect should succeed");

        assert_matches!(
            SocketHandler::set_connection_device(
                &mut sync_ctx,
                &mut non_sync_ctx,
                bound,
                set_device
            ),
            Err(SetDeviceError::ZoneChange)
        );
    }

    #[ip_test]
    #[test_case(*<I as TestIpExt>::FAKE_CONFIG.local_ip, true; "specified bound")]
    #[test_case(I::UNSPECIFIED_ADDRESS, true; "unspecified bound")]
    #[test_case(*<I as TestIpExt>::FAKE_CONFIG.local_ip, false; "specified listener")]
    #[test_case(I::UNSPECIFIED_ADDRESS, false; "unspecified listener")]
    fn bound_socket_info<I: Ip + TcpTestIpExt>(ip_addr: I::Addr, listen: bool) {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::with_sync_ctx(TcpSyncCtx::<I, _>::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));
        let unbound = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);

        let (addr, port) = (ip_addr, PORT_1);
        let zoned_addr = SpecifiedAddr::new(addr).map(ZonedAddr::Unzoned);
        let bound =
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, addr, Some(port))
                .expect("bind should succeed");
        let info = if listen {
            let listener =
                SocketHandler::listen(&mut sync_ctx, &mut non_sync_ctx, bound, nonzero!(25usize));
            SocketHandler::get_listener_info(&sync_ctx, listener)
        } else {
            SocketHandler::get_bound_info(&sync_ctx, bound)
        };
        assert_eq!(info, BoundInfo { addr: zoned_addr, port, device: None });
    }

    #[ip_test]
    fn connection_info<I: Ip + TcpTestIpExt>() {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::with_sync_ctx(TcpSyncCtx::<I, _>::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));
        let local = SocketAddr { ip: I::FAKE_CONFIG.local_ip, port: PORT_1 };
        let remote = SocketAddr { ip: I::FAKE_CONFIG.remote_ip, port: PORT_2 };

        let unbound = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);
        let bound = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            *local.ip,
            Some(local.port),
        )
        .expect("bind should succeed");

        let connected = SocketHandler::connect_bound(
            &mut sync_ctx,
            &mut non_sync_ctx,
            bound,
            remote,
            Default::default(),
        )
        .expect("connect should succeed");

        assert_eq!(
            SocketHandler::get_connection_info(&sync_ctx, connected),
            ConnectionInfo {
                local_addr: (ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip), PORT_1),
                remote_addr: (ZonedAddr::Unzoned(I::FAKE_CONFIG.remote_ip), PORT_2),
                device: None
            }
        );
    }

    #[test]
    fn bound_connection_info_zoned_addrs() {
        let local_ip = LinkLocalAddr::new(net_ip_v6!("fe80::1")).unwrap().into_specified();
        let remote_ip = LinkLocalAddr::new(net_ip_v6!("fe80::2")).unwrap().into_specified();
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::with_sync_ctx(TcpSyncCtx::<Ipv6, _>::new(
                local_ip,
                remote_ip,
                Ipv6::LINK_LOCAL_UNICAST_SUBNET.prefix(),
            ));
        let local = SocketAddr { ip: local_ip, port: PORT_1 };
        let remote = SocketAddr { ip: remote_ip, port: PORT_2 };

        let unbound = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);
        // TODO(https://fxbug.dev/115524): Bind to a local address with a zone
        // instead of setting the device manually.
        SocketHandler::set_unbound_device(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            Some(FakeDeviceId),
        );
        let bound = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            *local.ip,
            Some(local.port),
        )
        .expect("bind should succeed");

        assert_eq!(
            SocketHandler::get_bound_info(&sync_ctx, bound),
            BoundInfo {
                addr: Some(ZonedAddr::Zoned(AddrAndZone::new(*local_ip, FakeDeviceId).unwrap())),
                port: PORT_1,
                device: Some(FakeDeviceId)
            }
        );

        let connected = SocketHandler::connect_bound(
            &mut sync_ctx,
            &mut non_sync_ctx,
            bound,
            remote,
            Default::default(),
        )
        .expect("connect should succeed");

        assert_eq!(
            SocketHandler::get_connection_info(&sync_ctx, connected),
            ConnectionInfo {
                local_addr: (
                    ZonedAddr::Zoned(AddrAndZone::new(*local_ip, FakeDeviceId).unwrap()),
                    PORT_1
                ),
                remote_addr: (
                    ZonedAddr::Zoned(AddrAndZone::new(*remote_ip, FakeDeviceId).unwrap()),
                    PORT_2
                ),
                device: Some(FakeDeviceId)
            }
        );
    }

    #[ip_test]
    fn connection_close<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let (mut net, local, remote) =
            bind_listen_connect_accept_inner::<I>(I::UNSPECIFIED_ADDRESS, false, 0, 0.0);
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            SocketHandler::close_conn(sync_ctx, non_sync_ctx, remote);
        });
        net.run_until_idle(handle_frame, handle_timer);
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            sync_ctx.with_tcp_sockets(|sockets| {
                let (conn, (), _addr) =
                    sockets.socketmap.conns().get_by_id(&remote.into()).expect("invalid conn ID");
                assert_matches!(conn.state, State::FinWait2(_));
            })
        });
        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            SocketHandler::close_conn(sync_ctx, non_sync_ctx, local);
        });
        net.run_until_idle(handle_frame, handle_timer);

        for (name, id) in [(LOCAL, local), (REMOTE, remote)] {
            net.with_context(name, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
                sync_ctx.with_tcp_sockets(|sockets| {
                    assert_matches!(sockets.socketmap.conns().get_by_id(&id.into()), None);
                })
            });
        }
    }

    #[ip_test]
    fn connection_shutdown_then_close<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let (mut net, local, remote) =
            bind_listen_connect_accept_inner::<I>(I::UNSPECIFIED_ADDRESS, false, 0, 0.0);

        for (name, id) in [(LOCAL, local), (REMOTE, remote)] {
            net.with_context(name, |TcpCtx { sync_ctx, non_sync_ctx }| {
                assert_matches!(SocketHandler::shutdown_conn(sync_ctx, non_sync_ctx, id), Ok(()));
                sync_ctx.with_tcp_sockets(|sockets| {
                    let (conn, (), _addr) = remote.get_from_socketmap(&sockets.socketmap);
                    assert_matches!(conn.state, State::FinWait1(_));
                });
                assert_matches!(SocketHandler::shutdown_conn(sync_ctx, non_sync_ctx, id), Ok(()));
            });
        }
        net.run_until_idle(handle_frame, handle_timer);
        for (name, id) in [(LOCAL, local), (REMOTE, remote)] {
            net.with_context(name, |TcpCtx { sync_ctx, non_sync_ctx }| {
                sync_ctx.with_tcp_sockets(|sockets| {
                    let (conn, (), _addr) = remote.get_from_socketmap(&sockets.socketmap);
                    assert_matches!(conn.state, State::Closed(_));
                });
                SocketHandler::close_conn(sync_ctx, non_sync_ctx, id);
                sync_ctx.with_tcp_sockets(|sockets| {
                    assert_matches!(sockets.socketmap.conns().get_by_id(&id.into()), None);
                })
            });
        }
    }

    #[ip_test]
    fn remove_unbound<I: Ip + TcpTestIpExt>() {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::with_sync_ctx(TcpSyncCtx::<I, _>::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));
        let unbound = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);
        SocketHandler::remove_unbound(&mut sync_ctx, unbound);

        sync_ctx.with_tcp_sockets(|Sockets { socketmap: _, inactive, port_alloc: _ }| {
            assert_eq!(inactive.get(unbound.into()), None);
        })
    }

    #[ip_test]
    fn remove_bound<I: Ip + TcpTestIpExt>() {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::with_sync_ctx(TcpSyncCtx::<I, _>::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));
        let unbound = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx);
        let bound = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            *I::FAKE_CONFIG.local_ip,
            None,
        )
        .expect("bind should succeed");
        SocketHandler::remove_bound(&mut sync_ctx, bound);

        sync_ctx.with_tcp_sockets(|Sockets { socketmap, inactive, port_alloc: _ }| {
            assert_eq!(inactive.get(unbound.into()), None);
            assert_eq!(socketmap.listeners().get_by_id(&bound.into()), None);
        })
    }

    #[ip_test]
    fn shutdown_listener<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let mut net = new_test_net::<I>();
        let local_listener = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx);
            let bound = SocketHandler::bind(
                sync_ctx,
                non_sync_ctx,
                unbound,
                *I::FAKE_CONFIG.local_ip,
                Some(PORT_1),
            )
            .expect("bind should succeed");
            SocketHandler::listen(sync_ctx, non_sync_ctx, bound, NonZeroUsize::new(5).unwrap())
        });

        let remote_connection = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx);
            SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                unbound,
                SocketAddr { ip: I::FAKE_CONFIG.local_ip, port: PORT_1 },
                Default::default(),
            )
            .expect("connect should succeed")
        });

        // After the following step, we should have one established connection
        // in the listener's accept queue, which ought to be aborted during
        // shutdown.
        net.run_until_idle(handle_frame, handle_timer);

        // Create a second half-open connection so that we have one entry in the
        // pending queue.
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx);
            let _: ConnectionId<_> = SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                unbound,
                SocketAddr { ip: I::FAKE_CONFIG.local_ip, port: PORT_1 },
                Default::default(),
            )
            .expect("connect should succeed");
        });

        let _: StepResult = net.step(handle_frame, handle_timer);

        // We have a timer scheduled for the pending connection.
        net.with_context(LOCAL, |TcpCtx { sync_ctx: _, non_sync_ctx }| {
            assert_matches!(non_sync_ctx.timer_ctx().timers().len(), 1);
        });

        let local_bound = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            SocketHandler::shutdown_listener(sync_ctx, non_sync_ctx, local_listener)
        });

        // The timer for the pending connection should be cancelled.
        net.with_context(LOCAL, |TcpCtx { sync_ctx: _, non_sync_ctx }| {
            assert_eq!(non_sync_ctx.timer_ctx().timers().len(), 0);
        });

        net.run_until_idle(handle_frame, handle_timer);

        // The remote socket should now be reset to Closed state.
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            sync_ctx.with_tcp_sockets(|sockets| {
                let (conn, (), _addr) = remote_connection.get_from_socketmap(&sockets.socketmap);
                assert_matches!(
                    conn.state,
                    State::Closed(Closed { reason: UserError::ConnectionReset })
                );
            });
        });

        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let new_unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx);
            assert_matches!(
                SocketHandler::bind(
                    sync_ctx,
                    non_sync_ctx,
                    new_unbound,
                    *I::FAKE_CONFIG.local_ip,
                    Some(PORT_1),
                ),
                Err(LocalAddressError::AddressInUse)
            );
            // Bring the already-shutdown listener back to listener again.
            let _: ListenerId<_> = SocketHandler::listen(
                sync_ctx,
                non_sync_ctx,
                local_bound,
                NonZeroUsize::new(5).unwrap(),
            );
        });

        let new_remote_connection =
            net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
                let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx);
                SocketHandler::connect_unbound(
                    sync_ctx,
                    non_sync_ctx,
                    unbound,
                    SocketAddr { ip: I::FAKE_CONFIG.local_ip, port: PORT_1 },
                    Default::default(),
                )
                .expect("connect should succeed")
            });

        net.run_until_idle(handle_frame, handle_timer);

        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            sync_ctx.with_tcp_sockets(|sockets| {
                let (conn, (), _addr) =
                    new_remote_connection.get_from_socketmap(&sockets.socketmap);
                assert_matches!(conn.state, State::Established(_));
            });
        });
    }

    #[ip_test]
    fn set_send_buffer_size<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let mut net = new_test_net::<I>();

        // TODO(https://fxbug.dev/110625): Enhance this test to read back and
        // verify the values once we support getting the send buffer size for
        // TCP sockets.
        let mut local_send_size: usize = 2048;
        let mut remote_send_size: usize = 1024;

        let local_listener = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx);
            let bound = SocketHandler::bind(
                sync_ctx,
                non_sync_ctx,
                unbound,
                *I::FAKE_CONFIG.local_ip,
                Some(PORT_1),
            )
            .expect("bind should succeed");
            SocketHandler::set_send_buffer_size(sync_ctx, non_sync_ctx, bound, local_send_size);
            SocketHandler::listen(sync_ctx, non_sync_ctx, bound, NonZeroUsize::new(5).unwrap())
        });

        let remote_connection = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx);
            SocketHandler::set_send_buffer_size(sync_ctx, non_sync_ctx, unbound, remote_send_size);
            SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                unbound,
                SocketAddr { ip: I::FAKE_CONFIG.local_ip, port: PORT_1 },
                Default::default(),
            )
            .expect("connect should succeed")
        });
        let mut step_and_increment_send_sizes_until_idle =
            |net: &mut TcpTestNetwork<I>, local: SocketId<_>, remote: SocketId<_>| loop {
                local_send_size += 1;
                remote_send_size += 1;
                net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
                    SocketHandler::set_send_buffer_size(
                        sync_ctx,
                        non_sync_ctx,
                        local,
                        local_send_size,
                    )
                });
                net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
                    SocketHandler::set_send_buffer_size(
                        sync_ctx,
                        non_sync_ctx,
                        remote,
                        remote_send_size,
                    )
                });
                if net.step(handle_frame, handle_timer).is_idle() {
                    break;
                }
            };

        // Set the send buffer size at each stage of sockets on both ends of the
        // handshake process just to make sure it doesn't break.
        step_and_increment_send_sizes_until_idle(
            &mut net,
            local_listener.into(),
            remote_connection.into(),
        );

        let local_connection = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let (conn, _, _) = SocketHandler::accept(sync_ctx, non_sync_ctx, local_listener)
                .expect("received connection");
            conn
        });

        step_and_increment_send_sizes_until_idle(
            &mut net,
            local_connection.into(),
            remote_connection.into(),
        );

        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            SocketHandler::shutdown_conn(sync_ctx, non_sync_ctx, local_connection)
                .expect("is connected");
        });

        step_and_increment_send_sizes_until_idle(
            &mut net,
            local_connection.into(),
            remote_connection.into(),
        );

        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            SocketHandler::shutdown_conn(sync_ctx, non_sync_ctx, remote_connection)
                .expect("is connected");
        });

        step_and_increment_send_sizes_until_idle(
            &mut net,
            local_connection.into(),
            remote_connection.into(),
        );
    }
}
