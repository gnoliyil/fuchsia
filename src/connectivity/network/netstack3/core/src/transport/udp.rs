// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The User Datagram Protocol (UDP).

use alloc::{collections::hash_map::DefaultHasher, vec::Vec};
use core::{
    fmt::Debug,
    hash::{Hash, Hasher},
    num::{NonZeroU16, NonZeroU8, NonZeroUsize},
    ops::RangeInclusive,
};
use lock_order::{lock::UnlockedAccess, Locked};

use dense_map::EntryKey;
use derivative::Derivative;
use either::Either;
use net_types::{
    ip::{
        GenericOverIp, Ip, IpAddress, IpInvariant, IpMarked, IpVersion, IpVersionMarker, Ipv4,
        Ipv4Addr, Ipv6, Ipv6Addr,
    },
    MulticastAddr, SpecifiedAddr, Witness,
};
use packet::{BufferMut, Nested, ParsablePacket, Serializer};
use packet_formats::{
    ip::{IpProto, IpProtoExt},
    udp::{UdpPacket, UdpPacketBuilder, UdpParseArgs},
};
use thiserror::Error;
use tracing::{debug, trace};

pub(crate) use crate::socket::datagram::IpExt;
use crate::{
    algorithm::{PortAlloc, PortAllocImpl, ProtocolFlowId},
    context::{CounterContext, InstantContext, NonTestCtxMarker, RngContext, TracingContext},
    convert::BidirectionalConverter,
    counters::Counter,
    data_structures::socketmap::{IterShadows as _, SocketMap, Tagged},
    device::{AnyDevice, DeviceId, DeviceIdContext, Id, WeakDeviceId, WeakId},
    error::{LocalAddressError, SocketError, ZonedAddressError},
    ip::{
        icmp::IcmpIpExt,
        socket::{IpSockCreateAndSendError, IpSockCreationError, IpSockSendError},
        IpTransportContext, MulticastMembershipHandler, TransportIpContext, TransportReceiveError,
    },
    socket::{
        address::{
            AddrIsMappedError, ConnAddr, ConnIpAddr, DualStackConnIpAddr, DualStackListenerIpAddr,
            ListenerAddr, ListenerIpAddr, SocketIpAddr, SocketZonedIpAddr,
        },
        datagram::{
            self, AddrEntry, BoundSocketState as DatagramBoundSocketState,
            BoundSocketStateType as DatagramBoundSocketStateType,
            BoundSockets as DatagramBoundSockets, ConnectError, DatagramBoundStateContext,
            DatagramFlowId, DatagramSocketMapSpec, DatagramSocketSpec, DatagramStateContext,
            DualStackConnState, DualStackDatagramBoundStateContext, DualStackIpExt, EitherIpSocket,
            ExpectedConnError, ExpectedUnboundError, FoundSockets, InUseError, IpOptions,
            LocalIdentifierAllocator, MulticastMembershipInterfaceSelector,
            NonDualStackDatagramBoundStateContext, SendError as DatagramSendError,
            SetMulticastMembershipError, ShutdownType, SocketHopLimits,
            SocketInfo as DatagramSocketInfo, SocketState as DatagramSocketState,
            SocketsState as DatagramSocketsState,
        },
        AddrVec, Bound, IncompatibleError, InsertError, ListenerAddrInfo, MaybeDualStack,
        NotDualStackCapableError, RemoveResult, SetDualStackEnabledError, SocketAddrType,
        SocketMapAddrSpec, SocketMapAddrStateSpec, SocketMapConflictPolicy, SocketMapStateSpec,
    },
    sync::RwLock,
    trace_duration, transport, SyncCtx,
};

/// A builder for UDP layer state.
#[derive(Clone)]
pub(crate) struct UdpStateBuilder {
    send_port_unreachable: bool,
}

impl Default for UdpStateBuilder {
    fn default() -> UdpStateBuilder {
        UdpStateBuilder { send_port_unreachable: false }
    }
}

impl UdpStateBuilder {
    /// Enable or disable sending ICMP Port Unreachable messages in response to
    /// inbound UDP packets for which a corresponding local socket does not
    /// exist (default: disabled).
    ///
    /// Responding with an ICMP Port Unreachable error is a vector for reflected
    /// Denial-of-Service (DoS) attacks. The attacker can send a UDP packet to a
    /// closed port with the source address set to the address of the victim,
    /// and ICMP response will be sent there.
    ///
    /// According to [RFC 1122 Section 4.1.3.1], "\[i\]f a datagram arrives
    /// addressed to a UDP port for which there is no pending LISTEN call, UDP
    /// SHOULD send an ICMP Port Unreachable message." Since an ICMP response is
    /// not mandatory, and due to the security risks described, responses are
    /// disabled by default.
    ///
    /// [RFC 1122 Section 4.1.3.1]: https://tools.ietf.org/html/rfc1122#section-4.1.3.1
    #[cfg(test)]
    pub(crate) fn send_port_unreachable(&mut self, send_port_unreachable: bool) -> &mut Self {
        self.send_port_unreachable = send_port_unreachable;
        self
    }

    pub(crate) fn build<I: IpExt, D: WeakId>(self) -> UdpState<I, D> {
        UdpState {
            sockets: Default::default(),
            send_port_unreachable: self.send_port_unreachable,
            counters: Default::default(),
        }
    }
}

/// Convenience alias to make names shorter.
pub(crate) type UdpBoundSocketMap<I, D> = DatagramBoundSockets<I, D, Udp, (Udp, I, D)>;

impl<I: IpExt, NewIp: IpExt, D: WeakId> GenericOverIp<NewIp> for UdpBoundSocketMap<I, D> {
    type Type = UdpBoundSocketMap<NewIp, D>;
}

#[derive(Derivative, GenericOverIp)]
#[generic_over_ip(I, Ip)]
#[derivative(Default(bound = ""))]
pub(crate) struct BoundSockets<I: IpExt, D: WeakId> {
    bound_sockets: UdpBoundSocketMap<I, D>,
    /// lazy_port_alloc is lazy-initialized when it's used.
    lazy_port_alloc: Option<PortAlloc<UdpBoundSocketMap<I, D>>>,
}

pub(crate) type SocketsState<I, D> = DatagramSocketsState<I, D, Udp>;

/// A collection of UDP sockets.
#[derive(Derivative)]
#[derivative(Default(bound = ""))]
pub(crate) struct Sockets<I: Ip + IpExt, D: WeakId> {
    pub(crate) sockets_state: RwLock<SocketsState<I, D>>,
    pub(crate) bound: RwLock<BoundSockets<I, D>>,
}

/// The state associated with the UDP protocol.
///
/// `D` is the device ID type.
pub(crate) struct UdpState<I: IpExt, D: WeakId> {
    pub(crate) sockets: Sockets<I, D>,
    pub(crate) send_port_unreachable: bool,
    pub(crate) counters: UdpCounters<I>,
}

impl<I: IpExt, D: WeakId> Default for UdpState<I, D> {
    fn default() -> UdpState<I, D> {
        UdpStateBuilder::default().build()
    }
}

/// Counters for the UDP layer.
pub type UdpCounters<I> = IpMarked<I, UdpCountersInner>;

/// Counters for the UDP layer.
#[derive(Default)]
pub struct UdpCountersInner {
    /// Count of ICMP error messages received.
    pub rx_icmp_error: Counter,
    /// Count of UDP datagrams received from the IP layer, including error
    /// cases.
    pub rx: Counter,
    /// Count of incoming UDP datagrams dropped because it contained a mapped IP
    /// address in the header.
    pub rx_mapped_addr: Counter,
    /// Count of incoming UDP datagrams dropped because of an unknown
    /// destination port.
    pub rx_unknown_dest_port: Counter,
    /// Count of incoming UDP datagrams dropped because their UDP header was in
    /// a malformed state.
    pub rx_malformed: Counter,
    /// Count of outgoing UDP datagrams sent from the socket layer, including
    /// error cases.
    pub tx: Counter,
    /// Count of outgoing UDP datagrams which failed to be sent out of the
    /// transport layer.
    pub tx_error: Counter,
}

impl<BC: crate::BindingsContext, I: Ip> UnlockedAccess<crate::lock_ordering::UdpCounters<I>>
    for SyncCtx<BC>
{
    type Data = UdpCounters<I>;
    type Guard<'l> = &'l UdpCounters<I> where Self: 'l;

    fn access(&self) -> Self::Guard<'_> {
        self.state.udp_counters()
    }
}

impl<BC: crate::BindingsContext, I: Ip, L> CounterContext<UdpCounters<I>>
    for Locked<&SyncCtx<BC>, L>
{
    fn with_counters<O, F: FnOnce(&UdpCounters<I>) -> O>(&self, cb: F) -> O {
        cb(self.unlocked_access::<crate::lock_ordering::UdpCounters<I>>())
    }
}

/// Uninstantiatable type for implementing [`DatagramSocketSpec`].
pub(crate) enum Udp {}

/// Produces an iterator over eligible receiving socket addresses.
#[cfg(test)]
fn iter_receiving_addrs<I: Ip + IpExt, D: WeakId>(
    addr: ConnIpAddr<I::Addr, NonZeroU16, UdpRemotePort>,
    device: D,
) -> impl Iterator<Item = AddrVec<I, D, Udp>> {
    crate::socket::address::AddrVecIter::with_device(addr.into(), device)
}

fn check_posix_sharing<I: IpExt, D: WeakId>(
    new_sharing: Sharing,
    dest: AddrVec<I, D, Udp>,
    socketmap: &SocketMap<AddrVec<I, D, Udp>, Bound<(Udp, I, D)>>,
) -> Result<(), InsertError> {
    // Having a value present at a shadowed address is disqualifying, unless
    // both the new and existing sockets allow port sharing.
    if dest.iter_shadows().any(|a| {
        socketmap.get(&a).map_or(false, |bound| {
            !bound.tag(&a).to_sharing_options().is_shareable_with_new_state(new_sharing)
        })
    }) {
        return Err(InsertError::ShadowAddrExists);
    }

    // Likewise, the presence of a value that shadows the target address is
    // disqualifying unless both allow port sharing.
    match &dest {
        AddrVec::Conn(ConnAddr { ip: _, device: None }) | AddrVec::Listen(_) => {
            if socketmap.descendant_counts(&dest).any(|(tag, _): &(_, NonZeroUsize)| {
                !tag.to_sharing_options().is_shareable_with_new_state(new_sharing)
            }) {
                return Err(InsertError::ShadowerExists);
            }
        }
        AddrVec::Conn(ConnAddr { ip: _, device: Some(_) }) => {
            // No need to check shadows here because there are no addresses
            // that shadow a ConnAddr with a device.
            debug_assert_eq!(socketmap.descendant_counts(&dest).len(), 0)
        }
    }

    // There are a few combinations of addresses that can conflict with
    // each other even though there is not a direct shadowing relationship:
    // - listener address with device and connected address without.
    // - "any IP" listener with device and specific IP listener without.
    // - "any IP" listener with device and connected address without.
    //
    // The complication is that since these pairs of addresses don't have a
    // direct shadowing relationship, it's not possible to query for one
    // from the other in the socketmap without a linear scan. Instead. we
    // rely on the fact that the tag values in the socket map have different
    // values for entries with and without device IDs specified.
    fn conflict_exists<I: IpExt, D: WeakId>(
        new_sharing: Sharing,
        socketmap: &SocketMap<AddrVec<I, D, Udp>, Bound<(Udp, I, D)>>,
        addr: impl Into<AddrVec<I, D, Udp>>,
        mut is_conflicting: impl FnMut(&AddrVecTag) -> bool,
    ) -> bool {
        socketmap.descendant_counts(&addr.into()).any(|(tag, _): &(_, NonZeroUsize)| {
            is_conflicting(tag)
                && !tag.to_sharing_options().is_shareable_with_new_state(new_sharing)
        })
    }

    let found_indirect_conflict = match dest {
        AddrVec::Listen(ListenerAddr {
            ip: ListenerIpAddr { addr: None, identifier },
            device: Some(_device),
        }) => {
            // An address with a device will shadow an any-IP listener
            // `dest` with a device so we only need to check for addresses
            // without a device. Likewise, an any-IP listener will directly
            // shadow `dest`, so an indirect conflict can only come from a
            // specific listener or connected socket (without a device).
            conflict_exists(
                new_sharing,
                socketmap,
                ListenerAddr { ip: ListenerIpAddr { addr: None, identifier }, device: None },
                |AddrVecTag { has_device, addr_type, sharing: _ }| {
                    !*has_device
                        && match addr_type {
                            SocketAddrType::SpecificListener | SocketAddrType::Connected => true,
                            SocketAddrType::AnyListener => false,
                        }
                },
            )
        }
        AddrVec::Listen(ListenerAddr {
            ip: ListenerIpAddr { addr: Some(ip), identifier },
            device: Some(_device),
        }) => {
            // A specific-IP listener `dest` with a device will be shadowed
            // by a connected socket with a device and will shadow
            // specific-IP addresses without a device and any-IP listeners
            // with and without devices. That means an indirect conflict can
            // only come from a connected socket without a device.
            conflict_exists(
                new_sharing,
                socketmap,
                ListenerAddr { ip: ListenerIpAddr { addr: Some(ip), identifier }, device: None },
                |AddrVecTag { has_device, addr_type, sharing: _ }| {
                    !*has_device
                        && match addr_type {
                            SocketAddrType::Connected => true,
                            SocketAddrType::AnyListener | SocketAddrType::SpecificListener => false,
                        }
                },
            )
        }
        AddrVec::Listen(ListenerAddr {
            ip: ListenerIpAddr { addr: Some(_), identifier },
            device: None,
        }) => {
            // A specific-IP listener `dest` without a device will be
            // shadowed by a specific-IP listener with a device and by any
            // connected socket (with or without a device).  It will also
            // shadow an any-IP listener without a device, which means an
            // indirect conflict can only come from an any-IP listener with
            // a device.
            conflict_exists(
                new_sharing,
                socketmap,
                ListenerAddr { ip: ListenerIpAddr { addr: None, identifier }, device: None },
                |AddrVecTag { has_device, addr_type, sharing: _ }| {
                    *has_device
                        && match addr_type {
                            SocketAddrType::AnyListener => true,
                            SocketAddrType::SpecificListener | SocketAddrType::Connected => false,
                        }
                },
            )
        }
        AddrVec::Conn(ConnAddr {
            ip: ConnIpAddr { local: (local_ip, local_identifier), remote: _ },
            device: None,
        }) => {
            // A connected socket `dest` without a device shadows listeners
            // without devices, and is shadowed by a connected socket with
            // a device. It can indirectly conflict with listening sockets
            // with devices.

            // Check for specific-IP listeners with devices, which would
            // indirectly conflict.
            conflict_exists(
                new_sharing,
                socketmap,
                ListenerAddr {
                    ip: ListenerIpAddr {
                        addr: Some(local_ip),
                        identifier: local_identifier.clone(),
                    },
                    device: None,
                },
                |AddrVecTag { has_device, addr_type, sharing: _ }| {
                    *has_device
                        && match addr_type {
                            SocketAddrType::SpecificListener => true,
                            SocketAddrType::AnyListener | SocketAddrType::Connected => false,
                        }
                },
            ) ||
            // Check for any-IP listeners with devices since they conflict.
            // Note that this check cannot be combined with the one above
            // since they examine tag counts for different addresses. While
            // the counts of tags matched above *will* also be propagated to
            // the any-IP listener entry, they would be indistinguishable
            // from non-conflicting counts. For a connected address with
            // `Some(local_ip)`, the descendant counts at the listener
            // address with `addr = None` would include any
            // `SpecificListener` tags for both addresses with
            // `Some(local_ip)` and `Some(other_local_ip)`. The former
            // indirectly conflicts with `dest` but the latter does not,
            // hence this second distinct check.
            conflict_exists(
                new_sharing,
                socketmap,
                ListenerAddr {
                    ip: ListenerIpAddr { addr: None, identifier: local_identifier },
                    device: None,
                },
                |AddrVecTag { has_device, addr_type, sharing: _ }| {
                    *has_device
                        && match addr_type {
                            SocketAddrType::AnyListener => true,
                            SocketAddrType::SpecificListener | SocketAddrType::Connected => false,
                        }
                },
            )
        }
        AddrVec::Listen(ListenerAddr {
            ip: ListenerIpAddr { addr: None, identifier: _ },
            device: _,
        }) => false,
        AddrVec::Conn(ConnAddr { ip: _, device: Some(_device) }) => false,
    };
    if found_indirect_conflict {
        Err(InsertError::IndirectConflict)
    } else {
        Ok(())
    }
}

/// The remote port for a UDP socket.
#[derive(Copy, Clone, Debug, Eq, Hash, PartialEq)]
pub enum UdpRemotePort {
    /// The remote port is set to the following value.
    Set(NonZeroU16),
    /// The remote port is unset (i.e. "0") value. An unset remote port is
    /// treated specially in a few places:
    ///
    /// 1) Attempting to send to an unset remote port results in a
    /// [`UdpSerializeError::RemotePortUnset`] error. Note that this behavior
    /// diverges from Linux, which does allow sending to a remote_port of 0
    /// (supported by `send` but not `send_to`). The rationale for this
    /// divergence originates from RFC 8085 Section 5.1:
    ///
    ///    A UDP sender SHOULD NOT use a source port value of zero.  A source
    ///    port number that cannot be easily determined from the address or
    ///    payload type provides protection at the receiver from data injection
    ///    attacks by off-path devices. A UDP receiver SHOULD NOT bind to port
    ///    zero.
    ///
    ///    Applications SHOULD implement receiver port and address checks at the
    ///    application layer or explicitly request that the operating system
    ///    filter the received packets to prevent receiving packets with an
    ///    arbitrary port.  This measure is designed to provide additional
    ///    protection from data injection attacks from an off-path source (where
    ///    the port values may not be known).
    ///
    /// Combined, these two stanzas recommend hosts discard incoming traffic
    /// destined to remote port 0 for security reasons. Thus we choose to not
    /// allow hosts to send such packets under the assumption that it will be
    /// dropped by the receiving end.
    ///
    /// 2) A socket connected to a remote host on port 0 will not receive any
    /// packets from the remote host. This is because the
    /// [`BoundSocketMap::lookup`] implementation only delivers packets that
    /// specify a remote port to connected sockets with an exact match. Further,
    /// packets that don't specify a remote port are only delivered to listener
    /// sockets. This diverges from Linux (which treats a remote_port of 0) as
    /// wild card. If and when a concrete need for such behavior is identified,
    /// the [`BoundSocketMap`] lookup behavior can be adjusted accordingly.
    Unset,
}

impl From<NonZeroU16> for UdpRemotePort {
    fn from(p: NonZeroU16) -> Self {
        Self::Set(p)
    }
}

impl From<u16> for UdpRemotePort {
    fn from(p: u16) -> Self {
        NonZeroU16::new(p).map(UdpRemotePort::from).unwrap_or(UdpRemotePort::Unset)
    }
}

impl From<UdpRemotePort> for u16 {
    fn from(p: UdpRemotePort) -> Self {
        match p {
            UdpRemotePort::Unset => 0,
            UdpRemotePort::Set(p) => p.into(),
        }
    }
}

impl SocketMapAddrSpec for Udp {
    type RemoteIdentifier = UdpRemotePort;
    type LocalIdentifier = NonZeroU16;
}

impl<I: IpExt, D: Id> SocketMapStateSpec for (Udp, I, D) {
    type ListenerId = I::DualStackBoundSocketId<Udp>;
    type ConnId = I::DualStackBoundSocketId<Udp>;

    type AddrVecTag = AddrVecTag;

    type ListenerSharingState = Sharing;
    type ConnSharingState = Sharing;

    type ListenerAddrState = AddrState<Self::ListenerId>;

    type ConnAddrState = AddrState<Self::ConnId>;
    fn listener_tag(
        ListenerAddrInfo { has_device, specified_addr }: ListenerAddrInfo,
        state: &Self::ListenerAddrState,
    ) -> Self::AddrVecTag {
        AddrVecTag {
            has_device,
            addr_type: specified_addr
                .then_some(SocketAddrType::SpecificListener)
                .unwrap_or(SocketAddrType::AnyListener),
            sharing: state.to_sharing_options(),
        }
    }
    fn connected_tag(has_device: bool, state: &Self::ConnAddrState) -> Self::AddrVecTag {
        AddrVecTag {
            has_device,
            addr_type: SocketAddrType::Connected,
            sharing: state.to_sharing_options(),
        }
    }
}

impl<AA, I: IpExt, D: WeakId> SocketMapConflictPolicy<AA, Sharing, I, D, Udp> for (Udp, I, D)
where
    AA: Into<AddrVec<I, D, Udp>> + Clone,
{
    fn check_insert_conflicts(
        new_sharing_state: &Sharing,
        addr: &AA,
        socketmap: &SocketMap<AddrVec<I, D, Udp>, Bound<Self>>,
    ) -> Result<(), InsertError> {
        check_posix_sharing(*new_sharing_state, addr.clone().into(), socketmap)
    }
}

/// State held for IPv6 sockets related to dual-stack operation.
#[derive(Clone, Debug, Derivative)]
#[derivative(Default)]
pub(crate) struct DualStackSocketState {
    // Match Linux's behavior by enabling dualstack operations by default.
    #[derivative(Default(value = "true"))]
    dual_stack_enabled: bool,
}

/// Serialization errors for Udp Packets.
pub(crate) enum UdpSerializeError {
    /// Disallow sending packets with a remote port of 0. See
    /// [`UdpRemotePort::Unset`] for the rationale.
    RemotePortUnset,
}

impl DatagramSocketSpec for Udp {
    type AddrSpec = Self;
    type SocketId<I: IpExt> = SocketId<I>;
    type OtherStackIpOptions<I: IpExt> = I::OtherStackIpOptions<DualStackSocketState>;
    type ListenerIpAddr<I: IpExt> = I::DualStackListenerIpAddr<NonZeroU16>;
    type ConnIpAddr<I: IpExt> = I::DualStackConnIpAddr<Self>;
    type ConnStateExtra = ();
    type ConnState<I: IpExt, D: Debug + Eq + Hash> = I::DualStackConnState<D, Self>;
    type SocketMapSpec<I: IpExt, D: WeakId> = (Self, I, D);
    type SharingState = Sharing;

    type Serializer<I: IpExt, B: BufferMut> = Nested<B, UdpPacketBuilder<I::Addr>>;
    type SerializeError = UdpSerializeError;

    fn ip_proto<I: IpProtoExt>() -> I::Proto {
        IpProto::Udp.into()
    }

    fn make_bound_socket_map_id<I: IpExt, D: WeakId>(
        s: Self::SocketId<I>,
    ) -> I::DualStackBoundSocketId<Udp> {
        I::into_dual_stack_bound_socket_id(s)
    }

    fn make_packet<I: IpExt, B: BufferMut>(
        body: B,
        addr: &ConnIpAddr<I::Addr, NonZeroU16, UdpRemotePort>,
    ) -> Result<Self::Serializer<I, B>, UdpSerializeError> {
        let ConnIpAddr { local: (local_ip, local_port), remote: (remote_ip, remote_port) } = addr;
        let remote_port = match remote_port {
            UdpRemotePort::Unset => return Err(UdpSerializeError::RemotePortUnset),
            UdpRemotePort::Set(remote_port) => *remote_port,
        };
        Ok(body.encapsulate(UdpPacketBuilder::new(
            local_ip.addr(),
            remote_ip.addr(),
            Some(*local_port),
            remote_port,
        )))
    }

    fn try_alloc_listen_identifier<I: IpExt, D: WeakId>(
        rng: &mut impl crate::RngContext,
        is_available: impl Fn(NonZeroU16) -> Result<(), InUseError>,
    ) -> Option<NonZeroU16> {
        try_alloc_listen_port::<I, D>(rng, is_available)
    }

    fn conn_addr_from_state<I: IpExt, D: Clone + Debug + Eq + Hash>(
        state: &Self::ConnState<I, D>,
    ) -> ConnAddr<Self::ConnIpAddr<I>, D> {
        I::conn_addr_from_state(state)
    }
}

impl<I: IpExt, D: WeakId> DatagramSocketMapSpec<I, D, Udp> for (Udp, I, D) {
    type BoundSocketId = I::DualStackBoundSocketId<Udp>;
}

enum LookupResult<'a, I: IpExt, D: Id> {
    Conn(
        &'a I::DualStackBoundSocketId<Udp>,
        ConnAddr<ConnIpAddr<I::Addr, NonZeroU16, UdpRemotePort>, D>,
    ),
    Listener(
        &'a I::DualStackBoundSocketId<Udp>,
        ListenerAddr<ListenerIpAddr<I::Addr, NonZeroU16>, D>,
    ),
}

#[derive(Hash, Copy, Clone)]
struct SocketSelectorParams<I: Ip, A: AsRef<I::Addr>> {
    src_ip: I::Addr,
    dst_ip: A,
    src_port: u16,
    dst_port: u16,
    _ip: IpVersionMarker<I>,
}

#[derive(Debug, Eq, PartialEq)]
pub(crate) enum AddrState<T> {
    Exclusive(T),
    ReusePort(Vec<T>),
}

impl<'a, A: IpAddress, LI> From<&'a ListenerIpAddr<A, LI>> for SocketAddrType {
    fn from(ListenerIpAddr { addr, identifier: _ }: &'a ListenerIpAddr<A, LI>) -> Self {
        match addr {
            Some(_) => SocketAddrType::SpecificListener,
            None => SocketAddrType::AnyListener,
        }
    }
}

impl<'a, A: IpAddress, LI, RI> From<&'a ConnIpAddr<A, LI, RI>> for SocketAddrType {
    fn from(_: &'a ConnIpAddr<A, LI, RI>) -> Self {
        SocketAddrType::Connected
    }
}

#[derive(Copy, Clone, Debug, Eq, Hash, PartialEq)]
pub(crate) enum Sharing {
    Exclusive,
    ReusePort,
}

impl Default for Sharing {
    fn default() -> Self {
        Self::Exclusive
    }
}

impl Sharing {
    pub(crate) fn is_shareable_with_new_state(&self, new_state: Sharing) -> bool {
        match (self, new_state) {
            (Sharing::Exclusive, Sharing::Exclusive) => false,
            (Sharing::Exclusive, Sharing::ReusePort) => false,
            (Sharing::ReusePort, Sharing::Exclusive) => false,
            (Sharing::ReusePort, Sharing::ReusePort) => true,
        }
    }

    pub(crate) fn is_reuse_port(&self) -> bool {
        match self {
            Sharing::Exclusive => false,
            Sharing::ReusePort => true,
        }
    }
}

#[derive(Copy, Clone, Debug, Eq, Hash, PartialEq)]
pub(crate) struct AddrVecTag {
    pub(crate) has_device: bool,
    pub(crate) addr_type: SocketAddrType,
    pub(crate) sharing: Sharing,
}

pub(crate) trait ToSharingOptions {
    fn to_sharing_options(&self) -> Sharing;
}

impl ToSharingOptions for AddrVecTag {
    fn to_sharing_options(&self) -> Sharing {
        let AddrVecTag { has_device: _, addr_type: _, sharing } = self;
        *sharing
    }
}

impl<T> ToSharingOptions for AddrState<T> {
    fn to_sharing_options(&self) -> Sharing {
        match self {
            AddrState::Exclusive(_) => Sharing::Exclusive,
            AddrState::ReusePort(_) => Sharing::ReusePort,
        }
    }
}

impl<T> ToSharingOptions for (T, Sharing) {
    fn to_sharing_options(&self) -> Sharing {
        let (_state, sharing) = self;
        *sharing
    }
}

impl<I: Debug + Eq> SocketMapAddrStateSpec for AddrState<I> {
    type Id = I;
    type SharingState = Sharing;
    type Inserter<'a> = &'a mut Vec<I> where I: 'a;

    fn new(new_sharing_state: &Sharing, id: I) -> Self {
        match new_sharing_state {
            Sharing::Exclusive => Self::Exclusive(id),
            Sharing::ReusePort => Self::ReusePort(Vec::from([id])),
        }
    }

    fn contains_id(&self, id: &Self::Id) -> bool {
        match self {
            Self::Exclusive(x) => id == x,
            Self::ReusePort(ids) => ids.contains(id),
        }
    }

    fn try_get_inserter<'a, 'b>(
        &'b mut self,
        new_sharing_state: &'a Sharing,
    ) -> Result<&'b mut Vec<I>, IncompatibleError> {
        match (self, new_sharing_state) {
            (AddrState::Exclusive(_), _) | (AddrState::ReusePort(_), Sharing::Exclusive) => {
                Err(IncompatibleError)
            }
            (AddrState::ReusePort(ids), Sharing::ReusePort) => Ok(ids),
        }
    }

    fn could_insert(
        &self,
        new_sharing_state: &Self::SharingState,
    ) -> Result<(), IncompatibleError> {
        match (self, new_sharing_state) {
            (AddrState::Exclusive(_), _) | (_, Sharing::Exclusive) => Err(IncompatibleError),
            (AddrState::ReusePort(_), Sharing::ReusePort) => Ok(()),
        }
    }

    fn remove_by_id(&mut self, id: I) -> RemoveResult {
        match self {
            AddrState::Exclusive(_) => RemoveResult::IsLast,
            AddrState::ReusePort(ids) => {
                let index = ids.iter().position(|i| i == &id).expect("couldn't find ID to remove");
                assert_eq!(ids.swap_remove(index), id);
                if ids.is_empty() {
                    RemoveResult::IsLast
                } else {
                    RemoveResult::Success
                }
            }
        }
    }
}

impl<T> AddrState<T> {
    fn select_receiver<I: Ip, A: AsRef<I::Addr> + Hash>(
        &self,
        selector: SocketSelectorParams<I, A>,
    ) -> &T {
        match self {
            AddrState::Exclusive(id) => id,
            AddrState::ReusePort(ids) => {
                let mut hasher = DefaultHasher::new();
                selector.hash(&mut hasher);
                let index: usize = hasher.finish() as usize % ids.len();
                &ids[index]
            }
        }
    }

    fn collect_all_ids(&self) -> impl Iterator<Item = &'_ T> {
        match self {
            AddrState::Exclusive(id) => Either::Left(core::iter::once(id)),
            AddrState::ReusePort(ids) => Either::Right(ids.iter()),
        }
    }
}

impl<'a, I: Ip + IpExt, D: WeakId + 'a> AddrEntry<'a, I, D, Udp, (Udp, I, D)> {
    /// Returns an iterator that yields a `LookupResult` for each contained ID.
    fn collect_all_ids(self) -> impl Iterator<Item = LookupResult<'a, I, D>> + 'a {
        match self {
            Self::Listen(state, l) => Either::Left(
                state.collect_all_ids().map(move |id| LookupResult::Listener(id, l.clone())),
            ),
            Self::Conn(state, c) => Either::Right(
                state.collect_all_ids().map(move |id| LookupResult::Conn(id, c.clone())),
            ),
        }
    }

    /// Returns a `LookupResult` for the contained ID that matches the selector.
    fn select_receiver<A: AsRef<I::Addr> + Hash>(
        self,
        selector: SocketSelectorParams<I, A>,
    ) -> LookupResult<'a, I, D> {
        match self {
            Self::Listen(state, l) => LookupResult::Listener(state.select_receiver(selector), l),
            Self::Conn(state, c) => LookupResult::Conn(state.select_receiver(selector), c),
        }
    }
}

/// Finds the socket(s) that should receive an incoming packet.
///
/// Uses the provided addresses and receiving device to look up sockets that
/// should receive a matching incoming packet. The returned iterator may
/// yield 0, 1, or multiple sockets.
fn lookup<'s, I: Ip + IpExt, D: WeakId>(
    bound: &'s DatagramBoundSockets<I, D, Udp, (Udp, I, D)>,
    (src_ip, src_port): (Option<SocketIpAddr<I::Addr>>, Option<NonZeroU16>),
    (dst_ip, dst_port): (SocketIpAddr<I::Addr>, NonZeroU16),
    device: D,
) -> impl Iterator<Item = LookupResult<'s, I, D>> + 's {
    let matching_entries = bound.iter_receivers(
        (src_ip, src_port.map(UdpRemotePort::from)),
        (dst_ip, dst_port),
        device,
    );
    match matching_entries {
        None => Either::Left(None),
        Some(FoundSockets::Single(entry)) => {
            let selector = SocketSelectorParams::<_, SpecifiedAddr<I::Addr>> {
                src_ip: src_ip.map_or(I::UNSPECIFIED_ADDRESS, SocketIpAddr::addr),
                dst_ip: dst_ip.into(),
                src_port: src_port.map_or(0, NonZeroU16::get),
                dst_port: dst_port.get(),
                _ip: IpVersionMarker::default(),
            };
            Either::Left(Some(entry.select_receiver(selector)))
        }

        Some(FoundSockets::Multicast(entries)) => {
            Either::Right(entries.into_iter().flat_map(AddrEntry::collect_all_ids))
        }
    }
    .into_iter()
}

/// Helper function to allocate a listen port.
///
/// Finds a random ephemeral port that is not in the provided `used_ports` set.
fn try_alloc_listen_port<I: IpExt, D: WeakId>(
    bindings_ctx: &mut impl RngContext,
    is_available: impl Fn(NonZeroU16) -> Result<(), InUseError>,
) -> Option<NonZeroU16> {
    let mut port = UdpBoundSocketMap::<I, D>::rand_ephemeral(&mut bindings_ctx.rng());
    for _ in UdpBoundSocketMap::<I, D>::EPHEMERAL_RANGE {
        // We can unwrap here because we know that the EPHEMERAL_RANGE doesn't
        // include 0.
        let tryport = NonZeroU16::new(port.get()).unwrap();
        match is_available(tryport) {
            Ok(()) => return Some(tryport),
            Err(InUseError {}) => port.next(),
        }
    }
    None
}

impl<I: IpExt, D: WeakId> PortAllocImpl for UdpBoundSocketMap<I, D> {
    const EPHEMERAL_RANGE: RangeInclusive<u16> = 49152..=65535;
    type Id = ProtocolFlowId<SocketIpAddr<I::Addr>, UdpRemotePort>;

    fn is_port_available(&self, id: &Self::Id, port: u16) -> bool {
        // We can safely unwrap here, because the ports received in
        // `is_port_available` are guaranteed to be in `EPHEMERAL_RANGE`.
        let port = NonZeroU16::new(port).unwrap();
        let conn = ConnAddr::from_protocol_flow_and_local_port(id, port);

        // A port is free if there are no sockets currently using it, and if
        // there are no sockets that are shadowing it.
        AddrVec::from(conn).iter_shadows().all(|a| match &a {
            AddrVec::Listen(l) => self.listeners().get_by_addr(&l).is_none(),
            AddrVec::Conn(c) => self.conns().get_by_addr(&c).is_none(),
        } && self.get_shadower_counts(&a) == 0)
    }
}

/// Information associated with a UDP connection.
#[derive(Debug, GenericOverIp)]
#[cfg_attr(test, derive(PartialEq))]
#[generic_over_ip(A, IpAddress)]
pub struct ConnInfo<A: IpAddress, D> {
    /// The local address associated with a UDP connection.
    pub local_ip: SocketZonedIpAddr<A, D>,
    /// The local port associated with a UDP connection.
    pub local_port: NonZeroU16,
    /// The remote address associated with a UDP connection.
    pub remote_ip: SocketZonedIpAddr<A, D>,
    /// The remote port associated with a UDP connection.
    pub remote_port: UdpRemotePort,
}

impl<A: IpAddress, D: Clone + Debug>
    From<ConnAddr<<A::Version as DualStackIpExt>::DualStackConnIpAddr<Udp>, D>> for ConnInfo<A, D>
where
    A::Version: DualStackIpExt,
{
    // TODO(https://issues.fuchsia.dev/): Use sealed traits so we can remove
    // this lint.
    #[allow(private_interfaces)]
    fn from(c: ConnAddr<<A::Version as DualStackIpExt>::DualStackConnIpAddr<Udp>, D>) -> Self {
        fn to_ipv6_mapped(a: SpecifiedAddr<Ipv4Addr>) -> SpecifiedAddr<Ipv6Addr> {
            a.get().to_ipv6_mapped()
        }

        let ConnAddr { ip, device } = c;
        #[derive(GenericOverIp)]
        #[generic_over_ip(I, Ip)]
        struct Wrapper<I: DualStackIpExt>(I::DualStackConnIpAddr<Udp>);
        let (local_ip, IpInvariant(local_port), remote_ip, IpInvariant(remote_port)) =
            A::Version::map_ip(
                Wrapper(ip),
                |Wrapper(ConnIpAddr {
                     local: (local_ip, local_id),
                     remote: (remote_ip, remote_id),
                 })| {
                    (
                        local_ip.into(),
                        IpInvariant(local_id),
                        remote_ip.into(),
                        IpInvariant(remote_id),
                    )
                },
                |Wrapper(dual_stack_conn_ip_addr)| match dual_stack_conn_ip_addr {
                    DualStackConnIpAddr::ThisStack(ConnIpAddr {
                        local: (local_ip, local_id),
                        remote: (remote_ip, remote_id),
                    }) => (
                        local_ip.into(),
                        IpInvariant(local_id),
                        remote_ip.into(),
                        IpInvariant(remote_id),
                    ),
                    DualStackConnIpAddr::OtherStack(ConnIpAddr {
                        local: (local_ip, local_id),
                        remote: (remote_ip, remote_id),
                    }) => (
                        to_ipv6_mapped(local_ip.into()),
                        IpInvariant(local_id),
                        to_ipv6_mapped(remote_ip.into()),
                        IpInvariant(remote_id),
                    ),
                },
            );

        Self {
            local_ip: SocketZonedIpAddr::new_with_zone(local_ip, || {
                device.clone().expect("device must be bound for addresses that require zones")
            }),
            local_port,
            remote_ip: SocketZonedIpAddr::new_with_zone(remote_ip, || {
                device.expect("device must be bound for addresses that require zones")
            }),
            remote_port,
        }
    }
}

/// Information associated with a UDP listener
#[derive(GenericOverIp)]
#[cfg_attr(test, derive(Debug, Eq, PartialEq))]
#[generic_over_ip(A, IpAddress)]
pub struct ListenerInfo<A: IpAddress, D> {
    /// The local address associated with a UDP listener, or `None` for any
    /// address.
    pub local_ip: Option<SocketZonedIpAddr<A, D>>,
    /// The local port associated with a UDP listener.
    pub local_port: NonZeroU16,
}

impl<A: IpAddress, D>
    From<ListenerAddr<<A::Version as DualStackIpExt>::DualStackListenerIpAddr<NonZeroU16>, D>>
    for ListenerInfo<A, D>
where
    A::Version: DualStackIpExt,
{
    fn from(
        ListenerAddr { ip, device }: ListenerAddr<
            <A::Version as DualStackIpExt>::DualStackListenerIpAddr<NonZeroU16>,
            D,
        >,
    ) -> Self {
        #[derive(GenericOverIp)]
        #[generic_over_ip(I, Ip)]
        struct Wrapper<I: DualStackIpExt>(I::DualStackListenerIpAddr<NonZeroU16>);
        let (addr, IpInvariant(identifier)): (Option<SpecifiedAddr<A>>, _) = A::Version::map_ip(
            Wrapper(ip),
            |Wrapper(ListenerIpAddr { addr, identifier })| {
                (addr.map(SocketIpAddr::into), IpInvariant(identifier))
            },
            |Wrapper(addr)| match addr {
                DualStackListenerIpAddr::ThisStack(ListenerIpAddr { addr, identifier }) => {
                    (addr.map(SocketIpAddr::into), IpInvariant(identifier))
                }
                DualStackListenerIpAddr::OtherStack(ListenerIpAddr { addr, identifier }) => (
                    SpecifiedAddr::new(
                        addr.map_or(Ipv4::UNSPECIFIED_ADDRESS, SocketIpAddr::addr)
                            .to_ipv6_mapped()
                            .get(),
                    ),
                    IpInvariant(identifier),
                ),
                DualStackListenerIpAddr::BothStacks(identifier) => (None, IpInvariant(identifier)),
            },
        );

        let local_ip = addr.map(|addr| {
            SocketZonedIpAddr::new_with_zone(addr, || {
                device.expect("device must be bound for addresses that require zones")
            })
        });
        Self { local_ip, local_port: identifier }
    }
}

impl<A: IpAddress, D> From<NonZeroU16> for ListenerInfo<A, D> {
    fn from(local_port: NonZeroU16) -> Self {
        Self { local_ip: None, local_port }
    }
}

/// A unique identifier for a UDP socket.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash, GenericOverIp)]
#[generic_over_ip(I, Ip)]

pub struct SocketId<I: Ip>(usize, IpVersionMarker<I>);

impl<I: Ip> From<usize> for SocketId<I> {
    fn from(value: usize) -> Self {
        Self(value, IpVersionMarker::default())
    }
}

impl<I: Ip> EntryKey for SocketId<I> {
    fn get_key_index(&self) -> usize {
        let Self(index, _marker) = self;
        *index
    }
}

/// An execution context for the UDP protocol.
pub trait UdpBindingsContext<I: IcmpIpExt, D> {
    /// Receives a UDP packet on a socket.
    fn receive_udp<B: BufferMut>(
        &mut self,
        id: SocketId<I>,
        device_id: &D,
        dst_addr: (I::Addr, NonZeroU16),
        src_addr: (I::Addr, Option<NonZeroU16>),
        body: &B,
    );
}

/// The bindings context for UDP.
pub(crate) trait UdpStateBindingsContext<I: IpExt, D>:
    InstantContext + RngContext + TracingContext + UdpBindingsContext<I, D>
{
}
impl<I: IpExt, BC: InstantContext + RngContext + TracingContext + UdpBindingsContext<I, D>, D>
    UdpStateBindingsContext<I, D> for BC
{
}

/// An execution context for the UDP protocol which also provides access to state.
pub(crate) trait BoundStateContext<I: IpExt, BC: UdpStateBindingsContext<I, Self::DeviceId>>:
    DeviceIdContext<AnyDevice> + UdpStateContext
{
    /// The core context passed to the callback provided to methods.
    type IpSocketsCtx<'a>: TransportIpContext<I, BC>
        + MulticastMembershipHandler<I, BC>
        + DeviceIdContext<AnyDevice, DeviceId = Self::DeviceId, WeakDeviceId = Self::WeakDeviceId>;

    type DualStackContext: DualStackDatagramBoundStateContext<
        I,
        BC,
        Udp,
        DeviceId = Self::DeviceId,
        WeakDeviceId = Self::WeakDeviceId,
    >;
    type NonDualStackContext: NonDualStackDatagramBoundStateContext<
        I,
        BC,
        Udp,
        DeviceId = Self::DeviceId,
        WeakDeviceId = Self::WeakDeviceId,
    >;

    /// Calls the function with an immutable reference to UDP sockets.
    fn with_bound_sockets<
        O,
        F: FnOnce(&mut Self::IpSocketsCtx<'_>, &BoundSockets<I, Self::WeakDeviceId>) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O;

    /// Calls the function with a mutable reference to UDP sockets.
    fn with_bound_sockets_mut<
        O,
        F: FnOnce(&mut Self::IpSocketsCtx<'_>, &mut BoundSockets<I, Self::WeakDeviceId>) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O;

    /// Returns a context for dual- or non-dual-stack operation.
    fn dual_stack_context(
        &mut self,
    ) -> MaybeDualStack<&mut Self::DualStackContext, &mut Self::NonDualStackContext>;

    /// Calls the function without access to the UDP bound socket state.
    fn with_transport_context<O, F: FnOnce(&mut Self::IpSocketsCtx<'_>) -> O>(
        &mut self,
        cb: F,
    ) -> O;
}

pub(crate) trait StateContext<I: IpExt, BC: UdpStateBindingsContext<I, Self::DeviceId>>:
    DeviceIdContext<AnyDevice>
{
    /// The core context passed to the callback.
    type SocketStateCtx<'a>: BoundStateContext<I, BC>
        + DeviceIdContext<AnyDevice, DeviceId = Self::DeviceId, WeakDeviceId = Self::WeakDeviceId>
        + UdpStateContext;

    /// Calls the function with an immutable reference to a socket's state.
    fn with_sockets_state<
        O,
        F: FnOnce(&mut Self::SocketStateCtx<'_>, &SocketsState<I, Self::WeakDeviceId>) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O;

    /// Calls the function with a mutable reference to UDP sockets.
    fn with_sockets_state_mut<
        O,
        F: FnOnce(&mut Self::SocketStateCtx<'_>, &mut SocketsState<I, Self::WeakDeviceId>) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O;

    /// Calls the function without access to UDP socket state.
    fn with_bound_state_context<O, F: FnOnce(&mut Self::SocketStateCtx<'_>) -> O>(
        &mut self,
        cb: F,
    ) -> O;

    /// Returns true if UDP may send a port unreachable ICMP error message.
    fn should_send_port_unreachable(&mut self) -> bool;
}

/// Empty trait to work around coherence issues.
///
/// This serves only to convince the coherence checker that a particular blanket
/// trait implementation could only possibly conflict with other blanket impls
/// in this crate. It can be safely implemented for any type.
/// TODO(https://github.com/rust-lang/rust/issues/97811): Remove this once the
/// coherence checker doesn't require it.
pub(crate) trait UdpStateContext {}

/// An execution context for UDP dual-stack operations.
pub(crate) trait DualStackBoundStateContext<
    I: IpExt,
    BC: UdpStateBindingsContext<I, Self::DeviceId>,
>: DeviceIdContext<AnyDevice>
{
    /// The core context passed to the callbacks to methods.
    type IpSocketsCtx<'a>: TransportIpContext<I, BC>
        + DeviceIdContext<AnyDevice, DeviceId = Self::DeviceId, WeakDeviceId = Self::WeakDeviceId>
        // Allow creating IP sockets for the other IP version.
        + TransportIpContext<I::OtherVersion, BC>;

    /// Calls the provided callback with mutable access to both the
    /// demultiplexing maps.
    fn with_both_bound_sockets_mut<
        O,
        F: FnOnce(
            &mut Self::IpSocketsCtx<'_>,
            &mut BoundSockets<I, Self::WeakDeviceId>,
            &mut BoundSockets<I::OtherVersion, Self::WeakDeviceId>,
        ) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O;

    /// Calls the provided callback with mutable access to the demultiplexing
    /// map for the other IP version.
    fn with_other_bound_sockets_mut<
        O,
        F: FnOnce(
            &mut Self::IpSocketsCtx<'_>,
            &mut BoundSockets<I::OtherVersion, Self::WeakDeviceId>,
        ) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O;

    /// Calls the provided callback with access to the `IpSocketsCtx`.
    fn with_transport_context<O, F: FnOnce(&mut Self::IpSocketsCtx<'_>) -> O>(
        &mut self,
        cb: F,
    ) -> O;
}

/// An execution context for UDP non-dual-stack operations.
pub(crate) trait NonDualStackBoundStateContext<
    I: IpExt,
    BC: UdpStateBindingsContext<I, Self::DeviceId>,
>: DeviceIdContext<AnyDevice>
{
}

/// An implementation of [`IpTransportContext`] for UDP.
pub(crate) enum UdpIpTransportContext {}

fn receive_ip_packet<
    I: IpExt,
    B: BufferMut,
    BC: UdpStateBindingsContext<I, CC::DeviceId>
        + UdpStateBindingsContext<I::OtherVersion, CC::DeviceId>,
    CC: StateContext<I, BC> + StateContext<I::OtherVersion, BC> + CounterContext<UdpCounters<I>>,
>(
    core_ctx: &mut CC,
    bindings_ctx: &mut BC,
    device: &CC::DeviceId,
    src_ip: I::RecvSrcAddr,
    dst_ip: SpecifiedAddr<I::Addr>,
    mut buffer: B,
) -> Result<(), (B, TransportReceiveError)> {
    trace_duration!(bindings_ctx, "udp::receive_ip_packet");
    core_ctx.with_counters(|counters| {
        counters.rx.increment();
    });
    trace!("received UDP packet: {:x?}", buffer.as_mut());
    let src_ip: I::Addr = src_ip.into();

    let packet = if let Ok(packet) =
        buffer.parse_with::<_, UdpPacket<_>>(UdpParseArgs::new(src_ip, dst_ip.get()))
    {
        packet
    } else {
        // There isn't much we can do if the UDP packet is
        // malformed.
        core_ctx.with_counters(|counters| {
            counters.rx_malformed.increment();
        });
        return Ok(());
    };

    let src_ip = if let Some(src_ip) = SpecifiedAddr::new(src_ip) {
        match src_ip.try_into() {
            Ok(addr) => Some(addr),
            Err(AddrIsMappedError {}) => {
                core_ctx.with_counters(|counters| {
                    counters.rx_mapped_addr.increment();
                });
                trace!("udp::receive_ip_packet: mapped source address");
                return Ok(());
            }
        }
    } else {
        None
    };
    let dst_ip = match dst_ip.try_into() {
        Ok(addr) => addr,
        Err(AddrIsMappedError {}) => {
            core_ctx.with_counters(|counters| {
                counters.rx_mapped_addr.increment();
            });
            trace!("udp::receive_ip_packet: mapped destination address");
            return Ok(());
        }
    };

    let src_port = packet.src_port();
    // Unfortunately, type inference isn't smart enough for us to just do
    // packet.parse_metadata().
    let meta = ParsablePacket::<_, UdpParseArgs<I::Addr>>::parse_metadata(&packet);

    /// The maximum number of socket IDs that are expected to receive a given
    /// packet. While it's possible for this number to be exceeded, it's
    /// unlikely.
    const MAX_EXPECTED_IDS: usize = 16;

    /// Collection of sockets that will receive a packet.
    ///
    /// Making this a [`smallvec::SmallVec`] lets us keep all the retrieved ids
    /// on the stack most of the time. If there are more than
    /// [`MAX_EXPECTED_IDS`], this will spill and allocate on the heap.
    type Recipients<Id> = smallvec::SmallVec<[Id; MAX_EXPECTED_IDS]>;

    let dst_port = packet.dst_port();
    let recipients = StateContext::<I, _>::with_bound_state_context(core_ctx, |core_ctx| {
        let device_weak = core_ctx.downgrade_device_id(device);
        DatagramBoundStateContext::with_bound_sockets(core_ctx, |_core_ctx, bound_sockets| {
            lookup(bound_sockets, (src_ip, src_port), (dst_ip, dst_port), device_weak)
                .map(|result| match result {
                    // TODO(https://fxbug.dev/125489): Make these socket IDs
                    // strongly owned instead of just cloning them to prevent
                    // deletion before delivery is done.
                    LookupResult::Conn(id, _) | LookupResult::Listener(id, _) => id.clone(),
                })
                // Collect into an array on the stack that
                .collect::<Recipients<_>>()
        })
    });

    let was_delivered = recipients.into_iter().fold(false, |was_delivered, lookup_result| {
        let delivered = try_dual_stack_deliver::<I, B, BC, CC>(
            core_ctx,
            bindings_ctx,
            lookup_result,
            device,
            (dst_ip.addr(), dst_port),
            (src_ip.map_or(I::UNSPECIFIED_ADDRESS, SocketIpAddr::addr), src_port),
            &buffer,
        );
        was_delivered | delivered
    });

    if !was_delivered && StateContext::<I, _>::should_send_port_unreachable(core_ctx) {
        buffer.undo_parse(meta);
        core_ctx.with_counters(|counters| {
            counters.rx_unknown_dest_port.increment();
        });
        Err((buffer, TransportReceiveError::new_port_unreachable()))
    } else {
        Ok(())
    }
}

/// Tries to deliver the given UDP packet to the given UDP socket.
fn try_deliver<
    I: IpExt,
    CC: StateContext<I, BC>,
    BC: UdpStateBindingsContext<I, CC::DeviceId>,
    B: BufferMut,
>(
    core_ctx: &mut CC,
    bindings_ctx: &mut BC,
    id: SocketId<I>,
    device: &CC::DeviceId,
    (dst_ip, dst_port): (I::Addr, NonZeroU16),
    (src_ip, src_port): (I::Addr, Option<NonZeroU16>),
    buffer: &B,
) -> bool {
    core_ctx.with_sockets_state(|core_ctx, state| {
        let should_deliver = match state.get_socket_state(&id).expect("socket ID is valid") {
            DatagramSocketState::Bound(DatagramBoundSocketState {
                socket_type,
                original_bound_addr: _,
            }) => match socket_type {
                DatagramBoundSocketStateType::Connected { state, sharing: _ } => {
                    match transport::udp::BoundStateContext::dual_stack_context(core_ctx) {
                        MaybeDualStack::DualStack(dual_stack) => {
                            match dual_stack.converter().convert(state) {
                                DualStackConnState::ThisStack(state) => state.should_receive(),
                                DualStackConnState::OtherStack(state) => state.should_receive(),
                            }
                        }
                        MaybeDualStack::NotDualStack(not_dual_stack) => {
                            not_dual_stack.converter().convert(state).should_receive()
                        }
                    }
                }
                DatagramBoundSocketStateType::Listener { state: _, sharing: _ } => true,
            },
            DatagramSocketState::Unbound(_) => true,
        };
        if should_deliver {
            bindings_ctx.receive_udp(id, device, (dst_ip, dst_port), (src_ip, src_port), buffer);
        }
        should_deliver
    })
}

/// A wrapper for [`try_deliver`] that supports dual stack delivery.
fn try_dual_stack_deliver<
    I: IpExt,
    B: BufferMut,
    BC: UdpStateBindingsContext<I, CC::DeviceId>
        + UdpStateBindingsContext<I::OtherVersion, CC::DeviceId>,
    CC: StateContext<I, BC> + StateContext<I::OtherVersion, BC>,
>(
    core_ctx: &mut CC,
    bindings_ctx: &mut BC,
    socket: I::DualStackBoundSocketId<Udp>,
    device: &CC::DeviceId,
    (dst_ip, dst_port): (I::Addr, NonZeroU16),
    (src_ip, src_port): (I::Addr, Option<NonZeroU16>),
    buffer: &B,
) -> bool {
    #[derive(GenericOverIp)]
    #[generic_over_ip(I, Ip)]
    struct Inputs<I: IpExt> {
        src_ip: I::Addr,
        dst_ip: I::Addr,
        socket: I::DualStackBoundSocketId<Udp>,
    }

    struct Outputs<I: IpExt> {
        src_ip: I::Addr,
        dst_ip: I::Addr,
        socket: SocketId<I>,
    }

    #[derive(GenericOverIp)]
    #[generic_over_ip(I, Ip)]
    enum DualStackOutputs<I: DualStackIpExt> {
        CurrentStack(Outputs<I>),
        OtherStack(Outputs<I::OtherVersion>),
    }

    let dual_stack_outputs = I::map_ip(
        Inputs { src_ip, dst_ip, socket },
        |Inputs { src_ip, dst_ip, socket }| match socket {
            EitherIpSocket::V4(socket) => {
                DualStackOutputs::CurrentStack(Outputs { src_ip, dst_ip, socket })
            }
            EitherIpSocket::V6(socket) => DualStackOutputs::OtherStack(Outputs {
                src_ip: src_ip.to_ipv6_mapped().get(),
                dst_ip: dst_ip.to_ipv6_mapped().get(),
                socket,
            }),
        },
        |Inputs { src_ip, dst_ip, socket }| {
            DualStackOutputs::CurrentStack(Outputs { src_ip, dst_ip, socket })
        },
    );

    match dual_stack_outputs {
        DualStackOutputs::CurrentStack(Outputs { src_ip, dst_ip, socket }) => try_deliver(
            core_ctx,
            bindings_ctx,
            socket,
            device,
            (dst_ip, dst_port),
            (src_ip, src_port),
            buffer,
        ),
        DualStackOutputs::OtherStack(Outputs { src_ip, dst_ip, socket }) => try_deliver(
            core_ctx,
            bindings_ctx,
            socket,
            device,
            (dst_ip, dst_port),
            (src_ip, src_port),
            buffer,
        ),
    }
}

impl<
        I: IpExt,
        BC: UdpStateBindingsContext<I, CC::DeviceId>
            + UdpStateBindingsContext<I::OtherVersion, CC::DeviceId>,
        CC: StateContext<I, BC>
            + StateContext<I::OtherVersion, BC>
            + NonTestCtxMarker
            + CounterContext<UdpCounters<I>>,
    > IpTransportContext<I, BC, CC> for UdpIpTransportContext
{
    fn receive_icmp_error(
        core_ctx: &mut CC,
        _bindings_ctx: &mut BC,
        _device: &CC::DeviceId,
        original_src_ip: Option<SpecifiedAddr<I::Addr>>,
        original_dst_ip: SpecifiedAddr<I::Addr>,
        _original_udp_packet: &[u8],
        err: I::ErrorCode,
    ) {
        core_ctx.with_counters(|counters| {
            counters.rx_icmp_error.increment();
        });
        // NB: At the moment bindings has no need to consume ICMP errors, so we
        // swallow them here.
        debug!(
            "UDP received ICMP error {:?} from {:?} to {:?}",
            err, original_dst_ip, original_src_ip
        );
    }

    fn receive_ip_packet<B: BufferMut>(
        core_ctx: &mut CC,
        bindings_ctx: &mut BC,
        device: &CC::DeviceId,
        src_ip: I::RecvSrcAddr,
        dst_ip: SpecifiedAddr<I::Addr>,
        buffer: B,
    ) -> Result<(), (B, TransportReceiveError)> {
        receive_ip_packet::<I, _, _, _>(core_ctx, bindings_ctx, device, src_ip, dst_ip, buffer)
    }
}

/// An error encountered while sending a UDP packet to an alternate address.
#[derive(Error, Copy, Clone, Debug, Eq, PartialEq)]
pub enum SendToError {
    /// The socket is not writeable.
    #[error("not writeable")]
    NotWriteable,
    /// An error was encountered while trying to create a temporary IP socket
    /// to use for the send operation.
    #[error("could not create a temporary connection socket: {}", _0)]
    CreateSock(IpSockCreationError),
    /// An MTU was exceeded.
    #[error("the maximum transmission unit (MTU) was exceeded")]
    Mtu,
    /// There was a problem with the remote address relating to its zone.
    #[error("zone error: {}", _0)]
    Zone(ZonedAddressError),
    /// Disallow sending packets with a remote port of 0. See
    /// [`UdpRemotePort::Unset`] for the rationale.
    #[error("the remote port was unset")]
    RemotePortUnset,
    /// The remote address is mapped (i.e. an ipv4-mapped-ipv6 address), but the
    /// socket is not dual-stack enabled.
    #[error("the remote ip was unexpectedly an ipv4-mapped-ipv6 address")]
    RemoteUnexpectedlyMapped,
    /// The remote address is non-mapped (i.e not an ipv4-mapped-ipv6 address),
    /// but the socket is dual stack enabled and bound to a mapped address.
    #[error("the remote ip was unexpectedly not an ipv4-mapped-ipv6 address")]
    RemoteUnexpectedlyNonMapped,
}

pub(crate) trait SocketHandler<I: IpExt, BC>: DeviceIdContext<AnyDevice> {
    fn create_udp(&mut self) -> SocketId<I>;

    fn connect(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        remote_ip: Option<SocketZonedIpAddr<I::Addr, Self::DeviceId>>,
        remote_port: UdpRemotePort,
    ) -> Result<(), ConnectError>;

    fn set_device(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        device_id: Option<&Self::DeviceId>,
    ) -> Result<(), SocketError>;

    fn get_udp_bound_device(
        &mut self,
        bindings_ctx: &BC,
        id: SocketId<I>,
    ) -> Option<Self::WeakDeviceId>;

    fn set_dual_stack_enabled(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        enabled: bool,
    ) -> Result<(), SetDualStackEnabledError>;

    fn get_dual_stack_enabled(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
    ) -> Result<bool, NotDualStackCapableError>;

    fn set_udp_posix_reuse_port(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        reuse_port: bool,
    ) -> Result<(), ExpectedUnboundError>;

    fn get_udp_posix_reuse_port(&mut self, bindings_ctx: &BC, id: SocketId<I>) -> bool;

    fn set_udp_multicast_membership(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        multicast_group: MulticastAddr<I::Addr>,
        interface: MulticastMembershipInterfaceSelector<I::Addr, Self::DeviceId>,
        want_membership: bool,
    ) -> Result<(), SetMulticastMembershipError>;

    fn set_udp_unicast_hop_limit(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        unicast_hop_limit: Option<NonZeroU8>,
    );

    fn set_udp_multicast_hop_limit(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        multicast_hop_limit: Option<NonZeroU8>,
    );

    fn get_udp_unicast_hop_limit(&mut self, bindings_ctx: &BC, id: SocketId<I>) -> NonZeroU8;

    fn get_udp_multicast_hop_limit(&mut self, bindings_ctx: &BC, id: SocketId<I>) -> NonZeroU8;

    fn get_udp_transparent(&mut self, id: SocketId<I>) -> bool;

    fn set_udp_transparent(&mut self, id: SocketId<I>, value: bool);

    fn disconnect_connected(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
    ) -> Result<(), ExpectedConnError>;

    fn shutdown(
        &mut self,
        bindings_ctx: &BC,
        id: SocketId<I>,
        which: ShutdownType,
    ) -> Result<(), ExpectedConnError>;

    fn get_shutdown(&mut self, bindings_ctx: &BC, id: SocketId<I>) -> Option<ShutdownType>;

    fn close(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
    ) -> SocketInfo<I::Addr, Self::WeakDeviceId>;

    fn get_udp_info(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
    ) -> SocketInfo<I::Addr, Self::WeakDeviceId>;

    fn listen_udp(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        addr: Option<SocketZonedIpAddr<I::Addr, Self::DeviceId>>,
        port: Option<NonZeroU16>,
    ) -> Result<(), Either<ExpectedUnboundError, LocalAddressError>>;

    fn send<B: BufferMut>(
        &mut self,
        bindings_ctx: &mut BC,
        conn: SocketId<I>,
        body: B,
    ) -> Result<(), Either<SendError, ExpectedConnError>>;

    fn send_to<B: BufferMut>(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        remote_ip: Option<SocketZonedIpAddr<I::Addr, Self::DeviceId>>,
        remote_port: UdpRemotePort,
        body: B,
    ) -> Result<(), Either<LocalAddressError, SendToError>>;
}

impl<
        I: IpExt,
        BC: UdpStateBindingsContext<I, Self::DeviceId>,
        CC: StateContext<I, BC> + CounterContext<UdpCounters<I>>,
    > SocketHandler<I, BC> for CC
{
    fn create_udp(&mut self) -> SocketId<I> {
        datagram::create(self)
    }

    fn connect(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        remote_ip: Option<SocketZonedIpAddr<<I>::Addr, Self::DeviceId>>,
        remote_port: UdpRemotePort,
    ) -> Result<(), ConnectError> {
        datagram::connect(self, bindings_ctx, id, remote_ip, remote_port, ())
    }

    fn set_device(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        device_id: Option<&Self::DeviceId>,
    ) -> Result<(), SocketError> {
        datagram::set_device(self, bindings_ctx, id, device_id)
    }

    fn get_udp_bound_device(
        &mut self,
        bindings_ctx: &BC,
        id: SocketId<I>,
    ) -> Option<Self::WeakDeviceId> {
        datagram::get_bound_device(self, bindings_ctx, id)
    }

    fn set_dual_stack_enabled(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        enabled: bool,
    ) -> Result<(), SetDualStackEnabledError> {
        datagram::with_other_stack_ip_options_mut_if_unbound(
            self,
            bindings_ctx,
            id,
            |other_stack| {
                #[derive(GenericOverIp)]
                #[generic_over_ip(I, Ip)]
                struct WrapOtherStack<'a, I: IpExt>(
                    &'a mut I::OtherStackIpOptions<DualStackSocketState>,
                );
                I::map_ip(
                    (enabled, WrapOtherStack(other_stack)),
                    |(_enabled, _v4)| Err(SetDualStackEnabledError::NotCapable),
                    |(enabled, WrapOtherStack(other_stack))| {
                        let DualStackSocketState { dual_stack_enabled } = other_stack;
                        *dual_stack_enabled = enabled;
                        Ok(())
                    },
                )
            },
        )
        .map_err(|ExpectedUnboundError| {
            // NB: Match Linux and prefer to return `NotCapable` errors over
            // `SocketIsBound` errors, for IPv4 sockets.
            match I::VERSION {
                IpVersion::V4 => SetDualStackEnabledError::NotCapable,
                IpVersion::V6 => SetDualStackEnabledError::SocketIsBound,
            }
        })?
    }

    fn get_dual_stack_enabled(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
    ) -> Result<bool, NotDualStackCapableError> {
        datagram::with_other_stack_ip_options(self, bindings_ctx, id, |other_stack| {
            #[derive(GenericOverIp)]
            #[generic_over_ip(I, Ip)]
            struct WrapOtherStack<'a, I: IpExt>(&'a I::OtherStackIpOptions<DualStackSocketState>);
            I::map_ip(
                WrapOtherStack(other_stack),
                |_v4| Err(NotDualStackCapableError),
                |WrapOtherStack(other_stack)| {
                    let DualStackSocketState { dual_stack_enabled } = other_stack;
                    Ok(*dual_stack_enabled)
                },
            )
        })
    }

    fn set_udp_posix_reuse_port(
        &mut self,
        _bindings_ctx: &mut BC,
        id: SocketId<I>,
        reuse_port: bool,
    ) -> Result<(), ExpectedUnboundError> {
        datagram::update_sharing(
            self,
            id,
            if reuse_port { Sharing::ReusePort } else { Sharing::Exclusive },
        )
    }

    fn get_udp_posix_reuse_port(&mut self, _bindings_ctx: &BC, id: SocketId<I>) -> bool {
        datagram::get_sharing(self, id).is_reuse_port()
    }

    fn set_udp_multicast_membership(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        multicast_group: MulticastAddr<I::Addr>,
        interface: MulticastMembershipInterfaceSelector<I::Addr, Self::DeviceId>,
        want_membership: bool,
    ) -> Result<(), SetMulticastMembershipError> {
        datagram::set_multicast_membership(
            self,
            bindings_ctx,
            id,
            multicast_group,
            interface,
            want_membership,
        )
        .map_err(Into::into)
    }

    fn set_udp_unicast_hop_limit(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        unicast_hop_limit: Option<NonZeroU8>,
    ) {
        crate::socket::datagram::update_ip_hop_limit(
            self,
            bindings_ctx,
            id,
            SocketHopLimits::set_unicast(unicast_hop_limit),
        )
    }

    fn set_udp_multicast_hop_limit(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        multicast_hop_limit: Option<NonZeroU8>,
    ) {
        crate::socket::datagram::update_ip_hop_limit(
            self,
            bindings_ctx,
            id,
            SocketHopLimits::set_multicast(multicast_hop_limit),
        )
    }

    fn get_udp_unicast_hop_limit(&mut self, bindings_ctx: &BC, id: SocketId<I>) -> NonZeroU8 {
        crate::socket::datagram::get_ip_hop_limits(self, bindings_ctx, id).unicast
    }

    fn get_udp_multicast_hop_limit(&mut self, bindings_ctx: &BC, id: SocketId<I>) -> NonZeroU8 {
        crate::socket::datagram::get_ip_hop_limits(self, bindings_ctx, id).multicast
    }

    fn get_udp_transparent(&mut self, id: SocketId<I>) -> bool {
        crate::socket::datagram::get_ip_transparent(self, id)
    }

    fn set_udp_transparent(&mut self, id: SocketId<I>, value: bool) {
        crate::socket::datagram::set_ip_transparent(self, id, value)
    }

    fn disconnect_connected(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
    ) -> Result<(), ExpectedConnError> {
        datagram::disconnect_connected(self, bindings_ctx, id)
    }

    fn shutdown(
        &mut self,
        bindings_ctx: &BC,
        id: SocketId<I>,
        which: ShutdownType,
    ) -> Result<(), ExpectedConnError> {
        datagram::shutdown_connected(self, bindings_ctx, id, which)
    }

    fn get_shutdown(&mut self, bindings_ctx: &BC, id: SocketId<I>) -> Option<ShutdownType> {
        datagram::get_shutdown_connected(self, bindings_ctx, id)
    }

    fn close(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
    ) -> SocketInfo<I::Addr, Self::WeakDeviceId> {
        datagram::close(self, bindings_ctx, id).into()
    }

    fn get_udp_info(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
    ) -> SocketInfo<I::Addr, Self::WeakDeviceId> {
        datagram::get_info(self, bindings_ctx, id).into()
    }

    fn listen_udp(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        addr: Option<SocketZonedIpAddr<I::Addr, Self::DeviceId>>,
        port: Option<NonZeroU16>,
    ) -> Result<(), Either<ExpectedUnboundError, LocalAddressError>> {
        datagram::listen(self, bindings_ctx, id, addr, port)
    }

    fn send<B: BufferMut>(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        body: B,
    ) -> Result<(), Either<SendError, ExpectedConnError>> {
        self.with_counters(|counters| {
            counters.tx.increment();
        });
        datagram::send_conn(self, bindings_ctx, id, body).map_err(|err| {
            self.with_counters(|counters| {
                counters.tx_error.increment();
            });
            match err {
                DatagramSendError::NotConnected => Either::Right(ExpectedConnError),
                DatagramSendError::NotWriteable => Either::Left(SendError::NotWriteable),
                DatagramSendError::IpSock(err) => Either::Left(SendError::IpSock(err)),
                DatagramSendError::SerializeError(err) => match err {
                    UdpSerializeError::RemotePortUnset => Either::Left(SendError::RemotePortUnset),
                },
            }
        })
    }

    fn send_to<B: BufferMut>(
        &mut self,
        bindings_ctx: &mut BC,
        id: SocketId<I>,
        remote_ip: Option<SocketZonedIpAddr<I::Addr, Self::DeviceId>>,
        remote_port: UdpRemotePort,
        body: B,
    ) -> Result<(), Either<LocalAddressError, SendToError>> {
        self.with_counters(|counters| {
            counters.tx.increment();
        });
        datagram::send_to(self, bindings_ctx, id, remote_ip, remote_port, body).map_err(|e| {
            self.with_counters(|counters| counters.tx_error.increment());
            match e {
                Either::Left(e) => Either::Left(e),
                Either::Right(e) => {
                    let err = match e {
                        datagram::SendToError::SerializeError(err) => match err {
                            UdpSerializeError::RemotePortUnset => SendToError::RemotePortUnset,
                        },
                        datagram::SendToError::NotWriteable => SendToError::NotWriteable,
                        datagram::SendToError::Zone(e) => SendToError::Zone(e),
                        datagram::SendToError::CreateAndSend(e) => match e {
                            IpSockCreateAndSendError::Mtu => SendToError::Mtu,
                            IpSockCreateAndSendError::Create(e) => SendToError::CreateSock(e),
                        },
                        datagram::SendToError::RemoteUnexpectedlyMapped => {
                            SendToError::RemoteUnexpectedlyMapped
                        }
                        datagram::SendToError::RemoteUnexpectedlyNonMapped => {
                            SendToError::RemoteUnexpectedlyNonMapped
                        }
                    };
                    Either::Right(err)
                }
            }
        })
    }
}

/// Error when sending a packet on a socket.
#[derive(Copy, Clone, Debug, Eq, PartialEq, GenericOverIp)]
#[generic_over_ip()]
pub enum SendError {
    /// The socket is not writeable.
    NotWriteable,
    /// The packet couldn't be sent.
    IpSock(IpSockSendError),
    /// Disallow sending packets with a remote port of 0. See
    /// [`UdpRemotePort::Unset`] for the rationale.
    RemotePortUnset,
}

/// Sends a UDP packet on an existing socket.
///
/// # Errors
///
/// Returns an error if the socket is not connected or the packet cannot be sent.
/// On error, the original `body` is returned unmodified so that it can be
/// reused by the caller.
///
/// # Panics
///
/// Panics if `id` is not a valid UDP socket identifier.
pub fn send_udp<I: Ip, B: BufferMut, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
    body: B,
) -> Result<(), Either<SendError, ExpectedConnError>> {
    let mut core_ctx = Locked::new(core_ctx);

    I::map_ip::<_, Result<_, _>>(
        (IpInvariant((&mut core_ctx, bindings_ctx, body)), id.clone()),
        |(IpInvariant((core_ctx, bindings_ctx, body)), id)| {
            SocketHandler::<Ipv4, _>::send(core_ctx, bindings_ctx, id, body).map_err(IpInvariant)
        },
        |(IpInvariant((core_ctx, bindings_ctx, body)), id)| {
            SocketHandler::<Ipv6, _>::send(core_ctx, bindings_ctx, id, body).map_err(IpInvariant)
        },
    )
    .map_err(|IpInvariant(e)| e)
}

/// Sends a UDP packet to the provided destination address.
///
/// If this is called with an unbound socket, the socket will be implicitly
/// bound. If that succeeds, the ID for the new socket is returned.
///
/// # Errors
///
/// Returns an error if the socket is unbound and connecting fails, or if the
/// packet could not be sent. If the socket is unbound and connecting succeeds
/// but sending fails, the socket remains connected.
///
/// # Panics
///
/// Panics if `id` is not a valid UDP socket identifier.
pub fn send_udp_to<I: Ip, B: BufferMut, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
    remote_ip: Option<SocketZonedIpAddr<I::Addr, DeviceId<BC>>>,
    remote_port: UdpRemotePort,
    body: B,
) -> Result<(), Either<LocalAddressError, SendToError>> {
    let mut core_ctx = Locked::new(core_ctx);
    let id = id.clone();

    // Match Linux's behavior and verify the remote port is set.
    match remote_port {
        UdpRemotePort::Unset => return Err(Either::Right(SendToError::RemotePortUnset)),
        UdpRemotePort::Set(_) => {}
    }

    I::map_ip::<_, Result<_, _>>(
        (IpInvariant((&mut core_ctx, bindings_ctx, remote_port, body)), id, remote_ip),
        |(IpInvariant((core_ctx, bindings_ctx, remote_port, body)), id, remote_ip)| {
            SocketHandler::<Ipv4, _>::send_to(
                core_ctx,
                bindings_ctx,
                id,
                remote_ip,
                remote_port,
                body,
            )
            .map_err(IpInvariant)
        },
        |(IpInvariant((core_ctx, bindings_ctx, remote_port, body)), id, remote_ip)| {
            SocketHandler::<Ipv6, _>::send_to(
                core_ctx,
                bindings_ctx,
                id,
                remote_ip,
                remote_port,
                body,
            )
            .map_err(IpInvariant)
        },
    )
    .map_err(|IpInvariant(e)| e)
}

impl<I: IpExt, BC: UdpStateBindingsContext<I, Self::DeviceId>, CC: StateContext<I, BC>>
    DatagramStateContext<I, BC, Udp> for CC
{
    type SocketsStateCtx<'a> = CC::SocketStateCtx<'a>;

    fn with_sockets_state<
        O,
        F: FnOnce(
            &mut Self::SocketsStateCtx<'_>,
            &DatagramSocketsState<I, Self::WeakDeviceId, Udp>,
        ) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O {
        self.with_sockets_state(|core_ctx, sockets_state| cb(core_ctx, sockets_state))
    }

    fn with_sockets_state_mut<
        O,
        F: FnOnce(
            &mut Self::SocketsStateCtx<'_>,
            &mut DatagramSocketsState<I, Self::WeakDeviceId, Udp>,
        ) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O {
        self.with_sockets_state_mut(|core_ctx, sockets_state| cb(core_ctx, sockets_state))
    }

    fn with_bound_state_context<O, F: FnOnce(&mut Self::SocketsStateCtx<'_>) -> O>(
        &mut self,
        cb: F,
    ) -> O {
        self.with_bound_state_context(cb)
    }
}

impl<
        I: IpExt,
        BC: UdpStateBindingsContext<I, Self::DeviceId>,
        CC: BoundStateContext<I, BC> + UdpStateContext,
    > DatagramBoundStateContext<I, BC, Udp> for CC
{
    type IpSocketsCtx<'a> = CC::IpSocketsCtx<'a>;
    // TODO(https://fxbug.dev/133884): Remove the laziness by dropping `Option`.
    type LocalIdAllocator = Option<PortAlloc<UdpBoundSocketMap<I, CC::WeakDeviceId>>>;

    fn with_bound_sockets<
        O,
        F: FnOnce(
            &mut Self::IpSocketsCtx<'_>,
            &DatagramBoundSockets<I, Self::WeakDeviceId, Udp, (Udp, I, CC::WeakDeviceId)>,
        ) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O {
        self.with_bound_sockets(|core_ctx, BoundSockets { bound_sockets, lazy_port_alloc: _ }| {
            cb(core_ctx, bound_sockets)
        })
    }

    fn with_bound_sockets_mut<
        O,
        F: FnOnce(
            &mut Self::IpSocketsCtx<'_>,
            &mut DatagramBoundSockets<I, Self::WeakDeviceId, Udp, (Udp, I, CC::WeakDeviceId)>,
            &mut Self::LocalIdAllocator,
        ) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O {
        self.with_bound_sockets_mut(|core_ctx, BoundSockets { bound_sockets, lazy_port_alloc }| {
            cb(core_ctx, bound_sockets, lazy_port_alloc)
        })
    }

    type DualStackContext = CC::DualStackContext;
    type NonDualStackContext = CC::NonDualStackContext;
    fn dual_stack_context(
        &mut self,
    ) -> MaybeDualStack<&mut Self::DualStackContext, &mut Self::NonDualStackContext> {
        BoundStateContext::dual_stack_context(self)
    }

    fn with_transport_context<O, F: FnOnce(&mut Self::IpSocketsCtx<'_>) -> O>(
        &mut self,
        cb: F,
    ) -> O {
        self.with_transport_context(cb)
    }
}

impl<
        BC: UdpStateBindingsContext<Ipv6, CC::DeviceId> + UdpStateBindingsContext<Ipv4, CC::DeviceId>,
        CC: DualStackBoundStateContext<Ipv6, BC> + UdpStateContext,
    > DualStackDatagramBoundStateContext<Ipv6, BC, Udp> for CC
{
    type IpSocketsCtx<'a> = CC::IpSocketsCtx<'a>;
    fn dual_stack_enabled(
        &self,
        state: &impl AsRef<IpOptions<Ipv6, Self::WeakDeviceId, Udp>>,
    ) -> bool {
        let DualStackSocketState { dual_stack_enabled } = state.as_ref().other_stack();
        *dual_stack_enabled
    }

    type Converter = ();
    fn converter(&self) -> Self::Converter {
        ()
    }

    fn from_other_ip_addr(&self, addr: Ipv4Addr) -> Ipv6Addr {
        *addr.to_ipv6_mapped()
    }

    fn to_other_bound_socket_id(&self, id: SocketId<Ipv6>) -> EitherIpSocket<Udp> {
        EitherIpSocket::V6(id)
    }

    type LocalIdAllocator = Option<PortAlloc<UdpBoundSocketMap<Ipv6, CC::WeakDeviceId>>>;
    type OtherLocalIdAllocator = Option<PortAlloc<UdpBoundSocketMap<Ipv4, CC::WeakDeviceId>>>;

    fn with_both_bound_sockets_mut<
        O,
        F: FnOnce(
            &mut Self::IpSocketsCtx<'_>,
            &mut UdpBoundSocketMap<Ipv6, Self::WeakDeviceId>,
            &mut UdpBoundSocketMap<Ipv4, Self::WeakDeviceId>,
            &mut Self::LocalIdAllocator,
            &mut Self::OtherLocalIdAllocator,
        ) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O {
        self.with_both_bound_sockets_mut(
            |core_ctx,
             BoundSockets { bound_sockets: bound_first, lazy_port_alloc: alloc_first },
             BoundSockets { bound_sockets: bound_second, lazy_port_alloc: alloc_second }
            | {
                cb(core_ctx, bound_first, bound_second, alloc_first, alloc_second)
            },
        )
    }

    fn with_other_bound_sockets_mut<
        O,
        F: FnOnce(&mut Self::IpSocketsCtx<'_>, &mut UdpBoundSocketMap<Ipv4, Self::WeakDeviceId>) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O {
        self.with_other_bound_sockets_mut(
            |core_ctx, BoundSockets { bound_sockets, lazy_port_alloc: _ }| {
                cb(core_ctx, bound_sockets)
            },
        )
    }

    fn with_transport_context<O, F: FnOnce(&mut Self::IpSocketsCtx<'_>) -> O>(
        &mut self,
        cb: F,
    ) -> O {
        self.with_transport_context(|core_ctx| cb(core_ctx))
    }
}

impl<
        BC: UdpStateBindingsContext<Ipv4, CC::DeviceId>,
        CC: BoundStateContext<Ipv4, BC> + NonDualStackBoundStateContext<Ipv4, BC> + UdpStateContext,
    > NonDualStackDatagramBoundStateContext<Ipv4, BC, Udp> for CC
{
    type Converter = ();
    fn converter(&self) -> Self::Converter {
        ()
    }
}

impl<I: IpExt, BC: UdpStateBindingsContext<I, D::Strong>, D: WeakId>
    LocalIdentifierAllocator<I, D, Udp, BC, (Udp, I, D)>
    for Option<PortAlloc<UdpBoundSocketMap<I, D>>>
{
    fn try_alloc_local_id(
        &mut self,
        bound: &UdpBoundSocketMap<I, D>,
        bindings_ctx: &mut BC,
        flow: datagram::DatagramFlowId<I::Addr, UdpRemotePort>,
    ) -> Option<NonZeroU16> {
        let DatagramFlowId { local_ip, remote_ip, remote_id } = flow;
        let id = ProtocolFlowId::new(local_ip, remote_ip, remote_id);
        let mut rng = bindings_ctx.rng();
        // Lazily init port_alloc if it hasn't been inited yet.
        let port_alloc = self.get_or_insert_with(|| PortAlloc::new(&mut rng));
        port_alloc.try_alloc(&id, bound).and_then(NonZeroU16::new)
    }
}

/// Creates an unbound UDP socket.
///
/// `create_udp` creates a new UDP socket and returns an identifier for it.
pub fn create_udp<I: Ip, BC: crate::BindingsContext>(core_ctx: &SyncCtx<BC>) -> SocketId<I> {
    let mut core_ctx = Locked::new(core_ctx);
    I::map_ip(
        IpInvariant(&mut core_ctx),
        |IpInvariant(core_ctx)| SocketHandler::<Ipv4, _>::create_udp(core_ctx),
        |IpInvariant(core_ctx)| SocketHandler::<Ipv6, _>::create_udp(core_ctx),
    )
}

/// Connect a UDP socket
///
/// `connect` binds `id` as a connection to the remote address and port.
/// It is also bound to a local address and port, meaning that packets sent on
/// this connection will always come from that address and port. The local
/// address will be chosen based on the route to the remote address, and the
/// local port will be chosen from the available ones.
///
/// # Errors
///
/// `connect` will fail in the following cases:
/// - If both `local_ip` and `local_port` are specified but conflict with an
///   existing connection or listener
/// - If one or both are left unspecified but there is still no way to satisfy
///   the request (e.g., `local_ip` is specified but there are no available
///   local ports for that address)
/// - If there is no route to `remote_ip`
/// - If `id` belongs to an already-connected socket
///
/// # Panics
///
/// `connect` panics if `id` is not a valid [`UnboundId`].
pub fn connect<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
    remote_ip: Option<SocketZonedIpAddr<I::Addr, DeviceId<BC>>>,
    remote_port: UdpRemotePort,
) -> Result<(), ConnectError> {
    let mut core_ctx = Locked::new(core_ctx);
    let id = id.clone();
    I::map_ip::<_, Result<_, _>>(
        (IpInvariant((&mut core_ctx, bindings_ctx, remote_port)), id, remote_ip),
        |(IpInvariant((core_ctx, bindings_ctx, remote_port)), id, remote_ip)| {
            SocketHandler::<Ipv4, _>::connect(core_ctx, bindings_ctx, id, remote_ip, remote_port)
                .map_err(IpInvariant)
        },
        |(IpInvariant((core_ctx, bindings_ctx, remote_port)), id, remote_ip)| {
            SocketHandler::<Ipv6, _>::connect(core_ctx, bindings_ctx, id, remote_ip, remote_port)
                .map_err(IpInvariant)
        },
    )
    .map_err(|IpInvariant(e)| e)
}

/// Sets the bound device for a socket.
///
/// Sets the device to be used for sending and receiving packets for a socket.
/// If the socket is not currently bound to a local address and port, the device
/// will be used when binding.
///
/// # Panics
///
/// Panics if `id` is not a valid [`SocketId`].
pub fn set_udp_device<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
    device_id: Option<&DeviceId<BC>>,
) -> Result<(), SocketError> {
    let mut core_ctx = Locked::new(core_ctx);
    I::map_ip::<_, Result<_, _>>(
        (IpInvariant((&mut core_ctx, bindings_ctx, device_id)), id.clone()),
        |(IpInvariant((core_ctx, bindings_ctx, device_id)), id)| {
            SocketHandler::<Ipv4, _>::set_device(core_ctx, bindings_ctx, id, device_id)
                .map_err(IpInvariant)
        },
        |(IpInvariant((core_ctx, bindings_ctx, device_id)), id)| {
            SocketHandler::<Ipv6, _>::set_device(core_ctx, bindings_ctx, id, device_id)
                .map_err(IpInvariant)
        },
    )
    .map_err(|IpInvariant(e)| e)
}

/// Gets the device the specified socket is bound to.
///
/// # Panics
///
/// Panics if `id` is not a valid socket ID.
pub fn get_udp_bound_device<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &BC,
    id: &SocketId<I>,
) -> Option<WeakDeviceId<BC>> {
    let mut core_ctx = Locked::new(core_ctx);
    let id = id.clone();
    let IpInvariant(device) = I::map_ip::<_, IpInvariant<Option<WeakDeviceId<BC>>>>(
        (IpInvariant((&mut core_ctx, bindings_ctx)), id),
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            IpInvariant(SocketHandler::<Ipv4, _>::get_udp_bound_device(core_ctx, bindings_ctx, id))
        },
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            IpInvariant(SocketHandler::<Ipv6, _>::get_udp_bound_device(core_ctx, bindings_ctx, id))
        },
    );
    device
}

/// Enable or disable dual stack operations on the given socket.
///
/// This is notionally the inverse of the `IPV6_V6ONLY` socket option.
///
/// # Errors
///
/// Returns an error if the socket does not support the `IPV6_V6ONLY` socket
/// option (e.g. an IPv4 socket).
///
/// # Panics
///
/// Panics if `id` is not a valid `SocketId`.
pub fn set_udp_dual_stack_enabled<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
    enabled: bool,
) -> Result<(), SetDualStackEnabledError> {
    let mut core_ctx = Locked::new(core_ctx);
    I::map_ip(
        (IpInvariant((&mut core_ctx, bindings_ctx, enabled)), id.clone()),
        |(IpInvariant((core_ctx, bindings_ctx, enabled)), id)| {
            SocketHandler::<Ipv4, _>::set_dual_stack_enabled(core_ctx, bindings_ctx, id, enabled)
        },
        |(IpInvariant((core_ctx, bindings_ctx, enabled)), id)| {
            SocketHandler::<Ipv6, _>::set_dual_stack_enabled(core_ctx, bindings_ctx, id, enabled)
        },
    )
}

/// Get the enabled state of dual stack operations on the given socket.
///
/// This is notionally the inverse of the `IPV6_V6ONLY` socket option.
///
/// # Errors
///
/// Returns an error if the socket does not support the `IPV6_V6ONLY` socket
/// option (e.g. an IPv4 socket).
///
/// # Panics
///
/// Panics if `id` is not a valid `SocketId`.
pub fn get_udp_dual_stack_enabled<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
) -> Result<bool, NotDualStackCapableError> {
    let mut core_ctx = Locked::new(core_ctx);
    let id = id.clone();
    let IpInvariant(enabled) = I::map_ip::<_, IpInvariant<Result<bool, NotDualStackCapableError>>>(
        (IpInvariant((&mut core_ctx, bindings_ctx)), id),
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            IpInvariant(SocketHandler::<Ipv4, _>::get_dual_stack_enabled(
                core_ctx,
                bindings_ctx,
                id,
            ))
        },
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            IpInvariant(SocketHandler::<Ipv6, _>::get_dual_stack_enabled(
                core_ctx,
                bindings_ctx,
                id,
            ))
        },
    );
    enabled
}

/// Sets the POSIX `SO_REUSEPORT` option for the specified socket.
///
/// # Errors
///
/// Returns an error if the socket is already bound.
///
/// # Panics
///
/// `set_udp_posix_reuse_port` panics if `id` is not a valid `SocketId`.
pub fn set_udp_posix_reuse_port<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
    reuse_port: bool,
) -> Result<(), ExpectedUnboundError> {
    let mut core_ctx = Locked::new(core_ctx);
    I::map_ip(
        (IpInvariant((&mut core_ctx, bindings_ctx, reuse_port)), id.clone()),
        |(IpInvariant((core_ctx, bindings_ctx, reuse_port)), id)| {
            SocketHandler::<Ipv4, _>::set_udp_posix_reuse_port(
                core_ctx,
                bindings_ctx,
                id,
                reuse_port,
            )
        },
        |(IpInvariant((core_ctx, bindings_ctx, reuse_port)), id)| {
            SocketHandler::<Ipv6, _>::set_udp_posix_reuse_port(
                core_ctx,
                bindings_ctx,
                id,
                reuse_port,
            )
        },
    )
}

/// Gets the POSIX `SO_REUSEPORT` option for the specified socket.
///
/// # Panics
///
/// Panics if `id` is not a valid `SocketId`.
pub fn get_udp_posix_reuse_port<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &BC,
    id: &SocketId<I>,
) -> bool {
    let mut core_ctx = Locked::new(core_ctx);
    let id = id.clone();
    let IpInvariant(reuse_port) = I::map_ip::<_, IpInvariant<bool>>(
        (IpInvariant((&mut core_ctx, bindings_ctx)), id),
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            IpInvariant(SocketHandler::<Ipv4, _>::get_udp_posix_reuse_port(
                core_ctx,
                bindings_ctx,
                id,
            ))
        },
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            IpInvariant(SocketHandler::<Ipv6, _>::get_udp_posix_reuse_port(
                core_ctx,
                bindings_ctx,
                id,
            ))
        },
    );
    reuse_port
}

/// Sets the specified socket's membership status for the given group.
///
/// If `id` is unbound, the membership state will take effect when it is bound.
/// An error is returned if the membership change request is invalid (e.g.
/// leaving a group that was not joined, or joining a group multiple times) or
/// if the device to use to join is unspecified or conflicts with the existing
/// socket state.
pub fn set_udp_multicast_membership<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
    multicast_group: MulticastAddr<I::Addr>,
    interface: MulticastMembershipInterfaceSelector<I::Addr, DeviceId<BC>>,
    want_membership: bool,
) -> Result<(), SetMulticastMembershipError> {
    let mut core_ctx = Locked::new(core_ctx);
    let id = id.clone();
    I::map_ip::<_, Result<_, _>>(
        (
            IpInvariant((&mut core_ctx, bindings_ctx, want_membership)),
            id,
            multicast_group,
            interface,
        ),
        |(
            IpInvariant((core_ctx, bindings_ctx, want_membership)),
            id,
            multicast_group,
            interface,
        )| {
            SocketHandler::<Ipv4, _>::set_udp_multicast_membership(
                core_ctx,
                bindings_ctx,
                id,
                multicast_group,
                interface,
                want_membership,
            )
            .map_err(IpInvariant)
        },
        |(
            IpInvariant((core_ctx, bindings_ctx, want_membership)),
            id,
            multicast_group,
            interface,
        )| {
            SocketHandler::<Ipv6, _>::set_udp_multicast_membership(
                core_ctx,
                bindings_ctx,
                id,
                multicast_group,
                interface,
                want_membership,
            )
            .map_err(IpInvariant)
        },
    )
    .map_err(|IpInvariant(e)| e)
}

/// Sets the hop limit for packets sent by the socket to a unicast destination.
///
/// Sets the hop limit (IPv6) or TTL (IPv4) for outbound packets going to a
/// unicast address.
pub fn set_udp_unicast_hop_limit<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
    unicast_hop_limit: Option<NonZeroU8>,
) {
    let mut core_ctx = Locked::new(core_ctx);
    let id = id.clone();
    I::map_ip(
        (IpInvariant((&mut core_ctx, bindings_ctx, unicast_hop_limit)), id),
        |(IpInvariant((core_ctx, bindings_ctx, unicast_hop_limit)), id)| {
            SocketHandler::<Ipv4, _>::set_udp_unicast_hop_limit(
                core_ctx,
                bindings_ctx,
                id,
                unicast_hop_limit,
            )
        },
        |(IpInvariant((core_ctx, bindings_ctx, unicast_hop_limit)), id)| {
            SocketHandler::<Ipv6, _>::set_udp_unicast_hop_limit(
                core_ctx,
                bindings_ctx,
                id,
                unicast_hop_limit,
            )
        },
    )
}

/// Sets the hop limit for packets sent by the socket to a multicast destination.
///
/// Sets the hop limit (IPv6) or TTL (IPv4) for outbound packets going to a
/// unicast address.
pub fn set_udp_multicast_hop_limit<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
    multicast_hop_limit: Option<NonZeroU8>,
) {
    let mut core_ctx = Locked::new(core_ctx);
    let id = id.clone();
    I::map_ip(
        (IpInvariant((&mut core_ctx, bindings_ctx, multicast_hop_limit)), id),
        |(IpInvariant((core_ctx, bindings_ctx, multicast_hop_limit)), id)| {
            SocketHandler::<Ipv4, _>::set_udp_multicast_hop_limit(
                core_ctx,
                bindings_ctx,
                id,
                multicast_hop_limit,
            )
        },
        |(IpInvariant((core_ctx, bindings_ctx, multicast_hop_limit)), id)| {
            SocketHandler::<Ipv6, _>::set_udp_multicast_hop_limit(
                core_ctx,
                bindings_ctx,
                id,
                multicast_hop_limit,
            )
        },
    )
}

/// Gets the hop limit for packets sent by the socket to a unicast destination.
pub fn get_udp_unicast_hop_limit<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &BC,
    id: &SocketId<I>,
) -> NonZeroU8 {
    let mut core_ctx = Locked::new(core_ctx);
    let id = id.clone();
    let IpInvariant(hop_limit) = I::map_ip::<_, IpInvariant<NonZeroU8>>(
        (IpInvariant((&mut core_ctx, bindings_ctx)), id),
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            IpInvariant(SocketHandler::<Ipv4, _>::get_udp_unicast_hop_limit(
                core_ctx,
                bindings_ctx,
                id,
            ))
        },
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            IpInvariant(SocketHandler::<Ipv6, _>::get_udp_unicast_hop_limit(
                core_ctx,
                bindings_ctx,
                id,
            ))
        },
    );
    hop_limit
}

/// Gets the hop limit for packets sent by the socket to a multicast destination.
///
/// Gets the hop limit (IPv6) or TTL (IPv4) for outbound packets going to a
/// unicast address.
pub fn get_udp_multicast_hop_limit<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &BC,
    id: &SocketId<I>,
) -> NonZeroU8 {
    let mut core_ctx = Locked::new(core_ctx);
    let id = id.clone();
    let IpInvariant(hop_limit) = I::map_ip::<_, IpInvariant<NonZeroU8>>(
        (IpInvariant((&mut core_ctx, bindings_ctx)), id),
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            IpInvariant(SocketHandler::<Ipv4, _>::get_udp_multicast_hop_limit(
                core_ctx,
                bindings_ctx,
                id,
            ))
        },
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            IpInvariant(SocketHandler::<Ipv6, _>::get_udp_multicast_hop_limit(
                core_ctx,
                bindings_ctx,
                id,
            ))
        },
    );
    hop_limit
}

/// Gets the transparent option.
pub fn get_udp_transparent<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    id: &SocketId<I>,
) -> bool {
    I::map_ip(
        (IpInvariant(&mut Locked::new(core_ctx)), id.clone()),
        |(IpInvariant(core_ctx), id)| core_ctx.get_udp_transparent(id.clone()),
        |(IpInvariant(core_ctx), id)| core_ctx.get_udp_transparent(id.clone()),
    )
}

/// Sets the transparent option.
pub fn set_udp_transparent<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    id: &SocketId<I>,
    value: bool,
) {
    I::map_ip::<_, ()>(
        (IpInvariant(&mut Locked::new(core_ctx)), id.clone(), value),
        |(IpInvariant(core_ctx), id, value)| {
            SocketHandler::set_udp_transparent(core_ctx, id, value)
        },
        |(IpInvariant(core_ctx), id, value)| {
            SocketHandler::set_udp_transparent(core_ctx, id, value)
        },
    );
}

/// Disconnects a connected UDP socket.
///
/// `disconnect_udp_connected` removes an existing connected socket and replaces
/// it with a listening socket bound to the same local address and port.
///
/// # Errors
///
/// Returns an error if the socket is not connected.
///
/// # Panics
///
/// Panics if `id` is not a valid `SocketId`.
pub fn disconnect_udp_connected<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
) -> Result<(), ExpectedConnError> {
    let mut core_ctx = Locked::new(core_ctx);
    I::map_ip(
        (IpInvariant((&mut core_ctx, bindings_ctx)), id.clone()),
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            SocketHandler::<Ipv4, _>::disconnect_connected(core_ctx, bindings_ctx, id)
        },
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            SocketHandler::<Ipv6, _>::disconnect_connected(core_ctx, bindings_ctx, id)
        },
    )
}

/// Shuts down a socket for reading and/or writing.
///
/// # Errors
///
/// Returns an error if the socket is not connected.
///
/// # Panics
///
/// Panics if `id` is not a valid `SocketId`.
pub fn shutdown<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &BC,
    id: &SocketId<I>,
    which: ShutdownType,
) -> Result<(), ExpectedConnError> {
    let mut core_ctx = Locked::new(core_ctx);

    I::map_ip(
        (IpInvariant((&mut core_ctx, bindings_ctx, which)), id.clone()),
        |(IpInvariant((core_ctx, bindings_ctx, which)), id)| {
            SocketHandler::<Ipv4, _>::shutdown(core_ctx, bindings_ctx, id, which)
        },
        |(IpInvariant((core_ctx, bindings_ctx, which)), id)| {
            SocketHandler::<Ipv6, _>::shutdown(core_ctx, bindings_ctx, id, which)
        },
    )
}

/// Get the shutdown state for a socket.
///
/// If the socket is not connected, or if `shutdown` was not called on it,
/// returns `None`.
///
/// # Panics
///
/// Panics if `id` is not a valid `SocketId`.
pub fn get_shutdown<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &BC,
    id: &SocketId<I>,
) -> Option<ShutdownType> {
    let mut core_ctx = Locked::new(core_ctx);

    I::map_ip(
        (IpInvariant((&mut core_ctx, bindings_ctx)), id.clone()),
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            SocketHandler::<Ipv4, _>::get_shutdown(core_ctx, bindings_ctx, id)
        },
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            SocketHandler::<Ipv6, _>::get_shutdown(core_ctx, bindings_ctx, id)
        },
    )
}

/// Information about the addresses for a socket.
#[derive(GenericOverIp)]
#[cfg_attr(test, derive(Debug, PartialEq))]
#[generic_over_ip(A, IpAddress)]
pub enum SocketInfo<A: IpAddress, D> {
    /// The socket was not bound.
    Unbound,
    /// The socket was listening.
    Listener(ListenerInfo<A, D>),
    /// The socket was connected.
    Connected(ConnInfo<A, D>),
}

impl<I: IpExt, D: WeakId> From<DatagramSocketInfo<I, D, Udp>> for SocketInfo<I::Addr, D> {
    fn from(value: DatagramSocketInfo<I, D, Udp>) -> Self {
        match value {
            DatagramSocketInfo::Unbound => Self::Unbound,
            DatagramSocketInfo::Listener(addr) => Self::Listener(addr.into()),
            DatagramSocketInfo::Connected(addr) => Self::Connected(addr.into()),
        }
    }
}

/// Gets the [`SocketInfo`] associated with the UDP socket referenced by `id`.
///
/// # Panics
///
/// `get_udp_info` panics if `id` is not a valid `SocketId`.
pub fn get_udp_info<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
) -> SocketInfo<I::Addr, WeakDeviceId<BC>> {
    let mut core_ctx = Locked::new(core_ctx);
    I::map_ip(
        (IpInvariant((&mut core_ctx, bindings_ctx)), id.clone()),
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            SocketHandler::<Ipv4, _>::get_udp_info(core_ctx, bindings_ctx, id)
        },
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            SocketHandler::<Ipv6, _>::get_udp_info(core_ctx, bindings_ctx, id)
        },
    )
}

/// Removes a socket that was previously created.
///
/// # Panics
///
/// Panics if `id` is not a valid [`SocketId`].
pub fn close<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: SocketId<I>,
) -> SocketInfo<I::Addr, WeakDeviceId<BC>> {
    let mut core_ctx = Locked::new(core_ctx);
    I::map_ip(
        (IpInvariant((&mut core_ctx, bindings_ctx)), id.clone()),
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            SocketHandler::<Ipv4, _>::close(core_ctx, bindings_ctx, id)
        },
        |(IpInvariant((core_ctx, bindings_ctx)), id)| {
            SocketHandler::<Ipv6, _>::close(core_ctx, bindings_ctx, id)
        },
    )
}

/// Use an existing socket to listen for incoming UDP packets.
///
/// `listen_udp` converts `id` into a listening socket and registers the new
/// socket as a listener for incoming UDP packets on the given `port`. If `addr`
/// is `None`, the listener is a "wildcard listener", and is bound to all local
/// addresses. See the [`crate::transport`] module documentation for more
/// details.
///
/// If `addr` is `Some``, and `addr` is already bound on the given port (either
/// by a listener or a connection), `listen_udp` will fail. If `addr` is `None`,
/// and a wildcard listener is already bound to the given port, `listen_udp`
/// will fail.
///
/// # Errors
///
/// Returns an error if the socket is not currently unbound.
///
/// # Panics
///
/// `listen_udp` panics if `id` is not a valid [`SocketId`].
pub fn listen_udp<I: Ip, BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    id: &SocketId<I>,
    addr: Option<SocketZonedIpAddr<I::Addr, DeviceId<BC>>>,
    port: Option<NonZeroU16>,
) -> Result<(), Either<ExpectedUnboundError, LocalAddressError>> {
    let mut core_ctx = Locked::new(core_ctx);
    I::map_ip::<_, Result<_, _>>(
        (IpInvariant((&mut core_ctx, bindings_ctx, port)), id.clone(), addr),
        |(IpInvariant((core_ctx, bindings_ctx, port)), id, addr)| {
            SocketHandler::<Ipv4, _>::listen_udp(core_ctx, bindings_ctx, id, addr, port)
                .map_err(IpInvariant)
        },
        |(IpInvariant((core_ctx, bindings_ctx, port)), id, addr)| {
            SocketHandler::<Ipv6, _>::listen_udp(core_ctx, bindings_ctx, id, addr, port)
                .map_err(IpInvariant)
        },
    )
    .map_err(|IpInvariant(e)| e)
}

#[cfg(test)]
mod tests {
    use alloc::{
        borrow::ToOwned,
        collections::{HashMap, HashSet},
        vec,
        vec::Vec,
    };
    use const_unwrap::const_unwrap_option;
    use core::convert::TryInto as _;

    use assert_matches::assert_matches;

    use ip_test_macro::ip_test;
    use itertools::Itertools as _;
    use net_declare::net_ip_v4 as ip_v4;
    use net_declare::net_ip_v6;
    use net_types::{
        ip::{IpAddr, Ipv4, Ipv4Addr, Ipv6, Ipv6Addr, Ipv6SourceAddr},
        AddrAndZone, LinkLocalAddr, MulticastAddr, Scope as _, ScopeableAddress as _, ZonedAddr,
    };
    use packet::{Buf, ParsablePacket, Serializer};
    use test_case::test_case;

    use super::*;
    use crate::{
        context::testutil::{
            FakeBindingsCtx, FakeCoreCtx, FakeCtxWithCoreCtx, FakeFrameCtx, Wrapped,
            WrappedFakeCoreCtx,
        },
        device::{
            testutil::{FakeDeviceId, FakeStrongDeviceId, FakeWeakDeviceId, MultipleDevicesId},
            DeviceId,
        },
        error::RemoteAddressError,
        ip::{
            device::state::IpDeviceStateIpExt,
            socket::testutil::{FakeDeviceConfig, FakeDualStackIpSocketCtx},
            testutil::{DualStackSendIpPacketMeta, FakeIpDeviceIdCtx},
            ResolveRouteError, SendIpPacketMeta,
        },
        socket::{self, datagram::MulticastInterfaceSelector},
        testutil::{set_logger_for_test, TestIpExt as _},
        uninstantiable::UninstantiableWrapper,
    };

    /// A packet received on a socket.
    #[derive(Debug, Derivative, PartialEq)]
    #[derivative(Default(bound = ""))]
    struct SocketReceived<I: IcmpIpExt> {
        packets: Vec<ReceivedPacket<I>>,
    }

    #[derive(Debug, PartialEq)]
    struct ReceivedPacket<I: Ip> {
        addr: ReceivedPacketAddrs<I>,
        body: Vec<u8>,
    }

    #[derive(Debug, PartialEq)]
    struct ReceivedPacketAddrs<I: Ip> {
        src_ip: I::Addr,
        dst_ip: I::Addr,
        src_port: Option<NonZeroU16>,
    }

    impl FakeUdpCoreCtx<FakeDeviceId> {
        fn new_fake_device<I: TestIpExt>() -> Self {
            Self::with_local_remote_ip_addrs(vec![local_ip::<I>()], vec![remote_ip::<I>()])
        }
    }

    impl<Outer: Default> Wrapped<Outer, FakeUdpInnerCoreCtx<FakeDeviceId>> {
        fn with_local_remote_ip_addrs<A: Into<SpecifiedAddr<IpAddr>>>(
            local_ips: Vec<A>,
            remote_ips: Vec<A>,
        ) -> Self {
            Self::with_state(FakeDualStackIpSocketCtx::new([FakeDeviceConfig {
                device: FakeDeviceId,
                local_ips,
                remote_ips,
            }]))
        }
    }

    impl<Outer: Default, D: FakeStrongDeviceId> Wrapped<Outer, FakeUdpInnerCoreCtx<D>> {
        fn with_state(state: FakeDualStackIpSocketCtx<D>) -> Self {
            Wrapped {
                outer: Outer::default(),
                inner: WrappedFakeCoreCtx::with_inner_and_outer_state(state, Default::default()),
            }
        }
    }

    /// `FakeCtx` specialized for UDP.
    type FakeUdpCtx<D> = FakeCtxWithCoreCtx<FakeUdpCoreCtx<D>, (), (), FakeBindingsCtxState>;

    #[derive(Derivative)]
    #[derivative(Default(bound = ""))]
    struct FakeBoundSockets<D: WeakId> {
        v4: BoundSockets<Ipv4, D>,
        v6: BoundSockets<Ipv6, D>,
    }

    impl<D: WeakId, I: IpExt> AsRef<BoundSockets<I, D>> for FakeBoundSockets<D> {
        fn as_ref(&self) -> &BoundSockets<I, D> {
            I::map_ip(
                IpInvariant(self),
                |IpInvariant(state)| &state.v4,
                |IpInvariant(state)| &state.v6,
            )
        }
    }

    impl<D: WeakId, I: IpExt> AsMut<BoundSockets<I, D>> for FakeBoundSockets<D> {
        fn as_mut(&mut self) -> &mut BoundSockets<I, D> {
            I::map_ip(
                IpInvariant(self),
                |IpInvariant(state)| &mut state.v4,
                |IpInvariant(state)| &mut state.v6,
            )
        }
    }

    impl<I: IpExt, D: WeakId> AsRef<Self> for SocketsState<I, D> {
        fn as_ref(&self) -> &Self {
            self
        }
    }

    impl<I: IpExt, D: WeakId> AsMut<Self> for SocketsState<I, D> {
        fn as_mut(&mut self) -> &mut Self {
            self
        }
    }

    type FakeUdpInnerCoreCtx<D> =
        Wrapped<FakeBoundSockets<FakeWeakDeviceId<D>>, FakeBufferCoreCtx<D>>;

    /// `FakeBindingsCtx` specialized for UDP.
    type FakeUdpBindingsCtx = FakeBindingsCtx<(), (), FakeBindingsCtxState>;

    /// The FakeCoreCtx held as the inner state of the [`WrappedFakeCoreCtx`] that
    /// is [`FakeUdpCoreCtx`].
    type FakeBufferCoreCtx<D> =
        FakeCoreCtx<FakeDualStackIpSocketCtx<D>, DualStackSendIpPacketMeta<D>, D>;

    type UdpFakeDeviceCtx = FakeUdpCtx<FakeDeviceId>;
    type UdpFakeDeviceCoreCtx = FakeUdpCoreCtx<FakeDeviceId>;
    type UdpFakeDeviceBindingsCtx = FakeUdpBindingsCtx;

    #[derive(Default)]
    struct FakeBindingsCtxState {
        received_v4: HashMap<SocketId<Ipv4>, SocketReceived<Ipv4>>,
        received_v6: HashMap<SocketId<Ipv6>, SocketReceived<Ipv6>>,
    }

    impl FakeBindingsCtxState {
        fn received<I: TestIpExt>(&self) -> &HashMap<SocketId<I>, SocketReceived<I>> {
            #[derive(GenericOverIp)]
            #[generic_over_ip(I, Ip)]
            struct Wrap<'a, I: TestIpExt>(&'a HashMap<SocketId<I>, SocketReceived<I>>);
            let Wrap(map) = I::map_ip(
                IpInvariant(self),
                |IpInvariant(state)| Wrap(&state.received_v4),
                |IpInvariant(state)| Wrap(&state.received_v6),
            );
            map
        }

        fn received_mut<I: IcmpIpExt>(&mut self) -> &mut HashMap<SocketId<I>, SocketReceived<I>> {
            #[derive(GenericOverIp)]
            #[generic_over_ip(I, Ip)]
            struct Wrap<'a, I: IcmpIpExt>(&'a mut HashMap<SocketId<I>, SocketReceived<I>>);
            let Wrap(map) = I::map_ip(
                IpInvariant(self),
                |IpInvariant(state)| Wrap(&mut state.received_v4),
                |IpInvariant(state)| Wrap(&mut state.received_v6),
            );
            map
        }

        fn socket_data<I: TestIpExt>(&self) -> HashMap<SocketId<I>, Vec<&'_ [u8]>> {
            self.received::<I>()
                .iter()
                .map(|(id, SocketReceived { packets })| {
                    (
                        id.clone(),
                        packets.iter().map(|ReceivedPacket { addr: _, body }| &body[..]).collect(),
                    )
                })
                .collect()
        }
    }

    impl<I: IcmpIpExt, D> UdpBindingsContext<I, D> for FakeUdpBindingsCtx {
        fn receive_udp<B: BufferMut>(
            &mut self,
            id: SocketId<I>,
            _device: &D,
            (dst_ip, _dst_port): (I::Addr, NonZeroU16),
            (src_ip, src_port): (I::Addr, Option<NonZeroU16>),
            body: &B,
        ) {
            self.state_mut().received_mut::<I>().entry(id).or_default().packets.push(
                ReceivedPacket {
                    addr: ReceivedPacketAddrs { src_ip, dst_ip, src_port },
                    body: body.as_ref().to_owned(),
                },
            )
        }
    }

    impl<
            I: TestIpExt,
            D: FakeStrongDeviceId,
            Outer: AsRef<SocketsState<I, FakeWeakDeviceId<D>>>
                + AsMut<SocketsState<I, FakeWeakDeviceId<D>>>,
        > StateContext<I, FakeUdpBindingsCtx> for Wrapped<Outer, FakeUdpInnerCoreCtx<D>>
    {
        type SocketStateCtx<'a> = FakeUdpInnerCoreCtx<D>;

        fn with_sockets_state<
            O,
            F: FnOnce(&mut Self::SocketStateCtx<'_>, &SocketsState<I, Self::WeakDeviceId>) -> O,
        >(
            &mut self,
            cb: F,
        ) -> O {
            let Self { outer, inner } = self;
            cb(inner, outer.as_ref())
        }

        fn with_sockets_state_mut<
            O,
            F: FnOnce(&mut Self::SocketStateCtx<'_>, &mut SocketsState<I, Self::WeakDeviceId>) -> O,
        >(
            &mut self,
            cb: F,
        ) -> O {
            let Self { outer, inner } = self;
            cb(inner, outer.as_mut())
        }

        fn with_bound_state_context<O, F: FnOnce(&mut Self::SocketStateCtx<'_>) -> O>(
            &mut self,
            cb: F,
        ) -> O {
            let Self { outer: _, inner } = self;
            cb(inner)
        }

        fn should_send_port_unreachable(&mut self) -> bool {
            false
        }
    }

    impl<I: TestIpExt, D: FakeStrongDeviceId> BoundStateContext<I, FakeUdpBindingsCtx>
        for FakeUdpInnerCoreCtx<D>
    {
        type IpSocketsCtx<'a> = FakeBufferCoreCtx<D>;
        type DualStackContext = I::UdpDualStackBoundStateContext<D>;
        type NonDualStackContext = I::UdpNonDualStackBoundStateContext<D>;

        fn with_bound_sockets<
            O,
            F: FnOnce(&mut Self::IpSocketsCtx<'_>, &BoundSockets<I, Self::WeakDeviceId>) -> O,
        >(
            &mut self,
            cb: F,
        ) -> O {
            let Self { outer, inner } = self;
            cb(inner, outer.as_ref())
        }

        fn with_bound_sockets_mut<
            O,
            F: FnOnce(&mut Self::IpSocketsCtx<'_>, &mut BoundSockets<I, Self::WeakDeviceId>) -> O,
        >(
            &mut self,
            cb: F,
        ) -> O {
            let Self { outer, inner } = self;
            cb(inner, outer.as_mut())
        }

        fn with_transport_context<O, F: FnOnce(&mut Self::IpSocketsCtx<'_>) -> O>(
            &mut self,
            cb: F,
        ) -> O {
            let Self { outer: _, inner } = self;
            cb(inner)
        }

        fn dual_stack_context(
            &mut self,
        ) -> MaybeDualStack<&mut Self::DualStackContext, &mut Self::NonDualStackContext> {
            struct Wrap<'a, I: Ip + TestIpExt, D: FakeStrongDeviceId + 'static>(
                MaybeDualStack<
                    &'a mut I::UdpDualStackBoundStateContext<D>,
                    &'a mut I::UdpNonDualStackBoundStateContext<D>,
                >,
            );
            // TODO(https://fxbug.dev/131992): Replace this with a derived impl.
            impl<'a, I: TestIpExt, NewIp: TestIpExt, D: FakeStrongDeviceId + 'static>
                GenericOverIp<NewIp> for Wrap<'a, I, D>
            {
                type Type = Wrap<'a, NewIp, D>;
            }

            let Wrap(context) = I::map_ip(
                IpInvariant(self),
                |IpInvariant(this)| Wrap(MaybeDualStack::NotDualStack(this)),
                |IpInvariant(this)| Wrap(MaybeDualStack::DualStack(this)),
            );
            context
        }
    }

    impl<D: FakeStrongDeviceId + 'static> UdpStateContext for FakeUdpInnerCoreCtx<D> {}

    impl<D: FakeStrongDeviceId> NonDualStackBoundStateContext<Ipv4, FakeUdpBindingsCtx>
        for FakeUdpInnerCoreCtx<D>
    {
    }

    impl<D: FakeStrongDeviceId> DualStackBoundStateContext<Ipv6, FakeUdpBindingsCtx>
        for FakeUdpInnerCoreCtx<D>
    {
        type IpSocketsCtx<'a> = FakeBufferCoreCtx<D>;

        fn with_both_bound_sockets_mut<
            O,
            F: FnOnce(
                &mut Self::IpSocketsCtx<'_>,
                &mut BoundSockets<Ipv6, Self::WeakDeviceId>,
                &mut BoundSockets<Ipv4, Self::WeakDeviceId>,
            ) -> O,
        >(
            &mut self,
            cb: F,
        ) -> O {
            let Self { inner, outer: FakeBoundSockets { v4, v6 } } = self;
            cb(inner, v6, v4)
        }

        fn with_other_bound_sockets_mut<
            O,
            F: FnOnce(&mut Self::IpSocketsCtx<'_>, &mut BoundSockets<Ipv4, Self::WeakDeviceId>) -> O,
        >(
            &mut self,
            cb: F,
        ) -> O {
            DualStackBoundStateContext::with_both_bound_sockets_mut(
                self,
                |core_ctx, _bound, other_bound| cb(core_ctx, other_bound),
            )
        }

        fn with_transport_context<O, F: FnOnce(&mut Self::IpSocketsCtx<'_>) -> O>(
            &mut self,
            cb: F,
        ) -> O {
            let Self { inner, outer: _ } = self;
            cb(inner)
        }
    }

    /// Ip packet delivery for the [`FakeUdpCoreCtx`].
    impl<I: IpExt + IpDeviceStateIpExt + TestIpExt, D: FakeStrongDeviceId>
        IpTransportContext<I, FakeUdpBindingsCtx, FakeUdpCoreCtx<D>> for UdpIpTransportContext
    {
        fn receive_icmp_error(
            _core_ctx: &mut FakeUdpCoreCtx<D>,
            _bindings_ctx: &mut FakeUdpBindingsCtx,
            _device: &D,
            _original_src_ip: Option<SpecifiedAddr<I::Addr>>,
            _original_dst_ip: SpecifiedAddr<I::Addr>,
            _original_udp_packet: &[u8],
            _err: I::ErrorCode,
        ) {
            unimplemented!()
        }

        fn receive_ip_packet<B: BufferMut>(
            core_ctx: &mut FakeUdpCoreCtx<D>,
            bindings_ctx: &mut FakeUdpBindingsCtx,
            device: &D,
            src_ip: I::RecvSrcAddr,
            dst_ip: SpecifiedAddr<I::Addr>,
            buffer: B,
        ) -> Result<(), (B, TransportReceiveError)> {
            // NB: The compiler can't deduce that `I::OtherVersion`` implements
            // `TestIpExt`, so use `map_ip` to transform the associated types
            // into concrete types (`Ipv4` & `Ipv6`).
            #[derive(GenericOverIp)]
            #[generic_over_ip(I, Ip)]
            struct SrcWrapper<I: IpExt>(I::RecvSrcAddr);
            let IpInvariant(result) = net_types::map_ip_twice!(
                I,
                (IpInvariant((core_ctx, bindings_ctx, device, buffer)), SrcWrapper(src_ip), dst_ip),
                |(
                    IpInvariant((core_ctx, bindings_ctx, device, buffer)),
                    SrcWrapper(src_ip),
                    dst_ip,
                )| {
                    IpInvariant(receive_ip_packet::<I, _, _, _>(
                        core_ctx,
                        bindings_ctx,
                        device,
                        src_ip,
                        dst_ip,
                        buffer,
                    ))
                }
            );
            result
        }
    }

    #[derive(Derivative)]
    #[derivative(Default(bound = ""))]
    struct DualStackSocketsState<D: WeakId> {
        v4: SocketsState<Ipv4, D>,
        v6: SocketsState<Ipv6, D>,
        udpv4_counters: UdpCounters<Ipv4>,
        udpv6_counters: UdpCounters<Ipv6>,
    }

    impl<I: IpExt, D: WeakId> AsRef<SocketsState<I, D>> for DualStackSocketsState<D> {
        fn as_ref(&self) -> &SocketsState<I, D> {
            I::map_ip(IpInvariant(self), |IpInvariant(dual)| &dual.v4, |IpInvariant(dual)| &dual.v6)
        }
    }

    impl<D: WeakId> DualStackSocketsState<D> {
        fn udp_counters<I: Ip>(&self) -> &UdpCounters<I> {
            I::map_ip(
                IpInvariant(self),
                |IpInvariant(dual)| &dual.udpv4_counters,
                |IpInvariant(dual)| &dual.udpv6_counters,
            )
        }
    }

    impl<I: IpExt, D: WeakId> AsMut<SocketsState<I, D>> for DualStackSocketsState<D> {
        fn as_mut(&mut self) -> &mut SocketsState<I, D> {
            I::map_ip(
                IpInvariant(self),
                |IpInvariant(dual)| &mut dual.v4,
                |IpInvariant(dual)| &mut dual.v6,
            )
        }
    }

    type FakeUdpCoreCtx<D> =
        Wrapped<DualStackSocketsState<FakeWeakDeviceId<D>>, FakeUdpInnerCoreCtx<D>>;

    impl<I: Ip, D: FakeStrongDeviceId> CounterContext<UdpCounters<I>> for FakeUdpCoreCtx<D> {
        fn with_counters<O, F: FnOnce(&UdpCounters<I>) -> O>(&self, cb: F) -> O {
            cb(&self.outer.udp_counters())
        }
    }

    fn local_ip<I: TestIpExt>() -> SpecifiedAddr<I::Addr> {
        I::get_other_ip_address(1)
    }

    fn remote_ip<I: TestIpExt>() -> SpecifiedAddr<I::Addr> {
        I::get_other_ip_address(2)
    }

    trait TestIpExt: crate::testutil::TestIpExt + IpExt + IpDeviceStateIpExt {
        type UdpDualStackBoundStateContext<D: FakeStrongDeviceId + 'static>:
            DualStackDatagramBoundStateContext<Self, FakeUdpBindingsCtx, Udp, DeviceId=D, WeakDeviceId=D::Weak>;
        type UdpNonDualStackBoundStateContext<D: FakeStrongDeviceId + 'static>:
            NonDualStackDatagramBoundStateContext<Self, FakeUdpBindingsCtx, Udp, DeviceId=D, WeakDeviceId=D::Weak>;
        fn try_into_recv_src_addr(addr: Self::Addr) -> Option<Self::RecvSrcAddr>;
    }

    impl TestIpExt for Ipv4 {
        type UdpDualStackBoundStateContext<D: FakeStrongDeviceId + 'static> =
            UninstantiableWrapper<FakeUdpInnerCoreCtx<D>>;

        type UdpNonDualStackBoundStateContext<D: FakeStrongDeviceId + 'static> =
            FakeUdpInnerCoreCtx<D>;

        fn try_into_recv_src_addr(addr: Ipv4Addr) -> Option<Ipv4Addr> {
            Some(addr)
        }
    }

    impl TestIpExt for Ipv6 {
        type UdpDualStackBoundStateContext<D: FakeStrongDeviceId + 'static> =
            FakeUdpInnerCoreCtx<D>;
        type UdpNonDualStackBoundStateContext<D: FakeStrongDeviceId + 'static> =
            UninstantiableWrapper<FakeUdpInnerCoreCtx<D>>;

        fn try_into_recv_src_addr(addr: Ipv6Addr) -> Option<Ipv6SourceAddr> {
            Ipv6SourceAddr::new(addr)
        }
    }

    /// Helper function to inject an UDP packet with the provided parameters.
    fn receive_udp_packet<
        A: IpAddress,
        D: FakeStrongDeviceId,
        CC: DeviceIdContext<AnyDevice, DeviceId = D>,
    >(
        core_ctx: &mut CC,
        bindings_ctx: &mut FakeUdpBindingsCtx,
        device: D,
        src_ip: A,
        dst_ip: A,
        src_port: impl Into<u16>,
        dst_port: NonZeroU16,
        body: &[u8],
    ) where
        A::Version: TestIpExt,
        UdpIpTransportContext: IpTransportContext<A::Version, FakeUdpBindingsCtx, CC>,
    {
        let builder =
            UdpPacketBuilder::new(src_ip, dst_ip, NonZeroU16::new(src_port.into()), dst_port);
        let buffer = Buf::new(body.to_owned(), ..)
            .encapsulate(builder)
            .serialize_vec_outer()
            .unwrap()
            .into_inner();
        <UdpIpTransportContext as IpTransportContext<A::Version, _, _>>::receive_ip_packet(
            core_ctx,
            bindings_ctx,
            &device,
            <A::Version as TestIpExt>::try_into_recv_src_addr(src_ip).unwrap(),
            SpecifiedAddr::new(dst_ip).unwrap(),
            buffer,
        )
        .expect("Receive IP packet succeeds");
    }

    const LOCAL_PORT: NonZeroU16 = const_unwrap_option(NonZeroU16::new(100));
    const OTHER_LOCAL_PORT: NonZeroU16 = const_unwrap_option(LOCAL_PORT.checked_add(1));
    const REMOTE_PORT: NonZeroU16 = const_unwrap_option(NonZeroU16::new(200));
    const OTHER_REMOTE_PORT: NonZeroU16 = const_unwrap_option(REMOTE_PORT.checked_add(1));

    fn conn_addr<I>(
        device: Option<FakeWeakDeviceId<FakeDeviceId>>,
    ) -> AddrVec<I, FakeWeakDeviceId<FakeDeviceId>, Udp>
    where
        I: Ip + TestIpExt,
    {
        let local_ip = SocketIpAddr::new_from_specified_or_panic(local_ip::<I>());
        let remote_ip = SocketIpAddr::new_from_specified_or_panic(remote_ip::<I>());
        ConnAddr {
            ip: ConnIpAddr {
                local: (local_ip, LOCAL_PORT),
                remote: (remote_ip, REMOTE_PORT.into()),
            },
            device,
        }
        .into()
    }

    fn local_listener<I>(
        device: Option<FakeWeakDeviceId<FakeDeviceId>>,
    ) -> AddrVec<I, FakeWeakDeviceId<FakeDeviceId>, Udp>
    where
        I: Ip + TestIpExt,
    {
        let local_ip = SocketIpAddr::new_from_specified_or_panic(local_ip::<I>());
        ListenerAddr { ip: ListenerIpAddr { identifier: LOCAL_PORT, addr: Some(local_ip) }, device }
            .into()
    }

    fn wildcard_listener<I>(
        device: Option<FakeWeakDeviceId<FakeDeviceId>>,
    ) -> AddrVec<I, FakeWeakDeviceId<FakeDeviceId>, Udp>
    where
        I: Ip + TestIpExt,
    {
        ListenerAddr { ip: ListenerIpAddr { identifier: LOCAL_PORT, addr: None }, device }.into()
    }

    #[ip_test]
    #[test_case(conn_addr(Some(FakeWeakDeviceId(FakeDeviceId))), [
            conn_addr(None), local_listener(Some(FakeWeakDeviceId(FakeDeviceId))), local_listener(None),
            wildcard_listener(Some(FakeWeakDeviceId(FakeDeviceId))), wildcard_listener(None)
        ]; "conn with device")]
    #[test_case(local_listener(Some(FakeWeakDeviceId(FakeDeviceId))),
        [local_listener(None), wildcard_listener(Some(FakeWeakDeviceId(FakeDeviceId))), wildcard_listener(None)];
        "local listener with device")]
    #[test_case(wildcard_listener(Some(FakeWeakDeviceId(FakeDeviceId))), [wildcard_listener(None)];
        "wildcard listener with device")]
    #[test_case(conn_addr(None), [local_listener(None), wildcard_listener(None)]; "conn no device")]
    #[test_case(local_listener(None), [wildcard_listener(None)]; "local listener no device")]
    #[test_case(wildcard_listener(None), []; "wildcard listener no device")]
    fn test_udp_addr_vec_iter_shadows_conn<I: Ip + IpExt, D: WeakId, const N: usize>(
        addr: AddrVec<I, D, Udp>,
        expected_shadows: [AddrVec<I, D, Udp>; N],
    ) {
        assert_eq!(addr.iter_shadows().collect::<HashSet<_>>(), HashSet::from(expected_shadows));
    }

    #[ip_test]
    fn test_iter_receiving_addrs<I: Ip + TestIpExt>() {
        let addr = ConnIpAddr {
            local: (SocketIpAddr::new_from_specified_or_panic(local_ip::<I>()), LOCAL_PORT),
            remote: (
                SocketIpAddr::new_from_specified_or_panic(remote_ip::<I>()),
                REMOTE_PORT.into(),
            ),
        };
        assert_eq!(
            iter_receiving_addrs::<I, _>(addr, FakeWeakDeviceId(FakeDeviceId)).collect::<Vec<_>>(),
            vec![
                // A socket connected on exactly the receiving vector has precedence.
                conn_addr(Some(FakeWeakDeviceId(FakeDeviceId))),
                // Connected takes precedence over listening with device match.
                conn_addr(None),
                local_listener(Some(FakeWeakDeviceId(FakeDeviceId))),
                // Specific IP takes precedence over device match.
                local_listener(None),
                wildcard_listener(Some(FakeWeakDeviceId(FakeDeviceId))),
                // Fallback to least specific
                wildcard_listener(None)
            ]
        );
    }

    /// Tests UDP listeners over different IP versions.
    ///
    /// Tests that a listener can be created, that the context receives packet
    /// notifications for that listener, and that we can send data using that
    /// listener.
    #[ip_test]
    fn test_listen_udp<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();
        let socket = SocketHandler::create_udp(&mut core_ctx);
        // Create a listener on the local port, bound to the local IP:
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            Some(LOCAL_PORT),
        )
        .expect("listen_udp failed");

        // Inject a packet and check that the context receives it:
        let body = [1, 2, 3, 4, 5];
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip.get(),
            local_ip.get(),
            REMOTE_PORT,
            LOCAL_PORT,
            &body[..],
        );

        assert_eq!(
            bindings_ctx.state().received::<I>(),
            &HashMap::from([(
                socket,
                SocketReceived {
                    packets: vec![ReceivedPacket {
                        body: body.into(),
                        addr: ReceivedPacketAddrs {
                            src_ip: remote_ip.get(),
                            dst_ip: local_ip.get(),
                            src_port: Some(REMOTE_PORT),
                        },
                    }]
                }
            )])
        );

        // Send a packet providing a local ip:
        SocketHandler::send_to(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            REMOTE_PORT.into(),
            Buf::new(body.to_vec(), ..),
        )
        .expect("send_to suceeded");
        // And send a packet that doesn't:
        SocketHandler::send_to(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            REMOTE_PORT.into(),
            Buf::new(body.to_vec(), ..),
        )
        .expect("send_to succeeded");
        let frames = core_ctx.inner.inner.frames();
        assert_eq!(frames.len(), 2);
        let check_frame = |(meta, frame_body): &(
            DualStackSendIpPacketMeta<FakeDeviceId>,
            Vec<u8>,
        )| {
            let SendIpPacketMeta { device: _, src_ip, dst_ip, next_hop, proto, ttl: _, mtu: _ } =
                meta.try_as::<I>().unwrap();
            assert_eq!(next_hop, &remote_ip);
            assert_eq!(src_ip, &local_ip);
            assert_eq!(dst_ip, &remote_ip);
            assert_eq!(proto, &IpProto::Udp.into());
            let mut buf = &frame_body[..];
            let udp_packet =
                UdpPacket::parse(&mut buf, UdpParseArgs::new(src_ip.get(), dst_ip.get()))
                    .expect("Parsed sent UDP packet");
            assert_eq!(udp_packet.src_port().unwrap(), LOCAL_PORT);
            assert_eq!(udp_packet.dst_port(), REMOTE_PORT);
            assert_eq!(udp_packet.body(), &body[..]);
        };
        check_frame(&frames[0]);
        check_frame(&frames[1]);
    }

    /// Tests that UDP packets without a connection are dropped.
    ///
    /// Tests that receiving a UDP packet on a port over which there isn't a
    /// listener causes the packet to be dropped correctly.
    #[ip_test]
    fn test_udp_drop<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();

        let body = [1, 2, 3, 4, 5];
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip.get(),
            local_ip.get(),
            REMOTE_PORT,
            LOCAL_PORT,
            &body[..],
        );
        assert_eq!(&bindings_ctx.state().socket_data::<I>(), &HashMap::new());
    }

    /// Tests that UDP connections can be created and data can be transmitted
    /// over it.
    ///
    /// Only tests with specified local port and address bounds.
    #[ip_test]
    fn test_udp_conn_basic<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        // Create a UDP connection with a specified local port and local IP.
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            Some(LOCAL_PORT),
        )
        .expect("listen_udp failed");
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect failed");

        // Inject a UDP packet and see if we receive it on the context.
        let body = [1, 2, 3, 4, 5];
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip.get(),
            local_ip.get(),
            REMOTE_PORT,
            LOCAL_PORT,
            &body[..],
        );

        assert_eq!(bindings_ctx.state().socket_data(), HashMap::from([(socket, vec![&body[..]])]));

        // Now try to send something over this new connection.
        SocketHandler::send(&mut core_ctx, &mut bindings_ctx, socket, Buf::new(body.to_vec(), ..))
            .expect("send_udp_conn returned an error");

        let (meta, frame_body) = assert_matches!(core_ctx.inner.inner.frames(), [frame] => frame);
        // Check first frame.
        let SendIpPacketMeta { device: _, src_ip, dst_ip, next_hop, proto, ttl: _, mtu: _ } =
            meta.try_as::<I>().unwrap();
        assert_eq!(next_hop, &remote_ip);
        assert_eq!(src_ip, &local_ip);
        assert_eq!(dst_ip, &remote_ip);
        assert_eq!(proto, &IpProto::Udp.into());
        let mut buf = &frame_body[..];
        let udp_packet = UdpPacket::parse(&mut buf, UdpParseArgs::new(src_ip.get(), dst_ip.get()))
            .expect("Parsed sent UDP packet");
        assert_eq!(udp_packet.src_port().unwrap(), LOCAL_PORT);
        assert_eq!(udp_packet.dst_port(), REMOTE_PORT);
        assert_eq!(udp_packet.body(), &body[..]);
    }

    /// Tests that UDP connections fail with an appropriate error for
    /// non-routable remote addresses.
    #[ip_test]
    fn test_udp_conn_unroutable<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        // Set fake context callback to treat all addresses as unroutable.
        let _local_ip = local_ip::<I>();
        let remote_ip = I::get_other_ip_address(127);
        // Create a UDP connection with a specified local port and local IP.
        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let conn_err = SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            unbound,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            REMOTE_PORT.into(),
        )
        .unwrap_err();

        assert_eq!(conn_err, ConnectError::Ip(ResolveRouteError::Unreachable.into()));
    }

    /// Tests that UDP listener creation fails with an appropriate error when
    /// local address is non-local.
    #[ip_test]
    fn test_udp_conn_cannot_bind<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());

        // Use remote address to trigger IpSockCreationError::LocalAddrNotAssigned.
        let remote_ip = remote_ip::<I>();
        // Create a UDP listener with a specified local port and local ip:
        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let result = SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            unbound,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            Some(LOCAL_PORT),
        );

        assert_eq!(result, Err(Either::Right(LocalAddressError::CannotBindToAddress)));
    }

    #[test]
    fn test_udp_conn_picks_link_local_source_address() {
        set_logger_for_test();
        // When the remote address has global scope but the source address
        // is link-local, make sure that the socket implicitly has its bound
        // device set.
        set_logger_for_test();
        let local_ip = SpecifiedAddr::new(net_ip_v6!("fe80::1")).unwrap();
        let remote_ip = SpecifiedAddr::new(net_ip_v6!("1:2:3:4::")).unwrap();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } = UdpFakeDeviceCtx::with_core_ctx(
            UdpFakeDeviceCoreCtx::with_local_remote_ip_addrs(vec![local_ip], vec![remote_ip]),
        );
        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            REMOTE_PORT.into(),
        )
        .expect("can connect");

        let info = SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket);
        let (conn_local_ip, conn_remote_ip) = assert_matches!(
            info,
            SocketInfo::Connected(ConnInfo {
                local_ip: conn_local_ip,
                remote_ip: conn_remote_ip,
                local_port: _,
                remote_port: _,
            }) => (conn_local_ip, conn_remote_ip)
        );
        assert_eq!(
            conn_local_ip.into_inner(),
            ZonedAddr::Zoned(AddrAndZone::new(local_ip, FakeWeakDeviceId(FakeDeviceId)).unwrap())
        );
        assert_eq!(conn_remote_ip.into_inner(), ZonedAddr::Unzoned(remote_ip));

        // Double-check that the bound device can't be changed after being set
        // implicitly.
        assert_eq!(
            SocketHandler::set_device(&mut core_ctx, &mut bindings_ctx, socket, None),
            Err(SocketError::Local(
                LocalAddressError::Zone(ZonedAddressError::DeviceZoneMismatch,)
            ))
        );
    }

    fn set_device_removed<I: TestIpExt>(core_ctx: &mut UdpFakeDeviceCoreCtx, device_removed: bool) {
        let core_ctx: &mut FakeCoreCtx<_, _, _> = core_ctx.as_mut();
        let core_ctx: &mut FakeIpDeviceIdCtx<_> = core_ctx.get_mut().as_mut();
        core_ctx.set_device_removed(FakeDeviceId, device_removed);
    }

    #[ip_test]
    #[test_case(
        true,
        Err(IpSockCreationError::Route(ResolveRouteError::Unreachable).into()); "remove device")]
    #[test_case(false, Ok(()); "dont remove device")]
    fn test_udp_conn_device_removed<I: Ip + TestIpExt>(
        remove_device: bool,
        expected: Result<(), ConnectError>,
    ) {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());

        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::set_device(&mut core_ctx, &mut bindings_ctx, unbound, Some(&FakeDeviceId))
            .unwrap();

        if remove_device {
            set_device_removed::<I>(&mut core_ctx, remove_device);
        }

        let remote_ip = remote_ip::<I>();
        assert_eq!(
            SocketHandler::connect(
                &mut core_ctx,
                &mut bindings_ctx,
                unbound,
                Some(ZonedAddr::Unzoned(remote_ip).into()),
                REMOTE_PORT.into(),
            ),
            expected,
        );
    }

    /// Tests that UDP connections fail with an appropriate error when local
    /// ports are exhausted.
    #[ip_test]
    fn test_udp_conn_exhausted<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());

        let local_ip = local_ip::<I>();
        // Exhaust local ports to trigger FailedToAllocateLocalPort error.
        for port_num in UdpBoundSocketMap::<I, FakeWeakDeviceId<FakeDeviceId>>::EPHEMERAL_RANGE {
            let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
            SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(local_ip).into()),
                NonZeroU16::new(port_num),
            )
            .unwrap();
        }

        let remote_ip = remote_ip::<I>();
        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let conn_err = SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            unbound,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            REMOTE_PORT.into(),
        )
        .unwrap_err();

        assert_eq!(conn_err, ConnectError::CouldNotAllocateLocalPort);
    }

    #[ip_test]
    fn test_connect_success<I: Ip + TestIpExt>() {
        set_logger_for_test();

        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());

        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();
        let multicast_addr = I::get_multicast_addr(3);
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);

        // Set some properties on the socket that should be preserved.
        SocketHandler::set_udp_posix_reuse_port(&mut core_ctx, &mut bindings_ctx, socket, true)
            .expect("is unbound");
        SocketHandler::set_udp_multicast_membership(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            multicast_addr,
            MulticastInterfaceSelector::LocalAddress(local_ip).into(),
            true,
        )
        .expect("join multicast group should succeed");

        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            Some(LOCAL_PORT),
        )
        .expect("Initial call to listen_udp was expected to succeed");

        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect should succeed");

        // Check that socket options set on the listener are propagated to the
        // connected socket.
        assert!(SocketHandler::get_udp_posix_reuse_port(&mut core_ctx, &bindings_ctx, socket));
        assert_eq!(
            core_ctx.inner.inner.get_ref().multicast_memberships::<I>(),
            HashMap::from([(
                (FakeDeviceId, multicast_addr),
                const_unwrap_option(NonZeroUsize::new(1))
            )])
        );
        assert_eq!(
            SocketHandler::set_udp_multicast_membership(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                multicast_addr,
                MulticastInterfaceSelector::LocalAddress(local_ip).into(),
                true
            ),
            Err(SetMulticastMembershipError::GroupAlreadyJoined)
        );
    }

    #[ip_test]
    fn test_connect_fails<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let local_ip = local_ip::<I>();
        let remote_ip = I::get_other_ip_address(127);
        let multicast_addr = I::get_multicast_addr(3);
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);

        // Set some properties on the socket that should be preserved.
        SocketHandler::set_udp_posix_reuse_port(&mut core_ctx, &mut bindings_ctx, socket, true)
            .expect("is unbound");
        SocketHandler::set_udp_multicast_membership(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            multicast_addr,
            MulticastInterfaceSelector::LocalAddress(local_ip).into(),
            true,
        )
        .expect("join multicast group should succeed");

        // Create a UDP connection with a specified local port and local IP.
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            Some(LOCAL_PORT),
        )
        .expect("Initial call to listen_udp was expected to succeed");

        assert_matches!(
            SocketHandler::connect(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(remote_ip).into()),
                REMOTE_PORT.into(),
            ),
            Err(ConnectError::Ip(IpSockCreationError::Route(ResolveRouteError::Unreachable)))
        );

        // Check that the listener was unchanged by the failed connection.
        assert!(SocketHandler::get_udp_posix_reuse_port(&mut core_ctx, &bindings_ctx, socket));
        assert_eq!(
            core_ctx.inner.inner.get_ref().multicast_memberships::<I>(),
            HashMap::from([(
                (FakeDeviceId, multicast_addr),
                const_unwrap_option(NonZeroUsize::new(1))
            )])
        );
        assert_eq!(
            SocketHandler::set_udp_multicast_membership(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                multicast_addr,
                MulticastInterfaceSelector::LocalAddress(local_ip).into(),
                true
            ),
            Err(SetMulticastMembershipError::GroupAlreadyJoined)
        );
    }

    #[ip_test]
    fn test_reconnect_udp_conn_success<I: Ip + TestIpExt>() {
        set_logger_for_test();

        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();
        let other_remote_ip = I::get_other_ip_address(3);

        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::with_local_remote_ip_addrs(
                vec![local_ip],
                vec![remote_ip, other_remote_ip],
            ));

        let local_ip = ZonedAddr::Unzoned(local_ip).into();
        let remote_ip = ZonedAddr::Unzoned(remote_ip).into();
        let other_remote_ip = ZonedAddr::Unzoned(other_remote_ip).into();

        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(local_ip),
            Some(LOCAL_PORT),
        )
        .expect("listen should succeed");

        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(remote_ip),
            REMOTE_PORT.into(),
        )
        .expect("connect was expected to succeed");

        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(other_remote_ip),
            OTHER_REMOTE_PORT.into(),
        )
        .expect("connect should succeed");
        assert_eq!(
            SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket),
            SocketInfo::Connected(ConnInfo {
                local_ip: local_ip.into_inner().map_zone(FakeWeakDeviceId).into(),
                local_port: LOCAL_PORT,
                remote_ip: other_remote_ip.into_inner().map_zone(FakeWeakDeviceId).into(),
                remote_port: OTHER_REMOTE_PORT.into(),
            })
        );
    }

    #[ip_test]
    fn test_reconnect_udp_conn_fails<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let local_ip = ZonedAddr::Unzoned(local_ip::<I>()).into();
        let remote_ip = ZonedAddr::Unzoned(remote_ip::<I>()).into();
        let other_remote_ip = ZonedAddr::Unzoned(I::get_other_ip_address(3)).into();

        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(local_ip),
            Some(LOCAL_PORT),
        )
        .expect("listen should succeed");

        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(remote_ip),
            REMOTE_PORT.into(),
        )
        .expect("connect was expected to succeed");
        let error = SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(other_remote_ip),
            OTHER_REMOTE_PORT.into(),
        )
        .expect_err("connect should fail");
        assert_matches!(
            error,
            ConnectError::Ip(IpSockCreationError::Route(ResolveRouteError::Unreachable))
        );

        assert_eq!(
            SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket),
            SocketInfo::Connected(ConnInfo {
                local_ip: local_ip.into_inner().map_zone(FakeWeakDeviceId).into(),
                local_port: LOCAL_PORT,
                remote_ip: remote_ip.into_inner().map_zone(FakeWeakDeviceId).into(),
                remote_port: REMOTE_PORT.into()
            })
        );
    }

    #[ip_test]
    fn test_send_to<I: Ip + TestIpExt>() {
        set_logger_for_test();

        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();
        let other_remote_ip = I::get_other_ip_address(3);

        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::with_local_remote_ip_addrs(
                vec![local_ip],
                vec![remote_ip, other_remote_ip],
            ));

        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            Some(LOCAL_PORT),
        )
        .expect("listen should succeed");
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect should succeed");

        let body = [1, 2, 3, 4, 5];
        // Try to send something with send_to
        SocketHandler::send_to(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(other_remote_ip).into()),
            REMOTE_PORT.into(),
            Buf::new(body.to_vec(), ..),
        )
        .expect("send_to failed");

        // The socket should not have been affected.
        let info = SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket);
        let info = assert_matches!(info, SocketInfo::Connected(info) => info);
        assert_eq!(info.local_ip, ZonedAddr::Unzoned(local_ip).into());
        assert_eq!(info.remote_ip, ZonedAddr::Unzoned(remote_ip).into());
        assert_eq!(info.remote_port, REMOTE_PORT.into());

        // Check first frame.
        let (meta, frame_body) = assert_matches!(core_ctx.inner.inner.frames(), [frame] => frame);
        let SendIpPacketMeta { device: _, src_ip, dst_ip, next_hop, proto, ttl: _, mtu: _ } =
            meta.try_as::<I>().unwrap();

        assert_eq!(next_hop, &other_remote_ip);
        assert_eq!(src_ip, &local_ip);
        assert_eq!(dst_ip, &other_remote_ip);
        assert_eq!(proto, &I::Proto::from(IpProto::Udp));
        let mut buf = &frame_body[..];
        let udp_packet = UdpPacket::parse(&mut buf, UdpParseArgs::new(src_ip.get(), dst_ip.get()))
            .expect("Parsed sent UDP packet");
        assert_eq!(udp_packet.src_port().unwrap(), LOCAL_PORT);
        assert_eq!(udp_packet.dst_port(), REMOTE_PORT);
        assert_eq!(udp_packet.body(), &body[..]);
    }

    /// Tests that UDP send failures are propagated as errors.
    ///
    /// Only tests with specified local port and address bounds.
    #[ip_test]
    fn test_send_udp_conn_failure<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let _local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();
        // Create a UDP connection with a specified local port and local IP.
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect failed");

        // Instruct the fake frame context to throw errors.
        let frames: &mut FakeFrameCtx<_> = core_ctx.as_mut();
        frames.set_should_error_for_frame(|_frame_meta| true);

        // Now try to send something over this new connection:
        let send_err =
            SocketHandler::send(&mut core_ctx, &mut bindings_ctx, socket, Buf::new(Vec::new(), ..))
                .unwrap_err();
        assert_eq!(send_err, Either::Left(SendError::IpSock(IpSockSendError::Mtu)));
    }

    #[ip_test]
    fn test_send_udp_conn_device_removed<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let remote_ip = remote_ip::<I>();
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::set_device(&mut core_ctx, &mut bindings_ctx, socket, Some(&FakeDeviceId))
            .unwrap();
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect failed");

        for (device_removed, expected_res) in [
            (false, Ok(())),
            (
                true,
                Err(Either::Left(SendError::IpSock(IpSockSendError::Unroutable(
                    ResolveRouteError::Unreachable,
                )))),
            ),
        ] {
            set_device_removed::<I>(&mut core_ctx, device_removed);

            assert_eq!(
                SocketHandler::send(
                    &mut core_ctx,
                    &mut bindings_ctx,
                    socket,
                    Buf::new(Vec::new(), ..),
                ),
                expected_res,
            )
        }
    }

    #[ip_test]
    #[test_case(false, ShutdownType::Send; "shutdown send then send")]
    #[test_case(false, ShutdownType::SendAndReceive; "shutdown both then send")]
    #[test_case(true, ShutdownType::Send; "shutdown send then sendto")]
    #[test_case(true, ShutdownType::SendAndReceive; "shutdown both then sendto")]
    fn test_send_udp_after_shutdown<I: Ip + TestIpExt>(send_to: bool, shutdown: ShutdownType) {
        set_logger_for_test();

        #[derive(Debug)]
        struct NotWriteableError;

        fn send<I: Ip + TestIpExt, CC: SocketHandler<I, FakeUdpBindingsCtx>>(
            remote_ip: Option<SocketZonedIpAddr<I::Addr, CC::DeviceId>>,
            core_ctx: &mut CC,
            bindings_ctx: &mut FakeUdpBindingsCtx,
            id: SocketId<I>,
        ) -> Result<(), NotWriteableError> {
            match remote_ip {
                Some(remote_ip) => SocketHandler::send_to(
                    core_ctx,
                    bindings_ctx,
                    id,
                    Some(remote_ip),
                    REMOTE_PORT.into(),
                    Buf::new(Vec::new(), ..),
                )
                .map_err(
                    |e| assert_matches!(e, Either::Right(SendToError::NotWriteable) => NotWriteableError)
                ),
                None => SocketHandler::send(
                    core_ctx,
                    bindings_ctx,
                    id,
                    Buf::new(Vec::new(), ..),
                )
                .map_err(|e| assert_matches!(e, Either::Left(SendError::NotWriteable) => NotWriteableError)),
            }
        }

        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let remote_ip = ZonedAddr::Unzoned(remote_ip::<I>()).into();
        let send_to_ip = send_to.then_some(remote_ip);

        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(remote_ip),
            REMOTE_PORT.into(),
        )
        .expect("connect failed");

        send(send_to_ip, &mut core_ctx, &mut bindings_ctx, socket).expect("can send");
        SocketHandler::shutdown(&mut core_ctx, &bindings_ctx, socket, shutdown)
            .expect("is connected");

        assert_matches!(
            send(send_to_ip, &mut core_ctx, &mut bindings_ctx, socket),
            Err(NotWriteableError)
        );
    }

    #[ip_test]
    #[test_case(ShutdownType::Receive; "receive")]
    #[test_case(ShutdownType::SendAndReceive; "both")]
    fn test_marked_for_receive_shutdown<I: Ip + TestIpExt>(which: ShutdownType) {
        set_logger_for_test();

        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());

        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(local_ip::<I>()).into()),
            Some(LOCAL_PORT),
        )
        .expect("can bind");
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip::<I>()).into()),
            REMOTE_PORT.into(),
        )
        .expect("can connect");

        // Receive once, then set the shutdown flag, then receive again and
        // check that it doesn't get to the socket.

        let packet = [1, 1, 1, 1];
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip::<I>().get(),
            local_ip::<I>().get(),
            REMOTE_PORT,
            LOCAL_PORT,
            &packet[..],
        );

        assert_eq!(
            bindings_ctx.state().socket_data(),
            HashMap::from([(socket, vec![&packet[..]])])
        );
        SocketHandler::shutdown(&mut core_ctx, &bindings_ctx, socket, which).expect("is connected");
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip::<I>().get(),
            local_ip::<I>().get(),
            REMOTE_PORT,
            LOCAL_PORT,
            &packet[..],
        );
        assert_eq!(
            bindings_ctx.state().socket_data(),
            HashMap::from([(socket, vec![&packet[..]])])
        );

        // Calling shutdown for the send direction doesn't change anything.
        SocketHandler::shutdown(&mut core_ctx, &bindings_ctx, socket, ShutdownType::Send)
            .expect("is connected");
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip::<I>().get(),
            local_ip::<I>().get(),
            REMOTE_PORT,
            LOCAL_PORT,
            &packet[..],
        );
        assert_eq!(
            bindings_ctx.state().socket_data(),
            HashMap::from([(socket, vec![&packet[..]])])
        );
    }

    /// Tests that if we have multiple listeners and connections, demuxing the
    /// flows is performed correctly.
    #[ip_test]
    fn test_udp_demux<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let local_ip = local_ip::<I>();
        let remote_ip_a = I::get_other_ip_address(70);
        let remote_ip_b = I::get_other_ip_address(72);
        let local_port_a = NonZeroU16::new(100).unwrap();
        let local_port_b = NonZeroU16::new(101).unwrap();
        let local_port_c = NonZeroU16::new(102).unwrap();
        let local_port_d = NonZeroU16::new(103).unwrap();

        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::with_local_remote_ip_addrs(
                vec![local_ip],
                vec![remote_ip_a, remote_ip_b],
            ));

        // Create some UDP connections and listeners:
        // conn2 has just a remote addr different than conn1, which requires
        // allowing them to share the local port.
        let [conn1, conn2] = [remote_ip_a, remote_ip_b].map(|remote_ip| {
            let socket = SocketHandler::create_udp(&mut core_ctx);
            SocketHandler::set_udp_posix_reuse_port(&mut core_ctx, &mut bindings_ctx, socket, true)
                .expect("is unbound");
            SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(local_ip).into()),
                Some(local_port_d),
            )
            .expect("listen_udp failed");
            SocketHandler::connect(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(remote_ip).into()),
                REMOTE_PORT.into(),
            )
            .expect("connect failed");
            socket
        });
        let list1 = SocketHandler::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            list1,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            Some(local_port_a),
        )
        .expect("listen_udp failed");
        let list2 = SocketHandler::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            list2,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            Some(local_port_b),
        )
        .expect("listen_udp failed");
        let wildcard_list = SocketHandler::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            wildcard_list,
            None,
            Some(local_port_c),
        )
        .expect("listen_udp failed");

        let mut expectations = HashMap::<SocketId<I>, SocketReceived<I>>::new();
        // Now inject UDP packets that each of the created connections should
        // receive.
        let body_conn1 = [1, 1, 1, 1];
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip_a.get(),
            local_ip.get(),
            REMOTE_PORT,
            local_port_d,
            &body_conn1[..],
        );
        expectations.entry(conn1).or_default().packets.push(ReceivedPacket {
            body: body_conn1.into(),
            addr: ReceivedPacketAddrs {
                src_ip: remote_ip_a.get(),
                dst_ip: local_ip.get(),
                src_port: Some(REMOTE_PORT),
            },
        });
        assert_eq!(bindings_ctx.state().received(), &expectations);

        let body_conn2 = [2, 2, 2, 2];
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip_b.get(),
            local_ip.get(),
            REMOTE_PORT,
            local_port_d,
            &body_conn2[..],
        );
        expectations.entry(conn2).or_default().packets.push(ReceivedPacket {
            addr: ReceivedPacketAddrs {
                src_ip: remote_ip_b.get(),
                dst_ip: local_ip.get(),
                src_port: Some(REMOTE_PORT),
            },
            body: body_conn2.into(),
        });
        assert_eq!(bindings_ctx.state().received(), &expectations);

        let body_list1 = [3, 3, 3, 3];
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip_a.get(),
            local_ip.get(),
            REMOTE_PORT,
            local_port_a,
            &body_list1[..],
        );
        expectations.entry(list1).or_default().packets.push(ReceivedPacket {
            addr: ReceivedPacketAddrs {
                src_ip: remote_ip_a.get(),
                dst_ip: local_ip.get(),
                src_port: Some(REMOTE_PORT),
            },
            body: body_list1.into(),
        });
        assert_eq!(bindings_ctx.state().received(), &expectations);

        let body_list2 = [4, 4, 4, 4];
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip_a.get(),
            local_ip.get(),
            REMOTE_PORT,
            local_port_b,
            &body_list2[..],
        );
        expectations.entry(list2).or_default().packets.push(ReceivedPacket {
            body: body_list2.into(),
            addr: ReceivedPacketAddrs {
                src_ip: remote_ip_a.get(),
                dst_ip: local_ip.get(),
                src_port: Some(REMOTE_PORT),
            },
        });
        assert_eq!(bindings_ctx.state().received(), &expectations);

        let body_wildcard_list = [5, 5, 5, 5];
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip_a.get(),
            local_ip.get(),
            REMOTE_PORT,
            local_port_c,
            &body_wildcard_list[..],
        );
        expectations.entry(wildcard_list).or_default().packets.push(ReceivedPacket {
            addr: ReceivedPacketAddrs {
                src_ip: remote_ip_a.get(),
                dst_ip: local_ip.get(),
                src_port: Some(REMOTE_PORT),
            },
            body: body_wildcard_list.into(),
        });
        assert_eq!(bindings_ctx.state().received(), &expectations);
    }

    /// Tests UDP wildcard listeners for different IP versions.
    #[ip_test]
    fn test_wildcard_listeners<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let local_ip_a = I::get_other_ip_address(1);
        let local_ip_b = I::get_other_ip_address(2);
        let remote_ip_a = I::get_other_ip_address(70);
        let remote_ip_b = I::get_other_ip_address(72);
        let listener = SocketHandler::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            listener,
            None,
            Some(LOCAL_PORT),
        )
        .expect("listen_udp failed");

        let body = [1, 2, 3, 4, 5];
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip_a.get(),
            local_ip_a.get(),
            REMOTE_PORT,
            LOCAL_PORT,
            &body[..],
        );
        // Receive into a different local IP.
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            remote_ip_b.get(),
            local_ip_b.get(),
            REMOTE_PORT,
            LOCAL_PORT,
            &body[..],
        );

        // Check that we received both packets for the listener.
        assert_eq!(
            bindings_ctx.state().received::<I>(),
            &HashMap::from([(
                listener,
                SocketReceived {
                    packets: vec![
                        ReceivedPacket {
                            addr: ReceivedPacketAddrs {
                                src_ip: remote_ip_a.get(),
                                dst_ip: local_ip_a.get(),
                                src_port: Some(REMOTE_PORT),
                            },
                            body: body.into(),
                        },
                        ReceivedPacket {
                            addr: ReceivedPacketAddrs {
                                src_ip: remote_ip_b.get(),
                                dst_ip: local_ip_b.get(),
                                src_port: Some(REMOTE_PORT),
                            },
                            body: body.into()
                        }
                    ]
                }
            )])
        );
    }

    #[ip_test]
    fn test_receive_source_port_zero_on_listener<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let listener = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            listener,
            None,
            Some(LOCAL_PORT),
        )
        .expect("listen_udp failed");

        let body = [];
        let (src_ip, src_port) = (I::FAKE_CONFIG.remote_ip.get(), 0u16);
        let (dst_ip, dst_port) = (I::FAKE_CONFIG.local_ip.get(), LOCAL_PORT);

        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            src_ip,
            dst_ip,
            src_port,
            dst_port,
            &body[..],
        );
        // Check that we received both packets for the listener.
        assert_eq!(
            bindings_ctx.state().received(),
            &HashMap::from([(
                listener,
                SocketReceived {
                    packets: vec![ReceivedPacket {
                        body: vec![],
                        addr: ReceivedPacketAddrs { src_ip, dst_ip, src_port: None },
                    }],
                }
            )])
        );
    }

    #[ip_test]
    fn test_receive_source_addr_unspecified_on_listener<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let listener = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            listener,
            None,
            Some(LOCAL_PORT),
        )
        .expect("listen_udp failed");

        let body = [];
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            I::UNSPECIFIED_ADDRESS,
            I::FAKE_CONFIG.local_ip.get(),
            REMOTE_PORT,
            LOCAL_PORT,
            &body[..],
        );
        // Check that we received the packet on the listener.
        assert_eq!(
            bindings_ctx.state().socket_data(),
            HashMap::from([(listener, vec![&body[..]])])
        );
    }

    #[ip_test]
    #[test_case(const_unwrap_option(NonZeroU16::new(u16::MAX)), Ok(const_unwrap_option(NonZeroU16::new(u16::MAX))); "ephemeral available")]
    #[test_case(const_unwrap_option(NonZeroU16::new(100)), Err(LocalAddressError::FailedToAllocateLocalPort);
        "no ephemeral available")]
    fn test_bind_picked_port_all_others_taken<I: Ip + TestIpExt>(
        available_port: NonZeroU16,
        expected_result: Result<NonZeroU16, LocalAddressError>,
    ) {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());

        for port in 1..=u16::MAX {
            let port = NonZeroU16::new(port).unwrap();
            if port == available_port {
                continue;
            }
            let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
            let _listener = SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                unbound,
                None,
                Some(port),
            )
            .expect("uncontested bind");
        }

        // Now that all but the LOCAL_PORT are occupied, ask the stack to
        // select a port.
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let result =
            SocketHandler::listen_udp(&mut core_ctx, &mut bindings_ctx, socket, None, None)
                .map(|()| {
                    let info =
                        SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket);
                    assert_matches!(info, SocketInfo::Listener(info) => info.local_port)
                })
                .map_err(Either::unwrap_right);
        assert_eq!(result, expected_result);
    }

    #[ip_test]
    fn test_receive_multicast_packet<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let local_ip = local_ip::<I>();
        let remote_ip = I::get_other_ip_address(70);
        let multicast_addr = I::get_multicast_addr(0);
        let multicast_addr_other = I::get_multicast_addr(1);

        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } = UdpFakeDeviceCtx::with_core_ctx(
            UdpFakeDeviceCoreCtx::with_local_remote_ip_addrs(vec![local_ip], vec![remote_ip]),
        );

        // Create 3 sockets: one listener for all IPs, two listeners on the same
        // local address.
        let any_listener = {
            let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
            SocketHandler::set_udp_posix_reuse_port(&mut core_ctx, &mut bindings_ctx, socket, true)
                .expect("is unbound");
            SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                None,
                Some(LOCAL_PORT),
            )
            .expect("listen_udp failed");
            socket
        };

        let specific_listeners = [(); 2].map(|()| {
            let socket = SocketHandler::create_udp(&mut core_ctx);
            SocketHandler::set_udp_posix_reuse_port(&mut core_ctx, &mut bindings_ctx, socket, true)
                .expect("is unbound");
            SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(multicast_addr.into_specified()).into()),
                Some(LOCAL_PORT),
            )
            .expect("listen_udp failed");
            socket
        });

        let mut receive_packet = |body, local_ip: MulticastAddr<I::Addr>| {
            let body = [body];
            receive_udp_packet(
                &mut core_ctx,
                &mut bindings_ctx,
                FakeDeviceId,
                remote_ip.get(),
                local_ip.get(),
                REMOTE_PORT,
                LOCAL_PORT,
                &body,
            )
        };

        // These packets should be received by all listeners.
        receive_packet(1, multicast_addr);
        receive_packet(2, multicast_addr);

        // This packet should be received only by the all-IPs listener.
        receive_packet(3, multicast_addr_other);

        assert_eq!(
            bindings_ctx.state().socket_data(),
            HashMap::from([
                (specific_listeners[0], vec![[1].as_slice(), &[2]]),
                (specific_listeners[1], vec![&[1], &[2]]),
                (any_listener, vec![&[1], &[2], &[3]]),
            ]),
        );
    }

    type UdpMultipleDevicesCtx = FakeUdpCtx<MultipleDevicesId>;
    type UdpMultipleDevicesCoreCtx = FakeUdpCoreCtx<MultipleDevicesId>;
    type UdpMultipleDevicesBindingsCtx = FakeUdpBindingsCtx;

    impl FakeUdpCoreCtx<MultipleDevicesId> {
        fn new_multiple_devices<I: TestIpExt>() -> Self {
            let remote_ips = vec![I::get_other_remote_ip_address(1)];
            Self::with_state(FakeDualStackIpSocketCtx::new(
                MultipleDevicesId::all().into_iter().enumerate().map(|(i, device)| {
                    FakeDeviceConfig {
                        device,
                        local_ips: vec![I::get_other_ip_address((i + 1).try_into().unwrap())],
                        remote_ips: remote_ips.clone(),
                    }
                }),
            ))
        }
    }

    /// Tests that if sockets are bound to devices, they will only receive
    /// packets that are received on those devices.
    #[ip_test]
    fn test_bound_to_device_receive<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(
                UdpMultipleDevicesCoreCtx::new_multiple_devices::<I>(),
            );
        let core_ctx = &mut core_ctx;
        let bound_first_device = SocketHandler::<I, _>::create_udp(core_ctx);
        SocketHandler::listen_udp(
            core_ctx,
            &mut bindings_ctx,
            bound_first_device,
            Some(ZonedAddr::Unzoned(local_ip::<I>()).into()),
            Some(LOCAL_PORT),
        )
        .expect("listen should succeed");
        SocketHandler::connect(
            core_ctx,
            &mut bindings_ctx,
            bound_first_device,
            Some(ZonedAddr::Unzoned(I::get_other_remote_ip_address(1)).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect should succeed");
        SocketHandler::set_device(
            core_ctx,
            &mut bindings_ctx,
            bound_first_device,
            Some(&MultipleDevicesId::A),
        )
        .expect("bind should succeed");

        let bound_second_device = SocketHandler::create_udp(core_ctx);
        SocketHandler::set_device(
            core_ctx,
            &mut bindings_ctx,
            bound_second_device,
            Some(&MultipleDevicesId::B),
        )
        .unwrap();
        SocketHandler::listen_udp(
            core_ctx,
            &mut bindings_ctx,
            bound_second_device,
            None,
            Some(LOCAL_PORT),
        )
        .expect("listen should succeed");

        // Inject a packet received on `MultipleDevicesId::A` from the specified
        // remote; this should go to the first socket.
        let body = [1, 2, 3, 4, 5];
        receive_udp_packet(
            core_ctx,
            &mut bindings_ctx,
            MultipleDevicesId::A,
            I::get_other_remote_ip_address(1).get(),
            local_ip::<I>().get(),
            REMOTE_PORT,
            LOCAL_PORT,
            &body[..],
        );

        // A second packet received on `MultipleDevicesId::B` will go to the
        // second socket.
        receive_udp_packet(
            core_ctx,
            &mut bindings_ctx,
            MultipleDevicesId::B,
            I::get_other_remote_ip_address(1).get(),
            local_ip::<I>().get(),
            REMOTE_PORT,
            LOCAL_PORT,
            &body[..],
        );
        assert_eq!(
            bindings_ctx.state().socket_data(),
            HashMap::from([
                (bound_first_device, vec![&body[..]]),
                (bound_second_device, vec![&body[..]])
            ])
        );
    }

    /// Tests that if sockets are bound to devices, they will send packets out
    /// of those devices.
    #[ip_test]
    fn test_bound_to_device_send<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(
                UdpMultipleDevicesCoreCtx::new_multiple_devices::<I>(),
            );
        let core_ctx = &mut core_ctx;
        let bound_on_devices = MultipleDevicesId::all().map(|device| {
            let socket = SocketHandler::<I, _>::create_udp(core_ctx);
            SocketHandler::set_device(core_ctx, &mut bindings_ctx, socket, Some(&device)).unwrap();
            SocketHandler::listen_udp(core_ctx, &mut bindings_ctx, socket, None, Some(LOCAL_PORT))
                .expect("listen should succeed");
            socket
        });

        // Send a packet from each socket.
        let body = [1, 2, 3, 4, 5];
        for socket in bound_on_devices {
            SocketHandler::send_to(
                core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(I::get_other_remote_ip_address(1)).into()),
                REMOTE_PORT.into(),
                Buf::new(body.to_vec(), ..),
            )
            .expect("send should succeed");
        }

        let mut received_devices = core_ctx
            .inner
            .inner
            .frames()
            .iter()
            .map(|(meta, _body)| {
                let SendIpPacketMeta {
                    device,
                    src_ip: _,
                    dst_ip,
                    next_hop: _,
                    proto,
                    ttl: _,
                    mtu: _,
                } = meta.try_as::<I>().unwrap();
                assert_eq!(proto, &IpProto::Udp.into());
                assert_eq!(dst_ip, &I::get_other_remote_ip_address(1),);
                *device
            })
            .collect::<Vec<_>>();
        received_devices.sort();
        assert_eq!(received_devices, &MultipleDevicesId::all());
    }

    fn receive_packet_on<I: Ip + TestIpExt>(
        core_ctx: &mut UdpMultipleDevicesCoreCtx,
        bindings_ctx: &mut UdpMultipleDevicesBindingsCtx,
        device: MultipleDevicesId,
    ) {
        const BODY: [u8; 5] = [1, 2, 3, 4, 5];
        receive_udp_packet(
            core_ctx,
            bindings_ctx,
            device,
            I::get_other_remote_ip_address(1).get(),
            local_ip::<I>().get(),
            REMOTE_PORT,
            LOCAL_PORT,
            &BODY[..],
        )
    }

    /// Check that sockets can be bound to and unbound from devices.
    #[ip_test]
    fn test_bind_unbind_device<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(
                UdpMultipleDevicesCoreCtx::new_multiple_devices::<I>(),
            );
        let core_ctx = &mut core_ctx;

        // Start with `socket` bound to a device.
        let socket = SocketHandler::create_udp(core_ctx);
        // TODO(https://fxbug.dev/21198): Test against dual-stack sockets.
        if I::VERSION == net_types::ip::IpVersion::V6 {
            SocketHandler::set_dual_stack_enabled(core_ctx, &mut bindings_ctx, socket, false)
                .expect("disabling dual stack should succeed")
        }
        SocketHandler::set_device(core_ctx, &mut bindings_ctx, socket, Some(&MultipleDevicesId::A))
            .unwrap();
        SocketHandler::listen_udp(core_ctx, &mut bindings_ctx, socket, None, Some(LOCAL_PORT))
            .expect("listen failed");

        // Since it is bound, it does not receive a packet from another device.
        receive_packet_on::<I>(core_ctx, &mut bindings_ctx, MultipleDevicesId::B);
        let received = &bindings_ctx.state().socket_data::<I>();
        assert_eq!(received, &HashMap::new());

        // When unbound, the socket can receive packets on the other device.
        SocketHandler::set_device(core_ctx, &mut bindings_ctx, socket, None)
            .expect("clearing bound device failed");
        receive_packet_on::<I>(core_ctx, &mut bindings_ctx, MultipleDevicesId::B);
        let received = bindings_ctx.state().received::<I>().iter().collect::<Vec<_>>();
        let (rx_socket, socket_received) =
            assert_matches!(received[..], [(rx_socket, packets)] => (rx_socket, packets));
        assert_eq!(rx_socket, &socket);
        assert_matches!(socket_received.packets[..], [_]);
    }

    /// Check that bind fails as expected when it would cause illegal shadowing.
    #[ip_test]
    fn test_unbind_device_fails<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(
                UdpMultipleDevicesCoreCtx::new_multiple_devices::<I>(),
            );
        let core_ctx = &mut core_ctx;

        let bound_on_devices = MultipleDevicesId::all().map(|device| {
            let socket = SocketHandler::<I, _>::create_udp(core_ctx);
            // TODO(https://fxbug.dev/21198): Test against dual-stack sockets.
            if I::VERSION == net_types::ip::IpVersion::V6 {
                SocketHandler::set_dual_stack_enabled(core_ctx, &mut bindings_ctx, socket, false)
                    .expect("disabling dual stack should succeed")
            }
            SocketHandler::set_device(core_ctx, &mut bindings_ctx, socket, Some(&device)).unwrap();
            SocketHandler::listen_udp(core_ctx, &mut bindings_ctx, socket, None, Some(LOCAL_PORT))
                .expect("listen should succeed");
            socket
        });

        // Clearing the bound device is not allowed for either socket since it
        // would then be shadowed by the other socket.
        for socket in bound_on_devices {
            assert_matches!(
                SocketHandler::set_device(core_ctx, &mut bindings_ctx, socket, None),
                Err(SocketError::Local(LocalAddressError::AddressInUse))
            );
        }
    }

    /// Check that binding a device fails if it would make a connected socket
    /// unroutable.
    #[ip_test]
    fn test_bind_conn_socket_device_fails<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let device_configs = HashMap::from(
            [(MultipleDevicesId::A, 1), (MultipleDevicesId::B, 2)].map(|(device, i)| {
                (
                    device,
                    FakeDeviceConfig {
                        device,
                        local_ips: vec![I::get_other_ip_address(i)],
                        remote_ips: vec![I::get_other_remote_ip_address(i)],
                    },
                )
            }),
        );
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(UdpMultipleDevicesCoreCtx::with_state(
                FakeDualStackIpSocketCtx::new(device_configs.iter().map(|(_, v)| v).cloned()),
            ));
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(device_configs[&MultipleDevicesId::A].remote_ips[0]).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect should succeed");

        // `socket` is not explicitly bound to device `A` but its route must
        // go through it because of the destination address. Therefore binding
        // to device `B` wil not work.
        assert_matches!(
            SocketHandler::set_device(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(&MultipleDevicesId::B)
            ),
            Err(SocketError::Remote(RemoteAddressError::NoRoute))
        );

        // Binding to device `A` should be fine.
        SocketHandler::set_device(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(&MultipleDevicesId::A),
        )
        .expect("routing picked A already");
    }

    #[ip_test]
    fn test_bound_device_receive_multicast_packet<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let remote_ip = I::get_other_ip_address(1);
        let multicast_addr = I::get_multicast_addr(0);

        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(
                UdpMultipleDevicesCoreCtx::new_multiple_devices::<I>(),
            );

        // Create 3 sockets: one listener bound on each device and one not bound
        // to a device.

        let core_ctx = &mut core_ctx;
        let bound_on_devices = MultipleDevicesId::all().map(|device| {
            let listener = SocketHandler::<I, _>::create_udp(core_ctx);
            SocketHandler::set_device(core_ctx, &mut bindings_ctx, listener, Some(&device))
                .unwrap();
            SocketHandler::set_udp_posix_reuse_port(core_ctx, &mut bindings_ctx, listener, true)
                .expect("is unbound");
            SocketHandler::listen_udp(
                core_ctx,
                &mut bindings_ctx,
                listener,
                None,
                Some(LOCAL_PORT),
            )
            .expect("listen should succeed");

            (device, listener)
        });

        let listener = SocketHandler::create_udp(core_ctx);
        SocketHandler::set_udp_posix_reuse_port(core_ctx, &mut bindings_ctx, listener, true)
            .expect("is unbound");
        SocketHandler::listen_udp(core_ctx, &mut bindings_ctx, listener, None, Some(LOCAL_PORT))
            .expect("listen should succeed");

        fn index_for_device(id: MultipleDevicesId) -> u8 {
            match id {
                MultipleDevicesId::A => 0,
                MultipleDevicesId::B => 1,
                MultipleDevicesId::C => 2,
            }
        }

        let mut receive_packet = |remote_ip: SpecifiedAddr<I::Addr>, device: MultipleDevicesId| {
            let body = vec![index_for_device(device)];
            receive_udp_packet(
                core_ctx,
                &mut bindings_ctx,
                device,
                remote_ip.get(),
                multicast_addr.get(),
                REMOTE_PORT,
                LOCAL_PORT,
                &body,
            )
        };

        // Receive packets from the remote IP on each device (2 packets total).
        // Listeners bound on devices should receive one, and the other listener
        // should receive both.
        for device in MultipleDevicesId::all() {
            receive_packet(remote_ip, device);
        }

        let per_socket_data = bindings_ctx.state().socket_data();
        for (device, listener) in bound_on_devices {
            assert_eq!(per_socket_data[&listener], vec![&[index_for_device(device)]]);
        }
        let expected_listener_data = &MultipleDevicesId::all().map(|d| vec![index_for_device(d)]);
        assert_eq!(&per_socket_data[&listener], expected_listener_data);
    }

    /// Tests establishing a UDP connection without providing a local IP
    #[ip_test]
    fn test_conn_unspecified_local_ip<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(&mut core_ctx, &mut bindings_ctx, socket, None, Some(LOCAL_PORT))
            .expect("listen_udp failed");
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip::<I>()).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect failed");
        let info = SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket);
        assert_eq!(
            info,
            SocketInfo::Connected(ConnInfo {
                local_ip: ZonedAddr::Unzoned(local_ip::<I>()).into(),
                local_port: LOCAL_PORT,
                remote_ip: ZonedAddr::Unzoned(remote_ip::<I>()).into(),
                remote_port: REMOTE_PORT.into(),
            })
        );
    }

    /// Tests local port allocation for [`connect`].
    ///
    /// Tests that calling [`connect`] causes a valid local port to be
    /// allocated.
    #[ip_test]
    fn test_udp_local_port_alloc<I: Ip + TestIpExt>() {
        let local_ip = local_ip::<I>();
        let ip_a = I::get_other_ip_address(100);
        let ip_b = I::get_other_ip_address(200);

        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } = UdpFakeDeviceCtx::with_core_ctx(
            UdpFakeDeviceCoreCtx::with_local_remote_ip_addrs(vec![local_ip], vec![ip_a, ip_b]),
        );

        let conn_a = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            conn_a,
            Some(ZonedAddr::Unzoned(ip_a).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect failed");
        let conn_b = SocketHandler::create_udp(&mut core_ctx);
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            conn_b,
            Some(ZonedAddr::Unzoned(ip_b).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect failed");
        let conn_c = SocketHandler::create_udp(&mut core_ctx);
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            conn_c,
            Some(ZonedAddr::Unzoned(ip_a).into()),
            OTHER_REMOTE_PORT.into(),
        )
        .expect("connect failed");
        let conn_d = SocketHandler::create_udp(&mut core_ctx);
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            conn_d,
            Some(ZonedAddr::Unzoned(ip_a).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect failed");
        let valid_range = &UdpBoundSocketMap::<I, FakeWeakDeviceId<FakeDeviceId>>::EPHEMERAL_RANGE;
        let mut get_conn_port = |id| {
            let info = SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, id);
            let info = assert_matches!(info, SocketInfo::Connected(info) => info);
            let ConnInfo { local_ip: _, local_port, remote_ip: _, remote_port: _ } = info;
            local_port
        };
        let port_a = get_conn_port(conn_a).get();
        let port_b = get_conn_port(conn_b).get();
        let port_c = get_conn_port(conn_c).get();
        let port_d = get_conn_port(conn_d).get();
        assert!(valid_range.contains(&port_a));
        assert!(valid_range.contains(&port_b));
        assert!(valid_range.contains(&port_c));
        assert!(valid_range.contains(&port_d));
        assert_ne!(port_a, port_b);
        assert_ne!(port_a, port_c);
        assert_ne!(port_a, port_d);
    }

    /// Tests that if `listen_udp` fails, it can be retried later.
    #[ip_test]
    fn test_udp_retry_listen_after_removing_conflict<I: Ip + TestIpExt>() {
        set_logger_for_test();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());

        fn listen_unbound<
            I: Ip + TestIpExt,
            BC: UdpStateBindingsContext<I, CC::DeviceId>,
            CC: StateContext<I, BC> + CounterContext<UdpCounters<I>>,
        >(
            core_ctx: &mut CC,
            bindings_ctx: &mut BC,
            socket: SocketId<I>,
        ) -> Result<(), Either<ExpectedUnboundError, LocalAddressError>> {
            SocketHandler::listen_udp(
                core_ctx,
                bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(local_ip::<I>()).into()),
                Some(LOCAL_PORT),
            )
        }

        // Tie up the address so the second call to `connect` fails.
        let listener = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        listen_unbound(&mut core_ctx, &mut bindings_ctx, listener)
            .expect("Initial call to listen_udp was expected to succeed");

        // Trying to connect on the same address should fail.
        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        assert_eq!(
            listen_unbound(&mut core_ctx, &mut bindings_ctx, unbound),
            Err(Either::Right(LocalAddressError::AddressInUse))
        );

        // Once the first listener is removed, the second socket can be
        // connected.
        let _: SocketInfo<I::Addr, _> =
            SocketHandler::close(&mut core_ctx, &mut bindings_ctx, listener);

        listen_unbound(&mut core_ctx, &mut bindings_ctx, unbound).expect("listen should succeed");
    }

    /// Tests local port allocation for [`listen_udp`].
    ///
    /// Tests that calling [`listen_udp`] causes a valid local port to be
    /// allocated when no local port is passed.
    #[ip_test]
    fn test_udp_listen_port_alloc<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let local_ip = local_ip::<I>();

        let wildcard_list = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(&mut core_ctx, &mut bindings_ctx, wildcard_list, None, None)
            .expect("listen_udp failed");
        let specified_list = SocketHandler::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            specified_list,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            None,
        )
        .expect("listen_udp failed");

        let mut get_listener_port = |id| {
            let info = SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, id);
            let info = assert_matches!(info, SocketInfo::Listener(info) => info);
            let ListenerInfo { local_ip: _, local_port } = info;
            local_port
        };
        let wildcard_port = get_listener_port(wildcard_list);
        let specified_port = get_listener_port(specified_list);
        assert!(UdpBoundSocketMap::<I, FakeWeakDeviceId<FakeDeviceId>>::EPHEMERAL_RANGE
            .contains(&wildcard_port.get()));
        assert!(UdpBoundSocketMap::<I, FakeWeakDeviceId<FakeDeviceId>>::EPHEMERAL_RANGE
            .contains(&specified_port.get()));
        assert_ne!(wildcard_port, specified_port);
    }

    #[ip_test]
    fn test_bind_multiple_reuse_port<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let listeners = [(), ()].map(|()| {
            let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
            SocketHandler::set_udp_posix_reuse_port(&mut core_ctx, &mut bindings_ctx, socket, true)
                .expect("is unbound");
            SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                None,
                Some(LOCAL_PORT),
            )
            .expect("listen_udp failed");
            socket
        });

        for listener in listeners {
            assert_eq!(
                SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, listener),
                SocketInfo::Listener(ListenerInfo { local_ip: None, local_port: LOCAL_PORT })
            );
        }
    }

    #[ip_test]
    fn test_set_unset_reuse_port_unbound<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let _listener = {
            let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
            SocketHandler::set_udp_posix_reuse_port(
                &mut core_ctx,
                &mut bindings_ctx,
                unbound,
                true,
            )
            .expect("is unbound");
            SocketHandler::set_udp_posix_reuse_port(
                &mut core_ctx,
                &mut bindings_ctx,
                unbound,
                false,
            )
            .expect("is unbound");
            SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                unbound,
                None,
                Some(LOCAL_PORT),
            )
            .expect("listen_udp failed")
        };

        // Because there is already a listener bound without `SO_REUSEPORT` set,
        // the next bind to the same address should fail.
        assert_eq!(
            {
                let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
                SocketHandler::listen_udp(
                    &mut core_ctx,
                    &mut bindings_ctx,
                    unbound,
                    None,
                    Some(LOCAL_PORT),
                )
            },
            Err(Either::Right(LocalAddressError::AddressInUse))
        );
    }

    #[ip_test]
    #[test_case(bind_as_listener)]
    #[test_case(bind_as_connected)]
    fn test_set_unset_reuse_port_bound<I: Ip + TestIpExt>(
        set_up_socket: impl FnOnce(
            &mut UdpMultipleDevicesCoreCtx,
            &mut UdpFakeDeviceBindingsCtx,
            SocketId<I>,
        ),
    ) {
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(
                UdpMultipleDevicesCoreCtx::new_multiple_devices::<I>(),
            );
        let socket = SocketHandler::create_udp(&mut core_ctx);
        set_up_socket(&mut core_ctx, &mut bindings_ctx, socket);

        // Per src/connectivity/network/netstack3/docs/POSIX_COMPATIBILITY.md,
        // Netstack3 only allows setting SO_REUSEPORT on unbound sockets.
        assert_matches!(
            SocketHandler::set_udp_posix_reuse_port(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                false,
            ),
            Err(ExpectedUnboundError)
        )
    }

    /// Tests [`remove_udp`]
    #[ip_test]
    fn test_remove_udp_conn<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let local_ip = ZonedAddr::Unzoned(local_ip::<I>()).into();
        let remote_ip = ZonedAddr::Unzoned(remote_ip::<I>()).into();
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(local_ip),
            Some(LOCAL_PORT),
        )
        .unwrap();
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(remote_ip),
            REMOTE_PORT.into(),
        )
        .expect("connect failed");
        let info = SocketHandler::close(&mut core_ctx, &mut bindings_ctx, socket);
        let info = assert_matches!(info, SocketInfo::Connected(info) => info);
        // Assert that the info gotten back matches what was expected.
        assert_eq!(info.local_ip, local_ip.into_inner().map_zone(FakeWeakDeviceId).into());
        assert_eq!(info.local_port, LOCAL_PORT);
        assert_eq!(info.remote_ip, remote_ip.into_inner().map_zone(FakeWeakDeviceId).into());
        assert_eq!(info.remote_port, REMOTE_PORT.into());

        // Assert that that connection id was removed from the connections
        // state.
        let Wrapped { outer: sockets_state, inner: _ } = &core_ctx;
        assert_matches!(sockets_state.as_ref().get_socket_state(&socket), None);
    }

    /// Tests [`remove_udp`]
    #[ip_test]
    fn test_remove_udp_listener<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let local_ip = ZonedAddr::Unzoned(local_ip::<I>()).into();

        // Test removing a specified listener.
        let specified = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            specified,
            Some(local_ip),
            Some(LOCAL_PORT),
        )
        .expect("listen_udp failed");
        let info = SocketHandler::close(&mut core_ctx, &mut bindings_ctx, specified);
        let info = assert_matches!(info, SocketInfo::Listener(info) => info);
        assert_eq!(info.local_ip.unwrap(), local_ip.into_inner().map_zone(FakeWeakDeviceId).into());
        assert_eq!(info.local_port, LOCAL_PORT);
        let Wrapped { outer: sockets_state, inner: _ } = &core_ctx;
        assert_matches!(sockets_state.as_ref().get_socket_state(&specified), None);

        // Test removing a wildcard listener.
        let wildcard = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            wildcard,
            None,
            Some(LOCAL_PORT),
        )
        .expect("listen_udp failed");
        let info = SocketHandler::close(&mut core_ctx, &mut bindings_ctx, wildcard);
        let info = assert_matches!(info, SocketInfo::Listener(info) => info);
        assert_eq!(info.local_ip, None);
        assert_eq!(info.local_port, LOCAL_PORT);
        let Wrapped { outer: sockets_state, inner: _ } = &core_ctx;
        assert_matches!(sockets_state.as_ref().get_socket_state(&wildcard), None);
    }

    fn try_join_leave_multicast<I: Ip + TestIpExt>(
        mcast_addr: MulticastAddr<I::Addr>,
        interface: MulticastMembershipInterfaceSelector<I::Addr, MultipleDevicesId>,
        set_up_socket: impl FnOnce(
            &mut UdpMultipleDevicesCoreCtx,
            &mut UdpFakeDeviceBindingsCtx,
            SocketId<I>,
        ),
    ) -> (
        Result<(), SetMulticastMembershipError>,
        HashMap<(MultipleDevicesId, MulticastAddr<I::Addr>), NonZeroUsize>,
    ) {
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(
                UdpMultipleDevicesCoreCtx::new_multiple_devices::<I>(),
            );

        let socket = SocketHandler::create_udp(&mut core_ctx);
        set_up_socket(&mut core_ctx, &mut bindings_ctx, socket);
        let result = SocketHandler::set_udp_multicast_membership(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            mcast_addr,
            interface,
            true,
        );

        let memberships_snapshot = core_ctx.inner.inner.get_ref().multicast_memberships::<I>();
        if let Ok(()) = result {
            SocketHandler::set_udp_multicast_membership(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                mcast_addr,
                interface,
                false,
            )
            .expect("leaving group failed");
        }
        assert_eq!(core_ctx.inner.inner.get_ref().multicast_memberships::<I>(), HashMap::default());

        (result, memberships_snapshot)
    }

    fn leave_unbound<I: TestIpExt>(
        _core_ctx: &mut UdpMultipleDevicesCoreCtx,
        _bindings_ctx: &mut UdpFakeDeviceBindingsCtx,
        _unbound: SocketId<I>,
    ) {
    }

    fn bind_as_listener<I: TestIpExt>(
        core_ctx: &mut UdpMultipleDevicesCoreCtx,
        bindings_ctx: &mut UdpFakeDeviceBindingsCtx,
        unbound: SocketId<I>,
    ) {
        SocketHandler::listen_udp(
            core_ctx,
            bindings_ctx,
            unbound,
            Some(ZonedAddr::Unzoned(local_ip::<I>()).into()),
            Some(LOCAL_PORT),
        )
        .expect("listen should succeed")
    }

    fn bind_as_connected<I: TestIpExt>(
        core_ctx: &mut UdpMultipleDevicesCoreCtx,
        bindings_ctx: &mut UdpFakeDeviceBindingsCtx,
        unbound: SocketId<I>,
    ) {
        SocketHandler::connect(
            core_ctx,
            bindings_ctx,
            unbound,
            Some(ZonedAddr::Unzoned(I::get_other_remote_ip_address(1)).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect should succeed")
    }

    fn iface_id<A: IpAddress>(
        id: MultipleDevicesId,
    ) -> MulticastInterfaceSelector<A, MultipleDevicesId> {
        MulticastInterfaceSelector::Interface(id)
    }
    fn iface_addr<A: IpAddress>(
        addr: SpecifiedAddr<A>,
    ) -> MulticastInterfaceSelector<A, MultipleDevicesId> {
        MulticastInterfaceSelector::LocalAddress(addr)
    }

    #[ip_test]
    #[test_case(iface_id(MultipleDevicesId::A), leave_unbound::<I>; "device_no_addr_unbound")]
    #[test_case(iface_addr(local_ip::<I>()), leave_unbound::<I>; "addr_no_device_unbound")]
    #[test_case(iface_id(MultipleDevicesId::A), bind_as_listener::<I>; "device_no_addr_listener")]
    #[test_case(iface_addr(local_ip::<I>()), bind_as_listener::<I>; "addr_no_device_listener")]
    #[test_case(iface_id(MultipleDevicesId::A), bind_as_connected::<I>; "device_no_addr_connected")]
    #[test_case(iface_addr(local_ip::<I>()), bind_as_connected::<I>; "addr_no_device_connected")]
    fn test_join_leave_multicast_succeeds<I: Ip + TestIpExt>(
        interface: MulticastInterfaceSelector<I::Addr, MultipleDevicesId>,
        set_up_socket: impl FnOnce(
            &mut UdpMultipleDevicesCoreCtx,
            &mut UdpFakeDeviceBindingsCtx,
            SocketId<I>,
        ),
    ) {
        let mcast_addr = I::get_multicast_addr(3);

        let (result, ip_options) =
            try_join_leave_multicast(mcast_addr, interface.into(), set_up_socket);
        assert_eq!(result, Ok(()));
        assert_eq!(
            ip_options,
            HashMap::from([(
                (MultipleDevicesId::A, mcast_addr),
                const_unwrap_option(NonZeroUsize::new(1))
            )])
        );
    }

    #[ip_test]
    #[test_case(MultipleDevicesId::A, Some(local_ip::<I>()), leave_unbound, Ok(());
        "with_ip_unbound")]
    #[test_case(MultipleDevicesId::A, None, leave_unbound, Ok(());
        "without_ip_unbound")]
    #[test_case(MultipleDevicesId::A, Some(local_ip::<I>()), bind_as_listener, Ok(());
        "with_ip_listener")]
    #[test_case(MultipleDevicesId::A, Some(local_ip::<I>()), bind_as_connected, Ok(());
        "with_ip_connected")]
    fn test_join_leave_multicast_interface_inferred_from_bound_device<I: Ip + TestIpExt>(
        bound_device: MultipleDevicesId,
        interface_addr: Option<SpecifiedAddr<I::Addr>>,
        set_up_socket: impl FnOnce(
            &mut UdpMultipleDevicesCoreCtx,
            &mut UdpFakeDeviceBindingsCtx,
            SocketId<I>,
        ),
        expected_result: Result<(), SetMulticastMembershipError>,
    ) {
        let mcast_addr = I::get_multicast_addr(3);
        let (result, ip_options) = try_join_leave_multicast(
            mcast_addr,
            interface_addr
                .map(MulticastInterfaceSelector::LocalAddress)
                .map(Into::into)
                .unwrap_or(MulticastMembershipInterfaceSelector::AnyInterfaceWithRoute),
            |core_ctx, bindings_ctx, unbound| {
                SocketHandler::set_device(core_ctx, bindings_ctx, unbound, Some(&bound_device))
                    .unwrap();
                set_up_socket(core_ctx, bindings_ctx, unbound)
            },
        );
        assert_eq!(result, expected_result);
        assert_eq!(
            ip_options,
            expected_result.map_or(HashMap::default(), |()| HashMap::from([(
                (bound_device, mcast_addr),
                const_unwrap_option(NonZeroUsize::new(1))
            )]))
        );
    }

    #[ip_test]
    fn test_multicast_membership_with_removed_device<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());

        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::set_device(&mut core_ctx, &mut bindings_ctx, unbound, Some(&FakeDeviceId))
            .unwrap();

        set_device_removed::<I>(&mut core_ctx, true);

        let group = I::get_multicast_addr(4);
        assert_eq!(
            SocketHandler::set_udp_multicast_membership(
                &mut core_ctx,
                &mut bindings_ctx,
                unbound,
                group,
                // Will use the socket's bound device.
                MulticastMembershipInterfaceSelector::AnyInterfaceWithRoute,
                true,
            ),
            Err(SetMulticastMembershipError::DeviceDoesNotExist),
        );

        // Should not have updated the device's multicast state.
        //
        // Note that even though we mock the device being removed above, its
        // state still exists in the fake IP socket context so we can inspect
        // it here.
        assert_eq!(core_ctx.inner.inner.get_ref().multicast_memberships::<I>(), HashMap::default(),);
    }

    #[ip_test]
    fn test_remove_udp_unbound_leaves_multicast_groups<I: Ip + TestIpExt>() {
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(
                UdpMultipleDevicesCoreCtx::new_multiple_devices::<I>(),
            );

        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let group = I::get_multicast_addr(4);
        SocketHandler::set_udp_multicast_membership(
            &mut core_ctx,
            &mut bindings_ctx,
            unbound,
            group,
            MulticastInterfaceSelector::LocalAddress(local_ip::<I>()).into(),
            true,
        )
        .expect("join group failed");

        assert_eq!(
            core_ctx.inner.inner.get_ref().multicast_memberships::<I>(),
            HashMap::from([(
                (MultipleDevicesId::A, group),
                const_unwrap_option(NonZeroUsize::new(1))
            )])
        );

        let _: SocketInfo<I::Addr, _> =
            SocketHandler::close(&mut core_ctx, &mut bindings_ctx, unbound);
        assert_eq!(core_ctx.inner.inner.get_ref().multicast_memberships::<I>(), HashMap::default());
    }

    #[ip_test]
    fn test_remove_udp_listener_leaves_multicast_groups<I: Ip + TestIpExt>() {
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(
                UdpMultipleDevicesCoreCtx::new_multiple_devices::<I>(),
            );
        let local_ip = local_ip::<I>();

        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let first_group = I::get_multicast_addr(4);
        SocketHandler::set_udp_multicast_membership(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            first_group,
            MulticastInterfaceSelector::LocalAddress(local_ip).into(),
            true,
        )
        .expect("join group failed");

        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            Some(LOCAL_PORT),
        )
        .expect("listen_udp failed");
        let second_group = I::get_multicast_addr(5);
        SocketHandler::set_udp_multicast_membership(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            second_group,
            MulticastInterfaceSelector::LocalAddress(local_ip).into(),
            true,
        )
        .expect("join group failed");

        assert_eq!(
            core_ctx.inner.inner.get_ref().multicast_memberships::<I>(),
            HashMap::from([
                ((MultipleDevicesId::A, first_group), const_unwrap_option(NonZeroUsize::new(1))),
                ((MultipleDevicesId::A, second_group), const_unwrap_option(NonZeroUsize::new(1)))
            ])
        );

        let _: SocketInfo<I::Addr, _> =
            SocketHandler::close(&mut core_ctx, &mut bindings_ctx, socket);
        assert_eq!(core_ctx.inner.inner.get_ref().multicast_memberships::<I>(), HashMap::default());
    }

    #[ip_test]
    fn test_remove_udp_connected_leaves_multicast_groups<I: Ip + TestIpExt>() {
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(
                UdpMultipleDevicesCoreCtx::new_multiple_devices::<I>(),
            );
        let local_ip = local_ip::<I>();

        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let first_group = I::get_multicast_addr(4);
        SocketHandler::set_udp_multicast_membership(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            first_group,
            MulticastInterfaceSelector::LocalAddress(local_ip).into(),
            true,
        )
        .expect("join group failed");

        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(I::get_other_remote_ip_address(1)).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect failed");

        let second_group = I::get_multicast_addr(5);
        SocketHandler::set_udp_multicast_membership(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            second_group,
            MulticastInterfaceSelector::LocalAddress(local_ip).into(),
            true,
        )
        .expect("join group failed");

        assert_eq!(
            core_ctx.inner.inner.get_ref().multicast_memberships::<I>(),
            HashMap::from([
                ((MultipleDevicesId::A, first_group), const_unwrap_option(NonZeroUsize::new(1))),
                ((MultipleDevicesId::A, second_group), const_unwrap_option(NonZeroUsize::new(1)))
            ])
        );

        let _: SocketInfo<I::Addr, _> =
            SocketHandler::close(&mut core_ctx, &mut bindings_ctx, socket);
        assert_eq!(core_ctx.inner.inner.get_ref().multicast_memberships::<I>(), HashMap::default());
    }

    #[ip_test]
    #[should_panic(expected = "listen again failed")]
    fn test_listen_udp_removes_unbound<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let local_ip = local_ip::<I>();
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);

        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            Some(LOCAL_PORT),
        )
        .expect("listen_udp failed");

        // Attempting to create a new listener from the same unbound ID should
        // panic since the unbound socket ID is now invalid.
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            Some(OTHER_LOCAL_PORT),
        )
        .expect("listen again failed");
    }

    #[ip_test]
    fn test_get_conn_info<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let local_ip = ZonedAddr::Unzoned(local_ip::<I>()).into();
        let remote_ip = ZonedAddr::Unzoned(remote_ip::<I>()).into();
        // Create a UDP connection with a specified local port and local IP.
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(local_ip),
            Some(LOCAL_PORT),
        )
        .expect("listen_udp failed");
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(remote_ip),
            REMOTE_PORT.into(),
        )
        .expect("connect failed");
        let info = SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket);
        let info = assert_matches!(info, SocketInfo::Connected(info) => info);
        assert_eq!(info.local_ip, local_ip.into_inner().map_zone(FakeWeakDeviceId).into());
        assert_eq!(info.local_port, LOCAL_PORT);
        assert_eq!(info.remote_ip, remote_ip.into_inner().map_zone(FakeWeakDeviceId).into());
        assert_eq!(info.remote_port, REMOTE_PORT.into());
    }

    #[ip_test]
    fn test_get_listener_info<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let local_ip = ZonedAddr::Unzoned(local_ip::<I>()).into();

        // Check getting info on specified listener.
        let specified = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            specified,
            Some(local_ip),
            Some(LOCAL_PORT),
        )
        .expect("listen_udp failed");
        let info = SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, specified);
        let info = assert_matches!(info, SocketInfo::Listener(info) => info);
        assert_eq!(info.local_ip.unwrap(), local_ip.into_inner().map_zone(FakeWeakDeviceId).into());
        assert_eq!(info.local_port, LOCAL_PORT);

        // Check getting info on wildcard listener.
        let wildcard = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            wildcard,
            None,
            Some(OTHER_LOCAL_PORT),
        )
        .expect("listen_udp failed");
        let info = SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, wildcard);
        let info = assert_matches!(info, SocketInfo::Listener(info) => info);
        assert_eq!(info.local_ip, None);
        assert_eq!(info.local_port, OTHER_LOCAL_PORT);
    }

    #[ip_test]
    fn test_get_reuse_port<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let first = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        assert_eq!(
            SocketHandler::get_udp_posix_reuse_port(&mut core_ctx, &bindings_ctx, first),
            false,
        );

        SocketHandler::set_udp_posix_reuse_port(&mut core_ctx, &mut bindings_ctx, first, true)
            .expect("is unbound");

        assert_eq!(
            SocketHandler::get_udp_posix_reuse_port(&mut core_ctx, &bindings_ctx, first),
            true
        );

        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            first,
            Some(ZonedAddr::Unzoned(local_ip::<I>()).into()),
            None,
        )
        .expect("listen failed");
        assert_eq!(
            SocketHandler::get_udp_posix_reuse_port(&mut core_ctx, &bindings_ctx, first),
            true
        );
        let _: SocketInfo<_, _> = SocketHandler::close(&mut core_ctx, &mut bindings_ctx, first);

        let second = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::set_udp_posix_reuse_port(&mut core_ctx, &mut bindings_ctx, second, true)
            .expect("is unbound");
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            second,
            Some(ZonedAddr::Unzoned(remote_ip::<I>()).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect failed");

        assert_eq!(
            SocketHandler::get_udp_posix_reuse_port(&mut core_ctx, &bindings_ctx, second),
            true
        );
    }

    #[ip_test]
    fn test_get_bound_device_unbound<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);

        assert_eq!(
            SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, unbound),
            None
        );

        SocketHandler::set_device(&mut core_ctx, &mut bindings_ctx, unbound, Some(&FakeDeviceId))
            .unwrap();
        assert_eq!(
            SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, unbound),
            Some(FakeWeakDeviceId(FakeDeviceId))
        );
    }

    #[ip_test]
    fn test_get_bound_device_listener<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);

        SocketHandler::set_device(&mut core_ctx, &mut bindings_ctx, socket, Some(&FakeDeviceId))
            .unwrap();
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(local_ip::<I>()).into()),
            Some(LOCAL_PORT),
        )
        .expect("failed to listen");
        assert_eq!(
            SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, socket),
            Some(FakeWeakDeviceId(FakeDeviceId))
        );

        SocketHandler::set_device(&mut core_ctx, &mut bindings_ctx, socket, None)
            .expect("failed to set device");
        assert_eq!(SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, socket), None);
    }

    #[ip_test]
    fn test_get_bound_device_connected<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);

        SocketHandler::set_device(&mut core_ctx, &mut bindings_ctx, socket, Some(&FakeDeviceId))
            .unwrap();
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip::<I>()).into()),
            REMOTE_PORT.into(),
        )
        .expect("failed to connect");
        assert_eq!(
            SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, socket),
            Some(FakeWeakDeviceId(FakeDeviceId))
        );
        SocketHandler::set_device(&mut core_ctx, &mut bindings_ctx, socket, None)
            .expect("failed to set device");
        assert_eq!(SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, socket), None);
    }

    #[ip_test]
    fn test_listen_udp_forwards_errors<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let remote_ip = remote_ip::<I>();

        // Check listening to a non-local IP fails.
        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let listen_err = SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            unbound,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            Some(LOCAL_PORT),
        )
        .expect_err("listen_udp unexpectedly succeeded");
        assert_eq!(listen_err, Either::Right(LocalAddressError::CannotBindToAddress));

        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let _ = SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            unbound,
            None,
            Some(OTHER_LOCAL_PORT),
        )
        .expect("listen_udp failed");
        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let listen_err = SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            unbound,
            None,
            Some(OTHER_LOCAL_PORT),
        )
        .expect_err("listen_udp unexpectedly succeeded");
        assert_eq!(listen_err, Either::Right(LocalAddressError::AddressInUse));
    }

    const IPV6_LINK_LOCAL_ADDR: Ipv6Addr = net_ip_v6!("fe80::1234");
    #[test_case(IPV6_LINK_LOCAL_ADDR, IPV6_LINK_LOCAL_ADDR; "unicast")]
    #[test_case(IPV6_LINK_LOCAL_ADDR, MulticastAddr::new(net_ip_v6!("ff02::1234")).unwrap().get(); "multicast")]
    fn test_listen_udp_ipv6_link_local_requires_zone(
        interface_addr: Ipv6Addr,
        bind_addr: Ipv6Addr,
    ) {
        type I = Ipv6;
        let interface_addr = LinkLocalAddr::new(interface_addr).unwrap().into_specified();

        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::with_local_remote_ip_addrs(
                vec![interface_addr],
                vec![remote_ip::<I>()],
            ));

        let bind_addr = LinkLocalAddr::new(bind_addr).unwrap().into_specified();
        assert!(bind_addr.scope().can_have_zone());

        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let result = SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            unbound,
            Some(ZonedAddr::Unzoned(bind_addr).into()),
            Some(LOCAL_PORT),
        );
        assert_eq!(
            result,
            Err(Either::Right(LocalAddressError::Zone(ZonedAddressError::RequiredZoneNotProvided)))
        );
    }

    #[test_case(MultipleDevicesId::A, Ok(()); "matching")]
    #[test_case(MultipleDevicesId::B, Err(LocalAddressError::Zone(ZonedAddressError::DeviceZoneMismatch)); "not matching")]
    fn test_listen_udp_ipv6_link_local_with_bound_device_set(
        zone_id: MultipleDevicesId,
        expected_result: Result<(), LocalAddressError>,
    ) {
        type I = Ipv6;
        let ll_addr = LinkLocalAddr::new(net_ip_v6!("fe80::1234")).unwrap().into_specified();
        assert!(ll_addr.scope().can_have_zone());

        let remote_ips = vec![remote_ip::<I>()];
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(UdpMultipleDevicesCoreCtx::with_state(
                FakeDualStackIpSocketCtx::new(
                    [(MultipleDevicesId::A, ll_addr), (MultipleDevicesId::B, local_ip::<I>())].map(
                        |(device, local_ip)| FakeDeviceConfig {
                            device,
                            local_ips: vec![local_ip],
                            remote_ips: remote_ips.clone(),
                        },
                    ),
                ),
            ));

        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::set_device(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(&MultipleDevicesId::A),
        )
        .unwrap();

        let result = SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Zoned(AddrAndZone::new(ll_addr, zone_id).unwrap()).into()),
            Some(LOCAL_PORT),
        )
        .map_err(Either::unwrap_right);
        assert_eq!(result, expected_result);
    }

    #[test_case(MultipleDevicesId::A, Ok(()); "matching")]
    #[test_case(MultipleDevicesId::B, Err(LocalAddressError::AddressMismatch); "not matching")]
    fn test_listen_udp_ipv6_link_local_with_zone_requires_addr_assigned_to_device(
        zone_id: MultipleDevicesId,
        expected_result: Result<(), LocalAddressError>,
    ) {
        type I = Ipv6;
        let ll_addr = LinkLocalAddr::new(net_ip_v6!("fe80::1234")).unwrap().into_specified();
        assert!(ll_addr.scope().can_have_zone());

        let remote_ips = vec![remote_ip::<I>()];
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(UdpMultipleDevicesCoreCtx::with_state(
                FakeDualStackIpSocketCtx::new(
                    [(MultipleDevicesId::A, ll_addr), (MultipleDevicesId::B, local_ip::<I>())].map(
                        |(device, local_ip)| FakeDeviceConfig {
                            device,
                            local_ips: vec![local_ip],
                            remote_ips: remote_ips.clone(),
                        },
                    ),
                ),
            ));

        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let result = SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Zoned(AddrAndZone::new(ll_addr, zone_id).unwrap()).into()),
            Some(LOCAL_PORT),
        )
        .map_err(Either::unwrap_right);
        assert_eq!(result, expected_result);
    }

    #[test_case(None, Err(LocalAddressError::Zone(ZonedAddressError::DeviceZoneMismatch)); "clear device")]
    #[test_case(Some(MultipleDevicesId::A), Ok(()); "set same device")]
    #[test_case(Some(MultipleDevicesId::B),
                Err(LocalAddressError::Zone(ZonedAddressError::DeviceZoneMismatch)); "change device")]
    fn test_listen_udp_ipv6_listen_link_local_update_bound_device(
        new_device: Option<MultipleDevicesId>,
        expected_result: Result<(), LocalAddressError>,
    ) {
        type I = Ipv6;
        let ll_addr = LinkLocalAddr::new(net_ip_v6!("fe80::1234")).unwrap().into_specified();
        assert!(ll_addr.scope().can_have_zone());

        let remote_ips = vec![remote_ip::<I>()];
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(UdpMultipleDevicesCoreCtx::with_state(
                FakeDualStackIpSocketCtx::new(
                    [(MultipleDevicesId::A, ll_addr), (MultipleDevicesId::B, local_ip::<I>())].map(
                        |(device, local_ip)| FakeDeviceConfig {
                            device,
                            local_ips: vec![local_ip],
                            remote_ips: remote_ips.clone(),
                        },
                    ),
                ),
            ));

        let socket = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Zoned(AddrAndZone::new(ll_addr, MultipleDevicesId::A).unwrap()).into()),
            Some(LOCAL_PORT),
        )
        .expect("listen failed");

        assert_eq!(
            SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, socket),
            Some(FakeWeakDeviceId(MultipleDevicesId::A))
        );

        assert_eq!(
            SocketHandler::set_device(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                new_device.as_ref()
            ),
            expected_result.map_err(SocketError::Local),
        );
    }

    #[test_case(None; "bind all IPs")]
    #[test_case(Some(ZonedAddr::Unzoned(local_ip::<Ipv6>())); "bind unzoned")]
    #[test_case(Some(ZonedAddr::Zoned(AddrAndZone::new(SpecifiedAddr::new(net_ip_v6!("fe80::1")).unwrap(),
        MultipleDevicesId::A).unwrap())); "bind with same zone")]
    fn test_udp_ipv6_connect_with_unzoned(
        bound_addr: Option<ZonedAddr<SpecifiedAddr<Ipv6Addr>, MultipleDevicesId>>,
    ) {
        let remote_ips = vec![remote_ip::<Ipv6>()];

        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(UdpMultipleDevicesCoreCtx::with_state(
                FakeDualStackIpSocketCtx::new([
                    FakeDeviceConfig {
                        device: MultipleDevicesId::A,
                        local_ips: vec![
                            local_ip::<Ipv6>(),
                            SpecifiedAddr::new(net_ip_v6!("fe80::1")).unwrap(),
                        ],
                        remote_ips: remote_ips.clone(),
                    },
                    FakeDeviceConfig {
                        device: MultipleDevicesId::B,
                        local_ips: vec![SpecifiedAddr::new(net_ip_v6!("fe80::2")).unwrap()],
                        remote_ips: remote_ips,
                    },
                ]),
            ));

        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);

        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            bound_addr.map(SocketZonedIpAddr::from),
            Some(LOCAL_PORT),
        )
        .unwrap();

        assert_matches!(
            SocketHandler::connect(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(remote_ip::<Ipv6>()).into()),
                REMOTE_PORT.into(),
            ),
            Ok(())
        );
    }

    #[test]
    fn test_udp_ipv6_connect_zoned_get_info() {
        let ll_addr = LinkLocalAddr::new(net_ip_v6!("fe80::1234")).unwrap().into_specified();
        assert!(socket::must_have_zone(&ll_addr));

        let remote_ips = vec![remote_ip::<Ipv6>()];
        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(UdpMultipleDevicesCoreCtx::with_state(
                FakeDualStackIpSocketCtx::new(
                    [(MultipleDevicesId::A, ll_addr), (MultipleDevicesId::B, local_ip::<Ipv6>())]
                        .map(|(device, local_ip)| FakeDeviceConfig {
                            device,
                            local_ips: vec![local_ip],
                            remote_ips: remote_ips.clone(),
                        }),
                ),
            ));

        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);
        SocketHandler::set_device(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(&MultipleDevicesId::A),
        )
        .unwrap();

        let zoned_local_addr =
            ZonedAddr::Zoned(AddrAndZone::new(ll_addr, MultipleDevicesId::A).unwrap()).into();
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(zoned_local_addr),
            Some(LOCAL_PORT),
        )
        .unwrap();

        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip::<Ipv6>()).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect should succeed");

        assert_eq!(
            SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket),
            SocketInfo::Connected(ConnInfo {
                local_ip: zoned_local_addr.into_inner().map_zone(FakeWeakDeviceId).into(),
                local_port: LOCAL_PORT,
                remote_ip: ZonedAddr::Unzoned(remote_ip::<Ipv6>()).into(),
                remote_port: REMOTE_PORT.into(),
            })
        );
    }

    #[test_case(ZonedAddr::Zoned(AddrAndZone::new(SpecifiedAddr::new(net_ip_v6!("fe80::2")).unwrap(),
        MultipleDevicesId::B).unwrap()),
        Err(ConnectError::Zone(ZonedAddressError::DeviceZoneMismatch));
        "connect to different zone")]
    #[test_case(ZonedAddr::Unzoned(SpecifiedAddr::new(net_ip_v6!("fe80::3")).unwrap()),
        Ok(FakeWeakDeviceId(MultipleDevicesId::A)); "connect implicit zone")]
    fn test_udp_ipv6_bind_zoned(
        remote_addr: ZonedAddr<SpecifiedAddr<Ipv6Addr>, MultipleDevicesId>,
        expected: Result<FakeWeakDeviceId<MultipleDevicesId>, ConnectError>,
    ) {
        let remote_ips = vec![SpecifiedAddr::new(net_ip_v6!("fe80::3")).unwrap()];

        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(UdpMultipleDevicesCoreCtx::with_state(
                FakeDualStackIpSocketCtx::new([
                    FakeDeviceConfig {
                        device: MultipleDevicesId::A,
                        local_ips: vec![SpecifiedAddr::new(net_ip_v6!("fe80::1")).unwrap()],
                        remote_ips: remote_ips.clone(),
                    },
                    FakeDeviceConfig {
                        device: MultipleDevicesId::B,
                        local_ips: vec![SpecifiedAddr::new(net_ip_v6!("fe80::2")).unwrap()],
                        remote_ips: remote_ips,
                    },
                ]),
            ));

        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);

        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(
                ZonedAddr::Zoned(
                    AddrAndZone::new(
                        SpecifiedAddr::new(net_ip_v6!("fe80::1")).unwrap(),
                        MultipleDevicesId::A,
                    )
                    .unwrap(),
                )
                .into(),
            ),
            Some(LOCAL_PORT),
        )
        .unwrap();

        let result = SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(remote_addr.into()),
            REMOTE_PORT.into(),
        )
        .map(|()| {
            SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, socket).unwrap()
        });
        assert_eq!(result, expected);
    }

    #[ip_test]
    fn test_listen_udp_loopback_no_zone_is_required<I: Ip + TestIpExt>() {
        let loopback_addr = I::LOOPBACK_ADDRESS;
        let remote_ips = vec![remote_ip::<I>()];

        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(UdpMultipleDevicesCoreCtx::with_state(
                FakeDualStackIpSocketCtx::new(
                    [
                        (MultipleDevicesId::A, loopback_addr),
                        (MultipleDevicesId::B, local_ip::<I>()),
                    ]
                    .map(|(device, local_ip)| FakeDeviceConfig {
                        device,
                        local_ips: vec![local_ip],
                        remote_ips: remote_ips.clone(),
                    }),
                ),
            ));

        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        SocketHandler::set_device(
            &mut core_ctx,
            &mut bindings_ctx,
            unbound,
            Some(&MultipleDevicesId::A),
        )
        .unwrap();

        let result = SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            unbound,
            Some(ZonedAddr::Unzoned(loopback_addr).into()),
            Some(LOCAL_PORT),
        );
        assert_matches!(result, Ok(_));
    }

    #[test_case(None, true, Ok(()); "connected success")]
    #[test_case(None, false, Ok(()); "listening success")]
    #[test_case(Some(MultipleDevicesId::A), true, Ok(()); "conn bind same device")]
    #[test_case(Some(MultipleDevicesId::A), false, Ok(()); "listen bind same device")]
    #[test_case(
        Some(MultipleDevicesId::B),
        true,
        Err(SendToError::Zone(ZonedAddressError::DeviceZoneMismatch));
        "conn bind different device")]
    #[test_case(
        Some(MultipleDevicesId::B),
        false,
        Err(SendToError::Zone(ZonedAddressError::DeviceZoneMismatch));
        "listen bind different device")]
    fn test_udp_ipv6_send_to_zoned(
        bind_device: Option<MultipleDevicesId>,
        connect: bool,
        expected: Result<(), SendToError>,
    ) {
        let ll_addr = LinkLocalAddr::new(net_ip_v6!("fe80::1234")).unwrap().into_specified();
        assert!(socket::must_have_zone(&ll_addr));
        let conn_remote_ip = Ipv6::get_other_remote_ip_address(1);

        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(UdpMultipleDevicesCoreCtx::with_state(
                FakeDualStackIpSocketCtx::new(
                    [
                        (MultipleDevicesId::A, Ipv6::get_other_ip_address(1)),
                        (MultipleDevicesId::B, Ipv6::get_other_ip_address(2)),
                    ]
                    .map(|(device, local_ip)| FakeDeviceConfig {
                        device,
                        local_ips: vec![local_ip],
                        remote_ips: vec![ll_addr, conn_remote_ip],
                    }),
                ),
            ));

        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);

        if let Some(device) = bind_device {
            SocketHandler::set_device(&mut core_ctx, &mut bindings_ctx, socket, Some(&device))
                .unwrap();
        }

        let send_to_remote_addr =
            ZonedAddr::Zoned(AddrAndZone::new(ll_addr, MultipleDevicesId::A).unwrap()).into();
        let result = if connect {
            SocketHandler::connect(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(conn_remote_ip).into()),
                REMOTE_PORT.into(),
            )
            .expect("connect should succeed");
            SocketHandler::send_to(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(send_to_remote_addr),
                REMOTE_PORT.into(),
                Buf::new(Vec::new(), ..),
            )
        } else {
            SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                None,
                Some(LOCAL_PORT),
            )
            .expect("listen should succeed");
            SocketHandler::send_to(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(send_to_remote_addr),
                REMOTE_PORT.into(),
                Buf::new(Vec::new(), ..),
            )
        };

        assert_eq!(result.map_err(|err| assert_matches!(err, Either::Right(e) => e)), expected);
    }

    #[test_case(true; "connected")]
    #[test_case(false; "listening")]
    fn test_udp_ipv6_bound_zoned_send_to_zoned(connect: bool) {
        let ll_addr = LinkLocalAddr::new(net_ip_v6!("fe80::5678")).unwrap().into_specified();
        let device_a_local_ip = net_ip_v6!("fe80::1111");
        let conn_remote_ip = Ipv6::get_other_remote_ip_address(1);

        let UdpMultipleDevicesCtx { mut core_ctx, mut bindings_ctx } =
            UdpMultipleDevicesCtx::with_core_ctx(UdpMultipleDevicesCoreCtx::with_state(
                FakeDualStackIpSocketCtx::new(
                    [
                        (MultipleDevicesId::A, device_a_local_ip),
                        (MultipleDevicesId::B, net_ip_v6!("fe80::2222")),
                    ]
                    .map(|(device, local_ip)| FakeDeviceConfig {
                        device,
                        local_ips: vec![LinkLocalAddr::new(local_ip).unwrap().into_specified()],
                        remote_ips: vec![ll_addr, conn_remote_ip],
                    }),
                ),
            ));

        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(
                ZonedAddr::Zoned(
                    AddrAndZone::new(
                        SpecifiedAddr::new(device_a_local_ip).unwrap(),
                        MultipleDevicesId::A,
                    )
                    .unwrap(),
                )
                .into(),
            ),
            Some(LOCAL_PORT),
        )
        .expect("listen should succeed");

        // Use a remote address on device B, while the socket is listening on
        // device A. This should cause a failure when sending.
        let send_to_remote_addr =
            ZonedAddr::Zoned(AddrAndZone::new(ll_addr, MultipleDevicesId::B).unwrap()).into();

        let result = if connect {
            SocketHandler::connect(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(conn_remote_ip).into()),
                REMOTE_PORT.into(),
            )
            .expect("connect should succeed");
            SocketHandler::send_to(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(send_to_remote_addr),
                REMOTE_PORT.into(),
                Buf::new(Vec::new(), ..),
            )
        } else {
            SocketHandler::send_to(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(send_to_remote_addr),
                REMOTE_PORT.into(),
                Buf::new(Vec::new(), ..),
            )
        };

        assert_matches!(
            result,
            Err(Either::Right(SendToError::Zone(ZonedAddressError::DeviceZoneMismatch)))
        );
    }

    #[test_case(None; "removes implicit")]
    #[test_case(Some(FakeDeviceId); "preserves implicit")]
    fn test_connect_disconnect_affects_bound_device(bind_device: Option<FakeDeviceId>) {
        // If a socket is bound to an unzoned address, whether or not it has a
        // bound device should be restored after `connect` then `disconnect`.
        let ll_addr = LinkLocalAddr::new(net_ip_v6!("fe80::1234")).unwrap().into_specified();
        assert!(socket::must_have_zone(&ll_addr));

        let local_ip = local_ip::<Ipv6>();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } = UdpFakeDeviceCtx::with_core_ctx(
            UdpFakeDeviceCoreCtx::with_local_remote_ip_addrs(vec![local_ip], vec![ll_addr]),
        );

        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);
        SocketHandler::set_device(&mut core_ctx, &mut bindings_ctx, socket, bind_device.as_ref())
            .unwrap();

        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            Some(LOCAL_PORT),
        )
        .unwrap();
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Zoned(AddrAndZone::new(ll_addr, FakeDeviceId).unwrap()).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect should succeed");

        assert_eq!(
            SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, socket),
            Some(FakeWeakDeviceId(FakeDeviceId))
        );

        SocketHandler::disconnect_connected(&mut core_ctx, &mut bindings_ctx, socket)
            .expect("was connected");

        assert_eq!(
            SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, socket),
            bind_device.map(FakeWeakDeviceId),
        );
    }

    #[test]
    fn test_bind_zoned_addr_connect_disconnect() {
        // If a socket is bound to a zoned address, the address's device should
        // be retained after `connect` then `disconnect`.
        let ll_addr = LinkLocalAddr::new(net_ip_v6!("fe80::1234")).unwrap().into_specified();
        assert!(socket::must_have_zone(&ll_addr));

        let remote_ip = remote_ip::<Ipv6>();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } = UdpFakeDeviceCtx::with_core_ctx(
            UdpFakeDeviceCoreCtx::with_local_remote_ip_addrs(vec![ll_addr], vec![remote_ip]),
        );

        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Zoned(AddrAndZone::new(ll_addr, FakeDeviceId).unwrap()).into()),
            Some(LOCAL_PORT),
        )
        .unwrap();
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(remote_ip).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect should succeed");

        assert_eq!(
            SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, socket),
            Some(FakeWeakDeviceId(FakeDeviceId))
        );

        SocketHandler::disconnect_connected(&mut core_ctx, &mut bindings_ctx, socket)
            .expect("was connected");
        assert_eq!(
            SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, socket),
            Some(FakeWeakDeviceId(FakeDeviceId))
        );
    }

    #[test]
    fn test_bind_device_after_connect_persists_after_disconnect() {
        // If a socket is bound to an unzoned address, connected to a zoned address, and then has
        // its device set, the device should be *retained* after `disconnect`.
        let ll_addr = LinkLocalAddr::new(net_ip_v6!("fe80::1234")).unwrap().into_specified();
        assert!(socket::must_have_zone(&ll_addr));

        let local_ip = local_ip::<Ipv6>();
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } = UdpFakeDeviceCtx::with_core_ctx(
            UdpFakeDeviceCoreCtx::with_local_remote_ip_addrs(vec![local_ip], vec![ll_addr]),
        );

        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Unzoned(local_ip).into()),
            Some(LOCAL_PORT),
        )
        .unwrap();
        SocketHandler::connect(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            Some(ZonedAddr::Zoned(AddrAndZone::new(ll_addr, FakeDeviceId).unwrap()).into()),
            REMOTE_PORT.into(),
        )
        .expect("connect should succeed");

        assert_eq!(
            SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, socket),
            Some(FakeWeakDeviceId(FakeDeviceId))
        );

        // This is a no-op functionally since the socket is already bound to the
        // device but it implies that we shouldn't unbind the device on
        // disconnect.
        SocketHandler::set_device(&mut core_ctx, &mut bindings_ctx, socket, Some(&FakeDeviceId))
            .expect("binding same device should succeed");

        SocketHandler::disconnect_connected(&mut core_ctx, &mut bindings_ctx, socket)
            .expect("was connected");
        assert_eq!(
            SocketHandler::get_udp_bound_device(&mut core_ctx, &bindings_ctx, socket),
            Some(FakeWeakDeviceId(FakeDeviceId))
        );
    }

    #[ip_test]
    fn test_remove_udp_unbound<I: Ip + TestIpExt>() {
        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::new_fake_device::<I>());
        let unbound = SocketHandler::<I, _>::create_udp(&mut core_ctx);
        let _: SocketInfo<_, _> = SocketHandler::close(&mut core_ctx, &mut bindings_ctx, unbound);

        let Wrapped { outer: sockets_state, inner: _ } = &core_ctx;
        assert_matches!(sockets_state.as_ref().get_socket_state(&unbound), None)
    }

    #[ip_test]
    fn test_hop_limits_used_for_sending_packets<I: Ip + TestIpExt>() {
        let some_multicast_addr: MulticastAddr<I::Addr> = I::map_ip(
            (),
            |()| Ipv4::ALL_SYSTEMS_MULTICAST_ADDRESS,
            |()| MulticastAddr::new(net_ip_v6!("ff0e::1")).unwrap(),
        );

        let UdpFakeDeviceCtx { mut core_ctx, mut bindings_ctx } =
            UdpFakeDeviceCtx::with_core_ctx(UdpFakeDeviceCoreCtx::with_local_remote_ip_addrs(
                vec![local_ip::<I>()],
                vec![remote_ip::<I>(), some_multicast_addr.into_specified()],
            ));
        let listener = SocketHandler::<I, _>::create_udp(&mut core_ctx);

        const UNICAST_HOPS: NonZeroU8 = const_unwrap_option(NonZeroU8::new(23));
        const MULTICAST_HOPS: NonZeroU8 = const_unwrap_option(NonZeroU8::new(98));
        SocketHandler::set_udp_unicast_hop_limit(
            &mut core_ctx,
            &mut bindings_ctx,
            listener,
            Some(UNICAST_HOPS),
        );
        SocketHandler::set_udp_multicast_hop_limit(
            &mut core_ctx,
            &mut bindings_ctx,
            listener,
            Some(MULTICAST_HOPS),
        );

        SocketHandler::listen_udp(&mut core_ctx, &mut bindings_ctx, listener, None, None)
            .expect("listen failed");

        let mut send_and_get_ttl = |remote_ip| {
            SocketHandler::send_to(
                &mut core_ctx,
                &mut bindings_ctx,
                listener,
                Some(ZonedAddr::Unzoned(remote_ip).into()),
                REMOTE_PORT.into(),
                Buf::new(vec![], ..),
            )
            .expect("send failed");

            let (meta, _body) = core_ctx.inner.inner.frames().last().unwrap();
            let SendIpPacketMeta {
                device: _,
                src_ip: _,
                dst_ip,
                next_hop: _,
                proto: _,
                ttl,
                mtu: _,
            } = meta.try_as::<I>().unwrap();
            assert_eq!(*dst_ip, remote_ip);
            *ttl
        };

        assert_eq!(send_and_get_ttl(some_multicast_addr.into_specified()), Some(MULTICAST_HOPS));
        assert_eq!(send_and_get_ttl(remote_ip::<I>()), Some(UNICAST_HOPS));
    }

    const DUAL_STACK_ANY_ADDR: Ipv6Addr = net_ip_v6!("::");
    const DUAL_STACK_V4_ANY_ADDR: Ipv6Addr = net_ip_v6!("::FFFF:0.0.0.0");

    #[derive(Copy, Clone, Debug)]
    enum DualStackBindAddr {
        Any,
        V4Any,
        V4Specific,
    }

    impl DualStackBindAddr {
        const fn v6_addr(&self) -> Option<Ipv6Addr> {
            match self {
                Self::Any => Some(DUAL_STACK_ANY_ADDR),
                Self::V4Any => Some(DUAL_STACK_V4_ANY_ADDR),
                Self::V4Specific => None,
            }
        }
    }
    const V4_LOCAL_IP: Ipv4Addr = ip_v4!("192.168.1.10");
    const V4_LOCAL_IP_MAPPED: Ipv6Addr = net_ip_v6!("::ffff:192.168.1.10");
    const V6_LOCAL_IP: Ipv6Addr = net_ip_v6!("2201::1");
    const V6_REMOTE_IP: SpecifiedAddr<Ipv6Addr> =
        unsafe { SpecifiedAddr::new_unchecked(net_ip_v6!("2001:db8::1")) };
    const V4_REMOTE_IP_MAPPED: SpecifiedAddr<Ipv6Addr> =
        unsafe { SpecifiedAddr::new_unchecked(net_ip_v6!("::FFFF:192.0.2.1")) };

    fn get_dual_stack_context<'a, BC: 'a, CC: DatagramBoundStateContext<Ipv6, BC, Udp>>(
        core_ctx: &'a mut CC,
    ) -> &'a mut CC::DualStackContext {
        match core_ctx.dual_stack_context() {
            MaybeDualStack::NotDualStack(_) => unreachable!("UDP is a dual stack enabled protocol"),
            MaybeDualStack::DualStack(ds) => ds,
        }
    }

    #[test_case(DualStackBindAddr::Any; "dual-stack")]
    #[test_case(DualStackBindAddr::V4Any; "v4 any")]
    #[test_case(DualStackBindAddr::V4Specific; "v4 specific")]
    fn dual_stack_delivery(bind_addr: DualStackBindAddr) {
        const REMOTE_IP: Ipv4Addr = ip_v4!("8.8.8.8");
        const REMOTE_IP_MAPPED: Ipv6Addr = net_ip_v6!("::ffff:8.8.8.8");
        let bind_addr = bind_addr.v6_addr().unwrap_or(V4_LOCAL_IP_MAPPED);
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            FakeCtxWithCoreCtx::with_core_ctx(FakeUdpCoreCtx::with_local_remote_ip_addrs(
                vec![SpecifiedAddr::new(V4_LOCAL_IP).unwrap()],
                vec![SpecifiedAddr::new(REMOTE_IP).unwrap()],
            ));

        let listener = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);
        SocketHandler::listen_udp(
            &mut core_ctx,
            &mut bindings_ctx,
            listener,
            SpecifiedAddr::new(bind_addr).map(|a| ZonedAddr::Unzoned(a).into()),
            Some(LOCAL_PORT),
        )
        .expect("can bind");

        const BODY: &[u8] = b"abcde";
        receive_udp_packet(
            &mut core_ctx,
            &mut bindings_ctx,
            FakeDeviceId,
            REMOTE_IP,
            V4_LOCAL_IP,
            REMOTE_PORT,
            LOCAL_PORT,
            BODY,
        );

        assert_eq!(
            bindings_ctx.state().received::<Ipv6>(),
            &HashMap::from([(
                listener,
                SocketReceived {
                    packets: vec![ReceivedPacket {
                        body: BODY.into(),
                        addr: ReceivedPacketAddrs {
                            dst_ip: V4_LOCAL_IP_MAPPED,
                            src_ip: REMOTE_IP_MAPPED,
                            src_port: Some(REMOTE_PORT),
                        }
                    }],
                }
            )])
        );
    }

    #[test_case(DualStackBindAddr::Any, true; "dual-stack any bind v4 first")]
    #[test_case(DualStackBindAddr::V4Any, true; "v4 any bind v4 first")]
    #[test_case(DualStackBindAddr::V4Specific, true; "v4 specific bind v4 first")]
    #[test_case(DualStackBindAddr::Any, false; "dual-stack any bind v4 second")]
    #[test_case(DualStackBindAddr::V4Any, false; "v4 any bind v4 second")]
    #[test_case(DualStackBindAddr::V4Specific, false; "v4 specific bind v4 second")]
    fn dual_stack_bind_conflict(bind_addr: DualStackBindAddr, bind_v4_first: bool) {
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            FakeCtxWithCoreCtx::with_core_ctx(FakeUdpCoreCtx::with_local_remote_ip_addrs(
                vec![SpecifiedAddr::new(V4_LOCAL_IP).unwrap()],
                vec![],
            ));

        let v4_listener = SocketHandler::<Ipv4, _>::create_udp(&mut core_ctx);
        let v6_listener = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);

        let bind_v4 = |core_ctx, bindings_ctx| {
            SocketHandler::listen_udp(
                core_ctx,
                bindings_ctx,
                v4_listener,
                SpecifiedAddr::new(V4_LOCAL_IP).map(|a| ZonedAddr::Unzoned(a).into()),
                Some(LOCAL_PORT),
            )
        };
        let bind_v6 = |core_ctx, bindings_ctx| {
            SocketHandler::listen_udp(
                core_ctx,
                bindings_ctx,
                v6_listener,
                SpecifiedAddr::new(bind_addr.v6_addr().unwrap_or(V4_LOCAL_IP_MAPPED))
                    .map(|a| ZonedAddr::Unzoned(a).into()),
                Some(LOCAL_PORT),
            )
        };

        let second_bind_error = if bind_v4_first {
            bind_v4(&mut core_ctx, &mut bindings_ctx).expect("no conflict");
            bind_v6(&mut core_ctx, &mut bindings_ctx).expect_err("should conflict")
        } else {
            bind_v6(&mut core_ctx, &mut bindings_ctx).expect("no conflict");
            bind_v4(&mut core_ctx, &mut bindings_ctx).expect_err("should conflict")
        };
        assert_eq!(second_bind_error, Either::Right(LocalAddressError::AddressInUse));
    }

    #[test_case(DualStackBindAddr::V4Any; "v4 any")]
    #[test_case(DualStackBindAddr::V4Specific; "v4 specific")]
    fn dual_stack_enable(bind_addr: DualStackBindAddr) {
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            FakeCtxWithCoreCtx::with_core_ctx(FakeUdpCoreCtx::with_local_remote_ip_addrs(
                vec![SpecifiedAddr::new(V4_LOCAL_IP).unwrap()],
                vec![],
            ));

        let bind_addr = bind_addr.v6_addr().unwrap_or(V4_LOCAL_IP_MAPPED);
        let listener = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);

        assert_eq!(
            SocketHandler::<Ipv6, _>::get_dual_stack_enabled(
                &mut core_ctx,
                &mut bindings_ctx,
                listener
            ),
            Ok(true)
        );
        SocketHandler::<Ipv6, _>::set_dual_stack_enabled(
            &mut core_ctx,
            &mut bindings_ctx,
            listener,
            false,
        )
        .expect("can set dual-stack enabled");

        // With dual-stack behavior disabled, the IPv6 socket can't bind to
        // an IPv4-mapped IPv6 address.
        assert_eq!(
            SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                listener,
                SpecifiedAddr::new(bind_addr).map(|a| ZonedAddr::Unzoned(a).into()),
                Some(LOCAL_PORT),
            ),
            Err(Either::Right(LocalAddressError::CannotBindToAddress))
        );
        SocketHandler::<Ipv6, _>::set_dual_stack_enabled(
            &mut core_ctx,
            &mut bindings_ctx,
            listener,
            true,
        )
        .expect("can set dual-stack enabled");
        // Try again now that dual-stack sockets are enabled.
        assert_eq!(
            SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                listener,
                SpecifiedAddr::new(bind_addr).map(|a| ZonedAddr::Unzoned(a).into()),
                Some(LOCAL_PORT),
            ),
            Ok(())
        );
    }

    #[test]
    fn dual_stack_bind_unassigned_v4_address() {
        const NOT_ASSIGNED_MAPPED: Ipv6Addr = net_ip_v6!("::ffff:8.8.8.8");
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            FakeCtxWithCoreCtx::with_core_ctx(FakeUdpCoreCtx::with_local_remote_ip_addrs(
                vec![SpecifiedAddr::new(V4_LOCAL_IP).unwrap()],
                vec![],
            ));

        let listener = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);
        assert_eq!(
            SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                listener,
                SpecifiedAddr::new(NOT_ASSIGNED_MAPPED).map(|a| ZonedAddr::Unzoned(a).into()),
                Some(LOCAL_PORT),
            ),
            Err(Either::Right(LocalAddressError::CannotBindToAddress))
        );
    }

    // Calling `connect` on an already bound socket will cause the existing
    // `listener` entry in the bound state map to be upgraded to a `connected`
    // entry. Dual-stack listeners may exist in both the IPv4 and IPv6 bound
    // state maps, so make sure both entries are properly removed.
    #[test]
    fn dual_stack_connect_cleans_up_existing_listener() {
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            FakeCtxWithCoreCtx::with_core_ctx(FakeUdpCoreCtx::with_local_remote_ip_addrs(
                vec![Ipv6::FAKE_CONFIG.local_ip],
                vec![Ipv6::FAKE_CONFIG.remote_ip],
            ));

        const DUAL_STACK_ANY_ADDR: Option<SocketZonedIpAddr<Ipv6Addr, FakeDeviceId>> = None;

        fn assert_listeners(core_ctx: &mut FakeUdpCoreCtx<FakeDeviceId>, expect_present: bool) {
            const V4_LISTENER_ADDR: ListenerAddr<
                ListenerIpAddr<Ipv4Addr, NonZeroU16>,
                FakeWeakDeviceId<FakeDeviceId>,
            > = ListenerAddr {
                ip: ListenerIpAddr { addr: None, identifier: LOCAL_PORT },
                device: None,
            };
            const V6_LISTENER_ADDR: ListenerAddr<
                ListenerIpAddr<Ipv6Addr, NonZeroU16>,
                FakeWeakDeviceId<FakeDeviceId>,
            > = ListenerAddr {
                ip: ListenerIpAddr { addr: None, identifier: LOCAL_PORT },
                device: None,
            };

            transport::udp::DualStackBoundStateContext::with_both_bound_sockets_mut(
                get_dual_stack_context(&mut core_ctx.inner),
                |_core_ctx, v6_sockets, v4_sockets| {
                    let v4 = v4_sockets.bound_sockets.listeners().get_by_addr(&V4_LISTENER_ADDR);
                    let v6 = v6_sockets.bound_sockets.listeners().get_by_addr(&V6_LISTENER_ADDR);
                    if expect_present {
                        assert_matches!(v4, Some(_));
                        assert_matches!(v6, Some(_));
                    } else {
                        assert_matches!(v4, None);
                        assert_matches!(v6, None);
                    }
                },
            );
        }

        // Create a socket and listen on the IPv6 any address. Verify we have
        // listener state for both IPv4 and IPv6.
        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);
        assert_eq!(
            SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                DUAL_STACK_ANY_ADDR,
                Some(LOCAL_PORT),
            ),
            Ok(())
        );
        assert_listeners(&mut core_ctx, true);

        // Connect the socket to a remote V6 address and verify that both
        // the IPv4 and IPv6 listener state has been removed.
        assert_eq!(
            SocketHandler::connect(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(Ipv6::FAKE_CONFIG.remote_ip).into()),
                REMOTE_PORT.into(),
            ),
            Ok(())
        );
        assert_matches!(
            SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket),
            SocketInfo::Connected(_)
        );
        assert_listeners(&mut core_ctx, false);
    }

    #[test_case(net_ip_v6!("::"), true; "dual stack any")]
    #[test_case(net_ip_v6!("::"), false; "v6 any")]
    #[test_case(net_ip_v6!("::ffff:0.0.0.0"), true; "v4 unspecified")]
    #[test_case(V4_LOCAL_IP_MAPPED, true; "v4 specified")]
    #[test_case(V6_LOCAL_IP, true; "v6 specified dual stack enabled")]
    #[test_case(V6_LOCAL_IP, false; "v6 specified dual stack disabled")]
    fn dual_stack_get_info(bind_addr: Ipv6Addr, enable_dual_stack: bool) {
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            FakeCtxWithCoreCtx::with_core_ctx(FakeUdpCoreCtx::with_local_remote_ip_addrs::<
                SpecifiedAddr<IpAddr>,
            >(
                vec![
                    SpecifiedAddr::new(V4_LOCAL_IP).unwrap().into(),
                    SpecifiedAddr::new(V6_LOCAL_IP).unwrap().into(),
                ],
                vec![],
            ));

        let listener = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);
        SocketHandler::<Ipv6, _>::set_dual_stack_enabled(
            &mut core_ctx,
            &mut bindings_ctx,
            listener,
            enable_dual_stack,
        )
        .expect("can set dual-stack enabled");
        let bind_addr = SpecifiedAddr::new(bind_addr);
        assert_eq!(
            SocketHandler::<Ipv6, _>::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                listener,
                bind_addr.map(|a| ZonedAddr::Unzoned(a).into()),
                Some(LOCAL_PORT),
            ),
            Ok(())
        );

        assert_eq!(
            SocketHandler::<Ipv6, _>::get_udp_info(&mut core_ctx, &mut bindings_ctx, listener),
            SocketInfo::Listener(ListenerInfo {
                local_ip: bind_addr.map(|a| ZonedAddr::Unzoned(a).into()),
                local_port: LOCAL_PORT,
            })
        );
    }

    #[test_case(net_ip_v6!("::"), true; "dual stack any")]
    #[test_case(net_ip_v6!("::"), false; "v6 any")]
    #[test_case(net_ip_v6!("::ffff:0.0.0.0"), true; "v4 unspecified")]
    #[test_case(V4_LOCAL_IP_MAPPED, true; "v4 specified")]
    #[test_case(V6_LOCAL_IP, true; "v6 specified dual stack enabled")]
    #[test_case(V6_LOCAL_IP, false; "v6 specified dual stack disabled")]
    fn dual_stack_remove_listener(bind_addr: Ipv6Addr, enable_dual_stack: bool) {
        // Ensure that when a socket is removed, it doesn't leave behind state
        // in the demultiplexing maps. Do this by binding a new socket at the
        // same address and asserting success.
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            FakeCtxWithCoreCtx::with_core_ctx(FakeUdpCoreCtx::with_local_remote_ip_addrs::<
                SpecifiedAddr<IpAddr>,
            >(
                vec![
                    SpecifiedAddr::new(V4_LOCAL_IP).unwrap().into(),
                    SpecifiedAddr::new(V6_LOCAL_IP).unwrap().into(),
                ],
                vec![],
            ));

        let mut bind_listener = || {
            let listener = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);
            SocketHandler::<Ipv6, _>::set_dual_stack_enabled(
                &mut core_ctx,
                &mut bindings_ctx,
                listener,
                enable_dual_stack,
            )
            .expect("can set dual-stack enabled");
            let bind_addr = SpecifiedAddr::new(bind_addr);
            assert_eq!(
                SocketHandler::<Ipv6, _>::listen_udp(
                    &mut core_ctx,
                    &mut bindings_ctx,
                    listener,
                    bind_addr.map(|a| ZonedAddr::Unzoned(a).into()),
                    Some(LOCAL_PORT),
                ),
                Ok(())
            );

            assert_eq!(
                SocketHandler::<Ipv6, _>::close(&mut core_ctx, &mut bindings_ctx, listener),
                SocketInfo::Listener(ListenerInfo {
                    local_ip: bind_addr.map(|a| ZonedAddr::Unzoned(a).into()),
                    local_port: LOCAL_PORT,
                })
            );
        };

        // The first time should succeed because the state is empty.
        bind_listener();
        // The second time should succeed because the first removal didn't
        // leave any state behind.
        bind_listener();
    }

    #[test_case(V6_REMOTE_IP, true; "This stack with dualstack enabled")]
    #[test_case(V6_REMOTE_IP, false; "This stack with dualstack disabled")]
    #[test_case(V4_REMOTE_IP_MAPPED, true; "other stack with dualstack enabled")]
    fn dualstack_remove_connected(remote_ip: SpecifiedAddr<Ipv6Addr>, enable_dual_stack: bool) {
        // Ensure that when a socket is removed, it doesn't leave behind state
        // in the demultiplexing maps. Do this by binding a new socket at the
        // same address and asserting success.
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            datagram::testutil::setup_fake_ctx_with_dualstack_conn_addrs(
                Ipv6::UNSPECIFIED_ADDRESS.to_ip_addr(),
                remote_ip.into(),
                [FakeDeviceId {}],
                |device_configs| {
                    FakeUdpCoreCtx::with_state(FakeDualStackIpSocketCtx::new(device_configs))
                },
            );

        let mut bind_connected = || {
            let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);
            SocketHandler::<Ipv6, _>::set_dual_stack_enabled(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                enable_dual_stack,
            )
            .expect("can set dual-stack enabled");
            assert_eq!(
                SocketHandler::<Ipv6, _>::connect(
                    &mut core_ctx,
                    &mut bindings_ctx,
                    socket,
                    Some(ZonedAddr::Unzoned(remote_ip).into()),
                    REMOTE_PORT.into(),
                ),
                Ok(())
            );

            assert_matches!(
                SocketHandler::<Ipv6, _>::close(&mut core_ctx, &mut bindings_ctx, socket),
                SocketInfo::Connected(ConnInfo{
                    local_ip: _,
                    local_port: _,
                    remote_ip: found_remote_ip,
                    remote_port: found_remote_port,
                }) if found_remote_ip.into_inner().addr() == remote_ip &&
                    found_remote_port == REMOTE_PORT.into()
            );
        };

        // The first time should succeed because the state is empty.
        bind_connected();
        // The second time should succeed because the first removal didn't
        // leave any state behind.
        bind_connected();
    }

    #[test_case(false, V6_REMOTE_IP, Ok(());
        "connect to this stack with dualstack disabled")]
    #[test_case(true, V6_REMOTE_IP, Ok(());
        "connect to this stack with dualstack enabled")]
    #[test_case(false, V4_REMOTE_IP_MAPPED, Err(ConnectError::RemoteUnexpectedlyMapped);
        "connect to other stack with dualstack disabled")]
    #[test_case(true, V4_REMOTE_IP_MAPPED, Ok(());
        "connect to other stack with dualstack enabled")]
    fn dualstack_connect_unbound(
        enable_dual_stack: bool,
        remote_ip: SpecifiedAddr<Ipv6Addr>,
        expected_outcome: Result<(), ConnectError>,
    ) {
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            datagram::testutil::setup_fake_ctx_with_dualstack_conn_addrs(
                Ipv6::UNSPECIFIED_ADDRESS.to_ip_addr(),
                remote_ip.into(),
                [FakeDeviceId {}],
                |device_configs| {
                    FakeUdpCoreCtx::with_state(FakeDualStackIpSocketCtx::new(device_configs))
                },
            );

        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);

        SocketHandler::<Ipv6, _>::set_dual_stack_enabled(
            &mut core_ctx,
            &mut bindings_ctx,
            socket,
            enable_dual_stack,
        )
        .expect("can set dual-stack enabled");

        assert_eq!(
            SocketHandler::connect(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(remote_ip).into()),
                REMOTE_PORT.into(),
            ),
            expected_outcome
        );

        if expected_outcome.is_ok() {
            assert_matches!(
                SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket),
                SocketInfo::Connected(ConnInfo{
                    local_ip: _,
                    local_port: _,
                    remote_ip: found_remote_ip,
                    remote_port: found_remote_port,
                }) if found_remote_ip.into_inner().addr() == remote_ip &&
                    found_remote_port == REMOTE_PORT.into()
            );
            // Disconnect the socket, returning it to the original state.
            assert_eq!(
                SocketHandler::disconnect_connected(&mut core_ctx, &mut bindings_ctx, socket),
                Ok(())
            );
        }

        // Verify the original state is preserved.
        assert_eq!(
            SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket),
            SocketInfo::Unbound
        );
    }

    #[test_case(V6_LOCAL_IP, V6_REMOTE_IP, Ok(());
        "listener in this stack connected in this stack")]
    #[test_case(V6_LOCAL_IP, V4_REMOTE_IP_MAPPED, Err(ConnectError::RemoteUnexpectedlyMapped);
        "listener in this stack connected in other stack")]
    #[test_case(Ipv6::UNSPECIFIED_ADDRESS, V6_REMOTE_IP, Ok(());
        "listener in both stacks connected in this stack")]
    #[test_case(Ipv6::UNSPECIFIED_ADDRESS, V4_REMOTE_IP_MAPPED, Ok(());
        "listener in both stacks connected in other stack")]
    #[test_case(V4_LOCAL_IP_MAPPED, V6_REMOTE_IP,
        Err(ConnectError::RemoteUnexpectedlyNonMapped);
        "listener in other stack connected in this stack")]
    #[test_case(V4_LOCAL_IP_MAPPED, V4_REMOTE_IP_MAPPED, Ok(());
        "listener in other stack connected in other stack")]
    fn dualstack_connect_listener(
        local_ip: Ipv6Addr,
        remote_ip: SpecifiedAddr<Ipv6Addr>,
        expected_outcome: Result<(), ConnectError>,
    ) {
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            datagram::testutil::setup_fake_ctx_with_dualstack_conn_addrs(
                local_ip.to_ip_addr(),
                remote_ip.into(),
                [FakeDeviceId {}],
                |device_configs| {
                    FakeUdpCoreCtx::with_state(FakeDualStackIpSocketCtx::new(device_configs))
                },
            );

        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);

        assert_eq!(
            SocketHandler::listen_udp(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                SpecifiedAddr::new(local_ip).map(|local_ip| ZonedAddr::Unzoned(local_ip).into()),
                Some(LOCAL_PORT),
            ),
            Ok(())
        );

        assert_eq!(
            SocketHandler::connect(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(remote_ip).into()),
                REMOTE_PORT.into(),
            ),
            expected_outcome
        );

        if expected_outcome.is_ok() {
            assert_matches!(
                SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket),
                SocketInfo::Connected(ConnInfo{
                    local_ip: _,
                    local_port: _,
                    remote_ip: found_remote_ip,
                    remote_port: found_remote_port,
                }) if found_remote_ip.into_inner().addr() == remote_ip &&
                    found_remote_port == REMOTE_PORT.into()
            );
            // Disconnect the socket, returning it to the original state.
            assert_eq!(
                SocketHandler::disconnect_connected(&mut core_ctx, &mut bindings_ctx, socket),
                Ok(())
            );
        }

        // Verify the original state is preserved.
        assert_matches!(
            SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket),
            SocketInfo::Listener(ListenerInfo {
                local_ip: found_local_ip, local_port: found_local_port
            }) if found_local_port == LOCAL_PORT &&
                local_ip == found_local_ip.map(
                        |a| a.into_inner().addr().get()
                    ).unwrap_or(Ipv6::UNSPECIFIED_ADDRESS)
        );
    }

    #[test_case(V6_REMOTE_IP, V6_REMOTE_IP, Ok(());
        "connected in this stack reconnected in this stack")]
    #[test_case(V6_REMOTE_IP, V4_REMOTE_IP_MAPPED, Err(ConnectError::RemoteUnexpectedlyMapped);
        "connected in this stack reconnected in other stack")]
    #[test_case(V4_REMOTE_IP_MAPPED, V6_REMOTE_IP,
        Err(ConnectError::RemoteUnexpectedlyNonMapped);
        "connected in other stack reconnected in this stack")]
    #[test_case(V4_REMOTE_IP_MAPPED, V4_REMOTE_IP_MAPPED, Ok(());
        "connected in other stack reconnected in other stack")]
    fn dualstack_connect_connected(
        original_remote_ip: SpecifiedAddr<Ipv6Addr>,
        new_remote_ip: SpecifiedAddr<Ipv6Addr>,
        expected_outcome: Result<(), ConnectError>,
    ) {
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            datagram::testutil::setup_fake_ctx_with_dualstack_conn_addrs(
                Ipv6::UNSPECIFIED_ADDRESS.to_ip_addr(),
                original_remote_ip.into(),
                [FakeDeviceId {}],
                |device_configs| {
                    FakeUdpCoreCtx::with_state(FakeDualStackIpSocketCtx::new(device_configs))
                },
            );

        let socket = SocketHandler::<Ipv6, _>::create_udp(&mut core_ctx);

        assert_eq!(
            SocketHandler::connect(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(original_remote_ip).into()),
                REMOTE_PORT.into(),
            ),
            Ok(())
        );

        assert_eq!(
            SocketHandler::connect(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
                Some(ZonedAddr::Unzoned(new_remote_ip).into()),
                OTHER_REMOTE_PORT.into(),
            ),
            expected_outcome
        );

        let (expected_remote_ip, expected_remote_port) = if expected_outcome.is_ok() {
            (new_remote_ip, OTHER_REMOTE_PORT)
        } else {
            // Verify the original state is preserved.
            (original_remote_ip, REMOTE_PORT)
        };
        assert_matches!(
            SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket),
            SocketInfo::Connected(ConnInfo{
                local_ip: _,
                local_port: _,
                remote_ip: found_remote_ip,
                remote_port: found_remote_port,
            }) if found_remote_ip.into_inner().addr() == expected_remote_ip &&
                found_remote_port == expected_remote_port.into()
        );

        // Disconnect the socket and verify it returns to unbound state.
        assert_eq!(
            SocketHandler::disconnect_connected(&mut core_ctx, &mut bindings_ctx, socket),
            Ok(())
        );
        assert_eq!(
            SocketHandler::get_udp_info(&mut core_ctx, &mut bindings_ctx, socket),
            SocketInfo::Unbound
        );
    }

    type FakeBoundSocketMap<I> = UdpBoundSocketMap<I, FakeWeakDeviceId<FakeDeviceId>>;

    fn listen<I: Ip + IpExt>(
        ip: I::Addr,
        port: u16,
    ) -> AddrVec<I, FakeWeakDeviceId<FakeDeviceId>, Udp> {
        let addr = SpecifiedAddr::new(ip).map(SocketIpAddr::new_from_specified_or_panic);
        let port = NonZeroU16::new(port).expect("port must be nonzero");
        AddrVec::Listen(ListenerAddr {
            ip: ListenerIpAddr { addr, identifier: port },
            device: None,
        })
    }

    fn listen_device<I: Ip + IpExt>(
        ip: I::Addr,
        port: u16,
        device: FakeWeakDeviceId<FakeDeviceId>,
    ) -> AddrVec<I, FakeWeakDeviceId<FakeDeviceId>, Udp> {
        let addr = SpecifiedAddr::new(ip).map(SocketIpAddr::new_from_specified_or_panic);
        let port = NonZeroU16::new(port).expect("port must be nonzero");
        AddrVec::Listen(ListenerAddr {
            ip: ListenerIpAddr { addr, identifier: port },
            device: Some(device),
        })
    }

    fn conn<I: Ip + IpExt>(
        local_ip: I::Addr,
        local_port: u16,
        remote_ip: I::Addr,
        remote_port: u16,
    ) -> AddrVec<I, FakeWeakDeviceId<FakeDeviceId>, Udp> {
        let local_ip = SocketIpAddr::new(local_ip).expect("addr must be specified & non-mapped");
        let local_port = NonZeroU16::new(local_port).expect("port must be nonzero");
        let remote_ip = SocketIpAddr::new(remote_ip).expect("addr must be specified & non-mapped");
        let remote_port = NonZeroU16::new(remote_port).expect("port must be nonzero").into();
        AddrVec::Conn(ConnAddr {
            ip: ConnIpAddr { local: (local_ip, local_port), remote: (remote_ip, remote_port) },
            device: None,
        })
    }

    #[test_case([
        (listen(ip_v4!("0.0.0.0"), 1), Sharing::Exclusive),
        (listen(ip_v4!("0.0.0.0"), 2), Sharing::Exclusive)],
            Ok(()); "listen_any_ip_different_port")]
    #[test_case([
        (listen(ip_v4!("0.0.0.0"), 1), Sharing::Exclusive),
        (listen(ip_v4!("0.0.0.0"), 1), Sharing::Exclusive)],
            Err(InsertError::Exists); "any_ip_same_port")]
    #[test_case([
        (listen(ip_v4!("1.1.1.1"), 1), Sharing::Exclusive),
        (listen(ip_v4!("1.1.1.1"), 1), Sharing::Exclusive)],
            Err(InsertError::Exists); "listen_same_specific_ip")]
    #[test_case([
        (listen(ip_v4!("1.1.1.1"), 1), Sharing::ReusePort),
        (listen(ip_v4!("1.1.1.1"), 1), Sharing::ReusePort)],
            Ok(()); "listen_same_specific_ip_reuse_port")]
    #[test_case([
        (listen(ip_v4!("1.1.1.1"), 1), Sharing::Exclusive),
        (listen(ip_v4!("1.1.1.1"), 1), Sharing::ReusePort)],
            Err(InsertError::Exists); "listen_same_specific_ip_exclusive_reuse_port")]
    #[test_case([
        (listen(ip_v4!("1.1.1.1"), 1), Sharing::ReusePort),
        (listen(ip_v4!("1.1.1.1"), 1), Sharing::Exclusive)],
            Err(InsertError::Exists); "listen_same_specific_ip_reuse_port_exclusive")]
    #[test_case([
        (listen(ip_v4!("1.1.1.1"), 1), Sharing::ReusePort),
        (conn(ip_v4!("1.1.1.1"), 1, ip_v4!("2.2.2.2"), 2), Sharing::ReusePort)],
            Ok(()); "conn_shadows_listener_reuse_port")]
    #[test_case([
        (listen(ip_v4!("1.1.1.1"), 1), Sharing::Exclusive),
        (conn(ip_v4!("1.1.1.1"), 1, ip_v4!("2.2.2.2"), 2), Sharing::Exclusive)],
            Err(InsertError::ShadowAddrExists); "conn_shadows_listener_exclusive")]
    #[test_case([
        (listen(ip_v4!("1.1.1.1"), 1), Sharing::Exclusive),
        (conn(ip_v4!("1.1.1.1"), 1, ip_v4!("2.2.2.2"), 2), Sharing::ReusePort)],
            Err(InsertError::ShadowAddrExists); "conn_shadows_listener_exclusive_reuse_port")]
    #[test_case([
        (listen(ip_v4!("1.1.1.1"), 1), Sharing::ReusePort),
        (conn(ip_v4!("1.1.1.1"), 1, ip_v4!("2.2.2.2"), 2), Sharing::Exclusive)],
            Err(InsertError::ShadowAddrExists); "conn_shadows_listener_reuse_port_exclusive")]
    #[test_case([
        (listen_device(ip_v4!("1.1.1.1"), 1, FakeWeakDeviceId(FakeDeviceId)), Sharing::Exclusive),
        (conn(ip_v4!("1.1.1.1"), 1, ip_v4!("2.2.2.2"), 2), Sharing::Exclusive)],
            Err(InsertError::IndirectConflict); "conn_indirect_conflict_specific_listener")]
    #[test_case([
        (listen_device(ip_v4!("0.0.0.0"), 1, FakeWeakDeviceId(FakeDeviceId)), Sharing::Exclusive),
        (conn(ip_v4!("1.1.1.1"), 1, ip_v4!("2.2.2.2"), 2), Sharing::Exclusive)],
            Err(InsertError::IndirectConflict); "conn_indirect_conflict_any_listener")]
    #[test_case([
        (conn(ip_v4!("1.1.1.1"), 1, ip_v4!("2.2.2.2"), 2), Sharing::Exclusive),
        (listen_device(ip_v4!("1.1.1.1"), 1, FakeWeakDeviceId(FakeDeviceId)), Sharing::Exclusive)],
            Err(InsertError::IndirectConflict); "specific_listener_indirect_conflict_conn")]
    #[test_case([
        (conn(ip_v4!("1.1.1.1"), 1, ip_v4!("2.2.2.2"), 2), Sharing::Exclusive),
        (listen_device(ip_v4!("0.0.0.0"), 1, FakeWeakDeviceId(FakeDeviceId)), Sharing::Exclusive)],
            Err(InsertError::IndirectConflict); "any_listener_indirect_conflict_conn")]
    fn bind_sequence<
        C: IntoIterator<Item = (AddrVec<Ipv4, FakeWeakDeviceId<FakeDeviceId>, Udp>, Sharing)>,
    >(
        spec: C,
        expected: Result<(), InsertError>,
    ) {
        let mut map = FakeBoundSocketMap::<Ipv4>::default();
        let mut spec = spec.into_iter().enumerate().peekable();
        let mut try_insert = |(index, (addr, options))| match addr {
            AddrVec::Conn(c) => map
                .conns_mut()
                .try_insert(c, options, EitherIpSocket::V4(SocketId::from(index)))
                .map(|_| ())
                .map_err(|(e, _)| e),
            AddrVec::Listen(l) => map
                .listeners_mut()
                .try_insert(l, options, EitherIpSocket::V4(SocketId::from(index)))
                .map(|_| ())
                .map_err(|(e, _)| e),
        };
        let last = loop {
            let one_spec = spec.next().expect("empty list of test cases");
            if spec.peek().is_none() {
                break one_spec;
            } else {
                try_insert(one_spec).expect("intermediate bind failed")
            }
        };

        let result = try_insert(last);
        assert_eq!(result, expected);
    }

    #[test_case([
            (listen(ip_v4!("1.1.1.1"), 1), Sharing::Exclusive),
            (listen(ip_v4!("2.2.2.2"), 2), Sharing::Exclusive),
        ]; "distinct")]
    #[test_case([
            (listen(ip_v4!("1.1.1.1"), 1), Sharing::ReusePort),
            (listen(ip_v4!("1.1.1.1"), 1), Sharing::ReusePort),
        ]; "listen_reuse_port")]
    #[test_case([
            (conn(ip_v4!("1.1.1.1"), 1, ip_v4!("2.2.2.2"), 3), Sharing::ReusePort),
            (conn(ip_v4!("1.1.1.1"), 1, ip_v4!("2.2.2.2"), 3), Sharing::ReusePort),
        ]; "conn_reuse_port")]
    fn remove_sequence<I>(spec: I)
    where
        I: IntoIterator<Item = (AddrVec<Ipv4, FakeWeakDeviceId<FakeDeviceId>, Udp>, Sharing)>,
        I::IntoIter: ExactSizeIterator,
    {
        enum Socket<A: IpAddress, D, LI, RI> {
            Listener(SocketId<A::Version>, ListenerAddr<ListenerIpAddr<A, LI>, D>),
            Conn(SocketId<A::Version>, ConnAddr<ConnIpAddr<A, LI, RI>, D>),
        }
        let spec = spec.into_iter();
        let spec_len = spec.len();
        for spec in spec.permutations(spec_len) {
            let mut map = FakeBoundSocketMap::<Ipv4>::default();
            let sockets = spec
                .into_iter()
                .enumerate()
                .map(|(socket_index, (addr, options))| match addr {
                    AddrVec::Conn(c) => map
                        .conns_mut()
                        .try_insert(c, options, EitherIpSocket::V4(SocketId::from(socket_index)))
                        .map(|entry| {
                            Socket::Conn(
                                assert_matches!(entry.id(), EitherIpSocket::V4(id) => *id),
                                entry.get_addr().clone(),
                            )
                        })
                        .expect("insert_failed"),
                    AddrVec::Listen(l) => map
                        .listeners_mut()
                        .try_insert(l, options, EitherIpSocket::V4(SocketId::from(socket_index)))
                        .map(|entry| {
                            Socket::Listener(
                                assert_matches!(entry.id(), EitherIpSocket::V4(id) => *id),
                                entry.get_addr().clone(),
                            )
                        })
                        .expect("insert_failed"),
                })
                .collect::<Vec<_>>();

            for socket in sockets {
                match socket {
                    Socket::Listener(l, addr) => {
                        assert_matches!(
                            map.listeners_mut().remove(&EitherIpSocket::V4(l), &addr),
                            Ok(())
                        );
                    }
                    Socket::Conn(c, addr) => {
                        assert_matches!(
                            map.conns_mut().remove(&EitherIpSocket::V4(c), &addr),
                            Ok(())
                        );
                    }
                }
            }
        }
    }

    #[ip_test]
    #[test_case(true; "bind to device")]
    #[test_case(false; "no bind to device")]
    fn loopback_bind_to_device<I: Ip + IpExt + crate::testutil::TestIpExt>(bind_to_device: bool) {
        set_logger_for_test();
        const HELLO: &'static [u8] = b"Hello";
        let (mut ctx, local_device_ids) = I::FAKE_CONFIG.into_builder().build();
        let crate::testutil::Ctx { core_ctx, bindings_ctx } = &mut ctx;
        let loopback_device_id: DeviceId<crate::testutil::FakeBindingsCtx> =
            crate::device::add_loopback_device(
                core_ctx,
                net_types::ip::Mtu::new(u16::MAX as u32),
                crate::testutil::DEFAULT_INTERFACE_METRIC,
            )
            .expect("create the loopback interface")
            .into();
        crate::device::testutil::enable_device(core_ctx, bindings_ctx, &loopback_device_id);
        let socket = create_udp::<I, _>(core_ctx);
        // TODO(https://fxbug.dev/21198): Test against dual-stack sockets.
        let local_ip = ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip).into();
        listen_udp(core_ctx, bindings_ctx, &socket, Some(local_ip), Some(LOCAL_PORT)).unwrap();
        if bind_to_device {
            set_udp_device(
                core_ctx,
                bindings_ctx,
                &socket,
                Some(&local_device_ids[0].clone().into()),
            )
            .unwrap();
        }
        send_udp_to(
            core_ctx,
            bindings_ctx,
            &socket,
            Some(SocketZonedIpAddr::from(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip))),
            LOCAL_PORT.into(),
            Buf::new(HELLO.to_vec(), ..),
        )
        .unwrap();
        crate::testutil::handle_queued_rx_packets(core_ctx, bindings_ctx);

        // TODO(https://fxbug.dev/135041): They should both be non-empty. The
        // socket map should allow a looped back packet to be delivered despite
        // it being bound to a device other than loopback.
        if bind_to_device {
            assert_matches!(&bindings_ctx.take_udp_received(socket)[..], []);
        } else {
            assert_matches!(
                &bindings_ctx.take_udp_received(socket)[..],
                [packet] => assert_eq!(packet, HELLO)
            );
        }
    }

    enum OriginalSocketState {
        Unbound,
        Listener,
        Connected,
    }

    impl OriginalSocketState {
        fn create_socket<I: TestIpExt, CC: SocketHandler<I, BC>, BC>(
            &self,
            core_ctx: &mut CC,
            bindings_ctx: &mut BC,
        ) -> SocketId<I> {
            let socket = SocketHandler::<I, _>::create_udp(core_ctx);
            match self {
                OriginalSocketState::Unbound => {}
                OriginalSocketState::Listener => {
                    SocketHandler::<I, _>::listen_udp(
                        core_ctx,
                        bindings_ctx,
                        socket,
                        Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.local_ip).into()),
                        Some(LOCAL_PORT),
                    )
                    .expect("listen should succeed");
                }
                OriginalSocketState::Connected => {
                    SocketHandler::<I, _>::connect(
                        core_ctx,
                        bindings_ctx,
                        socket,
                        Some(ZonedAddr::Unzoned(I::FAKE_CONFIG.remote_ip).into()),
                        UdpRemotePort::Set(REMOTE_PORT),
                    )
                    .expect("connect should succeed");
                }
            }
            socket
        }
    }

    #[test_case(OriginalSocketState::Unbound; "unbound")]
    #[test_case(OriginalSocketState::Listener; "listener")]
    #[test_case(OriginalSocketState::Connected; "connected")]
    fn set_get_dual_stack_enabled_v4(original_state: OriginalSocketState) {
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            FakeCtxWithCoreCtx::with_core_ctx(FakeUdpCoreCtx::with_local_remote_ip_addrs(
                vec![Ipv4::FAKE_CONFIG.local_ip],
                vec![Ipv4::FAKE_CONFIG.remote_ip],
            ));
        let socket = original_state.create_socket(&mut core_ctx, &mut bindings_ctx);

        for enabled in [true, false] {
            assert_eq!(
                SocketHandler::<Ipv4, _>::set_dual_stack_enabled(
                    &mut core_ctx,
                    &mut bindings_ctx,
                    socket,
                    enabled,
                ),
                Err(SetDualStackEnabledError::NotCapable)
            );
            assert_eq!(
                SocketHandler::<Ipv4, _>::get_dual_stack_enabled(
                    &mut core_ctx,
                    &mut bindings_ctx,
                    socket,
                ),
                Err(NotDualStackCapableError)
            );
        }
    }

    #[test_case(OriginalSocketState::Unbound, Ok(()); "unbound")]
    #[test_case(OriginalSocketState::Listener, Err(SetDualStackEnabledError::SocketIsBound);
        "listener")]
    #[test_case(OriginalSocketState::Connected, Err(SetDualStackEnabledError::SocketIsBound);
        "connected")]
    fn set_get_dual_stack_enabled_v6(
        original_state: OriginalSocketState,
        expected_result: Result<(), SetDualStackEnabledError>,
    ) {
        let FakeCtxWithCoreCtx { mut core_ctx, mut bindings_ctx } =
            FakeCtxWithCoreCtx::with_core_ctx(FakeUdpCoreCtx::with_local_remote_ip_addrs(
                vec![Ipv6::FAKE_CONFIG.local_ip],
                vec![Ipv6::FAKE_CONFIG.remote_ip],
            ));
        let socket = original_state.create_socket(&mut core_ctx, &mut bindings_ctx);

        // Expect dual stack to be enabled by default.
        const ORIGINALLY_ENABLED: bool = true;
        assert_eq!(
            SocketHandler::<Ipv6, _>::get_dual_stack_enabled(
                &mut core_ctx,
                &mut bindings_ctx,
                socket,
            ),
            Ok(ORIGINALLY_ENABLED),
        );

        for enabled in [false, true] {
            assert_eq!(
                SocketHandler::<Ipv6, _>::set_dual_stack_enabled(
                    &mut core_ctx,
                    &mut bindings_ctx,
                    socket,
                    enabled,
                ),
                expected_result,
            );
            let expect_enabled = match expected_result {
                Ok(_) => enabled,
                // If the set was unsuccessful expect the state to be unchanged.
                Err(_) => ORIGINALLY_ENABLED,
            };
            assert_eq!(
                SocketHandler::<Ipv6, _>::get_dual_stack_enabled(
                    &mut core_ctx,
                    &mut bindings_ctx,
                    socket,
                ),
                Ok(expect_enabled),
            );
        }
    }
}
