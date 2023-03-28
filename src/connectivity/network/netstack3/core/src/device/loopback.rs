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
use net_types::{
    ip::{Ip as _, IpAddress, IpVersion, Ipv4, Ipv6},
    SpecifiedAddr,
};
use packet::{Buf, BufferMut, Serializer};

use crate::{
    device::Mtu,
    device::{
        queue::{
            rx::{
                BufferReceiveQueueHandler, ReceiveDequeContext, ReceiveQueue, ReceiveQueueContext,
                ReceiveQueueNonSyncContext, ReceiveQueueState, ReceiveQueueTypes,
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
}

#[derive(Copy, Clone)]
pub(super) enum LoopbackDevice {}

impl Device for LoopbackDevice {}

// TODO(https://fxbug.dev/121448): Remove this when it is unused.
impl<NonSyncCtx: NonSyncContext> DeviceIdContext<LoopbackDevice> for &'_ SyncCtx<NonSyncCtx> {
    type DeviceId = LoopbackDeviceId<NonSyncCtx::Instant, NonSyncCtx::LoopbackDeviceState>;
}

impl<'a, NonSyncCtx: NonSyncContext, L> DeviceIdContext<LoopbackDevice>
    for Locked<'a, SyncCtx<NonSyncCtx>, L>
{
    type DeviceId = <&'a SyncCtx<NonSyncCtx> as DeviceIdContext<LoopbackDevice>>::DeviceId;
}

pub(super) struct LoopbackDeviceState {
    mtu: Mtu,
    /// The routing metric of the loopback device this state is for.
    metric: RawMetric,
    rx_queue: ReceiveQueue<IpVersion, Buf<Vec<u8>>>,
    tx_queue: TransmitQueue<IpVersion, Buf<Vec<u8>>, BufVecU8Allocator>,
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
    type Data<'l> = crate::sync::LockGuard<'l, ReceiveQueueState<IpVersion, Buf<Vec<u8>>>>
        where
            Self: 'l;
    fn lock(&self) -> Self::Data<'_> {
        self.link.rx_queue.queue.lock()
    }
}

impl<I: Instant, S> LockFor<crate::lock_ordering::LoopbackRxDequeue>
    for IpLinkDeviceState<I, S, LoopbackDeviceState>
{
    type Data<'l> = crate::sync::LockGuard<'l, DequeueState<IpVersion, Buf<Vec<u8>>>>
        where
            Self: 'l;
    fn lock(&self) -> Self::Data<'_> {
        self.link.rx_queue.deque.lock()
    }
}

impl<I: Instant, S> LockFor<crate::lock_ordering::LoopbackTxQueue>
    for IpLinkDeviceState<I, S, LoopbackDeviceState>
{
    type Data<'l> = crate::sync::LockGuard<'l, TransmitQueueState<IpVersion, Buf<Vec<u8>>, BufVecU8Allocator>>
        where
            Self: 'l;
    fn lock(&self) -> Self::Data<'_> {
        self.link.tx_queue.queue.lock()
    }
}

impl<I: Instant, S> LockFor<crate::lock_ordering::LoopbackTxDequeue>
    for IpLinkDeviceState<I, S, LoopbackDeviceState>
{
    type Data<'l> = crate::sync::LockGuard<'l, DequeueState<IpVersion, Buf<Vec<u8>>>>
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
    sync_ctx: &mut Locked<'_, SyncCtx<NonSyncCtx>, L>,
    ctx: &mut NonSyncCtx,
    device_id: &LoopbackDeviceId<NonSyncCtx::Instant, NonSyncCtx::LoopbackDeviceState>,
    _local_addr: SpecifiedAddr<A>,
    body: S,
) -> Result<(), S> {
    match BufferTransmitQueueHandler::<LoopbackDevice, _, _>::queue_tx_frame(
        sync_ctx,
        ctx,
        device_id,
        A::Version::VERSION,
        body,
    ) {
        Ok(()) => Ok(()),
        Err(TransmitQueueFrameError::NoQueue(_)) => {
            unreachable!("loopback never fails to send a frame")
        }
        Err(TransmitQueueFrameError::QueueFull(s) | TransmitQueueFrameError::SerializeError(s)) => {
            Err(s)
        }
    }
}

/// Get the routing metric associated with this device.
pub(super) fn get_routing_metric<NonSyncCtx: NonSyncContext, L>(
    ctx: &mut Locked<'_, SyncCtx<NonSyncCtx>, L>,
    device_id: &LoopbackDeviceId<NonSyncCtx::Instant, NonSyncCtx::LoopbackDeviceState>,
) -> RawMetric {
    with_loopback_state(ctx, device_id, |mut state| state.cast_with(|s| &s.link.metric).copied())
}

/// Gets the MTU associated with this device.
pub(super) fn get_mtu<NonSyncCtx: NonSyncContext, L>(
    ctx: &mut Locked<'_, SyncCtx<NonSyncCtx>, L>,
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
    ReceiveQueueTypes<LoopbackDevice, C> for Locked<'_, SyncCtx<C>, L>
{
    type Meta = IpVersion;
    type Buffer = Buf<Vec<u8>>;
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::LoopbackRxQueue>>
    ReceiveQueueContext<LoopbackDevice, C> for Locked<'_, SyncCtx<C>, L>
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

    fn handle_packet(
        &mut self,
        ctx: &mut C,
        device_id: &LoopbackDeviceId<C::Instant, C::LoopbackDeviceState>,
        meta: IpVersion,
        buf: Buf<Vec<u8>>,
    ) {
        // TODO(https://fxbug.dev/120973): Remove this and push the Locked
        // wrapper through the receive path.
        let sync_ctx = self.unwrap_lock_order_protection();
        match meta {
            IpVersion::V4 => crate::ip::receive_ip_packet::<_, _, Ipv4>(
                sync_ctx,
                ctx,
                &device_id.clone().into(),
                FrameDestination::Unicast,
                buf,
            ),
            IpVersion::V6 => crate::ip::receive_ip_packet::<_, _, Ipv6>(
                sync_ctx,
                ctx,
                &device_id.clone().into(),
                FrameDestination::Unicast,
                buf,
            ),
        }
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::LoopbackRxDequeue>>
    ReceiveDequeContext<LoopbackDevice, C> for Locked<'_, SyncCtx<C>, L>
{
    type ReceiveQueueCtx<'a> = Locked<'a, SyncCtx<C>, crate::lock_ordering::LoopbackRxDequeue>;

    fn with_dequed_packets_and_rx_queue_ctx<
        O,
        F: FnOnce(&mut DequeueState<IpVersion, Buf<Vec<u8>>>, &mut Self::ReceiveQueueCtx<'_>) -> O,
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
    TransmitQueueTypes<LoopbackDevice, C> for Locked<'_, SyncCtx<C>, L>
{
    type Meta = IpVersion;
    type Allocator = BufVecU8Allocator;
    type Buffer = Buf<Vec<u8>>;
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::LoopbackTxQueue>>
    TransmitQueueContext<LoopbackDevice, C> for Locked<'_, SyncCtx<C>, L>
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
        match BufferReceiveQueueHandler::queue_rx_packet(self, ctx, device_id, meta, buf) {
            Ok(()) => {}
            Err(ReceiveQueueFullError((_ip_version, _frame))) => {
                // RX queue is full - there is nothing further we can do here.
                log::error!("dropped RX frame on loopback device due to full RX queue")
            }
        }

        Ok(())
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::LoopbackTxDequeue>>
    TransmitDequeueContext<LoopbackDevice, C> for Locked<'_, SyncCtx<C>, L>
{
    type TransmitQueueCtx<'a> = Locked<'a, SyncCtx<C>, crate::lock_ordering::LoopbackTxDequeue>;

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

    use net_types::{
        ip::{AddrSubnet, Ipv4, Ipv6},
        SpecifiedAddr,
    };

    use crate::{
        device::{DeviceId, Mtu},
        error::NotFoundError,
        ip::device::state::{AssignedAddress, IpDeviceStateIpExt},
        testutil::{FakeEventDispatcherConfig, TestIpExt, DEFAULT_INTERFACE_METRIC},
        Ctx, NonSyncContext, SyncCtx,
    };

    #[test]
    fn test_loopback_methods() {
        const MTU: Mtu = Mtu::new(66);
        let Ctx { sync_ctx, mut non_sync_ctx } = crate::testutil::FakeCtx::default();
        let mut sync_ctx = &sync_ctx;
        let device =
            crate::device::add_loopback_device(&mut sync_ctx, MTU, DEFAULT_INTERFACE_METRIC)
                .expect("error adding loopback device")
                .into();
        crate::device::testutil::enable_device(&mut sync_ctx, &mut non_sync_ctx, &device);

        assert_eq!(crate::ip::IpDeviceContext::<Ipv4, _>::get_mtu(&mut sync_ctx, &device), MTU);
        assert_eq!(crate::ip::IpDeviceContext::<Ipv6, _>::get_mtu(&mut sync_ctx, &device), MTU);

        fn test<I: TestIpExt + IpDeviceStateIpExt, NonSyncCtx: NonSyncContext>(
            sync_ctx: &mut &SyncCtx<NonSyncCtx>,
            ctx: &mut NonSyncCtx,
            device: &DeviceId<NonSyncCtx>,
            get_addrs: fn(
                &mut &SyncCtx<NonSyncCtx>,
                &DeviceId<NonSyncCtx>,
            ) -> Vec<SpecifiedAddr<I::Addr>>,
        ) {
            assert_eq!(get_addrs(sync_ctx, device), []);

            let FakeEventDispatcherConfig {
                subnet,
                local_ip,
                local_mac: _,
                remote_ip: _,
                remote_mac: _,
            } = I::FAKE_CONFIG;
            let addr = AddrSubnet::from_witness(local_ip, subnet.prefix())
                .expect("error creating AddrSubnet");
            assert_eq!(crate::device::add_ip_addr_subnet(sync_ctx, ctx, device, addr), Ok(()));
            let addr = addr.addr();
            assert_eq!(&get_addrs(sync_ctx, device)[..], [addr]);

            assert_eq!(crate::device::del_ip_addr(sync_ctx, ctx, device, &addr), Ok(()));
            assert_eq!(get_addrs(sync_ctx, device), []);

            assert_eq!(
                crate::device::del_ip_addr(sync_ctx, ctx, device, &addr),
                Err(NotFoundError)
            );
        }

        test::<Ipv4, _>(&mut sync_ctx, &mut non_sync_ctx, &device, |sync_ctx, device| {
            crate::ip::device::IpDeviceStateAccessor::<Ipv4, _>::with_ip_device_state(
                sync_ctx,
                device,
                |state| state.ip_state.iter_addrs().map(AssignedAddress::addr).collect::<Vec<_>>(),
            )
        });
        test::<Ipv6, _>(&mut sync_ctx, &mut non_sync_ctx, &device, |sync_ctx, device| {
            crate::ip::device::IpDeviceStateAccessor::<Ipv6, _>::with_ip_device_state(
                sync_ctx,
                device,
                |state| state.ip_state.iter_addrs().map(AssignedAddress::addr).collect::<Vec<_>>(),
            )
        });
    }
}
