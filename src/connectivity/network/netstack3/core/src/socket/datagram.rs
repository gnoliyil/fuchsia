// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Shared code for implementing datagram sockets.

use alloc::collections::HashSet;
use core::{
    fmt::Debug,
    hash::Hash,
    num::{NonZeroU16, NonZeroU8},
};
use packet::{BufferMut, Serializer};

use derivative::Derivative;
use net_types::{
    ip::{GenericOverIp, Ip, IpAddress},
    MulticastAddr, MulticastAddress as _, SpecifiedAddr, ZonedAddr,
};
use packet_formats::ip::{IpProto, IpProtoExt};
use thiserror::Error;

use crate::{
    algorithm::ProtocolFlowId,
    data_structures::{
        id_map::{Entry as IdMapEntry, IdMap, OccupiedEntry as IdMapOccupied},
        socketmap::Tagged,
    },
    error::{LocalAddressError, RemoteAddressError, SocketError, ZonedAddressError},
    ip::{
        socket::{
            BufferIpSocketHandler, IpSock, IpSockCreateAndSendError, IpSockCreationError,
            IpSockSendError, IpSocketHandler as _, SendOptions,
        },
        BufferTransportIpContext, HopLimits, IpDeviceId, IpExt, MulticastMembershipHandler,
        TransportIpContext,
    },
    socket::{
        self,
        address::{ConnAddr, ConnIpAddr, ListenerIpAddr},
        AddrVec, Bound, BoundSocketMap, ExistsError, InsertError, ListenerAddr, SocketMapAddrSpec,
        SocketMapAddrStateSpec, SocketMapConflictPolicy, SocketMapStateSpec,
    },
};

/// Datagram socket storage.
#[derive(Derivative)]
#[derivative(Default(bound = ""))]
pub(crate) struct DatagramSockets<A: SocketMapAddrSpec, S: DatagramSocketStateSpec>
where
    Bound<S>: Tagged<AddrVec<A>>,
{
    pub(crate) bound: BoundSocketMap<A, S>,
    pub(crate) unbound: IdMap<UnboundSocketState<A::IpAddr, A::DeviceId, S::UnboundSharingState>>,
}

#[derive(Debug, Derivative)]
#[derivative(Default(bound = "S: Default"))]
pub(crate) struct UnboundSocketState<A: IpAddress, D, S> {
    pub(crate) device: Option<D>,
    pub(crate) sharing: S,
    pub(crate) ip_options: IpOptions<A, D>,
}

#[derive(Debug)]
pub(crate) struct ListenerState<A: Eq + Hash, D: Hash + Eq> {
    pub(crate) ip_options: IpOptions<A, D>,
}

#[derive(Debug)]
pub(crate) struct ConnState<I: IpExt, D: Eq + Hash> {
    pub(crate) socket: IpSock<I, D, IpOptions<I::Addr, D>>,
    /// Determines whether a call to disconnect this socket should also clear
    /// the device on the socket address.
    ///
    /// This will only be `true` if
    ///   1) the corresponding address has a bound device
    ///   2) the local address does not require a zone
    ///   3) the remote address does require a zone
    ///   4) the device was not set via [`set_unbound_device`]
    ///
    /// In that case, when the socket is disconnected, the device should be
    /// cleared since it was set as part of a `connect` call, not explicitly.
    ///
    /// TODO(http://fxbug.dev/110370): Implement this by changing socket
    /// addresses.
    pub(crate) clear_device_on_disconnect: bool,
}

#[derive(Clone, Debug, Derivative)]
#[derivative(Default(bound = ""))]
pub(crate) struct IpOptions<A, D> {
    multicast_memberships: MulticastMemberships<A, D>,
    hop_limits: SocketHopLimits,
}

#[derive(Copy, Clone, Debug, Default, Eq, PartialEq)]
pub(crate) struct SocketHopLimits {
    unicast: Option<NonZeroU8>,
    // TODO(https://fxbug.dev/108323): Make this an Option<u8> to allow sending
    // multicast packets destined only for the local machine.
    multicast: Option<NonZeroU8>,
}

impl SocketHopLimits {
    pub(crate) fn set_unicast(value: Option<NonZeroU8>) -> impl FnOnce(&mut Self) {
        move |limits| limits.unicast = value
    }

    pub(crate) fn set_multicast(value: Option<NonZeroU8>) -> impl FnOnce(&mut Self) {
        move |limits| limits.multicast = value
    }

    fn get_limits_with_defaults(&self, defaults: &HopLimits) -> HopLimits {
        let Self { unicast, multicast } = self;
        HopLimits {
            unicast: unicast.unwrap_or(defaults.unicast),
            multicast: multicast.unwrap_or(defaults.multicast),
        }
    }
}

impl<A: IpAddress, D> SendOptions<A::Version> for IpOptions<A, D> {
    fn hop_limit(&self, destination: &SpecifiedAddr<A>) -> Option<NonZeroU8> {
        if destination.is_multicast() {
            self.hop_limits.multicast
        } else {
            self.hop_limits.unicast
        }
    }
}

#[derive(Clone, Debug, Derivative)]
#[derivative(Default(bound = ""))]
pub(crate) struct MulticastMemberships<A, D>(HashSet<(MulticastAddr<A>, D)>);

pub(crate) enum MulticastMembershipChange {
    Join,
    Leave,
}

impl<A: Eq + Hash, D: Eq + Hash + Clone> MulticastMemberships<A, D> {
    pub(crate) fn apply_membership_change(
        &mut self,
        address: MulticastAddr<A>,
        device: &D,
        want_membership: bool,
    ) -> Option<MulticastMembershipChange> {
        let device = device.clone();

        let Self(map) = self;
        if want_membership {
            map.insert((address, device)).then(|| MulticastMembershipChange::Join)
        } else {
            map.remove(&(address, device)).then(|| MulticastMembershipChange::Leave)
        }
    }
}

impl<A: Eq + Hash, D: Eq + Hash> IntoIterator for MulticastMemberships<A, D> {
    type Item = (MulticastAddr<A>, D);
    type IntoIter = <HashSet<(MulticastAddr<A>, D)> as IntoIterator>::IntoIter;

    fn into_iter(self) -> Self::IntoIter {
        let Self(memberships) = self;
        memberships.into_iter()
    }
}

impl<A: IpAddress, D: IpDeviceId> ConnAddr<A, D, NonZeroU16, NonZeroU16> {
    pub(crate) fn from_protocol_flow_and_local_port(
        id: &ProtocolFlowId<A>,
        local_port: NonZeroU16,
    ) -> Self {
        Self {
            ip: ConnIpAddr {
                local: (*id.local_addr(), local_port),
                remote: (*id.remote_addr(), id.remote_port()),
            },
            device: None,
        }
    }
}

fn leave_all_joined_groups<I: Ip, C, SC: MulticastMembershipHandler<I, C>>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    memberships: MulticastMemberships<I::Addr, SC::DeviceId>,
) {
    for (addr, device) in memberships {
        sync_ctx.leave_multicast_group(ctx, &device, addr)
    }
}

pub(crate) trait LocalIdentifierAllocator<A: SocketMapAddrSpec, C, S: SocketMapStateSpec>
where
    Bound<S>: Tagged<AddrVec<A>>,
{
    fn try_alloc_local_id(
        &mut self,
        bound: &BoundSocketMap<A, S>,
        ctx: &mut C,
        flow: DatagramFlowId<A::IpAddr, A::RemoteIdentifier>,
    ) -> Option<A::LocalIdentifier>;
}

pub(crate) struct DatagramFlowId<A: IpAddress, RI> {
    pub(crate) local_ip: SpecifiedAddr<A>,
    pub(crate) remote_ip: SpecifiedAddr<A>,
    pub(crate) remote_id: RI,
}

pub(crate) trait DatagramStateContext<A: SocketMapAddrSpec, C, S: SocketMapStateSpec>
where
    Bound<S>: Tagged<AddrVec<A>>,
{
    /// The synchronized context passed to the callback provided to
    /// `with_sockets_mut`.
    type IpSocketsCtx: TransportIpContext<A::IpVersion, C, DeviceId = A::DeviceId>
        + MulticastMembershipHandler<A::IpVersion, C>;

    /// The additional allocator passed to the callback provided to
    /// `with_sockets_mut`.
    type LocalIdAllocator: LocalIdentifierAllocator<A, C, S>;

    /// Calls the function with an immutable reference to the datagram sockets.
    fn with_sockets<O, F: FnOnce(&Self::IpSocketsCtx, &DatagramSockets<A, S>) -> O>(
        &self,
        cb: F,
    ) -> O;

    /// Calls the function with a mutable reference to the datagram sockets.
    fn with_sockets_mut<
        O,
        F: FnOnce(
            &mut Self::IpSocketsCtx,
            &mut DatagramSockets<A, S>,
            &mut Self::LocalIdAllocator,
        ) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O;
}

pub(crate) trait DatagramStateNonSyncContext<A: SocketMapAddrSpec> {
    /// Attempts to allocate an identifier for a listener.
    ///
    /// `is_available` checks whether the provided address could be used without
    /// conflicting with any existing entries in state context's socket map,
    /// returning an error otherwise.
    fn try_alloc_listen_identifier(
        &mut self,
        is_available: impl Fn(A::LocalIdentifier) -> Result<(), InUseError>,
    ) -> Option<A::LocalIdentifier>;
}

pub(crate) trait BufferDatagramStateContext<
    A: SocketMapAddrSpec,
    C,
    S: SocketMapStateSpec,
    B: BufferMut,
>: DatagramStateContext<A, C, S, IpSocketsCtx = Self::BufferIpSocketsCtx> where
    Bound<S>: Tagged<AddrVec<A>>,
{
    type BufferIpSocketsCtx: BufferTransportIpContext<A::IpVersion, C, B, DeviceId = A::DeviceId>;
}
impl<
        A: SocketMapAddrSpec,
        C,
        S: SocketMapStateSpec,
        B: BufferMut,
        SC: DatagramStateContext<A, C, S>,
    > BufferDatagramStateContext<A, C, S, B> for SC
where
    Bound<S>: Tagged<AddrVec<A>>,
    SC::IpSocketsCtx: BufferTransportIpContext<A::IpVersion, C, B, DeviceId = A::DeviceId>,
{
    type BufferIpSocketsCtx = SC::IpSocketsCtx;
}

pub(crate) trait DatagramSocketStateSpec: SocketMapStateSpec {
    type UnboundId: Clone + From<usize> + Into<usize> + Debug;
    type UnboundSharingState: Default;
}

pub(crate) trait DatagramSocketSpec<A: SocketMapAddrSpec>:
    DatagramSocketStateSpec<
        ListenerState = ListenerState<A::IpAddr, A::DeviceId>,
        ConnState = ConnState<A::IpVersion, A::DeviceId>,
    > + SocketMapConflictPolicy<
        ListenerAddr<A::IpAddr, A::DeviceId, A::LocalIdentifier>,
        <Self as SocketMapStateSpec>::ListenerSharingState,
        A,
    > + SocketMapConflictPolicy<
        ConnAddr<A::IpAddr, A::DeviceId, A::LocalIdentifier, A::RemoteIdentifier>,
        <Self as SocketMapStateSpec>::ConnSharingState,
        A,
    >
{
    type Serializer<B: BufferMut>: Serializer<Buffer = B>;
    fn make_packet<B: BufferMut>(
        body: B,
        addr: &ConnIpAddr<A::IpAddr, A::LocalIdentifier, A::RemoteIdentifier>,
    ) -> Self::Serializer<B>;
}

pub(crate) struct InUseError;

pub(crate) fn create_unbound<
    A: SocketMapAddrSpec,
    S: DatagramSocketStateSpec,
    C,
    SC: DatagramStateContext<A, C, S>,
>(
    sync_ctx: &mut SC,
) -> S::UnboundId
where
    Bound<S>: Tagged<AddrVec<A>>,
{
    sync_ctx.with_sockets_mut(|_sync_ctx, DatagramSockets { unbound, bound: _ }, _allocator| {
        unbound.push(UnboundSocketState::default()).into()
    })
}

pub(crate) fn remove_unbound<
    A: SocketMapAddrSpec,
    S: DatagramSocketStateSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: S::UnboundId,
) where
    Bound<S>: Tagged<AddrVec<A>>,
{
    sync_ctx.with_sockets_mut(|sync_ctx, state, _allocator| {
        let DatagramSockets { unbound, bound: _ } = state;
        let UnboundSocketState { device: _, sharing: _, ip_options } =
            unbound.remove(id.into()).expect("invalid UDP unbound ID");

        let IpOptions { multicast_memberships, hop_limits: _ } = ip_options;
        leave_all_joined_groups(sync_ctx, ctx, multicast_memberships);
    })
}

pub(crate) fn remove_listener<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: S::ListenerId,
) -> ListenerAddr<A::IpAddr, A::DeviceId, A::LocalIdentifier>
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ListenerAddrState:
        SocketMapAddrStateSpec<Id = S::ListenerId, SharingState = S::ListenerSharingState>,
{
    sync_ctx.with_sockets_mut(|sync_ctx, state, _allocator| {
        let DatagramSockets { bound, unbound: _ } = state;
        let (state, _, addr): (_, S::ListenerSharingState, _) =
            bound.listeners_mut().remove(&id).expect("Invalid UDP listener ID");

        let ListenerState { ip_options } = state;
        let IpOptions { multicast_memberships, hop_limits: _ } = ip_options;

        leave_all_joined_groups(sync_ctx, ctx, multicast_memberships);
        addr
    })
}

pub(crate) fn remove_conn<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: S::ConnId,
) -> ConnAddr<A::IpAddr, A::DeviceId, A::LocalIdentifier, A::RemoteIdentifier>
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ConnAddrState: SocketMapAddrStateSpec<Id = S::ConnId, SharingState = S::ConnSharingState>,
    A::IpVersion: IpExt,
{
    sync_ctx.with_sockets_mut(|sync_ctx, state, _allocator| {
        let DatagramSockets { bound, unbound: _ } = state;
        let (state, _sharing, addr): (_, S::ConnSharingState, _) =
            bound.conns_mut().remove(&id).expect("UDP connection not found");
        let ConnState { socket, clear_device_on_disconnect: _ } = state;

        let IpOptions { multicast_memberships, hop_limits: _ } = socket.into_options();
        leave_all_joined_groups(sync_ctx, ctx, multicast_memberships);
        addr
    })
}

/// Wrapper for an occupied entry that implements Into::into by
/// removing from the entry.
struct TakeMemberships<'a, A: IpAddress, D, S>(
    IdMapOccupied<'a, usize, UnboundSocketState<A, D, S>>,
);

impl<'a, A: IpAddress, D: Hash + Eq, S> From<TakeMemberships<'a, A, D, S>> for ListenerState<A, D> {
    fn from(take: TakeMemberships<'a, A, D, S>) -> Self {
        let TakeMemberships(entry) = take;
        let UnboundSocketState { device: _, sharing: _, ip_options } = entry.remove();
        ListenerState { ip_options }
    }
}

pub(crate) fn listen<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: S::UnboundId,
    addr: Option<ZonedAddr<A::IpAddr, A::DeviceId>>,
    local_id: Option<A::LocalIdentifier>,
) -> Result<S::ListenerId, LocalAddressError>
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ListenerAddrState:
        SocketMapAddrStateSpec<Id = S::ListenerId, SharingState = S::ListenerSharingState>,
    S::UnboundSharingState: Clone + Into<S::ListenerSharingState>,
    S::ListenerSharingState: Default,
{
    sync_ctx.with_sockets_mut(|sync_ctx, DatagramSockets { bound, unbound }, _allocator| {
        let identifier = match local_id {
            Some(local_id) => Ok(local_id),
            None => {
                let addr = addr.clone().map(|addr| addr.into_addr_zone().0);
                let sharing_options = Default::default();
                ctx.try_alloc_listen_identifier(|identifier| {
                    let check_addr =
                        ListenerAddr { device: None, ip: ListenerIpAddr { identifier, addr } };
                    bound.listeners().could_insert(&check_addr, &sharing_options).map_err(|e| {
                        match e {
                            InsertError::Exists
                            | InsertError::IndirectConflict
                            | InsertError::ShadowAddrExists
                            | InsertError::ShadowerExists => InUseError,
                        }
                    })
                })
            }
            .ok_or(LocalAddressError::FailedToAllocateLocalPort),
        }?;

        let UnboundSocketState { device, sharing: _, ip_options: _ } = unbound
            .get(id.clone().into())
            .unwrap_or_else(|| panic!("unbound ID {:?} is invalid", id));
        let (addr, device, identifier) = match addr {
            Some(addr) => {
                // Extract the specified address and the device. The device
                // is either the one from the address or the one to which
                // the socket was previously bound.
                let (addr, device) =
                    crate::transport::resolve_addr_with_device(addr, device.as_ref())?;

                // Binding to multicast addresses is allowed regardless.
                // Other addresses can only be bound to if they are assigned
                // to the device.
                if !addr.is_multicast() {
                    let mut assigned_to = sync_ctx.get_devices_with_assigned_addr(addr);
                    if let Some(device) = &device {
                        if !assigned_to.any(|d| &d == device) {
                            return Err(LocalAddressError::AddressMismatch);
                        }
                    } else {
                        if !assigned_to.any(|_: A::DeviceId| true) {
                            return Err(LocalAddressError::CannotBindToAddress);
                        }
                    }
                }
                (Some(addr), device, identifier)
            }
            None => (None, device.clone(), identifier),
        };

        let unbound_entry = match unbound.entry(id.clone().into()) {
            IdMapEntry::Vacant(_) => panic!("unbound ID {:?} is invalid", id),
            IdMapEntry::Occupied(o) => o,
        };

        let UnboundSocketState { device: _, sharing, ip_options: _ } = unbound_entry.get();
        let sharing = sharing.clone();
        match bound
            .listeners_mut()
            .try_insert(
                ListenerAddr { ip: ListenerIpAddr { addr, identifier }, device },
                // Passing TakeMemberships defers removal of unbound_entry
                // until try_insert is known to be able to succeed.
                TakeMemberships(unbound_entry),
                sharing.into(),
            )
            .map_err(|(e, state, _sharing): (_, _, S::ListenerSharingState)| (e, state))
        {
            Ok(entry) => Ok(entry.id()),
            Err((e, TakeMemberships(entry))) => {
                // Drop the occupied entry, leaving it in the unbound socket
                // IdMap.
                let _: (InsertError, IdMapOccupied<'_, _, _>) = (e, entry);
                Err(LocalAddressError::AddressInUse)
            }
        }
    })
}

/// An error when attempting to create a datagram socket.
#[derive(Error, Copy, Clone, Debug, Eq, PartialEq)]
pub enum SockCreationError {
    /// An error was encountered creating an IP socket.
    #[error("{}", _0)]
    Ip(#[from] IpSockCreationError),
    /// No local port was specified, and none could be automatically allocated.
    #[error("a local port could not be allocated")]
    CouldNotAllocateLocalPort,
    /// The specified socket addresses (IP addresses and ports) conflict with an
    /// existing socket.
    #[error("the socket's IP address and port conflict with an existing socket")]
    SockAddrConflict,
    /// There was a problem with the provided address relating to its zone.
    #[error("{}", _0)]
    Zone(#[from] ZonedAddressError),
}

pub(crate) fn connect<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: S::UnboundId,
    remote_ip: ZonedAddr<A::IpAddr, A::DeviceId>,
    remote_id: A::RemoteIdentifier,
    proto: IpProto,
) -> Result<S::ConnId, SockCreationError>
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ConnAddrState: SocketMapAddrStateSpec<Id = S::ConnId, SharingState = S::ConnSharingState>,
    S::UnboundSharingState: Clone + Into<S::ConnSharingState>,
{
    sync_ctx.with_sockets_mut(|sync_ctx, state, allocator| {
        let DatagramSockets { bound, unbound } = state;
        let occupied = match unbound.entry(id.clone().into()) {
            IdMapEntry::Vacant(_) => panic!("unbound socket {:?} not found", id),
            IdMapEntry::Occupied(o) => o,
        };
        let UnboundSocketState { device, sharing, ip_options: _ } = occupied.get();

        let (remote_ip, socket_device) =
            crate::transport::resolve_addr_with_device(remote_ip, device.as_ref())
                .map_err(SockCreationError::Zone)?;

        let device = socket_device.as_ref();

        let ip_sock = sync_ctx
            .new_ip_socket(ctx, None, None, remote_ip, proto.into(), Default::default())
            .map_err(|(e, _ip_options): (_, IpOptions<_, _>)| e)?;

        let local_ip = *ip_sock.local_ip();
        let remote_ip = *ip_sock.remote_ip();

        let local_id = match allocator.try_alloc_local_id(
            bound,
            ctx,
            DatagramFlowId { local_ip, remote_ip, remote_id: remote_id.clone() },
        ) {
            Some(x) => x,
            None => {
                return Err(SockCreationError::CouldNotAllocateLocalPort);
            }
        };

        let c = ConnAddr {
            ip: ConnIpAddr { local: (local_ip, local_id), remote: (remote_ip, remote_id) },
            device: device.cloned(),
        };
        match bound.conns_mut().try_insert(
            c,
            ConnState { socket: ip_sock, clear_device_on_disconnect: false },
            sharing.clone().into(),
        ) {
            Ok(mut entry) => {
                let UnboundSocketState { device: _, sharing: _, ip_options } = occupied.remove();
                *entry.get_state_mut().socket.options_mut() = ip_options;
                Ok(entry.id())
            }
            Err(e) => {
                let _: (InsertError, ConnState<_, _>, S::ConnSharingState) = e;
                Err(SockCreationError::SockAddrConflict)
            }
        }
    })
}

/// Error returned when [`connect_listener`] fails.
#[derive(Debug, Error, Eq, PartialEq)]
pub enum ConnectListenerError {
    /// An error was encountered creating an IP socket.
    #[error("{}", _0)]
    Ip(#[from] IpSockCreationError),
    /// There was a problem with the provided address relating to its zone.
    #[error("{}", _0)]
    Zone(#[from] ZonedAddressError),
    /// The new socket conflicts with an existing one.
    #[error("The address is already occupied")]
    AddressConflict,
}

pub(crate) fn connect_listener<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: S::ListenerId,
    remote_ip: ZonedAddr<A::IpAddr, A::DeviceId>,
    remote_id: A::RemoteIdentifier,
    proto: IpProto,
) -> Result<S::ConnId, (ConnectListenerError, S::ListenerId)>
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ListenerAddrState:
        SocketMapAddrStateSpec<Id = S::ListenerId, SharingState = S::ListenerSharingState>,
    S::ConnAddrState: SocketMapAddrStateSpec<Id = S::ConnId, SharingState = S::ConnSharingState>,
    S::UnboundSharingState: Clone + Into<S::ListenerSharingState>,
    S::ListenerSharingState: Into<S::ConnSharingState>,
{
    sync_ctx.with_sockets_mut(|sync_ctx, state, _allocator| {
        let DatagramSockets { bound, unbound: _ } = state;
        let entry = bound.listeners_mut().entry(&id).expect("Invalid listener ID");
        let (_, _, ListenerAddr { ip, device }): &(
            ListenerState<_, _>,
            S::ListenerSharingState,
            _,
        ) = entry.get();

        let (remote_ip, socket_device) =
            match crate::transport::resolve_addr_with_device(remote_ip, device.as_ref()) {
                Ok(x) => x,
                Err(e) => return Err((ConnectListenerError::Zone(e), id)),
            };

        let ListenerIpAddr { addr: local_ip, identifier: local_port } = ip.clone();

        let ip_sock = match sync_ctx.new_ip_socket(
            ctx,
            socket_device.as_ref(),
            local_ip,
            remote_ip,
            proto.into(),
            Default::default(),
        ) {
            Ok(ip_sock) => ip_sock,
            Err((e, _ip_options)) => return Err((e.into(), id)),
        };

        let clear_device_on_disconnect = device.is_none() && socket_device.is_some();
        let (ListenerState { ip_options }, sharing, original_addr) = entry.remove();

        let local_ip = *ip_sock.local_ip();
        let remote_ip = *ip_sock.remote_ip();

        let c = ConnAddr {
            ip: ConnIpAddr { local: (local_ip, local_port), remote: (remote_ip, remote_id) },
            device: socket_device,
        };
        let insert_error = match bound.conns_mut().try_insert(
            c,
            ConnState { socket: ip_sock, clear_device_on_disconnect },
            sharing.clone().into(),
        ) {
            Ok(mut entry) => {
                *entry.get_state_mut().socket.options_mut() = ip_options;
                return Ok(entry.id());
            }
            Err(e) => e,
        };

        let _: (InsertError, ConnState<_, _>, S::ConnSharingState) = insert_error;
        let listener = bound
            .listeners_mut()
            .try_insert(original_addr, ListenerState { ip_options }, sharing)
            .expect("reinserting just-removed listener failed")
            .id();
        Err((ConnectListenerError::AddressConflict, listener))
    })
}

pub(crate) fn reconnect<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: S::ConnId,
    remote_ip: ZonedAddr<A::IpAddr, A::DeviceId>,
    remote_id: A::RemoteIdentifier,
) -> Result<S::ConnId, (ConnectListenerError, S::ConnId)>
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ConnAddrState: SocketMapAddrStateSpec<Id = S::ConnId, SharingState = S::ConnSharingState>,
    S::UnboundSharingState: Clone + Into<S::ListenerSharingState>,
{
    sync_ctx.with_sockets_mut(|sync_ctx, state, _allocator| {
        let DatagramSockets { bound, unbound: _ } = state;
        let entry = bound.conns_mut().entry(&id).expect("Invalid conn ID");
        let (
            ConnState { socket, clear_device_on_disconnect: _ },
            _,
            ConnAddr { ip: ConnIpAddr { local, remote: _ }, device },
        ): &(ConnState<_, _>, S::ConnSharingState, _) = entry.get();
        let proto = socket.proto();

        let (remote_ip, socket_device) =
            match crate::transport::resolve_addr_with_device(remote_ip, device.as_ref()) {
                Ok(x) => x,
                Err(e) => return Err((ConnectListenerError::Zone(e), id)),
            };

        let (local_ip, _) = local;
        let ip_sock = match sync_ctx.new_ip_socket(
            ctx,
            socket_device.as_ref(),
            Some(*local_ip),
            remote_ip,
            proto,
            Default::default(),
        ) {
            Ok(ip_sock) => ip_sock,
            Err((e, _ip_options)) => return Err((e.into(), id)),
        };

        let local = local.clone();
        let (mut conn_state, sharing, original_addr) = entry.remove();
        let ConnState { socket, clear_device_on_disconnect } = &mut conn_state;

        let device = socket_device.as_ref();

        let c = ConnAddr {
            ip: ConnIpAddr { local, remote: (remote_ip, remote_id) },
            device: device.cloned(),
        };

        let insert_error = match bound.conns_mut().try_insert(
            c,
            ConnState { socket: ip_sock, clear_device_on_disconnect: *clear_device_on_disconnect },
            sharing,
        ) {
            Ok(mut entry) => {
                *entry.get_state_mut().socket.options_mut() = socket.take_options();
                return Ok(entry.id());
            }
            Err(e) => e,
        };

        let (_, _, sharing): (InsertError, ConnState<_, _>, _) = insert_error;
        // Restore the original socket if creation of the new socket fails.
        let id = bound
            .conns_mut()
            .try_insert(original_addr, conn_state, sharing)
            .unwrap_or_else(|(e, _, _): (_, ConnState<_, _>, S::ConnSharingState)| {
                unreachable!("reinserting just-removed connected socket failed: {:?}", e)
            })
            .id();
        Err((ConnectListenerError::AddressConflict, id))
    })
}

pub(crate) fn set_unbound_device<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &mut SC,
    _ctx: &mut C,
    id: S::UnboundId,
    device_id: Option<&A::DeviceId>,
) where
    Bound<S>: Tagged<AddrVec<A>>,
{
    sync_ctx.with_sockets_mut(|_sync_ctx, state, _allocator| {
        let DatagramSockets { unbound, bound: _ } = state;
        let UnboundSocketState { ref mut device, sharing: _, ip_options: _ } =
            unbound.get_mut(id.into()).expect("unbound UDP socket not found");
        *device = device_id.cloned();
    })
}

pub(crate) fn disconnect_connected<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &mut SC,
    _ctx: &mut C,
    id: S::ConnId,
) -> S::ListenerId
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ListenerAddrState:
        SocketMapAddrStateSpec<Id = S::ListenerId, SharingState = S::ListenerSharingState>,
    S::ConnAddrState: SocketMapAddrStateSpec<Id = S::ConnId, SharingState = S::ConnSharingState>,
    S::ConnSharingState: Into<S::ListenerSharingState>,
{
    sync_ctx.with_sockets_mut(|_sync_ctx, state, _allocator| {
        let DatagramSockets { bound, unbound: _ } = state;
        let (state, sharing, addr): (_, S::ConnSharingState, _) =
            bound.conns_mut().remove(&id).expect("connection not found");

        let ConnState { socket, clear_device_on_disconnect } = state;
        let ip_options = socket.into_options();

        let ConnAddr { ip: ConnIpAddr { local: (local_ip, identifier), remote: _ }, mut device } =
            addr;
        if clear_device_on_disconnect {
            device = None
        }

        let addr = ListenerAddr { ip: ListenerIpAddr { addr: Some(local_ip), identifier }, device };

        bound
            .listeners_mut()
            .try_insert(addr, ListenerState { ip_options }, sharing.into())
            .expect("inserting listener for disconnected socket failed")
            .id()
    })
}

pub(crate) fn send_conn<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: BufferDatagramStateContext<A, C, S, B>,
    S: DatagramSocketSpec<A>,
    B: BufferMut,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: S::ConnId,
    body: B,
) -> Result<(), (S::Serializer<B>, IpSockSendError)>
where
    Bound<S>: Tagged<AddrVec<A>>,
{
    sync_ctx.with_sockets_mut(|sync_ctx, state, _allocator| {
        let DatagramSockets { bound, unbound: _ } = state;
        let (ConnState { socket, clear_device_on_disconnect: _ }, _sharing, addr) =
            bound.conns().get_by_id(&id).expect("no such connection");
        let ConnAddr { ip, device: _ } = addr;

        sync_ctx.send_ip_packet(ctx, &socket, S::make_packet(body, &ip), None)
    })
}

pub(crate) enum SendToError<B, S> {
    Zone(B, ZonedAddressError),
    CreateAndSend(S, IpSockCreateAndSendError),
}

pub(crate) fn send_conn_to<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: BufferDatagramStateContext<A, C, S, B>,
    S: DatagramSocketSpec<A>,
    B: BufferMut,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: S::ConnId,
    remote_ip: ZonedAddr<A::IpAddr, A::DeviceId>,
    remote_port: A::RemoteIdentifier,
    body: B,
) -> Result<(), SendToError<B, S::Serializer<B>>>
where
    Bound<S>: Tagged<AddrVec<A>>,
{
    sync_ctx.with_sockets_mut(|sync_ctx, state, _allocator| {
        let DatagramSockets { bound, unbound: _ } = state;
        let (
            ConnState { socket, clear_device_on_disconnect: _ },
            _,
            ConnAddr { ip: ConnIpAddr { local, remote: _ }, device },
        ): &(_, S::ConnSharingState, _) = bound.conns().get_by_id(&id).expect("no such connection");

        let (local_ip, local_id) = local;

        send_oneshot::<A, S, _, _, _>(
            sync_ctx,
            ctx,
            (Some(*local_ip), local_id.clone()),
            remote_ip,
            remote_port,
            device,
            socket.options(),
            socket.proto(),
            body,
        )
    })
}

pub(crate) fn send_listener_to<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: BufferDatagramStateContext<A, C, S, B>,
    S: DatagramSocketSpec<A>,
    B: BufferMut,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: S::ListenerId,
    proto: <A::IpVersion as IpProtoExt>::Proto,
    remote_ip: ZonedAddr<A::IpAddr, A::DeviceId>,
    remote_port: A::RemoteIdentifier,
    body: B,
) -> Result<(), SendToError<B, S::Serializer<B>>>
where
    Bound<S>: Tagged<AddrVec<A>>,
{
    // TODO(https://fxbug.dev/92447) If `local_ip` is `None`, and so
    // `new_ip_socket` picks a local IP address for us, it may cause problems
    // when we don't match the bound listener addresses. We should revisit
    // whether that check is actually necessary.
    //
    // Also, if the local IP address is a multicast address this function should
    // probably fail and `send_udp_conn_to` must be used instead.
    sync_ctx.with_sockets_mut(|sync_ctx, state, _allocator| {
        let DatagramSockets { bound, unbound: _ } = state;
        let (
            ListenerState { ip_options },
            _,
            ListenerAddr { ip: ListenerIpAddr { addr: local_ip, identifier: local_port }, device },
        ): &(_, S::ListenerSharingState, _) =
            bound.listeners().get_by_id(&id).expect("specified listener not found");

        send_oneshot::<A, S, _, _, _>(
            sync_ctx,
            ctx,
            (*local_ip, local_port.clone()),
            remote_ip,
            remote_port,
            device,
            ip_options,
            proto,
            body,
        )
    })
}

fn send_oneshot<
    A: SocketMapAddrSpec,
    S: DatagramSocketSpec<A>,
    SC: BufferIpSocketHandler<A::IpVersion, C, B, DeviceId = A::DeviceId>,
    C,
    B: BufferMut,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    (local_ip, local_id): (Option<SpecifiedAddr<A::IpAddr>>, A::LocalIdentifier),
    remote_ip: ZonedAddr<A::IpAddr, A::DeviceId>,
    remote_id: A::RemoteIdentifier,
    device: &Option<A::DeviceId>,
    ip_options: &IpOptions<A::IpAddr, A::DeviceId>,
    proto: <A::IpVersion as IpProtoExt>::Proto,
    body: B,
) -> Result<(), SendToError<B, S::Serializer<B>>> {
    let (remote_ip, device) =
        match crate::transport::resolve_addr_with_device(remote_ip, device.as_ref()) {
            Ok(addr) => addr,
            Err(e) => return Err(SendToError::Zone(body, e)),
        };

    sync_ctx
        .send_oneshot_ip_packet(
            ctx,
            device.as_ref(),
            local_ip,
            remote_ip,
            proto,
            ip_options,
            |local_ip| {
                S::make_packet(
                    body,
                    &ConnIpAddr { local: (local_ip, local_id), remote: (remote_ip, remote_id) },
                )
            },
            None,
        )
        .map_err(|(body, err, _ip_options)| SendToError::CreateAndSend(body, err))
}

#[derive(Derivative)]
#[derivative(Clone(bound = ""), Debug(bound = ""))]
pub(crate) enum DatagramBoundId<S: DatagramSocketStateSpec> {
    Listener(S::ListenerId),
    Connected(S::ConnId),
}

pub(crate) fn set_listener_device<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &mut SC,
    _ctx: &mut C,
    id: S::ListenerId,
    device_id: Option<&A::DeviceId>,
) -> Result<(), LocalAddressError>
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ListenerAddrState:
        SocketMapAddrStateSpec<Id = S::ListenerId, SharingState = S::ListenerSharingState>,
{
    sync_ctx.with_sockets_mut(|_sync_ctx, DatagramSockets { unbound: _, bound }, _allocator| {
        // Don't allow changing the device if one of the IP addresses in the socket
        // address vector requires a zone (scope ID).
        let (_, _, addr): &(S::ListenerState, S::ListenerSharingState, _) = bound
            .listeners()
            .get_by_id(&id)
            .unwrap_or_else(|| panic!("invalid listener ID {:?}", id));
        let ListenerAddr { device, ip: ListenerIpAddr { addr, identifier: _ } } = addr;
        let must_have_zone = addr.as_ref().map_or(false, socket::must_have_zone);

        if device.as_ref() != device_id && must_have_zone {
            return Err(LocalAddressError::Zone(ZonedAddressError::DeviceZoneMismatch));
        }

        let entry = bound
            .listeners_mut()
            .entry(&id)
            .unwrap_or_else(|| panic!("invalid listener ID {:?}", id));
        let (_, _, addr): &(S::ListenerState, S::ListenerSharingState, _) = entry.get();
        let new_addr = ListenerAddr { device: device_id.cloned(), ..addr.clone() };
        entry
            .try_update_addr(new_addr)
            .map_err(|(ExistsError {}, _entry)| LocalAddressError::AddressInUse)
            .map(|_new_entry| ())
    })
}

pub(crate) fn set_connected_device<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: S::ConnId,
    device_id: Option<&A::DeviceId>,
) -> Result<(), SocketError>
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ConnAddrState: SocketMapAddrStateSpec<Id = S::ConnId, SharingState = S::ConnSharingState>,
{
    sync_ctx.with_sockets_mut(|sync_ctx, DatagramSockets { bound, unbound: _ }, _allocator| {
        // Don't allow changing the device if one of the IP addresses in the socket
        // address vector requires a zone (scope ID).
        let (state, _, addr): &(_, S::ConnSharingState, _) =
            bound.conns().get_by_id(&id).unwrap_or_else(|| panic!("invalid conn ID {:?}", id));
        let ConnAddr { device, ip: ConnIpAddr { local: (local_ip, _), remote: (remote_ip, _) } } =
            addr;
        let must_have_zone = [local_ip, remote_ip].into_iter().any(|a| socket::must_have_zone(a));

        if device.as_ref() != device_id && must_have_zone {
            return Err(SocketError::Local(LocalAddressError::Zone(
                ZonedAddressError::DeviceZoneMismatch,
            )));
        }

        let ConnState { socket, clear_device_on_disconnect: _ } = state;
        let mut new_socket = sync_ctx
            .new_ip_socket(
                ctx,
                device_id,
                Some(*local_ip),
                *remote_ip,
                socket.proto(),
                Default::default(),
            )
            .map_err(|_: (IpSockCreationError, IpOptions<_, _>)| {
                SocketError::Remote(RemoteAddressError::NoRoute)
            })?;

        let entry =
            bound.conns_mut().entry(&id).unwrap_or_else(|| panic!("invalid conn ID {:?}", id));
        let (_, _, addr): &(_, S::ConnSharingState, _) = entry.get();
        let new_addr = ConnAddr { device: device_id.cloned(), ..addr.clone() };

        let mut entry = match entry.try_update_addr(new_addr) {
            Err((ExistsError, _entry)) => {
                return Err(SocketError::Local(LocalAddressError::AddressInUse))
            }
            Ok(entry) => entry,
        };
        // Since the move was successful, replace the old socket with
        // the new one but move the options over.
        let ConnState { socket, clear_device_on_disconnect } = entry.get_state_mut();
        let _: IpOptions<_, _> = new_socket.replace_options(socket.take_options());
        *socket = new_socket;

        // If this operation explicitly sets the device for the socket, it
        // should no longer be cleared on disconnect.
        if device_id.is_some() {
            *clear_device_on_disconnect = false;
        }
        Ok(())
    })
}

pub(crate) fn get_bound_device<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &SC,
    _ctx: &C,
    id: impl Into<DatagramSocketId<S>>,
) -> Option<A::DeviceId>
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ListenerAddrState:
        SocketMapAddrStateSpec<Id = S::ListenerId, SharingState = S::ListenerSharingState>,
    S::ConnAddrState: SocketMapAddrStateSpec<Id = S::ConnId, SharingState = S::ConnSharingState>,
{
    sync_ctx.with_sockets(|_sync_ctx, state| {
        let DatagramSockets { bound, unbound } = state;
        match id.into() {
            DatagramSocketId::Unbound(id) => {
                let UnboundSocketState { device, sharing: _, ip_options: _ } =
                    unbound.get(id.into()).expect("unbound socket not found");
                device.clone()
            }
            DatagramSocketId::Bound(DatagramBoundId::Listener(id)) => {
                let (_, _, addr): &(S::ListenerState, S::ListenerSharingState, _) =
                    bound.listeners().get_by_id(&id).expect("UDP listener not found");
                let ListenerAddr { device, ip: _ } = addr;
                device.clone()
            }
            DatagramSocketId::Bound(DatagramBoundId::Connected(id)) => {
                let (_, _, addr): &(S::ConnState, S::ConnSharingState, _) =
                    bound.conns().get_by_id(&id).expect("UDP connected socket not found");
                let ConnAddr { device, ip: _ } = addr;
                device.clone()
            }
        }
    })
}

/// Error resulting from attempting to change multicast membership settings for
/// a socket.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum SetMulticastMembershipError {
    /// The provided address does not match the provided device.
    AddressNotAvailable,
    /// The provided address does not match any address on the host.
    NoDeviceWithAddress,
    /// No device or address was specified and there is no device with a route
    /// to the multicast address.
    NoDeviceAvailable,
    /// The requested membership change had no effect (tried to leave a group
    /// without joining, or to join a group again).
    NoMembershipChange,
    /// The socket is bound to a device that doesn't match the one specified.
    WrongDevice,
}

/// Selects the interface for the given remote address, optionally with a
/// constraint on the source address.
fn pick_interface_for_addr<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: TransportIpContext<A::IpVersion, C, DeviceId = A::DeviceId>,
>(
    sync_ctx: &SC,
    _remote_addr: MulticastAddr<A::IpAddr>,
    source_addr: Option<SpecifiedAddr<A::IpAddr>>,
) -> Result<A::DeviceId, SetMulticastMembershipError> {
    if let Some(source_addr) = source_addr {
        let mut devices = sync_ctx.get_devices_with_assigned_addr(source_addr);
        if let Some(d) = devices.next() {
            if devices.next() == None {
                return Ok(d);
            }
        }
    }
    log_unimplemented!((), "https://fxbug.dev/39479: Implement this by looking up a route");
    Err(SetMulticastMembershipError::NoDeviceAvailable)
}

#[derive(Derivative)]
#[derivative(Clone(bound = "S::UnboundId: Clone, S::ListenerId: Clone, S::ConnId: Clone"))]
pub(crate) enum DatagramSocketId<S: DatagramSocketStateSpec> {
    Unbound(S::UnboundId),
    Bound(DatagramBoundId<S>),
}

/// Selector for the device to affect when changing multicast settings.
#[derive(Copy, Clone, Debug, Eq, GenericOverIp, PartialEq)]
pub enum MulticastInterfaceSelector<A: IpAddress, D> {
    /// Use the device with the assigned address.
    LocalAddress(SpecifiedAddr<A>),
    /// Use the device with the specified identifier.
    Interface(D),
}

/// Selector for the device to use when changing multicast membership settings.
///
/// This is like `Option<MulticastInterfaceSelector` except it specifies the
/// semantics of the `None` value as "pick any device".
#[derive(Copy, Clone, Debug, Eq, PartialEq, GenericOverIp)]
pub enum MulticastMembershipInterfaceSelector<A: IpAddress, D> {
    /// Use the specified interface.
    Specified(MulticastInterfaceSelector<A, D>),
    /// Pick any device with a route to the multicast target address.
    AnyInterfaceWithRoute,
}

impl<A: IpAddress, D> From<MulticastInterfaceSelector<A, D>>
    for MulticastMembershipInterfaceSelector<A, D>
{
    fn from(selector: MulticastInterfaceSelector<A, D>) -> Self {
        Self::Specified(selector)
    }
}

/// Sets the specified socket's membership status for the given group.
///
/// If `id` is unbound, the membership state will take effect when it is bound.
/// An error is returned if the membership change request is invalid (e.g.
/// leaving a group that was not joined, or joining a group multiple times) or
/// if the device to use to join is unspecified or conflicts with the existing
/// socket state.
pub(crate) fn set_multicast_membership<
    A: SocketMapAddrSpec,
    C: DatagramStateNonSyncContext<A>,
    SC: DatagramStateContext<A, C, S>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: impl Into<DatagramSocketId<S>>,
    multicast_group: MulticastAddr<A::IpAddr>,
    interface: MulticastMembershipInterfaceSelector<A::IpAddr, A::DeviceId>,
    want_membership: bool,
) -> Result<(), SetMulticastMembershipError>
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ListenerAddrState:
        SocketMapAddrStateSpec<Id = S::ListenerId, SharingState = S::ListenerSharingState>,
    S::ConnAddrState: SocketMapAddrStateSpec<Id = S::ConnId, SharingState = S::ConnSharingState>,
{
    let id = id.into();

    sync_ctx.with_sockets_mut(|sync_ctx, state, _allocator| {
        let DatagramSockets { bound, unbound } = state;
        let bound_device = match id.clone() {
            DatagramSocketId::Unbound(id) => {
                let UnboundSocketState { device, sharing: _, ip_options: _ } =
                    unbound.get(id.into()).expect("unbound UDP socket not found");
                device
            }
            DatagramSocketId::Bound(DatagramBoundId::Listener(id)) => {
                let (_, _, ListenerAddr { ip: _, device }): &(
                    ListenerState<_, _>,
                    S::ListenerSharingState,
                    _,
                ) = bound.listeners().get_by_id(&id).expect("Listening socket not found");
                device
            }
            DatagramSocketId::Bound(DatagramBoundId::Connected(id)) => {
                let (_, _, ConnAddr { ip: _, device }): &(ConnState<_, _>, S::ConnSharingState, _) =
                    bound.conns().get_by_id(&id).expect("Connected socket not found");
                device
            }
        };

        let interface = match interface {
            MulticastMembershipInterfaceSelector::Specified(selector) => match selector {
                MulticastInterfaceSelector::Interface(device) => {
                    if bound_device.as_ref().map_or(false, |d| d != &device) {
                        return Err(SetMulticastMembershipError::WrongDevice);
                    } else {
                        device
                    }
                }
                MulticastInterfaceSelector::LocalAddress(addr) => {
                    pick_interface_for_addr(sync_ctx, multicast_group, Some(addr))?
                }
            },
            MulticastMembershipInterfaceSelector::AnyInterfaceWithRoute => {
                if let Some(bound_device) = bound_device.as_ref() {
                    bound_device.clone()
                } else {
                    pick_interface_for_addr(sync_ctx, multicast_group, None)?
                }
            }
        };

        let DatagramSockets { bound, unbound } = state;
        let ip_options = match id {
            DatagramSocketId::Unbound(id) => {
                let UnboundSocketState { device: _, sharing: _, ip_options } =
                    unbound.get_mut(id.into()).expect("unbound UDP socket not found");
                ip_options
            }

            DatagramSocketId::Bound(DatagramBoundId::Listener(id)) => {
                let (ListenerState { ip_options }, _, _): (
                    _,
                    &S::ListenerSharingState,
                    &ListenerAddr<_, _, _>,
                ) = bound.listeners_mut().get_by_id_mut(&id).expect("Listening socket not found");
                ip_options
            }
            DatagramSocketId::Bound(DatagramBoundId::Connected(id)) => {
                let (ConnState { socket, clear_device_on_disconnect: _ }, _, _): (
                    _,
                    &S::ConnSharingState,
                    &ConnAddr<_, _, _, _>,
                ) = bound.conns_mut().get_by_id_mut(&id).expect("Connected socket not found");
                socket.options_mut()
            }
        };

        let IpOptions { multicast_memberships, hop_limits: _ } = ip_options;
        match multicast_memberships
            .apply_membership_change(multicast_group, &interface, want_membership)
            .ok_or(SetMulticastMembershipError::NoMembershipChange)?
        {
            MulticastMembershipChange::Join => {
                sync_ctx.join_multicast_group(ctx, &interface, multicast_group)
            }
            MulticastMembershipChange::Leave => {
                sync_ctx.leave_multicast_group(ctx, &interface, multicast_group)
            }
        }

        Ok(())
    })
}

fn get_options_device<A: SocketMapAddrSpec, S: DatagramSocketSpec<A>>(
    DatagramSockets { bound, unbound }: &DatagramSockets<A, S>,
    id: DatagramSocketId<S>,
) -> (&IpOptions<A::IpAddr, A::DeviceId>, &Option<A::DeviceId>)
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ListenerAddrState:
        SocketMapAddrStateSpec<Id = S::ListenerId, SharingState = S::ListenerSharingState>,
    S::ConnAddrState: SocketMapAddrStateSpec<Id = S::ConnId, SharingState = S::ConnSharingState>,
    A::IpVersion: IpExt,
{
    match id {
        DatagramSocketId::Unbound(id) => {
            let UnboundSocketState { ip_options, device, sharing: _ } =
                unbound.get(id.into()).expect("unbound UDP socket not found");
            (ip_options, device)
        }
        DatagramSocketId::Bound(DatagramBoundId::Listener(id)) => {
            let (ListenerState { ip_options }, _, ListenerAddr { device, ip: _ }): &(
                _,
                S::ListenerSharingState,
                _,
            ) = bound.listeners().get_by_id(&id).expect("listening socket not found");
            (ip_options, device)
        }
        DatagramSocketId::Bound(DatagramBoundId::Connected(id)) => {
            let (
                ConnState { socket, clear_device_on_disconnect: _ },
                _,
                ConnAddr { device, ip: _ },
            ): &(_, S::ConnSharingState, _) =
                bound.conns().get_by_id(&id).expect("connected socket not found");
            (socket.options(), device)
        }
    }
}

fn get_options_mut<A: SocketMapAddrSpec, S: DatagramSocketSpec<A>>(
    DatagramSockets { bound, unbound }: &mut DatagramSockets<A, S>,
    id: DatagramSocketId<S>,
) -> &mut IpOptions<A::IpAddr, A::DeviceId>
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ListenerAddrState:
        SocketMapAddrStateSpec<Id = S::ListenerId, SharingState = S::ListenerSharingState>,
    S::ConnAddrState: SocketMapAddrStateSpec<Id = S::ConnId, SharingState = S::ConnSharingState>,
    A::IpVersion: IpExt,
{
    match id {
        DatagramSocketId::Unbound(id) => {
            let UnboundSocketState { ip_options, device: _, sharing: _ } =
                unbound.get_mut(id.into()).expect("unbound UDP socket not found");
            ip_options
        }
        DatagramSocketId::Bound(DatagramBoundId::Listener(id)) => {
            let (ListenerState { ip_options }, _, _): (
                _,
                &S::ListenerSharingState,
                &ListenerAddr<_, _, _>,
            ) = bound.listeners_mut().get_by_id_mut(&id).expect("listening socket not found");
            ip_options
        }
        DatagramSocketId::Bound(DatagramBoundId::Connected(id)) => {
            let (ConnState { socket, clear_device_on_disconnect: _ }, _, _): (
                _,
                &S::ConnSharingState,
                &ConnAddr<_, _, _, _>,
            ) = bound.conns_mut().get_by_id_mut(&id).expect("connected socket not found");
            socket.options_mut()
        }
    }
}

pub(crate) fn update_ip_hop_limit<
    A: SocketMapAddrSpec,
    SC: DatagramStateContext<A, C, S>,
    C: DatagramStateNonSyncContext<A>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &mut SC,
    _ctx: &mut C,
    id: impl Into<DatagramSocketId<S>>,
    update: impl FnOnce(&mut SocketHopLimits),
) where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ListenerAddrState:
        SocketMapAddrStateSpec<Id = S::ListenerId, SharingState = S::ListenerSharingState>,
    S::ConnAddrState: SocketMapAddrStateSpec<Id = S::ConnId, SharingState = S::ConnSharingState>,
    A::IpVersion: IpExt,
{
    sync_ctx.with_sockets_mut(|_sync_ctx, sockets, _allocator| {
        let options = get_options_mut(sockets, id.into());

        update(&mut options.hop_limits)
    })
}

pub(crate) fn get_ip_hop_limits<
    A: SocketMapAddrSpec,
    SC: DatagramStateContext<A, C, S>,
    C: DatagramStateNonSyncContext<A>,
    S: DatagramSocketSpec<A>,
>(
    sync_ctx: &SC,
    _ctx: &C,
    id: impl Into<DatagramSocketId<S>>,
) -> HopLimits
where
    Bound<S>: Tagged<AddrVec<A>>,
    S::ListenerAddrState:
        SocketMapAddrStateSpec<Id = S::ListenerId, SharingState = S::ListenerSharingState>,
    S::ConnAddrState: SocketMapAddrStateSpec<Id = S::ConnId, SharingState = S::ConnSharingState>,
    A::IpVersion: IpExt,
{
    sync_ctx.with_sockets(|sync_ctx, sockets| {
        let (options, device) = get_options_device(sockets, id.into());
        let IpOptions { hop_limits, multicast_memberships: _ } = options;
        hop_limits.get_limits_with_defaults(&sync_ctx.get_default_hop_limits(device.as_ref()))
    })
}

#[cfg(test)]
mod test {
    use core::{convert::Infallible as Never, marker::PhantomData};

    use derivative::Derivative;
    use ip_test_macro::ip_test;
    use net_types::ip::{Ip, Ipv4, Ipv6};
    use nonzero_ext::nonzero;

    use crate::{
        data_structures::socketmap::SocketMap,
        ip::{
            device::state::IpDeviceStateIpExt, socket::testutil::FakeIpSocketCtx,
            testutil::FakeDeviceId, DEFAULT_HOP_LIMITS,
        },
        socket::{IncompatibleError, InsertError, RemoveResult},
        testutil::FakeNonSyncCtx,
    };

    use super::*;

    trait DatagramIpExt: Ip + IpExt + IpDeviceStateIpExt {}

    impl DatagramIpExt for Ipv4 {}
    impl DatagramIpExt for Ipv6 {}

    struct FakeAddrSpec<I, D>(Never, PhantomData<(I, D)>);

    impl<I: IpExt, D: IpDeviceId> SocketMapAddrSpec for FakeAddrSpec<I, D> {
        type DeviceId = D;
        type IpAddr = I::Addr;
        type IpVersion = I;
        type LocalIdentifier = u8;
        type RemoteIdentifier = char;
    }

    struct FakeStateSpec<I, D>(Never, PhantomData<(I, D)>);

    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    struct Tag;

    #[derive(Copy, Clone, Debug, Default, Eq, PartialEq)]
    struct Sharing;

    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    struct Id<T>(usize, PhantomData<T>);

    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    struct Conn;
    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    struct Listen;
    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    struct Unbound;

    impl<S> From<usize> for Id<S> {
        fn from(u: usize) -> Self {
            Self(u, PhantomData)
        }
    }

    impl<S> From<Id<S>> for usize {
        fn from(Id(u, _): Id<S>) -> Self {
            u
        }
    }

    impl<I: DatagramIpExt, D: IpDeviceId> SocketMapStateSpec for FakeStateSpec<I, D> {
        type AddrVecTag = Tag;
        type ConnAddrState = Id<Conn>;
        type ConnId = Id<Conn>;
        type ConnSharingState = Sharing;
        type ConnState = ConnState<I, D>;
        type ListenerAddrState = Id<Listen>;
        type ListenerId = Id<Listen>;
        type ListenerSharingState = Sharing;
        type ListenerState = ListenerState<I::Addr, D>;
    }

    impl<A, S> Tagged<A> for Id<S> {
        type Tag = Tag;
        fn tag(&self, _address: &A) -> Self::Tag {
            Tag
        }
    }

    impl<I: DatagramIpExt, D: IpDeviceId> From<Id<Conn>> for DatagramSocketId<FakeStateSpec<I, D>> {
        fn from(u: Id<Conn>) -> Self {
            DatagramSocketId::Bound(DatagramBoundId::Connected(u))
        }
    }

    impl<I: DatagramIpExt, D: IpDeviceId> From<Id<Listen>> for DatagramSocketId<FakeStateSpec<I, D>> {
        fn from(u: Id<Listen>) -> Self {
            DatagramSocketId::Bound(DatagramBoundId::Listener(u))
        }
    }

    impl<I: DatagramIpExt, D: IpDeviceId> From<Id<Unbound>> for DatagramSocketId<FakeStateSpec<I, D>> {
        fn from(u: Id<Unbound>) -> Self {
            DatagramSocketId::Unbound(u)
        }
    }

    impl<I: DatagramIpExt, D: IpDeviceId> DatagramSocketStateSpec for FakeStateSpec<I, D> {
        type UnboundId = Id<Unbound>;
        type UnboundSharingState = Sharing;
    }

    impl<A, I: DatagramIpExt, D: IpDeviceId> SocketMapConflictPolicy<A, Sharing, FakeAddrSpec<I, D>>
        for FakeStateSpec<I, D>
    {
        fn check_for_conflicts(
            _new_sharing_state: &Sharing,
            _addr: &A,
            _socketmap: &SocketMap<AddrVec<FakeAddrSpec<I, D>>, Bound<Self>>,
        ) -> Result<(), InsertError>
        where
            Bound<Self>: Tagged<AddrVec<FakeAddrSpec<I, D>>>,
        {
            // Addresses are completely independent and shadowing doesn't cause
            // conflicts.
            Ok(())
        }
    }

    impl<I: DatagramIpExt, D: IpDeviceId> DatagramSocketSpec<FakeAddrSpec<I, D>>
        for FakeStateSpec<I, D>
    {
        type Serializer<B: BufferMut> = B;
        fn make_packet<B: BufferMut>(
            body: B,
            _addr: &ConnIpAddr<
                <FakeAddrSpec<I, D> as SocketMapAddrSpec>::IpAddr,
                <FakeAddrSpec<I, D> as SocketMapAddrSpec>::LocalIdentifier,
                <FakeAddrSpec<I, D> as SocketMapAddrSpec>::RemoteIdentifier,
            >,
        ) -> Self::Serializer<B> {
            body
        }
    }

    impl<S> SocketMapAddrStateSpec for Id<S> {
        type Id = Self;
        type SharingState = Sharing;
        type Inserter<'a> = Never where Self: 'a;

        fn new(_sharing: &Self::SharingState, id: Self) -> Self {
            id
        }
        fn try_get_inserter<'a, 'b>(
            &'b mut self,
            _new_sharing_state: &'a Self::SharingState,
        ) -> Result<Self::Inserter<'b>, IncompatibleError> {
            Err(IncompatibleError)
        }
        fn could_insert(
            &self,
            _new_sharing_state: &Self::SharingState,
        ) -> Result<(), IncompatibleError> {
            Err(IncompatibleError)
        }
        fn remove_by_id(&mut self, _id: Self::Id) -> RemoveResult {
            RemoveResult::IsLast
        }
    }

    #[derive(Derivative)]
    #[derivative(Default(bound = ""))]
    struct FakeDatagramState<I: DatagramIpExt, D: IpDeviceId> {
        sockets: DatagramSockets<FakeAddrSpec<I, D>, FakeStateSpec<I, D>>,
        state: FakeIpSocketCtx<I, D>,
    }

    impl<I: DatagramIpExt, D: IpDeviceId + 'static>
        DatagramStateContext<FakeAddrSpec<I, D>, FakeNonSyncCtx, FakeStateSpec<I, D>>
        for FakeDatagramState<I, D>
    {
        type IpSocketsCtx = FakeIpSocketCtx<I, D>;
        type LocalIdAllocator = ();

        fn with_sockets<
            O,
            F: FnOnce(
                &Self::IpSocketsCtx,
                &DatagramSockets<FakeAddrSpec<I, D>, FakeStateSpec<I, D>>,
            ) -> O,
        >(
            &self,
            cb: F,
        ) -> O {
            let Self { sockets, state } = self;
            cb(state, sockets)
        }

        fn with_sockets_mut<
            O,
            F: FnOnce(
                &mut Self::IpSocketsCtx,
                &mut DatagramSockets<FakeAddrSpec<I, D>, FakeStateSpec<I, D>>,
                &mut Self::LocalIdAllocator,
            ) -> O,
        >(
            &mut self,
            cb: F,
        ) -> O {
            let Self { sockets, state } = self;
            cb(state, sockets, &mut ())
        }
    }

    impl<I: IpExt> DatagramStateNonSyncContext<FakeAddrSpec<I, FakeDeviceId>> for FakeNonSyncCtx {
        fn try_alloc_listen_identifier(
            &mut self,
            _is_available: impl Fn(u8) -> Result<(), InUseError>,
        ) -> Option<<FakeAddrSpec<I, FakeDeviceId> as SocketMapAddrSpec>::LocalIdentifier> {
            unimplemented!("not required for any existing tests")
        }
    }

    impl<I: DatagramIpExt, D: IpDeviceId + 'static>
        LocalIdentifierAllocator<FakeAddrSpec<I, D>, FakeNonSyncCtx, FakeStateSpec<I, D>> for ()
    {
        fn try_alloc_local_id(
            &mut self,
            bound: &BoundSocketMap<FakeAddrSpec<I, D>, FakeStateSpec<I, D>>,
            _ctx: &mut FakeNonSyncCtx,
            _flow: DatagramFlowId<
                <FakeAddrSpec<I, D> as SocketMapAddrSpec>::IpAddr,
                <FakeAddrSpec<I, D> as SocketMapAddrSpec>::RemoteIdentifier,
            >,
        ) -> Option<<FakeAddrSpec<I, D> as SocketMapAddrSpec>::LocalIdentifier> {
            (0..u8::MAX).find_map(|identifier| {
                bound
                    .listeners()
                    .could_insert(
                        &ListenerAddr {
                            device: None,
                            ip: ListenerIpAddr { addr: None, identifier },
                        },
                        &Default::default(),
                    )
                    .is_ok()
                    .then_some(identifier)
            })
        }
    }

    #[ip_test]
    fn set_get_hop_limits<I: Ip + DatagramIpExt>() {
        let mut sync_ctx = FakeDatagramState::<I, FakeDeviceId>::default();
        let mut non_sync_ctx = FakeNonSyncCtx::default();

        let unbound = create_unbound(&mut sync_ctx);
        const EXPECTED_HOP_LIMITS: HopLimits =
            HopLimits { unicast: nonzero!(45u8), multicast: nonzero!(23u8) };

        update_ip_hop_limit(&mut sync_ctx, &mut non_sync_ctx, unbound, |limits| {
            *limits = SocketHopLimits {
                unicast: Some(EXPECTED_HOP_LIMITS.unicast),
                multicast: Some(EXPECTED_HOP_LIMITS.multicast),
            }
        });

        assert_eq!(get_ip_hop_limits(&sync_ctx, &non_sync_ctx, unbound), EXPECTED_HOP_LIMITS);
    }

    #[ip_test]
    fn default_hop_limits<I: Ip + DatagramIpExt>() {
        let mut sync_ctx = FakeDatagramState::<I, FakeDeviceId>::default();
        let mut non_sync_ctx = FakeNonSyncCtx::default();

        let unbound = create_unbound(&mut sync_ctx);
        assert_eq!(get_ip_hop_limits(&sync_ctx, &non_sync_ctx, unbound), DEFAULT_HOP_LIMITS);

        update_ip_hop_limit(&mut sync_ctx, &mut non_sync_ctx, unbound, |limits| {
            *limits =
                SocketHopLimits { unicast: Some(nonzero!(1u8)), multicast: Some(nonzero!(1u8)) }
        });

        // The limits no longer match the default.
        assert_ne!(get_ip_hop_limits(&sync_ctx, &non_sync_ctx, unbound), DEFAULT_HOP_LIMITS);

        // Clear the hop limits set on the socket.
        update_ip_hop_limit(&mut sync_ctx, &mut non_sync_ctx, unbound, |limits| {
            *limits = Default::default()
        });

        // The values should be back at the defaults.
        assert_eq!(get_ip_hop_limits(&sync_ctx, &non_sync_ctx, unbound), DEFAULT_HOP_LIMITS);
    }

    #[ip_test]
    fn bind_device_unbound<I: Ip + DatagramIpExt>() {
        let mut sync_ctx = FakeDatagramState::<I, FakeDeviceId>::default();
        let mut non_sync_ctx = FakeNonSyncCtx::default();

        let unbound = create_unbound(&mut sync_ctx);

        set_unbound_device(&mut sync_ctx, &mut non_sync_ctx, unbound, Some(&FakeDeviceId));
        assert_eq!(get_bound_device(&sync_ctx, &non_sync_ctx, unbound), Some(FakeDeviceId));

        set_unbound_device(&mut sync_ctx, &mut non_sync_ctx, unbound, None);
        assert_eq!(get_bound_device(&sync_ctx, &non_sync_ctx, unbound), None);
    }
}
