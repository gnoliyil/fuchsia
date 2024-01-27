// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::runtime::{EHandle, PacketReceiver, ReceiverRegistration},
    fuchsia_zircon::{self as zx, AsHandleRef},
    futures::task::{AtomicWaker, Context, Poll},
    std::sync::{
        atomic::{AtomicU32, Ordering},
        Arc,
    },
};

const OBJECT_PEER_CLOSED: zx::Signals = zx::Signals::OBJECT_PEER_CLOSED;
const OBJECT_READABLE: zx::Signals = zx::Signals::OBJECT_READABLE;
const OBJECT_WRITABLE: zx::Signals = zx::Signals::OBJECT_WRITABLE;

struct RWPacketReceiver {
    signals: AtomicU32,
    read_task: AtomicWaker,
    write_task: AtomicWaker,
}

impl PacketReceiver for RWPacketReceiver {
    fn receive_packet(&self, packet: zx::Packet) {
        let new = if let zx::PacketContents::SignalOne(p) = packet.contents() {
            p.observed()
        } else {
            return;
        };

        let old =
            zx::Signals::from_bits_truncate(self.signals.fetch_or(new.bits(), Ordering::SeqCst));

        let became_readable = new.contains(OBJECT_READABLE) && !old.contains(OBJECT_READABLE);
        let became_writable = new.contains(OBJECT_WRITABLE) && !old.contains(OBJECT_WRITABLE);
        let became_closed = new.contains(OBJECT_PEER_CLOSED) && !old.contains(OBJECT_PEER_CLOSED);

        if became_readable || became_closed {
            self.read_task.wake();
        }
        if became_writable || became_closed {
            self.write_task.wake();
        }
    }
}

/// A `Handle` that receives notifications when it is readable/writable.
pub struct RWHandle<T> {
    handle: T,
    receiver: ReceiverRegistration<RWPacketReceiver>,
}

impl<T> RWHandle<T>
where
    T: AsHandleRef,
{
    /// Creates a new `RWHandle` object which will receive notifications when
    /// the underlying handle becomes readable, writable, or closes.
    pub fn new(handle: T) -> Result<Self, zx::Status> {
        let ehandle = EHandle::local();

        let initial_signals = OBJECT_READABLE | OBJECT_WRITABLE;
        let receiver = ehandle.register_receiver(Arc::new(RWPacketReceiver {
            // Optimistically assume that the handle is readable and writable.
            // Reads and writes will be attempted before queueing a packet.
            // This makes handles slightly faster to read/write the first time
            // they're accessed after being created, provided they start off as
            // readable or writable. In return, there will be an extra wasted
            // syscall per read/write if the handle is not readable or writable.
            signals: AtomicU32::new(initial_signals.bits()),
            read_task: AtomicWaker::new(),
            write_task: AtomicWaker::new(),
        }));

        let rwhandle = RWHandle { handle, receiver };

        // Make sure we get notifications when the handle closes.
        rwhandle.schedule_packet(OBJECT_PEER_CLOSED)?;

        Ok(rwhandle)
    }

    /// Returns a reference to the underlying handle.
    pub fn get_ref(&self) -> &T {
        &self.handle
    }

    /// Returns a mutable reference to the underlying handle.
    pub fn get_mut(&mut self) -> &mut T {
        &mut self.handle
    }

    /// Consumes this type, returning the inner handle.
    pub fn into_inner(self) -> T {
        self.handle
    }

    /// Tests to see if the channel received a `OBJECT_PEER_CLOSED` signal
    pub fn is_closed(&self) -> bool {
        let signals =
            zx::Signals::from_bits_truncate(self.receiver().signals.load(Ordering::Relaxed));
        signals.contains(OBJECT_PEER_CLOSED)
    }

    /// Tests if the resource currently has either the provided `signal`
    /// or the `OBJECT_PEER_CLOSED` signal set.
    fn poll_signal_or_closed(
        &self,
        cx: &mut Context<'_>,
        task: &AtomicWaker,
        signal: zx::Signals,
    ) -> Poll<Result<zx::Signals, zx::Status>> {
        let signals =
            zx::Signals::from_bits_truncate(self.receiver().signals.load(Ordering::SeqCst));
        let was_closed = signals.contains(OBJECT_PEER_CLOSED);
        let was_signal = signals.contains(signal);
        if was_closed || was_signal {
            let mask = signal | OBJECT_PEER_CLOSED;
            Poll::Ready(Ok(signals & mask))
        } else {
            self.need_signal(cx, task, signal)?;
            Poll::Pending
        }
    }

    /// Tests to see if this resource is ready to be read from.
    /// If it is not, it arranges for the current task to receive a notification
    /// when a "readable" or "closed" signal arrives.
    /// Returns the cached signals, masked to `OBJECT_READABLE` and
    /// `OBJECT_PEER_CLOSED`. If the read syscall returns `SHOULD_WAIT`, you
    /// must call `need_read` to clear the cached state and wait for the
    /// resource to become readable again.
    pub fn poll_read(&self, cx: &mut Context<'_>) -> Poll<Result<zx::Signals, zx::Status>> {
        self.poll_signal_or_closed(cx, &self.receiver.read_task, OBJECT_READABLE)
    }

    /// Tests to see if this resource is ready to be written to.
    /// If it is not, it arranges for the current task to receive a notification
    /// when a "writable" or "closed" signal arrives.
    /// Returns the cached signals, masked to `OBJECT_WRITABLE` and
    /// `OBJECT_PEER_CLOSED`. If the write syscall returns `SHOULD_WAIT`,
    /// you must call `need_write` to clear the cached state and wait for the
    /// resource to become writable again.
    pub fn poll_write(&self, cx: &mut Context<'_>) -> Poll<Result<zx::Signals, zx::Status>> {
        self.poll_signal_or_closed(cx, &self.receiver.write_task, OBJECT_WRITABLE)
    }

    fn receiver(&self) -> &RWPacketReceiver {
        self.receiver.receiver()
    }

    /// Arranges for the current task to receive a notification when a
    /// given signal arrives.
    fn need_signal(
        &self,
        cx: &mut Context<'_>,
        task: &AtomicWaker,
        signal: zx::Signals,
    ) -> Result<(), zx::Status> {
        crate::runtime::need_signal(
            cx,
            task,
            &self.receiver.signals,
            signal,
            self.handle.as_handle_ref(),
            self.receiver.port(),
            self.receiver.key(),
        )
    }

    /// Arranges for the current task to receive a notification when a
    /// "readable" signal arrives.
    pub fn need_read(&self, cx: &mut Context<'_>) -> Result<(), zx::Status> {
        self.need_signal(cx, &self.receiver.read_task, OBJECT_READABLE)
    }

    /// Arranges for the current task to receive a notification when a
    /// "writable" signal arrives.
    pub fn need_write(&self, cx: &mut Context<'_>) -> Result<(), zx::Status> {
        self.need_signal(cx, &self.receiver.write_task, OBJECT_WRITABLE)
    }

    fn schedule_packet(&self, signals: zx::Signals) -> Result<(), zx::Status> {
        crate::runtime::schedule_packet(
            self.handle.as_handle_ref(),
            self.receiver.port(),
            self.receiver.key(),
            signals,
        )
    }
}
