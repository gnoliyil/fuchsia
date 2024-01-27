// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The Ethernet protocol.

use alloc::vec::Vec;
use core::{fmt::Debug, num::NonZeroU32};
use lock_order::{
    lock::{LockFor, RwLockFor, UnlockedAccess},
    relation::LockBefore,
    Locked,
};

use log::trace;
use net_types::{
    ethernet::Mac,
    ip::{IpAddress, Ipv4, Ipv4Addr, Ipv6, Ipv6Addr},
    BroadcastAddress, MulticastAddr, MulticastAddress, SpecifiedAddr, UnicastAddr, UnicastAddress,
    Witness,
};
use packet::{Buf, BufferMut, InnerPacketBuilder as _, Nested, Serializer};
use packet_formats::{
    arp::{peek_arp_types, ArpHardwareType, ArpNetworkType},
    ethernet::{
        EtherType, EthernetFrame, EthernetFrameBuilder, EthernetFrameLengthCheck, EthernetIpExt,
        ETHERNET_HDR_LEN_NO_TAG,
    },
    icmp::{
        ndp::{options::NdpOptionBuilder, NeighborSolicitation, OptionSequenceBuilder},
        IcmpUnusedCode,
    },
    utils::NonZeroDuration,
};

#[cfg(test)]
use crate::ip::device::nud::NudHandler;
use crate::{
    context::{
        CounterContext, RecvFrameContext, RngContext, SendFrameContext, TimerContext, TimerHandler,
    },
    data_structures::ref_counted_hash_map::{InsertResult, RefCountedHashSet, RemoveResult},
    device::{
        arp::{
            ArpContext, ArpFrameMetadata, ArpPacketHandler, ArpState, ArpTimerId, BufferArpContext,
        },
        link::LinkDevice,
        queue::{
            tx::{
                BufVecU8Allocator, BufferTransmitQueueHandler, TransmitDequeueContext,
                TransmitQueue, TransmitQueueContext, TransmitQueueNonSyncContext,
                TransmitQueueState, TransmitQueueTypes,
            },
            DequeueState, TransmitQueueFrameError,
        },
        state::IpLinkDeviceState,
        with_ethernet_state, with_ethernet_state_and_sync_ctx, Device, DeviceIdContext,
        DeviceLayerEventDispatcher, DeviceSendFrameError, EthernetDeviceId, FrameDestination, Mtu,
        RecvIpFrameMeta,
    },
    ip::device::{
        nud::{BufferNudContext, BufferNudHandler, NudContext, NudState, NudTimerId},
        state::{DualStackIpDeviceState, Ipv4DeviceState, Ipv6DeviceState},
    },
    sync::{Mutex, RwLock},
    BufferNonSyncContext, Instant, NonSyncContext, SyncCtx,
};

impl From<Mac> for FrameDestination {
    fn from(mac: Mac) -> FrameDestination {
        if mac.is_broadcast() {
            FrameDestination::Broadcast
        } else if mac.is_multicast() {
            FrameDestination::Multicast
        } else {
            debug_assert!(mac.is_unicast());
            FrameDestination::Unicast
        }
    }
}

const ETHERNET_HDR_LEN_NO_TAG_U32: u32 = ETHERNET_HDR_LEN_NO_TAG as u32;

/// The non-synchronized execution context for an Ethernet device.
pub(crate) trait EthernetIpLinkDeviceNonSyncContext<DeviceId>:
    CounterContext + RngContext + TimerContext<EthernetTimerId<DeviceId>>
{
}
impl<DeviceId, C: CounterContext + RngContext + TimerContext<EthernetTimerId<DeviceId>>>
    EthernetIpLinkDeviceNonSyncContext<DeviceId> for C
{
}

/// The execution context for an Ethernet device.
pub(crate) trait EthernetIpLinkDeviceContext<C: EthernetIpLinkDeviceNonSyncContext<Self::DeviceId>>:
    DeviceIdContext<EthernetLinkDevice>
{
    /// Calls the function with the ethernet device's static state.
    fn with_static_ethernet_device_state<O, F: FnOnce(&StaticEthernetDeviceState) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O;

    /// Calls the function with the ethernet device's static state and immutable
    /// reference to the dynamic state.
    fn with_ethernet_device_state<
        O,
        F: FnOnce(&StaticEthernetDeviceState, &DynamicEthernetDeviceState) -> O,
    >(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O;

    /// Calls the function with the ethernet device's static state and mutable
    /// reference to the dynamic state.
    fn with_ethernet_device_state_mut<
        O,
        F: FnOnce(&StaticEthernetDeviceState, &mut DynamicEthernetDeviceState) -> O,
    >(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O;
}

// TODO(https://fxbug.dev/121448): Remove this when it is unused.
impl<NonSyncCtx: NonSyncContext> EthernetIpLinkDeviceContext<NonSyncCtx>
    for &'_ SyncCtx<NonSyncCtx>
{
    fn with_static_ethernet_device_state<O, F: FnOnce(&StaticEthernetDeviceState) -> O>(
        &mut self,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
        cb: F,
    ) -> O {
        Locked::new(*self).with_static_ethernet_device_state(device_id, cb)
    }

    fn with_ethernet_device_state<
        O,
        F: FnOnce(&StaticEthernetDeviceState, &DynamicEthernetDeviceState) -> O,
    >(
        &mut self,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
        cb: F,
    ) -> O {
        Locked::new(*self).with_ethernet_device_state(device_id, cb)
    }

    fn with_ethernet_device_state_mut<
        O,
        F: FnOnce(&StaticEthernetDeviceState, &mut DynamicEthernetDeviceState) -> O,
    >(
        &mut self,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
        cb: F,
    ) -> O {
        Locked::new(*self).with_ethernet_device_state_mut(device_id, cb)
    }
}

impl<
        NonSyncCtx: NonSyncContext,
        L: LockBefore<crate::lock_ordering::EthernetDeviceDynamicState>,
    > EthernetIpLinkDeviceContext<NonSyncCtx> for Locked<'_, SyncCtx<NonSyncCtx>, L>
{
    fn with_static_ethernet_device_state<O, F: FnOnce(&StaticEthernetDeviceState) -> O>(
        &mut self,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
        cb: F,
    ) -> O {
        with_ethernet_state(self, device_id, |state| {
            cb(state.unlocked_access::<crate::lock_ordering::EthernetDeviceStaticState>())
        })
    }

    fn with_ethernet_device_state<
        O,
        F: FnOnce(&StaticEthernetDeviceState, &DynamicEthernetDeviceState) -> O,
    >(
        &mut self,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
        cb: F,
    ) -> O {
        with_ethernet_state(self, device_id, |mut state| {
            let (dynamic_state, locked) =
                state.read_lock_and::<crate::lock_ordering::EthernetDeviceDynamicState>();
            cb(
                &locked.unlocked_access::<crate::lock_ordering::EthernetDeviceStaticState>(),
                &dynamic_state,
            )
        })
    }

    fn with_ethernet_device_state_mut<
        O,
        F: FnOnce(&StaticEthernetDeviceState, &mut DynamicEthernetDeviceState) -> O,
    >(
        &mut self,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
        cb: F,
    ) -> O {
        with_ethernet_state(self, device_id, |mut state| {
            let (mut dynamic_state, locked) =
                state.write_lock_and::<crate::lock_ordering::EthernetDeviceDynamicState>();
            cb(
                &locked.unlocked_access::<crate::lock_ordering::EthernetDeviceStaticState>(),
                &mut dynamic_state,
            )
        })
    }
}

/// A shorthand for `BufferIpLinkDeviceContext` with all of the appropriate type
/// arguments fixed to their Ethernet values.
pub(super) trait BufferEthernetIpLinkDeviceContext<
    C: EthernetIpLinkDeviceNonSyncContext<Self::DeviceId>,
    B: BufferMut,
>:
    EthernetIpLinkDeviceContext<C>
    + RecvFrameContext<C, B, RecvIpFrameMeta<Self::DeviceId, Ipv4>>
    + RecvFrameContext<C, B, RecvIpFrameMeta<Self::DeviceId, Ipv6>>
{
}

impl<
        C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
        B: BufferMut,
        SC: EthernetIpLinkDeviceContext<C>
            + RecvFrameContext<C, B, RecvIpFrameMeta<SC::DeviceId, Ipv4>>
            + RecvFrameContext<C, B, RecvIpFrameMeta<SC::DeviceId, Ipv6>>,
    > BufferEthernetIpLinkDeviceContext<C, B> for SC
{
}

// TODO(https://fxbug.dev/121448): Remove this when it is unused.
impl<NonSyncCtx: NonSyncContext> NudContext<Ipv6, EthernetLinkDevice, NonSyncCtx>
    for &'_ SyncCtx<NonSyncCtx>
{
    fn retrans_timer(
        &mut self,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
    ) -> NonZeroDuration {
        NudContext::<Ipv6, _, _>::retrans_timer(&mut Locked::new(*self), device_id)
    }

    fn with_nud_state_mut<O, F: FnOnce(&mut NudState<Ipv6, Mac>) -> O>(
        &mut self,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
        cb: F,
    ) -> O {
        NudContext::<Ipv6, _, _>::with_nud_state_mut(&mut Locked::new(*self), device_id, cb)
    }

    fn send_neighbor_solicitation(
        &mut self,
        ctx: &mut NonSyncCtx,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
        lookup_addr: SpecifiedAddr<Ipv6Addr>,
    ) {
        NudContext::<Ipv6, _, _>::send_neighbor_solicitation(
            &mut Locked::new(*self),
            ctx,
            device_id,
            lookup_addr,
        )
    }
}

impl<NonSyncCtx: NonSyncContext, L: LockBefore<crate::lock_ordering::IpState<Ipv6>>>
    NudContext<Ipv6, EthernetLinkDevice, NonSyncCtx> for Locked<'_, SyncCtx<NonSyncCtx>, L>
{
    fn retrans_timer(
        &mut self,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
    ) -> NonZeroDuration {
        with_ethernet_state(self, device_id, |mut state| {
            let mut state = state.cast();
            let ipv6 = state.read_lock::<crate::lock_ordering::EthernetDeviceIpState<Ipv6>>();
            ipv6.retrans_timer
        })
    }

    fn with_nud_state_mut<O, F: FnOnce(&mut NudState<Ipv6, Mac>) -> O>(
        &mut self,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
        cb: F,
    ) -> O {
        with_ethernet_state(self, device_id, |mut state| {
            let mut nud = state.lock::<crate::lock_ordering::EthernetIpv6Nud>();
            cb(&mut nud)
        })
    }

    fn send_neighbor_solicitation(
        &mut self,
        ctx: &mut NonSyncCtx,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
        lookup_addr: SpecifiedAddr<Ipv6Addr>,
    ) {
        let dst_ip = lookup_addr.to_solicited_node_address().into_specified();
        let src_ip = crate::ip::IpDeviceContext::<Ipv6, _>::get_local_addr_for_remote(
            self,
            &device_id.clone().into(),
            dst_ip,
        );
        let src_ip = match src_ip {
            Some(s) => s,
            None => return,
        };

        let mac = get_mac(self, device_id);

        // TODO(https://fxbug.dev/85055): Either panic or guarantee that this error
        // can't happen statically.
        let _: Result<(), _> = crate::ip::icmp::send_ndp_packet(
            self,
            ctx,
            &device_id.clone().into(),
            Some(src_ip),
            dst_ip,
            OptionSequenceBuilder::<_>::new(
                [NdpOptionBuilder::SourceLinkLayerAddress(mac.bytes().as_ref())].iter(),
            )
            .into_serializer(),
            IcmpUnusedCode,
            NeighborSolicitation::new(lookup_addr.get()),
        );
    }
}

fn send_ip_frame_to_dst<
    B: BufferMut,
    S: Serializer<Buffer = B>,
    C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
    SC: EthernetIpLinkDeviceContext<C>
        + BufferTransmitQueueHandler<EthernetLinkDevice, B, C, Meta = ()>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    device_id: &SC::DeviceId,
    dst_mac: Mac,
    body: S,
    ether_type: EtherType,
) -> Result<(), S> {
    let local_mac = get_mac(sync_ctx, device_id);
    match BufferTransmitQueueHandler::<EthernetLinkDevice, _, _>::queue_tx_frame(
        sync_ctx,
        ctx,
        device_id,
        (),
        body.encapsulate(EthernetFrameBuilder::new(local_mac.get(), dst_mac, ether_type)),
    ) {
        Ok(()) => Ok(()),
        Err(TransmitQueueFrameError::NoQueue(e)) => {
            log::error!("device {} not ready to send frame: {:?}", device_id, e);
            Ok(())
        }
        Err(TransmitQueueFrameError::QueueFull(s) | TransmitQueueFrameError::SerializeError(s)) => {
            Err(s.into_inner())
        }
    }
}

// TODO(https://fxbug.dev/121448): Remove this when it is unused.
impl<B: BufferMut, NonSyncCtx: BufferNonSyncContext<B>>
    BufferNudContext<B, Ipv6, EthernetLinkDevice, NonSyncCtx> for &'_ SyncCtx<NonSyncCtx>
{
    fn send_ip_packet_to_neighbor_link_addr<S: Serializer<Buffer = B>>(
        &mut self,
        ctx: &mut NonSyncCtx,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
        dst_mac: Mac,
        body: S,
    ) -> Result<(), S> {
        BufferNudContext::<_, Ipv6, _, _>::send_ip_packet_to_neighbor_link_addr(
            &mut Locked::new(*self),
            ctx,
            device_id,
            dst_mac,
            body,
        )
    }
}

impl<
        B: BufferMut,
        NonSyncCtx: BufferNonSyncContext<B>,
        L: LockBefore<crate::lock_ordering::IpState<Ipv6>>,
    > BufferNudContext<B, Ipv6, EthernetLinkDevice, NonSyncCtx>
    for Locked<'_, SyncCtx<NonSyncCtx>, L>
{
    fn send_ip_packet_to_neighbor_link_addr<S: Serializer<Buffer = B>>(
        &mut self,
        ctx: &mut NonSyncCtx,
        device_id: &EthernetDeviceId<NonSyncCtx::Instant, NonSyncCtx::EthernetDeviceState>,
        dst_mac: Mac,
        body: S,
    ) -> Result<(), S> {
        send_ip_frame_to_dst(self, ctx, device_id, dst_mac, body, EtherType::Ipv6)
    }
}

/// The maximum frame size one ethernet device can send.
///
/// The frame size includes the ethernet header, the data payload, but excludes
/// the 4 bytes from FCS (frame check sequence) as we don't calculate CRC and it
/// is normally handled by the device.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct MaxFrameSize(NonZeroU32);

impl MaxFrameSize {
    /// The minimum ethernet frame size.
    ///
    /// We don't care about FCS, so the minimum frame size for us is 64 - 4.
    pub(crate) const MIN: MaxFrameSize = MaxFrameSize(nonzero_ext::nonzero!(60_u32));

    /// Creates from the maximum size of ethernet header and ethernet payload,
    /// checks that it is valid, i.e., larger than the minimum frame size.
    pub const fn new(frame_size: u32) -> Option<Self> {
        if frame_size < Self::MIN.get().get() {
            return None;
        }
        Some(Self(const_unwrap::const_unwrap_option(NonZeroU32::new(frame_size))))
    }

    const fn get(&self) -> NonZeroU32 {
        let Self(frame_size) = *self;
        frame_size
    }

    /// Converts the maximum frame size to its corresponding MTU.
    pub const fn as_mtu(&self) -> Mtu {
        // MTU must be positive because of the limit on minimum ethernet frame size
        Mtu::new(self.get().get().saturating_sub(ETHERNET_HDR_LEN_NO_TAG_U32))
    }

    /// Creates the maximum ethernet frame size from MTU.
    pub const fn from_mtu(mtu: Mtu) -> Option<MaxFrameSize> {
        let frame_size = mtu.get().saturating_add(ETHERNET_HDR_LEN_NO_TAG_U32);
        Self::new(frame_size)
    }
}

/// Builder for [`EthernetDeviceState`].
pub(crate) struct EthernetDeviceStateBuilder {
    mac: UnicastAddr<Mac>,
    max_frame_size: MaxFrameSize,
}

impl EthernetDeviceStateBuilder {
    /// Create a new `EthernetDeviceStateBuilder`.
    pub(crate) fn new(mac: UnicastAddr<Mac>, max_frame_size: MaxFrameSize) -> Self {
        // TODO(https://fxbug.dev/121480): Add a minimum frame size for all
        // Ethernet devices such that you can't create an `EthernetDeviceState`
        // with a `MaxFrameSize` smaller than the minimum. The absolute minimum
        // needs to be at least the minimum body size of an Ethernet frame. For
        // IPv6-capable devices, the minimum needs to be higher - the frame size
        // implied by the IPv6 minimum MTU. The easy path is to simply use that
        // frame size as the minimum in all cases, although we may at some point
        // want to figure out how to configure devices which don't support IPv6,
        // and allow smaller frame sizes for those devices.
        //
        // A few questions:
        // - How do we wire error information back up the call stack? Should
        //   this just return a Result or something?
        Self { mac, max_frame_size }
    }

    /// Build the `EthernetDeviceState` from this builder.
    pub(super) fn build(self) -> EthernetDeviceState {
        let Self { mac, max_frame_size } = self;

        EthernetDeviceState {
            ipv4_arp: Default::default(),
            ipv6_nud: Default::default(),
            static_state: StaticEthernetDeviceState { mac, max_frame_size },
            dynamic_state: RwLock::new(DynamicEthernetDeviceState::new(max_frame_size)),
            tx_queue: Default::default(),
        }
    }
}

pub(crate) struct DynamicEthernetDeviceState {
    /// The value this netstack assumes as the device's maximum frame size.
    max_frame_size: MaxFrameSize,

    /// A flag indicating whether the device will accept all ethernet frames
    /// that it receives, regardless of the ethernet frame's destination MAC
    /// address.
    promiscuous_mode: bool,

    /// Link multicast groups this device has joined.
    link_multicast_groups: RefCountedHashSet<MulticastAddr<Mac>>,
}

impl DynamicEthernetDeviceState {
    fn new(max_frame_size: MaxFrameSize) -> Self {
        Self { max_frame_size, promiscuous_mode: false, link_multicast_groups: Default::default() }
    }
}

pub(crate) struct StaticEthernetDeviceState {
    /// Mac address of the device this state is for.
    mac: UnicastAddr<Mac>,

    /// The maximum frame size allowed by the hardware.
    max_frame_size: MaxFrameSize,
}

/// The state associated with an Ethernet device.
pub(crate) struct EthernetDeviceState {
    /// IPv4 ARP state.
    ipv4_arp: Mutex<ArpState<EthernetLinkDevice>>,

    /// IPv6 NUD state.
    ipv6_nud: Mutex<NudState<Ipv6, Mac>>,

    static_state: StaticEthernetDeviceState,

    dynamic_state: RwLock<DynamicEthernetDeviceState>,

    tx_queue: TransmitQueue<(), Buf<Vec<u8>>, BufVecU8Allocator>,
}

impl<I: Instant, S> UnlockedAccess<crate::lock_ordering::EthernetDeviceStaticState>
    for IpLinkDeviceState<I, S, EthernetDeviceState>
{
    type Data<'l> = &'l StaticEthernetDeviceState
        where
            Self: 'l ;
    fn access(&self) -> Self::Data<'_> {
        &self.link.static_state
    }
}

impl<I: Instant, S> RwLockFor<crate::lock_ordering::EthernetDeviceDynamicState>
    for IpLinkDeviceState<I, S, EthernetDeviceState>
{
    type ReadData<'l> = crate::sync::RwLockReadGuard<'l, DynamicEthernetDeviceState>
        where
            Self: 'l;
    type WriteData<'l> = crate::sync::RwLockWriteGuard<'l, DynamicEthernetDeviceState>
        where
            Self: 'l;

    fn read_lock(&self) -> Self::ReadData<'_> {
        self.link.dynamic_state.read()
    }
    fn write_lock(&self) -> Self::WriteData<'_> {
        self.link.dynamic_state.write()
    }
}

impl<I: Instant> RwLockFor<crate::lock_ordering::EthernetDeviceIpState<Ipv4>>
    for DualStackIpDeviceState<I>
{
    type ReadData<'l> = crate::sync::RwLockReadGuard<'l, Ipv4DeviceState<I>>
        where
            Self: 'l;
    type WriteData<'l> = crate::sync::RwLockWriteGuard<'l, Ipv4DeviceState<I>>
        where
            Self: 'l;
    fn read_lock(&self) -> Self::ReadData<'_> {
        self.ipv4.read()
    }
    fn write_lock(&self) -> Self::WriteData<'_> {
        self.ipv4.write()
    }
}

impl<I: Instant> RwLockFor<crate::lock_ordering::EthernetDeviceIpState<Ipv6>>
    for DualStackIpDeviceState<I>
{
    type ReadData<'l> = crate::sync::RwLockReadGuard<'l, Ipv6DeviceState<I>>
        where
            Self: 'l;
    type WriteData<'l> = crate::sync::RwLockWriteGuard<'l, Ipv6DeviceState<I>>
        where
            Self: 'l;
    fn read_lock(&self) -> Self::ReadData<'_> {
        self.ipv6.read()
    }
    fn write_lock(&self) -> Self::WriteData<'_> {
        self.ipv6.write()
    }
}

impl<I: Instant, S> LockFor<crate::lock_ordering::EthernetIpv6Nud>
    for IpLinkDeviceState<I, S, EthernetDeviceState>
{
    type Data<'l> = crate::sync::LockGuard<'l, NudState<Ipv6, Mac>>
        where
            Self: 'l;
    fn lock(&self) -> Self::Data<'_> {
        self.link.ipv6_nud.lock()
    }
}

impl<I: Instant, S> LockFor<crate::lock_ordering::EthernetIpv4Arp>
    for IpLinkDeviceState<I, S, EthernetDeviceState>
{
    type Data<'l> = crate::sync::LockGuard<'l, ArpState<EthernetLinkDevice>>
        where
            Self: 'l;
    fn lock(&self) -> Self::Data<'_> {
        self.link.ipv4_arp.lock()
    }
}

impl<I: Instant, S> LockFor<crate::lock_ordering::EthernetTxQueue>
    for IpLinkDeviceState<I, S, EthernetDeviceState>
{
    type Data<'l> = crate::sync::LockGuard<'l, TransmitQueueState<(), Buf<Vec<u8>>, BufVecU8Allocator>>
        where
            Self: 'l;
    fn lock(&self) -> Self::Data<'_> {
        self.link.tx_queue.queue.lock()
    }
}

impl<I: Instant, S> LockFor<crate::lock_ordering::EthernetTxDequeue>
    for IpLinkDeviceState<I, S, EthernetDeviceState>
{
    type Data<'l> = crate::sync::LockGuard<'l, DequeueState<(), Buf<Vec<u8>>>>
        where
            Self: 'l;
    fn lock(&self) -> Self::Data<'_> {
        self.link.tx_queue.deque.lock()
    }
}

impl<C: NonSyncContext>
    TransmitQueueNonSyncContext<
        EthernetLinkDevice,
        EthernetDeviceId<C::Instant, C::EthernetDeviceState>,
    > for C
{
    fn wake_tx_task(&mut self, device_id: &EthernetDeviceId<C::Instant, C::EthernetDeviceState>) {
        DeviceLayerEventDispatcher::wake_tx_task(self, &device_id.clone().into())
    }
}

// TODO(https://fxbug.dev/121448): Remove this when it is unused.
impl<'a, C: NonSyncContext> TransmitQueueTypes<EthernetLinkDevice, C> for &'a SyncCtx<C> {
    type Meta = <Locked<'a, SyncCtx<C>, crate::lock_ordering::Unlocked> as TransmitQueueTypes<
        EthernetLinkDevice,
        C,
    >>::Meta;
    type Allocator =
        <Locked<'a, SyncCtx<C>, crate::lock_ordering::Unlocked> as TransmitQueueTypes<
            EthernetLinkDevice,
            C,
        >>::Allocator;
    type Buffer = <Locked<'a, SyncCtx<C>, crate::lock_ordering::Unlocked> as TransmitQueueTypes<
        EthernetLinkDevice,
        C,
    >>::Buffer;
}

// TODO(https://fxbug.dev/121448): Remove this when it is unused.
impl<C: NonSyncContext> TransmitQueueContext<EthernetLinkDevice, C> for &'_ SyncCtx<C> {
    fn with_transmit_queue_mut<
        O,
        F: FnOnce(&mut TransmitQueueState<Self::Meta, Self::Buffer, Self::Allocator>) -> O,
    >(
        &mut self,
        device_id: &EthernetDeviceId<C::Instant, C::EthernetDeviceState>,
        cb: F,
    ) -> O {
        TransmitQueueContext::<EthernetLinkDevice, _>::with_transmit_queue_mut(
            &mut Locked::new(*self),
            device_id,
            cb,
        )
    }

    fn send_frame(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        meta: Self::Meta,
        buf: Self::Buffer,
    ) -> Result<(), DeviceSendFrameError<(Self::Meta, Self::Buffer)>> {
        TransmitQueueContext::<EthernetLinkDevice, _>::send_frame(
            &mut Locked::new(*self),
            ctx,
            device_id,
            meta,
            buf,
        )
    }
}

// TODO(https://fxbug.dev/121448): Remove this when it is unused.
impl<C: NonSyncContext> TransmitDequeueContext<EthernetLinkDevice, C> for &'_ SyncCtx<C> {
    type TransmitQueueCtx<'a> =
        <Locked<'a, SyncCtx<C>, crate::lock_ordering::Unlocked> as TransmitDequeueContext<
            EthernetLinkDevice,
            C,
        >>::TransmitQueueCtx<'a>;

    fn with_dequed_packets_and_tx_queue_ctx<
        O,
        F: FnOnce(&mut DequeueState<Self::Meta, Self::Buffer>, &mut Self::TransmitQueueCtx<'_>) -> O,
    >(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O {
        TransmitDequeueContext::<EthernetLinkDevice, C>::with_dequed_packets_and_tx_queue_ctx(
            &mut Locked::new(*self),
            device_id,
            cb,
        )
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::EthernetTxQueue>>
    TransmitQueueTypes<EthernetLinkDevice, C> for Locked<'_, SyncCtx<C>, L>
{
    type Meta = ();
    type Allocator = BufVecU8Allocator;
    type Buffer = Buf<Vec<u8>>;
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::EthernetTxQueue>>
    TransmitQueueContext<EthernetLinkDevice, C> for Locked<'_, SyncCtx<C>, L>
{
    fn with_transmit_queue_mut<
        O,
        F: FnOnce(&mut TransmitQueueState<Self::Meta, Self::Buffer, Self::Allocator>) -> O,
    >(
        &mut self,
        device_id: &EthernetDeviceId<C::Instant, C::EthernetDeviceState>,
        cb: F,
    ) -> O {
        with_ethernet_state(self, device_id, |mut state| {
            let mut x = state.lock::<crate::lock_ordering::EthernetTxQueue>();
            cb(&mut x)
        })
    }

    fn send_frame(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        meta: Self::Meta,
        buf: Self::Buffer,
    ) -> Result<(), DeviceSendFrameError<(Self::Meta, Self::Buffer)>> {
        DeviceLayerEventDispatcher::send_frame(ctx, device_id, buf).map_err(
            |DeviceSendFrameError::DeviceNotReady(buf)| {
                DeviceSendFrameError::DeviceNotReady((meta, buf))
            },
        )
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::EthernetTxDequeue>>
    TransmitDequeueContext<EthernetLinkDevice, C> for Locked<'_, SyncCtx<C>, L>
{
    type TransmitQueueCtx<'a> = Locked<'a, SyncCtx<C>, crate::lock_ordering::EthernetTxDequeue>;

    fn with_dequed_packets_and_tx_queue_ctx<
        O,
        F: FnOnce(&mut DequeueState<Self::Meta, Self::Buffer>, &mut Self::TransmitQueueCtx<'_>) -> O,
    >(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O {
        with_ethernet_state_and_sync_ctx(self, device_id, |mut state, sync_ctx| {
            let mut x = state.lock::<crate::lock_ordering::EthernetTxDequeue>();
            let mut locked = sync_ctx.cast_locked();
            cb(&mut x, &mut locked)
        })
    }
}

/// Should a packet with destination MAC address, `dst`, be accepted by this
/// device?
///
/// Returns `true` if this device is in promiscuous mode or the frame is
/// destined for this device.
fn should_deliver(
    static_state: &StaticEthernetDeviceState,
    dynamic_state: &DynamicEthernetDeviceState,
    dst_mac: &Mac,
) -> bool {
    dynamic_state.promiscuous_mode
        || (static_state.mac.get() == *dst_mac)
        || dst_mac.is_broadcast()
        || (MulticastAddr::new(*dst_mac)
            .map(|a| dynamic_state.link_multicast_groups.contains(&a))
            .unwrap_or(false))
}

/// A timer ID for Ethernet devices.
///
/// `D` is the type of device ID that identifies different Ethernet devices.
#[derive(Clone, Eq, PartialEq, Debug, Hash)]
pub(crate) enum EthernetTimerId<D> {
    Arp(ArpTimerId<EthernetLinkDevice, D>),
    Nudv6(NudTimerId<Ipv6, EthernetLinkDevice, D>),
}

impl<D> From<ArpTimerId<EthernetLinkDevice, D>> for EthernetTimerId<D> {
    fn from(id: ArpTimerId<EthernetLinkDevice, D>) -> EthernetTimerId<D> {
        EthernetTimerId::Arp(id)
    }
}

impl<D> From<NudTimerId<Ipv6, EthernetLinkDevice, D>> for EthernetTimerId<D> {
    fn from(id: NudTimerId<Ipv6, EthernetLinkDevice, D>) -> EthernetTimerId<D> {
        EthernetTimerId::Nudv6(id)
    }
}

/// Handle an Ethernet timer firing.
pub(super) fn handle_timer<
    C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
    SC: EthernetIpLinkDeviceContext<C>
        + TimerHandler<C, NudTimerId<Ipv6, EthernetLinkDevice, SC::DeviceId>>
        + TimerHandler<C, ArpTimerId<EthernetLinkDevice, SC::DeviceId>>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    id: EthernetTimerId<SC::DeviceId>,
) {
    match id {
        EthernetTimerId::Arp(id) => TimerHandler::handle_timer(sync_ctx, ctx, id),
        EthernetTimerId::Nudv6(id) => TimerHandler::handle_timer(sync_ctx, ctx, id),
    }
}

// If we are provided with an impl of `TimerContext<EthernetTimerId<_>>`, then
// we can in turn provide impls of `TimerContext` for ARP, NDP, IGMP, and MLD
// timers.
impl_timer_context!(
    DeviceId,
    EthernetTimerId<DeviceId>,
    ArpTimerId<EthernetLinkDevice, DeviceId>,
    EthernetTimerId::Arp(id),
    id
);
impl_timer_context!(
    DeviceId,
    EthernetTimerId<DeviceId>,
    NudTimerId<Ipv6, EthernetLinkDevice, DeviceId>,
    EthernetTimerId::Nudv6(id),
    id
);

/// Send an IP packet in an Ethernet frame.
///
/// `send_ip_frame` accepts a device ID, a local IP address, and a
/// serializer. It computes the routing information, serializes
/// the serializer, and sends the resulting buffer in a new Ethernet
/// frame.
pub(super) fn send_ip_frame<
    B: BufferMut,
    C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
    SC: EthernetIpLinkDeviceContext<C>
        + BufferNudHandler<B, A::Version, EthernetLinkDevice, C>
        + BufferTransmitQueueHandler<EthernetLinkDevice, B, C, Meta = ()>,
    A: IpAddress,
    S: Serializer<Buffer = B>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    device_id: &SC::DeviceId,
    local_addr: SpecifiedAddr<A>,
    body: S,
) -> Result<(), S>
where
    A::Version: EthernetIpExt,
{
    ctx.increment_debug_counter("ethernet::send_ip_frame");

    trace!("ethernet::send_ip_frame: local_addr = {:?}; device = {:?}", local_addr, device_id);

    let body = body.with_size_limit(get_mtu(sync_ctx, device_id).get() as usize);

    if let Some(multicast) = MulticastAddr::new(local_addr.get()) {
        send_ip_frame_to_dst(
            sync_ctx,
            ctx,
            device_id,
            Mac::from(&multicast),
            body,
            A::Version::ETHER_TYPE,
        )
    } else {
        BufferNudHandler::<_, A::Version, _, _>::send_ip_packet_to_neighbor(
            sync_ctx, ctx, device_id, local_addr, body,
        )
    }
    .map_err(Nested::into_inner)
}

/// Receive an Ethernet frame from the network.
pub(super) fn receive_frame<
    C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
    B: BufferMut,
    SC: BufferEthernetIpLinkDeviceContext<C, B> + ArpPacketHandler<B, EthernetLinkDevice, C>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    device_id: &SC::DeviceId,
    mut buffer: B,
) {
    trace!("ethernet::receive_frame: device_id = {:?}", device_id);
    // NOTE(joshlf): We do not currently validate that the Ethernet frame
    // satisfies the minimum length requirement. We expect that if this
    // requirement is necessary (due to requirements of the physical medium),
    // the driver or hardware will have checked it, and that if this requirement
    // is not necessary, it is acceptable for us to operate on a smaller
    // Ethernet frame. If this becomes insufficient in the future, we may want
    // to consider making this behavior configurable (at compile time, at
    // runtime on a global basis, or at runtime on a per-device basis).
    let frame = if let Ok(frame) =
        buffer.parse_with::<_, EthernetFrame<_>>(EthernetFrameLengthCheck::NoCheck)
    {
        frame
    } else {
        trace!("ethernet::receive_frame: failed to parse ethernet frame");
        // TODO(joshlf): Do something else?
        return;
    };

    let (_, dst) = (frame.src_mac(), frame.dst_mac());

    let should_deliver = sync_ctx
        .with_ethernet_device_state(device_id, |static_state, dynamic_state| {
            should_deliver(static_state, dynamic_state, &dst)
        });
    if !should_deliver {
        trace!("ethernet::receive_frame: destination mac {:?} not for device {:?}", dst, device_id);
        return;
    }

    let frame_dst = FrameDestination::from(dst);

    match frame.ethertype() {
        Some(EtherType::Arp) => {
            let types = if let Ok(types) = peek_arp_types(buffer.as_ref()) {
                types
            } else {
                // TODO(joshlf): Do something else here?
                return;
            };
            match types {
                (ArpHardwareType::Ethernet, ArpNetworkType::Ipv4) => {
                    ArpPacketHandler::handle_packet(sync_ctx, ctx, device_id.clone(), buffer)
                }
            }
        }
        Some(EtherType::Ipv4) => sync_ctx.receive_frame(
            ctx,
            RecvIpFrameMeta::<_, Ipv4>::new(device_id.clone(), frame_dst),
            buffer,
        ),
        Some(EtherType::Ipv6) => sync_ctx.receive_frame(
            ctx,
            RecvIpFrameMeta::<_, Ipv6>::new(device_id.clone(), frame_dst),
            buffer,
        ),
        Some(EtherType::Other(_)) | None => {} // TODO(joshlf)
    }
}

/// Set the promiscuous mode flag on `device_id`.
pub(super) fn set_promiscuous_mode<
    C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
    SC: EthernetIpLinkDeviceContext<C>,
>(
    sync_ctx: &mut SC,
    _ctx: &mut C,
    device_id: &SC::DeviceId,
    enabled: bool,
) {
    sync_ctx.with_ethernet_device_state_mut(device_id, |_static_state, dynamic_state| {
        dynamic_state.promiscuous_mode = enabled
    })
}

/// Add `device_id` to a link multicast group `multicast_addr`.
///
/// Calling `join_link_multicast` with the same `device_id` and `multicast_addr`
/// is completely safe. A counter will be kept for the number of times
/// `join_link_multicast` has been called with the same `device_id` and
/// `multicast_addr` pair. To completely leave a multicast group,
/// [`leave_link_multicast`] must be called the same number of times
/// `join_link_multicast` has been called for the same `device_id` and
/// `multicast_addr` pair. The first time `join_link_multicast` is called for a
/// new `device` and `multicast_addr` pair, the device will actually join the
/// multicast group.
///
/// `join_link_multicast` is different from [`join_ip_multicast`] as
/// `join_link_multicast` joins an L2 multicast group, whereas
/// `join_ip_multicast` joins an L3 multicast group.
pub(super) fn join_link_multicast<
    C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
    SC: EthernetIpLinkDeviceContext<C>,
>(
    sync_ctx: &mut SC,
    _ctx: &mut C,
    device_id: &SC::DeviceId,
    multicast_addr: MulticastAddr<Mac>,
) {
    sync_ctx.with_ethernet_device_state_mut(device_id, |_static_state, dynamic_state| {
        let groups = &mut dynamic_state.link_multicast_groups;

        match groups.insert(multicast_addr) {
            InsertResult::Inserted(()) => {
                trace!(
                    "ethernet::join_link_multicast: joining link multicast {:?}",
                    multicast_addr
                );
            }
            InsertResult::AlreadyPresent => {
                trace!(
                    "ethernet::join_link_multicast: already joined link multicast {:?}",
                    multicast_addr,
                );
            }
        }
    })
}

/// Remove `device_id` from a link multicast group `multicast_addr`.
///
/// `leave_link_multicast` will attempt to remove `device_id` from the multicast
/// group `multicast_addr`. `device_id` may have "joined" the same multicast
/// address multiple times, so `device_id` will only leave the multicast group
/// once `leave_ip_multicast` has been called for each corresponding
/// [`join_link_multicast`]. That is, if `join_link_multicast` gets called 3
/// times and `leave_link_multicast` gets called two times (after all 3
/// `join_link_multicast` calls), `device_id` will still be in the multicast
/// group until the next (final) call to `leave_link_multicast`.
///
/// `leave_link_multicast` is different from [`leave_ip_multicast`] as
/// `leave_link_multicast` leaves an L2 multicast group, whereas
/// `leave_ip_multicast` leaves an L3 multicast group.
///
/// # Panics
///
/// If `device_id` is not in the multicast group `multicast_addr`.
pub(super) fn leave_link_multicast<
    C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
    SC: EthernetIpLinkDeviceContext<C>,
>(
    sync_ctx: &mut SC,
    _ctx: &mut C,
    device_id: &SC::DeviceId,
    multicast_addr: MulticastAddr<Mac>,
) {
    sync_ctx.with_ethernet_device_state_mut(device_id, |_static_state, dynamic_state| {
        let groups = &mut dynamic_state.link_multicast_groups;

        match groups.remove(multicast_addr) {
            RemoveResult::Removed(()) => {
                trace!("ethernet::leave_link_multicast: leaving link multicast {:?}", multicast_addr);
            }
            RemoveResult::StillPresent => {
                trace!(
                    "ethernet::leave_link_multicast: not leaving link multicast {:?} as there are still listeners for it",
                    multicast_addr,
                );
            }
            RemoveResult::NotPresent => {
                panic!(
                    "ethernet::leave_link_multicast: device {:?} has not yet joined link multicast {:?}",
                    device_id,
                    multicast_addr,
                );
            }
        }
    })
}

/// Get the MTU associated with this device.
pub(super) fn get_mtu<
    C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
    SC: EthernetIpLinkDeviceContext<C>,
>(
    sync_ctx: &mut SC,
    device_id: &SC::DeviceId,
) -> Mtu {
    sync_ctx.with_ethernet_device_state(device_id, |_static_state, dynamic_state| {
        dynamic_state.max_frame_size.as_mtu()
    })
}

/// Insert a static entry into this device's ARP table.
///
/// This will cause any conflicting dynamic entry to be removed, and
/// any future conflicting gratuitous ARPs to be ignored.
// TODO(rheacock): remove `cfg(test)` when this is used. Will probably be called
// by a pub fn in the device mod.
#[cfg(test)]
pub(super) fn insert_static_arp_table_entry<
    C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
    SC: EthernetIpLinkDeviceContext<C> + NudHandler<Ipv4, EthernetLinkDevice, C>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    device_id: &SC::DeviceId,
    addr: Ipv4Addr,
    mac: Mac,
) {
    if let Some(addr) = SpecifiedAddr::new(addr) {
        NudHandler::<Ipv4, EthernetLinkDevice, _>::set_static_neighbor(
            sync_ctx, ctx, device_id, addr, mac,
        )
    }
}

/// Insert an entry into this device's NDP table.
///
/// This method only gets called when testing to force set a neighbor's link
/// address so that lookups succeed immediately, without doing address
/// resolution.
// TODO(rheacock): Remove when this is called from non-test code.
#[cfg(test)]
pub(super) fn insert_ndp_table_entry<
    C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
    SC: EthernetIpLinkDeviceContext<C> + NudHandler<Ipv6, EthernetLinkDevice, C>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    device_id: &SC::DeviceId,
    addr: UnicastAddr<Ipv6Addr>,
    mac: Mac,
) {
    NudHandler::<Ipv6, EthernetLinkDevice, _>::set_static_neighbor(
        sync_ctx,
        ctx,
        device_id,
        addr.into_specified(),
        mac,
    )
}

impl<
        C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
        B: BufferMut,
        SC: EthernetIpLinkDeviceContext<C>
            + BufferTransmitQueueHandler<EthernetLinkDevice, B, C, Meta = ()>,
    > SendFrameContext<C, B, ArpFrameMetadata<EthernetLinkDevice, SC::DeviceId>> for SC
{
    fn send_frame<S: Serializer<Buffer = B>>(
        &mut self,
        ctx: &mut C,
        ArpFrameMetadata { device_id, dst_addr }: ArpFrameMetadata<
            EthernetLinkDevice,
            SC::DeviceId,
        >,
        body: S,
    ) -> Result<(), S> {
        send_ip_frame_to_dst(self, ctx, &device_id, dst_addr, body, EtherType::Arp)
    }
}

// TODO(https://fxbug.dev/121448): Remove this when it is unused.
impl<C: NonSyncContext> ArpContext<EthernetLinkDevice, C> for &'_ SyncCtx<C> {
    fn get_protocol_addr(
        &mut self,
        ctx: &mut C,
        device_id: &EthernetDeviceId<C::Instant, C::EthernetDeviceState>,
    ) -> Option<Ipv4Addr> {
        Locked::new(*self).get_protocol_addr(ctx, device_id)
    }

    fn get_hardware_addr(
        &mut self,
        ctx: &mut C,
        device_id: &EthernetDeviceId<C::Instant, C::EthernetDeviceState>,
    ) -> UnicastAddr<Mac> {
        Locked::new(*self).get_hardware_addr(ctx, device_id)
    }

    fn with_arp_state_mut<O, F: FnOnce(&mut ArpState<EthernetLinkDevice>) -> O>(
        &mut self,
        device_id: &EthernetDeviceId<C::Instant, C::EthernetDeviceState>,
        cb: F,
    ) -> O {
        Locked::new(*self).with_arp_state_mut(device_id, cb)
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::IpState<Ipv4>>>
    ArpContext<EthernetLinkDevice, C> for Locked<'_, SyncCtx<C>, L>
{
    fn get_protocol_addr(
        &mut self,
        _ctx: &mut C,
        device_id: &EthernetDeviceId<C::Instant, C::EthernetDeviceState>,
    ) -> Option<Ipv4Addr> {
        with_ethernet_state(self, device_id, |mut state| {
            let mut state = state.cast();
            let ipv4 = state.read_lock::<crate::lock_ordering::EthernetDeviceIpState<Ipv4>>();
            let ret = ipv4.ip_state.iter_addrs().next().cloned().map(|addr| addr.addr().get());
            ret
        })
    }

    fn get_hardware_addr(
        &mut self,
        _ctx: &mut C,
        device_id: &EthernetDeviceId<C::Instant, C::EthernetDeviceState>,
    ) -> UnicastAddr<Mac> {
        get_mac(self, device_id)
    }

    fn with_arp_state_mut<O, F: FnOnce(&mut ArpState<EthernetLinkDevice>) -> O>(
        &mut self,
        device_id: &EthernetDeviceId<C::Instant, C::EthernetDeviceState>,
        cb: F,
    ) -> O {
        with_ethernet_state(self, device_id, |mut state| {
            let mut arp = state.lock::<crate::lock_ordering::EthernetIpv4Arp>();
            cb(&mut arp)
        })
    }
}

// TODO(https://fxbug.dev/121448): Remove this when it is unused.
impl<B: BufferMut, C: BufferNonSyncContext<B>> BufferArpContext<EthernetLinkDevice, C, B>
    for &'_ SyncCtx<C>
{
    fn send_ip_packet_to_neighbor_link_addr<S: Serializer<Buffer = B>>(
        &mut self,
        ctx: &mut C,
        device_id: &EthernetDeviceId<C::Instant, C::EthernetDeviceState>,
        dst_mac: Mac,
        body: S,
    ) -> Result<(), S> {
        BufferArpContext::send_ip_packet_to_neighbor_link_addr(
            &mut Locked::new(*self),
            ctx,
            device_id,
            dst_mac,
            body,
        )
    }
}

impl<
        B: BufferMut,
        C: BufferNonSyncContext<B>,
        L: LockBefore<crate::lock_ordering::IpState<Ipv4>>,
    > BufferArpContext<EthernetLinkDevice, C, B> for Locked<'_, SyncCtx<C>, L>
{
    fn send_ip_packet_to_neighbor_link_addr<S: Serializer<Buffer = B>>(
        &mut self,
        ctx: &mut C,
        device_id: &EthernetDeviceId<C::Instant, C::EthernetDeviceState>,
        dst_mac: Mac,
        body: S,
    ) -> Result<(), S> {
        send_ip_frame_to_dst(self, ctx, device_id, dst_mac, body, EtherType::Ipv4)
    }
}

pub(super) fn get_mac<
    'a,
    C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
    SC: EthernetIpLinkDeviceContext<C>,
>(
    sync_ctx: &'a mut SC,
    device_id: &SC::DeviceId,
) -> UnicastAddr<Mac> {
    sync_ctx.with_static_ethernet_device_state(device_id, |state| state.mac)
}

pub(super) fn set_mtu<
    C: EthernetIpLinkDeviceNonSyncContext<SC::DeviceId>,
    SC: EthernetIpLinkDeviceContext<C>,
>(
    sync_ctx: &mut SC,
    device_id: &SC::DeviceId,
    mtu: Mtu,
) {
    sync_ctx.with_ethernet_device_state_mut(device_id, |static_state, dynamic_state| {
        if let Some(mut frame_size ) = MaxFrameSize::from_mtu(mtu) {
            // If `frame_size` is greater than what the device supports, set it
            // to maximum frame size the device supports.
            if frame_size > static_state.max_frame_size {
                trace!("ethernet::ndp_device::set_mtu: MTU of {:?} is greater than the device {:?}'s max MTU of {:?}, using device's max MTU instead", mtu, device_id, static_state.max_frame_size.as_mtu());
                frame_size = static_state.max_frame_size;
            }
            trace!("ethernet::ndp_device::set_mtu: setting link MTU to {:?}", mtu);
            dynamic_state.max_frame_size = frame_size;
        }
    })
}

/// An implementation of the [`LinkDevice`] trait for Ethernet devices.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub(crate) enum EthernetLinkDevice {}

impl Device for EthernetLinkDevice {}

impl LinkDevice for EthernetLinkDevice {
    type Address = Mac;
    type State = EthernetDeviceState;
}

#[cfg(test)]
mod tests {
    use alloc::{collections::hash_map::HashMap, vec, vec::Vec};

    use ip_test_macro::ip_test;
    use net_types::ip::{AddrSubnet, Ip, IpAddr, IpVersion};
    use packet::Buf;
    use packet_formats::{
        icmp::IcmpDestUnreachable,
        ip::{IpExt, IpPacketBuilder, IpProto},
        testdata::{dns_request_v4, dns_request_v6},
        testutil::{
            parse_icmp_packet_in_ip_packet_in_ethernet_frame, parse_ip_packet_in_ethernet_frame,
        },
    };
    use rand::Rng;
    use test_case::test_case;

    use super::*;
    use crate::{
        context::testutil::FakeFrameCtx,
        device::DeviceId,
        error::{ExistsError, NotFoundError},
        ip::{
            device::{
                is_ip_routing_enabled, nud::DynamicNeighborUpdateSource, set_routing_enabled,
                state::AssignedAddress,
            },
            dispatch_receive_ip_packet_name, receive_ip_packet,
            testutil::{is_in_ip_multicast, FakeDeviceId},
        },
        testutil::{
            add_arp_or_ndp_table_entry, assert_empty, get_counter_val, new_rng,
            FakeEventDispatcherBuilder, TestIpExt, FAKE_CONFIG_V4, IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
        },
        Ctx,
    };

    struct FakeEthernetCtx {
        static_state: StaticEthernetDeviceState,
        dynamic_state: DynamicEthernetDeviceState,
        static_arp_entries: HashMap<SpecifiedAddr<Ipv4Addr>, Mac>,
        tx_queue: TransmitQueueState<(), Buf<Vec<u8>>, BufVecU8Allocator>,
    }

    impl FakeEthernetCtx {
        fn new(mac: UnicastAddr<Mac>, max_frame_size: MaxFrameSize) -> FakeEthernetCtx {
            FakeEthernetCtx {
                static_state: StaticEthernetDeviceState { max_frame_size, mac },
                dynamic_state: DynamicEthernetDeviceState::new(max_frame_size),
                static_arp_entries: Default::default(),
                tx_queue: Default::default(),
            }
        }
    }

    type FakeNonSyncCtx =
        crate::context::testutil::FakeNonSyncCtx<EthernetTimerId<FakeDeviceId>, (), ()>;

    type FakeCtx =
        crate::context::testutil::FakeSyncCtx<FakeEthernetCtx, FakeDeviceId, FakeDeviceId>;

    impl EthernetIpLinkDeviceContext<FakeNonSyncCtx> for FakeCtx {
        fn with_static_ethernet_device_state<O, F: FnOnce(&StaticEthernetDeviceState) -> O>(
            &mut self,
            &FakeDeviceId: &FakeDeviceId,
            cb: F,
        ) -> O {
            cb(&self.get_ref().static_state)
        }

        fn with_ethernet_device_state<
            O,
            F: FnOnce(&StaticEthernetDeviceState, &DynamicEthernetDeviceState) -> O,
        >(
            &mut self,
            &FakeDeviceId: &FakeDeviceId,
            cb: F,
        ) -> O {
            let state = self.get_ref();
            cb(&state.static_state, &state.dynamic_state)
        }

        fn with_ethernet_device_state_mut<
            O,
            F: FnOnce(&StaticEthernetDeviceState, &mut DynamicEthernetDeviceState) -> O,
        >(
            &mut self,
            &FakeDeviceId: &FakeDeviceId,
            cb: F,
        ) -> O {
            let state = self.get_mut();
            cb(&state.static_state, &mut state.dynamic_state)
        }
    }

    impl NudHandler<Ipv6, EthernetLinkDevice, FakeNonSyncCtx> for FakeCtx {
        fn set_dynamic_neighbor(
            &mut self,
            _ctx: &mut FakeNonSyncCtx,
            _device_id: &Self::DeviceId,
            _neighbor: SpecifiedAddr<Ipv6Addr>,
            _link_addr: Mac,
            _is_confirmation: DynamicNeighborUpdateSource,
        ) {
            unimplemented!()
        }

        fn set_static_neighbor(
            &mut self,
            _ctx: &mut FakeNonSyncCtx,
            _device_id: &Self::DeviceId,
            _neighbor: SpecifiedAddr<Ipv6Addr>,
            _link_addr: Mac,
        ) {
            unimplemented!()
        }

        fn flush(&mut self, _ctx: &mut FakeNonSyncCtx, _device_id: &Self::DeviceId) {
            unimplemented!()
        }
    }

    impl<B: BufferMut> BufferNudHandler<B, Ipv6, EthernetLinkDevice, FakeNonSyncCtx> for FakeCtx {
        fn send_ip_packet_to_neighbor<S: Serializer<Buffer = B>>(
            &mut self,
            _ctx: &mut FakeNonSyncCtx,
            _device_id: &Self::DeviceId,
            _lookup_addr: SpecifiedAddr<Ipv6Addr>,
            _body: S,
        ) -> Result<(), S> {
            unimplemented!()
        }
    }

    impl NudHandler<Ipv4, EthernetLinkDevice, FakeNonSyncCtx> for FakeCtx {
        fn set_dynamic_neighbor(
            &mut self,
            _ctx: &mut FakeNonSyncCtx,
            _device_id: &Self::DeviceId,
            _neighbor: SpecifiedAddr<Ipv4Addr>,
            _link_addr: Mac,
            _is_confirmation: DynamicNeighborUpdateSource,
        ) {
            unimplemented!()
        }

        fn set_static_neighbor(
            &mut self,
            _ctx: &mut FakeNonSyncCtx,
            _device_id: &Self::DeviceId,
            neighbor: SpecifiedAddr<Ipv4Addr>,
            link_addr: Mac,
        ) {
            let _: Option<Mac> = self.get_mut().static_arp_entries.insert(neighbor, link_addr);
        }

        fn flush(&mut self, _ctx: &mut FakeNonSyncCtx, _device_id: &Self::DeviceId) {
            unimplemented!()
        }
    }

    impl<B: BufferMut> BufferNudHandler<B, Ipv4, EthernetLinkDevice, FakeNonSyncCtx> for FakeCtx {
        fn send_ip_packet_to_neighbor<S: Serializer<Buffer = B>>(
            &mut self,
            ctx: &mut FakeNonSyncCtx,
            device_id: &Self::DeviceId,
            lookup_addr: SpecifiedAddr<Ipv4Addr>,
            body: S,
        ) -> Result<(), S> {
            if let Some(link_addr) = self.get_ref().static_arp_entries.get(&lookup_addr).cloned() {
                send_ip_frame_to_dst(self, ctx, device_id, link_addr, body, EtherType::Ipv4)
            } else {
                Ok(())
            }
        }
    }

    impl TransmitQueueNonSyncContext<EthernetLinkDevice, FakeDeviceId> for FakeNonSyncCtx {
        fn wake_tx_task(&mut self, FakeDeviceId: &FakeDeviceId) {
            unimplemented!("unused by tests")
        }
    }

    impl TransmitQueueTypes<EthernetLinkDevice, FakeNonSyncCtx> for FakeCtx {
        type Meta = ();
        type Allocator = BufVecU8Allocator;
        type Buffer = Buf<Vec<u8>>;
    }

    impl TransmitQueueContext<EthernetLinkDevice, FakeNonSyncCtx> for FakeCtx {
        fn with_transmit_queue_mut<
            O,
            F: FnOnce(&mut TransmitQueueState<Self::Meta, Self::Buffer, Self::Allocator>) -> O,
        >(
            &mut self,
            _device_id: &Self::DeviceId,
            cb: F,
        ) -> O {
            cb(&mut self.get_mut().tx_queue)
        }

        fn send_frame(
            &mut self,
            _ctx: &mut FakeNonSyncCtx,
            device_id: &Self::DeviceId,
            (): Self::Meta,
            buf: Self::Buffer,
        ) -> Result<(), DeviceSendFrameError<(Self::Meta, Self::Buffer)>> {
            let frame_ctx: &mut FakeFrameCtx<_> = self.as_mut();
            frame_ctx.push(device_id.clone(), buf.as_ref().to_vec());
            Ok(())
        }
    }

    impl DeviceIdContext<EthernetLinkDevice> for FakeCtx {
        type DeviceId = FakeDeviceId;
    }

    fn contains_addr<A: IpAddress>(
        sync_ctx: &mut &crate::testutil::FakeSyncCtx,
        device: &DeviceId<crate::testutil::FakeNonSyncCtx>,
        addr: SpecifiedAddr<A>,
    ) -> bool {
        match addr.into() {
            IpAddr::V4(addr) => {
                crate::ip::device::IpDeviceStateAccessor::<Ipv4, _>::with_ip_device_state(
                    sync_ctx,
                    device,
                    |state| state.ip_state.iter_addrs().any(|a| a.addr() == addr),
                )
            }
            IpAddr::V6(addr) => {
                crate::ip::device::IpDeviceStateAccessor::<Ipv6, _>::with_ip_device_state(
                    sync_ctx,
                    device,
                    |state| state.ip_state.iter_addrs().any(|a| a.addr() == addr),
                )
            }
        }
    }

    #[test]
    fn test_mtu() {
        // Test that we send an Ethernet frame whose size is less than the MTU,
        // and that we don't send an Ethernet frame whose size is greater than
        // the MTU.
        fn test(size: usize, expect_frames_sent: usize) {
            let crate::context::testutil::FakeCtx { mut sync_ctx, mut non_sync_ctx } =
                crate::context::testutil::FakeCtx::with_sync_ctx(FakeCtx::with_state(
                    FakeEthernetCtx::new(FAKE_CONFIG_V4.local_mac, IPV6_MIN_IMPLIED_MAX_FRAME_SIZE),
                ));

            insert_static_arp_table_entry(
                &mut sync_ctx,
                &mut non_sync_ctx,
                &FakeDeviceId,
                FAKE_CONFIG_V4.remote_ip.get(),
                FAKE_CONFIG_V4.remote_mac.get(),
            );
            let _ = send_ip_frame(
                &mut sync_ctx,
                &mut non_sync_ctx,
                &FakeDeviceId,
                FAKE_CONFIG_V4.remote_ip,
                Buf::new(&mut vec![0; size], ..),
            );
            assert_eq!(sync_ctx.frames().len(), expect_frames_sent);
        }

        test(usize::try_from(u32::from(Ipv6::MINIMUM_LINK_MTU)).unwrap(), 1);
        test(usize::try_from(u32::from(Ipv6::MINIMUM_LINK_MTU)).unwrap() + 1, 0);
    }

    #[ip_test]
    #[test_case(true; "enabled")]
    #[test_case(false; "disabled")]
    fn test_receive_ip_frame<I: Ip + TestIpExt>(enable: bool) {
        // Should only receive a frame if the device is enabled.

        let config = I::FAKE_CONFIG;
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device = crate::device::add_ethernet_device(
            &mut sync_ctx,
            config.local_mac,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
        )
        .into();

        let mut bytes = match I::VERSION {
            IpVersion::V4 => dns_request_v4::ETHERNET_FRAME,
            IpVersion::V6 => dns_request_v6::ETHERNET_FRAME,
        }
        .bytes
        .to_vec();

        let mac_bytes = config.local_mac.bytes();
        bytes[0..6].copy_from_slice(&mac_bytes);

        let expected_received = if enable {
            crate::device::testutil::enable_device(&mut sync_ctx, &mut non_sync_ctx, &device);
            1
        } else {
            0
        };

        crate::device::receive_frame(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            Buf::new(bytes, ..),
        )
        .expect("error receiving frame");

        let counter = match I::VERSION {
            IpVersion::V4 => "receive_ipv4_packet",
            IpVersion::V6 => "receive_ipv6_packet",
        };
        assert_eq!(get_counter_val(&non_sync_ctx, counter), expected_received);
    }

    #[ip_test]
    #[test_case(true; "enabled")]
    #[test_case(false; "disabled")]
    fn test_send_ip_frame<I: Ip + TestIpExt>(enable: bool) {
        // Should only send a frame if the device is enabled.

        let config = I::FAKE_CONFIG;
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device = crate::device::add_ethernet_device(
            &mut sync_ctx,
            config.local_mac,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
        )
        .into();

        let expected_sent = if enable {
            crate::device::testutil::enable_device(&mut sync_ctx, &mut non_sync_ctx, &device);
            1
        } else {
            0
        };

        match I::VERSION {
            IpVersion::V4 => {
                let addr = SpecifiedAddr::new(dns_request_v4::IPV4_PACKET.metadata.dst_ip).unwrap();
                crate::device::insert_static_arp_table_entry(
                    &mut sync_ctx,
                    &mut non_sync_ctx,
                    &device,
                    addr.get(),
                    config.remote_mac,
                )
                .expect("insert static ARP entry");

                crate::ip::device::send_ip_frame::<Ipv4, _, _, _, _>(
                    &mut sync_ctx,
                    &mut non_sync_ctx,
                    &device,
                    addr,
                    Buf::new(dns_request_v4::IPV4_PACKET.bytes.to_vec(), ..),
                )
                .expect("error sending IPv4 frame")
            }

            IpVersion::V6 => {
                let addr = UnicastAddr::new(dns_request_v6::IPV6_PACKET.metadata.dst_ip).unwrap();
                crate::device::insert_ndp_table_entry(
                    &mut sync_ctx,
                    &mut non_sync_ctx,
                    &device,
                    addr,
                    config.remote_mac.get(),
                )
                .expect("insert static NDP entry");
                crate::ip::device::send_ip_frame::<Ipv6, _, _, _, _>(
                    &mut sync_ctx,
                    &mut non_sync_ctx,
                    &device,
                    addr.into_specified(),
                    Buf::new(dns_request_v6::IPV6_PACKET.bytes.to_vec(), ..),
                )
                .expect("error sending IPv6 frame")
            }
        }

        assert_eq!(get_counter_val(&non_sync_ctx, "ethernet::send_ip_frame"), expected_sent);
    }

    #[test]
    fn initialize_once() {
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device = crate::device::add_ethernet_device(
            &mut sync_ctx,
            FAKE_CONFIG_V4.local_mac,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
        )
        .into();
        crate::device::testutil::enable_device(&mut sync_ctx, &mut non_sync_ctx, &device);
    }

    fn is_routing_enabled<I: Ip>(
        sync_ctx: &mut &crate::testutil::FakeSyncCtx,
        device: &DeviceId<crate::testutil::FakeNonSyncCtx>,
    ) -> bool {
        match I::VERSION {
            IpVersion::V4 => is_ip_routing_enabled::<Ipv4, _, _>(sync_ctx, device),
            IpVersion::V6 => is_ip_routing_enabled::<Ipv6, _, _>(sync_ctx, device),
        }
    }

    #[ip_test]
    fn test_set_ip_routing<I: Ip + TestIpExt>() {
        fn check_other_is_routing_enabled<I: Ip>(
            sync_ctx: &mut &crate::testutil::FakeSyncCtx,
            device: &DeviceId<crate::testutil::FakeNonSyncCtx>,
            expected: bool,
        ) {
            let enabled = match I::VERSION {
                IpVersion::V4 => is_routing_enabled::<Ipv6>(sync_ctx, device),
                IpVersion::V6 => is_routing_enabled::<Ipv4>(sync_ctx, device),
            };

            assert_eq!(enabled, expected);
        }

        fn check_icmp<I: Ip>(buf: &[u8]) {
            match I::VERSION {
                IpVersion::V4 => {
                    let _ = parse_icmp_packet_in_ip_packet_in_ethernet_frame::<
                        Ipv4,
                        _,
                        IcmpDestUnreachable,
                        _,
                    >(buf, |_| {})
                    .unwrap();
                }
                IpVersion::V6 => {
                    let _ = parse_icmp_packet_in_ip_packet_in_ethernet_frame::<
                        Ipv6,
                        _,
                        IcmpDestUnreachable,
                        _,
                    >(buf, |_| {})
                    .unwrap();
                }
            }
        }

        let src_ip = I::get_other_ip_address(3);
        let src_mac = UnicastAddr::new(Mac::new([10, 11, 12, 13, 14, 15])).unwrap();
        let config = I::FAKE_CONFIG;
        let frame_dst = FrameDestination::Unicast;
        let mut rng = new_rng(70812476915813);
        let mut body: Vec<u8> = core::iter::repeat_with(|| rng.gen()).take(100).collect();
        let buf = Buf::new(&mut body[..], ..)
            .encapsulate(I::PacketBuilder::new(
                src_ip.get(),
                config.remote_ip.get(),
                64,
                IpProto::Tcp.into(),
            ))
            .serialize_vec_outer()
            .ok()
            .unwrap()
            .unwrap_b();

        // Test with netstack no forwarding

        let mut builder = FakeEventDispatcherBuilder::from_config(config.clone());
        let device_builder_id = 0;
        add_arp_or_ndp_table_entry(&mut builder, device_builder_id, src_ip.get(), src_mac);
        let (Ctx { sync_ctx, mut non_sync_ctx }, device_ids) = builder.build();
        let device: DeviceId<_> = device_ids[device_builder_id].clone().into();
        let mut sync_ctx = &sync_ctx;

        // Should not be a router (default).
        assert!(!is_routing_enabled::<I>(&mut sync_ctx, &device));
        check_other_is_routing_enabled::<I>(&mut sync_ctx, &device, false);

        // Receiving a packet not destined for the node should only result in a
        // dest unreachable message if routing is enabled.
        receive_ip_packet::<_, _, I>(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            frame_dst,
            buf.clone(),
        );
        assert_empty(non_sync_ctx.frames_sent().iter());

        // Set routing and expect packets to be forwarded.
        set_routing_enabled::<_, _, I>(&mut sync_ctx, &mut non_sync_ctx, &device, true)
            .expect("error setting routing enabled");
        assert!(is_routing_enabled::<I>(&mut sync_ctx, &device));
        // Should not update other Ip routing status.
        check_other_is_routing_enabled::<I>(&mut sync_ctx, &device, false);

        // Should route the packet since routing fully enabled (netstack &
        // device).
        receive_ip_packet::<_, _, I>(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            frame_dst,
            buf.clone(),
        );
        assert_eq!(non_sync_ctx.frames_sent().len(), 1);
        let (packet_buf, _, _, packet_src_ip, packet_dst_ip, proto, ttl) =
            parse_ip_packet_in_ethernet_frame::<I>(&non_sync_ctx.frames_sent()[0].1[..]).unwrap();
        assert_eq!(src_ip.get(), packet_src_ip);
        assert_eq!(config.remote_ip.get(), packet_dst_ip);
        assert_eq!(proto, IpProto::Tcp.into());
        assert_eq!(body, packet_buf);
        assert_eq!(ttl, 63);

        // Test routing a packet to an unknown address.
        let buf_unknown_dest = Buf::new(&mut body[..], ..)
            .encapsulate(I::PacketBuilder::new(
                src_ip.get(),
                // Addr must be remote, otherwise this will cause an NDP/ARP
                // request rather than ICMP unreachable.
                I::get_other_remote_ip_address(10).get(),
                64,
                IpProto::Tcp.into(),
            ))
            .serialize_vec_outer()
            .ok()
            .unwrap()
            .unwrap_b();
        receive_ip_packet::<_, _, I>(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            frame_dst,
            buf_unknown_dest,
        );
        assert_eq!(non_sync_ctx.frames_sent().len(), 2);
        check_icmp::<I>(&non_sync_ctx.frames_sent()[1].1);

        // Attempt to unset router
        set_routing_enabled::<_, _, I>(&mut sync_ctx, &mut non_sync_ctx, &device, false)
            .expect("error setting routing enabled");
        assert!(!is_routing_enabled::<I>(&mut sync_ctx, &device));
        check_other_is_routing_enabled::<I>(&mut sync_ctx, &device, false);

        // Should not route packets anymore
        receive_ip_packet::<_, _, I>(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            frame_dst,
            buf.clone(),
        );
        assert_eq!(non_sync_ctx.frames_sent().len(), 2);
    }

    #[ip_test]
    fn test_promiscuous_mode<I: Ip + TestIpExt + IpExt>() {
        // Test that frames not destined for a device will still be accepted
        // when the device is put into promiscuous mode. In all cases, frames
        // that are destined for a device must always be accepted.

        let config = I::FAKE_CONFIG;
        let (Ctx { sync_ctx, mut non_sync_ctx }, device_ids) =
            FakeEventDispatcherBuilder::from_config(config.clone()).build();
        let mut sync_ctx = &sync_ctx;
        let device = device_ids[0].clone().into();
        let other_mac = Mac::new([13, 14, 15, 16, 17, 18]);

        let buf = Buf::new(Vec::new(), ..)
            .encapsulate(I::PacketBuilder::new(
                config.remote_ip.get(),
                config.local_ip.get(),
                64,
                IpProto::Tcp.into(),
            ))
            .encapsulate(EthernetFrameBuilder::new(
                config.remote_mac.get(),
                config.local_mac.get(),
                I::ETHER_TYPE,
            ))
            .serialize_vec_outer()
            .ok()
            .unwrap()
            .unwrap_b();

        // Accept packet destined for this device if promiscuous mode is off.
        crate::device::set_promiscuous_mode(&mut sync_ctx, &mut non_sync_ctx, &device, false)
            .expect("error setting promiscuous mode");
        crate::device::receive_frame(&mut sync_ctx, &mut non_sync_ctx, &device, buf.clone())
            .expect("error receiving frame");
        assert_eq!(get_counter_val(&non_sync_ctx, dispatch_receive_ip_packet_name::<I>()), 1);

        // Accept packet destined for this device if promiscuous mode is on.
        crate::device::set_promiscuous_mode(&mut sync_ctx, &mut non_sync_ctx, &device, true)
            .expect("error setting promiscuous mode");
        crate::device::receive_frame(&mut sync_ctx, &mut non_sync_ctx, &device, buf.clone())
            .expect("error receiving frame");
        assert_eq!(get_counter_val(&non_sync_ctx, dispatch_receive_ip_packet_name::<I>()), 2);

        let buf = Buf::new(Vec::new(), ..)
            .encapsulate(I::PacketBuilder::new(
                config.remote_ip.get(),
                config.local_ip.get(),
                64,
                IpProto::Tcp.into(),
            ))
            .encapsulate(EthernetFrameBuilder::new(
                config.remote_mac.get(),
                other_mac,
                I::ETHER_TYPE,
            ))
            .serialize_vec_outer()
            .ok()
            .unwrap()
            .unwrap_b();

        // Reject packet not destined for this device if promiscuous mode is
        // off.
        crate::device::set_promiscuous_mode(&mut sync_ctx, &mut non_sync_ctx, &device, false)
            .expect("error setting promiscuous mode");
        crate::device::receive_frame(&mut sync_ctx, &mut non_sync_ctx, &device, buf.clone())
            .expect("error receiving frame");
        assert_eq!(get_counter_val(&non_sync_ctx, dispatch_receive_ip_packet_name::<I>()), 2);

        // Accept packet not destined for this device if promiscuous mode is on.
        crate::device::set_promiscuous_mode(&mut sync_ctx, &mut non_sync_ctx, &device, true)
            .expect("error setting promiscuous mode");
        crate::device::receive_frame(&mut sync_ctx, &mut non_sync_ctx, &device, buf.clone())
            .expect("error receiving frame");
        assert_eq!(get_counter_val(&non_sync_ctx, dispatch_receive_ip_packet_name::<I>()), 3);
    }

    #[ip_test]
    fn test_add_remove_ip_addresses<I: Ip + TestIpExt>() {
        let config = I::FAKE_CONFIG;
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device = crate::device::add_ethernet_device(
            &mut sync_ctx,
            config.local_mac,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
        )
        .into();
        crate::device::testutil::enable_device(&mut sync_ctx, &mut non_sync_ctx, &device);

        let ip1 = I::get_other_ip_address(1);
        let ip2 = I::get_other_ip_address(2);
        let ip3 = I::get_other_ip_address(3);

        let prefix = I::Addr::BYTES * 8;
        let as1 = AddrSubnet::new(ip1.get(), prefix).unwrap();
        let as2 = AddrSubnet::new(ip2.get(), prefix).unwrap();

        assert!(!contains_addr(&mut sync_ctx, &device, ip1));
        assert!(!contains_addr(&mut sync_ctx, &device, ip2));
        assert!(!contains_addr(&mut sync_ctx, &device, ip3));

        // Add ip1 (ok)
        crate::device::add_ip_addr_subnet(&mut sync_ctx, &mut non_sync_ctx, &device, as1).unwrap();
        assert!(contains_addr(&mut sync_ctx, &device, ip1));
        assert!(!contains_addr(&mut sync_ctx, &device, ip2));
        assert!(!contains_addr(&mut sync_ctx, &device, ip3));

        // Add ip2 (ok)
        crate::device::add_ip_addr_subnet(&mut sync_ctx, &mut non_sync_ctx, &device, as2).unwrap();
        assert!(contains_addr(&mut sync_ctx, &device, ip1));
        assert!(contains_addr(&mut sync_ctx, &device, ip2));
        assert!(!contains_addr(&mut sync_ctx, &device, ip3));

        // Del ip1 (ok)
        crate::device::del_ip_addr(&mut sync_ctx, &mut non_sync_ctx, &device, &ip1).unwrap();
        assert!(!contains_addr(&mut sync_ctx, &device, ip1));
        assert!(contains_addr(&mut sync_ctx, &device, ip2));
        assert!(!contains_addr(&mut sync_ctx, &device, ip3));

        // Del ip1 again (ip1 not found)
        assert_eq!(
            crate::device::del_ip_addr(&mut sync_ctx, &mut non_sync_ctx, &device, &ip1),
            Err(NotFoundError)
        );
        assert!(!contains_addr(&mut sync_ctx, &device, ip1));
        assert!(contains_addr(&mut sync_ctx, &device, ip2));
        assert!(!contains_addr(&mut sync_ctx, &device, ip3));

        // Add ip2 again (ip2 already exists)
        assert_eq!(
            crate::device::add_ip_addr_subnet(&mut sync_ctx, &mut non_sync_ctx, &device, as2)
                .unwrap_err(),
            ExistsError,
        );
        assert!(!contains_addr(&mut sync_ctx, &device, ip1));
        assert!(contains_addr(&mut sync_ctx, &device, ip2));
        assert!(!contains_addr(&mut sync_ctx, &device, ip3));

        // Add ip2 with different subnet (ip2 already exists)
        assert_eq!(
            crate::device::add_ip_addr_subnet(
                &mut sync_ctx,
                &mut non_sync_ctx,
                &device,
                AddrSubnet::new(ip2.get(), prefix - 1).unwrap()
            )
            .unwrap_err(),
            ExistsError,
        );
        assert!(!contains_addr(&mut sync_ctx, &device, ip1));
        assert!(contains_addr(&mut sync_ctx, &device, ip2));
        assert!(!contains_addr(&mut sync_ctx, &device, ip3));
    }

    fn receive_simple_ip_packet_test<A: IpAddress>(
        sync_ctx: &mut &crate::testutil::FakeSyncCtx,
        non_sync_ctx: &mut crate::testutil::FakeNonSyncCtx,
        device: &DeviceId<crate::testutil::FakeNonSyncCtx>,
        src_ip: A,
        dst_ip: A,
        expected: usize,
    ) where
        A::Version: TestIpExt,
    {
        let buf = Buf::new(Vec::new(), ..)
            .encapsulate(<<A::Version as IpExt>::PacketBuilder as IpPacketBuilder<_>>::new(
                src_ip,
                dst_ip,
                64,
                IpProto::Tcp.into(),
            ))
            .serialize_vec_outer()
            .ok()
            .unwrap()
            .into_inner();

        receive_ip_packet::<_, _, A::Version>(
            sync_ctx,
            non_sync_ctx,
            device,
            FrameDestination::Unicast,
            buf,
        );
        assert_eq!(
            get_counter_val(non_sync_ctx, dispatch_receive_ip_packet_name::<A::Version>()),
            expected
        );
    }

    #[ip_test]
    fn test_multiple_ip_addresses<I: Ip + TestIpExt>() {
        let config = I::FAKE_CONFIG;
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device = crate::device::add_ethernet_device(
            &mut sync_ctx,
            config.local_mac,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
        )
        .into();
        crate::device::testutil::enable_device(&mut sync_ctx, &mut non_sync_ctx, &device);

        let ip1 = I::get_other_ip_address(1);
        let ip2 = I::get_other_ip_address(2);
        let from_ip = I::get_other_ip_address(3).get();

        assert!(!contains_addr(&mut sync_ctx, &device, ip1));
        assert!(!contains_addr(&mut sync_ctx, &device, ip2));

        // Should not receive packets on any IP.
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip1.get(),
            0,
        );
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip2.get(),
            0,
        );

        // Add ip1 to device.
        crate::device::add_ip_addr_subnet(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            AddrSubnet::new(ip1.get(), I::Addr::BYTES * 8).unwrap(),
        )
        .unwrap();
        assert!(contains_addr(&mut sync_ctx, &device, ip1));
        assert!(!contains_addr(&mut sync_ctx, &device, ip2));

        // Should receive packets on ip1 but not ip2
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip1.get(),
            1,
        );
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip2.get(),
            1,
        );

        // Add ip2 to device.
        crate::device::add_ip_addr_subnet(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            AddrSubnet::new(ip2.get(), I::Addr::BYTES * 8).unwrap(),
        )
        .unwrap();
        assert!(contains_addr(&mut sync_ctx, &device, ip1));
        assert!(contains_addr(&mut sync_ctx, &device, ip2));

        // Should receive packets on both ips
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip1.get(),
            2,
        );
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip2.get(),
            3,
        );

        // Remove ip1
        crate::device::del_ip_addr(&mut sync_ctx, &mut non_sync_ctx, &device, &ip1).unwrap();
        assert!(!contains_addr(&mut sync_ctx, &device, ip1));
        assert!(contains_addr(&mut sync_ctx, &device, ip2));

        // Should receive packets on ip2
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip1.get(),
            3,
        );
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip2.get(),
            4,
        );
    }

    fn join_ip_multicast<A: IpAddress, NonSyncCtx: NonSyncContext>(
        mut sync_ctx: &SyncCtx<NonSyncCtx>,
        ctx: &mut NonSyncCtx,
        device: &DeviceId<NonSyncCtx>,
        multicast_addr: MulticastAddr<A>,
    ) {
        match multicast_addr.into() {
            IpAddr::V4(multicast_addr) => crate::ip::device::join_ip_multicast::<Ipv4, _, _>(
                &mut sync_ctx,
                ctx,
                device,
                multicast_addr,
            ),
            IpAddr::V6(multicast_addr) => crate::ip::device::join_ip_multicast::<Ipv6, _, _>(
                &mut sync_ctx,
                ctx,
                device,
                multicast_addr,
            ),
        }
    }

    fn leave_ip_multicast<A: IpAddress, NonSyncCtx: NonSyncContext>(
        mut sync_ctx: &SyncCtx<NonSyncCtx>,
        ctx: &mut NonSyncCtx,
        device: &DeviceId<NonSyncCtx>,
        multicast_addr: MulticastAddr<A>,
    ) {
        match multicast_addr.into() {
            IpAddr::V4(multicast_addr) => crate::ip::device::leave_ip_multicast::<Ipv4, _, _>(
                &mut sync_ctx,
                ctx,
                device,
                multicast_addr,
            ),
            IpAddr::V6(multicast_addr) => crate::ip::device::leave_ip_multicast::<Ipv6, _, _>(
                &mut sync_ctx,
                ctx,
                device,
                multicast_addr,
            ),
        }
    }

    /// Test that we can join and leave a multicast group, but we only truly
    /// leave it after calling `leave_ip_multicast` the same number of times as
    /// `join_ip_multicast`.
    #[ip_test]
    fn test_ip_join_leave_multicast_addr_ref_count<I: Ip + TestIpExt>() {
        let config = I::FAKE_CONFIG;
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device = crate::device::add_ethernet_device(
            &mut sync_ctx,
            config.local_mac,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
        )
        .into();
        crate::device::testutil::enable_device(&mut sync_ctx, &mut non_sync_ctx, &device);

        let multicast_addr = I::get_multicast_addr(3);

        // Should not be in the multicast group yet.
        assert!(!is_in_ip_multicast(&sync_ctx, &device, multicast_addr));

        // Join the multicast group.
        join_ip_multicast(&mut sync_ctx, &mut non_sync_ctx, &device, multicast_addr);
        assert!(is_in_ip_multicast(&sync_ctx, &device, multicast_addr));

        // Leave the multicast group.
        leave_ip_multicast(&mut sync_ctx, &mut non_sync_ctx, &device, multicast_addr);
        assert!(!is_in_ip_multicast(&sync_ctx, &device, multicast_addr));

        // Join the multicst group.
        join_ip_multicast(&mut sync_ctx, &mut non_sync_ctx, &device, multicast_addr);
        assert!(is_in_ip_multicast(&sync_ctx, &device, multicast_addr));

        // Join it again...
        join_ip_multicast(&mut sync_ctx, &mut non_sync_ctx, &device, multicast_addr);
        assert!(is_in_ip_multicast(&sync_ctx, &device, multicast_addr));

        // Leave it (still in it because we joined twice).
        leave_ip_multicast(&mut sync_ctx, &mut non_sync_ctx, &device, multicast_addr);
        assert!(is_in_ip_multicast(&sync_ctx, &device, multicast_addr));

        // Leave it again... (actually left now).
        leave_ip_multicast(&mut sync_ctx, &mut non_sync_ctx, &device, multicast_addr);
        assert!(!is_in_ip_multicast(&sync_ctx, &device, multicast_addr));
    }

    /// Test leaving a multicast group a device has not yet joined.
    ///
    /// # Panics
    ///
    /// This method should always panic as leaving an unjoined multicast group
    /// is a panic condition.
    #[ip_test]
    #[should_panic(expected = "attempted to leave IP multicast group we were not a member of:")]
    fn test_ip_leave_unjoined_multicast<I: Ip + TestIpExt>() {
        let config = I::FAKE_CONFIG;
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device = crate::device::add_ethernet_device(
            &mut sync_ctx,
            config.local_mac,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
        )
        .into();
        crate::device::testutil::enable_device(&mut sync_ctx, &mut non_sync_ctx, &device);

        let multicast_addr = I::get_multicast_addr(3);

        // Should not be in the multicast group yet.
        assert!(!is_in_ip_multicast(&sync_ctx, &device, multicast_addr));

        // Leave it (this should panic).
        leave_ip_multicast(&mut sync_ctx, &mut non_sync_ctx, &device, multicast_addr);
    }

    #[test]
    fn test_ipv6_duplicate_solicited_node_address() {
        // Test that we still receive packets destined to a solicited-node
        // multicast address of an IP address we deleted because another
        // (distinct) IP address that is still assigned uses the same
        // solicited-node multicast address.

        let config = Ipv6::FAKE_CONFIG;
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device = crate::device::add_ethernet_device(
            &mut sync_ctx,
            config.local_mac,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
        )
        .into();
        crate::device::testutil::enable_device(&mut sync_ctx, &mut non_sync_ctx, &device);

        let ip1 = SpecifiedAddr::new(Ipv6Addr::new([0, 0, 0, 1, 0, 0, 0, 1])).unwrap();
        let ip2 = SpecifiedAddr::new(Ipv6Addr::new([0, 0, 0, 2, 0, 0, 0, 1])).unwrap();
        let from_ip = Ipv6Addr::new([0, 0, 0, 3, 0, 0, 0, 1]);

        // ip1 and ip2 are not equal but their solicited node addresses are the
        // same.
        assert_ne!(ip1, ip2);
        assert_eq!(ip1.to_solicited_node_address(), ip2.to_solicited_node_address());
        let sn_addr = ip1.to_solicited_node_address().get();

        let addr_sub1 = AddrSubnet::new(ip1.get(), 64).unwrap();
        let addr_sub2 = AddrSubnet::new(ip2.get(), 64).unwrap();

        assert_eq!(get_counter_val(&non_sync_ctx, "dispatch_receive_ip_packet"), 0);

        // Add ip1 to the device.
        //
        // Should get packets destined for the solicited node address and ip1.
        crate::device::add_ip_addr_subnet(&mut sync_ctx, &mut non_sync_ctx, &device, addr_sub1)
            .unwrap();
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip1.get(),
            1,
        );
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip2.get(),
            1,
        );
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            sn_addr,
            2,
        );

        // Add ip2 to the device.
        //
        // Should get packets destined for the solicited node address, ip1 and
        // ip2.
        crate::device::add_ip_addr_subnet(&mut sync_ctx, &mut non_sync_ctx, &device, addr_sub2)
            .unwrap();
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip1.get(),
            3,
        );
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip2.get(),
            4,
        );
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            sn_addr,
            5,
        );

        // Remove ip1 from the device.
        //
        // Should get packets destined for the solicited node address and ip2.
        crate::device::del_ip_addr(&mut sync_ctx, &mut non_sync_ctx, &device, &ip1).unwrap();
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip1.get(),
            5,
        );
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            ip2.get(),
            6,
        );
        receive_simple_ip_packet_test(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            from_ip,
            sn_addr,
            7,
        );
    }

    #[test]
    fn test_add_ip_addr_subnet_link_local() {
        // Test that `add_ip_addr_subnet` allows link-local addresses.

        let config = Ipv6::FAKE_CONFIG;
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device = crate::device::add_ethernet_device(
            &mut sync_ctx,
            config.local_mac,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
        )
        .into();

        crate::device::testutil::enable_device(&mut sync_ctx, &mut non_sync_ctx, &device);
        // Verify that there is a single assigned address.
        assert_eq!(
            sync_ctx
                .state
                .device
                .devices
                .read()
                .ethernet
                .get(0)
                .unwrap()
                .ip
                .ipv6
                .read()
                .ip_state
                .iter_addrs()
                .map(|entry| entry.addr_sub().addr())
                .collect::<Vec<_>>(),
            [config.local_mac.to_ipv6_link_local().addr().get()]
        );
        crate::device::add_ip_addr_subnet(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device,
            AddrSubnet::new(Ipv6::LINK_LOCAL_UNICAST_SUBNET.network(), 128).unwrap(),
        )
        .unwrap();
        // Assert that the new address got added.
        let addr_subs: Vec<_> = sync_ctx
            .state
            .device
            .devices
            .read()
            .ethernet
            .get(0)
            .unwrap()
            .ip
            .ipv6
            .read()
            .ip_state
            .iter_addrs()
            .map(|entry| entry.addr_sub().addr().get())
            .collect();
        assert_eq!(
            addr_subs,
            [
                config.local_mac.to_ipv6_link_local().addr().get(),
                Ipv6::LINK_LOCAL_UNICAST_SUBNET.network()
            ]
        );
    }
}
