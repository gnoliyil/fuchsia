// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Link-layer sockets (analogous to Linux's AF_PACKET sockets).

use alloc::collections::HashSet;
use core::{fmt::Debug, hash::Hash, num::NonZeroU16};

use dense_map::{DenseMap, EntryKey};
use derivative::Derivative;
use lock_order::{
    lock::{LockFor, RwLockFor},
    relation::LockBefore,
    Locked,
};
use net_types::ethernet::Mac;
use packet::{BufferMut, ParsablePacket as _, Serializer};
use packet_formats::{
    error::ParseError,
    ethernet::{EtherType, EthernetFrameLengthCheck},
};

use crate::{
    context::SendFrameContext,
    device::{self, AnyDevice, Device, DeviceId, DeviceIdContext, FrameDestination, WeakDeviceId},
    sync::{Mutex, PrimaryRc, RwLock, StrongRc},
    SyncCtx,
};

/// A selector for frames based on link-layer protocol number.
#[derive(Copy, Clone, Debug, Eq, Hash, PartialEq)]
pub enum Protocol {
    /// Select all frames, regardless of protocol number.
    All,
    /// Select frames with the given protocol number.
    Specific(NonZeroU16),
}

/// Selector for devices to send and receive packets on.
#[derive(Clone, Debug, Derivative, Eq, Hash, PartialEq)]
#[derivative(Default(bound = ""))]
pub enum TargetDevice<D> {
    /// Act on any device in the system.
    #[derivative(Default)]
    AnyDevice,
    /// Act on a specific device.
    SpecificDevice(D),
}

/// Information about the bound state of a socket.
#[derive(Debug)]
#[cfg_attr(test, derive(PartialEq))]
pub struct SocketInfo<D> {
    /// The protocol the socket is bound to, or `None` if no protocol is set.
    pub protocol: Option<Protocol>,
    /// The device selector for which the socket is set.
    pub device: TargetDevice<D>,
}

/// Provides associated types for device sockets provided by the bindings
/// context.
pub trait DeviceSocketTypes {
    /// State for the socket held by core and exposed to bindings.
    type SocketState: Send + Sync + Debug;
}

/// The execution context for device sockets provided by bindings.
pub trait DeviceSocketBindingsContext<DeviceId>: DeviceSocketTypes {
    /// Called for each received frame that matches the provided socket.
    ///
    /// `frame` and `raw_frame` are parsed and raw views into the same data.
    fn receive_frame(
        &self,
        socket: &Self::SocketState,
        device: &DeviceId,
        frame: Frame<&[u8]>,
        raw_frame: &[u8],
    );
}

/// Strong owner of socket state.
///
/// This type strongly owns the socket state.
#[derive(Debug)]
pub(crate) struct PrimaryId<S, D>(PrimaryRc<SocketState<S, D>>);

/// Reference to live socket state.
///
/// The existence of a `StrongId` attests to the liveness of the state of the
/// backing socket.
#[derive(Debug, Derivative)]
#[derivative(Clone(bound = ""), Hash(bound = ""))]
pub(crate) struct StrongId<S, D>(StrongRc<SocketState<S, D>>);

impl<S, D> PartialEq for StrongId<S, D> {
    fn eq(&self, StrongId(other): &Self) -> bool {
        let Self(strong) = self;
        StrongRc::ptr_eq(strong, other)
    }
}

impl<S, D> Eq for StrongId<S, D> {}

impl<S, D> EntryKey for StrongId<S, D> {
    fn get_key_index(&self) -> usize {
        let Self(strong) = self;
        let SocketState { external_state: _, all_sockets_index, target: _ } = &**strong;
        *all_sockets_index
    }
}

trait StrongSocketId {
    type Primary;
}

impl<S, D> StrongSocketId for StrongId<S, D> {
    type Primary = PrimaryId<S, D>;
}

/// Holds shared state for sockets.
#[derive(Derivative)]
#[derivative(Default(bound = ""))]
pub(super) struct Sockets<Primary, Strong> {
    /// Holds strong (but not owning) references to sockets that aren't
    /// targeting a particular device.
    any_device_sockets: RwLock<AnyDeviceSockets<Strong>>,

    /// Table of all sockets in the system, regardless of target.
    ///
    /// Holds the primary (owning) reference for all sockets.
    // This needs to be after `any_device_sockets` so that when an instance of
    // this type is dropped, any strong IDs get dropped before their
    // corresponding primary IDs.
    all_sockets: Mutex<AllSockets<Primary>>,
}

#[derive(Derivative)]
#[derivative(Default(bound = ""))]
pub(crate) struct AnyDeviceSockets<Id>(HashSet<Id>);

#[derive(Derivative)]
#[derivative(Default(bound = ""))]
pub(crate) struct AllSockets<Id>(DenseMap<Id>);

#[derive(Debug)]
struct SocketState<S, D> {
    /// The index into `Sockets::all_sockets` for this state.
    all_sockets_index: usize,
    /// State provided by bindings that is held in core.
    external_state: S,
    /// The socket's target device and protocol.
    // TODO(https://fxbug.dev/126263): Consider splitting up the state here to
    // improve performance.
    target: Mutex<Target<D>>,
}

#[derive(Debug, Derivative)]
#[derivative(Default(bound = ""))]
struct Target<D> {
    protocol: Option<Protocol>,
    device: TargetDevice<D>,
}

/// Per-device state for packet sockets.
///
/// Holds sockets that are bound to a particular device. An instance of this
/// should be held in the state for each device in the system.
#[derive(Derivative)]
#[derivative(Default(bound = ""))]
#[cfg_attr(test, derivative(Debug, PartialEq(bound = "Id: Hash + Eq")))]
pub(crate) struct DeviceSockets<Id>(HashSet<Id>);

/// Convenience alias for use in device state storage.
pub(super) type HeldDeviceSockets<BT> =
    DeviceSockets<StrongId<<BT as DeviceSocketTypes>::SocketState, WeakDeviceId<BT>>>;

/// Convenience alias for use in shared storage.
///
/// The type parameter is expected to implement [`crate::NonSyncContext`].
pub(super) type HeldSockets<BT> = Sockets<
    PrimaryId<<BT as DeviceSocketTypes>::SocketState, WeakDeviceId<BT>>,
    StrongId<<BT as DeviceSocketTypes>::SocketState, WeakDeviceId<BT>>,
>;

/// Common types across all core context traits for device sockets.
trait DeviceSocketContextTypes {
    /// The strongly-owning socket ID type.
    ///
    /// This type is held in various data structures and its existence
    /// indicates liveness of socket state, but not ownership.
    type SocketId: Clone + Debug + Eq + Hash + StrongSocketId;
}

/// Core context for accessing socket state.
trait DeviceSocketContext<BC: DeviceSocketBindingsContext<Self::DeviceId>>:
    DeviceSocketAccessor<BC>
{
    /// The synchronized context available in callbacks to methods on this
    /// context.
    type SocketTablesCoreCtx<'a>: DeviceSocketAccessor<
        BC,
        DeviceId = Self::DeviceId,
        WeakDeviceId = Self::WeakDeviceId,
        SocketId = Self::SocketId,
    >;

    /// Creates a new socket with the given external state.
    ///
    /// The ID returned by this method must be removed by calling
    /// [`SyncContext::remove_socket`] once it is no longer in use.
    fn create_socket(&mut self, state: BC::SocketState) -> Self::SocketId;

    /// Removes a socket.
    ///
    /// # Panics
    ///
    /// This will panic if the provided socket ID is not the last instance.
    fn remove_socket(&mut self, socket: Self::SocketId);

    /// Executes the provided callback with immutable access to socket state.
    fn with_any_device_sockets<
        F: FnOnce(&AnyDeviceSockets<Self::SocketId>, &mut Self::SocketTablesCoreCtx<'_>) -> R,
        R,
    >(
        &mut self,
        cb: F,
    ) -> R;

    /// Executes the provided callback with mutable access to socket state.
    fn with_any_device_sockets_mut<
        F: FnOnce(&mut AnyDeviceSockets<Self::SocketId>, &mut Self::SocketTablesCoreCtx<'_>) -> R,
        R,
    >(
        &mut self,
        cb: F,
    ) -> R;
}

/// Synchronized context for accessing the state of an individual socket.
trait SocketStateAccessor<BC: DeviceSocketBindingsContext<Self::DeviceId>>:
    DeviceSocketContextTypes + DeviceIdContext<AnyDevice>
{
    /// Synchronized context available in callbacks to methods on this context.
    type SocketStateCoreCtx<'a>: DeviceIdContext<
        AnyDevice,
        DeviceId = Self::DeviceId,
        WeakDeviceId = Self::WeakDeviceId,
    >;

    /// Provides read-only access to the state of a socket.
    fn with_socket_state<
        F: FnOnce(
            &BC::SocketState,
            &Target<Self::WeakDeviceId>,
            &mut Self::SocketStateCoreCtx<'_>,
        ) -> R,
        R,
    >(
        &mut self,
        socket: &Self::SocketId,
        cb: F,
    ) -> R;

    /// Provides mutable access to the state of a socket.
    fn with_socket_state_mut<
        F: FnOnce(
            &BC::SocketState,
            &mut Target<Self::WeakDeviceId>,
            &mut Self::SocketStateCoreCtx<'_>,
        ) -> R,
        R,
    >(
        &mut self,
        socket: &Self::SocketId,
        cb: F,
    ) -> R;
}

/// Synchronized context for accessing the socket state for a device.
trait DeviceSocketAccessor<BC: DeviceSocketBindingsContext<Self::DeviceId>>:
    SocketStateAccessor<BC>
{
    /// Synchronized context available in callbacks to methods on this context.
    type DeviceSocketCoreCtx<'a>: SocketStateAccessor<
        BC,
        SocketId = Self::SocketId,
        DeviceId = Self::DeviceId,
        WeakDeviceId = Self::WeakDeviceId,
    >;

    /// Executes the provided callback with immutable access to device-specific
    /// socket state.
    fn with_device_sockets<
        F: FnOnce(&DeviceSockets<Self::SocketId>, &mut Self::DeviceSocketCoreCtx<'_>) -> R,
        R,
    >(
        &mut self,
        device: &Self::DeviceId,
        cb: F,
    ) -> R;

    // Executes the provided callback with mutable access to device-specific
    // socket state.
    fn with_device_sockets_mut<
        F: FnOnce(&mut DeviceSockets<Self::SocketId>, &mut Self::DeviceSocketCoreCtx<'_>) -> R,
        R,
    >(
        &mut self,
        device: &Self::DeviceId,
        cb: F,
    ) -> R;
}

/// Internal implementation trait that allows abstracting over device ID types.
trait DeviceSocketApi<BC: DeviceSocketBindingsContext<Self::DeviceId>>:
    DeviceSocketContextTypes + DeviceIdContext<AnyDevice>
{
    /// Creates a new packet socket.
    fn create(&mut self, external_state: BC::SocketState) -> Self::SocketId;

    /// Sets the device for a packet socket without affecting the protocol.
    fn set_device(&mut self, socket: &Self::SocketId, device: TargetDevice<&Self::DeviceId>);

    /// Sets both the device and protocol for a packet socket.
    fn set_device_and_protocol(
        &mut self,
        id: &Self::SocketId,
        device: TargetDevice<&Self::DeviceId>,
        protocol: Protocol,
    );

    /// Gets information about a socket.
    fn get_info(&mut self, id: &Self::SocketId) -> SocketInfo<Self::WeakDeviceId>;

    /// Removes a packet socket.
    fn remove(&mut self, id: Self::SocketId);

    /// Sends a frame exactly as provided, or returns an error.
    fn send_frame<S>(
        &mut self,
        bindings_ctx: &mut BC,
        socket: &Self::SocketId,
        params: SendFrameParams<Self::DeviceId>,
        body: S,
    ) -> Result<(), (S, SendFrameError)>
    where
        S: Serializer,
        S::Buffer: BufferMut;

    /// Sends a datagram with a constructed link-layer header or returns an
    /// error.
    fn send_datagram<S>(
        &mut self,
        bindings_ctx: &mut BC,
        socket: &Self::SocketId,
        params: SendDatagramParams<Self::DeviceId>,
        body: S,
    ) -> Result<(), (S, SendDatagramError)>
    where
        S: Serializer,
        S::Buffer: BufferMut;
}

/// An error encountered when sending a frame.
#[derive(Copy, Clone, Debug, PartialEq)]
pub enum SendFrameError {
    /// The socket is not bound to a device and no egress device was specified.
    NoDevice,
    /// The device failed to send the frame.
    SendFailed,
}

/// An error encountered when sending a datagram.
#[derive(Copy, Clone, Debug, PartialEq)]
pub enum SendDatagramError {
    /// There was a problem sending the constructed frame.
    Frame(SendFrameError),
    /// No protocol number was provided.
    NoProtocol,
}

/// The destination to use when sending a datagram.
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct SendDatagramParams<D> {
    /// The frame-level sending parameters.
    pub frame: SendFrameParams<D>,
    /// The protocol to use, or `None` to use the socket's bound protocol.
    pub protocol: Option<NonZeroU16>,
    /// The destination address.
    pub dest_addr: Mac,
}

/// The destination to use when sending a frame.
#[derive(Debug, Clone, Derivative, Eq, PartialEq)]
#[derivative(Default(bound = ""))]
pub struct SendFrameParams<D> {
    /// The egress device, or `None` to use the socket's bound device.
    pub device: Option<D>,
}

enum MaybeUpdate<T> {
    NoChange,
    NewValue(T),
}

fn update_device_and_protocol<
    CC: DeviceSocketContext<BC>,
    BC: DeviceSocketBindingsContext<CC::DeviceId>,
>(
    core_ctx: &mut CC,
    socket: &CC::SocketId,
    new_device: TargetDevice<&CC::DeviceId>,
    protocol_update: MaybeUpdate<Protocol>,
) {
    core_ctx.with_any_device_sockets_mut(|AnyDeviceSockets(any_device_sockets), core_ctx| {
        // Even if we're never moving the socket from/to the any-device
        // state, we acquire the lock to make the move between devices
        // atomic from the perspective of frame delivery. Otherwise there
        // would be a brief period during which arriving frames wouldn't be
        // delivered to the socket from either device.
        let old_device = core_ctx.with_socket_state_mut(
            socket,
            |_: &BC::SocketState, Target { protocol, device }, core_ctx| {
                match protocol_update {
                    MaybeUpdate::NewValue(p) => *protocol = Some(p),
                    MaybeUpdate::NoChange => (),
                };
                let old_device = match &device {
                    TargetDevice::SpecificDevice(device) => core_ctx.upgrade_weak_device_id(device),
                    TargetDevice::AnyDevice => {
                        assert!(any_device_sockets.remove(socket));
                        None
                    }
                };
                *device = match &new_device {
                    TargetDevice::AnyDevice => TargetDevice::AnyDevice,
                    TargetDevice::SpecificDevice(d) => {
                        TargetDevice::SpecificDevice(core_ctx.downgrade_device_id(d))
                    }
                };
                old_device
            },
        );

        // This modification occurs without holding the socket's individual
        // lock. That's safe because all modifications to the socket's
        // device are done within a `with_sockets_mut` call, which
        // synchronizes them.

        if let Some(device) = old_device {
            // Remove the reference to the socket from the old device if
            // there is one, and it hasn't been removed.
            core_ctx.with_device_sockets_mut(
                &device,
                |DeviceSockets(device_sockets), _core_ctx| {
                    assert!(device_sockets.remove(socket), "socket not found in device state");
                },
            );
        }

        // Add the reference to the new device, if there is one.
        match &new_device {
            TargetDevice::SpecificDevice(new_device) => core_ctx.with_device_sockets_mut(
                new_device,
                |DeviceSockets(device_sockets), _core_ctx| {
                    assert!(device_sockets.insert(socket.clone()));
                },
            ),
            TargetDevice::AnyDevice => {
                assert!(any_device_sockets.insert(socket.clone()))
            }
        }
    })
}

impl<CC, BC> DeviceSocketApi<BC> for CC
where
    CC: DeviceSocketContext<BC> + SendFrameContext<BC, DeviceSocketMetadata<CC::DeviceId>>,
    BC: DeviceSocketBindingsContext<CC::DeviceId>,
{
    fn create(&mut self, external_state: BC::SocketState) -> Self::SocketId {
        let strong = self.create_socket(external_state);
        self.with_any_device_sockets_mut(|AnyDeviceSockets(any_device_sockets), _core_ctx| {
            // On creation, sockets do not target any device or protocol.
            // Inserting them into the `any_device_sockets` table lets us treat
            // newly-created sockets uniformly with sockets whose target device
            // or protocol was set. The difference is unobservable at runtime
            // since newly-created sockets won't match any frames being
            // delivered.
            assert!(any_device_sockets.insert(strong.clone()));
        });
        strong
    }

    fn set_device(&mut self, socket: &Self::SocketId, device: TargetDevice<&CC::DeviceId>) {
        update_device_and_protocol(self, socket, device, MaybeUpdate::NoChange)
    }

    fn set_device_and_protocol(
        &mut self,
        socket: &Self::SocketId,
        device: TargetDevice<&CC::DeviceId>,
        protocol: Protocol,
    ) {
        update_device_and_protocol(self, socket, device, MaybeUpdate::NewValue(protocol))
    }

    fn get_info(&mut self, id: &Self::SocketId) -> SocketInfo<Self::WeakDeviceId> {
        self.with_socket_state(id, |_external_state, Target { device, protocol }, _core_ctx| {
            SocketInfo { device: device.clone(), protocol: *protocol }
        })
    }

    fn remove(&mut self, id: Self::SocketId) {
        self.with_any_device_sockets_mut(|AnyDeviceSockets(any_device_sockets), core_ctx| {
            let old_device =
                core_ctx.with_socket_state_mut(&id, |_: &BC::SocketState, target, core_ctx| {
                    let Target { device, protocol: _ } = target;
                    match &device {
                        TargetDevice::SpecificDevice(device) => {
                            core_ctx.upgrade_weak_device_id(device)
                        }
                        TargetDevice::AnyDevice => {
                            assert!(any_device_sockets.remove(&id));
                            None
                        }
                    }
                });
            if let Some(device) = old_device {
                core_ctx.with_device_sockets_mut(
                    &device,
                    |DeviceSockets(device_sockets), _core_ctx| {
                        assert!(device_sockets.remove(&id), "device doesn't have socket");
                    },
                )
            }
        });

        self.remove_socket(id)
    }

    fn send_frame<S>(
        &mut self,
        bindings_ctx: &mut BC,
        id: &Self::SocketId,
        params: SendFrameParams<CC::DeviceId>,
        body: S,
    ) -> Result<(), (S, SendFrameError)>
    where
        S: Serializer,
        S::Buffer: BufferMut,
    {
        let metadata = match self.with_socket_state(id, |_: &BC::SocketState, target, core_ctx| {
            make_send_metadata(core_ctx, target, params, None)
        }) {
            Ok(metadata) => metadata,
            Err(e) => return Err((body, e)),
        };
        self.send_frame(bindings_ctx, metadata, body).map_err(|s| (s, SendFrameError::SendFailed))
    }

    fn send_datagram<S>(
        &mut self,
        bindings_ctx: &mut BC,
        id: &Self::SocketId,
        params: SendDatagramParams<Self::DeviceId>,
        body: S,
    ) -> Result<(), (S, SendDatagramError)>
    where
        S: Serializer,
        S::Buffer: BufferMut,
    {
        let metadata = match self.with_socket_state(
            id,
            |_: &BC::SocketState, target @ Target { device: _, protocol }, core_ctx| {
                let SendDatagramParams { frame, protocol: target_protocol, dest_addr } = params;
                let protocol = match target_protocol.or_else(|| {
                    protocol.and_then(|p| match p {
                        Protocol::Specific(p) => Some(p),
                        Protocol::All => None,
                    })
                }) {
                    None => return Err(SendDatagramError::NoProtocol),
                    Some(p) => p,
                };

                make_send_metadata(
                    core_ctx,
                    target,
                    frame,
                    Some(DatagramHeader { dest_addr, protocol: EtherType::from(protocol.get()) }),
                )
                .map_err(SendDatagramError::Frame)
            },
        ) {
            Ok(metadata) => metadata,
            Err(e) => return Err((body, e)),
        };
        self.send_frame(bindings_ctx, metadata, body)
            .map_err(|s| (s, SendDatagramError::Frame(SendFrameError::SendFailed)))
    }
}

#[derive(Debug, PartialEq)]
pub(super) struct DeviceSocketMetadata<D> {
    pub(super) device_id: D,
    pub(super) header: Option<DatagramHeader>,
}

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub(super) struct DatagramHeader {
    pub(super) dest_addr: Mac,
    pub(super) protocol: EtherType,
}

fn make_send_metadata<CC: DeviceIdContext<AnyDevice>>(
    core_ctx: &mut CC,
    bound: &Target<CC::WeakDeviceId>,
    params: SendFrameParams<<CC as DeviceIdContext<AnyDevice>>::DeviceId>,
    header: Option<DatagramHeader>,
) -> Result<DeviceSocketMetadata<CC::DeviceId>, SendFrameError> {
    let Target { protocol: _, device } = bound;
    let SendFrameParams { device: target_device } = params;

    let device_id = match target_device.or_else(|| match device {
        TargetDevice::AnyDevice => None,
        TargetDevice::SpecificDevice(d) => core_ctx.upgrade_weak_device_id(d),
    }) {
        Some(d) => d,
        None => return Err(SendFrameError::NoDevice),
    };

    Ok(DeviceSocketMetadata { device_id, header })
}

/// Public identifier for a socket.
///
/// Strongly owns the state of the socket. So long as the `SocketId` for a
/// socket is not dropped, the socket is guaranteed to exist.
#[derive(Derivative)]
#[derivative(Clone(bound = ""), Debug(bound = "BC::SocketState: Debug"))]
pub struct SocketId<BC: crate::BindingsContext>(StrongId<BC::SocketState, WeakDeviceId<BC>>);

impl<BC: crate::BindingsContext> SocketId<BC> {
    /// Provides immutable access to [`DeviceSocketTypes::SocketState`] for the
    /// socket.
    pub fn socket_state(&self) -> &BC::SocketState {
        let Self(StrongId(strong)) = self;
        let SocketState { external_state, all_sockets_index: _, target: _ } = &**strong;
        external_state
    }
}

/// Creates an packet socket with no protocol set configured for all devices.
pub fn create<BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    external_state: BC::SocketState,
) -> SocketId<BC> {
    let mut core_ctx = Locked::new(core_ctx);
    SocketId(DeviceSocketApi::create(&mut core_ctx, external_state))
}

/// Sets the device for which a packet socket will receive packets.
pub fn set_device<BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    SocketId(id): &SocketId<BC>,
    device: TargetDevice<&DeviceId<BC>>,
) {
    let mut core_ctx = Locked::new(core_ctx);
    DeviceSocketApi::set_device(&mut core_ctx, id, device)
}

/// Sets the device and protocol for which a socket will receive packets.
pub fn set_device_and_protocol<BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    SocketId(id): &SocketId<BC>,
    device: TargetDevice<&DeviceId<BC>>,
    protocol: Protocol,
) {
    let mut core_ctx = Locked::new(core_ctx);
    DeviceSocketApi::set_device_and_protocol(&mut core_ctx, id, device, protocol)
}

/// Gets the bound info for a socket.
pub fn get_info<BC: crate::BindingsContext>(
    core_ctx: &SyncCtx<BC>,
    SocketId(id): &SocketId<BC>,
) -> SocketInfo<WeakDeviceId<BC>> {
    let mut core_ctx = Locked::new(core_ctx);
    DeviceSocketApi::get_info(&mut core_ctx, id)
}

/// Removes a bound socket.
///
/// # Panics
///
/// If the provided [`SocketId`] is not the last instance for a socket, this
/// method will panic.
pub fn remove<BC: crate::BindingsContext>(core_ctx: &SyncCtx<BC>, SocketId(id): SocketId<BC>) {
    let mut core_ctx = Locked::new(core_ctx);
    DeviceSocketApi::remove(&mut core_ctx, id)
}

/// Sends a frame for the specified socket without any additional framing.
pub fn send_frame<BC: crate::BindingsContext, B: BufferMut>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    SocketId(id): &SocketId<BC>,
    params: SendFrameParams<DeviceId<BC>>,
    body: B,
) -> Result<(), (B, SendFrameError)> {
    let mut core_ctx = Locked::new(core_ctx);
    DeviceSocketApi::send_frame(&mut core_ctx, bindings_ctx, id, params, body)
}

/// Sends a datagram with system-determined framing.
pub fn send_datagram<BC: crate::BindingsContext, B: BufferMut>(
    core_ctx: &SyncCtx<BC>,
    bindings_ctx: &mut BC,
    SocketId(id): &SocketId<BC>,
    params: SendDatagramParams<DeviceId<BC>>,
    body: B,
) -> Result<(), (B, SendDatagramError)> {
    let mut core_ctx = Locked::new(core_ctx);
    DeviceSocketApi::send_datagram(&mut core_ctx, bindings_ctx, id, params, body)
}

/// Allows the rest of the stack to dispatch packets to listening sockets.
///
/// This is implemented on top of [`SyncContext`] and abstracts packet socket
/// delivery from the rest of the system.
pub(crate) trait DeviceSocketHandler<D: Device, BC>: DeviceIdContext<D> {
    /// Dispatch a received frame to sockets.
    fn handle_frame(
        &mut self,
        bindings_ctx: &mut BC,
        device: &Self::DeviceId,
        frame: Frame<&[u8]>,
        whole_frame: &[u8],
    );
}

/// A frame received on a device.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ReceivedFrame<B> {
    /// An ethernet frame received on a socket.
    Ethernet {
        /// Where the frame was destined.
        destination: FrameDestination,
        /// The parsed ethernet frame.
        frame: EthernetFrame<B>,
    },
}

/// A frame sent on a device.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SentFrame<B> {
    /// An ethernet frame sent on a device.
    Ethernet(EthernetFrame<B>),
}

/// A frame couldn't be parsed as a [`SentFrame`].
#[derive(Debug)]
pub(crate) struct ParseSentFrameError;

impl SentFrame<&[u8]> {
    /// Tries to parse the given frame as an Ethernet frame.
    pub(crate) fn try_parse_as_ethernet(
        mut buf: &[u8],
    ) -> Result<SentFrame<&[u8]>, ParseSentFrameError> {
        packet_formats::ethernet::EthernetFrame::parse(&mut buf, EthernetFrameLengthCheck::NoCheck)
            .map_err(|_: ParseError| ParseSentFrameError)
            .map(|frame| SentFrame::Ethernet(frame.into()))
    }
}

/// Data from an Ethernet frame.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct EthernetFrame<B> {
    /// The source address of the frame.
    pub src_mac: Mac,
    /// The destination address of the frame.
    pub dst_mac: Mac,
    /// The protocol of the frame, or `None` if there was none.
    pub protocol: Option<u16>,
    /// The body of the frame.
    pub body: B,
}

/// A frame sent or received on a device
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Frame<B> {
    /// A sent frame.
    Sent(SentFrame<B>),
    /// A received frame.
    Received(ReceivedFrame<B>),
}

impl<B> From<SentFrame<B>> for Frame<B> {
    fn from(value: SentFrame<B>) -> Self {
        Self::Sent(value)
    }
}

impl<B> From<ReceivedFrame<B>> for Frame<B> {
    fn from(value: ReceivedFrame<B>) -> Self {
        Self::Received(value)
    }
}

impl<'a> From<packet_formats::ethernet::EthernetFrame<&'a [u8]>> for EthernetFrame<&'a [u8]> {
    fn from(frame: packet_formats::ethernet::EthernetFrame<&'a [u8]>) -> Self {
        Self {
            src_mac: frame.src_mac(),
            dst_mac: frame.dst_mac(),
            protocol: frame.ethertype().map(Into::into),
            body: frame.into_body(),
        }
    }
}

impl<'a> ReceivedFrame<&'a [u8]> {
    pub(crate) fn from_ethernet(
        frame: packet_formats::ethernet::EthernetFrame<&'a [u8]>,
        destination: FrameDestination,
    ) -> Self {
        Self::Ethernet { destination, frame: frame.into() }
    }
}

impl<B> Frame<B> {
    fn protocol(&self) -> Option<u16> {
        match self {
            Self::Sent(SentFrame::Ethernet(frame))
            | Self::Received(ReceivedFrame::Ethernet { destination: _, frame }) => frame.protocol,
        }
    }

    /// Convenience method for consuming the `Frame` and producing the body.
    pub fn into_body(self) -> B {
        match self {
            Self::Received(ReceivedFrame::Ethernet { destination: _, frame })
            | Self::Sent(SentFrame::Ethernet(frame)) => {
                let EthernetFrame { src_mac: _, dst_mac: _, protocol: _, body } = frame;
                body
            }
        }
    }
}

impl<
        D: Device,
        BC: DeviceSocketBindingsContext<<CC as DeviceIdContext<AnyDevice>>::DeviceId>,
        CC: DeviceSocketContext<BC> + DeviceIdContext<D>,
    > DeviceSocketHandler<D, BC> for CC
where
    <CC as DeviceIdContext<D>>::DeviceId: Into<<CC as DeviceIdContext<AnyDevice>>::DeviceId>,
{
    fn handle_frame(
        &mut self,
        bindings_ctx: &mut BC,
        device: &Self::DeviceId,
        frame: Frame<&[u8]>,
        whole_frame: &[u8],
    ) {
        let device = device.clone().into();

        // TODO(https://fxbug.dev/125732): Invert the order of acquisition
        // for the lock on the sockets held in the device and the any-device
        // sockets lock.
        self.with_any_device_sockets(|AnyDeviceSockets(any_device_sockets), core_ctx| {
            // Iterate through the device's sockets while also holding the
            // any-device sockets lock. This prevents double delivery to the
            // same socket. If the two tables were locked independently,
            // we could end up with a race, with the following thread
            // interleaving (thread A is executing this code for device D,
            // thread B is updating the device to D for the same socket X):
            //   A) lock the any device sockets table
            //   A) deliver to socket X in the table
            //   A) unlock the any device sockets table
            //   B) lock the any device sockets table, then D's sockets
            //   B) remove X from the any table and add to D's
            //   B) unlock D's sockets and any device sockets
            //   A) lock D's sockets
            //   A) deliver to socket X in D's table (!)
            core_ctx.with_device_sockets(&device, |DeviceSockets(device_sockets), core_ctx| {
                for socket in any_device_sockets.iter().chain(device_sockets) {
                    core_ctx.with_socket_state(
                        socket,
                        |external_state, Target { protocol, device: _ }, _core_ctx| {
                            let should_deliver = match protocol {
                                None => false,
                                Some(p) => match p {
                                    // Sent frames are only delivered to sockets
                                    // matching all protocols for Linux
                                    // compatibility. See https://github.com/google/gvisor/blob/68eae979409452209e4faaeac12aee4191b3d6f0/test/syscalls/linux/packet_socket.cc#L331-L392.
                                    Protocol::Specific(p) => match frame {
                                        Frame::Received(_) => Some(p.get()) == frame.protocol(),
                                        Frame::Sent(_) => false,
                                    },
                                    Protocol::All => true,
                                },
                            };
                            if should_deliver {
                                bindings_ctx.receive_frame(
                                    external_state,
                                    &device,
                                    frame,
                                    whole_frame,
                                )
                            }
                        },
                    )
                }
            })
        })
    }
}

impl<BC: crate::BindingsContext, L> DeviceSocketContextTypes for Locked<&SyncCtx<BC>, L> {
    type SocketId = StrongId<BC::SocketState, WeakDeviceId<BC>>;
}

impl<BC: crate::BindingsContext, L: LockBefore<crate::lock_ordering::AllDeviceSockets>>
    DeviceSocketContext<BC> for Locked<&SyncCtx<BC>, L>
{
    type SocketTablesCoreCtx<'a> = Locked<&'a SyncCtx<BC>, crate::lock_ordering::AnyDeviceSockets>;

    fn create_socket(&mut self, state: BC::SocketState) -> Self::SocketId {
        let mut sockets = self.lock();
        let AllSockets(sockets) = &mut *sockets;
        let entry = sockets.push_with(|index| {
            PrimaryId(PrimaryRc::new(SocketState {
                all_sockets_index: index,
                external_state: state,
                target: Mutex::new(Target::default()),
            }))
        });
        let PrimaryId(primary) = &entry.get();
        StrongId(PrimaryRc::clone_strong(primary))
    }

    fn remove_socket(&mut self, socket: Self::SocketId) {
        let mut state = self.lock();
        let AllSockets(sockets) = &mut *state;

        let PrimaryId(primary) = sockets.remove(socket.get_key_index()).expect("unknown socket ID");
        // Make sure to drop the strong ID before trying to unwrap the primary
        // ID.
        drop(socket);

        let _: SocketState<_, _> = PrimaryRc::unwrap(primary);
    }

    fn with_any_device_sockets<
        F: FnOnce(&AnyDeviceSockets<Self::SocketId>, &mut Self::SocketTablesCoreCtx<'_>) -> R,
        R,
    >(
        &mut self,
        cb: F,
    ) -> R {
        let (sockets, mut locked) = self.read_lock_and::<crate::lock_ordering::AnyDeviceSockets>();
        cb(&*sockets, &mut locked)
    }

    fn with_any_device_sockets_mut<
        F: FnOnce(&mut AnyDeviceSockets<Self::SocketId>, &mut Self::SocketTablesCoreCtx<'_>) -> R,
        R,
    >(
        &mut self,
        cb: F,
    ) -> R {
        let (mut sockets, mut locked) =
            self.write_lock_and::<crate::lock_ordering::AnyDeviceSockets>();
        cb(&mut *sockets, &mut locked)
    }
}

impl<BC: crate::BindingsContext, L: LockBefore<crate::lock_ordering::DeviceSocketState>>
    SocketStateAccessor<BC> for Locked<&SyncCtx<BC>, L>
{
    type SocketStateCoreCtx<'a> = Locked<&'a SyncCtx<BC>, crate::lock_ordering::DeviceSocketState>;

    fn with_socket_state<
        F: FnOnce(
            &BC::SocketState,
            &Target<Self::WeakDeviceId>,
            &mut Self::SocketStateCoreCtx<'_>,
        ) -> R,
        R,
    >(
        &mut self,
        StrongId(strong): &Self::SocketId,
        cb: F,
    ) -> R {
        let SocketState { external_state, target, all_sockets_index: _ } = &**strong;
        cb(external_state, &*target.lock(), &mut self.cast_locked())
    }

    fn with_socket_state_mut<
        F: FnOnce(
            &BC::SocketState,
            &mut Target<Self::WeakDeviceId>,
            &mut Self::SocketStateCoreCtx<'_>,
        ) -> R,
        R,
    >(
        &mut self,
        StrongId(primary): &Self::SocketId,
        cb: F,
    ) -> R {
        let SocketState { external_state, target, all_sockets_index: _ } = &**primary;
        cb(external_state, &mut *target.lock(), &mut self.cast_locked())
    }
}

impl<BC: crate::BindingsContext, L: LockBefore<crate::lock_ordering::DeviceSockets>>
    DeviceSocketAccessor<BC> for Locked<&SyncCtx<BC>, L>
{
    type DeviceSocketCoreCtx<'a> = Locked<&'a SyncCtx<BC>, crate::lock_ordering::DeviceSockets>;

    fn with_device_sockets<
        F: FnOnce(&DeviceSockets<Self::SocketId>, &mut Self::DeviceSocketCoreCtx<'_>) -> R,
        R,
    >(
        &mut self,
        device: &Self::DeviceId,
        cb: F,
    ) -> R {
        match device {
            DeviceId::Ethernet(device) => device::integration::with_ethernet_state_and_sync_ctx(
                self,
                device,
                |mut device_state, locked| {
                    let device_sockets =
                        device_state.read_lock::<crate::lock_ordering::DeviceSockets>();
                    cb(&*device_sockets, &mut locked.cast_locked())
                },
            ),
            DeviceId::Loopback(device) => device::integration::with_loopback_state_and_sync_ctx(
                self,
                device,
                |mut device_state, locked| {
                    let device_sockets =
                        device_state.read_lock::<crate::lock_ordering::DeviceSockets>();
                    cb(&*device_sockets, &mut locked.cast_locked())
                },
            ),
        }
    }

    fn with_device_sockets_mut<
        F: FnOnce(&mut DeviceSockets<Self::SocketId>, &mut Self::DeviceSocketCoreCtx<'_>) -> R,
        R,
    >(
        &mut self,
        device: &Self::DeviceId,
        cb: F,
    ) -> R {
        match device {
            DeviceId::Ethernet(device) => device::integration::with_ethernet_state_and_sync_ctx(
                self,
                device,
                |mut device_state, locked| {
                    let mut device_sockets =
                        device_state.write_lock::<crate::lock_ordering::DeviceSockets>();
                    cb(&mut *device_sockets, &mut locked.cast_locked())
                },
            ),
            DeviceId::Loopback(device) => device::integration::with_loopback_state_and_sync_ctx(
                self,
                device,
                |mut device_state, locked| {
                    let mut device_sockets =
                        device_state.write_lock::<crate::lock_ordering::DeviceSockets>();
                    cb(&mut *device_sockets, &mut locked.cast_locked())
                },
            ),
        }
    }
}

impl<BC: crate::BindingsContext> RwLockFor<crate::lock_ordering::AnyDeviceSockets> for SyncCtx<BC> {
    type Data = AnyDeviceSockets<StrongId<BC::SocketState, WeakDeviceId<BC>>>;
    type ReadGuard<'l> = crate::sync::RwLockReadGuard<'l, AnyDeviceSockets<StrongId<BC::SocketState, WeakDeviceId<BC>>>>
        where Self: 'l;
    type WriteGuard<'l> = crate::sync::RwLockWriteGuard<'l, AnyDeviceSockets<StrongId<BC::SocketState, WeakDeviceId<BC>>>>
        where Self: 'l;

    fn read_lock(&self) -> Self::ReadGuard<'_> {
        self.state.device.shared_sockets.any_device_sockets.read()
    }
    fn write_lock(&self) -> Self::WriteGuard<'_> {
        self.state.device.shared_sockets.any_device_sockets.write()
    }
}

impl<BC: crate::BindingsContext> LockFor<crate::lock_ordering::AllDeviceSockets> for SyncCtx<BC> {
    type Data = AllSockets<PrimaryId<BC::SocketState, WeakDeviceId<BC>>>;
    type Guard<'l> = crate::sync::LockGuard<'l, AllSockets<PrimaryId<BC::SocketState, WeakDeviceId<BC>>>>
        where Self: 'l;

    fn lock(&self) -> Self::Guard<'_> {
        self.state.device.shared_sockets.all_sockets.lock()
    }
}

#[cfg(test)]
mod testutil {
    use crate::context::testutil::FakeBindingsCtx;
    use crate::device::DeviceLayerStateTypes;
    use crate::testutil::MonotonicIdentifier;

    use super::*;

    impl<TimerId, Event: Debug, State> DeviceSocketTypes for FakeBindingsCtx<TimerId, Event, State> {
        type SocketState = ();
    }

    impl<TimerId, Event: Debug, State> DeviceLayerStateTypes
        for FakeBindingsCtx<TimerId, Event, State>
    {
        type EthernetDeviceState = ();
        type LoopbackDeviceState = ();
        type DeviceIdentifier = MonotonicIdentifier;
    }

    impl<TimerId, Event: Debug, State, DeviceId> DeviceSocketBindingsContext<DeviceId>
        for FakeBindingsCtx<TimerId, Event, State>
    {
        fn receive_frame(
            &self,
            _socket: &Self::SocketState,
            _device: &DeviceId,
            _frame: Frame<&[u8]>,
            _raw_frame: &[u8],
        ) {
            unimplemented!()
        }
    }
}

#[cfg(test)]
mod tests {
    use alloc::{
        collections::{HashMap, HashSet},
        vec,
        vec::Vec,
    };

    use const_unwrap::const_unwrap_option;
    use derivative::Derivative;
    use net_types::ethernet::Mac;
    use packet::{Buf, BufferMut, FragmentedBuffer as _, ParsablePacket};
    use packet_formats::ethernet::EthernetFrameLengthCheck;
    use test_case::test_case;

    use crate::{
        context::testutil::FakeCoreCtx,
        device::{
            testutil::{FakeStrongDeviceId, FakeWeakDeviceId, MultipleDevicesId},
            Id,
        },
        sync::Mutex,
    };

    use super::*;

    impl Frame<&[u8]> {
        pub(crate) fn cloned(self) -> Frame<Vec<u8>> {
            match self {
                Self::Sent(SentFrame::Ethernet(frame)) => {
                    Frame::Sent(SentFrame::Ethernet(frame.cloned()))
                }
                Self::Received(super::ReceivedFrame::Ethernet { destination, frame }) => {
                    Frame::Received(super::ReceivedFrame::Ethernet {
                        destination,
                        frame: frame.cloned(),
                    })
                }
            }
        }
    }

    impl EthernetFrame<&[u8]> {
        fn cloned(self) -> EthernetFrame<Vec<u8>> {
            let Self { src_mac, dst_mac, protocol, body } = self;
            EthernetFrame { src_mac, dst_mac, protocol, body: Vec::from(body) }
        }
    }

    #[derive(Clone, Debug, PartialEq)]
    struct ReceivedFrame<D> {
        device: D,
        frame: Frame<Vec<u8>>,
        raw: Vec<u8>,
    }

    #[derive(Debug, Derivative)]
    #[derivative(Default(bound = ""))]
    struct FakeBindingsCtx<D> {
        sent: Vec<(DeviceSocketMetadata<D>, Vec<u8>)>,
    }

    impl<D: Id> DeviceSocketTypes for FakeBindingsCtx<D> {
        type SocketState = ExternalSocketState<D>;
    }

    impl<D: Id> DeviceSocketBindingsContext<D> for FakeBindingsCtx<D> {
        fn receive_frame(
            &self,
            state: &ExternalSocketState<D>,
            device: &D,
            frame: Frame<&[u8]>,
            raw_frame: &[u8],
        ) {
            let ExternalSocketState(queue) = state;
            queue.lock().push(ReceivedFrame {
                device: device.clone(),
                frame: frame.cloned(),
                raw: raw_frame.into(),
            })
        }
    }

    impl<D: FakeStrongDeviceId> SendFrameContext<FakeBindingsCtx<D>, DeviceSocketMetadata<D>>
        for FakeCoreCtx<FakeSockets<D>, (), D>
    {
        fn send_frame<S>(
            &mut self,
            bindings_ctx: &mut FakeBindingsCtx<D>,
            metadata: DeviceSocketMetadata<D>,
            frame: S,
        ) -> Result<(), S>
        where
            S: Serializer,
            S::Buffer: BufferMut,
        {
            let DeviceSocketMetadata { device_id: _, header: _ } = &metadata;
            match frame.serialize_vec_outer() {
                Ok(frame) => Ok(bindings_ctx.sent.push((
                    metadata,
                    frame.map_b(Buf::into_inner).map_a(|b| b.to_flattened_vec()).into_inner(),
                ))),
                Err((_, s)) => Err(s),
            }
        }
    }

    #[derive(Debug, Derivative)]
    #[derivative(Default(bound = ""))]
    struct ExternalSocketState<D>(Mutex<Vec<ReceivedFrame<D>>>);

    type FakeAllSockets<D> =
        DenseMap<(ExternalSocketState<D>, Target<<D as crate::device::StrongId>::Weak>)>;

    #[derive(Derivative)]
    #[derivative(Default(bound = ""))]
    struct FakeSockets<D: FakeStrongDeviceId> {
        any_device_sockets: AnyDeviceSockets<FakeStrongId>,
        device_sockets: HashMap<D, DeviceSockets<FakeStrongId>>,
        all_sockets: FakeAllSockets<D>,
    }

    /// Tuple of references
    struct FakeSocketsMutRefs<'m, AnyDevice, AllSockets, Devices>(
        &'m mut AnyDevice,
        &'m mut AllSockets,
        &'m mut Devices,
    );

    /// Helper trait to allow treating a `&mut self` as a
    /// [`FakeSocketsMutRefs`].
    trait AsFakeSocketsMutRefs {
        type AnyDevice: 'static;
        type AllSockets: 'static;
        type Devices: 'static;
        fn as_sockets_ref(
            &mut self,
        ) -> FakeSocketsMutRefs<'_, Self::AnyDevice, Self::AllSockets, Self::Devices>;
    }

    impl<D: FakeStrongDeviceId> AsFakeSocketsMutRefs for FakeCoreCtx<FakeSockets<D>, (), D> {
        type AnyDevice = AnyDeviceSockets<FakeStrongId>;
        type AllSockets = FakeAllSockets<D>;
        type Devices = HashMap<D, DeviceSockets<FakeStrongId>>;

        fn as_sockets_ref(
            &mut self,
        ) -> FakeSocketsMutRefs<
            '_,
            AnyDeviceSockets<FakeStrongId>,
            FakeAllSockets<D>,
            HashMap<D, DeviceSockets<FakeStrongId>>,
        > {
            let FakeSockets { any_device_sockets, device_sockets, all_sockets } = self.get_mut();
            FakeSocketsMutRefs(any_device_sockets, all_sockets, device_sockets)
        }
    }

    impl<'m, AnyDevice: 'static, AllSockets: 'static, Devices: 'static> AsFakeSocketsMutRefs
        for FakeSocketsMutRefs<'m, AnyDevice, AllSockets, Devices>
    {
        type AnyDevice = AnyDevice;
        type AllSockets = AllSockets;
        type Devices = Devices;

        fn as_sockets_ref(&mut self) -> FakeSocketsMutRefs<'_, AnyDevice, AllSockets, Devices> {
            let Self(any_device, all_sockets, devices) = self;
            FakeSocketsMutRefs(any_device, all_sockets, devices)
        }
    }

    impl<As: AsFakeSocketsMutRefs> DeviceSocketContextTypes for As {
        type SocketId = FakeStrongId;
    }

    #[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
    pub(crate) struct FakeStrongId(usize);

    #[derive(Debug)]
    pub(crate) struct FakePrimaryId(usize);

    impl StrongSocketId for FakeStrongId {
        type Primary = FakePrimaryId;
    }

    impl<D: Clone> TargetDevice<&D> {
        fn with_weak_id(&self) -> TargetDevice<FakeWeakDeviceId<D>> {
            match self {
                TargetDevice::AnyDevice => TargetDevice::AnyDevice,
                TargetDevice::SpecificDevice(d) => {
                    TargetDevice::SpecificDevice(FakeWeakDeviceId((*d).clone()))
                }
            }
        }
    }

    impl<D: Eq + Hash + FakeStrongDeviceId> FakeSockets<D> {
        fn new(devices: impl IntoIterator<Item = D>) -> Self {
            let device_sockets =
                devices.into_iter().map(|d| (d, DeviceSockets::default())).collect();
            Self {
                any_device_sockets: AnyDeviceSockets::default(),
                device_sockets,
                all_sockets: FakeAllSockets::<D>::default(),
            }
        }

        fn remove_device(&mut self, device: &D) -> DeviceSockets<FakeStrongId> {
            let Self { any_device_sockets: _, device_sockets, all_sockets: _ } = self;
            device_sockets.remove(device).unwrap()
        }
    }

    /// Simplified trait that provides a blanket impl of [`DeviceIdContext`].
    pub(crate) trait FakeDeviceIdContext {
        type DeviceId: FakeStrongDeviceId;
        fn contains_id(&self, device_id: &Self::DeviceId) -> bool;
    }

    impl<CC: FakeDeviceIdContext> DeviceIdContext<AnyDevice> for CC {
        type DeviceId = CC::DeviceId;
        type WeakDeviceId = FakeWeakDeviceId<CC::DeviceId>;
        fn downgrade_device_id(&self, device_id: &Self::DeviceId) -> Self::WeakDeviceId {
            FakeWeakDeviceId(device_id.clone())
        }
        fn upgrade_weak_device_id(&self, device_id: &Self::WeakDeviceId) -> Option<Self::DeviceId> {
            self.contains_id(&device_id.0).then_some(device_id.0.clone())
        }
    }

    impl<D: FakeStrongDeviceId> DeviceIdContext<AnyDevice> for FakeCoreCtx<FakeSockets<D>, (), D> {
        type DeviceId = D;
        type WeakDeviceId = D::Weak;
        fn downgrade_device_id(&self, device_id: &Self::DeviceId) -> Self::WeakDeviceId {
            self.get_ref().device_sockets.downgrade_device_id(device_id)
        }
        fn upgrade_weak_device_id(&self, device_id: &Self::WeakDeviceId) -> Option<Self::DeviceId> {
            self.get_ref().device_sockets.upgrade_weak_device_id(device_id)
        }
    }

    impl<
            'm,
            DeviceId: FakeStrongDeviceId,
            As: AsFakeSocketsMutRefs<AllSockets = FakeAllSockets<DeviceId>>
                + DeviceIdContext<AnyDevice, DeviceId = DeviceId, WeakDeviceId = DeviceId::Weak>
                + DeviceSocketContextTypes<SocketId = FakeStrongId>,
        > SocketStateAccessor<FakeBindingsCtx<DeviceId>> for As
    where
        As::Devices: FakeDeviceIdContext<DeviceId = DeviceId>,
    {
        type SocketStateCoreCtx<'a> = FakeSocketsMutRefs<'a, As::AnyDevice, (), As::Devices>;

        fn with_socket_state<
            F: FnOnce(
                &ExternalSocketState<Self::DeviceId>,
                &Target<Self::WeakDeviceId>,
                &mut Self::SocketStateCoreCtx<'_>,
            ) -> R,
            R,
        >(
            &mut self,
            socket: &Self::SocketId,
            cb: F,
        ) -> R {
            let FakeSocketsMutRefs(any_device, all_sockets, devices) = self.as_sockets_ref();
            let (state, target) = all_sockets.get(socket.0).unwrap();
            cb(state, target, &mut FakeSocketsMutRefs(any_device, &mut (), devices))
        }

        fn with_socket_state_mut<
            F: FnOnce(
                &ExternalSocketState<Self::DeviceId>,
                &mut Target<Self::WeakDeviceId>,
                &mut Self::SocketStateCoreCtx<'_>,
            ) -> R,
            R,
        >(
            &mut self,
            socket: &Self::SocketId,
            cb: F,
        ) -> R {
            let FakeSocketsMutRefs(any_device, all_sockets, devices) = self.as_sockets_ref();
            let (state, target) = all_sockets.get_mut(socket.0).unwrap();
            cb(state, target, &mut FakeSocketsMutRefs(any_device, &mut (), devices))
        }
    }

    impl<
            'm,
            DeviceId: FakeStrongDeviceId,
            As: AsFakeSocketsMutRefs<
                    AllSockets = FakeAllSockets<DeviceId>,
                    Devices = HashMap<DeviceId, DeviceSockets<FakeStrongId>>,
                > + DeviceIdContext<AnyDevice, DeviceId = DeviceId, WeakDeviceId = DeviceId::Weak>
                + DeviceSocketContextTypes<SocketId = FakeStrongId>,
        > DeviceSocketAccessor<FakeBindingsCtx<DeviceId>> for As
    {
        type DeviceSocketCoreCtx<'a> =
            FakeSocketsMutRefs<'a, As::AnyDevice, FakeAllSockets<DeviceId>, HashSet<DeviceId>>;
        fn with_device_sockets<
            F: FnOnce(&DeviceSockets<Self::SocketId>, &mut Self::DeviceSocketCoreCtx<'_>) -> R,
            R,
        >(
            &mut self,
            device: &Self::DeviceId,
            cb: F,
        ) -> R {
            let FakeSocketsMutRefs(any_device, all_sockets, device_sockets) = self.as_sockets_ref();
            let mut devices = device_sockets.keys().cloned().collect();
            let device = device_sockets.get(device).unwrap();
            cb(device, &mut FakeSocketsMutRefs(any_device, all_sockets, &mut devices))
        }
        fn with_device_sockets_mut<
            F: FnOnce(&mut DeviceSockets<Self::SocketId>, &mut Self::DeviceSocketCoreCtx<'_>) -> R,
            R,
        >(
            &mut self,
            device: &Self::DeviceId,
            cb: F,
        ) -> R {
            let FakeSocketsMutRefs(any_device, all_sockets, device_sockets) = self.as_sockets_ref();
            let mut devices = device_sockets.keys().cloned().collect();
            let device = device_sockets.get_mut(device).unwrap();
            cb(device, &mut FakeSocketsMutRefs(any_device, all_sockets, &mut devices))
        }
    }

    impl<
            'm,
            DeviceId: FakeStrongDeviceId,
            As: AsFakeSocketsMutRefs<
                    AnyDevice = AnyDeviceSockets<FakeStrongId>,
                    AllSockets = FakeAllSockets<DeviceId>,
                    Devices = HashMap<DeviceId, DeviceSockets<FakeStrongId>>,
                > + DeviceIdContext<AnyDevice, DeviceId = DeviceId, WeakDeviceId = DeviceId::Weak>
                + DeviceSocketContextTypes<SocketId = FakeStrongId>,
        > DeviceSocketContext<FakeBindingsCtx<DeviceId>> for As
    {
        type SocketTablesCoreCtx<'a> = FakeSocketsMutRefs<
            'a,
            (),
            FakeAllSockets<DeviceId>,
            HashMap<DeviceId, DeviceSockets<FakeStrongId>>,
        >;
        fn create_socket(&mut self, state: ExternalSocketState<DeviceId>) -> Self::SocketId {
            let FakeSocketsMutRefs(_any_device, all_sockets, _devices) = self.as_sockets_ref();
            FakeStrongId(all_sockets.push((state, Target::default())))
        }
        fn remove_socket(&mut self, id: Self::SocketId) {
            let FakeSocketsMutRefs(
                AnyDeviceSockets(any_device_sockets),
                all_sockets,
                device_sockets,
            ) = self.as_sockets_ref();
            // Ensure there aren't any additional references to the socket's
            // state.
            assert!(!any_device_sockets.contains(&id));
            assert!(!device_sockets
                .iter()
                .any(|(_device, DeviceSockets(sockets))| sockets.contains(&id)));

            let FakeStrongId(index) = id;
            let _: (_, _) = all_sockets.remove(index).unwrap();
        }
        fn with_any_device_sockets<
            F: FnOnce(&AnyDeviceSockets<Self::SocketId>, &mut Self::SocketTablesCoreCtx<'_>) -> R,
            R,
        >(
            &mut self,
            cb: F,
        ) -> R {
            let FakeSocketsMutRefs(any_device_sockets, all_sockets, device_sockets) =
                self.as_sockets_ref();
            cb(any_device_sockets, &mut FakeSocketsMutRefs(&mut (), all_sockets, device_sockets))
        }
        fn with_any_device_sockets_mut<
            F: FnOnce(&mut AnyDeviceSockets<Self::SocketId>, &mut Self::SocketTablesCoreCtx<'_>) -> R,
            R,
        >(
            &mut self,
            cb: F,
        ) -> R {
            let FakeSocketsMutRefs(any_device_sockets, all_sockets, device_sockets) =
                self.as_sockets_ref();
            cb(any_device_sockets, &mut FakeSocketsMutRefs(&mut (), all_sockets, device_sockets))
        }
    }

    impl<'m, X: 'static, Y: 'static, Z: FakeDeviceIdContext + 'static> FakeDeviceIdContext
        for FakeSocketsMutRefs<'m, X, Y, Z>
    {
        type DeviceId = Z::DeviceId;
        fn contains_id(&self, device_id: &Self::DeviceId) -> bool {
            self.2.contains_id(device_id)
        }
    }

    impl<D: FakeStrongDeviceId> FakeDeviceIdContext for HashSet<D> {
        type DeviceId = D;
        fn contains_id(&self, device_id: &Self::DeviceId) -> bool {
            self.contains(device_id)
        }
    }

    impl<V, D: FakeStrongDeviceId> FakeDeviceIdContext for HashMap<D, V> {
        type DeviceId = D;
        fn contains_id(&self, device_id: &Self::DeviceId) -> bool {
            self.contains_key(device_id)
        }
    }

    impl<D: FakeStrongDeviceId> SendFrameContext<FakeBindingsCtx<D>, DeviceSocketMetadata<D>>
        for HashMap<D, DeviceSockets<FakeStrongId>>
    {
        fn send_frame<S>(
            &mut self,
            bindings_ctx: &mut FakeBindingsCtx<D>,
            metadata: DeviceSocketMetadata<D>,
            frame: S,
        ) -> Result<(), S>
        where
            S: Serializer,
            S::Buffer: BufferMut,
        {
            let body = frame.serialize_vec_outer().map_err(|(_, s)| s)?;
            let body = body.map_a(|b| b.to_flattened_vec()).map_b(Buf::into_inner).into_inner();
            bindings_ctx.sent.push((metadata, body));
            Ok(())
        }
    }

    const SOME_PROTOCOL: NonZeroU16 = const_unwrap_option(NonZeroU16::new(2000));

    #[test]
    fn create_remove() {
        let mut core_ctx = FakeCoreCtx::with_state(FakeSockets::new(MultipleDevicesId::all()));

        let bound = DeviceSocketApi::create(&mut core_ctx, Default::default());
        assert_eq!(
            DeviceSocketApi::get_info(&mut core_ctx, &bound),
            SocketInfo { device: TargetDevice::AnyDevice, protocol: None }
        );

        DeviceSocketApi::remove(&mut core_ctx, bound);
    }

    #[test_case(TargetDevice::AnyDevice)]
    #[test_case(TargetDevice::SpecificDevice(&MultipleDevicesId::A))]
    fn test_set_device(device: TargetDevice<&MultipleDevicesId>) {
        let mut core_ctx = FakeCoreCtx::with_state(FakeSockets::new(MultipleDevicesId::all()));

        let bound = DeviceSocketApi::create(&mut core_ctx, Default::default());
        DeviceSocketApi::set_device(&mut core_ctx, &bound, device.clone());
        assert_eq!(
            DeviceSocketApi::get_info(&mut core_ctx, &bound),
            SocketInfo { device: device.with_weak_id(), protocol: None }
        );

        let FakeSockets { device_sockets, any_device_sockets: _, all_sockets: _ } =
            core_ctx.get_ref();
        if let TargetDevice::SpecificDevice(d) = device {
            let DeviceSockets(socket_ids) = device_sockets.get(&d).expect("device state exists");
            assert_eq!(socket_ids, &HashSet::from([bound]));
        }
    }

    #[test]
    fn update_device() {
        let mut core_ctx = FakeCoreCtx::with_state(FakeSockets::new(MultipleDevicesId::all()));

        let bound = DeviceSocketApi::create(&mut core_ctx, Default::default());

        DeviceSocketApi::set_device(
            &mut core_ctx,
            &bound,
            TargetDevice::SpecificDevice(&MultipleDevicesId::A),
        );

        // Now update the device and make sure the socket only appears in the
        // one device's list.
        DeviceSocketApi::set_device(
            &mut core_ctx,
            &bound,
            TargetDevice::SpecificDevice(&MultipleDevicesId::B),
        );
        assert_eq!(
            DeviceSocketApi::get_info(&mut core_ctx, &bound),
            SocketInfo {
                device: TargetDevice::SpecificDevice(FakeWeakDeviceId(MultipleDevicesId::B)),
                protocol: None
            }
        );

        let FakeSockets { device_sockets, any_device_sockets: _, all_sockets: _ } =
            core_ctx.get_ref();
        let device_socket_lists = device_sockets
            .iter()
            .map(|(d, DeviceSockets(indexes))| (d, indexes.iter().collect()))
            .collect::<HashMap<_, _>>();

        assert_eq!(
            device_socket_lists,
            HashMap::from([
                (&MultipleDevicesId::A, vec![]),
                (&MultipleDevicesId::B, vec![&bound]),
                (&MultipleDevicesId::C, vec![])
            ])
        );
    }

    #[test_case(Protocol::All, TargetDevice::AnyDevice)]
    #[test_case(Protocol::Specific(SOME_PROTOCOL), TargetDevice::AnyDevice)]
    #[test_case(Protocol::All, TargetDevice::SpecificDevice(&MultipleDevicesId::A))]
    #[test_case(
        Protocol::Specific(SOME_PROTOCOL),
        TargetDevice::SpecificDevice(&MultipleDevicesId::A)
    )]
    fn create_set_device_and_protocol_remove_multiple(
        protocol: Protocol,
        device: TargetDevice<&MultipleDevicesId>,
    ) {
        let mut core_ctx = FakeCoreCtx::with_state(FakeSockets::new(MultipleDevicesId::all()));

        let mut sockets =
            [(); 3].map(|()| DeviceSocketApi::create(&mut core_ctx, Default::default()));
        for socket in &mut sockets {
            DeviceSocketApi::set_device_and_protocol(
                &mut core_ctx,
                socket,
                device.clone(),
                protocol,
            );
            assert_eq!(
                DeviceSocketApi::get_info(&mut core_ctx, socket),
                SocketInfo { device: device.with_weak_id(), protocol: Some(protocol) }
            );
        }

        for socket in sockets {
            DeviceSocketApi::remove(&mut core_ctx, socket)
        }
    }

    #[test]
    fn change_device_after_removal() {
        let mut core_ctx = FakeCoreCtx::with_state(FakeSockets::new(MultipleDevicesId::all()));

        let bound = DeviceSocketApi::create(&mut core_ctx, Default::default());
        // Set the device for the socket before removing the device state
        // entirely.
        const DEVICE_TO_REMOVE: MultipleDevicesId = MultipleDevicesId::A;
        DeviceSocketApi::set_device(
            &mut core_ctx,
            &bound,
            TargetDevice::SpecificDevice(&DEVICE_TO_REMOVE),
        );

        // Now remove the device; this should cause future attempts to upgrade
        // the device ID to fail.
        let removed = core_ctx.get_mut().remove_device(&DEVICE_TO_REMOVE);
        assert_eq!(removed, DeviceSockets(HashSet::from([bound.clone()])));

        // Changing the device should gracefully handle the fact that the
        // earlier-bound device is now gone.
        DeviceSocketApi::set_device(
            &mut core_ctx,
            &bound,
            TargetDevice::SpecificDevice(&MultipleDevicesId::B),
        );
        assert_eq!(
            DeviceSocketApi::get_info(&mut core_ctx, &bound),
            SocketInfo {
                device: TargetDevice::SpecificDevice(FakeWeakDeviceId(MultipleDevicesId::B)),
                protocol: None,
            }
        );

        let FakeSockets { device_sockets, any_device_sockets: _, all_sockets: _ } =
            core_ctx.get_ref();
        let DeviceSockets(weak_sockets) =
            device_sockets.get(&MultipleDevicesId::B).expect("device state exists");
        assert_eq!(weak_sockets, &HashSet::from([bound]));
    }

    struct TestData;
    impl TestData {
        const SRC_MAC: Mac = Mac::new([0, 1, 2, 3, 4, 5]);
        const DST_MAC: Mac = Mac::new([6, 7, 8, 9, 10, 11]);
        /// Arbitrary protocol number.
        const PROTO: NonZeroU16 = const_unwrap_option(NonZeroU16::new(0x08AB));
        const BODY: &'static [u8] = b"some pig";
        const BUFFER: &'static [u8] = &[
            6, 7, 8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 0x08, 0xAB, b's', b'o', b'm', b'e', b' ', b'p',
            b'i', b'g',
        ];

        /// Creates an EthernetFrame with the values specified above.
        fn frame() -> packet_formats::ethernet::EthernetFrame<&'static [u8]> {
            let mut buffer_view = Self::BUFFER;
            packet_formats::ethernet::EthernetFrame::parse(
                &mut buffer_view,
                EthernetFrameLengthCheck::NoCheck,
            )
            .unwrap()
        }
    }

    const WRONG_PROTO: NonZeroU16 = const_unwrap_option(NonZeroU16::new(0x08ff));

    fn make_bound<CC: DeviceSocketApi<BC>, BC: DeviceSocketBindingsContext<CC::DeviceId>>(
        core_ctx: &mut CC,
        device: TargetDevice<CC::DeviceId>,
        protocol: Option<Protocol>,
        state: BC::SocketState,
    ) -> CC::SocketId {
        let id = DeviceSocketApi::create(core_ctx, state);
        let device = match &device {
            TargetDevice::AnyDevice => TargetDevice::AnyDevice,
            TargetDevice::SpecificDevice(d) => TargetDevice::SpecificDevice(d),
        };
        match protocol {
            Some(protocol) => {
                DeviceSocketApi::set_device_and_protocol(core_ctx, &id, device, protocol)
            }
            None => DeviceSocketApi::set_device(core_ctx, &id, device),
        };
        id
    }

    /// Deliver one frame to the provided contexts and return the IDs of the
    /// sockets it was delivered to.
    fn deliver_one_frame(
        delivered_frame: Frame<&[u8]>,
        mut core_ctx: FakeCoreCtx<FakeSockets<MultipleDevicesId>, (), MultipleDevicesId>,
        mut bindings_ctx: FakeBindingsCtx<MultipleDevicesId>,
    ) -> HashSet<FakeStrongId> {
        DeviceSocketHandler::handle_frame(
            &mut core_ctx,
            &mut bindings_ctx,
            &MultipleDevicesId::A,
            delivered_frame.clone(),
            TestData::BUFFER,
        );

        let FakeSockets { all_sockets, any_device_sockets: _, device_sockets: _ } =
            core_ctx.into_state();

        all_sockets
            .into_iter()
            .filter_map(|(index, (ExternalSocketState(frames), _)): (_, (_, Target<_>))| {
                let frames = frames.into_inner();
                (!frames.is_empty()).then(|| {
                    assert_eq!(
                        frames,
                        &[ReceivedFrame {
                            device: MultipleDevicesId::A,
                            frame: delivered_frame.cloned(),
                            raw: TestData::BUFFER.into(),
                        }]
                    );
                    FakeStrongId(index)
                })
            })
            .collect()
    }

    #[test]
    fn receive_frame_deliver_to_multiple() {
        let mut core_ctx = FakeCoreCtx::with_state(FakeSockets::new(MultipleDevicesId::all()));
        let bindings_ctx = FakeBindingsCtx::default();

        use Protocol::*;
        use TargetDevice::*;
        let never_bound = {
            let state = ExternalSocketState::<MultipleDevicesId>::default();
            DeviceSocketApi::create(&mut core_ctx, state)
        };

        let mut make_bound = |device, protocol| {
            let state = ExternalSocketState::<MultipleDevicesId>::default();
            make_bound(&mut core_ctx, device, protocol, state)
        };
        let bound_a_no_protocol = make_bound(SpecificDevice(MultipleDevicesId::A), None);
        let bound_a_all_protocols = make_bound(SpecificDevice(MultipleDevicesId::A), Some(All));
        let bound_a_right_protocol =
            make_bound(SpecificDevice(MultipleDevicesId::A), Some(Specific(TestData::PROTO)));
        let bound_a_wrong_protocol =
            make_bound(SpecificDevice(MultipleDevicesId::A), Some(Specific(WRONG_PROTO)));
        let bound_b_no_protocol = make_bound(SpecificDevice(MultipleDevicesId::B), None);
        let bound_b_all_protocols = make_bound(SpecificDevice(MultipleDevicesId::B), Some(All));
        let bound_b_right_protocol =
            make_bound(SpecificDevice(MultipleDevicesId::B), Some(Specific(TestData::PROTO)));
        let bound_b_wrong_protocol =
            make_bound(SpecificDevice(MultipleDevicesId::B), Some(Specific(WRONG_PROTO)));
        let bound_any_no_protocol = make_bound(AnyDevice, None);
        let bound_any_all_protocols = make_bound(AnyDevice, Some(All));
        let bound_any_right_protocol = make_bound(AnyDevice, Some(Specific(TestData::PROTO)));
        let bound_any_wrong_protocol = make_bound(AnyDevice, Some(Specific(WRONG_PROTO)));

        let mut sockets_with_received_frames = deliver_one_frame(
            super::ReceivedFrame::from_ethernet(
                TestData::frame(),
                FrameDestination::Individual { local: true },
            )
            .into(),
            core_ctx,
            bindings_ctx,
        );

        let _ = (
            never_bound,
            bound_a_no_protocol,
            bound_a_wrong_protocol,
            bound_b_no_protocol,
            bound_b_all_protocols,
            bound_b_right_protocol,
            bound_b_wrong_protocol,
            bound_any_no_protocol,
            bound_any_wrong_protocol,
        );

        assert!(sockets_with_received_frames.remove(&bound_a_all_protocols));
        assert!(sockets_with_received_frames.remove(&bound_a_right_protocol));
        assert!(sockets_with_received_frames.remove(&bound_any_all_protocols));
        assert!(sockets_with_received_frames.remove(&bound_any_right_protocol));
        assert!(sockets_with_received_frames.is_empty());
    }

    #[test]
    fn sent_frame_deliver_to_multiple() {
        let mut core_ctx = FakeCoreCtx::with_state(FakeSockets::new(MultipleDevicesId::all()));
        let bindings_ctx = FakeBindingsCtx::default();

        use Protocol::*;
        use TargetDevice::*;
        let never_bound = {
            let state = ExternalSocketState::<MultipleDevicesId>::default();
            DeviceSocketApi::create(&mut core_ctx, state)
        };

        let mut make_bound = |device, protocol| {
            let state = ExternalSocketState::<MultipleDevicesId>::default();
            make_bound(&mut core_ctx, device, protocol, state)
        };
        let bound_a_no_protocol = make_bound(SpecificDevice(MultipleDevicesId::A), None);
        let bound_a_all_protocols = make_bound(SpecificDevice(MultipleDevicesId::A), Some(All));
        let bound_a_same_protocol =
            make_bound(SpecificDevice(MultipleDevicesId::A), Some(Specific(TestData::PROTO)));
        let bound_a_wrong_protocol =
            make_bound(SpecificDevice(MultipleDevicesId::A), Some(Specific(WRONG_PROTO)));
        let bound_b_no_protocol = make_bound(SpecificDevice(MultipleDevicesId::B), None);
        let bound_b_all_protocols = make_bound(SpecificDevice(MultipleDevicesId::B), Some(All));
        let bound_b_same_protocol =
            make_bound(SpecificDevice(MultipleDevicesId::B), Some(Specific(TestData::PROTO)));
        let bound_b_wrong_protocol =
            make_bound(SpecificDevice(MultipleDevicesId::B), Some(Specific(WRONG_PROTO)));
        let bound_any_no_protocol = make_bound(AnyDevice, None);
        let bound_any_all_protocols = make_bound(AnyDevice, Some(All));
        let bound_any_same_protocol = make_bound(AnyDevice, Some(Specific(TestData::PROTO)));
        let bound_any_wrong_protocol = make_bound(AnyDevice, Some(Specific(WRONG_PROTO)));

        let mut sockets_with_received_frames = deliver_one_frame(
            SentFrame::Ethernet(TestData::frame().into()).into(),
            core_ctx,
            bindings_ctx,
        );

        let _ = (
            never_bound,
            bound_a_no_protocol,
            bound_a_same_protocol,
            bound_a_wrong_protocol,
            bound_b_no_protocol,
            bound_b_all_protocols,
            bound_b_same_protocol,
            bound_b_wrong_protocol,
            bound_any_no_protocol,
            bound_any_same_protocol,
            bound_any_wrong_protocol,
        );

        // Only any-protocol sockets receive sent frames.
        assert!(sockets_with_received_frames.remove(&bound_a_all_protocols));
        assert!(sockets_with_received_frames.remove(&bound_any_all_protocols));
        assert!(sockets_with_received_frames.is_empty());
    }

    #[test]
    fn deliver_multiple_frames() {
        let mut core_ctx = FakeCoreCtx::with_state(FakeSockets::new(MultipleDevicesId::all()));
        let mut bindings_ctx = FakeBindingsCtx::default();
        let socket = make_bound(
            &mut core_ctx,
            TargetDevice::AnyDevice,
            Some(Protocol::All),
            ExternalSocketState::default(),
        );

        const RECEIVE_COUNT: usize = 10;
        for _ in 0..RECEIVE_COUNT {
            DeviceSocketHandler::handle_frame(
                &mut core_ctx,
                &mut bindings_ctx,
                &MultipleDevicesId::A,
                super::ReceivedFrame::from_ethernet(
                    TestData::frame(),
                    FrameDestination::Individual { local: true },
                )
                .into(),
                TestData::BUFFER,
            );
        }

        let FakeSockets { mut all_sockets, any_device_sockets: _, device_sockets: _ } =
            core_ctx.into_state();
        let FakeStrongId(index) = socket;
        let (ExternalSocketState(received), _): (_, Target<_>) = all_sockets.remove(index).unwrap();
        assert_eq!(
            received.into_inner(),
            vec![
                ReceivedFrame {
                    device: MultipleDevicesId::A,
                    frame: Frame::Received(super::ReceivedFrame::Ethernet {
                        destination: FrameDestination::Individual { local: true },
                        frame: EthernetFrame {
                            src_mac: TestData::SRC_MAC,
                            dst_mac: TestData::DST_MAC,
                            protocol: Some(TestData::PROTO.into()),
                            body: Vec::from(TestData::BODY),
                        }
                    }),
                    raw: TestData::BUFFER.into()
                };
                RECEIVE_COUNT
            ]
        );
        assert!(all_sockets.is_empty());
    }

    #[test_case(None, None, Err(SendFrameError::NoDevice); "no bound or override device")]
    #[test_case(Some(MultipleDevicesId::A), None, Ok(MultipleDevicesId::A); "bound device set")]
    #[test_case(None, Some(MultipleDevicesId::A), Ok(MultipleDevicesId::A); "send device set")]
    #[test_case(Some(MultipleDevicesId::A), Some(MultipleDevicesId::A), Ok(MultipleDevicesId::A);
        "both set same")]
    #[test_case(Some(MultipleDevicesId::A), Some(MultipleDevicesId::B), Ok(MultipleDevicesId::B);
        "send overides")]
    fn send_frame_on_socket(
        bind_device: Option<MultipleDevicesId>,
        send_device: Option<MultipleDevicesId>,
        expected_device: Result<MultipleDevicesId, SendFrameError>,
    ) {
        let mut core_ctx = FakeCoreCtx::with_state(FakeSockets::new(MultipleDevicesId::all()));
        let mut bindings_ctx = FakeBindingsCtx::default();

        let id = DeviceSocketApi::create(&mut core_ctx, Default::default());
        if let Some(bind_device) = bind_device {
            DeviceSocketApi::set_device(
                &mut core_ctx,
                &id,
                TargetDevice::SpecificDevice(&bind_device),
            );
        }

        let destination = SendFrameParams { device: send_device };
        let expected_status = expected_device.as_ref().map(|_| ()).map_err(|e| *e);
        assert_eq!(
            DeviceSocketApi::send_frame(
                &mut core_ctx,
                &mut bindings_ctx,
                &id,
                destination,
                Buf::new(Vec::from(TestData::BODY), ..),
            )
            .map_err(|(_, e): (Buf<Vec<u8>>, _)| e),
            expected_status
        );

        if let Ok(expected_device) = expected_device {
            let FakeBindingsCtx { sent } = bindings_ctx;
            assert_eq!(
                sent,
                [(
                    DeviceSocketMetadata { device_id: expected_device, header: None },
                    Vec::from(TestData::BODY)
                )]
            )
        }
    }

    #[test_case(
        None, None,
        SendDatagramParams {
            frame: SendFrameParams {device: None},
            dest_addr: TestData::DST_MAC,
            protocol: None
        },
        Err(SendDatagramError::NoProtocol); "no protocol or device")]
    #[test_case(
        None, Some(MultipleDevicesId::A),
        SendDatagramParams {
            frame: SendFrameParams {device: None},
            dest_addr: TestData::DST_MAC,
            protocol: None
        },
        Err(SendDatagramError::NoProtocol); "bound no protocol")]
    #[test_case(
        Some(Protocol::All), Some(MultipleDevicesId::A),
        SendDatagramParams {
            frame: SendFrameParams {device: None},
            dest_addr: TestData::DST_MAC,
            protocol: None
        },
        Err(SendDatagramError::NoProtocol); "bound all protocols")]
    #[test_case(
        Some(Protocol::Specific(TestData::PROTO)), None,
        SendDatagramParams {
            frame: SendFrameParams {device: None},
            dest_addr: TestData::DST_MAC,
            protocol: None,
        },
        Err(SendDatagramError::Frame(SendFrameError::NoDevice)); "no device")]
    #[test_case(
        Some(Protocol::Specific(TestData::PROTO)), Some(MultipleDevicesId::A),
        SendDatagramParams {
            frame: SendFrameParams {device: None},
            dest_addr: TestData::DST_MAC,
            protocol: None,
        },
        Ok(MultipleDevicesId::A); "device and proto from bound")]
    #[test_case(
        None, None,
        SendDatagramParams {
            frame: SendFrameParams { device: Some(MultipleDevicesId::C), },
            dest_addr: TestData::DST_MAC,
            protocol: Some(TestData::PROTO),
        },
        Ok(MultipleDevicesId::C); "device and proto from destination")]
    #[test_case(
        Some(Protocol::Specific(WRONG_PROTO)), Some(MultipleDevicesId::A),
        SendDatagramParams {
            frame: SendFrameParams {device: Some(MultipleDevicesId::C),},
            dest_addr: TestData::DST_MAC,
            protocol: Some(TestData::PROTO),
        },
        Ok(MultipleDevicesId::C); "destination overrides")]
    fn send_datagram_on_socket(
        bind_protocol: Option<Protocol>,
        bind_device: Option<MultipleDevicesId>,
        destination: SendDatagramParams<MultipleDevicesId>,
        expected_device: Result<MultipleDevicesId, SendDatagramError>,
    ) {
        let mut core_ctx = FakeCoreCtx::with_state(FakeSockets::new(MultipleDevicesId::all()));
        let mut bindings_ctx = FakeBindingsCtx::default();

        let id = make_bound(
            &mut core_ctx,
            bind_device.map_or(TargetDevice::AnyDevice, TargetDevice::SpecificDevice),
            bind_protocol,
            Default::default(),
        );

        let expected_status = expected_device.as_ref().map(|_| ()).map_err(|e| *e);
        assert_eq!(
            DeviceSocketApi::send_datagram(
                &mut core_ctx,
                &mut bindings_ctx,
                &id,
                destination,
                Buf::new(Vec::from(TestData::BODY), ..),
            )
            .map_err(|(_, e): (Buf<Vec<u8>>, _)| e),
            expected_status
        );

        if let Ok(expected_device) = expected_device {
            let FakeBindingsCtx { sent } = bindings_ctx;
            let expected_sent = (
                DeviceSocketMetadata {
                    device_id: expected_device,
                    header: Some(DatagramHeader {
                        dest_addr: TestData::DST_MAC,
                        protocol: TestData::PROTO.get().into(),
                    }),
                },
                Vec::from(TestData::BODY),
            );
            assert_eq!(sent, [expected_sent])
        }
    }

    #[test]
    fn drop_real_ids() {
        /// Test with a real `SyncCtx` to assert that IDs aren't dropped in the
        /// wrong order.
        use crate::testutil::{Ctx, FakeEventDispatcherBuilder, FAKE_CONFIG_V4};
        let (mut ctx, device_ids) = FakeEventDispatcherBuilder::from_config(FAKE_CONFIG_V4).build();

        let Ctx { core_ctx, bindings_ctx: _ } = &mut ctx;

        let never_bound = create(core_ctx, Mutex::default());
        let bound_any_device = {
            let id = create(core_ctx, Mutex::default());
            set_device(core_ctx, &id, TargetDevice::AnyDevice);
            id
        };
        let bound_specific_device = {
            let id = create(core_ctx, Mutex::default());
            set_device(
                core_ctx,
                &id,
                TargetDevice::SpecificDevice(&DeviceId::Ethernet(device_ids[0].clone())),
            );
            id
        };

        // Make sure the socket IDs go out of scope before `ctx`.
        drop((never_bound, bound_any_device, bound_specific_device));
    }
}
