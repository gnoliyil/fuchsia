// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The loopback device.

use alloc::vec::Vec;
use core::fmt::{self, Debug, Display, Formatter};

use derivative::Derivative;
use lock_order::{
    lock::{LockFor, RwLockFor},
    relation::LockBefore,
    Locked,
};
use log::trace;
use net_types::{ethernet::Mac, ip::IpAddress, SpecifiedAddr};
use packet::{Buf, Buffer as _, BufferMut, Serializer};
use packet_formats::ethernet::{
    EtherType, EthernetFrame, EthernetFrameBuilder, EthernetFrameLengthCheck, EthernetIpExt,
};

use crate::{
    device::{
        queue::{
            rx::{
                BufferReceiveQueueHandler, ReceiveDequeContext, ReceiveDequeFrameContext,
                ReceiveQueue, ReceiveQueueContext, ReceiveQueueNonSyncContext, ReceiveQueueState,
                ReceiveQueueTypes,
            },
            tx::{
                BufVecU8Allocator, BufferTransmitQueueHandler, TransmitDequeueContext,
                TransmitQueue, TransmitQueueContext, TransmitQueueNonSyncContext,
                TransmitQueueState, TransmitQueueTypes,
            },
            DequeueState, ReceiveQueueFullError, TransmitQueueFrameError,
        },
        socket::DeviceSockets,
        state::IpLinkDeviceState,
        with_loopback_state, with_loopback_state_and_sync_ctx, Device, DeviceIdContext,
        DeviceLayerEventDispatcher, DeviceSendFrameError, FrameDestination,
    },
    device::{Id, Mtu, StrongId, WeakId},
    ip::types::RawMetric,
    sync::{RwLock, StrongRc, WeakRc},
    Instant, NonSyncContext, SyncCtx,
};

/// A weak device ID identifying a loopback device.
///
/// This device ID is like [`WeakDeviceId`] but specifically for loopback
/// devices.
///
/// [`WeakDeviceId`]: crate::device::WeakDeviceId
#[derive(Derivative)]
#[derivative(Clone(bound = ""), Hash(bound = ""))]
pub struct LoopbackWeakDeviceId<I: Instant, S>(
    pub(super) WeakRc<IpLinkDeviceState<I, S, LoopbackDeviceState>>,
);

impl<I: Instant, S> PartialEq for LoopbackWeakDeviceId<I, S> {
    fn eq(&self, LoopbackWeakDeviceId(other): &LoopbackWeakDeviceId<I, S>) -> bool {
        let LoopbackWeakDeviceId(me) = self;
        WeakRc::ptr_eq(me, other)
    }
}

impl<I: Instant, S> PartialEq<LoopbackDeviceId<I, S>> for LoopbackWeakDeviceId<I, S> {
    fn eq(&self, other: &LoopbackDeviceId<I, S>) -> bool {
        <LoopbackDeviceId<I, S> as PartialEq<LoopbackWeakDeviceId<I, S>>>::eq(other, self)
    }
}

impl<I: Instant, S> Eq for LoopbackWeakDeviceId<I, S> {}

impl<I: Instant, S> Debug for LoopbackWeakDeviceId<I, S> {
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        Display::fmt(self, f)
    }
}

impl<I: Instant, S> Display for LoopbackWeakDeviceId<I, S> {
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        write!(f, "Weak Loopback")
    }
}

impl<I: Instant, S: Send + Sync> WeakId for LoopbackWeakDeviceId<I, S> {
    type Strong = LoopbackDeviceId<I, S>;
}

impl<I: Instant, S: Send + Sync> Id for LoopbackWeakDeviceId<I, S> {
    fn is_loopback(&self) -> bool {
        true
    }
}

impl<I: Instant, S> LoopbackWeakDeviceId<I, S> {
    /// Attempts to upgrade the ID to a [`LoopbackDeviceId`], failing if the
    /// device no longer exists.
    pub fn upgrade(&self) -> Option<LoopbackDeviceId<I, S>> {
        let Self(rc) = self;
        rc.upgrade().map(LoopbackDeviceId)
    }
}

/// A strong device ID identifying a loopback device.
///
/// This device ID is like [`DeviceId`] but specifically for loopback devices.
///
/// [`DeviceId`]: crate::device::DeviceId
#[derive(Derivative)]
#[derivative(Clone(bound = ""), Hash(bound = ""))]
pub struct LoopbackDeviceId<I: Instant, S>(
    pub(super) StrongRc<IpLinkDeviceState<I, S, LoopbackDeviceState>>,
);

impl<I: Instant, S> PartialEq for LoopbackDeviceId<I, S> {
    fn eq(&self, LoopbackDeviceId(other): &LoopbackDeviceId<I, S>) -> bool {
        let LoopbackDeviceId(me) = self;
        StrongRc::ptr_eq(me, other)
    }
}

impl<I: Instant, S> PartialEq<LoopbackWeakDeviceId<I, S>> for LoopbackDeviceId<I, S> {
    fn eq(&self, LoopbackWeakDeviceId(other): &LoopbackWeakDeviceId<I, S>) -> bool {
        let LoopbackDeviceId(me) = self;
        StrongRc::weak_ptr_eq(me, other)
    }
}

impl<I: Instant, S> Eq for LoopbackDeviceId<I, S> {}

impl<I: Instant, S> Debug for LoopbackDeviceId<I, S> {
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        Display::fmt(self, f)
    }
}

impl<I: Instant, S> Display for LoopbackDeviceId<I, S> {
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        write!(f, "Loopback")
    }
}

impl<I: Instant, S: Send + Sync> StrongId for LoopbackDeviceId<I, S> {
    type Weak = LoopbackWeakDeviceId<I, S>;
}

impl<I: Instant, S: Send + Sync> Id for LoopbackDeviceId<I, S> {
    fn is_loopback(&self) -> bool {
        true
    }
}

impl<I: Instant, S> LoopbackDeviceId<I, S> {
    /// Returns a reference to the external state for the device.
    pub fn external_state(&self) -> &S {
        let Self(rc) = self;
        &rc.external_state
    }

    /// Returns a weak loopback device ID.
    pub fn downgrade(&self) -> LoopbackWeakDeviceId<I, S> {
        let Self(rc) = self;
        LoopbackWeakDeviceId(StrongRc::downgrade(rc))
    }

    pub(super) fn removed(&self) -> bool {
        let Self(rc) = self;
        StrongRc::marked_for_destruction(rc)
    }
}

#[derive(Copy, Clone)]
pub(super) enum LoopbackDevice {}

impl Device for LoopbackDevice {}

impl<NonSyncCtx: NonSyncContext, L> DeviceIdContext<LoopbackDevice>
    for Locked<&SyncCtx<NonSyncCtx>, L>
{
    type DeviceId = LoopbackDeviceId<NonSyncCtx::Instant, NonSyncCtx::LoopbackDeviceState>;
    type WeakDeviceId = LoopbackWeakDeviceId<NonSyncCtx::Instant, NonSyncCtx::LoopbackDeviceState>;
    fn downgrade_device_id(&self, device_id: &Self::DeviceId) -> Self::WeakDeviceId {
        device_id.downgrade()
    }
    fn is_device_installed(&self, device_id: &Self::DeviceId) -> bool {
        !device_id.removed()
    }
    fn upgrade_weak_device_id(
        &self,
        weak_device_id: &Self::WeakDeviceId,
    ) -> Option<Self::DeviceId> {
        weak_device_id.upgrade()
    }
}

pub(super) struct LoopbackDeviceState {
    mtu: Mtu,
    /// The routing metric of the loopback device this state is for.
    metric: RawMetric,
    rx_queue: ReceiveQueue<(), Buf<Vec<u8>>>,
    tx_queue: TransmitQueue<(), Buf<Vec<u8>>, BufVecU8Allocator>,
    sockets: RwLock<DeviceSockets>,
}

impl LoopbackDeviceState {
    pub(super) fn new(mtu: Mtu, metric: RawMetric) -> LoopbackDeviceState {
        LoopbackDeviceState {
            mtu,
            metric,
            rx_queue: Default::default(),
            tx_queue: Default::default(),
            sockets: Default::default(),
        }
    }
}

impl<I: Instant, S> LockFor<crate::lock_ordering::LoopbackRxQueue>
    for IpLinkDeviceState<I, S, LoopbackDeviceState>
{
    type Data<'l> = crate::sync::LockGuard<'l, ReceiveQueueState<(), Buf<Vec<u8>>>>
        where
            Self: 'l;
    fn lock(&self) -> Self::Data<'_> {
        self.link.rx_queue.queue.lock()
    }
}

impl<I: Instant, S> LockFor<crate::lock_ordering::LoopbackRxDequeue>
    for IpLinkDeviceState<I, S, LoopbackDeviceState>
{
    type Data<'l> = crate::sync::LockGuard<'l, DequeueState<(), Buf<Vec<u8>>>>
        where
            Self: 'l;
    fn lock(&self) -> Self::Data<'_> {
        self.link.rx_queue.deque.lock()
    }
}

impl<I: Instant, S> LockFor<crate::lock_ordering::LoopbackTxQueue>
    for IpLinkDeviceState<I, S, LoopbackDeviceState>
{
    type Data<'l> = crate::sync::LockGuard<'l, TransmitQueueState<(), Buf<Vec<u8>>, BufVecU8Allocator>>
        where
            Self: 'l;
    fn lock(&self) -> Self::Data<'_> {
        self.link.tx_queue.queue.lock()
    }
}

impl<I: Instant, S> LockFor<crate::lock_ordering::LoopbackTxDequeue>
    for IpLinkDeviceState<I, S, LoopbackDeviceState>
{
    type Data<'l> = crate::sync::LockGuard<'l, DequeueState<(), Buf<Vec<u8>>>>
        where
            Self: 'l;
    fn lock(&self) -> Self::Data<'_> {
        self.link.tx_queue.deque.lock()
    }
}

impl<I: Instant, S> RwLockFor<crate::lock_ordering::DeviceSockets>
    for IpLinkDeviceState<I, S, LoopbackDeviceState>
{
    type ReadData<'l> = crate::sync::RwLockReadGuard<'l, DeviceSockets>
        where
            Self: 'l ;
    type WriteData<'l> = crate::sync::RwLockWriteGuard<'l, DeviceSockets>
        where
            Self: 'l ;
    fn read_lock(&self) -> Self::ReadData<'_> {
        self.link.sockets.read()
    }
    fn write_lock(&self) -> Self::WriteData<'_> {
        self.link.sockets.write()
    }
}

pub(super) fn send_ip_frame<
    B: BufferMut,
    NonSyncCtx: NonSyncContext,
    A: IpAddress,
    S: Serializer<Buffer = B>,
    L: LockBefore<crate::lock_ordering::LoopbackTxQueue>,
>(
    sync_ctx: &mut Locked<&SyncCtx<NonSyncCtx>, L>,
    ctx: &mut NonSyncCtx,
    device_id: &LoopbackDeviceId<NonSyncCtx::Instant, NonSyncCtx::LoopbackDeviceState>,
    _local_addr: SpecifiedAddr<A>,
    packet: S,
) -> Result<(), S>
where
    A::Version: EthernetIpExt,
{
    const LOOPBACK_MAC: Mac = Mac::UNSPECIFIED;

    let frame = packet.encapsulate(EthernetFrameBuilder::new(
        LOOPBACK_MAC,
        LOOPBACK_MAC,
        <A::Version as EthernetIpExt>::ETHER_TYPE,
    ));

    match BufferTransmitQueueHandler::<LoopbackDevice, _, _>::queue_tx_frame(
        sync_ctx,
        ctx,
        device_id,
        (),
        frame,
    ) {
        Ok(()) => Ok(()),
        Err(TransmitQueueFrameError::NoQueue(_)) => {
            unreachable!("loopback never fails to send a frame")
        }
        Err(TransmitQueueFrameError::QueueFull(s) | TransmitQueueFrameError::SerializeError(s)) => {
            Err(s.into_inner())
        }
    }
}

/// Get the routing metric associated with this device.
pub(super) fn get_routing_metric<NonSyncCtx: NonSyncContext, L>(
    ctx: &mut Locked<&SyncCtx<NonSyncCtx>, L>,
    device_id: &LoopbackDeviceId<NonSyncCtx::Instant, NonSyncCtx::LoopbackDeviceState>,
) -> RawMetric {
    with_loopback_state(ctx, device_id, |mut state| state.cast_with(|s| &s.link.metric).copied())
}

/// Gets the MTU associated with this device.
pub(super) fn get_mtu<NonSyncCtx: NonSyncContext, L>(
    ctx: &mut Locked<&SyncCtx<NonSyncCtx>, L>,
    device_id: &LoopbackDeviceId<NonSyncCtx::Instant, NonSyncCtx::LoopbackDeviceState>,
) -> Mtu {
    with_loopback_state(ctx, device_id, |mut state| state.cast_with(|s| &s.link.mtu).copied())
}

impl<C: NonSyncContext>
    ReceiveQueueNonSyncContext<LoopbackDevice, LoopbackDeviceId<C::Instant, C::LoopbackDeviceState>>
    for C
{
    fn wake_rx_task(&mut self, device_id: &LoopbackDeviceId<C::Instant, C::LoopbackDeviceState>) {
        DeviceLayerEventDispatcher::wake_rx_task(self, device_id)
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::LoopbackRxQueue>>
    ReceiveQueueTypes<LoopbackDevice, C> for Locked<&SyncCtx<C>, L>
{
    type Meta = ();
    type Buffer = Buf<Vec<u8>>;
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::LoopbackRxQueue>>
    ReceiveQueueContext<LoopbackDevice, C> for Locked<&SyncCtx<C>, L>
{
    fn with_receive_queue_mut<
        O,
        F: FnOnce(&mut ReceiveQueueState<Self::Meta, Self::Buffer>) -> O,
    >(
        &mut self,
        device_id: &LoopbackDeviceId<C::Instant, C::LoopbackDeviceState>,
        cb: F,
    ) -> O {
        with_loopback_state(self, device_id, |mut state| {
            let mut x = state.lock::<crate::lock_ordering::LoopbackRxQueue>();
            cb(&mut x)
        })
    }
}

impl<C: NonSyncContext> ReceiveDequeFrameContext<LoopbackDevice, C>
    for Locked<&SyncCtx<C>, crate::lock_ordering::LoopbackRxDequeue>
{
    fn handle_frame(
        &mut self,
        ctx: &mut C,
        device_id: &LoopbackDeviceId<C::Instant, C::LoopbackDeviceState>,
        (): Self::Meta,
        mut buf: Buf<Vec<u8>>,
    ) {
        let (frame, whole_body) =
            match buf.parse_with_view::<_, EthernetFrame<_>>(EthernetFrameLengthCheck::NoCheck) {
                Err(e) => {
                    trace!("dropping invalid ethernet frame over loopback: {:?}", e);
                    return;
                }
                Ok(e) => e,
            };

        let frame_dest = FrameDestination::from_dest(frame.dst_mac(), Mac::UNSPECIFIED);

        crate::device::socket::BufferSocketHandler::<LoopbackDevice, _>::handle_received_frame(
            self,
            ctx,
            device_id,
            crate::device::socket::Frame::from_ethernet(&frame, frame_dest),
            whole_body,
        );

        let ethertype = match frame.ethertype() {
            Some(e) => e,
            None => {
                trace!("dropping ethernet frame without ethertype");
                return;
            }
        };

        match ethertype {
            EtherType::Ipv4 => crate::ip::receive_ipv4_packet(
                self,
                ctx,
                &device_id.clone().into(),
                frame_dest,
                buf,
            ),
            EtherType::Ipv6 => crate::ip::receive_ipv6_packet(
                self,
                ctx,
                &device_id.clone().into(),
                frame_dest,
                buf,
            ),
            ethertype @ EtherType::Arp | ethertype @ EtherType::Other(_) => {
                trace!("not handling loopback frame of type {:?}", ethertype)
            }
        }
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::LoopbackRxDequeue>>
    ReceiveDequeContext<LoopbackDevice, C> for Locked<&SyncCtx<C>, L>
{
    type ReceiveQueueCtx<'a> = Locked<&'a SyncCtx<C>, crate::lock_ordering::LoopbackRxDequeue>;

    fn with_dequed_frames_and_rx_queue_ctx<
        O,
        F: FnOnce(&mut DequeueState<(), Buf<Vec<u8>>>, &mut Self::ReceiveQueueCtx<'_>) -> O,
    >(
        &mut self,
        device_id: &LoopbackDeviceId<C::Instant, C::LoopbackDeviceState>,
        cb: F,
    ) -> O {
        with_loopback_state_and_sync_ctx(self, device_id, |mut state, sync_ctx| {
            let mut x = state.lock::<crate::lock_ordering::LoopbackRxDequeue>();
            let mut locked = sync_ctx.cast_locked();
            cb(&mut x, &mut locked)
        })
    }
}

impl<C: NonSyncContext>
    TransmitQueueNonSyncContext<
        LoopbackDevice,
        LoopbackDeviceId<C::Instant, C::LoopbackDeviceState>,
    > for C
{
    fn wake_tx_task(&mut self, device_id: &LoopbackDeviceId<C::Instant, C::LoopbackDeviceState>) {
        DeviceLayerEventDispatcher::wake_tx_task(self, &device_id.clone().into())
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::LoopbackTxQueue>>
    TransmitQueueTypes<LoopbackDevice, C> for Locked<&SyncCtx<C>, L>
{
    type Meta = ();
    type Allocator = BufVecU8Allocator;
    type Buffer = Buf<Vec<u8>>;
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::LoopbackTxQueue>>
    TransmitQueueContext<LoopbackDevice, C> for Locked<&SyncCtx<C>, L>
{
    fn with_transmit_queue_mut<
        O,
        F: FnOnce(&mut TransmitQueueState<Self::Meta, Self::Buffer, Self::Allocator>) -> O,
    >(
        &mut self,
        device_id: &LoopbackDeviceId<C::Instant, C::LoopbackDeviceState>,
        cb: F,
    ) -> O {
        with_loopback_state(self, device_id, |mut state| {
            let mut x = state.lock::<crate::lock_ordering::LoopbackTxQueue>();
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
        // Never handle frames synchronously with the send path - always queue
        // the frame to be received by the loopback device into a queue which
        // a dedicated RX task will kick to handle the queued packet.
        //
        // This is done so that a socket lock may be held while sending a packet
        // which may need to be delivered to the sending socket itself. Without
        // this decoupling of RX/TX paths, sending a packet while holding onto
        // the socket lock will result in a deadlock.
        match BufferReceiveQueueHandler::queue_rx_frame(self, ctx, device_id, meta, buf) {
            Ok(()) => {}
            Err(ReceiveQueueFullError(((), _frame))) => {
                // RX queue is full - there is nothing further we can do here.
                log::error!("dropped RX frame on loopback device due to full RX queue")
            }
        }

        Ok(())
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::LoopbackTxDequeue>>
    TransmitDequeueContext<LoopbackDevice, C> for Locked<&SyncCtx<C>, L>
{
    type TransmitQueueCtx<'a> = Locked<&'a SyncCtx<C>, crate::lock_ordering::LoopbackTxDequeue>;

    fn with_dequed_packets_and_tx_queue_ctx<
        O,
        F: FnOnce(&mut DequeueState<Self::Meta, Self::Buffer>, &mut Self::TransmitQueueCtx<'_>) -> O,
    >(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O {
        with_loopback_state_and_sync_ctx(self, device_id, |mut state, sync_ctx| {
            let mut x = state.lock::<crate::lock_ordering::LoopbackTxDequeue>();
            let mut locked = sync_ctx.cast_locked();
            cb(&mut x, &mut locked)
        })
    }
}

#[cfg(test)]
mod tests {
    use alloc::vec::Vec;

    use assert_matches::assert_matches;
    use ip_test_macro::ip_test;
    use lock_order::{Locked, Unlocked};
    use net_types::ip::{AddrSubnet, Ip, Ipv4, Ipv6};
    use packet::ParseBuffer;

    use crate::{
        device::{DeviceId, Mtu},
        error::NotFoundError,
        ip::device::{state::AssignedAddress, IpDeviceIpExt, IpDeviceStateContext},
        testutil::{
            FakeEventDispatcherConfig, FakeNonSyncCtx, TestIpExt, DEFAULT_INTERFACE_METRIC,
        },
        Ctx, SyncCtx,
    };

    use super::*;

    const MTU: Mtu = Mtu::new(66);

    #[test]
    fn loopback_mtu() {
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device =
            crate::device::add_loopback_device(&mut sync_ctx, MTU, DEFAULT_INTERFACE_METRIC)
                .expect("error adding loopback device")
                .into();
        crate::device::testutil::enable_device(&mut sync_ctx, &mut non_sync_ctx, &device);

        assert_eq!(
            crate::ip::IpDeviceContext::<Ipv4, _>::get_mtu(&mut Locked::new(sync_ctx), &device),
            MTU
        );
        assert_eq!(
            crate::ip::IpDeviceContext::<Ipv6, _>::get_mtu(&mut Locked::new(sync_ctx), &device),
            MTU
        );
    }

    #[ip_test]
    fn test_loopback_add_remove_addrs<I: Ip + TestIpExt + IpDeviceIpExt>()
    where
        for<'a> Locked<&'a SyncCtx<FakeNonSyncCtx>, Unlocked>:
            IpDeviceStateContext<I, FakeNonSyncCtx, DeviceId = DeviceId<FakeNonSyncCtx>>,
    {
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device =
            crate::device::add_loopback_device(&mut sync_ctx, MTU, DEFAULT_INTERFACE_METRIC)
                .expect("error adding loopback device")
                .into();
        crate::device::testutil::enable_device(&mut sync_ctx, &mut non_sync_ctx, &device);

        let get_addrs = || {
            crate::ip::device::IpDeviceStateContext::<I, _>::with_ip_device_addresses(
                &mut Locked::new(sync_ctx),
                &device,
                |addrs| addrs.iter().map(AssignedAddress::addr).collect::<Vec<_>>(),
            )
        };

        let FakeEventDispatcherConfig {
            subnet,
            local_ip,
            local_mac: _,
            remote_ip: _,
            remote_mac: _,
        } = I::FAKE_CONFIG;
        let addr =
            AddrSubnet::from_witness(local_ip, subnet.prefix()).expect("error creating AddrSubnet");

        assert_eq!(get_addrs(), []);

        assert_eq!(
            crate::device::add_ip_addr_subnet(sync_ctx, &mut non_sync_ctx, &device, addr),
            Ok(())
        );
        let addr = addr.addr();
        assert_eq!(&get_addrs()[..], [addr]);

        assert_eq!(crate::device::del_ip_addr(sync_ctx, &mut non_sync_ctx, &device, &addr), Ok(()));
        assert_eq!(get_addrs(), []);

        assert_eq!(
            crate::device::del_ip_addr(sync_ctx, &mut non_sync_ctx, &device, &addr),
            Err(NotFoundError)
        );
    }

    #[ip_test]
    fn loopback_sends_ethernet<I: Ip + TestIpExt>() {
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device =
            crate::device::add_loopback_device(&mut sync_ctx, MTU, DEFAULT_INTERFACE_METRIC)
                .expect("error adding loopback device");
        crate::device::testutil::enable_device(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &device.clone().into(),
        );

        let local_addr = I::FAKE_CONFIG.local_ip;
        const BODY: &[u8] = b"IP body".as_slice();

        let body = Buf::new(Vec::from(BODY), ..);
        send_ip_frame(&mut Locked::new(sync_ctx), &mut non_sync_ctx, &device, local_addr, body)
            .expect("can send");

        // There is no transmit queue so the frames will immediately go into the
        // receive queue.
        let mut frames = ReceiveQueueContext::<LoopbackDevice, _>::with_receive_queue_mut(
            &mut Locked::new(sync_ctx),
            &device,
            |queue_state| queue_state.take_frames().map(|((), frame)| frame).collect::<Vec<_>>(),
        );

        let frame = assert_matches!(frames.as_mut_slice(), [frame] => frame);

        let eth = frame
            .parse_with::<_, EthernetFrame<_>>(EthernetFrameLengthCheck::NoCheck)
            .expect("is ethernet");
        assert_eq!(eth.src_mac(), Mac::UNSPECIFIED);
        assert_eq!(eth.dst_mac(), Mac::UNSPECIFIED);
        assert_eq!(eth.ethertype(), Some(I::ETHER_TYPE));

        // Trim the body to account for ethernet padding.
        assert_eq!(&frame.as_ref()[..BODY.len()], BODY);
    }
}
