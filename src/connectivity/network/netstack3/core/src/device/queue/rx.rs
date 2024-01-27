// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! RX device queues.

use derivative::Derivative;
use packet::ParseBuffer;

use crate::{
    device::{
        queue::{
            fifo, DequeueResult, DequeueState, EnqueueResult, ReceiveQueueFullError, MAX_BATCH_SIZE,
        },
        Device, DeviceIdContext,
    },
    sync::Mutex,
};

/// The state used to hold a queue of received packets to be handled at a later
/// time.
#[derive(Derivative)]
#[derivative(Default(bound = ""))]
#[cfg_attr(test, derive(Debug, PartialEq, Eq))]
pub(crate) struct ReceiveQueueState<Meta, Buffer> {
    queue: fifo::Queue<Meta, Buffer>,
}

/// The non-synchonized context for the receive queue.
pub(crate) trait ReceiveQueueNonSyncContext<D: Device, DeviceId> {
    /// Wakes up RX task.
    fn wake_rx_task(&mut self, device_id: &DeviceId);
}

#[derive(Derivative)]
#[derivative(Default(bound = ""))]
pub(crate) struct ReceiveQueue<Meta, Buffer> {
    /// The state for dequeued packets that will be handled.
    ///
    /// See `queue` for lock ordering.
    pub(crate) deque: Mutex<DequeueState<Meta, Buffer>>,
    /// A queue of received packets protected by a lock.
    ///
    /// Lock ordering: `deque` must be locked before `queue` is locked when both
    /// are needed at the same time.
    pub(crate) queue: Mutex<ReceiveQueueState<Meta, Buffer>>,
}

pub(crate) trait ReceiveQueueTypes<D: Device, C>: DeviceIdContext<D> {
    /// Metadata associated with an RX packet.
    ///
    /// E.g. loopback may hold the packet's IP version in the metadata (since
    /// loopback has no headers that may be used to identify the packet type).
    type Meta;

    /// The type of buffer holding an RX packet.
    type Buffer: ParseBuffer;
}

/// The execution context for a receive queue.
pub(crate) trait ReceiveQueueContext<D: Device, C>: ReceiveQueueTypes<D, C> {
    /// Calls the function with the RX queue state.
    fn with_receive_queue_mut<O, F: FnOnce(&mut ReceiveQueueState<Self::Meta, Self::Buffer>) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O;

    /// Handle a received packet.
    fn handle_packet(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        meta: Self::Meta,
        buf: Self::Buffer,
    );
}

pub(crate) trait ReceiveDequeContext<D: Device, C>: ReceiveQueueTypes<D, C> {
    type ReceiveQueueCtx<'a>: ReceiveQueueContext<
        D,
        C,
        Meta = Self::Meta,
        Buffer = Self::Buffer,
        DeviceId = Self::DeviceId,
    >;

    /// Calls the function with the RX deque state and the RX queue context.
    fn with_dequed_packets_and_rx_queue_ctx<
        O,
        F: FnOnce(&mut DequeueState<Self::Meta, Self::Buffer>, &mut Self::ReceiveQueueCtx<'_>) -> O,
    >(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O;
}

/// An implementation of a receive queue.
pub(crate) trait ReceiveQueueHandler<D: Device, C>: ReceiveQueueTypes<D, C> {
    /// Handle any queued RX packets.
    fn handle_queued_rx_packets(&mut self, ctx: &mut C, device_id: &Self::DeviceId);
}

/// An implementation of a receive queue, with a buffer.
pub(crate) trait BufferReceiveQueueHandler<D: Device, B: ParseBuffer, C>:
    ReceiveQueueTypes<D, C>
{
    /// Queues a packet for reception.
    ///
    /// # Errors
    ///
    /// Returns an error with the metadata and body if the queue is full.
    fn queue_rx_packet(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        meta: Self::Meta,
        body: B,
    ) -> Result<(), ReceiveQueueFullError<(Self::Meta, B)>>;
}

impl<D: Device, C: ReceiveQueueNonSyncContext<D, SC::DeviceId>, SC: ReceiveDequeContext<D, C>>
    ReceiveQueueHandler<D, C> for SC
{
    fn handle_queued_rx_packets(&mut self, ctx: &mut C, device_id: &SC::DeviceId) {
        self.with_dequed_packets_and_rx_queue_ctx(
            device_id,
            |DequeueState { dequed_packets }, rx_queue_ctx| {
                assert_eq!(
                    dequed_packets.len(),
                    0,
                    "should not keep dequeued packets across calls to this fn"
                );

                let ret = rx_queue_ctx.with_receive_queue_mut(
                    device_id,
                    |ReceiveQueueState { queue }| {
                        queue.dequeue_packets_into(dequed_packets, MAX_BATCH_SIZE)
                    },
                );

                while let Some((meta, p)) = dequed_packets.pop_front() {
                    rx_queue_ctx.handle_packet(ctx, device_id, meta, p);
                }

                match ret {
                    DequeueResult::MorePacketsStillQueued => ctx.wake_rx_task(device_id),
                    DequeueResult::NoMorePacketsLeft => {
                        // There are no more packets left after the batch we
                        // just handled. When the next RX packet gets enqueued,
                        // the RX task will be woken up again.
                    }
                }
            },
        )
    }
}

impl<D: Device, C: ReceiveQueueNonSyncContext<D, SC::DeviceId>, SC: ReceiveQueueContext<D, C>>
    BufferReceiveQueueHandler<D, SC::Buffer, C> for SC
{
    fn queue_rx_packet(
        &mut self,
        ctx: &mut C,
        device_id: &SC::DeviceId,
        meta: SC::Meta,
        body: SC::Buffer,
    ) -> Result<(), ReceiveQueueFullError<(Self::Meta, SC::Buffer)>> {
        self.with_receive_queue_mut(device_id, |ReceiveQueueState { queue }| {
            queue.queue_rx_packet(meta, body).map(|res| match res {
                EnqueueResult::QueueWasPreviouslyEmpty => ctx.wake_rx_task(device_id),
                EnqueueResult::QueuePreviouslyHadPackets => {
                    // We have already woken up the RX task when the queue was
                    // previously empty so there is no need to do it again.
                }
            })
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use alloc::{vec, vec::Vec};

    use packet::Buf;

    use crate::{
        context::testutil::{FakeCtx, FakeNonSyncCtx, FakeSyncCtx},
        device::{
            link::testutil::{FakeLinkDevice, FakeLinkDeviceId},
            queue::MAX_RX_QUEUED_PACKETS,
        },
    };

    #[derive(Default)]
    struct FakeRxQueueState {
        queue: ReceiveQueueState<(), Buf<Vec<u8>>>,
        handled_packets: Vec<Buf<Vec<u8>>>,
    }

    #[derive(Default)]
    struct FakeRxQueueNonSyncCtxState {
        woken_rx_tasks: Vec<FakeLinkDeviceId>,
    }

    type FakeSyncCtxImpl = FakeSyncCtx<FakeRxQueueState, (), FakeLinkDeviceId>;
    type FakeNonSyncCtxImpl = FakeNonSyncCtx<(), (), FakeRxQueueNonSyncCtxState>;

    impl ReceiveQueueNonSyncContext<FakeLinkDevice, FakeLinkDeviceId> for FakeNonSyncCtxImpl {
        fn wake_rx_task(&mut self, device_id: &FakeLinkDeviceId) {
            self.state_mut().woken_rx_tasks.push(device_id.clone())
        }
    }

    impl ReceiveQueueTypes<FakeLinkDevice, FakeNonSyncCtxImpl> for FakeSyncCtxImpl {
        type Meta = ();
        type Buffer = Buf<Vec<u8>>;
    }

    impl ReceiveQueueContext<FakeLinkDevice, FakeNonSyncCtxImpl> for FakeSyncCtxImpl {
        fn with_receive_queue_mut<O, F: FnOnce(&mut ReceiveQueueState<(), Buf<Vec<u8>>>) -> O>(
            &mut self,
            &FakeLinkDeviceId: &FakeLinkDeviceId,
            cb: F,
        ) -> O {
            cb(&mut self.get_mut().queue)
        }

        fn handle_packet(
            &mut self,
            _ctx: &mut FakeNonSyncCtxImpl,
            &FakeLinkDeviceId: &FakeLinkDeviceId,
            (): (),
            buf: Buf<Vec<u8>>,
        ) {
            self.get_mut().handled_packets.push(buf)
        }
    }

    impl ReceiveDequeContext<FakeLinkDevice, FakeNonSyncCtxImpl> for FakeSyncCtxImpl {
        type ReceiveQueueCtx<'a> = Self;

        fn with_dequed_packets_and_rx_queue_ctx<
            O,
            F: FnOnce(
                &mut DequeueState<Self::Meta, Self::Buffer>,
                &mut Self::ReceiveQueueCtx<'_>,
            ) -> O,
        >(
            &mut self,
            &FakeLinkDeviceId: &FakeLinkDeviceId,
            cb: F,
        ) -> O {
            cb(&mut DequeueState::default(), self)
        }
    }

    #[test]
    fn queue_and_dequeue() {
        let FakeCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtx::with_sync_ctx(FakeSyncCtxImpl::default());

        for _ in 0..2 {
            for i in 0..MAX_RX_QUEUED_PACKETS {
                let body = Buf::new(vec![i as u8], ..);
                assert_eq!(
                    BufferReceiveQueueHandler::queue_rx_packet(
                        &mut sync_ctx,
                        &mut non_sync_ctx,
                        &FakeLinkDeviceId,
                        (),
                        body
                    ),
                    Ok(())
                );
                // We should only ever be woken up once when the first packet
                // was enqueued.
                assert_eq!(non_sync_ctx.state().woken_rx_tasks, [FakeLinkDeviceId]);
            }

            let body = Buf::new(vec![131], ..);
            assert_eq!(
                BufferReceiveQueueHandler::queue_rx_packet(
                    &mut sync_ctx,
                    &mut non_sync_ctx,
                    &FakeLinkDeviceId,
                    (),
                    body.clone(),
                ),
                Err(ReceiveQueueFullError(((), body)))
            );
            // We should only ever be woken up once when the first packet
            // was enqueued.
            assert_eq!(
                core::mem::take(&mut non_sync_ctx.state_mut().woken_rx_tasks),
                [FakeLinkDeviceId]
            );

            assert!(MAX_RX_QUEUED_PACKETS > MAX_BATCH_SIZE);
            for i in (0..(MAX_RX_QUEUED_PACKETS - MAX_BATCH_SIZE)).step_by(MAX_BATCH_SIZE) {
                ReceiveQueueHandler::handle_queued_rx_packets(
                    &mut sync_ctx,
                    &mut non_sync_ctx,
                    &FakeLinkDeviceId,
                );
                assert_eq!(
                    core::mem::take(&mut sync_ctx.get_mut().handled_packets),
                    (i..i + MAX_BATCH_SIZE)
                        .into_iter()
                        .map(|i| Buf::new(vec![i as u8], ..))
                        .collect::<Vec<_>>()
                );
                // We should get a wake up signal when packets remain after
                // handling a batch of RX packets.
                assert_eq!(
                    core::mem::take(&mut non_sync_ctx.state_mut().woken_rx_tasks),
                    [FakeLinkDeviceId]
                );
            }

            ReceiveQueueHandler::handle_queued_rx_packets(
                &mut sync_ctx,
                &mut non_sync_ctx,
                &FakeLinkDeviceId,
            );
            assert_eq!(
                core::mem::take(&mut sync_ctx.get_mut().handled_packets),
                (MAX_BATCH_SIZE * (MAX_RX_QUEUED_PACKETS / MAX_BATCH_SIZE - 1)
                    ..MAX_RX_QUEUED_PACKETS)
                    .into_iter()
                    .map(|i| Buf::new(vec![i as u8], ..))
                    .collect::<Vec<_>>()
            );
            // Should not have woken up the RX task since the queue should be
            // empty.
            assert_eq!(core::mem::take(&mut non_sync_ctx.state_mut().woken_rx_tasks), []);

            // The queue should now be empty so the next iteration of queueing
            // `MAX_RX_QUEUED_PACKETS` packets should succeed.
        }
    }
}
