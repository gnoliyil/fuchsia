// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! IPv4 and IPv6 sockets.

use alloc::vec::Vec;
use core::cmp::Ordering;
use core::convert::Infallible;
use core::num::{NonZeroU32, NonZeroU8};

use net_types::{
    ip::{Ip, IpVersion, Ipv6Addr, Mtu},
    SpecifiedAddr, UnicastAddr,
};
use packet::{Buf, BufferMut, SerializeError, Serializer};
use thiserror::Error;

use crate::{
    context::{CounterContext, InstantContext, NonTestCtxMarker, TracingContext},
    ip::{
        device::state::IpDeviceStateIpExt,
        types::{NextHop, ResolvedRoute},
        AnyDevice, DeviceIdContext, EitherDeviceId, IpDeviceContext, IpExt, IpLayerIpExt,
        ResolveRouteError, SendIpPacketMeta,
    },
    socket::address::SocketIpAddr,
    trace_duration,
};

/// An execution context defining a type of IP socket.
pub(crate) trait IpSocketHandler<I: IpExt, C>: DeviceIdContext<AnyDevice> {
    /// Constructs a new [`IpSock`].
    ///
    /// `new_ip_socket` constructs a new `IpSock` to the given remote IP
    /// address from the given local IP address with the given IP protocol. If
    /// no local IP address is given, one will be chosen automatically. If
    /// `device` is `Some`, the socket will be bound to the given device - only
    /// routes which egress over the device will be used. If no route is
    /// available which egresses over the device - even if routes are available
    /// which egress over other devices - the socket will be considered
    /// unroutable.
    ///
    /// `new_ip_socket` returns an error if no route to the remote was found in
    /// the forwarding table or if the given local IP address is not valid for
    /// the found route.
    ///
    /// The builder may be used to override certain default parameters. Passing
    /// `None` for the `builder` parameter is equivalent to passing
    /// `Some(Default::default())`.
    fn new_ip_socket<O>(
        &mut self,
        ctx: &mut C,
        device: Option<EitherDeviceId<&Self::DeviceId, &Self::WeakDeviceId>>,
        local_ip: Option<SocketIpAddr<I::Addr>>,
        remote_ip: SocketIpAddr<I::Addr>,
        proto: I::Proto,
        options: O,
    ) -> Result<IpSock<I, Self::WeakDeviceId, O>, (IpSockCreationError, O)>;
}

/// An error in sending a packet on an IP socket.
#[derive(Error, Copy, Clone, Debug, Eq, PartialEq)]
pub enum IpSockSendError {
    /// An MTU was exceeded.
    ///
    /// This could be caused by an MTU at any layer of the stack, including both
    /// device MTUs and packet format body size limits.
    #[error("a maximum transmission unit (MTU) was exceeded")]
    Mtu,
    /// The socket is currently unroutable.
    #[error("the socket is currently unroutable: {}", _0)]
    Unroutable(#[from] ResolveRouteError),
}

impl From<SerializeError<Infallible>> for IpSockSendError {
    fn from(err: SerializeError<Infallible>) -> IpSockSendError {
        match err {
            SerializeError::Alloc(err) => match err {},
            SerializeError::SizeLimitExceeded => IpSockSendError::Mtu,
        }
    }
}

/// An error in sending a packet on a temporary IP socket.
#[derive(Error, Copy, Clone, Debug)]
pub enum IpSockCreateAndSendError {
    /// An MTU was exceeded.
    ///
    /// This could be caused by an MTU at any layer of the stack, including both
    /// device MTUs and packet format body size limits.
    #[error("a maximum transmission unit (MTU) was exceeded")]
    Mtu,
    /// The temporary socket could not be created.
    #[error("the temporary socket could not be created: {}", _0)]
    Create(#[from] IpSockCreationError),
}

/// Extension trait for `Ip` providing socket-specific functionality.
pub(crate) trait SocketIpExt: Ip + IpExt {
    /// `Self::LOOPBACK_ADDRESS`, but wrapped in the `SocketIpAddr` type.
    const LOOPBACK_ADDRESS_AS_SOCKET_IP_ADDR: SocketIpAddr<Self::Addr> = unsafe {
        // SAFETY: The loopback address is a valid SocketIpAddr, as verified
        // in the `loopback_addr_is_valid_socket_addr` test.
        SocketIpAddr::new_from_specified_unchecked(Self::LOOPBACK_ADDRESS)
    };
}

impl<I: Ip + IpExt> SocketIpExt for I {}

#[cfg(test)]
mod socket_ip_ext_test {
    use super::*;
    use ip_test_macro::ip_test;
    use net_types::ip::{Ipv4, Ipv6};

    #[ip_test]
    fn loopback_addr_is_valid_socket_addr<I: Ip + SocketIpExt>() {
        // `LOOPBACK_ADDRESS_AS_SOCKET_IP_ADDR is defined with the "unchecked"
        // constructor (the only const constructor). Verify here that the
        // addr actually satisfies all the requirements (protecting against far
        // away changes)
        let _addr = SocketIpAddr::new(I::LOOPBACK_ADDRESS_AS_SOCKET_IP_ADDR.addr())
            .expect("loopback address should be a valid SocketIpAddr");
    }
}

#[derive(Debug)]
pub(crate) enum SendOneShotIpPacketError<S, O, E> {
    CreateAndSendError { body: S, err: IpSockCreateAndSendError, options: O },
    SerializeError(E),
}

/// An extension of [`IpSocketHandler`] adding the ability to send packets on an
/// IP socket.
pub(crate) trait BufferIpSocketHandler<I: SocketIpExt, C, B: BufferMut>:
    IpSocketHandler<I, C>
{
    /// Sends an IP packet on a socket.
    ///
    /// The generated packet has its metadata initialized from `socket`,
    /// including the source and destination addresses, the Time To Live/Hop
    /// Limit, and the Protocol/Next Header. The outbound device is also chosen
    /// based on information stored in the socket.
    ///
    /// `mtu` may be used to optionally impose an MTU on the outgoing packet.
    /// Note that the device's MTU will still be imposed on the packet. That is,
    /// the smaller of `mtu` and the device's MTU will be imposed on the packet.
    ///
    /// If the socket is currently unroutable, an error is returned.
    fn send_ip_packet<S: Serializer<Buffer = B>, O: SendOptions<I>>(
        &mut self,
        ctx: &mut C,
        socket: &IpSock<I, Self::WeakDeviceId, O>,
        body: S,
        mtu: Option<u32>,
    ) -> Result<(), (S, IpSockSendError)>;

    /// Creates a temporary IP socket and sends a single packet on it.
    ///
    /// `local_ip`, `remote_ip`, `proto`, and `builder` are passed directly to
    /// [`IpSocketHandler::new_ip_socket`]. `get_body_from_src_ip` is given the
    /// source IP address for the packet - which may have been chosen
    /// automatically if `local_ip` is `None` - and returns the body to be
    /// encapsulated. This is provided in case the body's contents depend on the
    /// chosen source IP address.
    ///
    /// If `device` is specified, the available routes are limited to those that
    /// egress over the device.
    ///
    /// `mtu` may be used to optionally impose an MTU on the outgoing packet.
    /// Note that the device's MTU will still be imposed on the packet. That is,
    /// the smaller of `mtu` and the device's MTU will be imposed on the packet.
    ///
    /// # Errors
    ///
    /// If an error is encountered while sending the packet, the body returned
    /// from `get_body_from_src_ip` will be returned along with the error. If an
    /// error is encountered while constructing the temporary IP socket,
    /// `get_body_from_src_ip` will be called on an arbitrary IP address in
    /// order to obtain a body to return. In the case where a buffer was passed
    /// by ownership to `get_body_from_src_ip`, this allows the caller to
    /// recover that buffer. `get_body_from_src_ip` is fallible, and if there's
    /// an error, it will be returned as well.
    fn send_oneshot_ip_packet_with_fallible_serializer<
        S: Serializer<Buffer = B>,
        E,
        F: FnOnce(SocketIpAddr<I::Addr>) -> Result<S, E>,
        O: SendOptions<I>,
    >(
        &mut self,
        ctx: &mut C,
        device: Option<EitherDeviceId<&Self::DeviceId, &Self::WeakDeviceId>>,
        local_ip: Option<SocketIpAddr<I::Addr>>,
        remote_ip: SocketIpAddr<I::Addr>,
        proto: I::Proto,
        options: O,
        get_body_from_src_ip: F,
        mtu: Option<u32>,
    ) -> Result<(), SendOneShotIpPacketError<S, O, E>> {
        // We use a `match` instead of `map_err` because `map_err` would require passing a closure
        // which takes ownership of `get_body_from_src_ip`, which we also use in the success case.
        match self.new_ip_socket(ctx, device, local_ip, remote_ip, proto, options) {
            Err((err, options)) => {
                Err(match get_body_from_src_ip(I::LOOPBACK_ADDRESS_AS_SOCKET_IP_ADDR) {
                    Ok(body) => SendOneShotIpPacketError::CreateAndSendError {
                        body,
                        err: err.into(),
                        options,
                    },
                    Err(ser_err) => SendOneShotIpPacketError::SerializeError(ser_err),
                })
            }
            Ok(tmp) => {
                let packet = get_body_from_src_ip(*tmp.local_ip())
                    .map_err(SendOneShotIpPacketError::SerializeError)?;
                self.send_ip_packet(ctx, &tmp, packet, mtu).map_err(|(body, err)| match err {
                    IpSockSendError::Mtu => {
                        let IpSock { options, definition: _ } = tmp;
                        SendOneShotIpPacketError::CreateAndSendError {
                            body,
                            err: IpSockCreateAndSendError::Mtu,
                            options,
                        }
                    }
                    IpSockSendError::Unroutable(_) => {
                        unreachable!("socket which was just created should still be routable")
                    }
                })
            }
        }
    }

    /// Sends a one-shot IP packet but with a non-fallible serializer.
    fn send_oneshot_ip_packet<
        S: Serializer<Buffer = B>,
        F: FnOnce(SocketIpAddr<I::Addr>) -> S,
        O: SendOptions<I>,
    >(
        &mut self,
        ctx: &mut C,
        device: Option<EitherDeviceId<&Self::DeviceId, &Self::WeakDeviceId>>,
        local_ip: Option<SocketIpAddr<I::Addr>>,
        remote_ip: SocketIpAddr<I::Addr>,
        proto: I::Proto,
        options: O,
        get_body_from_src_ip: F,
        mtu: Option<u32>,
    ) -> Result<(), (S, IpSockCreateAndSendError, O)> {
        self.send_oneshot_ip_packet_with_fallible_serializer(
            ctx,
            device,
            local_ip,
            remote_ip,
            proto,
            options,
            |ip| Ok::<_, Infallible>(get_body_from_src_ip(ip)),
            mtu,
        )
        .map_err(|err| match err {
            SendOneShotIpPacketError::CreateAndSendError { body, err, options } => {
                (body, err, options)
            }
            SendOneShotIpPacketError::SerializeError(infallible) => match infallible {},
        })
    }
}

/// Maximum packet size, that is the maximum IP payload one packet can carry.
///
/// More details from https://www.rfc-editor.org/rfc/rfc1122#section-3.3.2.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Mms(NonZeroU32);

impl Mms {
    pub(crate) fn from_mtu<I: IpExt>(mtu: Mtu, options_size: u32) -> Option<Self> {
        NonZeroU32::new(mtu.get().saturating_sub(I::IP_HEADER_LENGTH.get() + options_size))
            .map(|mms| Self(mms.min(I::IP_MAX_PAYLOAD_LENGTH)))
    }

    pub(crate) fn get(&self) -> NonZeroU32 {
        let Self(mms) = *self;
        mms
    }
}

/// Possible errors when retrieving the maximum transport message size.
#[derive(Error, Copy, Clone, Debug, Eq, PartialEq)]
pub(crate) enum MmsError {
    /// Cannot find the device that is used for the ip socket, possibly because
    /// there is no route.
    #[error("cannot find the device: {}", _0)]
    NoDevice(#[from] ResolveRouteError),
    /// The MTU provided by the device is too small such that there is no room
    /// for a transport message at all.
    #[error("invalid MTU: {:?}", _0)]
    MTUTooSmall(Mtu),
}

/// Gets device related information of an IP socket.
pub(crate) trait DeviceIpSocketHandler<I, C>: DeviceIdContext<AnyDevice>
where
    I: IpLayerIpExt,
{
    /// Gets the maximum message size for the transport layer, it equals the
    /// device MTU minus the IP header size.
    ///
    /// This corresponds to the GET_MAXSIZES call described in:
    /// https://www.rfc-editor.org/rfc/rfc1122#section-3.4
    fn get_mms<O: SendOptions<I>>(
        &mut self,
        ctx: &mut C,
        ip_sock: &IpSock<I, Self::WeakDeviceId, O>,
    ) -> Result<Mms, MmsError>;
}

/// An error encountered when creating an IP socket.
#[derive(Error, Copy, Clone, Debug, Eq, PartialEq)]
pub enum IpSockCreationError {
    /// An error occurred while looking up a route.
    #[error("a route cannot be determined: {}", _0)]
    Route(#[from] ResolveRouteError),
}

/// An IP socket.
#[derive(Clone, Debug)]
#[cfg_attr(test, derive(PartialEq))]
pub(crate) struct IpSock<I: IpExt, D, O> {
    /// The definition of the socket.
    ///
    /// This does not change for the lifetime of the socket.
    definition: IpSockDefinition<I, D>,
    /// Options set on the socket that are independent of the socket definition.
    ///
    /// TODO(https://fxbug.dev/39479): use this to record multicast options.
    #[allow(unused)]
    options: O,
}

/// The definition of an IP socket.
///
/// These values are part of the socket's definition, and never change.
#[derive(Clone, Debug)]
#[cfg_attr(test, derive(PartialEq))]
pub(crate) struct IpSockDefinition<I: IpExt, D> {
    remote_ip: SocketIpAddr<I::Addr>,
    // Guaranteed to be unicast in its subnet since it's always equal to an
    // address assigned to the local device. We can't use the `UnicastAddr`
    // witness type since `Ipv4Addr` doesn't implement `UnicastAddress`.
    //
    // TODO(joshlf): Support unnumbered interfaces. Once we do that, a few
    // issues arise: A) Does the unicast restriction still apply, and is that
    // even well-defined for IPv4 in the absence of a subnet? B) Presumably we
    // have to always bind to a particular interface?
    local_ip: SocketIpAddr<I::Addr>,
    device: Option<D>,
    proto: I::Proto,
}

impl<I: IpExt, D, O> IpSock<I, D, O> {
    pub(crate) fn local_ip(&self) -> &SocketIpAddr<I::Addr> {
        &self.definition.local_ip
    }

    pub(crate) fn remote_ip(&self) -> &SocketIpAddr<I::Addr> {
        &self.definition.remote_ip
    }

    pub(crate) fn device(&self) -> Option<&D> {
        self.definition.device.as_ref()
    }

    pub(crate) fn proto(&self) -> I::Proto {
        self.definition.proto
    }

    pub(crate) fn options(&self) -> &O {
        &self.options
    }

    pub(crate) fn options_mut(&mut self) -> &mut O {
        &mut self.options
    }

    /// Swaps in `new_options` for the existing options and returns the old
    /// options.
    pub(crate) fn replace_options(&mut self, new_options: O) -> O {
        core::mem::replace(self.options_mut(), new_options)
    }

    pub(crate) fn take_options(&mut self) -> O
    where
        O: Default,
    {
        self.replace_options(Default::default())
    }

    /// Consumes the socket and returns the contained options.
    pub(crate) fn into_options(self) -> O {
        let Self { definition: _, options } = self;
        options
    }
}

// TODO(joshlf): Once we support configuring transport-layer protocols using
// type parameters, use that to ensure that `proto` is the right protocol for
// the caller. We will still need to have a separate enforcement mechanism for
// raw IP sockets once we support those.

/// The non-synchronized execution context for IP sockets.
pub(super) trait IpSocketNonSyncContext:
    InstantContext + CounterContext + TracingContext
{
}
impl<C: InstantContext + CounterContext + TracingContext> IpSocketNonSyncContext for C {}

/// The context required in order to implement [`IpSocketHandler`].
///
/// Blanket impls of `IpSocketHandler` are provided in terms of
/// `IpSocketContext`.
pub(super) trait IpSocketContext<I, C: IpSocketNonSyncContext>:
    DeviceIdContext<AnyDevice>
where
    I: IpDeviceStateIpExt,
{
    /// Returns a route for a socket.
    ///
    /// If `device` is specified, the available routes are limited to those that
    /// egress over the device.
    fn lookup_route(
        &mut self,
        ctx: &mut C,
        device: Option<&Self::DeviceId>,
        src_ip: Option<SpecifiedAddr<I::Addr>>,
        dst_ip: SpecifiedAddr<I::Addr>,
    ) -> Result<ResolvedRoute<I, Self::DeviceId>, ResolveRouteError>;
}

/// The context required in order to implement [`BufferIpSocketHandler`].
///
/// Blanket impls of `BufferIpSocketHandler` are provided in terms of
/// `BufferIpSocketContext`.
pub(super) trait BufferIpSocketContext<I, C: IpSocketNonSyncContext, B: BufferMut>:
    IpSocketContext<I, C>
where
    I: IpDeviceStateIpExt + packet_formats::ip::IpExt,
{
    /// Send an IP packet to the next-hop node.
    fn send_ip_packet<S: Serializer<Buffer = B>>(
        &mut self,
        ctx: &mut C,
        meta: SendIpPacketMeta<I, &Self::DeviceId, SpecifiedAddr<I::Addr>>,
        body: S,
    ) -> Result<(), S>;
}

impl<I: Ip + IpExt + IpDeviceStateIpExt, C: IpSocketNonSyncContext, SC: IpSocketContext<I, C>>
    IpSocketHandler<I, C> for SC
{
    fn new_ip_socket<O>(
        &mut self,
        ctx: &mut C,
        device: Option<EitherDeviceId<&SC::DeviceId, &SC::WeakDeviceId>>,
        local_ip: Option<SocketIpAddr<I::Addr>>,
        remote_ip: SocketIpAddr<I::Addr>,
        proto: I::Proto,
        options: O,
    ) -> Result<IpSock<I, SC::WeakDeviceId, O>, (IpSockCreationError, O)> {
        let device = if let Some(device) = device.as_ref() {
            if let Some(device) = device.as_strong_ref(self) {
                Some(device)
            } else {
                return Err((IpSockCreationError::Route(ResolveRouteError::Unreachable), options));
            }
        } else {
            None
        };

        let device = device.as_ref().map(|d| d.as_ref());

        // Make sure the remote is routable with a local address before creating
        // the socket. We do not care about the actual destination here because
        // we will recalculate it when we send a packet so that the best route
        // available at the time is used for each outgoing packet.
        let ResolvedRoute { src_addr, device: route_device, next_hop: _ } =
            match self.lookup_route(ctx, device, local_ip.map(|a| a.into()), remote_ip.into()) {
                Ok(r) => r,
                Err(e) => return Err((e.into(), options)),
            };
        // TODO(https://fxbug.dev/132092): Remove this panic opportunity once
        // `lookup_route` returns a `NonMappedAddr`.
        let src_addr = SocketIpAddr::new_from_specified_or_panic(src_addr);

        // If the source or destination address require a device, make sure to
        // set that in the socket definition. Otherwise defer to what was provided.
        let socket_device = (crate::socket::must_have_zone(src_addr.as_ref())
            || crate::socket::must_have_zone(remote_ip.as_ref()))
        .then_some(route_device)
        .as_ref()
        .or(device)
        .map(|d| self.downgrade_device_id(d));

        let definition =
            IpSockDefinition { local_ip: src_addr, remote_ip, device: socket_device, proto };
        Ok(IpSock { definition: definition, options })
    }
}

/// Provides hooks for altering sending behavior of [`IpSock`].
///
/// Must be implemented by the socket option type of an `IpSock` when using it
/// to call [`BufferIpSocketHandler::send_ip_packet`]. This is implemented as a
/// trait instead of an inherent impl on a type so that users of sockets that
/// don't need certain option types, like TCP for anything multicast-related,
/// can avoid allocating space for those options.
pub trait SendOptions<I: Ip> {
    /// Returns the hop limit to set on a packet going to the given destination.
    ///
    /// If `Some(u)`, `u` will be used as the hop limit (IPv6) or TTL (IPv4) for
    /// a packet going to the given destination. Otherwise the default value
    /// will be used.
    fn hop_limit(&self, destination: &SpecifiedAddr<I::Addr>) -> Option<NonZeroU8>;
}

/// Empty send options that never overrides default values.
#[derive(Copy, Clone, Debug, Default, Eq, PartialEq)]
pub(crate) struct DefaultSendOptions;

impl<I: Ip> SendOptions<I> for DefaultSendOptions {
    fn hop_limit(&self, _destination: &SpecifiedAddr<I::Addr>) -> Option<NonZeroU8> {
        None
    }
}

impl<I: Ip, S: SendOptions<I>> SendOptions<I> for &'_ S {
    fn hop_limit(&self, destination: &SpecifiedAddr<<I as Ip>::Addr>) -> Option<NonZeroU8> {
        S::hop_limit(self, destination)
    }
}

fn send_ip_packet<
    I: IpExt + IpDeviceStateIpExt + packet_formats::ip::IpExt,
    B: BufferMut,
    S: Serializer<Buffer = B>,
    C: IpSocketNonSyncContext,
    SC: BufferIpSocketContext<I, C, B>
        + BufferIpSocketContext<I, C, Buf<Vec<u8>>>
        + IpSocketContext<I, C>,
    O: SendOptions<I>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    socket: &IpSock<I, SC::WeakDeviceId, O>,
    body: S,
    mtu: Option<u32>,
) -> Result<(), (S, IpSockSendError)> {
    trace_duration!(ctx, "ip::send_packet");

    let IpSock { definition: IpSockDefinition { remote_ip, local_ip, device, proto }, options } =
        socket;
    let remote_ip: SpecifiedAddr<_> = (*remote_ip).into();
    let local_ip: SpecifiedAddr<_> = (*local_ip).into();

    let device = if let Some(device) = device {
        let Some(device) = sync_ctx.upgrade_weak_device_id(device) else {
            return Err((body, ResolveRouteError::Unreachable.into()));
        };
        Some(device)
    } else {
        None
    };

    let ResolvedRoute { src_addr: got_local_ip, device, next_hop } =
        match sync_ctx.lookup_route(ctx, device.as_ref(), Some(local_ip), remote_ip) {
            Ok(o) => o,
            Err(e) => return Err((body, IpSockSendError::Unroutable(e))),
        };

    assert_eq!(local_ip, got_local_ip);

    let next_hop = match next_hop {
        NextHop::RemoteAsNeighbor => remote_ip,
        NextHop::Gateway(gateway) => gateway,
    };

    BufferIpSocketContext::send_ip_packet(
        sync_ctx,
        ctx,
        SendIpPacketMeta {
            device: &device,
            src_ip: local_ip,
            dst_ip: remote_ip,
            next_hop,
            ttl: options.hop_limit(&remote_ip),
            proto: *proto,
            mtu,
        },
        body,
    )
    .map_err(|s| (s, IpSockSendError::Mtu))
}

impl<
        I: Ip + IpExt + IpDeviceStateIpExt,
        B: BufferMut,
        C: IpSocketNonSyncContext,
        SC: BufferIpSocketContext<I, C, B>
            + BufferIpSocketContext<I, C, Buf<Vec<u8>>>
            + IpSocketContext<I, C>,
    > BufferIpSocketHandler<I, C, B> for SC
{
    fn send_ip_packet<S: Serializer<Buffer = B>, O: SendOptions<I>>(
        &mut self,
        ctx: &mut C,
        ip_sock: &IpSock<I, SC::WeakDeviceId, O>,
        body: S,
        mtu: Option<u32>,
    ) -> Result<(), (S, IpSockSendError)> {
        // TODO(joshlf): Call `trace!` with relevant fields from the socket.
        let counter_name = match I::VERSION {
            IpVersion::V4 => "send_ipv4_packet",
            IpVersion::V6 => "send_ipv6_packet",
        };
        ctx.increment_debug_counter(counter_name);

        send_ip_packet(self, ctx, ip_sock, body, mtu)
    }
}

impl<
        I: IpLayerIpExt + IpDeviceStateIpExt,
        C: IpSocketNonSyncContext,
        SC: IpDeviceContext<I, C> + IpSocketContext<I, C> + NonTestCtxMarker,
    > DeviceIpSocketHandler<I, C> for SC
{
    fn get_mms<O: SendOptions<I>>(
        &mut self,
        ctx: &mut C,
        ip_sock: &IpSock<I, Self::WeakDeviceId, O>,
    ) -> Result<Mms, MmsError> {
        let IpSockDefinition { remote_ip, local_ip, device, proto: _ } = &ip_sock.definition;
        let device = device
            .as_ref()
            .map(|d| self.upgrade_weak_device_id(d).ok_or(ResolveRouteError::Unreachable))
            .transpose()?;

        let ResolvedRoute { src_addr: _, device, next_hop: _ } = self
            .lookup_route(ctx, device.as_ref(), Some((*local_ip).into()), (*remote_ip).into())
            .map_err(MmsError::NoDevice)?;
        let mtu = IpDeviceContext::<I, C>::get_mtu(self, &device);
        // TODO(https://fxbug.dev/121911): Calculate the options size when they
        // are supported.
        Mms::from_mtu::<I>(mtu, 0 /* no ip options used */).ok_or(MmsError::MTUTooSmall(mtu))
    }
}

/// IPv6 source address selection as defined in [RFC 6724 Section 5].
pub(crate) mod ipv6_source_address_selection {
    use net_types::ip::{AddrSubnet, IpAddress as _};

    use super::*;

    use crate::ip::device::state::Ipv6AddressFlags;

    pub(crate) struct SasCandidate<D> {
        pub(crate) addr_sub: AddrSubnet<Ipv6Addr, UnicastAddr<Ipv6Addr>>,
        pub(crate) flags: Ipv6AddressFlags,
        pub(crate) device: D,
    }

    /// Selects the source address for an IPv6 socket using the algorithm
    /// defined in [RFC 6724 Section 5].
    ///
    /// This algorithm is only applicable when the user has not explicitly
    /// specified a source address.
    ///
    /// `remote_ip` is the remote IP address of the socket, `outbound_device` is
    /// the device over which outbound traffic to `remote_ip` is sent (according
    /// to the forwarding table), and `addresses` is an iterator of all
    /// addresses on all devices. The algorithm works by iterating over
    /// `addresses` and selecting the address which is most preferred according
    /// to a set of selection criteria.
    pub(crate) fn select_ipv6_source_address<
        'a,
        D: PartialEq,
        I: Iterator<Item = SasCandidate<D>>,
    >(
        remote_ip: Option<SpecifiedAddr<Ipv6Addr>>,
        outbound_device: &D,
        addresses: I,
    ) -> Option<UnicastAddr<Ipv6Addr>> {
        // Source address selection as defined in RFC 6724 Section 5.
        //
        // The algorithm operates by defining a partial ordering on available
        // source addresses, and choosing one of the best address as defined by
        // that ordering (given multiple best addresses, the choice from among
        // those is implementation-defined). The partial order is defined in
        // terms of a sequence of rules. If a given rule defines an order
        // between two addresses, then that is their order. Otherwise, the next
        // rule must be consulted, and so on until all of the rules are
        // exhausted.

        addresses
            // Tentative addresses are not considered available to the source
            // selection algorithm.
            .filter(|SasCandidate { addr_sub: _, flags, device: _ }| flags.assigned)
            .max_by(|a, b| select_ipv6_source_address_cmp(remote_ip, outbound_device, a, b))
            .map(|SasCandidate { addr_sub, flags: _, device: _ }| addr_sub.addr())
    }

    /// Comparison operator used by `select_ipv6_source_address`.
    fn select_ipv6_source_address_cmp<D: PartialEq>(
        remote_ip: Option<SpecifiedAddr<Ipv6Addr>>,
        outbound_device: &D,
        a: &SasCandidate<D>,
        b: &SasCandidate<D>,
    ) -> Ordering {
        // TODO(fxbug.dev/46822): Implement rules 2, 4, 5.5, 6, and 7.

        let a_addr = a.addr_sub.addr().into_specified();
        let b_addr = b.addr_sub.addr().into_specified();

        // Assertions required in order for this implementation to be valid.

        // Required by the implementation of Rule 1.
        if let Some(remote_ip) = remote_ip {
            debug_assert!(!(a_addr == remote_ip && b_addr == remote_ip));
        }

        // Addresses that are not considered assigned are not valid source
        // addresses.
        debug_assert!(a.flags.assigned);
        debug_assert!(b.flags.assigned);

        rule_1(remote_ip, a_addr, b_addr)
            .then_with(|| rule_3(a.flags.deprecated, b.flags.deprecated))
            .then_with(|| rule_5(outbound_device, &a.device, &b.device))
            .then_with(|| rule_8(remote_ip, a.addr_sub, b.addr_sub))
    }

    // Assumes that `a` and `b` are not both equal to `remote_ip`.
    fn rule_1(
        remote_ip: Option<SpecifiedAddr<Ipv6Addr>>,
        a: SpecifiedAddr<Ipv6Addr>,
        b: SpecifiedAddr<Ipv6Addr>,
    ) -> Ordering {
        let remote_ip = match remote_ip {
            Some(remote_ip) => remote_ip,
            None => return Ordering::Equal,
        };
        if (a == remote_ip) != (b == remote_ip) {
            // Rule 1: Prefer same address.
            //
            // Note that both `a` and `b` cannot be equal to `remote_ip` since
            // that would imply that we had added the same address twice to the
            // same device.
            //
            // If `(a == remote_ip) != (b == remote_ip)`, then exactly one of
            // them is equal. If this inequality does not hold, then they must
            // both be unequal to `remote_ip`. In the first case, we have a tie,
            // and in the second case, the rule doesn't apply. In either case,
            // we move onto the next rule.
            if a == remote_ip {
                Ordering::Greater
            } else {
                Ordering::Less
            }
        } else {
            Ordering::Equal
        }
    }

    fn rule_3(a_deprecated: bool, b_deprecated: bool) -> Ordering {
        match (a_deprecated, b_deprecated) {
            (true, false) => Ordering::Less,
            (true, true) | (false, false) => Ordering::Equal,
            (false, true) => Ordering::Greater,
        }
    }

    fn rule_5<D: PartialEq>(outbound_device: &D, a_device: &D, b_device: &D) -> Ordering {
        if (a_device == outbound_device) != (b_device == outbound_device) {
            // Rule 5: Prefer outgoing interface.
            if a_device == outbound_device {
                Ordering::Greater
            } else {
                Ordering::Less
            }
        } else {
            Ordering::Equal
        }
    }

    fn rule_8(
        remote_ip: Option<SpecifiedAddr<Ipv6Addr>>,
        a: AddrSubnet<Ipv6Addr, UnicastAddr<Ipv6Addr>>,
        b: AddrSubnet<Ipv6Addr, UnicastAddr<Ipv6Addr>>,
    ) -> Ordering {
        let remote_ip = match remote_ip {
            Some(remote_ip) => remote_ip,
            None => return Ordering::Equal,
        };
        // Per RFC 6724 Section 2.2:
        //
        //   We define the common prefix length CommonPrefixLen(S, D) of a
        //   source address S and a destination address D as the length of the
        //   longest prefix (looking at the most significant, or leftmost, bits)
        //   that the two addresses have in common, up to the length of S's
        //   prefix (i.e., the portion of the address not including the
        //   interface ID).  For example, CommonPrefixLen(fe80::1, fe80::2) is
        //   64.
        fn common_prefix_len(
            src: AddrSubnet<Ipv6Addr, UnicastAddr<Ipv6Addr>>,
            dst: SpecifiedAddr<Ipv6Addr>,
        ) -> u8 {
            core::cmp::min(src.addr().common_prefix_len(&dst), src.subnet().prefix())
        }

        // Rule 8: Use longest matching prefix.
        //
        // Note that, per RFC 6724 Section 5:
        //
        //   Rule 8 MAY be superseded if the implementation has other means of
        //   choosing among source addresses.  For example, if the
        //   implementation somehow knows which source address will result in
        //   the "best" communications performance.
        //
        // We don't currently make use of this option, but it's an option for
        // the future.
        common_prefix_len(a, remote_ip).cmp(&common_prefix_len(b, remote_ip))
    }

    #[cfg(test)]
    mod tests {
        use net_declare::net_ip_v6;
        use net_types::ip::AddrSubnet;

        use super::*;

        #[test]
        fn test_select_ipv6_source_address() {
            // Test the comparison operator used by `select_ipv6_source_address`
            // by separately testing each comparison condition.

            let remote = SpecifiedAddr::new(net_ip_v6!("2001:0db8:1::")).unwrap();
            let local0 = SpecifiedAddr::new(net_ip_v6!("2001:0db8:2::")).unwrap();
            let local1 = SpecifiedAddr::new(net_ip_v6!("2001:0db8:3::")).unwrap();
            let dev0 = &0;
            let dev1 = &1;
            let dev2 = &2;

            // Rule 1: Prefer same address
            assert_eq!(rule_1(Some(remote), remote, local0), Ordering::Greater);
            assert_eq!(rule_1(Some(remote), local0, remote), Ordering::Less);
            assert_eq!(rule_1(Some(remote), local0, local1), Ordering::Equal);
            assert_eq!(rule_1(None, local0, local1), Ordering::Equal);

            // Rule 3: Avoid deprecated states
            assert_eq!(rule_3(false, true), Ordering::Greater);
            assert_eq!(rule_3(true, false), Ordering::Less);
            assert_eq!(rule_3(true, true), Ordering::Equal);
            assert_eq!(rule_3(false, false), Ordering::Equal);

            // Rule 5: Prefer outgoing interface
            assert_eq!(rule_5(dev0, dev0, dev2), Ordering::Greater);
            assert_eq!(rule_5(dev0, dev2, dev0), Ordering::Less);
            assert_eq!(rule_5(dev0, dev0, dev0), Ordering::Equal);
            assert_eq!(rule_5(dev0, dev2, dev2), Ordering::Equal);

            // Rule 8: Use longest matching prefix.
            {
                let new_addr_entry = |addr, prefix_len| AddrSubnet::new(addr, prefix_len).unwrap();

                // First, test that the longest prefix match is preferred when
                // using addresses whose common prefix length is shorter than
                // the subnet prefix length.

                // 4 leading 0x01 bytes.
                let remote = SpecifiedAddr::new(net_ip_v6!("1111::")).unwrap();
                // 3 leading 0x01 bytes.
                let local0 = new_addr_entry(net_ip_v6!("1110::"), 64);
                // 2 leading 0x01 bytes.
                let local1 = new_addr_entry(net_ip_v6!("1100::"), 64);

                assert_eq!(rule_8(Some(remote), local0, local1), Ordering::Greater);
                assert_eq!(rule_8(Some(remote), local1, local0), Ordering::Less);
                assert_eq!(rule_8(Some(remote), local0, local0), Ordering::Equal);
                assert_eq!(rule_8(Some(remote), local1, local1), Ordering::Equal);
                assert_eq!(rule_8(None, local0, local1), Ordering::Equal);

                // Second, test that the common prefix length is capped at the
                // subnet prefix length.

                // 3 leading 0x01 bytes, but a subnet prefix length of 8 (1 byte).
                let local0 = new_addr_entry(net_ip_v6!("1110::"), 8);
                // 2 leading 0x01 bytes, but a subnet prefix length of 8 (1 byte).
                let local1 = new_addr_entry(net_ip_v6!("1100::"), 8);

                assert_eq!(rule_8(Some(remote), local0, local1), Ordering::Equal);
                assert_eq!(rule_8(Some(remote), local1, local0), Ordering::Equal);
                assert_eq!(rule_8(Some(remote), local0, local0), Ordering::Equal);
                assert_eq!(rule_8(Some(remote), local1, local1), Ordering::Equal);
                assert_eq!(rule_8(None, local0, local1), Ordering::Equal);
            }

            {
                let new_addr_entry = |addr, device| SasCandidate {
                    addr_sub: AddrSubnet::new(addr, 128).unwrap(),
                    flags: Ipv6AddressFlags { deprecated: false, assigned: true },
                    device,
                };

                // If no rules apply, then the two address entries are equal.
                assert_eq!(
                    select_ipv6_source_address_cmp(
                        Some(remote),
                        dev0,
                        &new_addr_entry(*local0, *dev1),
                        &new_addr_entry(*local1, *dev2),
                    ),
                    Ordering::Equal
                );
            }
        }

        #[test]
        fn test_select_ipv6_source_address_no_remote() {
            // Verify that source address selection correctly applies all
            // applicable rules when the remote is `None`.
            let dev0 = &0;
            let dev1 = &1;
            let dev2 = &2;

            let local0 = SpecifiedAddr::new(net_ip_v6!("2001:0db8:2::")).unwrap();
            let local1 = SpecifiedAddr::new(net_ip_v6!("2001:0db8:3::")).unwrap();

            let new_addr_entry = |addr, deprecated, device| SasCandidate {
                addr_sub: AddrSubnet::new(addr, 128).unwrap(),
                flags: Ipv6AddressFlags { deprecated, assigned: true },
                device,
            };

            // Verify that Rule 3 still applies (avoid deprecated states).
            assert_eq!(
                select_ipv6_source_address_cmp(
                    None,
                    dev0,
                    &new_addr_entry(*local0, false, *dev1),
                    &new_addr_entry(*local1, true, *dev2),
                ),
                Ordering::Greater
            );

            // Verify that Rule 5 still applies (Prefer outgoing interface).
            assert_eq!(
                select_ipv6_source_address_cmp(
                    None,
                    dev0,
                    &new_addr_entry(*local0, false, *dev0),
                    &new_addr_entry(*local1, false, *dev1),
                ),
                Ordering::Greater
            );
        }
    }
}

/// Test fake implementations of the traits defined in the `socket` module.
#[cfg(test)]
pub(crate) mod testutil {
    use alloc::{boxed::Box, collections::HashMap, vec::Vec};
    use core::{fmt::Debug, num::NonZeroUsize};

    use derivative::Derivative;
    use net_types::{
        ip::{GenericOverIp, IpAddr},
        MulticastAddr,
    };

    use super::*;
    use crate::{
        context::{
            testutil::{FakeInstant, FakeNonSyncCtx, FakeSyncCtx},
            RngContext, SendFrameContext, TracingContext,
        },
        device::testutil::{FakeStrongDeviceId, FakeWeakDeviceId},
        ip::{
            device::state::{AssignedAddress as _, DualStackIpDeviceState, IpDeviceState},
            forwarding::{
                testutil::{DualStackForwardingTable, FakeIpForwardingCtx},
                ForwardingTable,
            },
            testutil::FakeIpDeviceIdCtx,
            types::Destination,
            HopLimits, MulticastMembershipHandler, SendIpPacketMeta, TransportIpContext,
            DEFAULT_HOP_LIMITS,
        },
        sync::PrimaryRc,
    };

    /// A fake implementation of [`IpSocketContext`].
    ///
    /// `IpSocketContext` is implemented for `FakeIpSocketCtx` and any
    /// `FakeCtx<S>` where `S` implements `AsRef` and `AsMut` for
    /// `FakeIpSocketCtx`.
    #[derive(Derivative, GenericOverIp)]
    #[generic_over_ip(I, Ip)]
    #[derivative(Default(bound = ""))]
    pub(crate) struct FakeIpSocketCtx<I: IpDeviceStateIpExt, D> {
        pub(crate) table: ForwardingTable<I, D>,
        device_state: HashMap<D, IpDeviceState<FakeInstant, I>>,
        ip_forwarding_ctx: FakeIpForwardingCtx<D>,
    }

    impl<I: IpDeviceStateIpExt, D> AsRef<FakeIpDeviceIdCtx<D>> for FakeIpSocketCtx<I, D> {
        fn as_ref(&self) -> &FakeIpDeviceIdCtx<D> {
            let FakeIpSocketCtx { device_state: _, table: _, ip_forwarding_ctx } = self;
            ip_forwarding_ctx.get_ref().as_ref()
        }
    }

    impl<I: IpDeviceStateIpExt, D> AsMut<FakeIpDeviceIdCtx<D>> for FakeIpSocketCtx<I, D> {
        fn as_mut(&mut self) -> &mut FakeIpDeviceIdCtx<D> {
            let FakeIpSocketCtx { device_state: _, table: _, ip_forwarding_ctx } = self;
            ip_forwarding_ctx.get_mut().as_mut()
        }
    }

    impl<I: IpDeviceStateIpExt, D> AsRef<Self> for FakeIpSocketCtx<I, D> {
        fn as_ref(&self) -> &Self {
            self
        }
    }

    impl<I: IpDeviceStateIpExt, D> AsMut<Self> for FakeIpSocketCtx<I, D> {
        fn as_mut(&mut self) -> &mut Self {
            self
        }
    }

    impl<
            I: IpExt + IpDeviceStateIpExt,
            C: CounterContext + InstantContext + TracingContext,
            DeviceId: FakeStrongDeviceId,
        > TransportIpContext<I, C> for FakeIpSocketCtx<I, DeviceId>
    {
        fn get_default_hop_limits(&mut self, device: Option<&Self::DeviceId>) -> HopLimits {
            device.map_or(DEFAULT_HOP_LIMITS, |device| {
                let hop_limit = self.get_device_state(device).default_hop_limit.read().clone();
                HopLimits { unicast: hop_limit, multicast: DEFAULT_HOP_LIMITS.multicast }
            })
        }

        type DevicesWithAddrIter<'a> = alloc::boxed::Box<dyn Iterator<Item = DeviceId> + 'a>;

        fn get_devices_with_assigned_addr(
            &mut self,
            addr: SpecifiedAddr<<I>::Addr>,
        ) -> Self::DevicesWithAddrIter<'_> {
            Box::new(self.find_devices_with_addr(addr))
        }

        fn confirm_reachable(
            &mut self,
            _ctx: &mut C,
            _device: &Self::DeviceId,
            _neighbor: SpecifiedAddr<I::Addr>,
        ) {
        }
    }

    impl<I: IpDeviceStateIpExt, DeviceId: FakeStrongDeviceId> DeviceIdContext<AnyDevice>
        for FakeIpSocketCtx<I, DeviceId>
    {
        type DeviceId = <FakeIpDeviceIdCtx<DeviceId> as DeviceIdContext<AnyDevice>>::DeviceId;
        type WeakDeviceId =
            <FakeIpDeviceIdCtx<DeviceId> as DeviceIdContext<AnyDevice>>::WeakDeviceId;

        fn downgrade_device_id(&self, device_id: &DeviceId) -> FakeWeakDeviceId<DeviceId> {
            self.ip_forwarding_ctx.downgrade_device_id(device_id)
        }

        fn upgrade_weak_device_id(
            &self,
            device_id: &FakeWeakDeviceId<DeviceId>,
        ) -> Option<DeviceId> {
            self.ip_forwarding_ctx.upgrade_weak_device_id(device_id)
        }
    }

    fn lookup_route<I: IpDeviceStateIpExt, D: FakeStrongDeviceId, Instant: crate::Instant>(
        table: &ForwardingTable<I, D>,
        ip_forwarding_ctx: &mut FakeIpForwardingCtx<D>,
        device_state: &HashMap<D, impl AsRef<IpDeviceState<Instant, I>>>,
        device: Option<&D>,
        local_ip: Option<SpecifiedAddr<I::Addr>>,
        addr: SpecifiedAddr<I::Addr>,
    ) -> Result<ResolvedRoute<I, D>, ResolveRouteError> {
        let (destination, ()) = table
            .lookup_filter_map(ip_forwarding_ctx, device, *addr, |_, d| match &local_ip {
                None => Some(()),
                Some(local_ip) => device_state
                    .get(d)
                    .and_then(|state| state.as_ref().addrs.read().find(local_ip).map(|_| ())),
            })
            .next()
            .ok_or(ResolveRouteError::Unreachable)?;

        let Destination { device, next_hop } = destination;
        let addrs = device_state.get(&device).unwrap().as_ref().addrs.read();
        let mut addrs = addrs.iter();
        let local_ip = match local_ip {
            None => addrs.map(|e| e.addr()).next().ok_or(ResolveRouteError::NoSrcAddr)?,
            Some(local_ip) => {
                // We already constrained the set of devices so this
                // should be a given.
                assert!(
                    addrs.any(|e| e.addr() == local_ip),
                    "didn't find IP {:?} in {:?}",
                    local_ip,
                    addrs.collect::<Vec<_>>()
                );
                local_ip
            }
        };

        Ok(ResolvedRoute { src_addr: local_ip, device: device.clone(), next_hop })
    }

    impl<
            I: IpDeviceStateIpExt,
            C: CounterContext + InstantContext + TracingContext,
            DeviceId: FakeStrongDeviceId,
        > IpSocketContext<I, C> for FakeIpSocketCtx<I, DeviceId>
    {
        fn lookup_route(
            &mut self,
            _ctx: &mut C,
            device: Option<&Self::DeviceId>,
            local_ip: Option<SpecifiedAddr<I::Addr>>,
            addr: SpecifiedAddr<I::Addr>,
        ) -> Result<ResolvedRoute<I, Self::DeviceId>, ResolveRouteError> {
            let FakeIpSocketCtx { device_state, table, ip_forwarding_ctx } = self;
            lookup_route(table, ip_forwarding_ctx, device_state, device, local_ip, addr)
        }
    }

    impl<
            I: IpDeviceStateIpExt,
            S: AsRef<FakeIpSocketCtx<I, DeviceId>>
                + AsMut<FakeIpSocketCtx<I, DeviceId>>
                + AsRef<FakeIpDeviceIdCtx<DeviceId>>,
            Id,
            Meta,
            Event: Debug,
            DeviceId: FakeStrongDeviceId,
            NonSyncCtxState,
        > IpSocketContext<I, FakeNonSyncCtx<Id, Event, NonSyncCtxState>>
        for FakeSyncCtx<S, Meta, DeviceId>
    {
        fn lookup_route(
            &mut self,
            ctx: &mut FakeNonSyncCtx<Id, Event, NonSyncCtxState>,
            device: Option<&Self::DeviceId>,
            local_ip: Option<SpecifiedAddr<I::Addr>>,
            addr: SpecifiedAddr<I::Addr>,
        ) -> Result<ResolvedRoute<I, Self::DeviceId>, ResolveRouteError> {
            self.get_mut().as_mut().lookup_route(ctx, device, local_ip, addr)
        }
    }

    impl<
            I: IpDeviceStateIpExt + packet_formats::ip::IpExt,
            B: BufferMut,
            S,
            Id,
            Meta,
            Event: Debug,
            DeviceId,
            NonSyncCtxState,
        > BufferIpSocketContext<I, FakeNonSyncCtx<Id, Event, NonSyncCtxState>, B>
        for FakeSyncCtx<S, Meta, DeviceId>
    where
        FakeSyncCtx<S, Meta, DeviceId>: SendFrameContext<
                FakeNonSyncCtx<Id, Event, NonSyncCtxState>,
                B,
                SendIpPacketMeta<I, Self::DeviceId, SpecifiedAddr<I::Addr>>,
            > + IpSocketContext<I, FakeNonSyncCtx<Id, Event, NonSyncCtxState>>,
    {
        fn send_ip_packet<SS: Serializer<Buffer = B>>(
            &mut self,
            ctx: &mut FakeNonSyncCtx<Id, Event, NonSyncCtxState>,
            SendIpPacketMeta {  device, src_ip, dst_ip, next_hop, proto, ttl, mtu }: SendIpPacketMeta<I, &Self::DeviceId, SpecifiedAddr<I::Addr>>,
            body: SS,
        ) -> Result<(), SS> {
            let meta = SendIpPacketMeta {
                device: device.clone(),
                src_ip,
                dst_ip,
                next_hop,
                proto,
                ttl,
                mtu,
            };
            self.send_frame(ctx, meta, body)
        }
    }

    impl<I: IpDeviceStateIpExt, D: FakeStrongDeviceId> FakeIpSocketCtx<I, D> {
        pub(crate) fn with_devices_state(
            devices: impl IntoIterator<
                Item = (D, IpDeviceState<FakeInstant, I>, Vec<SpecifiedAddr<I::Addr>>),
            >,
        ) -> Self {
            let mut table = ForwardingTable::default();
            let mut device_state = HashMap::default();
            for (device, state, addrs) in devices {
                for ip in addrs {
                    crate::ip::forwarding::testutil::add_on_link_forwarding_entry(
                        &mut table,
                        ip,
                        device.clone(),
                    )
                }
                assert!(
                    device_state.insert(device.clone(), state).is_none(),
                    "duplicate entries for {device:?}",
                );
            }

            FakeIpSocketCtx { table, device_state, ip_forwarding_ctx: Default::default() }
        }

        pub(crate) fn find_devices_with_addr(
            &self,
            addr: SpecifiedAddr<I::Addr>,
        ) -> impl Iterator<Item = D> + '_ {
            let Self { table: _, device_state, ip_forwarding_ctx: _ } = self;
            find_devices_with_addr::<I, _, _>(device_state, addr)
        }

        pub(crate) fn get_device_state(&self, device: &D) -> &IpDeviceState<FakeInstant, I> {
            let Self { device_state, table: _, ip_forwarding_ctx: _ } = self;
            device_state.get(device).unwrap_or_else(|| panic!("no device {device:?}"))
        }
    }

    fn find_devices_with_addr<
        I: IpDeviceStateIpExt,
        D: FakeStrongDeviceId,
        Instant: crate::Instant,
    >(
        device_state: &HashMap<D, impl AsRef<IpDeviceState<Instant, I>>>,
        addr: SpecifiedAddr<I::Addr>,
    ) -> impl Iterator<Item = D> + '_ {
        device_state.iter().filter_map(move |(device, state)| {
            state
                .as_ref()
                .addrs
                .read()
                .find(&addr)
                .map(|_: &PrimaryRc<I::AssignedAddress<_>>| device.clone())
        })
    }

    fn multicast_memberships<
        I: IpDeviceStateIpExt,
        D: FakeStrongDeviceId,
        Instant: crate::Instant,
    >(
        device_state: &HashMap<D, impl AsRef<IpDeviceState<Instant, I>>>,
    ) -> HashMap<(D, MulticastAddr<I::Addr>), NonZeroUsize> {
        device_state
            .iter()
            .flat_map(|(device, device_state)| {
                device_state
                    .as_ref()
                    .multicast_groups
                    .read()
                    .iter_counts()
                    .map(|(addr, count)| ((device.clone(), *addr), count))
                    .collect::<Vec<_>>()
            })
            .collect()
    }

    impl<
            I: IpDeviceStateIpExt,
            D: FakeStrongDeviceId,
            C: RngContext + InstantContext<Instant = FakeInstant>,
        > MulticastMembershipHandler<I, C> for FakeIpSocketCtx<I, D>
    {
        fn join_multicast_group(
            &mut self,
            ctx: &mut C,
            device: &Self::DeviceId,
            addr: MulticastAddr<<I as Ip>::Addr>,
        ) {
            let Self { device_state, table: _, ip_forwarding_ctx: _ } = self;
            let state =
                device_state.get_mut(device).unwrap_or_else(|| panic!("no device {device:?}"));
            state.multicast_groups.write().join_multicast_group(ctx, addr)
        }

        fn leave_multicast_group(
            &mut self,
            _ctx: &mut C,
            device: &Self::DeviceId,
            addr: MulticastAddr<<I as Ip>::Addr>,
        ) {
            let Self { device_state, table: _, ip_forwarding_ctx: _ } = self;
            let state =
                device_state.get_mut(device).unwrap_or_else(|| panic!("no device {device:?}"));
            state.multicast_groups.write().leave_multicast_group(addr)
        }
    }

    impl<
            I: IpExt + IpDeviceStateIpExt,
            C: CounterContext + InstantContext + TracingContext,
            D: FakeStrongDeviceId,
            State: TransportIpContext<I, C, DeviceId = D>,
            Meta,
        > TransportIpContext<I, C> for FakeSyncCtx<State, Meta, D>
    where
        Self: IpSocketContext<I, C, DeviceId = D, WeakDeviceId = FakeWeakDeviceId<D>>,
    {
        type DevicesWithAddrIter<'a> = State::DevicesWithAddrIter<'a>
            where Self: 'a;

        fn get_devices_with_assigned_addr(
            &mut self,
            addr: SpecifiedAddr<I::Addr>,
        ) -> Self::DevicesWithAddrIter<'_> {
            TransportIpContext::<I, C>::get_devices_with_assigned_addr(self.get_mut(), addr)
        }

        fn get_default_hop_limits(&mut self, device: Option<&Self::DeviceId>) -> HopLimits {
            TransportIpContext::<I, C>::get_default_hop_limits(self.get_mut(), device)
        }

        fn confirm_reachable(
            &mut self,
            ctx: &mut C,
            device: &Self::DeviceId,
            neighbor: SpecifiedAddr<I::Addr>,
        ) {
            TransportIpContext::<I, C>::confirm_reachable(self.get_mut(), ctx, device, neighbor)
        }
    }

    #[derive(Derivative)]
    #[derivative(Default(bound = ""))]
    pub(crate) struct FakeDualStackIpSocketCtx<D> {
        table: DualStackForwardingTable<D>,
        device_state: HashMap<D, DualStackIpDeviceState<FakeInstant>>,
        ip_forwarding_ctx: FakeIpForwardingCtx<D>,
    }

    impl<D: FakeStrongDeviceId> FakeDualStackIpSocketCtx<D> {
        pub(crate) fn new<A: Into<SpecifiedAddr<IpAddr>>>(
            devices: impl IntoIterator<Item = FakeDeviceConfig<D, A>>,
        ) -> Self {
            Self::with_devices_state(devices.into_iter().map(
                |FakeDeviceConfig { device, local_ips, remote_ips }| {
                    let mut device_state = DualStackIpDeviceState::default();
                    for ip in local_ips {
                        match IpAddr::from(ip.into()) {
                            IpAddr::V4(ip) => crate::ip::device::state::testutil::add_addr_subnet(
                                device_state.as_mut(),
                                ip,
                            ),
                            IpAddr::V6(ip) => crate::ip::device::state::testutil::add_addr_subnet(
                                device_state.as_mut(),
                                ip,
                            ),
                        }
                    }
                    (device, device_state, remote_ips)
                },
            ))
        }

        pub(crate) fn get_device_state(&self, device: &D) -> &DualStackIpDeviceState<FakeInstant> {
            let Self { device_state, table: _, ip_forwarding_ctx: _ } = self;
            device_state.get(device).unwrap_or_else(|| panic!("no device {device:?}"))
        }

        pub(crate) fn find_devices_with_addr<I: IpDeviceStateIpExt>(
            &self,
            addr: SpecifiedAddr<I::Addr>,
        ) -> impl Iterator<Item = D> + '_ {
            let Self { table: _, device_state, ip_forwarding_ctx: _ } = self;
            find_devices_with_addr::<I, _, _>(device_state, addr)
        }

        pub(crate) fn multicast_memberships<I: IpDeviceStateIpExt>(
            &self,
        ) -> HashMap<(D, MulticastAddr<I::Addr>), NonZeroUsize> {
            let Self { device_state, table: _, ip_forwarding_ctx: _ } = self;
            multicast_memberships::<I, _, _>(device_state)
        }

        pub(crate) fn with_devices_state(
            devices: impl IntoIterator<
                Item = (
                    D,
                    DualStackIpDeviceState<FakeInstant>,
                    Vec<impl Into<SpecifiedAddr<IpAddr>>>,
                ),
            >,
        ) -> Self {
            let mut table = DualStackForwardingTable::default();
            let mut device_state = HashMap::default();
            for (device, state, addrs) in devices {
                for ip in addrs {
                    match IpAddr::from(ip.into()) {
                        IpAddr::V4(ip) => {
                            crate::ip::forwarding::testutil::add_on_link_forwarding_entry(
                                table.as_mut(),
                                ip,
                                device.clone(),
                            );
                        }
                        IpAddr::V6(ip) => {
                            crate::ip::forwarding::testutil::add_on_link_forwarding_entry(
                                table.as_mut(),
                                ip,
                                device.clone(),
                            );
                        }
                    }
                }
                assert!(
                    device_state.insert(device.clone(), state).is_none(),
                    "duplicate entries for {device:?}",
                );
            }
            Self { table, device_state, ip_forwarding_ctx: FakeIpForwardingCtx::default() }
        }
    }

    impl<D: FakeStrongDeviceId> AsRef<Self> for FakeDualStackIpSocketCtx<D> {
        fn as_ref(&self) -> &Self {
            self
        }
    }

    impl<D: FakeStrongDeviceId> AsRef<FakeIpDeviceIdCtx<D>> for FakeDualStackIpSocketCtx<D> {
        fn as_ref(&self) -> &FakeIpDeviceIdCtx<D> {
            self.ip_forwarding_ctx.get_ref().as_ref()
        }
    }

    impl<D: FakeStrongDeviceId> AsMut<FakeIpDeviceIdCtx<D>> for FakeDualStackIpSocketCtx<D> {
        fn as_mut(&mut self) -> &mut FakeIpDeviceIdCtx<D> {
            self.ip_forwarding_ctx.get_mut().as_mut()
        }
    }

    impl<DeviceId: FakeStrongDeviceId> DeviceIdContext<AnyDevice>
        for FakeDualStackIpSocketCtx<DeviceId>
    {
        type DeviceId = DeviceId;
        type WeakDeviceId = DeviceId::Weak;

        fn downgrade_device_id(&self, device_id: &DeviceId) -> Self::WeakDeviceId {
            self.ip_forwarding_ctx.downgrade_device_id(device_id)
        }

        fn upgrade_weak_device_id(
            &self,
            device_id: &FakeWeakDeviceId<DeviceId>,
        ) -> Option<DeviceId> {
            self.ip_forwarding_ctx.upgrade_weak_device_id(device_id)
        }
    }

    impl<I: IpDeviceStateIpExt, DeviceId: FakeStrongDeviceId, C: IpSocketNonSyncContext>
        IpSocketContext<I, C> for FakeDualStackIpSocketCtx<DeviceId>
    {
        fn lookup_route(
            &mut self,
            _ctx: &mut C,
            device: Option<&Self::DeviceId>,
            local_ip: Option<SpecifiedAddr<I::Addr>>,
            addr: SpecifiedAddr<I::Addr>,
        ) -> Result<ResolvedRoute<I, Self::DeviceId>, ResolveRouteError> {
            let Self { table, device_state, ip_forwarding_ctx } = self;
            lookup_route(
                table.as_ref(),
                ip_forwarding_ctx.as_mut(),
                device_state,
                device,
                local_ip,
                addr,
            )
        }
    }

    impl<
            I: IpDeviceStateIpExt,
            Id,
            Meta,
            Event: Debug,
            DeviceId: FakeStrongDeviceId,
            NonSyncCtxState,
        > IpSocketContext<I, FakeNonSyncCtx<Id, Event, NonSyncCtxState>>
        for FakeSyncCtx<FakeDualStackIpSocketCtx<DeviceId>, Meta, DeviceId>
    {
        fn lookup_route(
            &mut self,
            ctx: &mut FakeNonSyncCtx<Id, Event, NonSyncCtxState>,
            device: Option<&Self::DeviceId>,
            local_ip: Option<SpecifiedAddr<I::Addr>>,
            addr: SpecifiedAddr<I::Addr>,
        ) -> Result<ResolvedRoute<I, Self::DeviceId>, ResolveRouteError> {
            self.get_mut().lookup_route(ctx, device, local_ip, addr)
        }
    }

    impl<
            I: IpDeviceStateIpExt,
            D: FakeStrongDeviceId,
            C: RngContext + InstantContext<Instant = FakeInstant>,
        > MulticastMembershipHandler<I, C> for FakeDualStackIpSocketCtx<D>
    {
        fn join_multicast_group(
            &mut self,
            ctx: &mut C,
            device: &Self::DeviceId,
            addr: MulticastAddr<<I as Ip>::Addr>,
        ) {
            let Self { device_state, table: _, ip_forwarding_ctx: _ } = self;
            let state =
                device_state.get_mut(device).unwrap_or_else(|| panic!("no device {device:?}"));
            let state: &IpDeviceState<_, I> = state.as_ref();
            let mut groups = state.multicast_groups.write();
            groups.join_multicast_group(ctx, addr)
        }

        fn leave_multicast_group(
            &mut self,
            _ctx: &mut C,
            device: &Self::DeviceId,
            addr: MulticastAddr<<I as Ip>::Addr>,
        ) {
            let Self { device_state, table: _, ip_forwarding_ctx: _ } = self;
            let state =
                device_state.get_mut(device).unwrap_or_else(|| panic!("no device {device:?}"));
            let state: &IpDeviceState<_, I> = state.as_ref();
            let mut groups = state.multicast_groups.write();
            groups.leave_multicast_group(addr)
        }
    }

    impl<
            I: IpExt + IpDeviceStateIpExt,
            C: CounterContext + InstantContext + TracingContext,
            DeviceId: FakeStrongDeviceId,
        > TransportIpContext<I, C> for FakeDualStackIpSocketCtx<DeviceId>
    {
        fn get_default_hop_limits(&mut self, device: Option<&Self::DeviceId>) -> HopLimits {
            device.map_or(DEFAULT_HOP_LIMITS, |device| {
                let state: &IpDeviceState<_, I> = self.get_device_state(device).as_ref();
                let hop_limit = state.default_hop_limit.read().clone();
                HopLimits { unicast: hop_limit, multicast: DEFAULT_HOP_LIMITS.multicast }
            })
        }

        type DevicesWithAddrIter<'a> = alloc::boxed::Box<dyn Iterator<Item = DeviceId> + 'a>;

        fn get_devices_with_assigned_addr(
            &mut self,
            addr: SpecifiedAddr<<I>::Addr>,
        ) -> Self::DevicesWithAddrIter<'_> {
            Box::new(self.find_devices_with_addr::<I>(addr))
        }

        fn confirm_reachable(
            &mut self,
            _ctx: &mut C,
            _device: &Self::DeviceId,
            _neighbor: SpecifiedAddr<I::Addr>,
        ) {
            unimplemented!()
        }
    }

    #[derive(Clone, GenericOverIp)]
    #[generic_over_ip()]
    pub(crate) struct FakeDeviceConfig<D, A> {
        pub(crate) device: D,
        pub(crate) local_ips: Vec<A>,
        pub(crate) remote_ips: Vec<A>,
    }

    impl<I: IpDeviceStateIpExt, D: FakeStrongDeviceId> FakeIpSocketCtx<I, D> {
        /// Creates a new `FakeIpSocketCtx<Ipv6>` with the given device
        /// configs.
        pub(crate) fn new(
            devices: impl IntoIterator<Item = FakeDeviceConfig<D, SpecifiedAddr<I::Addr>>>,
        ) -> Self {
            FakeIpSocketCtx::with_devices_state(devices.into_iter().map(
                |FakeDeviceConfig { device, local_ips, remote_ips }| {
                    let mut device_state = IpDeviceState::default();
                    for ip in local_ips {
                        crate::ip::device::state::testutil::add_addr_subnet(&mut device_state, ip);
                    }
                    (device, device_state, remote_ips)
                },
            ))
        }
    }
}

#[cfg(test)]
mod tests {
    use assert_matches::assert_matches;
    use const_unwrap::const_unwrap_option;
    use ip_test_macro::ip_test;
    use lock_order::Locked;
    use net_types::{
        ip::{
            AddrSubnet, GenericOverIp, IpAddr, IpAddress, IpInvariant, Ipv4, Ipv4Addr, Ipv6, Mtu,
        },
        Witness,
    };
    use packet::{Buf, InnerPacketBuilder, ParseBuffer};
    use packet_formats::{
        ethernet::EthernetFrameLengthCheck,
        icmp::{IcmpIpExt, IcmpUnusedCode},
        ip::{IpExt, IpPacket, Ipv4Proto},
        ipv4::{Ipv4OnlyMeta, Ipv4Packet},
        testutil::{parse_ethernet_frame, parse_ip_packet_in_ethernet_frame},
    };
    use test_case::test_case;

    use super::*;
    use crate::{
        context::{testutil::FakeInstant, EventContext},
        device::{testutil::FakeDeviceId, DeviceId},
        ip::{
            device::{
                IpDeviceConfigurationContext as DeviceIpDeviceConfigurationContext, IpDeviceEvent,
                IpDeviceIpExt, IpDeviceNonSyncContext,
            },
            types::{AddableEntryEither, AddableMetric, RawMetric},
            IpDeviceContext, IpLayerEvent, IpLayerIpExt, IpStateContext,
        },
        testutil::*,
        TimerContext,
    };

    enum AddressType {
        LocallyOwned,
        Remote,
        Unspecified {
            // Indicates whether or not it should be possible for the stack to
            // select an address when the client fails to specify one.
            can_select: bool,
        },
        Unroutable,
    }

    enum DeviceType {
        Unspecified,
        OtherDevice,
        LocalDevice,
    }

    struct NewSocketTestCase {
        local_ip_type: AddressType,
        remote_ip_type: AddressType,
        device_type: DeviceType,
        expected_result: Result<(), IpSockCreationError>,
    }

    trait IpSocketIpExt: Ip + TestIpExt + IcmpIpExt + IpExt + crate::ip::IpExt {
        const DISPATCH_RECEIVE_COUNTER: &'static str;
        fn multicast_addr(host: u8) -> SpecifiedAddr<Self::Addr>;
    }

    impl IpSocketIpExt for Ipv4 {
        const DISPATCH_RECEIVE_COUNTER: &'static str = "dispatch_receive_ipv4_packet";
        fn multicast_addr(host: u8) -> SpecifiedAddr<Self::Addr> {
            let [a, b, c, _] = Ipv4::MULTICAST_SUBNET.network().ipv4_bytes();
            SpecifiedAddr::new(Ipv4Addr::new([a, b, c, host])).unwrap()
        }
    }
    impl IpSocketIpExt for Ipv6 {
        const DISPATCH_RECEIVE_COUNTER: &'static str = "dispatch_receive_ipv6_packet";

        fn multicast_addr(host: u8) -> SpecifiedAddr<Self::Addr> {
            let mut bytes = Ipv6::MULTICAST_SUBNET.network().ipv6_bytes();
            bytes[15] = host;
            SpecifiedAddr::new(Ipv6Addr::from_bytes(bytes)).unwrap()
        }
    }

    #[derive(Copy, Clone, Debug, Eq, PartialEq)]
    struct WithHopLimit(Option<NonZeroU8>);

    impl<I: Ip> SendOptions<I> for WithHopLimit {
        fn hop_limit(&self, _destination: &SpecifiedAddr<I::Addr>) -> Option<NonZeroU8> {
            let Self(hop_limit) = self;
            *hop_limit
        }
    }

    fn remove_all_local_addrs<I: Ip + IpLayerIpExt + IpDeviceIpExt>(
        sync_ctx: &FakeSyncCtx,
        ctx: &mut FakeNonSyncCtx,
    ) where
        for<'a> Locked<&'a FakeSyncCtx, crate::lock_ordering::Unlocked>:
            DeviceIpDeviceConfigurationContext<
                I,
                FakeNonSyncCtx,
                DeviceId = DeviceId<FakeNonSyncCtx>,
            >,
        FakeNonSyncCtx: IpDeviceNonSyncContext<I, DeviceId<FakeNonSyncCtx>, Instant = FakeInstant>,
    {
        let devices = DeviceIpDeviceConfigurationContext::<I, _>::with_devices_and_state(
            &mut Locked::new(sync_ctx),
            |devices, _ctx| devices.collect::<Vec<_>>(),
        );
        for device in devices {
            #[derive(GenericOverIp)]
            #[generic_over_ip(I, Ip)]
            struct WrapVecAddrSubnet<I: Ip>(Vec<AddrSubnet<I::Addr>>);

            let WrapVecAddrSubnet(subnets) = I::map_ip(
                IpInvariant((&mut Locked::new(sync_ctx), &device)),
                |IpInvariant((sync_ctx, device))| {
                    crate::ip::device::with_assigned_ipv4_addr_subnets(sync_ctx, device, |addrs| {
                        WrapVecAddrSubnet(addrs.collect::<Vec<_>>())
                    })
                },
                |IpInvariant((sync_ctx, device))| {
                    crate::ip::device::testutil::with_assigned_ipv6_addr_subnets(
                        sync_ctx,
                        device,
                        |addrs| WrapVecAddrSubnet(addrs.collect::<Vec<_>>()),
                    )
                },
            );

            for subnet in subnets {
                crate::device::del_ip_addr(sync_ctx, ctx, &device, &subnet.addr())
                    .expect("failed to remove addr from device");
            }
        }
    }

    #[ip_test]
    #[test_case(NewSocketTestCase {
            local_ip_type: AddressType::Unroutable,
            remote_ip_type: AddressType::Remote,
            device_type: DeviceType::Unspecified,
            expected_result: Err(ResolveRouteError::NoSrcAddr.into()),
        }; "unroutable local to remote")]
    #[test_case(NewSocketTestCase {
            local_ip_type: AddressType::LocallyOwned,
            remote_ip_type: AddressType::Unroutable,
            device_type: DeviceType::Unspecified,
            expected_result: Err(ResolveRouteError::Unreachable.into()),
        }; "local to unroutable remote")]
    #[test_case(NewSocketTestCase {
            local_ip_type: AddressType::LocallyOwned,
            remote_ip_type: AddressType::Remote,
            device_type: DeviceType::Unspecified,
            expected_result: Ok(()),
        }; "local to remote")]
    #[test_case(NewSocketTestCase {
            local_ip_type: AddressType::Unspecified { can_select: true },
            remote_ip_type: AddressType::Remote,
            device_type: DeviceType::Unspecified,
            expected_result: Ok(()),
        }; "unspecified to remote")]
    #[test_case(NewSocketTestCase {
            local_ip_type: AddressType::Unspecified { can_select: true },
            remote_ip_type: AddressType::Remote,
            device_type: DeviceType::LocalDevice,
            expected_result: Ok(()),
        }; "unspecified to remote through local device")]
    #[test_case(NewSocketTestCase {
            local_ip_type: AddressType::Unspecified { can_select: true },
            remote_ip_type: AddressType::Remote,
            device_type: DeviceType::OtherDevice,
            expected_result: Err(ResolveRouteError::Unreachable.into()),
        }; "unspecified to remote through other device")]
    #[test_case(NewSocketTestCase {
            local_ip_type: AddressType::Unspecified { can_select: false },
            remote_ip_type: AddressType::Remote,
            device_type: DeviceType::Unspecified,
            expected_result: Err(ResolveRouteError::NoSrcAddr.into()),
        }; "new unspcified to remote can't select")]
    #[test_case(NewSocketTestCase {
            local_ip_type: AddressType::Remote,
            remote_ip_type: AddressType::Remote,
            device_type: DeviceType::Unspecified,
            expected_result: Err(ResolveRouteError::NoSrcAddr.into()),
        }; "new remote to remote")]
    #[test_case(NewSocketTestCase {
            local_ip_type: AddressType::LocallyOwned,
            remote_ip_type: AddressType::LocallyOwned,
            device_type: DeviceType::Unspecified,
            expected_result: Ok(()),
        }; "new local to local")]
    #[test_case(NewSocketTestCase {
            local_ip_type: AddressType::Unspecified { can_select: true },
            remote_ip_type: AddressType::LocallyOwned,
            device_type: DeviceType::Unspecified,
            expected_result: Ok(()),
        }; "new unspecified to local")]
    #[test_case(NewSocketTestCase {
            local_ip_type: AddressType::Remote,
            remote_ip_type: AddressType::LocallyOwned,
            device_type: DeviceType::Unspecified,
            expected_result: Err(ResolveRouteError::NoSrcAddr.into()),
        }; "new remote to local")]
    fn test_new<I: Ip + IpSocketIpExt + IpLayerIpExt + IpDeviceIpExt>(test_case: NewSocketTestCase)
    where
        for<'a> Locked<&'a FakeSyncCtx, crate::lock_ordering::Unlocked>:
            IpSocketHandler<I, FakeNonSyncCtx>
                + DeviceIdContext<AnyDevice, DeviceId = DeviceId<FakeNonSyncCtx>>
                + DeviceIpDeviceConfigurationContext<
                    I,
                    FakeNonSyncCtx,
                    DeviceId = DeviceId<FakeNonSyncCtx>,
                >,
        FakeNonSyncCtx: TimerContext<I::Timer<DeviceId<FakeNonSyncCtx>>>
            + EventContext<IpDeviceEvent<DeviceId<FakeNonSyncCtx>, I, FakeInstant>>,
    {
        let cfg = I::FAKE_CONFIG;
        let proto = I::ICMP_IP_PROTO;

        let FakeEventDispatcherConfig { local_ip, remote_ip, subnet, local_mac: _, remote_mac: _ } =
            cfg;
        let (Ctx { sync_ctx, mut non_sync_ctx }, device_ids) =
            FakeEventDispatcherBuilder::from_config(cfg).build();
        let sync_ctx = &sync_ctx;
        let loopback_device_id = crate::device::add_loopback_device(
            &sync_ctx,
            Mtu::new(u16::MAX as u32),
            DEFAULT_INTERFACE_METRIC,
        )
        .expect("create the loopback interface")
        .into();
        crate::device::testutil::enable_device(&sync_ctx, &mut non_sync_ctx, &loopback_device_id);

        let NewSocketTestCase { local_ip_type, remote_ip_type, expected_result, device_type } =
            test_case;

        let local_device = match device_type {
            DeviceType::Unspecified => None,
            DeviceType::LocalDevice => Some(device_ids[0].clone().into()),
            DeviceType::OtherDevice => Some(loopback_device_id.clone()),
        };

        let (expected_from_ip, from_ip) = match local_ip_type {
            AddressType::LocallyOwned => (local_ip, Some(local_ip)),
            AddressType::Remote => (remote_ip, Some(remote_ip)),
            AddressType::Unspecified { can_select } => {
                if !can_select {
                    remove_all_local_addrs::<I>(&sync_ctx, &mut non_sync_ctx);
                }
                (local_ip, None)
            }
            AddressType::Unroutable => {
                remove_all_local_addrs::<I>(&sync_ctx, &mut non_sync_ctx);
                (local_ip, Some(local_ip))
            }
        };

        let to_ip = match remote_ip_type {
            AddressType::LocallyOwned => local_ip,
            AddressType::Remote => remote_ip,
            AddressType::Unspecified { can_select: _ } => {
                panic!("remote_ip_type cannot be unspecified")
            }
            AddressType::Unroutable => {
                crate::testutil::del_routes_to_subnet(&sync_ctx, &mut non_sync_ctx, subnet.into())
                    .unwrap();
                remote_ip
            }
        };

        let get_expected_result = |template| expected_result.map(|()| template);
        let weak_local_device = local_device
            .as_ref()
            .map(|d| DeviceIdContext::downgrade_device_id(&Locked::new(sync_ctx), d));
        let template = IpSock {
            definition: IpSockDefinition {
                remote_ip: SocketIpAddr::new_from_specified_or_panic(to_ip),
                local_ip: SocketIpAddr::new_from_specified_or_panic(expected_from_ip),
                device: weak_local_device.clone(),
                proto,
            },
            options: WithHopLimit(None),
        };

        let res = IpSocketHandler::<I, _>::new_ip_socket(
            &mut Locked::new(sync_ctx),
            &mut non_sync_ctx,
            weak_local_device.as_ref().map(EitherDeviceId::Weak),
            from_ip.map(SocketIpAddr::new_from_specified_or_panic),
            SocketIpAddr::new_from_specified_or_panic(to_ip),
            proto,
            WithHopLimit(None),
        )
        .map_err(|(e, WithHopLimit(_))| e);
        assert_eq!(res, get_expected_result(template.clone()));

        // Hop Limit is specified.
        const SPECIFIED_HOP_LIMIT: NonZeroU8 = const_unwrap_option(NonZeroU8::new(1));
        assert_eq!(
            IpSocketHandler::new_ip_socket(
                &mut Locked::new(sync_ctx),
                &mut non_sync_ctx,
                weak_local_device.as_ref().map(EitherDeviceId::Weak),
                from_ip.map(SocketIpAddr::new_from_specified_or_panic),
                SocketIpAddr::new_from_specified_or_panic(to_ip),
                proto,
                WithHopLimit(Some(SPECIFIED_HOP_LIMIT)),
            )
            .map_err(|(e, WithHopLimit(_))| e),
            {
                // The template socket, but with the TTL set to 1.
                let mut template_with_hop_limit = template.clone();
                let IpSock { definition: _, options } = &mut template_with_hop_limit;
                *options = WithHopLimit(Some(SPECIFIED_HOP_LIMIT));
                get_expected_result(template_with_hop_limit)
            }
        );
    }

    #[ip_test]
    #[test_case(AddressType::LocallyOwned, AddressType::LocallyOwned; "local to local")]
    #[test_case(AddressType::Unspecified { can_select: true },
            AddressType::LocallyOwned; "unspecified to local")]
    #[test_case(AddressType::LocallyOwned, AddressType::Remote; "local to remote")]
    fn test_send_local<I: Ip + IpSocketIpExt>(
        from_addr_type: AddressType,
        to_addr_type: AddressType,
    ) where
        for<'a> Locked<&'a FakeSyncCtx, crate::lock_ordering::Unlocked>: BufferIpSocketHandler<I, FakeNonSyncCtx, packet::EmptyBuf>
            + DeviceIdContext<AnyDevice, DeviceId = DeviceId<FakeNonSyncCtx>>,
    {
        set_logger_for_test();

        use packet_formats::icmp::{IcmpEchoRequest, IcmpPacketBuilder};

        let FakeEventDispatcherConfig::<I::Addr> {
            subnet,
            local_ip,
            remote_ip,
            local_mac,
            remote_mac: _,
        } = I::FAKE_CONFIG;

        let mut builder = FakeEventDispatcherBuilder::default();
        let device_idx = builder.add_device(local_mac);
        let (Ctx { sync_ctx, mut non_sync_ctx }, device_ids) = builder.build();
        let device_id: DeviceId<_> = device_ids[device_idx].clone().into();
        let sync_ctx = &sync_ctx;
        crate::device::add_ip_addr_subnet(
            &sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            AddrSubnet::new(local_ip.get(), 16).unwrap(),
        )
        .unwrap();
        crate::device::add_ip_addr_subnet(
            &sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            AddrSubnet::new(remote_ip.get(), 16).unwrap(),
        )
        .unwrap();
        crate::testutil::add_route(
            &sync_ctx,
            &mut non_sync_ctx,
            AddableEntryEither::without_gateway(
                subnet.into(),
                device_id.clone(),
                AddableMetric::ExplicitMetric(RawMetric(0)),
            ),
        )
        .unwrap();

        let loopback_device_id = crate::device::add_loopback_device(
            &sync_ctx,
            Mtu::new(u16::MAX as u32),
            DEFAULT_INTERFACE_METRIC,
        )
        .expect("create the loopback interface")
        .into();
        crate::device::testutil::enable_device(&sync_ctx, &mut non_sync_ctx, &loopback_device_id);

        let (expected_from_ip, from_ip) = match from_addr_type {
            AddressType::LocallyOwned => (local_ip, Some(local_ip)),
            AddressType::Remote => panic!("from_addr_type cannot be remote"),
            AddressType::Unspecified { can_select: _ } => (local_ip, None),
            AddressType::Unroutable => panic!("from_addr_type cannot be unroutable"),
        };

        let to_ip = match to_addr_type {
            AddressType::LocallyOwned => local_ip,
            AddressType::Remote => remote_ip,
            AddressType::Unspecified { can_select: _ } => {
                panic!("to_addr_type cannot be unspecified")
            }
            AddressType::Unroutable => panic!("to_addr_type cannot be unroutable"),
        };

        let sock = IpSocketHandler::<I, _>::new_ip_socket(
            &mut Locked::new(sync_ctx),
            &mut non_sync_ctx,
            None,
            from_ip.map(SocketIpAddr::new_from_specified_or_panic),
            SocketIpAddr::new_from_specified_or_panic(to_ip),
            I::ICMP_IP_PROTO,
            DefaultSendOptions,
        )
        .unwrap();

        let reply = IcmpEchoRequest::new(0, 0).reply();
        let body = &[1, 2, 3, 4];
        let buffer = Buf::new(body.to_vec(), ..)
            .encapsulate(IcmpPacketBuilder::<I, _>::new(
                expected_from_ip.get(),
                to_ip.get(),
                IcmpUnusedCode,
                reply,
            ))
            .serialize_vec_outer()
            .unwrap();

        // Send an echo packet on the socket and validate that the packet is
        // delivered locally.
        BufferIpSocketHandler::<I, _, _>::send_ip_packet(
            &mut Locked::new(sync_ctx),
            &mut non_sync_ctx,
            &sock,
            buffer.into_inner().buffer_view().as_ref().into_serializer(),
            None,
        )
        .unwrap();

        handle_queued_rx_packets(sync_ctx, &mut non_sync_ctx);

        assert_eq!(non_sync_ctx.frames_sent().len(), 0);

        assert_eq!(get_counter_val(&non_sync_ctx, I::DISPATCH_RECEIVE_COUNTER), 1);
    }

    #[ip_test]
    fn test_send<I: Ip + IpSocketIpExt + IpLayerIpExt>()
    where
        for<'a> Locked<&'a FakeSyncCtx, crate::lock_ordering::Unlocked>: BufferIpSocketHandler<I, FakeNonSyncCtx, packet::EmptyBuf>
            + IpDeviceContext<I, FakeNonSyncCtx, DeviceId = DeviceId<FakeNonSyncCtx>>
            + IpStateContext<I, FakeNonSyncCtx>,
        FakeNonSyncCtx: EventContext<IpLayerEvent<DeviceId<FakeNonSyncCtx>, I>>,
    {
        // Test various edge cases of the
        // `BufferIpSocketContext::send_ip_packet` method.

        let cfg = I::FAKE_CONFIG;
        let proto = I::ICMP_IP_PROTO;
        let socket_options = WithHopLimit(Some(const_unwrap_option(NonZeroU8::new(1))));

        let FakeEventDispatcherConfig::<_> { local_mac, remote_mac, local_ip, remote_ip, subnet } =
            cfg;

        let (Ctx { sync_ctx, mut non_sync_ctx }, device_ids) =
            FakeEventDispatcherBuilder::from_config(cfg).build();
        let sync_ctx = &sync_ctx;
        // Create a normal, routable socket.
        let sock = IpSocketHandler::<I, _>::new_ip_socket(
            &mut Locked::new(sync_ctx),
            &mut non_sync_ctx,
            None,
            None,
            SocketIpAddr::new_from_specified_or_panic(remote_ip),
            proto,
            socket_options,
        )
        .unwrap();

        let curr_id = crate::ip::gen_ipv4_packet_id(&mut Locked::new(sync_ctx));

        let check_frame =
            move |frame: &[u8], packet_count| match [local_ip.get(), remote_ip.get()].into() {
                IpAddr::V4([local_ip, remote_ip]) => {
                    let (mut body, src_mac, dst_mac, _ethertype) =
                        parse_ethernet_frame(frame, EthernetFrameLengthCheck::NoCheck).unwrap();
                    let packet = (&mut body).parse::<Ipv4Packet<&[u8]>>().unwrap();
                    assert_eq!(src_mac, local_mac.get());
                    assert_eq!(dst_mac, remote_mac.get());
                    assert_eq!(packet.src_ip(), local_ip);
                    assert_eq!(packet.dst_ip(), remote_ip);
                    assert_eq!(packet.proto(), Ipv4::ICMP_IP_PROTO);
                    assert_eq!(packet.ttl(), 1);
                    let Ipv4OnlyMeta { id } = packet.version_specific_meta();
                    assert_eq!(usize::from(id), usize::from(curr_id) + packet_count);
                    assert_eq!(body, [0]);
                }
                IpAddr::V6([local_ip, remote_ip]) => {
                    let (body, src_mac, dst_mac, src_ip, dst_ip, ip_proto, ttl) =
                        parse_ip_packet_in_ethernet_frame::<Ipv6>(
                            frame,
                            EthernetFrameLengthCheck::NoCheck,
                        )
                        .unwrap();
                    assert_eq!(body, [0]);
                    assert_eq!(src_mac, local_mac.get());
                    assert_eq!(dst_mac, remote_mac.get());
                    assert_eq!(src_ip, local_ip);
                    assert_eq!(dst_ip, remote_ip);
                    assert_eq!(ip_proto, Ipv6::ICMP_IP_PROTO);
                    assert_eq!(ttl, 1);
                }
            };
        let mut packet_count = 0;
        assert_eq!(non_sync_ctx.frames_sent().len(), packet_count);

        // Send a packet on the socket and make sure that the right contents
        // are sent.
        BufferIpSocketHandler::<I, _, _>::send_ip_packet(
            &mut Locked::new(sync_ctx),
            &mut non_sync_ctx,
            &sock,
            (&[0u8][..]).into_serializer(),
            None,
        )
        .unwrap();
        let mut check_sent_frame = |non_sync_ctx: &crate::testutil::FakeNonSyncCtx| {
            packet_count += 1;
            assert_eq!(non_sync_ctx.frames_sent().len(), packet_count);
            let (dev, frame) = &non_sync_ctx.frames_sent()[packet_count - 1];
            assert_eq!(dev, &device_ids[0]);
            check_frame(&frame, packet_count);
        };
        check_sent_frame(&non_sync_ctx);

        // Send a packet while imposing an MTU that is large enough to fit the
        // packet.
        let small_body = [0; 1];
        let small_body_serializer = (&small_body).into_serializer();
        let res = BufferIpSocketHandler::<I, _, _>::send_ip_packet(
            &mut Locked::new(sync_ctx),
            &mut non_sync_ctx,
            &sock,
            small_body_serializer,
            Some(Ipv6::MINIMUM_LINK_MTU.into()),
        );
        assert_matches!(res, Ok(()));
        check_sent_frame(&non_sync_ctx);

        // Send a packet on the socket while imposing an MTU which will not
        // allow a packet to be sent.
        let res = BufferIpSocketHandler::<I, _, _>::send_ip_packet(
            &mut Locked::new(sync_ctx),
            &mut non_sync_ctx,
            &sock,
            small_body_serializer,
            Some(1), // mtu
        );
        assert_matches!(res, Err((_, IpSockSendError::Mtu)));

        assert_eq!(non_sync_ctx.frames_sent().len(), packet_count);
        // Try sending a packet which will be larger than the device's MTU,
        // and make sure it fails.
        let res = BufferIpSocketHandler::<I, _, _>::send_ip_packet(
            &mut Locked::new(sync_ctx),
            &mut non_sync_ctx,
            &sock,
            (&[0; crate::ip::Ipv6::MINIMUM_LINK_MTU.get() as usize][..]).into_serializer(),
            None,
        );
        assert_matches!(res, Err((_, IpSockSendError::Mtu)));

        // Make sure that sending on an unroutable socket fails.
        crate::ip::forwarding::testutil::del_routes_to_subnet::<I, _, _>(
            &mut Locked::new(sync_ctx),
            &mut non_sync_ctx,
            subnet,
        )
        .unwrap();
        let res = BufferIpSocketHandler::<I, _, _>::send_ip_packet(
            &mut Locked::new(sync_ctx),
            &mut non_sync_ctx,
            &sock,
            small_body_serializer,
            None,
        );
        assert_matches!(res, Err((_, IpSockSendError::Unroutable(ResolveRouteError::Unreachable))));
    }

    #[ip_test]
    fn test_send_hop_limits<I: Ip + IpSocketIpExt + IpLayerIpExt>()
    where
        for<'a> Locked<&'a FakeSyncCtx, crate::lock_ordering::Unlocked>: BufferIpSocketHandler<I, FakeNonSyncCtx, packet::EmptyBuf>
            + IpDeviceContext<I, FakeNonSyncCtx, DeviceId = DeviceId<FakeNonSyncCtx>>
            + IpStateContext<I, FakeNonSyncCtx>,
    {
        set_logger_for_test();

        #[derive(Copy, Clone, Debug)]
        struct SetHopLimitFor<A>(SpecifiedAddr<A>);

        const SET_HOP_LIMIT: NonZeroU8 = const_unwrap_option(NonZeroU8::new(42));

        impl<A: IpAddress> SendOptions<A::Version> for SetHopLimitFor<A> {
            fn hop_limit(&self, destination: &SpecifiedAddr<A>) -> Option<NonZeroU8> {
                let Self(expected_destination) = self;
                (destination == expected_destination).then_some(SET_HOP_LIMIT)
            }
        }

        let FakeEventDispatcherConfig::<I::Addr> {
            local_ip,
            remote_ip: _,
            local_mac,
            subnet: _,
            remote_mac: _,
        } = I::FAKE_CONFIG;

        let mut builder = FakeEventDispatcherBuilder::default();
        let device_idx = builder.add_device(local_mac);
        let (Ctx { sync_ctx, mut non_sync_ctx }, device_ids) = builder.build();
        let device_id: DeviceId<_> = device_ids[device_idx].clone().into();
        let sync_ctx = &sync_ctx;
        crate::device::add_ip_addr_subnet(
            &sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            AddrSubnet::new(local_ip.get(), 16).unwrap(),
        )
        .unwrap();

        // Use multicast remote addresses since unicast addresses would trigger
        // ARP/NDP requests.
        crate::testutil::add_route(
            &sync_ctx,
            &mut non_sync_ctx,
            AddableEntryEither::without_gateway(
                I::MULTICAST_SUBNET.into(),
                device_id.clone(),
                AddableMetric::ExplicitMetric(RawMetric(0)),
            ),
        )
        .expect("add device route");
        let remote_ip = I::multicast_addr(0);
        let options = SetHopLimitFor(remote_ip);
        let other_remote_ip = I::multicast_addr(1);

        let mut send_to = |destination_ip| {
            let sock = IpSocketHandler::<I, _>::new_ip_socket(
                &mut Locked::new(sync_ctx),
                &mut non_sync_ctx,
                None,
                None,
                destination_ip,
                I::ICMP_IP_PROTO,
                options,
            )
            .unwrap();

            BufferIpSocketHandler::<I, _, _>::send_ip_packet(
                &mut Locked::new(sync_ctx),
                &mut non_sync_ctx,
                &sock,
                (&[0u8][..]).into_serializer(),
                None,
            )
            .unwrap();
        };

        // Send to two remote addresses: `remote_ip` and `other_remote_ip` and
        // check that the frames were sent with the correct hop limits.
        send_to(SocketIpAddr::new_from_specified_or_panic(remote_ip));
        send_to(SocketIpAddr::new_from_specified_or_panic(other_remote_ip));

        let frames = non_sync_ctx.frames_sent();
        let [df_remote, df_other_remote] = assert_matches!(&frames[..], [df1, df2] => [df1, df2]);
        {
            let (_dev, frame) = df_remote;
            let (_body, _src_mac, _dst_mac, _src_ip, dst_ip, _ip_proto, hop_limit) =
                parse_ip_packet_in_ethernet_frame::<I>(&frame, EthernetFrameLengthCheck::NoCheck)
                    .unwrap();
            assert_eq!(dst_ip, remote_ip.get());
            // The `SetHopLimit`-returned value should take precedence.
            assert_eq!(hop_limit, SET_HOP_LIMIT.get());
        }

        {
            let (_dev, frame) = df_other_remote;
            let (_body, _src_mac, _dst_mac, _src_ip, dst_ip, _ip_proto, hop_limit) =
                parse_ip_packet_in_ethernet_frame::<I>(&frame, EthernetFrameLengthCheck::NoCheck)
                    .unwrap();
            assert_eq!(dst_ip, other_remote_ip.get());
            // When the options object does not provide a hop limit the default
            // is used.
            assert_eq!(hop_limit, crate::ip::DEFAULT_HOP_LIMITS.unicast.get());
        }
    }

    #[test]
    fn manipulate_options() {
        // The values here don't matter since we won't actually be using this
        // socket to send anything.
        const START_OPTION: usize = 23;
        const DEFAULT_OPTION: usize = 0;
        const NEW_OPTION: usize = 55;
        let mut socket = IpSock::<Ipv4, FakeDeviceId, _> {
            definition: IpSockDefinition {
                remote_ip: Ipv4::LOOPBACK_ADDRESS_AS_SOCKET_IP_ADDR,
                local_ip: Ipv4::LOOPBACK_ADDRESS_AS_SOCKET_IP_ADDR,
                device: None,
                proto: Ipv4Proto::Icmp,
            },
            options: START_OPTION,
        };

        assert_eq!(socket.take_options(), START_OPTION);
        assert_eq!(socket.replace_options(NEW_OPTION), DEFAULT_OPTION);
        assert_eq!(socket.options(), &NEW_OPTION);
        assert_eq!(socket.into_options(), NEW_OPTION);
    }

    #[ip_test]
    #[test_case(true; "remove device")]
    #[test_case(false; "dont remove device")]
    fn get_mms_device_removed<I: Ip + IpSocketIpExt + IpLayerIpExt>(remove_device: bool)
    where
        for<'a> Locked<&'a FakeSyncCtx, crate::lock_ordering::Unlocked>: BufferIpSocketHandler<I, FakeNonSyncCtx, packet::EmptyBuf>
            + IpDeviceContext<I, FakeNonSyncCtx, DeviceId = DeviceId<FakeNonSyncCtx>>
            + IpStateContext<I, FakeNonSyncCtx>
            + DeviceIpSocketHandler<I, FakeNonSyncCtx>,
    {
        set_logger_for_test();

        let FakeEventDispatcherConfig::<I::Addr> {
            local_ip,
            remote_ip: _,
            local_mac,
            subnet: _,
            remote_mac: _,
        } = I::FAKE_CONFIG;

        let mut builder = FakeEventDispatcherBuilder::default();
        let device_idx = builder.add_device(local_mac);
        let (Ctx { sync_ctx, mut non_sync_ctx }, device_ids) = builder.build();
        let eth_device_id = device_ids[device_idx].clone();
        core::mem::drop(device_ids);
        let device_id: DeviceId<_> = eth_device_id.clone().into();
        let sync_ctx = &sync_ctx;
        crate::device::add_ip_addr_subnet(
            &sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            AddrSubnet::new(local_ip.get(), 16).unwrap(),
        )
        .unwrap();
        crate::testutil::add_route(
            &sync_ctx,
            &mut non_sync_ctx,
            AddableEntryEither::without_gateway(
                I::MULTICAST_SUBNET.into(),
                device_id.clone(),
                AddableMetric::ExplicitMetric(RawMetric(0)),
            ),
        )
        .unwrap();

        let ip_sock = IpSocketHandler::<I, _>::new_ip_socket(
            &mut Locked::new(sync_ctx),
            &mut non_sync_ctx,
            None,
            None,
            SocketIpAddr::new_from_specified_or_panic(I::multicast_addr(1)),
            I::ICMP_IP_PROTO,
            WithHopLimit(None),
        )
        .unwrap();

        let expected = if remove_device {
            // Don't keep any strong device IDs to the device before removing.
            core::mem::drop(device_id);
            crate::device::remove_ethernet_device(&sync_ctx, &mut non_sync_ctx, eth_device_id)
                .into_removed();
            Err(MmsError::NoDevice(ResolveRouteError::Unreachable))
        } else {
            Ok(Mms::from_mtu::<I>(
                IpDeviceContext::<I, _>::get_mtu(&mut Locked::new(sync_ctx), &device_id),
                0, /* no ip options/ext hdrs used */
            )
            .unwrap())
        };

        assert_eq!(
            DeviceIpSocketHandler::get_mms(&mut Locked::new(sync_ctx), &mut non_sync_ctx, &ip_sock),
            expected,
        );
    }
}
