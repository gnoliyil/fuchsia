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
use lock_order::Locked;

use assert_matches::assert_matches;
use const_unwrap::const_unwrap_option;
use derivative::Derivative;
use net_types::{
    ip::{
        GenericOverIp, Ip, IpAddr, IpAddress, IpInvariant, IpVersionMarker, Ipv4, Ipv4Addr, Ipv6,
        Ipv6Addr,
    },
    AddrAndZone, SpecifiedAddr, ZonedAddr,
};
use packet::EmptyBuf;
use packet_formats::ip::IpProto;
use rand::RngCore;
use smallvec::{smallvec, SmallVec};
use thiserror::Error;
use tracing::{debug, warn};

use crate::{
    algorithm::{PortAlloc, PortAllocImpl},
    context::{TimerContext, TracingContext},
    data_structures::{
        id_map::{self, Entry as IdMapEntry, EntryKey, IdMap},
        id_map_collection::{
            Entry as IdMapCollectionEntry, IdMapCollection,
            OccupiedEntry as IdMapCollectionOccupied,
        },
        socketmap::{IterShadows as _, SocketMap, Tagged},
    },
    device::{AnyDevice, DeviceId, DeviceIdContext, Id, WeakDeviceId, WeakId},
    error::{ExistsError, LocalAddressError, ZonedAddressError},
    ip::{
        icmp::IcmpErrorCode,
        socket::{
            BufferIpSocketHandler as _, DefaultSendOptions, DeviceIpSocketHandler, IpSock,
            IpSockCreationError, IpSocketHandler as _, Mms,
        },
        BufferTransportIpContext, EitherDeviceId, IpExt, IpLayerIpExt, TransportIpContext as _,
    },
    socket::{
        address::{ConnAddr, ConnIpAddr, IpPortSpec, ListenerAddr, ListenerIpAddr},
        AddrVec, Bound, BoundSocketMap, Connection as BoundConnection, ConvertSocketMapState,
        ConvertSocketTypeState, IncompatibleError, InsertError, Inserter,
        Listener as BoundListener, RemoveResult, SocketId as BoundSocketId, SocketMapAddrStateSpec,
        SocketMapAddrStateUpdateSharingSpec, SocketMapConflictPolicy, SocketMapStateSpec,
        SocketMapUpdateSharingPolicy, SocketState as BoundSocketState, SocketStateSpec,
        UpdateSharingError,
    },
    transport::tcp::{
        buffer::{IntoBuffers, ReceiveBuffer, SendBuffer},
        seqnum::SeqNum,
        socket::{demux::tcp_serialize_segment, isn::IsnGenerator},
        state::{CloseError, CloseReason, Closed, Initial, State, Takeable},
        BufferSizes, ConnectionError, Mss, OptionalBufferSizes, SocketOptions,
    },
    Instant, SyncCtx,
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
pub trait NonSyncContext: TimerContext<TimerId> + TracingContext {
    /// Receive buffer used by TCP.
    type ReceiveBuffer: ReceiveBuffer;
    /// Send buffer used by TCP.
    type SendBuffer: SendBuffer;
    /// The object that will be returned by the state machine when a passive
    /// open connection becomes established. The bindings can use this object
    /// to read/write bytes from/into the created buffers.
    type ReturnedBuffers: Debug;
    /// The extra information provided by the Bindings that implements platform
    /// dependent behaviors. It serves as a [`ListenerNotifier`] if the socket
    /// was used as a listener and it will be used to provide buffers if used
    /// to establish connections.
    type ListenerNotifierOrProvidedBuffers: Debug
        + Takeable
        + Clone
        + IntoBuffers<Self::ReceiveBuffer, Self::SendBuffer>
        + ListenerNotifier;

    /// The buffer sizes to use when creating new sockets.
    fn default_buffer_sizes() -> BufferSizes;

    /// Creates new buffers and returns the object that Bindings need to
    /// read/write from/into the created buffers.
    fn new_passive_open_buffers(
        buffer_sizes: BufferSizes,
    ) -> (Self::ReceiveBuffer, Self::SendBuffer, Self::ReturnedBuffers);
}

/// A notifier used to tell Bindings about new pending connections for a single
/// socket.
pub trait ListenerNotifier {
    /// When the ready queue length has changed, signal to the Bindings.
    fn new_incoming_connections(&mut self, num_ready: usize);
}

/// Sync context for TCP.
pub(crate) trait SyncContext<I: IpLayerIpExt, C: NonSyncContext>:
    DeviceIdContext<AnyDevice>
{
    type IpTransportCtx<'a>: BufferTransportIpContext<
            I,
            C,
            EmptyBuf,
            DeviceId = Self::DeviceId,
            WeakDeviceId = Self::WeakDeviceId,
        > + DeviceIpSocketHandler<I, C>;

    /// Calls the function with a `Self::IpTransportCtx`, immutable reference to
    /// an initial sequence number generator and a mutable reference to TCP
    /// socket state.
    fn with_ip_transport_ctx_isn_generator_and_tcp_sockets_mut<
        O,
        F: FnOnce(
            &mut Self::IpTransportCtx<'_>,
            &IsnGenerator<C::Instant>,
            &mut Sockets<I, Self::WeakDeviceId, C>,
        ) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O;

    /// Calls the function with a `Self::IpTransportCtx` and a mutable reference
    /// to TCP socket state.
    fn with_ip_transport_ctx_and_tcp_sockets_mut<
        O,
        F: FnOnce(&mut Self::IpTransportCtx<'_>, &mut Sockets<I, Self::WeakDeviceId, C>) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O {
        self.with_ip_transport_ctx_isn_generator_and_tcp_sockets_mut(|ctx, _isn, sockets| {
            cb(ctx, sockets)
        })
    }

    /// Calls the function with a mutable reference to TCP socket state.
    fn with_tcp_sockets_mut<O, F: FnOnce(&mut Sockets<I, Self::WeakDeviceId, C>) -> O>(
        &mut self,
        cb: F,
    ) -> O {
        self.with_ip_transport_ctx_isn_generator_and_tcp_sockets_mut(|_ctx, _isn, sockets| {
            cb(sockets)
        })
    }

    /// Calls the function with an immutable reference to TCP socket state.
    fn with_tcp_sockets<O, F: FnOnce(&Sockets<I, Self::WeakDeviceId, C>) -> O>(
        &mut self,
        cb: F,
    ) -> O;
}

/// Socket address includes the ip address and the port number.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, GenericOverIp)]
pub struct SocketAddr<A: IpAddress, D> {
    /// The IP component of the address.
    pub ip: ZonedAddr<A, D>,
    /// The port component of the address.
    pub port: NonZeroU16,
}

impl<A: IpAddress, D> From<SocketAddr<A, D>>
    for IpAddr<SocketAddr<Ipv4Addr, D>, SocketAddr<Ipv6Addr, D>>
{
    fn from(addr: SocketAddr<A, D>) -> IpAddr<SocketAddr<Ipv4Addr, D>, SocketAddr<Ipv6Addr, D>> {
        let IpInvariant(addr) = <A::Version as Ip>::map_ip(
            addr,
            |i| IpInvariant(IpAddr::V4(i)),
            |i| IpInvariant(IpAddr::V6(i)),
        );
        addr
    }
}

/// An implementation of [`IpTransportContext`] for TCP.
pub(crate) enum TcpIpTransportContext {}

/// Uninstantiatable type for implementing [`SocketMapStateSpec`].
struct TcpSocketSpec<Ip, Device, NonSyncContext>(PhantomData<(Ip, Device, NonSyncContext)>, Never);

impl<I: IpExt, D: Id, C: NonSyncContext> SocketMapStateSpec for TcpSocketSpec<I, D, C> {
    type ListenerId = MaybeListenerId<I>;
    type ConnId = MaybeClosedConnectionId<I>;

    type ListenerSharingState = ListenerSharingState;
    type ConnSharingState = SharingState;
    type AddrVecTag = AddrVecTag;

    type ListenerAddrState = ListenerAddrState<I>;
    type ConnAddrState = ConnAddrState<I>;
}

impl<I: IpExt, D: Id, C: NonSyncContext> SocketStateSpec for TcpSocketSpec<I, D, C> {
    type ListenerState = MaybeListener<
        I,
        C::ReturnedBuffers,
        C::ListenerNotifierOrProvidedBuffers,
        C::ListenerNotifierOrProvidedBuffers,
    >;
    type ConnState = Connection<
        I,
        D,
        C::Instant,
        C::ReceiveBuffer,
        C::SendBuffer,
        C::ListenerNotifierOrProvidedBuffers,
    >;
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
struct AddrVecTag {
    sharing: SharingState,
    state: SocketTagState,
    has_device: bool,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
enum SocketTagState {
    Conn,
    Listener,
    Bound,
}

#[derive(Debug)]
enum ListenerAddrState<I: Ip> {
    ExclusiveBound(BoundId<I>),
    ExclusiveListener(ListenerId<I>),
    Shared { listener: Option<ListenerId<I>>, bound: SmallVec<[BoundId<I>; 1]> },
}

#[derive(Clone, Debug)]
#[cfg_attr(test, derive(PartialEq))]
pub(crate) struct ListenerSharingState {
    pub(crate) sharing: SharingState,
    pub(crate) listening: bool,
}

enum ListenerAddrInserter<'a, I: Ip> {
    Listener(&'a mut Option<ListenerId<I>>),
    Bound(&'a mut SmallVec<[BoundId<I>; 1]>),
}

impl<'a, I: Ip> Inserter<MaybeListenerId<I>> for ListenerAddrInserter<'a, I> {
    fn insert(self, MaybeListenerId(x, marker): MaybeListenerId<I>) {
        match self {
            Self::Listener(o) => *o = Some(ListenerId(x, marker)),
            Self::Bound(b) => b.push(BoundId(x, marker)),
        }
    }
}

impl<I: Ip> SocketMapAddrStateSpec for ListenerAddrState<I> {
    type SharingState = ListenerSharingState;
    type Id = MaybeListenerId<I>;
    type Inserter<'a> = ListenerAddrInserter<'a, I>;

    fn new(new_sharing_state: &Self::SharingState, MaybeListenerId(id, marker): Self::Id) -> Self {
        let ListenerSharingState { sharing, listening } = new_sharing_state;
        match sharing {
            SharingState::Exclusive => match listening {
                true => Self::ExclusiveListener(ListenerId(id, marker)),
                false => Self::ExclusiveBound(BoundId(id, marker)),
            },
            SharingState::ReuseAddress => {
                let (listener, bound) = if *listening {
                    (Some(ListenerId(id, marker)), Default::default())
                } else {
                    (None, smallvec![BoundId(id, marker)])
                };
                Self::Shared { listener, bound }
            }
        }
    }

    fn contains_id(&self, MaybeListenerId(id, _): &Self::Id) -> bool {
        match self {
            Self::ExclusiveBound(BoundId(x, _)) => id == x,
            Self::ExclusiveListener(ListenerId(x, _)) => id == x,
            Self::Shared { listener, bound } => {
                listener.as_ref().map_or(false, |ListenerId(x, _)| id == x)
                    || bound.contains(&BoundId(*id, IpVersionMarker::default()))
            }
        }
    }

    fn could_insert(
        &self,
        new_sharing_state: &Self::SharingState,
    ) -> Result<(), IncompatibleError> {
        match self {
            Self::ExclusiveBound(_) | Self::ExclusiveListener(_) => Err(IncompatibleError),
            Self::Shared { listener, bound: _ } => {
                let ListenerSharingState { listening: _, sharing } = new_sharing_state;
                match sharing {
                    SharingState::Exclusive => Err(IncompatibleError),
                    SharingState::ReuseAddress => match listener {
                        Some(_) => Err(IncompatibleError),
                        None => Ok(()),
                    },
                }
            }
        }
    }

    fn remove_by_id(&mut self, MaybeListenerId(id, _marker): Self::Id) -> RemoveResult {
        match self {
            Self::ExclusiveBound(BoundId(b, _marker)) => {
                assert_eq!(*b, id);
                RemoveResult::IsLast
            }
            Self::ExclusiveListener(ListenerId(l, _marker)) => {
                assert_eq!(*l, id);
                RemoveResult::IsLast
            }
            Self::Shared { listener, bound } => {
                match listener {
                    Some(ListenerId(l, _marker)) if *l == id => {
                        *listener = None;
                    }
                    Some(_) | None => {
                        let index = bound
                            .iter()
                            .position(|BoundId(b, _)| *b == id)
                            .expect("invalid MaybeListenerId");
                        let _: BoundId<_> = bound.swap_remove(index);
                    }
                };
                match (listener, bound.is_empty()) {
                    (Some(_), _) => RemoveResult::Success,
                    (None, false) => RemoveResult::Success,
                    (None, true) => RemoveResult::IsLast,
                }
            }
        }
    }

    fn try_get_inserter<'a, 'b>(
        &'b mut self,
        new_sharing_state: &'a Self::SharingState,
    ) -> Result<Self::Inserter<'b>, IncompatibleError> {
        match self {
            Self::ExclusiveBound(_) | Self::ExclusiveListener(_) => Err(IncompatibleError),
            Self::Shared { listener, bound } => {
                let ListenerSharingState { listening, sharing } = new_sharing_state;
                match sharing {
                    SharingState::Exclusive => Err(IncompatibleError),
                    SharingState::ReuseAddress => {
                        match listener {
                            Some(_) => {
                                // Always fail to insert if there is already a
                                // listening socket.
                                Err(IncompatibleError)
                            }
                            None => Ok(match listening {
                                true => ListenerAddrInserter::Listener(listener),
                                false => ListenerAddrInserter::Bound(bound),
                            }),
                        }
                    }
                }
            }
        }
    }
}

impl<I: IpExt, D: WeakId, C: NonSyncContext>
    SocketMapUpdateSharingPolicy<
        ListenerAddr<I::Addr, D, NonZeroU16>,
        ListenerSharingState,
        I,
        D,
        IpPortSpec,
    > for TcpSocketSpec<I, D, C>
where
    Bound<Self>: Tagged<AddrVec<I, D, IpPortSpec>, Tag = AddrVecTag>,
{
    fn allows_sharing_update(
        socketmap: &SocketMap<AddrVec<I, D, IpPortSpec>, Bound<Self>>,
        addr: &ListenerAddr<I::Addr, D, NonZeroU16>,
        ListenerSharingState{listening: old_listening, sharing: old_sharing}: &ListenerSharingState,
        ListenerSharingState{listening: new_listening, sharing: new_sharing}: &ListenerSharingState,
    ) -> Result<(), UpdateSharingError> {
        let ListenerAddr { device, ip: ListenerIpAddr { addr: _, identifier } } = addr;
        match (old_listening, new_listening) {
            (true, false) => (), // Changing a listener to bound is always okay.
            (true, true) | (false, false) => (), // No change
            (false, true) => {
                // Upgrading a bound socket to a listener requires no other
                // listeners on similar addresses. We can check that by checking
                // that there are no listeners shadowing the any-listener
                // address.
                if socketmap
                    .descendant_counts(
                        &ListenerAddr {
                            device: None,
                            ip: ListenerIpAddr { addr: None, identifier: *identifier },
                        }
                        .into(),
                    )
                    .any(
                        |(AddrVecTag { state, has_device: _, sharing: _ }, _): &(
                            _,
                            NonZeroUsize,
                        )| match state {
                            SocketTagState::Conn | SocketTagState::Bound => false,
                            SocketTagState::Listener => true,
                        },
                    )
                {
                    return Err(UpdateSharingError);
                }
            }
        }

        match (old_sharing, new_sharing) {
            (SharingState::Exclusive, SharingState::Exclusive)
            | (SharingState::ReuseAddress, SharingState::ReuseAddress) => (),
            (SharingState::Exclusive, SharingState::ReuseAddress) => (),
            (SharingState::ReuseAddress, SharingState::Exclusive) => {
                // Linux allows this, but it introduces inconsistent socket
                // state: if some sockets were allowed to bind because they all
                // had SO_REUSEADDR set, then allowing clearing SO_REUSEADDR on
                // one of them makes the state inconsistent. We only allow this
                // if it doesn't introduce inconsistencies.
                let root_addr = ListenerAddr {
                    device: None,
                    ip: ListenerIpAddr { addr: None, identifier: *identifier },
                };

                let conflicts = match device {
                    // If the socket doesn't have a device, it conflicts with
                    // any listeners that shadow it or that it shadows.
                    None => {
                        socketmap.descendant_counts(&addr.clone().into()).any(
                            |(AddrVecTag { has_device: _, sharing: _, state }, _)| match state {
                                SocketTagState::Conn => false,
                                SocketTagState::Bound | SocketTagState::Listener => true,
                            },
                        ) || (addr != &root_addr && socketmap.get(&root_addr.into()).is_some())
                    }
                    Some(_) => {
                        // If the socket has a device, it will indirectly
                        // conflict with a listener that doesn't have a device
                        // that is either on the same address or the unspecified
                        // address (on the same port).
                        socketmap.descendant_counts(&root_addr.into()).any(
                            |(AddrVecTag { has_device, sharing: _, state }, _)| match state {
                                SocketTagState::Conn => false,
                                SocketTagState::Bound | SocketTagState::Listener => !has_device,
                            },
                        )
                        // Detect a conflict with a shadower (which must also
                        // have a device) on the same address or on a specific
                        // address if this socket is on the unspecified address.
                        || socketmap.descendant_counts(&addr.clone().into()).any(
                            |(AddrVecTag { has_device: _, sharing: _, state }, _)| match state {
                                SocketTagState::Conn => false,
                                SocketTagState::Bound | SocketTagState::Listener => true,
                            },
                        )
                    }
                };

                if conflicts {
                    return Err(UpdateSharingError);
                }
            }
        }

        Ok(())
    }
}

impl<I: Ip> SocketMapAddrStateUpdateSharingSpec for ListenerAddrState<I> {
    fn try_update_sharing(
        &mut self,
        MaybeListenerId(id, marker): Self::Id,
        ListenerSharingState{listening: new_listening, sharing: new_sharing}: &Self::SharingState,
    ) -> Result<(), IncompatibleError> {
        match self {
            Self::ExclusiveBound(BoundId(i, _marker))
            | Self::ExclusiveListener(ListenerId(i, _marker)) => {
                assert_eq!(*i, id);
                *self = match new_sharing {
                    SharingState::Exclusive => match new_listening {
                        true => Self::ExclusiveListener(ListenerId(id, marker)),
                        false => Self::ExclusiveBound(BoundId(id, marker)),
                    },
                    SharingState::ReuseAddress => {
                        let (listener, bound) = match new_listening {
                            true => (Some(ListenerId(id, marker)), Default::default()),
                            false => (None, smallvec![BoundId(id, marker)]),
                        };
                        Self::Shared { listener, bound }
                    }
                };
                Ok(())
            }
            Self::Shared { listener, bound } => {
                if listener == &Some(ListenerId(id, marker)) {
                    match new_sharing {
                        SharingState::Exclusive => {
                            if bound.is_empty() {
                                *self = match new_listening {
                                    true => Self::ExclusiveListener(ListenerId(id, marker)),
                                    false => Self::ExclusiveBound(BoundId(id, marker)),
                                };
                                Ok(())
                            } else {
                                Err(IncompatibleError)
                            }
                        }
                        SharingState::ReuseAddress => match new_listening {
                            true => Ok(()), // no-op
                            false => {
                                bound.push(BoundId(id, marker));
                                *listener = None;
                                Ok(())
                            }
                        },
                    }
                } else {
                    let index = bound
                        .iter()
                        .position(|BoundId(b, _)| *b == id)
                        .expect("ID is neither listener nor bound");
                    if *new_listening && listener.is_some() {
                        return Err(IncompatibleError);
                    }
                    match new_sharing {
                        SharingState::Exclusive => {
                            if bound.len() > 1 {
                                return Err(IncompatibleError);
                            } else {
                                *self = match new_listening {
                                    true => Self::ExclusiveListener(ListenerId(id, marker)),
                                    false => Self::ExclusiveBound(BoundId(id, marker)),
                                };
                                Ok(())
                            }
                        }
                        SharingState::ReuseAddress => {
                            match new_listening {
                                false => Ok(()), // no-op
                                true => {
                                    let _: BoundId<_> = bound.swap_remove(index);
                                    let bound = bound.take();
                                    *self = Self::Shared {
                                        bound,
                                        listener: Some(ListenerId(id, marker)),
                                    };
                                    Ok(())
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub(crate) enum SharingState {
    Exclusive,
    ReuseAddress,
}

impl Default for SharingState {
    fn default() -> Self {
        Self::Exclusive
    }
}

impl<I: IpExt, D: WeakId, C: NonSyncContext>
    SocketMapConflictPolicy<
        ListenerAddr<I::Addr, D, NonZeroU16>,
        ListenerSharingState,
        I,
        D,
        IpPortSpec,
    > for TcpSocketSpec<I, D, C>
{
    fn check_insert_conflicts(
        sharing: &ListenerSharingState,
        addr: &ListenerAddr<I::Addr, D, NonZeroU16>,
        socketmap: &SocketMap<AddrVec<I, D, IpPortSpec>, Bound<Self>>,
    ) -> Result<(), InsertError> {
        let addr = AddrVec::Listen(addr.clone());
        let ListenerSharingState { listening: _, sharing } = sharing;
        // Check if any shadow address is present, specifically, if
        // there is an any-listener with the same port.
        for a in addr.iter_shadows() {
            if let Some(s) = socketmap.get(&a) {
                match s {
                    Bound::Conn(c) => unreachable!("found conn state {c:?} at listener addr {a:?}"),
                    Bound::Listen(l) => match l {
                        ListenerAddrState::ExclusiveListener(_)
                        | ListenerAddrState::ExclusiveBound(_) => {
                            return Err(InsertError::ShadowAddrExists)
                        }
                        ListenerAddrState::Shared { listener, bound: _ } => match sharing {
                            SharingState::Exclusive => return Err(InsertError::ShadowAddrExists),
                            SharingState::ReuseAddress => match listener {
                                Some(_) => return Err(InsertError::ShadowAddrExists),
                                None => (),
                            },
                        },
                    },
                }
            }
        }

        // Check if shadower exists. Note: Listeners do conflict with existing
        // connections, unless the listeners and connections have sharing
        // enabled.
        for (tag, _count) in socketmap.descendant_counts(&addr) {
            let AddrVecTag { sharing: tag_sharing, has_device: _, state: _ } = tag;
            match (tag_sharing, sharing) {
                (SharingState::Exclusive, SharingState::Exclusive | SharingState::ReuseAddress) => {
                    return Err(InsertError::ShadowerExists)
                }
                (SharingState::ReuseAddress, SharingState::Exclusive) => {
                    return Err(InsertError::ShadowerExists)
                }
                (SharingState::ReuseAddress, SharingState::ReuseAddress) => (),
            }
        }
        Ok(())
    }
}

impl<I: IpExt, D: WeakId, C: NonSyncContext>
    SocketMapConflictPolicy<
        ConnAddr<I::Addr, D, NonZeroU16, NonZeroU16>,
        SharingState,
        I,
        D,
        IpPortSpec,
    > for TcpSocketSpec<I, D, C>
{
    fn check_insert_conflicts(
        _sharing: &SharingState,
        addr: &ConnAddr<I::Addr, D, NonZeroU16, NonZeroU16>,
        socketmap: &SocketMap<AddrVec<I, D, IpPortSpec>, Bound<Self>>,
    ) -> Result<(), InsertError> {
        // We need to make sure there are no present sockets that have the same
        // 4-tuple with the to-be-added socket.
        let addr = AddrVec::Conn(ConnAddr { device: None, ..*addr });
        if let Some(_) = socketmap.get(&addr) {
            return Err(InsertError::Exists);
        }
        // No shadower exists, i.e., no sockets with the same 4-tuple but with
        // a device bound.
        if socketmap.descendant_counts(&addr).len() > 0 {
            return Err(InsertError::ShadowerExists);
        }
        // Otherwise, connections don't conflict with existing listeners.
        Ok(())
    }
}

impl<I: Ip, D, LI> Tagged<ListenerAddr<I::Addr, D, LI>> for ListenerAddrState<I> {
    type Tag = AddrVecTag;
    fn tag(&self, address: &ListenerAddr<I::Addr, D, LI>) -> Self::Tag {
        let has_device = address.device.is_some();
        let (sharing, state) = match self {
            ListenerAddrState::ExclusiveBound(_) => {
                (SharingState::Exclusive, SocketTagState::Bound)
            }
            ListenerAddrState::ExclusiveListener(_) => {
                (SharingState::Exclusive, SocketTagState::Listener)
            }
            ListenerAddrState::Shared { listener, bound: _ } => (
                SharingState::ReuseAddress,
                match listener {
                    Some(_) => SocketTagState::Listener,
                    None => SocketTagState::Bound,
                },
            ),
        };
        AddrVecTag { sharing, state, has_device }
    }
}

#[derive(Debug)]
struct ConnAddrState<I: Ip> {
    sharing: SharingState,
    id: MaybeClosedConnectionId<I>,
}

impl<I: Ip> ConnAddrState<I> {
    pub(crate) fn id(&self) -> MaybeClosedConnectionId<I> {
        self.id.clone()
    }
}

impl<I: Ip> SocketMapAddrStateSpec for ConnAddrState<I> {
    type Id = MaybeClosedConnectionId<I>;
    type Inserter<'a> = Never;
    type SharingState = SharingState;

    fn new(new_sharing_state: &Self::SharingState, id: Self::Id) -> Self {
        Self { sharing: *new_sharing_state, id }
    }

    fn contains_id(&self, id: &Self::Id) -> bool {
        &self.id == id
    }

    fn could_insert(
        &self,
        _new_sharing_state: &Self::SharingState,
    ) -> Result<(), IncompatibleError> {
        Err(IncompatibleError)
    }

    fn remove_by_id(&mut self, id: Self::Id) -> RemoveResult {
        let Self { sharing: _, id: existing_id } = self;
        assert_eq!(*existing_id, id);
        return RemoveResult::IsLast;
    }

    fn try_get_inserter<'a, 'b>(
        &'b mut self,
        _new_sharing_state: &'a Self::SharingState,
    ) -> Result<Self::Inserter<'b>, IncompatibleError> {
        Err(IncompatibleError)
    }
}

impl<I: Ip, D, LI, RI> Tagged<ConnAddr<I::Addr, D, LI, RI>> for ConnAddrState<I> {
    type Tag = AddrVecTag;
    fn tag(&self, address: &ConnAddr<I::Addr, D, LI, RI>) -> Self::Tag {
        let Self { sharing, id: _ } = self;
        AddrVecTag {
            sharing: *sharing,
            has_device: address.device.is_some(),
            state: SocketTagState::Conn,
        }
    }
}

#[derive(Debug, Derivative, Clone)]
#[cfg_attr(test, derive(PartialEq))]
struct Unbound<D, Extra> {
    bound_device: Option<D>,
    buffer_sizes: BufferSizes,
    socket_options: SocketOptions,
    sharing: SharingState,
    socket_extra: Extra,
}

/// Holds all the TCP socket states.
pub(crate) struct Sockets<I: IpExt, D: WeakId, C: NonSyncContext> {
    port_alloc: PortAlloc<BoundSocketMap<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>>,
    inactive: IdMap<Unbound<D, C::ListenerNotifierOrProvidedBuffers>>,
    socketmap: BoundSocketMap<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
    bound_state: IdMapCollection<
        BoundSocketId<TcpSocketSpec<I, D, C>>,
        BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
    >,
}

impl<I: IpExt, D: WeakId, C: NonSyncContext> PortAllocImpl
    for BoundSocketMap<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>
{
    const TABLE_SIZE: NonZeroUsize = const_unwrap_option(NonZeroUsize::new(20));
    const EPHEMERAL_RANGE: RangeInclusive<u16> = 49152..=65535;
    type Id = Option<SpecifiedAddr<I::Addr>>;

    fn is_port_available(&self, addr: &Self::Id, port: u16) -> bool {
        // We can safely unwrap here, because the ports received in
        // `is_port_available` are guaranteed to be in `EPHEMERAL_RANGE`.
        let port = NonZeroU16::new(port).unwrap();
        let root_addr = AddrVec::from(ListenerAddr {
            ip: ListenerIpAddr { addr: *addr, identifier: port },
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

impl<I: IpExt, D: WeakId, C: NonSyncContext> Sockets<I, D, C> {
    fn get_listener_by_id_mut(
        &mut self,
        id: ListenerId<I>,
    ) -> Option<&mut Listener<I, C::ReturnedBuffers, C::ListenerNotifierOrProvidedBuffers>> {
        self.bound_state
            .get_mut(&BoundSocketId::Listener(MaybeListenerId::from(id)))
            .map(BoundListener::from_socket_state_mut)
            .map(|(maybe_listener, _sharing, _local_addr)| match maybe_listener {
                MaybeListener::Bound(_) => {
                    unreachable!("contract violated: ListenerId points to an inactive entry")
                }
                MaybeListener::Listener(l) => l,
            })
    }

    pub(crate) fn new(rng: &mut impl RngCore) -> Self {
        Self {
            port_alloc: PortAlloc::new(rng),
            inactive: IdMap::new(),
            socketmap: Default::default(),
            bound_state: Default::default(),
        }
    }
}

/// A link stored in each passively created connections that points back to the
/// parent listener.
///
/// The link is an [`Acceptor::Pending`] iff the acceptee is in the pending
/// state; The link is an [`Acceptor::Ready`] iff the acceptee is ready and has
/// an established connection.
#[derive(Debug, Clone, Copy)]
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
struct Connection<I: IpExt, D: Id, II: Instant, R: ReceiveBuffer, S: SendBuffer, ActiveOpen> {
    acceptor: Option<Acceptor<I>>,
    state: State<II, R, S, ActiveOpen>,
    ip_sock: IpSock<I, D, DefaultSendOptions>,
    /// The user has indicated that this connection will never be used again, we
    /// keep the connection in the socketmap to perform the shutdown but it will
    /// be auto removed once the state reaches Closed.
    defunct: bool,
    socket_options: SocketOptions,
    /// In contrast to a hard error, which will cause a connection to be closed,
    /// a soft error will not abort the connection, but it can be read by either
    /// calling `get_connection_error`, or after the connection times out.
    soft_error: Option<ConnectionError>,
    /// Whether the handshake has finished or aborted.
    handshake_status: HandshakeStatus,
}

/// The Listener state.
///
/// State for sockets that participate in the passive open. Contrary to
/// [`Connection`], only the local address is specified.
#[derive(Debug)]
#[cfg_attr(test, derive(PartialEq, Eq))]
struct Listener<I: Ip, PassiveOpen, Notifier> {
    backlog: NonZeroUsize,
    ready: VecDeque<(ConnectionId<I>, PassiveOpen)>,
    pending: Vec<ConnectionId<I>>,
    buffer_sizes: BufferSizes,
    socket_options: SocketOptions,
    notifier: Notifier,
    // If ip sockets can be half-specified so that only the local address
    // is needed, we can construct an ip socket here to be reused.
}

impl<I: Ip, PassiveOpen, Notifier> Listener<I, PassiveOpen, Notifier> {
    fn new(
        backlog: NonZeroUsize,
        buffer_sizes: BufferSizes,
        socket_options: SocketOptions,
        notifier: Notifier,
    ) -> Self {
        Self {
            backlog,
            ready: VecDeque::new(),
            pending: Vec::new(),
            buffer_sizes,
            socket_options,
            notifier,
        }
    }
}

#[derive(Clone, Debug)]
#[cfg_attr(test, derive(Eq, PartialEq))]
struct BoundState<Extra> {
    buffer_sizes: BufferSizes,
    socket_options: SocketOptions,
    socket_extra: Extra,
}

/// Represents either a bound socket or a listener socket.
#[derive(Debug)]
#[cfg_attr(test, derive(PartialEq))]
enum MaybeListener<I: Ip, PassiveOpen, Extra, Notifier> {
    Bound(BoundState<Extra>),
    Listener(Listener<I, PassiveOpen, Notifier>),
}

impl<
        I: Ip,
        PassiveOpen: core::fmt::Debug,
        Extra: Debug,
        Notifier: Debug + Into<Extra> + Takeable,
    > MaybeListener<I, PassiveOpen, Extra, Notifier>
{
    fn maybe_shutdown(&mut self) -> Option<Listener<I, PassiveOpen, Notifier>> {
        let (buffer_sizes, socket_options, socket_extra) = match self {
            Self::Bound(_) => return None,
            Self::Listener(Listener {
                backlog: _,
                ready: _,
                pending: _,
                buffer_sizes,
                socket_options,
                notifier,
            }) => (buffer_sizes.clone(), socket_options.clone(), notifier.take().into()),
        };
        let replaced = core::mem::replace(
            self,
            Self::Bound(BoundState { buffer_sizes, socket_options, socket_extra }),
        );
        assert_matches!(
            replaced,
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

impl<I: IpExt> BoundId<I> {
    fn get_from_bound_state<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> Option<(
        &MaybeListener<
            I,
            C::ReturnedBuffers,
            C::ListenerNotifierOrProvidedBuffers,
            C::ListenerNotifierOrProvidedBuffers,
        >,
        &ListenerSharingState,
        &ListenerAddr<I::Addr, D, NonZeroU16>,
    )> {
        MaybeListenerId::from(self).get_from_bound_state(bound_state)
    }

    fn get_from_bound_state_mut<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &mut IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> Option<(
        &mut MaybeListener<
            I,
            C::ReturnedBuffers,
            C::ListenerNotifierOrProvidedBuffers,
            C::ListenerNotifierOrProvidedBuffers,
        >,
        &mut ListenerSharingState,
        &mut ListenerAddr<I::Addr, D, NonZeroU16>,
    )> {
        MaybeListenerId::from(self).get_from_bound_state_mut(bound_state)
    }
}

impl<I: IpExt> ListenerId<I> {
    fn get_from_bound_state<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> Option<(
        &MaybeListener<
            I,
            C::ReturnedBuffers,
            C::ListenerNotifierOrProvidedBuffers,
            C::ListenerNotifierOrProvidedBuffers,
        >,
        &ListenerSharingState,
        &ListenerAddr<I::Addr, D, NonZeroU16>,
    )> {
        MaybeListenerId::from(self).get_from_bound_state(bound_state)
    }

    fn get_from_bound_state_mut<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &mut IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> Option<(
        &mut MaybeListener<
            I,
            C::ReturnedBuffers,
            C::ListenerNotifierOrProvidedBuffers,
            C::ListenerNotifierOrProvidedBuffers,
        >,
        &mut ListenerSharingState,
        &mut ListenerAddr<I::Addr, D, NonZeroU16>,
    )> {
        MaybeListenerId::from(self).get_from_bound_state_mut(bound_state)
    }
}

impl<I: IpExt> MaybeListenerId<I> {
    fn get_from_bound_state<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> Option<(
        &MaybeListener<
            I,
            C::ReturnedBuffers,
            C::ListenerNotifierOrProvidedBuffers,
            C::ListenerNotifierOrProvidedBuffers,
        >,
        &ListenerSharingState,
        &ListenerAddr<I::Addr, D, NonZeroU16>,
    )> {
        let state = bound_state.get(&BoundSocketId::Listener(self))?;
        let (listener, sharing, addr) = BoundListener::from_socket_state_ref(state);
        Some((listener, sharing, addr))
    }

    fn get_from_bound_state_mut<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &mut IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> Option<(
        &mut MaybeListener<
            I,
            C::ReturnedBuffers,
            C::ListenerNotifierOrProvidedBuffers,
            C::ListenerNotifierOrProvidedBuffers,
        >,
        &mut ListenerSharingState,
        &mut ListenerAddr<I::Addr, D, NonZeroU16>,
    )> {
        let state = bound_state.get_mut(&BoundSocketId::Listener(self))?;
        let (listener, sharing, addr) = BoundListener::from_socket_state_mut(state);
        Some((listener, sharing, addr))
    }

    fn get_bound_state_entry<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &mut IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> IdMapCollectionOccupied<
        '_,
        BoundSocketId<TcpSocketSpec<I, D, C>>,
        BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
    > {
        match bound_state.entry(BoundSocketId::Listener(self)) {
            IdMapCollectionEntry::Vacant(_) => panic!("invalid MaybeListenerId: not found"),
            IdMapCollectionEntry::Occupied(o) => o,
        }
    }
}

impl<I: IpExt> MaybeClosedConnectionId<I> {
    fn get_from_bound_state<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> Option<(
        &Connection<
            I,
            D,
            C::Instant,
            C::ReceiveBuffer,
            C::SendBuffer,
            C::ListenerNotifierOrProvidedBuffers,
        >,
        &SharingState,
        &ConnAddr<I::Addr, D, NonZeroU16, NonZeroU16>,
    )> {
        let state = bound_state.get(&BoundSocketId::Connection(self))?;
        let (conn, sharing, addr) = BoundConnection::from_socket_state_ref(state);
        Some((conn, sharing, addr))
    }

    fn get_from_bound_state_mut<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &mut IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> Option<(
        &mut Connection<
            I,
            D,
            C::Instant,
            C::ReceiveBuffer,
            C::SendBuffer,
            C::ListenerNotifierOrProvidedBuffers,
        >,
        &mut SharingState,
        &mut ConnAddr<I::Addr, D, NonZeroU16, NonZeroU16>,
    )> {
        let state = bound_state.get_mut(&BoundSocketId::Connection(self))?;
        let (conn, sharing, addr) = BoundConnection::from_socket_state_mut(state);
        Some((conn, sharing, addr))
    }

    fn get_bound_state_entry<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &mut IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> IdMapCollectionOccupied<
        '_,
        BoundSocketId<TcpSocketSpec<I, D, C>>,
        BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
    > {
        match bound_state.entry(BoundSocketId::Connection(self)) {
            IdMapCollectionEntry::Vacant(_) => panic!("invalid ConnectionId: not found"),
            IdMapCollectionEntry::Occupied(o) => o,
        }
    }
}

impl<I: IpExt> ConnectionId<I> {
    fn get_from_bound_state<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> Option<(
        &Connection<
            I,
            D,
            C::Instant,
            C::ReceiveBuffer,
            C::SendBuffer,
            C::ListenerNotifierOrProvidedBuffers,
        >,
        &SharingState,
        &ConnAddr<I::Addr, D, NonZeroU16, NonZeroU16>,
    )> {
        MaybeClosedConnectionId::from(self).get_from_bound_state(bound_state).map(
            |(conn, sharing, addr)| {
                assert!(!conn.defunct, "invalid ConnectionId: already defunct");
                (conn, sharing, addr)
            },
        )
    }

    fn get_from_bound_state_mut<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &mut IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> Option<(
        &mut Connection<
            I,
            D,
            C::Instant,
            C::ReceiveBuffer,
            C::SendBuffer,
            C::ListenerNotifierOrProvidedBuffers,
        >,
        &mut SharingState,
        &mut ConnAddr<I::Addr, D, NonZeroU16, NonZeroU16>,
    )> {
        MaybeClosedConnectionId::from(self).get_from_bound_state_mut(bound_state).map(
            |(conn, sharing, addr)| {
                assert!(!conn.defunct, "invalid ConnectionId: already defunct");
                (conn, sharing, addr)
            },
        )
    }

    fn get_bound_state_entry<D: WeakId, C: NonSyncContext>(
        self,
        bound_state: &mut IdMapCollection<
            BoundSocketId<TcpSocketSpec<I, D, C>>,
            BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
        >,
    ) -> IdMapCollectionOccupied<
        '_,
        BoundSocketId<TcpSocketSpec<I, D, C>>,
        BoundSocketState<I, D, IpPortSpec, TcpSocketSpec<I, D, C>>,
    > {
        MaybeClosedConnectionId::from(self).get_bound_state_entry(bound_state)
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

/// The status of a handshake.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum HandshakeStatus {
    /// The handshake is still pending.
    Pending,
    /// The handshake is aborted.
    Aborted,
    /// The handshake is completed.
    Completed {
        /// Whether it has been reported to the user yet.
        reported: bool,
    },
}

impl HandshakeStatus {
    fn update_if_pending(&mut self, new_status: Self) {
        if *self == HandshakeStatus::Pending {
            *self = new_status;
        }
    }
}
pub(crate) trait SocketHandler<I: Ip, C: NonSyncContext>:
    DeviceIdContext<AnyDevice>
{
    fn create_socket(
        &mut self,
        ctx: &mut C,
        socket_extra: C::ListenerNotifierOrProvidedBuffers,
    ) -> UnboundId<I>;

    fn bind(
        &mut self,
        ctx: &mut C,
        id: UnboundId<I>,
        local_ip: Option<ZonedAddr<I::Addr, Self::DeviceId>>,
        port: Option<NonZeroU16>,
    ) -> Result<BoundId<I>, LocalAddressError>;

    fn listen(
        &mut self,
        id: BoundId<I>,
        backlog: NonZeroUsize,
    ) -> Result<ListenerId<I>, ListenError>;

    fn accept(
        &mut self,
        _ctx: &mut C,
        id: ListenerId<I>,
    ) -> Result<
        (ConnectionId<I>, SocketAddr<I::Addr, Self::WeakDeviceId>, C::ReturnedBuffers),
        AcceptError,
    >;

    fn connect_bound(
        &mut self,
        ctx: &mut C,
        id: BoundId<I>,
        remote: SocketAddr<I::Addr, Self::DeviceId>,
    ) -> Result<ConnectionId<I>, ConnectError>;

    fn connect_unbound(
        &mut self,
        ctx: &mut C,
        id: UnboundId<I>,
        remote_ip: ZonedAddr<I::Addr, Self::DeviceId>,
        remote_port: NonZeroU16,
    ) -> Result<ConnectionId<I>, ConnectError>;

    fn get_handshake_status(&mut self, id: ConnectionId<I>) -> HandshakeStatus;

    fn shutdown_conn(&mut self, ctx: &mut C, id: ConnectionId<I>) -> Result<(), NoConnection>;
    fn close_conn(&mut self, ctx: &mut C, id: ConnectionId<I>);
    fn remove_unbound(&mut self, id: UnboundId<I>);
    fn remove_bound(&mut self, id: BoundId<I>);
    fn shutdown_listener(&mut self, ctx: &mut C, id: ListenerId<I>) -> BoundId<I>;

    fn get_unbound_info(&mut self, id: UnboundId<I>) -> UnboundInfo<Self::WeakDeviceId>;
    fn get_bound_info(&mut self, id: BoundId<I>) -> BoundInfo<I::Addr, Self::WeakDeviceId>;
    fn get_listener_info(&mut self, id: ListenerId<I>) -> BoundInfo<I::Addr, Self::WeakDeviceId>;
    fn get_connection_info(
        &mut self,
        id: ConnectionId<I>,
    ) -> ConnectionInfo<I::Addr, Self::WeakDeviceId>;
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
    fn with_socket_options_mut<R, F: FnOnce(&mut SocketOptions) -> R, Id: Into<SocketId<I>>>(
        &mut self,
        ctx: &mut C,
        id: Id,
        f: F,
    ) -> R;
    fn with_socket_options<R, F: FnOnce(&SocketOptions) -> R, Id: Into<SocketId<I>>>(
        &mut self,
        id: Id,
        f: F,
    ) -> R;

    fn set_send_buffer_size<Id: Into<SocketId<I>>>(&mut self, ctx: &mut C, id: Id, size: usize);
    fn send_buffer_size<Id: Into<SocketId<I>>>(&mut self, ctx: &mut C, id: Id) -> Option<usize>;
    fn set_receive_buffer_size<Id: Into<SocketId<I>>>(&mut self, ctx: &mut C, id: Id, size: usize);
    fn receive_buffer_size<Id: Into<SocketId<I>>>(&mut self, ctx: &mut C, id: Id) -> Option<usize>;

    fn set_reuseaddr_unbound(&mut self, id: UnboundId<I>, reuse: bool);
    fn set_reuseaddr_bound(&mut self, id: BoundId<I>, reuse: bool)
        -> Result<(), SetReuseAddrError>;
    fn set_reuseaddr_listener(
        &mut self,
        id: ListenerId<I>,
        reuse: bool,
    ) -> Result<(), SetReuseAddrError>;
    fn reuseaddr(&mut self, id: SocketId<I>) -> bool;

    /// Receives an ICMP error from the IP layer.
    fn on_icmp_error(
        &mut self,
        ctx: &mut C,
        orig_src_ip: SpecifiedAddr<I::Addr>,
        orig_dst_ip: SpecifiedAddr<I::Addr>,
        orig_src_port: NonZeroU16,
        orig_dst_port: NonZeroU16,
        seq: SeqNum,
        error: IcmpErrorCode,
    );

    fn get_connection_error(&mut self, conn_id: ConnectionId<I>) -> Option<ConnectionError>;
    fn with_info<V: InfoVisitor>(&mut self, cb: V) -> V::VisitResult;
}

impl<I: IpLayerIpExt, C: NonSyncContext, SC: SyncContext<I, C>> SocketHandler<I, C> for SC {
    fn create_socket(
        &mut self,
        _ctx: &mut C,
        socket_extra: C::ListenerNotifierOrProvidedBuffers,
    ) -> UnboundId<I> {
        let unbound = Unbound {
            buffer_sizes: C::default_buffer_sizes(),
            bound_device: Default::default(),
            sharing: Default::default(),
            socket_options: Default::default(),
            socket_extra,
        };
        UnboundId(
            self.with_tcp_sockets_mut(move |sockets| sockets.inactive.push(unbound)),
            IpVersionMarker::default(),
        )
    }

    fn bind(
        &mut self,
        _ctx: &mut C,
        id: UnboundId<I>,
        addr: Option<ZonedAddr<I::Addr, Self::DeviceId>>,
        port: Option<NonZeroU16>,
    ) -> Result<BoundId<I>, LocalAddressError> {
        // TODO(https://fxbug.dev/104300): Check if local_ip is a unicast address.
        self.with_ip_transport_ctx_and_tcp_sockets_mut(
            |ip_transport_ctx, Sockets { bound_state: socket_bound_state, port_alloc, inactive, socketmap }| {
                let port = match port {
                    None => {
                        let addr = addr.as_ref().map(ZonedAddr::addr);
                        match port_alloc.try_alloc(&addr, &socketmap) {
                            Some(port) => {
                                NonZeroU16::new(port).expect("ephemeral ports must be non-zero")
                            }
                            None => return Err(LocalAddressError::FailedToAllocateLocalPort),
                        }
                    }
                    Some(port) => port,
                };
                debug!("bind {id:?} to {addr:?}:{}", port.get());
                let mut inactive_entry = match inactive.entry(id.into()) {
                    IdMapEntry::Vacant(_) => panic!("invalid unbound ID"),
                    IdMapEntry::Occupied(o) => o,
                };

                let Unbound { bound_device, buffer_sizes, socket_options, sharing, socket_extra, } =
                    inactive_entry.get_mut();
                let bound_state = BoundState {
                    buffer_sizes: buffer_sizes.clone(),
                    socket_options: socket_options.clone(),
                    socket_extra: socket_extra.take(),
                };

                let (local_ip, device) = match addr {
                    Some(addr) => {
                        // Extract the specified address and the device. The
                        // device is either the one from the address or the one
                        // to which the socket was previously bound.
                        let (addr, required_device) =
                            crate::transport::resolve_addr_with_device(addr, bound_device.clone())?;

                        let mut assigned_to = ip_transport_ctx.get_devices_with_assigned_addr(addr);
                        if !assigned_to.any(|d| {
                            required_device
                                .as_ref()
                                .map_or(true, |device| device == &EitherDeviceId::Strong(d))
                        }) {
                            return Err(LocalAddressError::AddressMismatch);
                        }

                        (Some(addr), required_device)
                    }
                    None => (None, bound_device.clone().map(EitherDeviceId::Weak)),
                };

                let bound = socketmap
                    .listeners_mut()
                    .try_insert(
                        ListenerAddr {
                            ip: ListenerIpAddr { addr: local_ip, identifier: port },
                            device: device.map(|d| d.as_weak(ip_transport_ctx).into_owned()),
                        },
                        ListenerSharingState { sharing: *sharing, listening: false },
                        |addr, sharing| {
                            let entry = socket_bound_state.push_entry(
                                |index| <BoundListener as ConvertSocketMapState<I, SC::WeakDeviceId, IpPortSpec, _>>::to_socket_id(index.into()),
                                BoundListener::to_socket_state((
                                    MaybeListener::Bound(bound_state),
                                    sharing,
                                    addr,
                                )),
                            );
                            <BoundListener as ConvertSocketMapState<I, SC::WeakDeviceId, IpPortSpec, _>>::from_socket_id_ref(entry.key()).clone()
                        },
                    )
                    .map(|entry| {
                        let MaybeListenerId(x, marker) = entry.id();
                        BoundId(x, marker)
                    })
                    .map_err(|_: (InsertError, ListenerSharingState)| {
                        LocalAddressError::AddressInUse
                    })?;
                let _: Unbound<_, _> = inactive_entry.remove();
                Ok(bound)
            },
        )
    }

    fn listen(
        &mut self,
        id: BoundId<I>,
        backlog: NonZeroUsize,
    ) -> Result<ListenerId<I>, ListenError> {
        self.with_tcp_sockets_mut(|sockets| {
            debug!("listen on {id:?} with backlog {backlog}");
            let (listener, listener_sharing, addr) =
                id.get_from_bound_state_mut(&mut sockets.bound_state).expect("missing listener");
            let id = id.into();
            let entry =
                sockets.socketmap.listeners_mut().entry(&id, &addr).expect("invalid listener id");
            let ListenerSharingState { sharing, listening } = listener_sharing;
            debug_assert!(!*listening, "invalid bound ID that has a listener socket");
            let sharing = *sharing;

            let new_sharing = ListenerSharingState { sharing, listening: true };
            match entry.try_update_sharing(&listener_sharing, new_sharing.clone()) {
                Ok(()) => {
                    *listener_sharing = new_sharing;
                }
                Err(UpdateSharingError) => return Err(ListenError::ListenerExists),
            };

            match listener {
                MaybeListener::Bound(BoundState { buffer_sizes, socket_options, socket_extra }) => {
                    *listener = MaybeListener::Listener(Listener::new(
                        backlog,
                        buffer_sizes.clone(),
                        socket_options.clone(),
                        socket_extra.clone(),
                    ));
                }
                MaybeListener::Listener(_) => {
                    unreachable!("invalid bound id that points to a listener entry")
                }
            }
            let MaybeListenerId(index, _marker) = id;
            Ok(ListenerId(index, IpVersionMarker::default()))
        })
    }

    fn accept(
        &mut self,
        _ctx: &mut C,
        id: ListenerId<I>,
    ) -> Result<
        (ConnectionId<I>, SocketAddr<I::Addr, Self::WeakDeviceId>, C::ReturnedBuffers),
        AcceptError,
    > {
        self.with_tcp_sockets_mut(|sockets| {
            debug!("accept on {id:?}");
            let Listener {
                ready,
                backlog: _,
                buffer_sizes: _,
                pending: _,
                socket_options: _,
                notifier,
            } = sockets.get_listener_by_id_mut(id).expect("invalid listener id");
            let (conn_id, client_buffers) = ready.pop_front().ok_or(AcceptError::WouldBlock)?;
            notifier.new_incoming_connections(ready.len());

            let (conn, _, conn_addr): (_, &mut SharingState, _) = conn_id
                .get_from_bound_state_mut(&mut sockets.bound_state)
                .expect("invalid connection");
            conn.acceptor = None;
            let ConnAddr { ip: ConnIpAddr { local: _, remote }, device } = conn_addr;
            let (remote_ip, remote_port) = *remote;

            Ok((
                conn_id,
                SocketAddr { ip: maybe_zoned(remote_ip, device), port: remote_port },
                client_buffers,
            ))
        })
    }

    fn connect_bound(
        &mut self,
        ctx: &mut C,
        id: BoundId<I>,
        remote: SocketAddr<I::Addr, Self::DeviceId>,
    ) -> Result<ConnectionId<I>, ConnectError> {
        let bound_id = MaybeListenerId::from(id);
        let SocketAddr { ip: remote_ip, port: remote_port } = remote;
        self.with_ip_transport_ctx_isn_generator_and_tcp_sockets_mut(
            |ip_transport_ctx, isn, sockets| {
                debug!("connect on {id:?} to {remote_ip:?}:{remote_port}");
                let Sockets { bound_state, socketmap, port_alloc: _, inactive: _ } = sockets;
                let (bound, sharing, bound_addr) =
                    bound_id.get_from_bound_state(bound_state).expect("invalid bound");
                let bound = assert_matches!(bound, MaybeListener::Bound(b) => b);
                let BoundState { buffer_sizes, socket_options, socket_extra } = bound.clone();
                let ListenerAddr { ip, device: bound_device } = bound_addr;
                let bound_addr = bound_addr.clone();
                let ListenerIpAddr { addr: local_ip, identifier: local_port } = *ip;

                let (remote_ip, device) =
                    crate::transport::resolve_addr_with_device(remote_ip, bound_device.clone())?;

                let ip_sock = ip_transport_ctx
                    .new_ip_socket(
                        ctx,
                        device.as_ref().map(|d| d.as_ref()),
                        local_ip,
                        remote_ip,
                        IpProto::Tcp.into(),
                        DefaultSendOptions,
                    )
                    .map_err(|(err, DefaultSendOptions {})| match err {
                        IpSockCreationError::Route(_) => ConnectError::NoRoute,
                    })?;

                let mms = ip_transport_ctx.get_mms(ctx, &ip_sock).map_err(
                    |_err: crate::ip::socket::MmsError| {
                        // We either cannot find the route, or the device for
                        // the route cannot handle the smallest TCP/IP packet.
                        ConnectError::NoRoute
                    },
                )?;

                let ListenerSharingState { sharing, listening: _ } = *sharing;
                let buffer_sizes = buffer_sizes.clone();
                let socket_options = socket_options.clone();
                let make_connection = move |addr, state| {
                    let _ = bound_state
                        .remove(&crate::socket::SocketId::Listener(bound_id))
                        .expect("already looked up listener");
                    let entry = bound_state.push_entry(
                        |index| crate::socket::SocketId::Connection(index.into()),
                        BoundConnection::to_socket_state((state, sharing, addr)),
                    );
                    let MaybeClosedConnectionId(index, marker) =
                        <BoundConnection as ConvertSocketMapState<
                            I,
                            Self::WeakDeviceId,
                            IpPortSpec,
                            _,
                        >>::from_socket_id_ref(entry.key())
                        .clone();
                    ConnectionId(index, marker)
                };
                let conn_id = connect_inner(
                    isn,
                    socketmap,
                    make_connection,
                    ip_transport_ctx,
                    ctx,
                    ip_sock,
                    local_port,
                    remote_port,
                    socket_extra,
                    buffer_sizes,
                    socket_options,
                    sharing,
                    mms,
                )?;
                sockets
                    .socketmap
                    .listeners_mut()
                    .remove(&bound_id, &bound_addr)
                    .expect("was present");
                Ok(conn_id)
            },
        )
    }

    fn connect_unbound(
        &mut self,
        ctx: &mut C,
        id: UnboundId<I>,
        remote_ip: ZonedAddr<I::Addr, Self::DeviceId>,
        remote_port: NonZeroU16,
    ) -> Result<ConnectionId<I>, ConnectError> {
        self.with_ip_transport_ctx_isn_generator_and_tcp_sockets_mut(
            |ip_transport_ctx, isn, sockets| {
                debug!("connect on {id:?} to {remote_ip:?}:{remote_port}");
                let inactive = match sockets.inactive.entry(id.into()) {
                    id_map::Entry::Vacant(_) => panic!("invalid unbound ID {:?}", id),
                    id_map::Entry::Occupied(o) => o,
                };
                let Unbound {
                    bound_device,
                    buffer_sizes: _,
                    socket_options: _,
                    sharing: _,
                    socket_extra: _,
                } = inactive.get();

                let (remote_ip, device) =
                    crate::transport::resolve_addr_with_device(remote_ip, bound_device.clone())?;

                let ip_sock = ip_transport_ctx
                    .new_ip_socket(
                        ctx,
                        device.as_ref().map(|d| d.as_ref()),
                        None,
                        remote_ip,
                        IpProto::Tcp.into(),
                        DefaultSendOptions,
                    )
                    .map_err(|(err, DefaultSendOptions)| match err {
                        IpSockCreationError::Route(_) => ConnectError::NoRoute,
                    })?;

                let local_port = match sockets
                    .port_alloc
                    .try_alloc(&Some(*ip_sock.local_ip()), &sockets.socketmap)
                {
                    Some(port) => NonZeroU16::new(port).expect("ephemeral ports must be non-zero"),
                    None => return Err(ConnectError::NoPort),
                };

                let Unbound {
                    buffer_sizes,
                    bound_device: _,
                    socket_options,
                    sharing,
                    socket_extra,
                } = inactive.get();

                let mms = ip_transport_ctx
                    .get_mms(ctx, &ip_sock)
                    .map_err(|_err: crate::ip::socket::MmsError| ConnectError::NoRoute)?;

                let make_connection = |addr, state| {
                    let entry = sockets.bound_state.push_entry(
                        |index| crate::socket::SocketId::Connection(index.into()),
                        BoundConnection::to_socket_state((state, *sharing, addr)),
                    );
                    let MaybeClosedConnectionId(index, marker) =
                        <BoundConnection as ConvertSocketMapState<
                            I,
                            Self::WeakDeviceId,
                            IpPortSpec,
                            _,
                        >>::from_socket_id_ref(entry.key())
                        .clone();
                    ConnectionId(index, marker)
                };
                let conn_id = connect_inner(
                    isn,
                    &mut sockets.socketmap,
                    make_connection,
                    ip_transport_ctx,
                    ctx,
                    ip_sock,
                    local_port,
                    remote_port,
                    socket_extra.clone(),
                    buffer_sizes.clone(),
                    socket_options.clone(),
                    *sharing,
                    mms,
                )?;
                let _: Unbound<_, _> = inactive.remove();
                Ok(conn_id)
            },
        )
    }

    fn get_handshake_status(&mut self, id: ConnectionId<I>) -> HandshakeStatus {
        self.with_tcp_sockets_mut(|sockets| {
            let (conn, _sharing, _addr) =
                id.get_from_bound_state_mut(&mut sockets.bound_state).expect("invalid conn ID");
            let status = conn.handshake_status;
            if let HandshakeStatus::Completed { reported } = &mut conn.handshake_status {
                *reported = true;
            }
            status
        })
    }

    fn shutdown_conn(&mut self, ctx: &mut C, id: ConnectionId<I>) -> Result<(), NoConnection> {
        self.with_ip_transport_ctx_and_tcp_sockets_mut(|ip_transport_ctx, sockets| {
            debug!("shutdown on {id:?}");
            let (conn, _, addr): (_, &mut SharingState, _) =
                id.get_from_bound_state_mut(&mut sockets.bound_state).expect("invalid conn ID");
            match conn.state.close(CloseReason::Shutdown, &conn.socket_options) {
                Ok(()) => Ok(do_send_inner(id.into(), conn, addr, ip_transport_ctx, ctx)),
                Err(CloseError::NoConnection) => Err(NoConnection),
                Err(CloseError::Closing) => Ok(()),
            }
        })
    }

    fn close_conn(&mut self, ctx: &mut C, id: ConnectionId<I>) {
        self.with_ip_transport_ctx_and_tcp_sockets_mut(|ip_transport_ctx, sockets| {
            debug!("close on {id:?}");
            let mut entry = id.get_bound_state_entry(&mut sockets.bound_state);
            let (conn, _, addr): &mut (_, SharingState, _) =
                BoundConnection::from_socket_state_mut(entry.get_mut());
            conn.defunct = true;
            let already_closed =
                match conn.state.close(CloseReason::Close { now: ctx.now() }, &conn.socket_options)
                {
                    Err(CloseError::NoConnection) => true,
                    Err(CloseError::Closing) => false,
                    Ok(()) => matches!(conn.state, State::Closed(_)),
                };
            if already_closed {
                let id = id.into();
                assert_matches!(sockets.socketmap.conns_mut().remove(&id, &addr), Ok(()));
                let (_state, _sharing, _addr) = BoundConnection::from_socket_state(entry.remove());
                let _: Option<_> = ctx.cancel_timer(TimerId::new::<I>(id));
                return;
            }
            do_send_inner(id.into(), conn, &addr, ip_transport_ctx, ctx)
        })
    }

    fn remove_unbound(&mut self, id: UnboundId<I>) {
        self.with_tcp_sockets_mut(
            |Sockets { bound_state: _, socketmap: _, inactive, port_alloc: _ }| {
                assert_matches!(inactive.remove(id.into()), Some(_));
            },
        );
    }

    fn remove_bound(&mut self, id: BoundId<I>) {
        let id: MaybeListenerId<I> = id.into();
        self.with_tcp_sockets_mut(
            |Sockets { bound_state, socketmap, inactive: _, port_alloc: _ }| {
                let (_state, _sharing, addr) = BoundListener::from_socket_state(
                    id.get_bound_state_entry(bound_state).remove(),
                );
                assert_matches!(socketmap.listeners_mut().remove(&id, &addr), Ok(()));
            },
        );
    }

    fn shutdown_listener(&mut self, ctx: &mut C, id: ListenerId<I>) -> BoundId<I> {
        self.with_ip_transport_ctx_and_tcp_sockets_mut(
            |ip_transport_ctx, Sockets { bound_state, socketmap, inactive: _, port_alloc: _ }| {
                debug!("shutdown on {id:?}");
                let listener_state =
                    id.get_from_bound_state_mut(bound_state).expect("missing listener state");
                let (maybe_listener, sharing, addr) = listener_state;
                let entry =
                    socketmap.listeners_mut().entry(&id.into(), addr).expect("invalid listener ID");
                let ListenerSharingState { sharing: _, listening } = sharing;
                assert!(*listening, "listener {id:?} is not listening");
                let new_sharing = ListenerSharingState { listening: false, ..sharing.clone() };
                match entry.try_update_sharing(sharing, new_sharing.clone()) {
                    Ok(()) => (),
                    Err(e) => {
                        unreachable!(
                            "downgrading a TCP listener to bound should not fail, got {e:?}"
                        )
                    }
                };

                let Listener {
                    backlog: _,
                    pending,
                    ready,
                    buffer_sizes: _,
                    socket_options: _,
                    notifier: _,
                } = maybe_listener.maybe_shutdown().expect("must be a listener");
                *sharing = new_sharing;

                for conn_id in pending.into_iter().chain(
                    ready
                        .into_iter()
                        .map(|(conn_id, _passive_open): (_, C::ReturnedBuffers)| conn_id),
                ) {
                    let _: Option<C::Instant> = ctx.cancel_timer(TimerId::new::<I>(conn_id.into()));
                    let (mut conn, _sharing, conn_addr) = BoundConnection::from_socket_state(
                        conn_id.get_bound_state_entry(bound_state).remove(),
                    );
                    assert_matches!(
                        socketmap.conns_mut().remove(&conn_id.into(), &conn_addr),
                        Ok(())
                    );
                    if let Some(reset) = conn.state.abort() {
                        let ConnAddr { ip, device: _ } = conn_addr;
                        let ser = tcp_serialize_segment(reset, ip);
                        ip_transport_ctx
                            .send_ip_packet(ctx, &conn.ip_sock, ser, None)
                            .unwrap_or_else(|(body, err)| {
                                debug!(
                                    "failed to reset connection to {:?}, body: {:?}, err: {:?}",
                                    ip, body, err
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
        self.with_ip_transport_ctx_and_tcp_sockets_mut(|sync_ctx, sockets| {
            debug!("set device on {id:?} to {device:?}");
            let Sockets { bound_state: _, inactive, port_alloc: _, socketmap: _ } = sockets;
            let Unbound {
                bound_device,
                buffer_sizes: _,
                socket_options: _,
                sharing: _,
                socket_extra: _,
            } = inactive.get_mut(id.into()).expect("invalid unbound socket ID");
            *bound_device = device.map(|d| sync_ctx.downgrade_device_id(&d));
        })
    }

    fn set_bound_device(
        &mut self,
        _ctx: &mut C,
        id: impl Into<MaybeListenerId<I>>,
        new_device: Option<Self::DeviceId>,
    ) -> Result<(), SetDeviceError> {
        let id = id.into();
        self.with_ip_transport_ctx_and_tcp_sockets_mut(|sync_ctx, sockets| {
            debug!("set device on {id:?} to {new_device:?}");
            let Sockets { bound_state, socketmap, inactive: _, port_alloc: _ } = sockets;
            let bound_state =
                id.get_from_bound_state_mut(bound_state).expect("missing bound state");
            let (_state, _sharing, addr) = bound_state;
            let entry = socketmap.listeners_mut().entry(&id, addr).expect("invalid ID");
            let ListenerAddr { device: old_device, ip: ip_addr } = addr;
            let ListenerIpAddr { identifier: _, addr: ip } = ip_addr;

            if !crate::socket::can_device_change(
                ip.as_ref(), /* local_ip */
                None,        /* remote_ip */
                old_device.as_ref(),
                new_device.as_ref(),
            ) {
                return Err(SetDeviceError::ZoneChange);
            }

            let ip = *ip_addr;
            match entry.try_update_addr(ListenerAddr {
                device: new_device.map(|d| sync_ctx.downgrade_device_id(&d)),
                ip,
            }) {
                Ok(entry) => {
                    *addr = entry.get_addr().clone();
                    Ok(())
                }
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
            |ip_transport_ctx, Sockets { bound_state, socketmap, inactive: _, port_alloc: _ }| {
                debug!("set device on {id:?} to {new_device:?}");
                let bound_state =
                    id.get_from_bound_state_mut(bound_state).expect("invalid conn state");
                let (connection, _sharing, addr) = bound_state;
                let entry = socketmap.conns_mut().entry(&id.into(), addr).expect("invalid conn ID");
                let ConnAddr {
                    device: old_device,
                    ip: ConnIpAddr { local: (local_ip, _), remote: (remote_ip, _) },
                } = addr;

                if !crate::socket::can_device_change(
                    Some(local_ip),
                    Some(remote_ip),
                    old_device.as_ref(),
                    new_device.as_ref(),
                ) {
                    return Err(SetDeviceError::ZoneChange);
                }

                let new_socket = ip_transport_ctx
                    .new_ip_socket(
                        ctx,
                        new_device.as_ref().map(EitherDeviceId::Strong),
                        Some(*local_ip),
                        *remote_ip,
                        IpProto::Tcp.into(),
                        Default::default(),
                    )
                    .map_err(|_: (IpSockCreationError, DefaultSendOptions)| {
                        SetDeviceError::Unroutable
                    })?;

                match entry.try_update_addr(ConnAddr {
                    device: new_socket.device().cloned(),
                    ..addr.clone()
                }) {
                    Ok(entry) => {
                        *addr = entry.get_addr().clone();
                        let Connection {
                            ip_sock,
                            acceptor: _,
                            state: _,
                            defunct: _,
                            socket_options: _,
                            soft_error: _,
                            handshake_status: _,
                        } = connection;
                        *ip_sock = new_socket;
                        Ok(())
                    }
                    Err((ExistsError, _entry)) => Err(SetDeviceError::Conflict),
                }
            },
        )
    }

    fn get_unbound_info(&mut self, id: UnboundId<I>) -> UnboundInfo<SC::WeakDeviceId> {
        self.with_tcp_sockets(|sockets| {
            let Sockets { bound_state: _, socketmap: _, inactive, port_alloc: _ } = sockets;
            inactive.get(id.into()).expect("invalid unbound ID").into()
        })
    }

    fn get_bound_info(&mut self, id: BoundId<I>) -> BoundInfo<I::Addr, SC::WeakDeviceId> {
        self.with_tcp_sockets(|sockets| {
            let (bound, _, bound_addr): (_, &ListenerSharingState, _) =
                id.get_from_bound_state(&sockets.bound_state).expect("invalid bound");
            assert_matches!(bound, MaybeListener::Bound(_));
            bound_addr.clone()
        })
        .into()
    }

    fn get_listener_info(&mut self, id: ListenerId<I>) -> BoundInfo<I::Addr, SC::WeakDeviceId> {
        self.with_tcp_sockets(|sockets| {
            let (listener, _, addr): (_, &ListenerSharingState, _) =
                id.get_from_bound_state(&sockets.bound_state).expect("invalid listener");
            assert_matches!(listener, MaybeListener::Listener(_));
            addr.clone()
        })
        .into()
    }

    fn get_connection_info(
        &mut self,
        id: ConnectionId<I>,
    ) -> ConnectionInfo<I::Addr, SC::WeakDeviceId> {
        self.with_tcp_sockets(|sockets| {
            let (_conn, _, addr): (_, &SharingState, _) =
                id.get_from_bound_state(&sockets.bound_state).expect("invalid connection");
            addr.clone()
        })
        .into()
    }

    fn do_send(&mut self, ctx: &mut C, conn_id: MaybeClosedConnectionId<I>) {
        self.with_ip_transport_ctx_and_tcp_sockets_mut(|ip_transport_ctx, sockets| {
            if let Some((conn, sharing, addr)) =
                conn_id.get_from_bound_state_mut(&mut sockets.bound_state)
            {
                let _: &SharingState = sharing;
                do_send_inner(conn_id, conn, addr, ip_transport_ctx, ctx);
            }
        })
    }

    fn handle_timer(&mut self, ctx: &mut C, conn_id: MaybeClosedConnectionId<I>) {
        self.with_ip_transport_ctx_and_tcp_sockets_mut(|ip_transport_ctx, sockets| {
            let mut entry = conn_id.get_bound_state_entry(&mut sockets.bound_state);
            let (conn, _sharing, addr) = BoundConnection::from_socket_state_mut(entry.get_mut());
            do_send_inner(conn_id, conn, addr, ip_transport_ctx, ctx);
            if conn.defunct && matches!(conn.state, State::Closed(_)) {
                assert_matches!(sockets.socketmap.conns_mut().remove(&conn_id, addr), Ok(()));
                let _ = entry.remove();
                let _: Option<_> = ctx.cancel_timer(TimerId::new::<I>(conn_id));
            }
        })
    }

    fn with_socket_options_mut<R, F: FnOnce(&mut SocketOptions) -> R, Id: Into<SocketId<I>>>(
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
                        .socket_options);
                }
                SocketId::Bound(bound_id) => bound_id.into(),
                SocketId::Listener(listener_id) => listener_id.into(),
                SocketId::Connection(conn_id) => {
                    let (conn, _, addr): (_, &mut SharingState, _) = conn_id
                        .get_from_bound_state_mut(&mut sockets.bound_state)
                        .expect("invalid conn ID");
                    let old = conn.socket_options;
                    let result = f(&mut conn.socket_options);
                    if old != conn.socket_options {
                        do_send_inner(conn_id.into(), conn, addr, ip_transport_ctx, ctx);
                    }
                    return result;
                }
            };
            let (maybe_listener, _, _bound_addr): (_, &mut ListenerSharingState, _) =
                maybe_listener_id
                    .get_from_bound_state_mut(&mut sockets.bound_state)
                    .expect("invalid ID");
            match maybe_listener {
                MaybeListener::Bound(bound) => f(&mut bound.socket_options),
                MaybeListener::Listener(listener) => f(&mut listener.socket_options),
            }
        })
    }

    fn with_socket_options<R, F: FnOnce(&SocketOptions) -> R, Id: Into<SocketId<I>>>(
        &mut self,
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
                        .socket_options);
                }
                SocketId::Bound(bound_id) => bound_id.into(),
                SocketId::Listener(listener_id) => listener_id.into(),
                SocketId::Connection(conn_id) => {
                    let (conn, _, _addr): (_, &SharingState, _) = conn_id
                        .get_from_bound_state(&sockets.bound_state)
                        .expect("invalid conn ID");
                    return f(&conn.socket_options);
                }
            };
            let (maybe_listener, _, _bound_addr): (_, &ListenerSharingState, _) = maybe_listener_id
                .get_from_bound_state(&sockets.bound_state)
                .expect("invalid listener ID");
            match maybe_listener {
                MaybeListener::Bound(bound) => f(&bound.socket_options),
                MaybeListener::Listener(listener) => f(&listener.socket_options),
            }
        })
    }

    fn set_send_buffer_size<Id: Into<SocketId<I>>>(&mut self, _ctx: &mut C, id: Id, size: usize) {
        set_buffer_size::<SendBufferSize, I, C, SC>(self, id.into(), size)
    }

    fn send_buffer_size<Id: Into<SocketId<I>>>(&mut self, _ctx: &mut C, id: Id) -> Option<usize> {
        get_buffer_size::<SendBufferSize, I, C, SC>(self, id.into())
    }

    fn set_receive_buffer_size<Id: Into<SocketId<I>>>(
        &mut self,
        _ctx: &mut C,
        id: Id,
        size: usize,
    ) {
        set_buffer_size::<ReceiveBufferSize, I, C, SC>(self, id.into(), size)
    }

    fn receive_buffer_size<Id: Into<SocketId<I>>>(
        &mut self,
        _ctx: &mut C,
        id: Id,
    ) -> Option<usize> {
        get_buffer_size::<ReceiveBufferSize, I, C, SC>(self, id.into())
    }

    fn set_reuseaddr_unbound(&mut self, id: UnboundId<I>, reuse: bool) {
        self.with_tcp_sockets_mut(|sockets| {
            let Sockets { bound_state: _, port_alloc: _, inactive, socketmap: _ } = sockets;
            let Unbound {
                sharing,
                bound_device: _,
                buffer_sizes: _,
                socket_options: _,
                socket_extra: _,
            } = inactive.get_mut(id.into()).expect("invalid socket ID");
            *sharing = match reuse {
                true => SharingState::ReuseAddress,
                false => SharingState::Exclusive,
            };
        })
    }

    fn set_reuseaddr_bound(
        &mut self,
        id: BoundId<I>,
        reuse: bool,
    ) -> Result<(), SetReuseAddrError> {
        set_reuseaddr_maybe_listener(false, self, id.into(), reuse)
    }

    fn set_reuseaddr_listener(
        &mut self,
        id: ListenerId<I>,
        reuse: bool,
    ) -> Result<(), SetReuseAddrError> {
        set_reuseaddr_maybe_listener(true, self, id.into(), reuse)
    }

    fn reuseaddr(&mut self, id: SocketId<I>) -> bool {
        get_reuseaddr(self, id)
    }

    fn on_icmp_error(
        &mut self,
        _ctx: &mut C,
        orig_src_ip: SpecifiedAddr<I::Addr>,
        orig_dst_ip: SpecifiedAddr<I::Addr>,
        orig_src_port: NonZeroU16,
        orig_dst_port: NonZeroU16,
        seq: SeqNum,
        error: IcmpErrorCode,
    ) {
        self.with_tcp_sockets_mut(
            |Sockets { bound_state, port_alloc: _, inactive: _, socketmap }| {
                let conn_id = match socketmap.conns().get_by_addr(&ConnAddr {
                    ip: ConnIpAddr {
                        local: (orig_src_ip, orig_src_port),
                        remote: (orig_dst_ip, orig_dst_port),
                    },
                    device: None,
                }) {
                    Some(ConnAddrState { sharing: _, id }) => *id,
                    None => return,
                };
                let (
                    Connection {
                        acceptor,
                        state,
                        ip_sock: _,
                        defunct: _,
                        socket_options: _,
                        soft_error,
                        handshake_status,
                    },
                    _sharing,
                    _addr,
                ) = conn_id.get_from_bound_state_mut(bound_state).expect("invalid connection");
                *soft_error = soft_error.or(state.on_icmp_error(error, seq));

                if let State::Closed(Closed { reason }) = state {
                    tracing::info!("handshake_status: {handshake_status:?}");
                    handshake_status.update_if_pending(HandshakeStatus::Aborted);
                    match *acceptor {
                        Some(acceptor) => match acceptor {
                            Acceptor::Pending(listener_id) | Acceptor::Ready(listener_id) => {
                                if let Some((MaybeListener::Listener(listener), _sharing, _addr)) =
                                    listener_id.get_from_bound_state_mut(bound_state)
                                {
                                    let old_len = listener.pending.len() + listener.ready.len();
                                    listener
                                        .pending
                                        .retain(|id| MaybeClosedConnectionId::from(*id) != conn_id);
                                    listener.ready.retain(|(id, _passive_open)| {
                                        MaybeClosedConnectionId::from(*id) != conn_id
                                    });
                                    assert_eq!(
                                        listener.pending.len() + listener.ready.len() + 1,
                                        old_len
                                    );
                                } else {
                                    unreachable!("inconsistent state: expected listener");
                                }
                            }
                        },
                        None => {
                            if let Some(err) = reason {
                                if *err == ConnectionError::TimedOut {
                                    *err = soft_error.unwrap_or(ConnectionError::TimedOut);
                                }
                            }
                        }
                    }
                }
            },
        )
    }

    fn get_connection_error(&mut self, conn_id: ConnectionId<I>) -> Option<ConnectionError> {
        self.with_tcp_sockets_mut(
            |Sockets { bound_state, port_alloc: _, inactive: _, socketmap: _ }| {
                let (conn, _sharing, _addr) =
                    conn_id.get_from_bound_state_mut(bound_state).expect("invalid connection ID");
                let hard_error = if let State::Closed(Closed { reason: hard_error }) = conn.state {
                    hard_error.clone()
                } else {
                    None
                };
                hard_error.or_else(|| conn.soft_error.take())
            },
        )
    }

    fn with_info<V: InfoVisitor>(&mut self, visitor: V) -> V::VisitResult {
        self.with_tcp_sockets(|sockets| {
            let Sockets { port_alloc: _, inactive, socketmap: _, bound_state } = sockets;
            let inactive = inactive.iter().map(|(index, _unbound)| SocketStats::<I> {
                id: UnboundId(index, IpVersionMarker::default()).into(),
                local: None,
                remote: None,
            });
            let bound = bound_state.iter_maps().flat_map(|map| {
                map.iter().filter_map(|(index, state)| match state {
                    BoundSocketState::Listener((state, _sharing, addr)) => {
                        let id = match state {
                            MaybeListener::Bound(_) => {
                                BoundId(index, IpVersionMarker::default()).into()
                            }
                            MaybeListener::Listener(_) => {
                                ListenerId(index, IpVersionMarker::default()).into()
                            }
                        };
                        let ListenerAddr { ip: ListenerIpAddr { identifier, addr }, device: _ } =
                            *addr;
                        Some(SocketStats { id, local: Some((addr, identifier)), remote: None })
                    }
                    BoundSocketState::Connected((state, _sharing, addr)) => {
                        let Connection {
                            acceptor: _,
                            state: _,
                            ip_sock: _,
                            defunct,
                            socket_options: _,
                            soft_error: _,
                            handshake_status: _,
                        } = state;
                        (!defunct).then(|| {
                            let ConnAddr {
                                ip: ConnIpAddr { local: (local_ip, local_port), remote },
                                device: _,
                            } = *addr;
                            let id = ConnectionId(index, IpVersionMarker::default()).into();
                            let local = Some((Some(local_ip), local_port));
                            let remote = Some(remote);
                            SocketStats { id, local, remote }
                        })
                    }
                })
            });

            visitor.visit(inactive.chain(bound))
        })
    }
}

fn get_reuseaddr<I: IpLayerIpExt, C: NonSyncContext, SC: SyncContext<I, C>>(
    sync_ctx: &mut SC,
    id: SocketId<I>,
) -> bool {
    sync_ctx.with_tcp_sockets(|sockets| {
        let Sockets { bound_state, port_alloc: _, inactive, socketmap: _ } = sockets;
        let maybe_listener_id: MaybeListenerId<_> = match id {
            SocketId::Unbound(id) => {
                let Unbound {
                    sharing,
                    bound_device: _,
                    buffer_sizes: _,
                    socket_options: _,
                    socket_extra: _,
                } = inactive.get(id.into()).expect("invalid socket ID");
                return match sharing {
                    SharingState::Exclusive => false,
                    SharingState::ReuseAddress => true,
                };
            }
            SocketId::Bound(id) => id.into(),
            SocketId::Listener(id) => id.into(),
            SocketId::Connection(id) => {
                let (_, sharing, _): (&Connection<_, _, _, _, _, _>, _, &ConnAddr<_, _, _, _>) =
                    id.get_from_bound_state(bound_state).expect("invalid connection ID");
                return match sharing {
                    SharingState::Exclusive => false,
                    SharingState::ReuseAddress => true,
                };
            }
        };
        let (_, sharing, _): (&MaybeListener<_, _, _, _>, _, &ListenerAddr<_, _, _>) =
            maybe_listener_id.get_from_bound_state(bound_state).expect("invalid listener state");
        let ListenerSharingState { sharing, listening: _ } = sharing;
        return match sharing {
            SharingState::Exclusive => false,
            SharingState::ReuseAddress => true,
        };
    })
}

fn set_reuseaddr_maybe_listener<I: IpLayerIpExt, C: NonSyncContext, SC: SyncContext<I, C>>(
    listener: bool,
    sync_ctx: &mut SC,
    id: MaybeListenerId<I>,
    reuse: bool,
) -> Result<(), SetReuseAddrError> {
    sync_ctx.with_tcp_sockets_mut(|sockets| {
        let Sockets { bound_state, port_alloc: _, inactive: _, socketmap } = sockets;
        let bound_state = id.get_from_bound_state_mut(bound_state).expect("invalid listener ID");
        let (_, old_sharing @ ListenerSharingState { listening, sharing }, addr) = &bound_state;
        let entry = socketmap.listeners_mut().entry(&id, addr).expect("invalid socket ID");
        assert_eq!(listener, *listening);
        let new_sharing = match reuse {
            true => SharingState::ReuseAddress,
            false => SharingState::Exclusive,
        };
        if new_sharing == *sharing {
            return Ok(());
        }
        let new_sharing = ListenerSharingState { listening: false, sharing: new_sharing };
        match entry.try_update_sharing(old_sharing, new_sharing.clone()) {
            Ok(()) => {
                let (_, sharing, _) = bound_state;
                *sharing = new_sharing;
                Ok(())
            }
            Err(UpdateSharingError) => Err(SetReuseAddrError),
        }
    })
}

fn do_send_inner<I, SC, C>(
    conn_id: MaybeClosedConnectionId<I>,
    conn: &mut Connection<
        I,
        SC::WeakDeviceId,
        C::Instant,
        C::ReceiveBuffer,
        C::SendBuffer,
        C::ListenerNotifierOrProvidedBuffers,
    >,
    addr: &ConnAddr<I::Addr, SC::WeakDeviceId, NonZeroU16, NonZeroU16>,
    ip_transport_ctx: &mut SC,
    ctx: &mut C,
) where
    I: IpExt,
    C: NonSyncContext,
    SC: BufferTransportIpContext<I, C, EmptyBuf>,
{
    while let Some(seg) = conn.state.poll_send(u32::MAX, ctx.now(), &conn.socket_options) {
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
                debug!(
                    "failed to send an ip packet on {:?}, body: {:?}, err: {:?}",
                    conn_id, body, err
                )
            },
        );
    }

    if let Some(instant) = conn.state.poll_send_at() {
        let _: Option<_> = ctx.schedule_timer_instant(instant, TimerId::new::<I>(conn_id));
    }
}

enum SendBufferSize {}
enum ReceiveBufferSize {}

trait AccessBufferSize {
    fn set_unconnected_size(sizes: &mut BufferSizes, new_size: usize);
    fn set_connected_size<
        Instant: crate::Instant + 'static,
        S: SendBuffer,
        R: ReceiveBuffer,
        P: Debug + Takeable,
    >(
        state: &mut State<Instant, R, S, P>,
        new_size: usize,
    );
    fn get_buffer_size(sizes: &OptionalBufferSizes) -> Option<usize>;
}

impl AccessBufferSize for SendBufferSize {
    fn set_unconnected_size(sizes: &mut BufferSizes, new_size: usize) {
        let BufferSizes { send, receive: _ } = sizes;
        *send = new_size
    }

    fn set_connected_size<
        Instant: crate::Instant + 'static,
        S: SendBuffer,
        R: ReceiveBuffer,
        P: Debug + Takeable,
    >(
        state: &mut State<Instant, R, S, P>,
        new_size: usize,
    ) {
        state.set_send_buffer_size(new_size)
    }

    fn get_buffer_size(sizes: &OptionalBufferSizes) -> Option<usize> {
        let OptionalBufferSizes { send, receive: _ } = sizes;
        *send
    }
}

impl AccessBufferSize for ReceiveBufferSize {
    fn set_unconnected_size(sizes: &mut BufferSizes, new_size: usize) {
        let BufferSizes { send: _, receive } = sizes;
        *receive = new_size
    }

    fn set_connected_size<
        Instant: crate::Instant + 'static,
        S: SendBuffer,
        R: ReceiveBuffer,
        P: Debug + Takeable,
    >(
        state: &mut State<Instant, R, S, P>,
        new_size: usize,
    ) {
        state.set_receive_buffer_size(new_size)
    }

    fn get_buffer_size(sizes: &OptionalBufferSizes) -> Option<usize> {
        let OptionalBufferSizes { send: _, receive } = sizes;
        *receive
    }
}

fn set_buffer_size<
    Which: AccessBufferSize,
    I: IpLayerIpExt,
    C: NonSyncContext,
    SC: SyncContext<I, C>,
>(
    sync_ctx: &mut SC,
    id: SocketId<I>,
    size: usize,
) {
    sync_ctx.with_tcp_sockets_mut(|sockets| {
        let Sockets { bound_state, port_alloc: _, inactive, socketmap: _ } = sockets;
        let get_listener = match id {
            SocketId::Unbound(id) => {
                let Unbound {
                    bound_device: _,
                    buffer_sizes,
                    socket_options: _,
                    sharing: _,
                    socket_extra: _,
                } = inactive.get_mut(id.into()).expect("invalid unbound ID");
                return Which::set_unconnected_size(buffer_sizes, size);
            }
            SocketId::Connection(id) => {
                let (conn, _, _): (_, &mut SharingState, &mut ConnAddr<_, _, _, _>) =
                    id.get_from_bound_state_mut(bound_state).expect("invalid conn ID");
                let Connection {
                    acceptor: _,
                    state,
                    ip_sock: _,
                    defunct: _,
                    socket_options: _,
                    soft_error: _,
                    handshake_status: _,
                } = conn;
                return Which::set_connected_size(state, size);
            }
            SocketId::Bound(id) => MaybeListenerId::from(id)
                .get_from_bound_state_mut(bound_state)
                .expect("invalid bound ID"),
            SocketId::Listener(id) => MaybeListenerId::from(id)
                .get_from_bound_state_mut(bound_state)
                .expect("invalid listener ID"),
        };

        let (state, _, _): (_, &mut ListenerSharingState, &mut ListenerAddr<_, _, _>) =
            get_listener;
        let buffer_sizes = match state {
            MaybeListener::Bound(BoundState {
                buffer_sizes,
                socket_options: _,
                socket_extra: _,
            }) => buffer_sizes,
            MaybeListener::Listener(Listener {
                backlog: _,
                ready: _,
                pending: _,
                buffer_sizes,
                socket_options: _,
                notifier: _,
            }) => buffer_sizes,
        };
        Which::set_unconnected_size(buffer_sizes, size)
    })
}

fn get_buffer_size<
    Which: AccessBufferSize,
    I: IpLayerIpExt,
    C: NonSyncContext,
    SC: SyncContext<I, C>,
>(
    sync_ctx: &mut SC,
    id: SocketId<I>,
) -> Option<usize> {
    sync_ctx.with_tcp_sockets(|sockets| {
        let Sockets { bound_state, port_alloc: _, inactive, socketmap: _ } = sockets;
        let sizes = (|| {
            let listener = match id {
                SocketId::Unbound(id) => {
                    let Unbound {
                        bound_device: _,
                        buffer_sizes,
                        socket_options: _,
                        sharing: _,
                        socket_extra: _,
                    } = inactive.get(id.into()).expect("invalid unbound ID");
                    return buffer_sizes.into_optional();
                }
                SocketId::Connection(id) => {
                    let (conn, _, _): (_, &SharingState, &ConnAddr<_, _, _, _>) =
                        id.get_from_bound_state(bound_state).expect("invalid connection ID");
                    let Connection {
                        acceptor: _,
                        state,
                        ip_sock: _,
                        defunct: _,
                        socket_options: _,
                        soft_error: _,
                        handshake_status: _,
                    } = conn;
                    return state.target_buffer_sizes();
                }
                SocketId::Bound(id) => MaybeListenerId::from(id)
                    .get_from_bound_state(bound_state)
                    .expect("invalid bound ID"),
                SocketId::Listener(id) => MaybeListenerId::from(id)
                    .get_from_bound_state(bound_state)
                    .expect("invalid listener ID"),
            };

            let (state, _, _): (_, &ListenerSharingState, &ListenerAddr<_, _, _>) = listener;
            match state {
                MaybeListener::Bound(BoundState {
                    buffer_sizes,
                    socket_options: _,
                    socket_extra: _,
                }) => buffer_sizes,
                MaybeListener::Listener(Listener {
                    backlog: _,
                    ready: _,
                    pending: _,
                    buffer_sizes,
                    socket_options: _,
                    notifier: _,
                }) => buffer_sizes,
            }
            .into_optional()
        })();
        Which::get_buffer_size(&sizes)
    })
}

/// Creates a new socket in unbound state.
pub fn create_socket<I, C>(
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    socket_extra: C::ListenerNotifierOrProvidedBuffers,
) -> UnboundId<I>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        IpInvariant((&mut sync_ctx, ctx, socket_extra)),
        |IpInvariant((sync_ctx, ctx, socket_extra))| {
            SocketHandler::create_socket(sync_ctx, ctx, socket_extra)
        },
        |IpInvariant((sync_ctx, ctx, socket_extra))| {
            SocketHandler::create_socket(sync_ctx, ctx, socket_extra)
        },
    )
}

/// Sets the device to which a socket should be bound.
///
/// Sets the device on which the socket (once bound or connected) should send
/// and receive packets, or `None` to clear the bound device.
pub fn set_unbound_device<I, C>(
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: UnboundId<I>,
    device: Option<DeviceId<C>>,
) where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
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
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: ListenerId<I>,
    device: Option<DeviceId<C>>,
) -> Result<(), SetDeviceError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
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
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: BoundId<I>,
    device: Option<DeviceId<C>>,
) -> Result<(), SetDeviceError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
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
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: ConnectionId<I>,
    device: Option<DeviceId<C>>,
) -> Result<(), SetDeviceError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
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
///
/// Requests that the given socket be bound to the local address, if one is
/// provided; otherwise to all addresses. If `port` is specified (is `Some`),
/// the socket will be bound to that port. Otherwise a port will be selected to
/// not conflict with existing bound or connected sockets.
pub fn bind<I, C>(
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: UnboundId<I>,
    local_ip: Option<ZonedAddr<I::Addr, DeviceId<C>>>,
    port: Option<NonZeroU16>,
) -> Result<BoundId<I>, LocalAddressError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
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
    sync_ctx: &SyncCtx<C>,
    id: BoundId<I>,
    backlog: NonZeroUsize,
) -> Result<ListenerId<I>, ListenError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant((&mut sync_ctx, backlog)), id),
        |(IpInvariant((sync_ctx, backlog)), id)| SocketHandler::listen(sync_ctx, id, backlog),
        |(IpInvariant((sync_ctx, backlog)), id)| SocketHandler::listen(sync_ctx, id, backlog),
    )
}

/// Possible errors for accept operation.
#[derive(Debug, GenericOverIp)]
pub enum AcceptError {
    /// There is no established socket currently.
    WouldBlock,
}

/// Errors for the listen operation.
#[derive(Debug, GenericOverIp)]
pub enum ListenError {
    /// There would be a conflict with another listening socket.
    ListenerExists,
}

/// Possible error for calling `shutdown` on a not-yet connected socket.
#[derive(Debug, GenericOverIp)]
#[cfg_attr(test, derive(PartialEq, Eq))]
pub struct NoConnection;

/// Error returned when attempting to set the ReuseAddress option.
#[derive(Debug, GenericOverIp)]
pub struct SetReuseAddrError;

/// Accepts an established socket from the queue of a listener socket.
pub fn accept<I: Ip, C>(
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: ListenerId<I>,
) -> Result<(ConnectionId<I>, SocketAddr<I::Addr, WeakDeviceId<C>>, C::ReturnedBuffers), AcceptError>
where
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
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
#[derive(Debug, Error, GenericOverIp)]
#[cfg_attr(test, derive(PartialEq, Eq))]
pub enum ConnectError {
    /// Cannot allocate a local port for the connection.
    #[error("Unable to allocate a port")]
    NoPort,
    /// Cannot find a route to the remote host.
    #[error("No route to remote host")]
    NoRoute,
    /// There was a problem with the provided address relating to its zone.
    #[error("{}", _0)]
    Zone(#[from] ZonedAddressError),
    /// There is an existing connection with the same 4-tuple.
    #[error("There is already a connection at the address requested")]
    ConnectionExists,
}

/// Connects a socket that has been bound locally.
///
/// When the method returns, the connection is not guaranteed to be established.
/// It is up to the caller (Bindings) to determine when the connection has been
/// established. Bindings are free to use anything available on the platform to
/// check, for instance, signals.
pub fn connect_bound<I, C>(
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: BoundId<I>,
    remote: SocketAddr<I::Addr, DeviceId<C>>,
) -> Result<ConnectionId<I>, ConnectError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx)), id, remote),
        |(IpInvariant((sync_ctx, ctx)), id, remote)| {
            SocketHandler::connect_bound(sync_ctx, ctx, id, remote)
        },
        |(IpInvariant((sync_ctx, ctx)), id, remote)| {
            SocketHandler::connect_bound(sync_ctx, ctx, id, remote)
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
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: UnboundId<I>,
    remote_ip: ZonedAddr<I::Addr, DeviceId<C>>,
    remote_port: NonZeroU16,
) -> Result<ConnectionId<I>, ConnectError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, remote_port)), id, remote_ip),
        |(IpInvariant((sync_ctx, ctx, remote_port)), id, remote_ip)| {
            SocketHandler::connect_unbound(sync_ctx, ctx, id, remote_ip, remote_port)
        },
        |(IpInvariant((sync_ctx, ctx, remote_port)), id, remote_ip)| {
            SocketHandler::connect_unbound(sync_ctx, ctx, id, remote_ip, remote_port)
        },
    )
}

fn connect_inner<I, SC, C>(
    isn: &IsnGenerator<C::Instant>,
    socketmap: &mut BoundSocketMap<
        I,
        SC::WeakDeviceId,
        IpPortSpec,
        TcpSocketSpec<I, SC::WeakDeviceId, C>,
    >,
    make_connection: impl FnOnce(
        ConnAddr<I::Addr, SC::WeakDeviceId, NonZeroU16, NonZeroU16>,
        <TcpSocketSpec<I, SC::WeakDeviceId, C> as SocketStateSpec>::ConnState,
    ) -> ConnectionId<I>,
    ip_transport_ctx: &mut SC,
    ctx: &mut C,
    ip_sock: IpSock<I, SC::WeakDeviceId, DefaultSendOptions>,
    local_port: NonZeroU16,
    remote_port: NonZeroU16,
    netstack_buffers: C::ListenerNotifierOrProvidedBuffers,
    buffer_sizes: BufferSizes,
    socket_options: SocketOptions,
    sharing: SharingState,
    device_mms: Mms,
) -> Result<ConnectionId<I>, ConnectError>
where
    I: IpLayerIpExt,
    C: NonSyncContext,
    SC: BufferTransportIpContext<I, C, EmptyBuf>,
{
    let isn = isn.generate(
        ctx.now(),
        (ip_sock.local_ip().clone(), local_port),
        (ip_sock.remote_ip().clone(), remote_port),
    );
    let conn_addr = ConnAddr {
        ip: ConnIpAddr {
            local: (ip_sock.local_ip().clone(), local_port),
            remote: (ip_sock.remote_ip().clone(), remote_port),
        },
        device: ip_sock.device().cloned(),
    };
    let now = ctx.now();
    let (syn_sent, syn) = Closed::<Initial>::connect(
        isn,
        now,
        netstack_buffers,
        buffer_sizes,
        Mss::from_mms::<I>(device_mms).ok_or(ConnectError::NoRoute)?,
        Mss::default::<I>(),
        &socket_options,
    );
    let state = State::SynSent(syn_sent);
    let poll_send_at = state.poll_send_at().expect("no retrans timer");

    // Before recording the connection, make sure it is viable.
    ip_transport_ctx
        .send_ip_packet(ctx, &ip_sock, tcp_serialize_segment(syn, conn_addr.ip), None)
        .map_err(|(body, err)| {
            warn!("tcp: failed to send ip packet {:?}: {:?}", body, err);
            ConnectError::NoRoute
        })?;

    let conn_id = socketmap
        .conns_mut()
        .try_insert(conn_addr.clone(), sharing, |_addr, _sharing| {
            make_connection(
                conn_addr,
                Connection {
                    acceptor: None,
                    state,
                    ip_sock,
                    defunct: false,
                    socket_options,
                    soft_error: None,
                    handshake_status: HandshakeStatus::Pending,
                },
            )
            .into()
        })
        .map_err(|(err, _sharing)| match err {
            // The connection will conflict with an existing one.
            InsertError::Exists | InsertError::ShadowerExists => ConnectError::ConnectionExists,
            // Connections don't conflict with listeners, and we should not
            // observe the following errors.
            InsertError::ShadowAddrExists | InsertError::IndirectConflict => {
                panic!("failed to insert connection: {:?}", err)
            }
        })?
        .id();

    assert_eq!(ctx.schedule_timer_instant(poll_send_at, TimerId::new::<I>(conn_id)), None);
    // This conversion Ok because `conn_id` is newly created; No one should
    // have called close on it.
    let MaybeClosedConnectionId(id, marker) = conn_id;
    Ok(ConnectionId(id, marker))
}

/// Closes the connection. The user has promised that they will not use `id`
/// again, we can reclaim the connection after the connection becomes `Closed`.
pub fn close_conn<I, C>(sync_ctx: &SyncCtx<C>, ctx: &mut C, id: ConnectionId<I>)
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx)), id),
        |(IpInvariant((sync_ctx, ctx)), id)| SocketHandler::close_conn(sync_ctx, ctx, id),
        |(IpInvariant((sync_ctx, ctx)), id)| SocketHandler::close_conn(sync_ctx, ctx, id),
    )
}

/// Gets the handshake status for the connection.
pub fn get_handshake_status<I, C>(sync_ctx: &SyncCtx<C>, id: ConnectionId<I>) -> HandshakeStatus
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    let IpInvariant(status) = I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| {
            IpInvariant(SocketHandler::get_handshake_status(sync_ctx, id))
        },
        |(IpInvariant(sync_ctx), id)| {
            IpInvariant(SocketHandler::get_handshake_status(sync_ctx, id))
        },
    );
    status
}

/// Shuts down the write-half of the connection. Calling this function signals
/// the other side of the connection that we will not be sending anything over
/// the connection; The connection will still stay in the socketmap even after
/// reaching `Closed` state. The user needs to call `close_conn` in order to
/// remove it.
pub fn shutdown_conn<I, C>(
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: ConnectionId<I>,
) -> Result<(), NoConnection>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx)), id),
        |(IpInvariant((sync_ctx, ctx)), id)| SocketHandler::shutdown_conn(sync_ctx, ctx, id),
        |(IpInvariant((sync_ctx, ctx)), id)| SocketHandler::shutdown_conn(sync_ctx, ctx, id),
    )
}

/// Removes an unbound socket.
pub fn remove_unbound<I, C>(sync_ctx: &SyncCtx<C>, id: UnboundId<I>)
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::remove_unbound(sync_ctx, id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::remove_unbound(sync_ctx, id),
    )
}

/// Removes a bound socket.
pub fn remove_bound<I, C>(sync_ctx: &SyncCtx<C>, id: BoundId<I>)
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
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
pub fn shutdown_listener<I, C>(sync_ctx: &SyncCtx<C>, ctx: &mut C, id: ListenerId<I>) -> BoundId<I>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx)), id),
        |(IpInvariant((sync_ctx, ctx)), id)| SocketHandler::shutdown_listener(sync_ctx, ctx, id),
        |(IpInvariant((sync_ctx, ctx)), id)| SocketHandler::shutdown_listener(sync_ctx, ctx, id),
    )
}

/// Sets the POSIX SO_REUSEADDR socket option on an unbound socket.
pub fn set_reuseaddr_unbound<I, C>(sync_ctx: &SyncCtx<C>, id: UnboundId<I>, reuse: bool)
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant((&mut sync_ctx, reuse)), id),
        |(IpInvariant((sync_ctx, reuse)), id)| {
            SocketHandler::set_reuseaddr_unbound(sync_ctx, id, reuse)
        },
        |(IpInvariant((sync_ctx, reuse)), id)| {
            SocketHandler::set_reuseaddr_unbound(sync_ctx, id, reuse)
        },
    )
}

/// Sets the POSIX SO_REUSEADDR socket option on a bound socket.
pub fn set_reuseaddr_bound<I, C>(
    sync_ctx: &SyncCtx<C>,
    id: BoundId<I>,
    reuse: bool,
) -> Result<(), SetReuseAddrError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant((&mut sync_ctx, reuse)), id),
        |(IpInvariant((sync_ctx, reuse)), id)| {
            SocketHandler::set_reuseaddr_bound(sync_ctx, id, reuse)
        },
        |(IpInvariant((sync_ctx, reuse)), id)| {
            SocketHandler::set_reuseaddr_bound(sync_ctx, id, reuse)
        },
    )
}

/// Sets the POSIX SO_REUSEADDR socket option on a listening socket.
pub fn set_reuseaddr_listener<I, C>(
    sync_ctx: &SyncCtx<C>,
    id: ListenerId<I>,
    reuse: bool,
) -> Result<(), SetReuseAddrError>
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant((&mut sync_ctx, reuse)), id),
        |(IpInvariant((sync_ctx, reuse)), id)| {
            SocketHandler::set_reuseaddr_listener(sync_ctx, id, reuse)
        },
        |(IpInvariant((sync_ctx, reuse)), id)| {
            SocketHandler::set_reuseaddr_listener(sync_ctx, id, reuse)
        },
    )
}

/// Gets the POSIX SO_REUSEADDR socket option on a socket.
pub fn reuseaddr<I, C>(sync_ctx: &SyncCtx<C>, id: impl Into<SocketId<I>>) -> bool
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    let id = id.into();
    I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::reuseaddr(sync_ctx, id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::reuseaddr(sync_ctx, id),
    )
}

/// Statistics about an individual socket.
pub struct SocketStats<I: Ip> {
    /// Identifier for the socket.
    pub id: SocketId<I>,
    /// The local address of the socket.
    pub local: Option<(Option<SpecifiedAddr<I::Addr>>, NonZeroU16)>,
    /// The remote address of the socket.
    pub remote: Option<(SpecifiedAddr<I::Addr>, NonZeroU16)>,
}

/// Visitor for socket state.
pub trait InfoVisitor {
    /// The result of [`InfoVisitor::visit`].
    type VisitResult;

    /// Consumes `self` and a socket state iterator to produce a `VisitResult`.
    fn visit<I: Ip>(self, stats: impl Iterator<Item = SocketStats<I>>) -> Self::VisitResult;
}

/// Provides access to shared and per-socket TCP stats via a visitor.
pub fn with_info<I, C, V>(sync_ctx: &SyncCtx<C>, cb: V) -> V::VisitResult
where
    I: IpExt,
    C: crate::NonSyncContext,
    V: InfoVisitor,
{
    let mut sync_ctx = Locked::new(sync_ctx);
    let IpInvariant(r) = I::map_ip(
        IpInvariant((&mut sync_ctx, cb)),
        |IpInvariant((sync_ctx, cb))| {
            IpInvariant(SocketHandler::<Ipv4, _>::with_info(sync_ctx, cb))
        },
        |IpInvariant((sync_ctx, cb))| {
            IpInvariant(SocketHandler::<Ipv6, _>::with_info(sync_ctx, cb))
        },
    );
    r
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
    pub local_addr: SocketAddr<A, D>,
    /// The remote address the socket is connected to.
    pub remote_addr: SocketAddr<A, D>,
    /// The device the socket is bound to.
    pub device: Option<D>,
}

impl<D: Clone, Extra> From<&'_ Unbound<D, Extra>> for UnboundInfo<D> {
    fn from(unbound: &Unbound<D, Extra>) -> Self {
        let Unbound {
            bound_device: device,
            buffer_sizes: _,
            socket_options: _,
            sharing: _,
            socket_extra: _,
        } = unbound;
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
        let convert = |(ip, port): (SpecifiedAddr<A>, NonZeroU16)| SocketAddr {
            ip: maybe_zoned(ip, &device),
            port,
        };
        Self { local_addr: convert(local), remote_addr: convert(remote), device }
    }
}

/// Get information for unbound TCP socket.
pub fn get_unbound_info<I: Ip, C: crate::NonSyncContext>(
    sync_ctx: &SyncCtx<C>,
    id: UnboundId<I>,
) -> UnboundInfo<WeakDeviceId<C>> {
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv4, _>::get_unbound_info(sync_ctx, id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv6, _>::get_unbound_info(sync_ctx, id),
    )
}

/// Get information for bound TCP socket.
pub fn get_bound_info<I: Ip, C: crate::NonSyncContext>(
    sync_ctx: &SyncCtx<C>,
    id: BoundId<I>,
) -> BoundInfo<I::Addr, WeakDeviceId<C>> {
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv4, _>::get_bound_info(sync_ctx, id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv6, _>::get_bound_info(sync_ctx, id),
    )
}

/// Get information for listener TCP socket.
pub fn get_listener_info<I: Ip, C: crate::NonSyncContext>(
    sync_ctx: &SyncCtx<C>,
    id: ListenerId<I>,
) -> BoundInfo<I::Addr, WeakDeviceId<C>> {
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv4, _>::get_listener_info(sync_ctx, id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv6, _>::get_listener_info(sync_ctx, id),
    )
}

/// Get information for connection TCP socket.
pub fn get_connection_info<I: Ip, C: crate::NonSyncContext>(
    sync_ctx: &SyncCtx<C>,
    id: ConnectionId<I>,
) -> ConnectionInfo<I::Addr, WeakDeviceId<C>> {
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant(&mut sync_ctx), id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv4, _>::get_connection_info(sync_ctx, id),
        |(IpInvariant(sync_ctx), id)| SocketHandler::<Ipv6, _>::get_connection_info(sync_ctx, id),
    )
}

/// Access options mutably for a TCP socket.
pub fn with_socket_options_mut<
    I: Ip,
    C: crate::NonSyncContext,
    R,
    F: FnOnce(&mut SocketOptions) -> R,
    Id: Into<SocketId<I>>,
>(
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: Id,
    f: F,
) -> R {
    let mut sync_ctx = Locked::new(sync_ctx);
    let IpInvariant(r) = I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, f)), id.into()),
        |(IpInvariant((sync_ctx, ctx, f)), id)| {
            IpInvariant(SocketHandler::with_socket_options_mut(sync_ctx, ctx, id, f))
        },
        |(IpInvariant((sync_ctx, ctx, f)), id)| {
            IpInvariant(SocketHandler::with_socket_options_mut(sync_ctx, ctx, id, f))
        },
    );
    r
}

/// Access socket options immutably for a TCP socket.
pub fn with_socket_options<
    I: Ip,
    C: crate::NonSyncContext,
    R,
    F: FnOnce(&SocketOptions) -> R,
    Id: Into<SocketId<I>>,
>(
    sync_ctx: &SyncCtx<C>,
    id: Id,
    f: F,
) -> R {
    let mut sync_ctx = Locked::new(sync_ctx);
    let IpInvariant(r) = I::map_ip(
        (IpInvariant((&mut sync_ctx, f)), id.into()),
        |(IpInvariant((sync_ctx, f)), id)| {
            IpInvariant(SocketHandler::with_socket_options(sync_ctx, id, f))
        },
        |(IpInvariant((sync_ctx, f)), id)| {
            IpInvariant(SocketHandler::with_socket_options(sync_ctx, id, f))
        },
    );
    r
}

/// Set the size of the send buffer for this socket and future derived sockets.
pub fn set_send_buffer_size<I: Ip, C: crate::NonSyncContext, Id: Into<SocketId<I>>>(
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: Id,
    size: usize,
) {
    let mut sync_ctx = Locked::new(sync_ctx);
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
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: Id,
) -> Option<usize> {
    let mut sync_ctx = Locked::new(sync_ctx);
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

/// Set the size of the send buffer for this socket and future derived sockets.
pub fn set_receive_buffer_size<I: Ip, C: crate::NonSyncContext, Id: Into<SocketId<I>>>(
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: Id,
    size: usize,
) {
    let mut sync_ctx = Locked::new(sync_ctx);
    I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx, size)), id.into()),
        |(IpInvariant((sync_ctx, ctx, size)), id)| {
            SocketHandler::set_receive_buffer_size(sync_ctx, ctx, id, size)
        },
        |(IpInvariant((sync_ctx, ctx, size)), id)| {
            SocketHandler::set_receive_buffer_size(sync_ctx, ctx, id, size)
        },
    )
}

/// Get the size of the receive buffer for this socket and future derived
/// sockets.
pub fn receive_buffer_size<I: Ip, C: crate::NonSyncContext, Id: Into<SocketId<I>>>(
    sync_ctx: &SyncCtx<C>,
    ctx: &mut C,
    id: Id,
) -> Option<usize> {
    let mut sync_ctx = Locked::new(sync_ctx);
    let IpInvariant(size) = I::map_ip(
        (IpInvariant((&mut sync_ctx, ctx)), id.into()),
        |(IpInvariant((sync_ctx, ctx)), id)| {
            IpInvariant(SocketHandler::receive_buffer_size(sync_ctx, ctx, id))
        },
        |(IpInvariant((sync_ctx, ctx)), id)| {
            IpInvariant(SocketHandler::receive_buffer_size(sync_ctx, ctx, id))
        },
    );
    size
}

/// Gets the last error on the connection.
pub fn get_connection_error<I: Ip, C: crate::NonSyncContext>(
    sync_ctx: &SyncCtx<C>,
    conn_id: ConnectionId<I>,
) -> Option<ConnectionError> {
    let mut sync_ctx = Locked::new(sync_ctx);
    let IpInvariant(err) = I::map_ip(
        (IpInvariant(&mut sync_ctx), conn_id),
        |(IpInvariant(sync_ctx), conn_id)| {
            IpInvariant(SocketHandler::get_connection_error(sync_ctx, conn_id))
        },
        |(IpInvariant(sync_ctx), conn_id)| {
            IpInvariant(SocketHandler::get_connection_error(sync_ctx, conn_id))
        },
    );
    err
}

/// Call this function whenever a socket can push out more data. That means either:
///
/// - A retransmission timer fires.
/// - An ack received from peer so that our send window is enlarged.
/// - The user puts data into the buffer and we are notified.
pub fn do_send<I, C>(sync_ctx: &SyncCtx<C>, ctx: &mut C, conn_id: MaybeClosedConnectionId<I>)
where
    I: IpExt,
    C: crate::NonSyncContext,
{
    let mut sync_ctx = Locked::new(sync_ctx);
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

impl<I: Ip> EntryKey for MaybeListenerId<I> {
    fn get_key_index(&self) -> usize {
        let Self(x, _marker) = self;
        *x
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

impl<I: Ip> EntryKey for MaybeClosedConnectionId<I> {
    fn get_key_index(&self) -> usize {
        let Self(x, _marker) = self;
        *x
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

impl<I: Ip> Into<usize> for ConnectionId<I> {
    fn into(self) -> usize {
        let Self(x, _marker) = self;
        x
    }
}

impl<I: Ip> EntryKey for UnboundId<I> {
    fn get_key_index(&self) -> usize {
        let Self(x, _marker) = self;
        *x
    }
}

impl<I: Ip> EntryKey for BoundId<I> {
    fn get_key_index(&self) -> usize {
        let Self(x, _marker) = self;
        *x
    }
}

impl<I: Ip> EntryKey for ListenerId<I> {
    fn get_key_index(&self) -> usize {
        let Self(x, _marker) = self;
        *x
    }
}

impl<I: Ip> EntryKey for ConnectionId<I> {
    fn get_key_index(&self) -> usize {
        let Self(x, _marker) = self;
        *x
    }
}

#[cfg(test)]
mod tests {
    use core::{cell::RefCell, fmt::Debug, time::Duration};
    use fakealloc::{rc::Rc, vec};

    use const_unwrap::const_unwrap_option;
    use ip_test_macro::ip_test;
    use net_declare::net_ip_v6;
    use net_types::{
        ip::{AddrSubnet, Ip, Ipv4, Ipv6, Ipv6SourceAddr, Mtu},
        AddrAndZone, LinkLocalAddr, Witness,
    };
    use packet::{Buf, ParseBuffer as _};
    use packet_formats::{
        icmp::{Icmpv4DestUnreachableCode, Icmpv6DestUnreachableCode},
        tcp::{TcpParseArgs, TcpSegment},
    };
    use rand::Rng as _;
    use test_case::test_case;

    use crate::{
        context::{
            testutil::{
                FakeCtxWithSyncCtx, FakeFrameCtx, FakeInstant, FakeNetwork, FakeNetworkContext,
                FakeNonSyncCtx, FakeSyncCtx, InstantAndData, PendingFrameData, StepResult,
                WithFakeFrameContext, WrappedFakeSyncCtx,
            },
            InstantContext as _,
        },
        device::testutil::{FakeDeviceId, FakeStrongDeviceId, FakeWeakDeviceId, MultipleDevicesId},
        ip::{
            device::state::{
                IpDeviceState, IpDeviceStateIpExt, Ipv4AddrConfig, Ipv4AddressEntry,
                Ipv6AddrConfig, Ipv6AddressEntry, Ipv6DadState,
            },
            icmp::{IcmpIpExt, Icmpv4ErrorCode, Icmpv6ErrorCode},
            socket::{
                testutil::{FakeBufferIpSocketCtx, FakeDeviceConfig, FakeIpSocketCtx},
                MmsError, SendOptions,
            },
            BufferIpTransportContext as _, IpTransportContext, SendIpPacketMeta,
        },
        testutil::{new_rng, run_with_many_seeds, set_logger_for_test, FakeCryptoRng, TestIpExt},
        transport::tcp::{
            buffer::{Buffer, BufferLimits, RingBuffer, SendPayload},
            segment::Payload,
            state::{TimeWait, MSL},
            ConnectionError, DEFAULT_FIN_WAIT2_TIMEOUT,
        },
    };

    use super::*;

    impl<A: IpAddress, D> SocketAddr<A, D> {
        fn map_zone<Y>(self, f: impl FnOnce(D) -> Y) -> SocketAddr<A, Y> {
            let Self { ip, port } = self;
            SocketAddr { ip: ip.map_zone(f), port }
        }
    }

    trait TcpTestIpExt: IpExt + TestIpExt + IpDeviceStateIpExt + IpLayerIpExt {
        fn recv_src_addr(addr: Self::Addr) -> Self::RecvSrcAddr;

        fn new_device_state(
            addrs: impl IntoIterator<Item = Self::Addr>,
            prefix: u8,
        ) -> IpDeviceState<FakeInstant, Self>;
    }

    type FakeBufferIpTransportCtx<I, D> = FakeSyncCtx<
        FakeBufferIpSocketCtx<I, D>,
        SendIpPacketMeta<I, D, SpecifiedAddr<<I as Ip>::Addr>>,
        D,
    >;

    struct FakeTcpState<I: TcpTestIpExt, D: FakeStrongDeviceId> {
        isn_generator: IsnGenerator<FakeInstant>,
        sockets: Sockets<I, FakeWeakDeviceId<D>, TcpNonSyncCtx>,
    }

    impl<I: TcpTestIpExt, D: FakeStrongDeviceId> Default for FakeTcpState<I, D> {
        fn default() -> Self {
            Self {
                isn_generator: Default::default(),
                sockets: Sockets {
                    bound_state: IdMapCollection::new(),
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

    impl<I: TcpTestIpExt, D: FakeStrongDeviceId>
        WithFakeFrameContext<SendIpPacketMeta<I, D, SpecifiedAddr<<I as Ip>::Addr>>>
        for TcpCtx<I, D>
    {
        fn with_fake_frame_ctx_mut<
            O,
            F: FnOnce(&mut FakeFrameCtx<SendIpPacketMeta<I, D, SpecifiedAddr<<I as Ip>::Addr>>>) -> O,
        >(
            &mut self,
            f: F,
        ) -> O {
            f(&mut self.sync_ctx.inner.as_mut())
        }
    }

    impl<I: TcpTestIpExt, D: FakeStrongDeviceId> FakeNetworkContext for TcpCtx<I, D> {
        type TimerId = TimerId;
        type SendMeta = SendIpPacketMeta<I, FakeDeviceId, SpecifiedAddr<<I as Ip>::Addr>>;
    }

    #[derive(Debug, Eq, PartialEq)]
    enum EitherId<V4, V6> {
        V4(V4),
        V6(V6),
    }

    impl<I: Ip> From<ListenerId<I>> for EitherId<ListenerId<Ipv4>, ListenerId<Ipv6>> {
        fn from(value: ListenerId<I>) -> Self {
            let IpInvariant(either) =
                I::map_ip(value, |v4| IpInvariant(Self::V4(v4)), |v6| IpInvariant(Self::V6(v6)));
            either
        }
    }

    impl<I: Ip> From<ConnectionId<I>> for EitherId<ConnectionId<Ipv4>, ConnectionId<Ipv6>> {
        fn from(value: ConnectionId<I>) -> Self {
            let IpInvariant(either) =
                I::map_ip(value, |v4| IpInvariant(Self::V4(v4)), |v6| IpInvariant(Self::V6(v6)));
            either
        }
    }

    type TcpNonSyncCtx = FakeNonSyncCtx<TimerId, (), ()>;

    impl Buffer for Rc<RefCell<RingBuffer>> {
        fn limits(&self) -> BufferLimits {
            self.borrow().limits()
        }

        fn target_capacity(&self) -> usize {
            self.borrow().target_capacity()
        }

        fn request_capacity(&mut self, size: usize) {
            self.borrow_mut().set_target_size(size)
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
        fn limits(&self) -> BufferLimits {
            let Self { fake_stream, ring } = self;
            let BufferLimits { capacity: ring_capacity, len: ring_len } = ring.limits();
            let len = ring_len + fake_stream.borrow().len();
            let capacity = ring_capacity + fake_stream.borrow().capacity();
            BufferLimits { len, capacity }
        }

        fn target_capacity(&self) -> usize {
            let Self { fake_stream: _, ring } = self;
            ring.target_capacity()
        }

        fn request_capacity(&mut self, size: usize) {
            let Self { fake_stream: _, ring } = self;
            ring.set_target_size(size)
        }
    }

    impl SendBuffer for TestSendBuffer {
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
                let BufferLimits { capacity, len } = ring.limits();
                let len = (capacity - len).min(fake_stream.borrow().len());
                let rest = fake_stream.borrow_mut().split_off(len);
                let first = fake_stream.replace(rest);
                assert_eq!(ring.enqueue_data(&first[..]), len);
            }
            ring.peek_with(offset, f)
        }
    }

    #[derive(Clone, Debug, Default, Eq, PartialEq)]
    pub struct ClientBuffers {
        receive: Rc<RefCell<RingBuffer>>,
        send: Rc<RefCell<Vec<u8>>>,
    }

    impl ClientBuffers {
        fn new(buffer_sizes: BufferSizes) -> Self {
            let BufferSizes { send, receive } = buffer_sizes;
            Self {
                receive: Rc::new(RefCell::new(RingBuffer::new(receive))),
                send: Rc::new(RefCell::new(Vec::with_capacity(send))),
            }
        }
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    pub enum ProvidedBuffers {
        Buffers(WriteBackClientBuffers),
        NoBuffers,
    }

    impl Default for ProvidedBuffers {
        fn default() -> Self {
            Self::NoBuffers
        }
    }

    impl From<WriteBackClientBuffers> for ProvidedBuffers {
        fn from(buffers: WriteBackClientBuffers) -> Self {
            ProvidedBuffers::Buffers(buffers)
        }
    }

    impl From<ProvidedBuffers> for WriteBackClientBuffers {
        fn from(extra: ProvidedBuffers) -> Self {
            match extra {
                ProvidedBuffers::Buffers(buffers) => buffers,
                ProvidedBuffers::NoBuffers => Default::default(),
            }
        }
    }

    impl From<ProvidedBuffers> for () {
        fn from(_: ProvidedBuffers) -> Self {
            ()
        }
    }

    impl From<()> for ProvidedBuffers {
        fn from(_: ()) -> Self {
            Default::default()
        }
    }

    impl NonSyncContext for TcpNonSyncCtx {
        type ReceiveBuffer = Rc<RefCell<RingBuffer>>;
        type SendBuffer = TestSendBuffer;
        type ReturnedBuffers = ClientBuffers;
        type ListenerNotifierOrProvidedBuffers = ProvidedBuffers;

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

        fn default_buffer_sizes() -> BufferSizes {
            BufferSizes::default()
        }
    }

    #[derive(Debug, Default, Clone, PartialEq, Eq)]
    pub struct WriteBackClientBuffers(Rc<RefCell<Option<ClientBuffers>>>);

    impl IntoBuffers<Rc<RefCell<RingBuffer>>, TestSendBuffer> for ProvidedBuffers {
        fn into_buffers(
            self,
            buffer_sizes: BufferSizes,
        ) -> (Rc<RefCell<RingBuffer>>, TestSendBuffer) {
            let buffers = ClientBuffers::new(buffer_sizes);
            if let ProvidedBuffers::Buffers(b) = self {
                *b.0.as_ref().borrow_mut() = Some(buffers.clone());
            }
            let ClientBuffers { receive, send } = buffers;
            (receive, TestSendBuffer::new(send, Default::default()))
        }
    }

    impl ListenerNotifier for ProvidedBuffers {
        fn new_incoming_connections(&mut self, _: usize) {}
    }

    impl<I: TcpTestIpExt, D: FakeStrongDeviceId + 'static> DeviceIpSocketHandler<I, TcpNonSyncCtx>
        for FakeBufferIpTransportCtx<I, D>
    {
        fn get_mms<O: SendOptions<I>>(
            &mut self,
            _ctx: &mut TcpNonSyncCtx,
            _ip_sock: &IpSock<I, FakeWeakDeviceId<D>, O>,
        ) -> Result<Mms, MmsError> {
            Ok(Mms::from_mtu::<I>(Mtu::new(1500), 0).unwrap())
        }
    }

    impl<I: TcpTestIpExt, D: FakeStrongDeviceId + 'static> SyncContext<I, TcpNonSyncCtx>
        for TcpSyncCtx<I, D>
    {
        type IpTransportCtx<'a> = FakeBufferIpTransportCtx<I, D>;

        fn with_ip_transport_ctx_isn_generator_and_tcp_sockets_mut<
            O,
            F: FnOnce(
                &mut FakeBufferIpTransportCtx<I, D>,
                &IsnGenerator<FakeInstant>,
                &mut Sockets<I, FakeWeakDeviceId<D>, TcpNonSyncCtx>,
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

        fn with_tcp_sockets<O, F: FnOnce(&Sockets<I, FakeWeakDeviceId<D>, TcpNonSyncCtx>) -> O>(
            &mut self,
            cb: F,
        ) -> O {
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
                        I::new_device_state([*addr], prefix),
                        alloc::vec![peer],
                    )),
                )),
                FakeTcpState::default(),
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

        fn new_device_state(
            addrs: impl IntoIterator<Item = Self::Addr>,
            prefix: u8,
        ) -> IpDeviceState<FakeInstant, Self> {
            let device_state = IpDeviceState::default();
            for addr in addrs {
                let _addr_id = device_state
                    .addrs
                    .write()
                    .add(Ipv4AddressEntry::new(
                        AddrSubnet::new(addr, prefix).unwrap(),
                        Ipv4AddrConfig::default(),
                    ))
                    .expect("failed to add address");
            }
            device_state
        }
    }

    impl TcpTestIpExt for Ipv6 {
        fn recv_src_addr(addr: Self::Addr) -> Self::RecvSrcAddr {
            Ipv6SourceAddr::new(addr).unwrap()
        }

        fn new_device_state(
            addrs: impl IntoIterator<Item = Self::Addr>,
            prefix: u8,
        ) -> IpDeviceState<FakeInstant, Self> {
            let device_state = IpDeviceState::default();
            for addr in addrs {
                let _addr_id = device_state
                    .addrs
                    .write()
                    .add(Ipv6AddressEntry::new(
                        AddrSubnet::new(addr, prefix).unwrap(),
                        Ipv6DadState::Assigned,
                        Ipv6AddrConfig::default(),
                    ))
                    .expect("failed to add address");
            }
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

    impl<I: TcpTestIpExt, D: FakeStrongDeviceId, NewIp: TcpTestIpExt> GenericOverIp<NewIp>
        for TcpCtx<I, D>
    {
        type Type = TcpCtx<NewIp, D>;
    }

    fn handle_timer<I: Ip + TcpTestIpExt, D: FakeStrongDeviceId + 'static>(
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

    /// How to bind the client socket in `bind_listen_connect_accept_inner`.
    struct BindConfig {
        /// Which port to bind the client to.
        client_port: Option<NonZeroU16>,
        /// Which port to bind the server to.
        server_port: NonZeroU16,
        /// Whether to set REUSE_ADDR for the client.
        client_reuse_addr: bool,
    }

    /// The following test sets up two connected testing context - one as the
    /// server and the other as the client. Tests if a connection can be
    /// established using `bind`, `listen`, `connect` and `accept`.
    ///
    /// # Arguments
    ///
    /// * `listen_addr` - The address to listen on.
    /// * `bind_config` - Specifics about how to bind the client socket.
    ///
    /// # Returns
    ///
    /// Returns a tuple of
    ///   - the created test network.
    ///   - the client socket from local.
    ///   - the send end of the client socket.
    ///   - the accepted socket from remote.
    fn bind_listen_connect_accept_inner<I: Ip + TcpTestIpExt>(
        listen_addr: I::Addr,
        BindConfig { client_port, server_port, client_reuse_addr }: BindConfig,
        seed: u128,
        drop_rate: f64,
    ) -> (TcpTestNetwork<I>, ConnectionId<I>, Rc<RefCell<Vec<u8>>>, ConnectionId<I>) {
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
            let conn = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            let bound = SocketHandler::bind(
                sync_ctx,
                non_sync_ctx,
                conn,
                SpecifiedAddr::new(listen_addr).map(ZonedAddr::Unzoned),
                Some(server_port),
            )
            .expect("failed to bind the server socket");
            SocketHandler::listen(sync_ctx, bound, backlog).expect("can listen")
        });

        let client_ends = WriteBackClientBuffers::default();
        let client = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let conn = SocketHandler::create_socket(
                sync_ctx,
                non_sync_ctx,
                ProvidedBuffers::Buffers(client_ends.clone()),
            );
            if client_reuse_addr {
                SocketHandler::set_reuseaddr_unbound(sync_ctx, conn, true);
            }
            if let Some(port) = client_port {
                let conn = SocketHandler::bind(
                    sync_ctx,
                    non_sync_ctx,
                    conn,
                    Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip)),
                    Some(port),
                )
                .expect("failed to bind the client socket");
                SocketHandler::connect_bound(
                    sync_ctx,
                    non_sync_ctx,
                    conn,
                    SocketAddr {
                        ip: ZonedAddr::Unzoned(I::FAKE_CONFIG.remote_ip),
                        port: server_port,
                    },
                )
                .expect("failed to connect")
            } else {
                SocketHandler::connect_unbound(
                    sync_ctx,
                    non_sync_ctx,
                    conn,
                    ZonedAddr::Unzoned(I::FAKE_CONFIG.remote_ip),
                    server_port,
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
                Some(Listener { backlog: _, ready, pending, buffer_sizes: _, socket_options: _, notifier: _ }) => {
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
        if let Some(port) = client_port {
            assert_eq!(
                addr,
                SocketAddr { ip: ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip), port: port }
            );
        } else {
            assert_eq!(addr.ip, ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip));
        }

        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            assert_eq!(
                SocketHandler::get_handshake_status(sync_ctx, client),
                HandshakeStatus::Completed { reported: false },
            );
        });

        let mut assert_connected = |name: &'static str, conn_id: ConnectionId<I>| {
            let (conn, _, _): (_, &SharingState, &ConnAddr<_, _, _, _>) = conn_id
                .get_from_bound_state(&net.sync_ctx(name).outer.sockets.bound_state)
                .unwrap();
            assert_matches!(
                conn,
                Connection {
                    acceptor: None,
                    state: State::Established(_),
                    ip_sock: _,
                    defunct: false,
                    socket_options: _,
                    soft_error: None,
                    handshake_status: HandshakeStatus::Completed { reported: true },
                }
            )
        };

        assert_connected(LOCAL, client);
        assert_connected(REMOTE, accepted);

        let ClientBuffers { send: client_snd_end, receive: client_rcv_end } =
            client_ends.0.as_ref().borrow_mut().take().unwrap();
        let ClientBuffers { send: accepted_snd_end, receive: accepted_rcv_end } = accepted_ends;
        for snd_end in [client_snd_end.clone(), accepted_snd_end] {
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
            Some(&mut Listener::new(
                backlog,
                BufferSizes::default(),
                SocketOptions::default(),
                Default::default()
            )),
        );

        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let bound = SocketHandler::shutdown_listener(sync_ctx, non_sync_ctx, server);
            SocketHandler::remove_bound(sync_ctx, bound);
        });

        (net, client, client_snd_end, accepted)
    }

    #[ip_test]
    #[test_case(BindConfig { client_port: None, server_port: PORT_1, client_reuse_addr: false }, I::UNSPECIFIED_ADDRESS)]
    #[test_case(BindConfig { client_port: Some(PORT_1), server_port: PORT_1, client_reuse_addr: false }, I::UNSPECIFIED_ADDRESS)]
    #[test_case(BindConfig { client_port: None, server_port: PORT_1, client_reuse_addr: true }, I::UNSPECIFIED_ADDRESS)]
    #[test_case(BindConfig { client_port: Some(PORT_1), server_port: PORT_1, client_reuse_addr: true }, I::UNSPECIFIED_ADDRESS)]
    #[test_case(BindConfig { client_port: None, server_port: PORT_1, client_reuse_addr: false }, *<I as TestIpExt>::FAKE_CONFIG.remote_ip)]
    #[test_case(BindConfig { client_port: Some(PORT_1), server_port: PORT_1, client_reuse_addr: false }, *<I as TestIpExt>::FAKE_CONFIG.remote_ip)]
    #[test_case(BindConfig { client_port: None, server_port: PORT_1, client_reuse_addr: true }, *<I as TestIpExt>::FAKE_CONFIG.remote_ip)]
    #[test_case(BindConfig { client_port: Some(PORT_1), server_port: PORT_1, client_reuse_addr: true }, *<I as TestIpExt>::FAKE_CONFIG.remote_ip)]
    fn bind_listen_connect_accept<I: Ip + TcpTestIpExt>(
        bind_config: BindConfig,
        listen_addr: I::Addr,
    ) {
        set_logger_for_test();
        let (_net, _client, _client_snd_end, _accepted) =
            bind_listen_connect_accept_inner::<I>(listen_addr, bind_config, 0, 0.0);
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
        let s1 = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        let s2 = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());

        let _b1 = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            s1,
            Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip)),
            Some(PORT_1),
        )
        .expect("first bind should succeed");
        assert_matches!(
            SocketHandler::bind(
                &mut sync_ctx,
                &mut non_sync_ctx,
                s2,
                SpecifiedAddr::new(conflict_addr).map(ZonedAddr::Unzoned),
                Some(PORT_1)
            ),
            Err(LocalAddressError::AddressInUse)
        );
        let _b2 = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            s2,
            SpecifiedAddr::new(conflict_addr).map(ZonedAddr::Unzoned),
            Some(PORT_2),
        )
        .expect("able to rebind to a free address");
    }

    #[ip_test]
    #[test_case(const_unwrap_option(NonZeroU16::new(u16::MAX)), Ok(const_unwrap_option(NonZeroU16::new(u16::MAX))); "ephemeral available")]
    #[test_case(const_unwrap_option(NonZeroU16::new(100)), Err(LocalAddressError::FailedToAllocateLocalPort);
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
            let unbound =
                SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
            let bound =
                SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, None, Some(port))
                    .expect("uncontested bind");
            let _listener = SocketHandler::listen(
                &mut sync_ctx,
                bound,
                const_unwrap_option(NonZeroUsize::new(1)),
            )
            .expect("can listen");
        }

        // Now that all but the LOCAL_PORT are occupied, ask the stack to
        // select a port.
        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        let result = SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, None, None)
            .map(|bound| SocketHandler::get_bound_info(&mut sync_ctx, bound).port);
        assert_eq!(result, expected_result);
    }

    #[ip_test]
    fn bind_to_non_existent_address<I: Ip + TcpTestIpExt>() {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<I, _>::with_sync_ctx(TcpSyncCtx::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));
        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        assert_matches!(
            SocketHandler::bind(
                &mut sync_ctx,
                &mut non_sync_ctx,
                unbound,
                Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.remote_ip)),
                None
            ),
            Err(LocalAddressError::AddressMismatch)
        );

        sync_ctx.with_tcp_sockets(|sockets| {
            assert_matches!(sockets.inactive.get(unbound.into()), Some(_));
        });
    }

    #[test]
    fn bind_addr_requires_zone() {
        let local_ip = LinkLocalAddr::new(net_ip_v6!("fe80::1")).unwrap().into_specified();

        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<Ipv6, _>::with_sync_ctx(TcpSyncCtx::new(
                Ipv6::FAKE_CONFIG.local_ip,
                Ipv6::FAKE_CONFIG.remote_ip,
                Ipv6::FAKE_CONFIG.subnet.prefix(),
            ));
        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        assert_matches!(
            SocketHandler::bind(
                &mut sync_ctx,
                &mut non_sync_ctx,
                unbound,
                Some(ZonedAddr::Unzoned(local_ip)),
                None
            ),
            Err(LocalAddressError::Zone(ZonedAddressError::RequiredZoneNotProvided))
        );

        sync_ctx.with_tcp_sockets(|sockets| {
            assert_matches!(sockets.inactive.get(unbound.into()), Some(_));
        });
    }

    #[test]
    fn connect_bound_requires_zone() {
        let ll_ip = LinkLocalAddr::new(net_ip_v6!("fe80::1")).unwrap().into_specified();

        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<Ipv6, _>::with_sync_ctx(TcpSyncCtx::new(
                Ipv6::FAKE_CONFIG.local_ip,
                Ipv6::FAKE_CONFIG.remote_ip,
                Ipv6::FAKE_CONFIG.subnet.prefix(),
            ));
        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        let bound = SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, None, None)
            .expect("bind succeeds");
        assert_matches!(
            SocketHandler::connect_bound(
                &mut sync_ctx,
                &mut non_sync_ctx,
                bound,
                SocketAddr { ip: ZonedAddr::Unzoned(ll_ip), port: PORT_1 },
            ),
            Err(ConnectError::Zone(ZonedAddressError::RequiredZoneNotProvided))
        );

        sync_ctx.with_tcp_sockets(|sockets| {
            assert_matches!(bound.get_from_bound_state(&sockets.bound_state), Some(_));
        });
    }

    #[test]
    fn connect_unbound_picks_link_local_source_addr() {
        set_logger_for_test();
        let client_ip = SpecifiedAddr::new(net_ip_v6!("fe80::1")).unwrap();
        let server_ip = SpecifiedAddr::new(net_ip_v6!("1:2:3:4::")).unwrap();
        let mut net = FakeNetwork::new(
            [
                (LOCAL, TcpCtx::with_sync_ctx(TcpSyncCtx::new(client_ip, server_ip, 0))),
                (REMOTE, TcpCtx::with_sync_ctx(TcpSyncCtx::new(server_ip, client_ip, 0))),
            ],
            |net, meta| {
                if net == LOCAL {
                    alloc::vec![(REMOTE, meta, None)]
                } else {
                    alloc::vec![(LOCAL, meta, None)]
                }
            },
        );
        const PORT: NonZeroU16 = const_unwrap_option(NonZeroU16::new(100));
        let client_connection = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let socket: UnboundId<Ipv6> =
                SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                socket,
                ZonedAddr::Unzoned(server_ip),
                PORT,
            )
            .expect("can connect")
        });
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let socket = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            let bound = SocketHandler::bind(sync_ctx, non_sync_ctx, socket, None, Some(PORT))
                .expect("failed to bind the client socket");
            let _listener =
                SocketHandler::listen(sync_ctx, bound, NonZeroUsize::MIN).expect("can listen");
        });

        // Advance until the connection is established.
        net.run_until_idle(handle_frame, handle_timer);

        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            assert_eq!(
                SocketHandler::get_handshake_status(sync_ctx, client_connection),
                HandshakeStatus::Completed { reported: false },
            );

            let info = SocketHandler::get_connection_info(sync_ctx, client_connection);
            // The local address picked for the connection is link-local, which
            // means the device for the connection must also be set (since the
            // address requires a zone).
            let (local_ip, remote_ip) = assert_matches!(
                info,
                ConnectionInfo {
                    local_addr: SocketAddr { ip: ZonedAddr::Zoned(local_ip), port: _ },
                    remote_addr: SocketAddr { ip: ZonedAddr::Unzoned(remote_ip), port: PORT },
                    device: Some(FakeWeakDeviceId(FakeDeviceId))
                } => (local_ip, remote_ip)
            );
            assert_eq!(
                local_ip,
                AddrAndZone::new(client_ip.get(), FakeWeakDeviceId(FakeDeviceId)).unwrap()
            );
            assert_eq!(remote_ip, server_ip);

            // Double-check that the bound device can't be changed after being set
            // implicitly.
            assert_matches!(
                SocketHandler::set_connection_device(
                    sync_ctx,
                    non_sync_ctx,
                    client_connection,
                    None
                ),
                Err(SetDeviceError::ZoneChange)
            );
        });
    }

    #[test]
    fn accept_connect_picks_link_local_addr() {
        set_logger_for_test();
        let server_ip = SpecifiedAddr::new(net_ip_v6!("fe80::1")).unwrap();
        let client_ip = SpecifiedAddr::new(net_ip_v6!("1:2:3:4::")).unwrap();
        let mut net = FakeNetwork::new(
            [
                (LOCAL, TcpCtx::with_sync_ctx(TcpSyncCtx::new(server_ip, client_ip, 0))),
                (REMOTE, TcpCtx::with_sync_ctx(TcpSyncCtx::new(client_ip, server_ip, 0))),
            ],
            |net, meta| {
                if net == LOCAL {
                    alloc::vec![(REMOTE, meta, None)]
                } else {
                    alloc::vec![(LOCAL, meta, None)]
                }
            },
        );
        const PORT: NonZeroU16 = const_unwrap_option(NonZeroU16::new(100));
        let server_listener = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let socket: UnboundId<Ipv6> =
                SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            let bound = SocketHandler::bind(sync_ctx, non_sync_ctx, socket, None, Some(PORT))
                .expect("failed to bind the client socket");
            SocketHandler::listen(sync_ctx, bound, NonZeroUsize::MIN).expect("can listen")
        });
        let client_connection = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let socket = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                socket,
                AddrAndZone::new(server_ip.get(), FakeDeviceId).unwrap().into(),
                PORT,
            )
            .expect("failed to open a connection")
        });

        // Advance until the connection is established.
        net.run_until_idle(handle_frame, handle_timer);

        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let (server_connection, _addr, _buffers) =
                SocketHandler::accept(sync_ctx, non_sync_ctx, server_listener)
                    .expect("connection is waiting");

            let info = SocketHandler::get_connection_info(sync_ctx, server_connection);
            // The local address picked for the connection is link-local, which
            // means the device for the connection must also be set (since the
            // address requires a zone).
            let (local_ip, remote_ip) = assert_matches!(
                info,
                ConnectionInfo {
                    local_addr: SocketAddr { ip: ZonedAddr::Zoned(local_ip), port: PORT },
                    remote_addr: SocketAddr { ip: ZonedAddr::Unzoned(remote_ip), port: _ },
                    device: Some(FakeWeakDeviceId(FakeDeviceId))
                } => (local_ip, remote_ip)
            );
            assert_eq!(
                local_ip,
                AddrAndZone::new(server_ip.get(), FakeWeakDeviceId(FakeDeviceId)).unwrap()
            );
            assert_eq!(remote_ip, client_ip);

            // Double-check that the bound device can't be changed after being set
            // implicitly.
            assert_matches!(
                SocketHandler::set_connection_device(
                    sync_ctx,
                    non_sync_ctx,
                    server_connection,
                    None
                ),
                Err(SetDeviceError::ZoneChange)
            );
        });
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            assert_eq!(
                SocketHandler::get_handshake_status(sync_ctx, client_connection),
                HandshakeStatus::Completed { reported: false },
            );
        });
    }

    // The test verifies that if client tries to connect to a closed port on
    // server, the connection is aborted and RST is received.
    #[ip_test]
    fn connect_reset<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let mut net = new_test_net::<I>();

        let client = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let conn = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            let conn = SocketHandler::bind(
                sync_ctx,
                non_sync_ctx,
                conn,
                Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip)),
                Some(PORT_1),
            )
            .expect("failed to bind the client socket");
            SocketHandler::connect_bound(
                sync_ctx,
                non_sync_ctx,
                conn,
                SocketAddr { ip: ZonedAddr::Unzoned(I::FAKE_CONFIG.remote_ip), port: PORT_1 },
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
        // Finally, the connection should be reset and bindings should have been
        // signaled.
        let (conn, _, _): (_, _, &ConnAddr<_, _, _, _>) =
            client.get_from_bound_state(&net.sync_ctx(LOCAL).outer.sockets.bound_state).unwrap();
        assert_matches!(
            conn,
            Connection {
                acceptor: None,
                state: State::Closed(Closed { reason: Some(ConnectionError::ConnectionReset) }),
                ip_sock: _,
                defunct: false,
                socket_options: _,
                soft_error: None,
                handshake_status: HandshakeStatus::Aborted,
            }
        );
        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            assert_eq!(
                SocketHandler::get_handshake_status(sync_ctx, client),
                HandshakeStatus::Aborted,
            );
        });
    }

    #[ip_test]
    fn retransmission<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        run_with_many_seeds(|seed| {
            let (_net, _client, _client_snd_end, _accepted) = bind_listen_connect_accept_inner::<I>(
                I::UNSPECIFIED_ADDRESS,
                BindConfig { client_port: None, server_port: PORT_1, client_reuse_addr: false },
                seed,
                0.2,
            );
        });
    }

    const LOCAL_PORT: NonZeroU16 = const_unwrap_option(NonZeroU16::new(1845));

    #[ip_test]
    fn listener_with_bound_device_conflict<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::with_sync_ctx(TcpSyncCtx::<I, _>::new_multiple_devices());

        let bound_a =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        SocketHandler::set_unbound_device(
            &mut sync_ctx,
            &mut non_sync_ctx,
            bound_a,
            Some(MultipleDevicesId::A),
        );
        let bound_a =
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, bound_a, None, Some(LOCAL_PORT))
                .expect("bind should succeed");
        let _bound_a = SocketHandler::listen(
            &mut sync_ctx,
            bound_a,
            const_unwrap_option(NonZeroUsize::new(10)),
        )
        .expect("can listen");

        let s = SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        // Binding `s` to the unspecified address should fail since the address
        // is shadowed by `bound_a`.
        assert_matches!(
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, s, None, Some(LOCAL_PORT)),
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
        let _: BoundId<_> =
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, s, None, Some(LOCAL_PORT))
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

        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        let bound = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            Some(ZonedAddr::Zoned(AddrAndZone::new(*ll_addr, MultipleDevicesId::A).unwrap())),
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

        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        let bound = SocketHandler::connect_unbound(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            AddrAndZone::new(*ll_addr, MultipleDevicesId::A).unwrap().into(),
            LOCAL_PORT,
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
        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());

        let (addr, port) = (SpecifiedAddr::new(ip_addr).map(ZonedAddr::Unzoned), PORT_1);
        let bound =
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, addr, Some(port))
                .expect("bind should succeed");
        let info = if listen {
            let listener = SocketHandler::listen(
                &mut sync_ctx,
                bound,
                const_unwrap_option(NonZeroUsize::new(25)),
            )
            .expect("can listen");
            SocketHandler::get_listener_info(&mut sync_ctx, listener)
        } else {
            SocketHandler::get_bound_info(&mut sync_ctx, bound)
        };
        assert_eq!(
            info,
            BoundInfo { addr: addr.map(|a| a.map_zone(FakeWeakDeviceId)), port, device: None }
        );
    }

    #[ip_test]
    fn connection_info<I: Ip + TcpTestIpExt>() {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::with_sync_ctx(TcpSyncCtx::<I, _>::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));
        let local = SocketAddr { ip: ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip), port: PORT_1 };
        let remote = SocketAddr { ip: ZonedAddr::Unzoned(I::FAKE_CONFIG.remote_ip), port: PORT_2 };

        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        let bound = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            Some(local.ip),
            Some(local.port),
        )
        .expect("bind should succeed");

        let connected =
            SocketHandler::connect_bound(&mut sync_ctx, &mut non_sync_ctx, bound, remote)
                .expect("connect should succeed");

        assert_eq!(
            SocketHandler::get_connection_info(&mut sync_ctx, connected),
            ConnectionInfo {
                local_addr: local.map_zone(FakeWeakDeviceId),
                remote_addr: remote.map_zone(FakeWeakDeviceId),
                device: None,
            },
        );
    }

    #[test_case(true; "any")]
    #[test_case(false; "link local")]
    fn accepted_connection_info_zone(listen_any: bool) {
        set_logger_for_test();
        let client_ip = SpecifiedAddr::new(net_ip_v6!("fe80::1")).unwrap();
        let server_ip = SpecifiedAddr::new(net_ip_v6!("fe80::2")).unwrap();
        let mut net = FakeNetwork::new(
            [
                (
                    LOCAL,
                    TcpCtx::with_sync_ctx(TcpSyncCtx::new(
                        server_ip,
                        client_ip,
                        Ipv6::LINK_LOCAL_UNICAST_SUBNET.prefix(),
                    )),
                ),
                (
                    REMOTE,
                    TcpCtx::with_sync_ctx(TcpSyncCtx::new(
                        client_ip,
                        server_ip,
                        Ipv6::LINK_LOCAL_UNICAST_SUBNET.prefix(),
                    )),
                ),
            ],
            move |net, meta: SendIpPacketMeta<_, _, _>| {
                if net == LOCAL {
                    alloc::vec![(REMOTE, meta, None)]
                } else {
                    alloc::vec![(LOCAL, meta, None)]
                }
            },
        );

        let local_server = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound =
                SocketHandler::<Ipv6, _>::create_socket(sync_ctx, non_sync_ctx, Default::default());
            let device = FakeDeviceId;
            let bind_addr = match listen_any {
                true => None,
                false => Some(ZonedAddr::Zoned(AddrAndZone::new(*server_ip, device).unwrap())),
            };
            let bind =
                SocketHandler::bind(sync_ctx, non_sync_ctx, unbound, bind_addr, Some(PORT_1))
                    .expect("failed to bind the client socket");
            SocketHandler::listen(sync_ctx, bind, const_unwrap_option(NonZeroUsize::new(1)))
                .expect("can listen")
        });

        let _remote_client = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            let device = FakeDeviceId;
            SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                unbound,
                ZonedAddr::Zoned(AddrAndZone::new(*server_ip, device).unwrap()),
                PORT_1,
            )
            .expect("failed to connect")
        });

        net.run_until_idle(handle_frame, handle_timer);

        let ConnectionInfo { remote_addr, local_addr, device } =
            net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
                let (server_conn, _addr, _buffers) =
                    SocketHandler::accept(sync_ctx, non_sync_ctx, local_server)
                        .expect("connection is available");
                SocketHandler::get_connection_info(sync_ctx, server_conn)
            });

        let device = assert_matches!(device, Some(device) => device);
        assert_eq!(
            local_addr,
            SocketAddr {
                ip: ZonedAddr::Zoned(AddrAndZone::new(*server_ip, device).unwrap()),
                port: PORT_1
            }
        );
        let SocketAddr { ip: remote_ip, port: _ } = remote_addr;
        assert_eq!(remote_ip, ZonedAddr::Zoned(AddrAndZone::new(*client_ip, device).unwrap()));
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

        let local_addr = SocketAddr {
            ip: ZonedAddr::Zoned(AddrAndZone::new(*local_ip, FakeDeviceId).unwrap()),
            port: PORT_1,
        };
        let remote_addr = SocketAddr {
            ip: ZonedAddr::Zoned(AddrAndZone::new(*remote_ip, FakeDeviceId).unwrap()),
            port: PORT_2,
        };

        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        let bound = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            Some(local_addr.ip),
            Some(local_addr.port),
        )
        .expect("bind should succeed");

        assert_eq!(
            SocketHandler::get_bound_info(&mut sync_ctx, bound),
            BoundInfo {
                addr: Some(local_addr.ip.map_zone(FakeWeakDeviceId)),
                port: local_addr.port,
                device: Some(FakeWeakDeviceId(FakeDeviceId))
            }
        );

        let connected =
            SocketHandler::connect_bound(&mut sync_ctx, &mut non_sync_ctx, bound, remote_addr)
                .expect("connect should succeed");

        assert_eq!(
            SocketHandler::get_connection_info(&mut sync_ctx, connected),
            ConnectionInfo {
                local_addr: local_addr.map_zone(FakeWeakDeviceId),
                remote_addr: remote_addr.map_zone(FakeWeakDeviceId),
                device: Some(FakeWeakDeviceId(FakeDeviceId))
            }
        );
    }

    #[ip_test]
    // Assuming instant delivery of segments:
    // - If peer calls close, then the timeout we need to wait is in
    // TIME_WAIT, which is 2MSL.
    #[test_case(true, 2 * MSL; "peer calls close")]
    // - If not, we will be in the FIN_WAIT2 state and waiting for its
    // timeout.
    #[test_case(false, DEFAULT_FIN_WAIT2_TIMEOUT; "peer doesn't call close")]
    fn connection_close_peer_calls_close<I: Ip + TcpTestIpExt>(
        peer_calls_close: bool,
        expected_time_to_close: Duration,
    ) {
        set_logger_for_test();
        let (mut net, local, _local_snd_end, remote) = bind_listen_connect_accept_inner::<I>(
            I::UNSPECIFIED_ADDRESS,
            BindConfig { client_port: None, server_port: PORT_1, client_reuse_addr: false },
            0,
            0.0,
        );
        let close_called = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            SocketHandler::close_conn(sync_ctx, non_sync_ctx, local);
            non_sync_ctx.now()
        });
        let local = MaybeClosedConnectionId::from(local);

        while {
            assert!(!net.step(handle_frame, handle_timer).is_idle());
            let is_fin_wait_2 = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
                sync_ctx.with_tcp_sockets(|sockets| {
                    let (conn, _, _addr) =
                        local.get_from_bound_state(&sockets.bound_state).expect("invalid conn ID");
                    matches!(conn.state, State::FinWait2(_))
                })
            });
            !is_fin_wait_2
        } {}

        if peer_calls_close {
            net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
                SocketHandler::close_conn(sync_ctx, non_sync_ctx, remote);
            });
        }
        let remote = MaybeClosedConnectionId::from(remote);

        net.run_until_idle(handle_frame, handle_timer);

        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            assert_eq!(non_sync_ctx.now().duration_since(close_called), expected_time_to_close);
            sync_ctx.with_tcp_sockets(|sockets| {
                assert_matches!(local.get_from_bound_state(&sockets.bound_state), None);
            })
        });
        if peer_calls_close {
            net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
                sync_ctx.with_tcp_sockets(|sockets| {
                    assert_matches!(remote.get_from_bound_state(&sockets.bound_state), None);
                })
            });
        }
    }

    #[ip_test]
    fn connection_shutdown_then_close_peer_doesnt_call_close<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let (mut net, local, _local_snd_end, _remote) = bind_listen_connect_accept_inner::<I>(
            I::UNSPECIFIED_ADDRESS,
            BindConfig { client_port: None, server_port: PORT_1, client_reuse_addr: false },
            0,
            0.0,
        );
        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            assert_eq!(SocketHandler::shutdown_conn(sync_ctx, non_sync_ctx, local), Ok(()));
        });
        loop {
            assert!(!net.step(handle_frame, handle_timer).is_idle());
            let is_fin_wait_2 = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
                sync_ctx.with_tcp_sockets(|sockets| {
                    let (conn, _, _addr) =
                        local.get_from_bound_state(&sockets.bound_state).unwrap();
                    matches!(conn.state, State::FinWait2(_))
                })
            });
            if is_fin_wait_2 {
                break;
            }
        }
        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            SocketHandler::close_conn(sync_ctx, non_sync_ctx, local);
        });
        net.run_until_idle(handle_frame, handle_timer);
        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            sync_ctx.with_tcp_sockets(|sockets| {
                assert_matches!(local.get_from_bound_state(&sockets.bound_state), None);
            })
        });
    }

    #[ip_test]
    fn connection_shutdown_then_close<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let (mut net, local, _local_snd_end, remote) = bind_listen_connect_accept_inner::<I>(
            I::UNSPECIFIED_ADDRESS,
            BindConfig { client_port: None, server_port: PORT_1, client_reuse_addr: false },
            0,
            0.0,
        );

        for (name, id) in [(LOCAL, local), (REMOTE, remote)] {
            net.with_context(name, |TcpCtx { sync_ctx, non_sync_ctx }| {
                assert_matches!(SocketHandler::shutdown_conn(sync_ctx, non_sync_ctx, id), Ok(()));
                sync_ctx.with_tcp_sockets(|sockets| {
                    let (conn, _, _addr) = id.get_from_bound_state(&sockets.bound_state).unwrap();
                    assert_matches!(conn.state, State::FinWait1(_));
                });
                assert_matches!(SocketHandler::shutdown_conn(sync_ctx, non_sync_ctx, id), Ok(()));
            });
        }
        net.run_until_idle(handle_frame, handle_timer);
        for (name, id) in [(LOCAL, local), (REMOTE, remote)] {
            net.with_context(name, |TcpCtx { sync_ctx, non_sync_ctx }| {
                sync_ctx.with_tcp_sockets(|sockets| {
                    let (conn, _, _addr) = id.get_from_bound_state(&sockets.bound_state).unwrap();
                    assert_matches!(conn.state, State::Closed(_));
                });
                SocketHandler::close_conn(sync_ctx, non_sync_ctx, id);
                sync_ctx.with_tcp_sockets(|sockets| {
                    assert_matches!(id.get_from_bound_state(&sockets.bound_state), None);
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
        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        SocketHandler::remove_unbound(&mut sync_ctx, unbound);

        sync_ctx.with_tcp_sockets(
            |Sockets { bound_state: _, socketmap: _, inactive, port_alloc: _ }| {
                assert_eq!(inactive.get(unbound.into()), None);
            },
        )
    }

    #[ip_test]
    fn remove_bound<I: Ip + TcpTestIpExt>() {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::with_sync_ctx(TcpSyncCtx::<I, _>::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));
        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        let bound = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip)),
            None,
        )
        .expect("bind should succeed");
        SocketHandler::remove_bound(&mut sync_ctx, bound);

        sync_ctx.with_tcp_sockets(
            |Sockets { bound_state, socketmap: _, inactive, port_alloc: _ }| {
                assert_eq!(inactive.get(unbound.into()), None);
                assert_eq!(bound.get_from_bound_state(bound_state), None);
            },
        )
    }

    #[ip_test]
    fn shutdown_listener<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let mut net = new_test_net::<I>();
        let local_listener = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            let bound = SocketHandler::bind(
                sync_ctx,
                non_sync_ctx,
                unbound,
                Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip)),
                Some(PORT_1),
            )
            .expect("bind should succeed");
            SocketHandler::listen(sync_ctx, bound, NonZeroUsize::new(5).unwrap())
                .expect("can listen")
        });

        let remote_connection = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                unbound,
                ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip),
                PORT_1,
            )
            .expect("connect should succeed")
        });

        // After the following step, we should have one established connection
        // in the listener's accept queue, which ought to be aborted during
        // shutdown.
        net.run_until_idle(handle_frame, handle_timer);

        // The incoming connection was signaled, and the remote end was notified
        // of connection establishment.
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            assert_eq!(
                SocketHandler::get_handshake_status(sync_ctx, remote_connection),
                HandshakeStatus::Completed { reported: false },
            );
        });

        // Create a second half-open connection so that we have one entry in the
        // pending queue.
        let second_connection = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                unbound,
                ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip),
                PORT_1,
            )
            .expect("connect should succeed")
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

        // Both remote sockets should now be reset to Closed state.
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            for conn in [remote_connection, second_connection] {
                assert_eq!(
                    SocketHandler::get_connection_error(sync_ctx, conn),
                    Some(ConnectionError::ConnectionReset),
                )
            }

            sync_ctx.with_tcp_sockets(|sockets| {
                let (conn, _, _addr) =
                    remote_connection.get_from_bound_state(&sockets.bound_state).unwrap();
                assert_matches!(
                    conn.state,
                    State::Closed(Closed { reason: Some(ConnectionError::ConnectionReset) })
                );
            });
        });

        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let new_unbound =
                SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            assert_matches!(
                SocketHandler::bind(
                    sync_ctx,
                    non_sync_ctx,
                    new_unbound,
                    Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip,)),
                    Some(PORT_1),
                ),
                Err(LocalAddressError::AddressInUse)
            );
            // Bring the already-shutdown listener back to listener again.
            let _: ListenerId<_> =
                SocketHandler::listen(sync_ctx, local_bound, NonZeroUsize::new(5).unwrap())
                    .expect("can listen again");
        });

        let new_remote_connection =
            net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
                let unbound =
                    SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
                SocketHandler::connect_unbound(
                    sync_ctx,
                    non_sync_ctx,
                    unbound,
                    ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip),
                    PORT_1,
                )
                .expect("connect should succeed")
            });

        net.run_until_idle(handle_frame, handle_timer);

        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            sync_ctx.with_tcp_sockets(|sockets| {
                let (conn, _, _addr) =
                    new_remote_connection.get_from_bound_state(&sockets.bound_state).unwrap();
                assert_matches!(conn.state, State::Established(_));
            });
            assert_eq!(
                SocketHandler::get_handshake_status(sync_ctx, new_remote_connection),
                HandshakeStatus::Completed { reported: false },
            );
        });
    }

    #[ip_test]
    fn set_buffer_size<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let mut net = new_test_net::<I>();

        let mut local_sizes = BufferSizes { send: 2048, receive: 2000 };
        let mut remote_sizes = BufferSizes { send: 1024, receive: 2000 };

        let local_listener = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            let bound = SocketHandler::bind(
                sync_ctx,
                non_sync_ctx,
                unbound,
                Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip)),
                Some(PORT_1),
            )
            .expect("bind should succeed");
            SocketHandler::set_send_buffer_size(sync_ctx, non_sync_ctx, bound, local_sizes.send);
            SocketHandler::set_receive_buffer_size(
                sync_ctx,
                non_sync_ctx,
                bound,
                local_sizes.receive,
            );
            SocketHandler::listen(sync_ctx, bound, NonZeroUsize::new(5).unwrap())
                .expect("can listen")
        });

        let remote_connection = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            SocketHandler::set_send_buffer_size(sync_ctx, non_sync_ctx, unbound, remote_sizes.send);
            SocketHandler::set_receive_buffer_size(
                sync_ctx,
                non_sync_ctx,
                unbound,
                local_sizes.receive,
            );
            SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                unbound,
                ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip),
                PORT_1,
            )
            .expect("connect should succeed")
        });
        let mut step_and_increment_buffer_sizes_until_idle =
            |net: &mut TcpTestNetwork<I>, local: SocketId<_>, remote: SocketId<_>| loop {
                local_sizes.send += 1;
                local_sizes.receive += 1;
                remote_sizes.send += 1;
                remote_sizes.receive += 1;
                net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
                    SocketHandler::set_send_buffer_size(
                        sync_ctx,
                        non_sync_ctx,
                        local,
                        local_sizes.send,
                    );
                    if let Some(size) =
                        SocketHandler::send_buffer_size(sync_ctx, non_sync_ctx, local)
                    {
                        assert_eq!(size, local_sizes.send);
                    }
                    SocketHandler::set_receive_buffer_size(
                        sync_ctx,
                        non_sync_ctx,
                        local,
                        local_sizes.receive,
                    );
                    if let Some(size) =
                        SocketHandler::receive_buffer_size(sync_ctx, non_sync_ctx, local)
                    {
                        assert_eq!(size, local_sizes.receive);
                    }
                });
                net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
                    SocketHandler::set_send_buffer_size(
                        sync_ctx,
                        non_sync_ctx,
                        remote,
                        remote_sizes.send,
                    );
                    if let Some(size) =
                        SocketHandler::send_buffer_size(sync_ctx, non_sync_ctx, remote)
                    {
                        assert_eq!(size, remote_sizes.send);
                    }
                    SocketHandler::set_receive_buffer_size(
                        sync_ctx,
                        non_sync_ctx,
                        remote,
                        remote_sizes.receive,
                    );
                    if let Some(size) =
                        SocketHandler::receive_buffer_size(sync_ctx, non_sync_ctx, remote)
                    {
                        assert_eq!(size, remote_sizes.receive);
                    }
                });
                if net.step(handle_frame, handle_timer).is_idle() {
                    break;
                }
            };

        // Set the send buffer size at each stage of sockets on both ends of the
        // handshake process just to make sure it doesn't break.
        step_and_increment_buffer_sizes_until_idle(
            &mut net,
            local_listener.into(),
            remote_connection.into(),
        );

        let local_connection = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let (conn, _, _) = SocketHandler::accept(sync_ctx, non_sync_ctx, local_listener)
                .expect("received connection");
            conn
        });

        step_and_increment_buffer_sizes_until_idle(
            &mut net,
            local_connection.into(),
            remote_connection.into(),
        );

        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            SocketHandler::shutdown_conn(sync_ctx, non_sync_ctx, local_connection)
                .expect("is connected");
        });

        step_and_increment_buffer_sizes_until_idle(
            &mut net,
            local_connection.into(),
            remote_connection.into(),
        );

        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            SocketHandler::shutdown_conn(sync_ctx, non_sync_ctx, remote_connection)
                .expect("is connected");
        });

        step_and_increment_buffer_sizes_until_idle(
            &mut net,
            local_connection.into(),
            remote_connection.into(),
        );
    }

    #[ip_test]
    fn set_reuseaddr_unbound<I: Ip + TcpTestIpExt>() {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<I, _>::with_sync_ctx(TcpSyncCtx::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));

        let first_bound = {
            let unbound =
                SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
            SocketHandler::set_reuseaddr_unbound(&mut sync_ctx, unbound, true);
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, None, None)
                .expect("bind succeeds")
        };
        let _second_bound = {
            let unbound =
                SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
            SocketHandler::set_reuseaddr_unbound(&mut sync_ctx, unbound, true);
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, None, None)
                .expect("bind succeeds")
        };

        let _listen = SocketHandler::listen(
            &mut sync_ctx,
            first_bound,
            const_unwrap_option(NonZeroUsize::new(10)),
        )
        .expect("can listen");
    }

    #[ip_test]
    #[test_case([true, true], Ok(()); "allowed with set")]
    #[test_case([false, true], Err(LocalAddressError::AddressInUse); "first unset")]
    #[test_case([true, false], Err(LocalAddressError::AddressInUse); "second unset")]
    #[test_case([false, false], Err(LocalAddressError::AddressInUse); "both unset")]
    fn reuseaddr_multiple_bound<I: Ip + TcpTestIpExt>(
        set_reuseaddr: [bool; 2],
        expected: Result<(), LocalAddressError>,
    ) {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<I, _>::with_sync_ctx(TcpSyncCtx::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));

        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        SocketHandler::set_reuseaddr_unbound(&mut sync_ctx, unbound, set_reuseaddr[0]);
        let _first_bound =
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, None, Some(PORT_1))
                .expect("bind succeeds");

        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        SocketHandler::set_reuseaddr_unbound(&mut sync_ctx, unbound, set_reuseaddr[1]);
        let second_bind_result =
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, None, Some(PORT_1));

        assert_eq!(second_bind_result.map(|_: BoundId<I>| ()), expected);
    }

    #[ip_test]
    fn toggle_reuseaddr_bound_different_addrs<I: Ip + TcpTestIpExt>() {
        let addrs = [1, 2].map(|i| I::get_other_ip_address(i));
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<I, _>::with_sync_ctx(TcpSyncCtx::with_inner_and_outer_state(
                FakeBufferIpSocketCtx::with_ctx(FakeIpSocketCtx::<I, _>::with_devices_state(
                    core::iter::once((
                        FakeDeviceId,
                        I::new_device_state(
                            addrs.iter().map(Witness::get),
                            I::FAKE_CONFIG.subnet.prefix(),
                        ),
                        vec![],
                    )),
                )),
                FakeTcpState::default(),
            ));

        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        let first = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            Some(ZonedAddr::Unzoned(addrs[0])),
            Some(PORT_1),
        )
        .unwrap();

        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        let _second = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            Some(ZonedAddr::Unzoned(addrs[1])),
            Some(PORT_1),
        )
        .unwrap();
        // Setting and un-setting ReuseAddr should be fine since these sockets
        // don't conflict.
        SocketHandler::set_reuseaddr_bound(&mut sync_ctx, first, true).expect("can set");
        SocketHandler::set_reuseaddr_bound(&mut sync_ctx, first, false).expect("can un-set");
    }

    #[ip_test]
    fn unset_reuseaddr_bound_unspecified_specified<I: Ip + TcpTestIpExt>() {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<I, _>::with_sync_ctx(TcpSyncCtx::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));
        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        SocketHandler::set_reuseaddr_unbound(&mut sync_ctx, unbound, true);
        let first = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip)),
            Some(PORT_1),
        )
        .unwrap();

        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        SocketHandler::set_reuseaddr_unbound(&mut sync_ctx, unbound, true);
        let second =
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, None, Some(PORT_1))
                .unwrap();

        // Both sockets can be bound because they have ReuseAddr set. Since
        // removing it would introduce inconsistent state, that's not allowed.
        assert_matches!(
            SocketHandler::set_reuseaddr_bound(&mut sync_ctx, first, false),
            Err(SetReuseAddrError)
        );
        assert_matches!(
            SocketHandler::set_reuseaddr_bound(&mut sync_ctx, second, false),
            Err(SetReuseAddrError)
        );
    }

    #[ip_test]
    fn reuseaddr_allows_binding_under_connection<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        let mut net = new_test_net::<I>();

        let server = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            SocketHandler::set_reuseaddr_unbound(sync_ctx, unbound, true);
            let bound = SocketHandler::bind(
                sync_ctx,
                non_sync_ctx,
                unbound,
                Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip)),
                Some(PORT_1),
            )
            .expect("failed to bind the client socket");
            SocketHandler::listen(sync_ctx, bound, const_unwrap_option(NonZeroUsize::new(10)))
                .expect("can listen")
        });

        let client = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                unbound,
                ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip),
                PORT_1,
            )
            .expect("connect should succeed")
        });
        // Finish the connection establishment.
        net.run_until_idle(handle_frame, handle_timer);
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            assert_eq!(
                SocketHandler::get_handshake_status(sync_ctx, client),
                HandshakeStatus::Completed { reported: false },
            );
        });

        // Now accept the connection and close the listening socket. Then
        // binding a new socket on the same local address should fail unless the
        // socket has SO_REUSEADDR set.
        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let (_server_conn, _, _): (_, SocketAddr<_, _>, ClientBuffers) =
                SocketHandler::accept(sync_ctx, non_sync_ctx, server).expect("pending connection");

            let server = SocketHandler::shutdown_listener(sync_ctx, non_sync_ctx, server);
            SocketHandler::remove_bound(sync_ctx, server);

            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            assert_eq!(
                SocketHandler::bind(sync_ctx, non_sync_ctx, unbound, None, Some(PORT_1)),
                Err(LocalAddressError::AddressInUse)
            );

            // Binding should succeed after setting ReuseAddr.
            SocketHandler::set_reuseaddr_unbound(sync_ctx, unbound, true);
            assert_matches!(
                SocketHandler::bind(sync_ctx, non_sync_ctx, unbound, None, Some(PORT_1)),
                Ok(_)
            );
        });
    }

    #[ip_test]
    #[test_case([true, true]; "specified specified")]
    #[test_case([false, true]; "any specified")]
    #[test_case([true, false]; "specified any")]
    #[test_case([false, false]; "any any")]
    fn set_reuseaddr_bound_allows_other_bound<I: Ip + TcpTestIpExt>(bind_specified: [bool; 2]) {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<I, _>::with_sync_ctx(TcpSyncCtx::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));

        let [first_addr, second_addr] =
            bind_specified.map(|b| b.then_some(I::FAKE_CONFIG.local_ip).map(ZonedAddr::Unzoned));
        let first_bound = {
            let unbound =
                SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, first_addr, Some(PORT_1))
                .expect("bind succeeds")
        };

        let second =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());

        // Binding the second socket will fail because the first doesn't have
        // SO_REUSEADDR set.
        assert_matches!(
            SocketHandler::bind(
                &mut sync_ctx,
                &mut non_sync_ctx,
                second,
                second_addr,
                Some(PORT_1)
            ),
            Err(LocalAddressError::AddressInUse)
        );

        // Setting SO_REUSEADDR for the second socket isn't enough.
        SocketHandler::set_reuseaddr_unbound(&mut sync_ctx, second, true);
        assert_matches!(
            SocketHandler::bind(
                &mut sync_ctx,
                &mut non_sync_ctx,
                second,
                second_addr,
                Some(PORT_1)
            ),
            Err(LocalAddressError::AddressInUse)
        );

        // Setting SO_REUSEADDR for the first socket lets the second bind.
        SocketHandler::set_reuseaddr_bound(&mut sync_ctx, first_bound, true).expect("only socket");
        let _second_bound = SocketHandler::bind(
            &mut sync_ctx,
            &mut non_sync_ctx,
            second,
            second_addr,
            Some(PORT_1),
        )
        .expect("can bind");
    }

    #[ip_test]
    fn clear_reuseaddr_listener<I: Ip + TcpTestIpExt>() {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<I, _>::with_sync_ctx(TcpSyncCtx::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));

        let bound = {
            let unbound =
                SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
            SocketHandler::set_reuseaddr_unbound(&mut sync_ctx, unbound, true);
            SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, None, Some(PORT_1))
                .expect("bind succeeds")
        };

        let listener = {
            let unbound =
                SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
            SocketHandler::set_reuseaddr_unbound(&mut sync_ctx, unbound, true);
            let bound =
                SocketHandler::bind(&mut sync_ctx, &mut non_sync_ctx, unbound, None, Some(PORT_1))
                    .expect("bind succeeds");
            SocketHandler::listen(&mut sync_ctx, bound, const_unwrap_option(NonZeroUsize::new(5)))
                .expect("can listen")
        };

        // We can't clear SO_REUSEADDR on the listener because it's sharing with
        // the bound socket.
        assert_matches!(
            SocketHandler::set_reuseaddr_listener(&mut sync_ctx, listener, false),
            Err(SetReuseAddrError)
        );

        // We can, however, connect to the listener with the bound socket. Then
        // the unencumbered listener can clear SO_REUSEADDR.
        let _connected = SocketHandler::connect_bound(
            &mut sync_ctx,
            &mut non_sync_ctx,
            bound,
            SocketAddr { ip: ZonedAddr::Unzoned(I::FAKE_CONFIG.remote_ip), port: PORT_1 },
        )
        .expect("can connect");
        SocketHandler::set_reuseaddr_listener(&mut sync_ctx, listener, false).expect("can unset")
    }

    fn deliver_icmp_error<
        I: TcpTestIpExt + IcmpIpExt,
        SC: SyncContext<I, C, DeviceId = FakeDeviceId>,
        C: NonSyncContext,
    >(
        sync_ctx: &mut SC,
        non_sync_ctx: &mut C,
        original_src_ip: SpecifiedAddr<I::Addr>,
        original_dst_ip: SpecifiedAddr<I::Addr>,
        original_body: &[u8],
        err: I::ErrorCode,
    ) {
        <TcpIpTransportContext as IpTransportContext<I, _, _>>::receive_icmp_error(
            sync_ctx,
            non_sync_ctx,
            &FakeDeviceId,
            Some(original_src_ip),
            original_dst_ip,
            original_body,
            err,
        );
    }

    #[test_case(Icmpv4DestUnreachableCode::DestNetworkUnreachable => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::DestHostUnreachable => ConnectionError::HostUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::DestProtocolUnreachable => ConnectionError::ProtocolUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::DestPortUnreachable => ConnectionError::PortUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::SourceRouteFailed => ConnectionError::SourceRouteFailed)]
    #[test_case(Icmpv4DestUnreachableCode::DestNetworkUnknown => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::DestHostUnknown => ConnectionError::DestinationHostDown)]
    #[test_case(Icmpv4DestUnreachableCode::SourceHostIsolated => ConnectionError::SourceHostIsolated)]
    #[test_case(Icmpv4DestUnreachableCode::NetworkAdministrativelyProhibited => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::HostAdministrativelyProhibited => ConnectionError::HostUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::NetworkUnreachableForToS => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::HostUnreachableForToS => ConnectionError::HostUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::CommAdministrativelyProhibited => ConnectionError::HostUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::HostPrecedenceViolation => ConnectionError::HostUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::PrecedenceCutoffInEffect => ConnectionError::HostUnreachable)]
    fn icmp_destination_unreachable_connect_v4(
        error: Icmpv4DestUnreachableCode,
    ) -> ConnectionError {
        icmp_destination_unreachable_connect_inner::<Ipv4>(Icmpv4ErrorCode::DestUnreachable(error))
    }

    #[test_case(Icmpv6DestUnreachableCode::NoRoute => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv6DestUnreachableCode::CommAdministrativelyProhibited => ConnectionError::HostUnreachable)]
    #[test_case(Icmpv6DestUnreachableCode::BeyondScope => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv6DestUnreachableCode::AddrUnreachable => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv6DestUnreachableCode::PortUnreachable => ConnectionError::PortUnreachable)]
    #[test_case(Icmpv6DestUnreachableCode::SrcAddrFailedPolicy => ConnectionError::SourceRouteFailed)]
    #[test_case(Icmpv6DestUnreachableCode::RejectRoute => ConnectionError::NetworkUnreachable)]
    fn icmp_destination_unreachable_connect_v6(
        error: Icmpv6DestUnreachableCode,
    ) -> ConnectionError {
        icmp_destination_unreachable_connect_inner::<Ipv6>(Icmpv6ErrorCode::DestUnreachable(error))
    }

    fn icmp_destination_unreachable_connect_inner<I: TcpTestIpExt + IcmpIpExt>(
        icmp_error: I::ErrorCode,
    ) -> ConnectionError {
        let TcpCtx { mut sync_ctx, mut non_sync_ctx } =
            TcpCtx::<I, _>::with_sync_ctx(TcpSyncCtx::new(
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.subnet.prefix(),
            ));

        let unbound =
            SocketHandler::create_socket(&mut sync_ctx, &mut non_sync_ctx, Default::default());
        let connection = SocketHandler::connect_unbound(
            &mut sync_ctx,
            &mut non_sync_ctx,
            unbound,
            ZonedAddr::Unzoned(I::FAKE_CONFIG.remote_ip),
            PORT_1,
        )
        .expect("failed to create a connection socket");
        let frames = sync_ctx.inner.take_frames();
        let frame = assert_matches!(&frames[..], [(_meta, frame)] => frame);
        deliver_icmp_error(
            &mut sync_ctx,
            &mut non_sync_ctx,
            I::FAKE_CONFIG.local_ip,
            I::FAKE_CONFIG.remote_ip,
            &frame[0..8],
            icmp_error,
        );
        // The TCP handshake should be aborted.
        assert_eq!(
            SocketHandler::get_handshake_status(&mut sync_ctx, connection),
            HandshakeStatus::Aborted,
        );
        SocketHandler::get_connection_error(&mut sync_ctx, connection).unwrap()
    }

    #[test_case(Icmpv4DestUnreachableCode::DestNetworkUnreachable => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::DestHostUnreachable => ConnectionError::HostUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::DestProtocolUnreachable => ConnectionError::ProtocolUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::DestPortUnreachable => ConnectionError::PortUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::SourceRouteFailed => ConnectionError::SourceRouteFailed)]
    #[test_case(Icmpv4DestUnreachableCode::DestNetworkUnknown => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::DestHostUnknown => ConnectionError::DestinationHostDown)]
    #[test_case(Icmpv4DestUnreachableCode::SourceHostIsolated => ConnectionError::SourceHostIsolated)]
    #[test_case(Icmpv4DestUnreachableCode::NetworkAdministrativelyProhibited => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::HostAdministrativelyProhibited => ConnectionError::HostUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::NetworkUnreachableForToS => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::HostUnreachableForToS => ConnectionError::HostUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::CommAdministrativelyProhibited => ConnectionError::HostUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::HostPrecedenceViolation => ConnectionError::HostUnreachable)]
    #[test_case(Icmpv4DestUnreachableCode::PrecedenceCutoffInEffect => ConnectionError::HostUnreachable)]
    fn icmp_destination_unreachable_established_v4(
        error: Icmpv4DestUnreachableCode,
    ) -> ConnectionError {
        icmp_destination_unreachable_established_inner::<Ipv4>(Icmpv4ErrorCode::DestUnreachable(
            error,
        ))
    }

    #[test_case(Icmpv6DestUnreachableCode::NoRoute => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv6DestUnreachableCode::CommAdministrativelyProhibited => ConnectionError::HostUnreachable)]
    #[test_case(Icmpv6DestUnreachableCode::BeyondScope => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv6DestUnreachableCode::AddrUnreachable => ConnectionError::NetworkUnreachable)]
    #[test_case(Icmpv6DestUnreachableCode::PortUnreachable => ConnectionError::PortUnreachable)]
    #[test_case(Icmpv6DestUnreachableCode::SrcAddrFailedPolicy => ConnectionError::SourceRouteFailed)]
    #[test_case(Icmpv6DestUnreachableCode::RejectRoute => ConnectionError::NetworkUnreachable)]
    fn icmp_destination_unreachable_established_v6(
        error: Icmpv6DestUnreachableCode,
    ) -> ConnectionError {
        icmp_destination_unreachable_established_inner::<Ipv6>(Icmpv6ErrorCode::DestUnreachable(
            error,
        ))
    }

    fn icmp_destination_unreachable_established_inner<I: TcpTestIpExt + IcmpIpExt>(
        icmp_error: I::ErrorCode,
    ) -> ConnectionError {
        let (mut net, local, local_snd_end, _remote) = bind_listen_connect_accept_inner::<I>(
            I::UNSPECIFIED_ADDRESS,
            BindConfig { client_port: None, server_port: PORT_1, client_reuse_addr: false },
            0,
            0.0,
        );
        local_snd_end.borrow_mut().extend_from_slice(b"Hello");
        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            SocketHandler::do_send(sync_ctx, non_sync_ctx, local.into());
        });
        net.collect_frames();
        let original_body = assert_matches!(
            &net.iter_pending_frames().collect::<Vec<_>>()[..],
            [InstantAndData(_instant, PendingFrameData {
                dst_context: _,
                meta: _,
                frame,
            })] => {
            frame.clone()
        });
        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            deliver_icmp_error(
                sync_ctx,
                non_sync_ctx,
                I::FAKE_CONFIG.local_ip,
                I::FAKE_CONFIG.remote_ip,
                &original_body[..],
                icmp_error,
            );
            // An error should be posted on the connection.
            let error = assert_matches!(
                SocketHandler::get_connection_error(sync_ctx, local),
                Some(error) => error
            );
            // But it should stay established.
            sync_ctx.with_tcp_sockets(
                |Sockets { bound_state, inactive: _, port_alloc: _, socketmap: _ }| {
                    let (conn, _sharing, _addr) = local.get_from_bound_state(bound_state).unwrap();
                    assert_matches!(conn.state, State::Established(_));
                },
            );
            error
        })
    }

    #[ip_test]
    fn icmp_destination_unreachable_listener<I: Ip + TcpTestIpExt + IcmpIpExt>() {
        let mut net = new_test_net::<I>();

        let backlog = NonZeroUsize::new(1).unwrap();
        let server = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let conn = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            let bound = SocketHandler::bind(sync_ctx, non_sync_ctx, conn, None, Some(PORT_1))
                .expect("failed to bind the server socket");
            SocketHandler::listen(sync_ctx, bound, backlog).expect("can listen")
        });

        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let conn = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            let _client = SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                conn,
                ZonedAddr::Unzoned(I::FAKE_CONFIG.remote_ip),
                PORT_1,
            )
            .expect("failed to connect");
        });

        assert!(!net.step(handle_frame, handle_timer).is_idle());

        net.collect_frames();
        let original_body = assert_matches!(
            &net.iter_pending_frames().collect::<Vec<_>>()[..],
            [InstantAndData(_instant, PendingFrameData {
                dst_context: _,
                meta: _,
                frame,
            })] => {
            frame.clone()
        });
        let icmp_error = I::map_ip(
            (),
            |()| Icmpv4ErrorCode::DestUnreachable(Icmpv4DestUnreachableCode::DestPortUnreachable),
            |()| Icmpv6ErrorCode::DestUnreachable(Icmpv6DestUnreachableCode::PortUnreachable),
        );
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            deliver_icmp_error(
                sync_ctx,
                non_sync_ctx,
                I::FAKE_CONFIG.remote_ip,
                I::FAKE_CONFIG.local_ip,
                &original_body[..],
                icmp_error,
            );
            sync_ctx.with_tcp_sockets(
                |Sockets { bound_state, inactive: _, port_alloc: _, socketmap: _ }| {
                    let (listener, _sharing, _addr) =
                        server.get_from_bound_state(bound_state).unwrap();
                    let listener = assert_matches!(listener, MaybeListener::Listener(l) => l);
                    assert_eq!(listener.pending.len(), 0);
                    assert_eq!(listener.ready.len(), 0);
                },
            );
        });
    }

    #[ip_test]
    fn time_wait_reuse<I: Ip + TcpTestIpExt>() {
        set_logger_for_test();
        const CLIENT_PORT: NonZeroU16 = const_unwrap_option(NonZeroU16::new(2));
        const SERVER_PORT: NonZeroU16 = const_unwrap_option(NonZeroU16::new(1));
        let (mut net, local, _local_snd_end, remote) = bind_listen_connect_accept_inner::<I>(
            I::UNSPECIFIED_ADDRESS,
            BindConfig {
                client_port: Some(CLIENT_PORT),
                server_port: SERVER_PORT,
                client_reuse_addr: true,
            },
            0,
            0.0,
        );
        // Locally, we create a connection with a full accept queue.
        let listener = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            SocketHandler::set_reuseaddr_unbound(sync_ctx, unbound, true);
            let bound = SocketHandler::bind(
                sync_ctx,
                non_sync_ctx,
                unbound,
                Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip)),
                Some(CLIENT_PORT),
            )
            .expect("failed to bind");
            SocketHandler::listen(sync_ctx, bound, NonZeroUsize::new(1).unwrap())
                .expect("failed to listen")
        });
        // This connection is never used, just to keep accept queue full.
        let extra_conn = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                unbound,
                ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip),
                CLIENT_PORT,
            )
            .expect("failed to connect")
        });
        net.run_until_idle(handle_frame, handle_timer);

        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            assert_eq!(
                SocketHandler::get_handshake_status(sync_ctx, extra_conn),
                HandshakeStatus::Completed { reported: false },
            );
        });

        // Now we shutdown the sockets and try to bring the local socket to
        // TIME-WAIT.
        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            SocketHandler::close_conn(sync_ctx, non_sync_ctx, local);
        });
        let local = MaybeClosedConnectionId::from(local);
        assert!(!net.step(handle_frame, handle_timer).is_idle());
        assert!(!net.step(handle_frame, handle_timer).is_idle());
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            SocketHandler::close_conn(sync_ctx, non_sync_ctx, remote);
        });
        assert!(!net.step(handle_frame, handle_timer).is_idle());
        assert!(!net.step(handle_frame, handle_timer).is_idle());
        // The connection should go to TIME-WAIT.
        let (tw_last_seq, tw_last_ack, tw_expiry) = net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            sync_ctx.with_tcp_sockets(
                |Sockets { inactive: _, port_alloc: _, socketmap: _, bound_state }| {
                let (conn, _sharing, _addr) =
                    local.get_from_bound_state(bound_state).expect("failed to get connection");
                assert_matches!(conn.state, State::TimeWait(TimeWait {last_seq,last_ack, last_wnd: _, expiry, last_wnd_scale: _ }) => (last_seq, last_ack, expiry))
            })
        });

        // Try to initiate a connection from the remote since we have an active
        // listener locally.
        let conn = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            SocketHandler::connect_unbound(
                sync_ctx,
                non_sync_ctx,
                unbound,
                ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip),
                CLIENT_PORT,
            )
            .expect("failed to connect")
        });
        while net.next_step() != Some(tw_expiry) {
            assert!(!net.step(handle_frame, handle_timer).is_idle());
        }
        // This attempt should fail due the full accept queue at the listener.
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            sync_ctx.with_tcp_sockets(
                |Sockets { inactive: _, port_alloc: _, socketmap: _, bound_state }| {
                    let (conn, _sharing, _addr) =
                        conn.get_from_bound_state(bound_state).expect("invalid connection ID");
                    assert_matches!(
                        conn.state,
                        State::Closed(Closed { reason: Some(ConnectionError::TimedOut) })
                    );
                },
            );
        });
        // Now free up the accept queue by accepting the connection.
        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let _accepted = SocketHandler::accept(sync_ctx, non_sync_ctx, listener)
                .expect("failed to accept a new connection");
        });
        let conn = net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            let bound = SocketHandler::bind(
                sync_ctx,
                non_sync_ctx,
                unbound,
                Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.remote_ip)),
                Some(SERVER_PORT),
            )
            .expect("failed to bind");
            SocketHandler::connect_bound(
                sync_ctx,
                non_sync_ctx,
                bound,
                SocketAddr { ip: ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip), port: CLIENT_PORT },
            )
            .expect("failed to connect")
        });
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
            assert!(parsed.syn());
            let iss = SeqNum::new(parsed.seq_num());
            assert!(iss.after(tw_last_ack) && iss.before(tw_last_seq));
        });
        // The TIME-WAIT socket should be reused to establish the connection.
        net.run_until_idle(handle_frame, handle_timer);
        net.with_context(REMOTE, |TcpCtx { sync_ctx, non_sync_ctx: _ }| {
            assert_eq!(
                SocketHandler::get_handshake_status(sync_ctx, conn),
                HandshakeStatus::Completed { reported: false },
            );
        });
    }

    #[ip_test]
    fn conn_addr_not_available<I: Ip + TcpTestIpExt + IcmpIpExt>() {
        set_logger_for_test();
        let (mut net, _local, _local_snd_end, _remote) = bind_listen_connect_accept_inner::<I>(
            I::UNSPECIFIED_ADDRESS,
            BindConfig { client_port: Some(PORT_1), server_port: PORT_1, client_reuse_addr: true },
            0,
            0.0,
        );
        // Now we are using the same 4-tuple again to try to create a new
        // connection, this should fail.
        net.with_context(LOCAL, |TcpCtx { sync_ctx, non_sync_ctx }| {
            let unbound = SocketHandler::create_socket(sync_ctx, non_sync_ctx, Default::default());
            SocketHandler::set_reuseaddr_unbound(sync_ctx, unbound, true);
            let bound = SocketHandler::bind(
                sync_ctx,
                non_sync_ctx,
                unbound,
                Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip)),
                Some(PORT_1),
            )
            .expect("failed to bind");
            assert_eq!(
                SocketHandler::connect_bound(
                    sync_ctx,
                    non_sync_ctx,
                    bound,
                    SocketAddr { ip: ZonedAddr::Unzoned(I::FAKE_CONFIG.remote_ip), port: PORT_1 },
                ),
                Err(ConnectError::ConnectionExists),
            )
        });
    }
}
