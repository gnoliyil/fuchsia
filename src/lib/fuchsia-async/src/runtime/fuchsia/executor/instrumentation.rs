// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Fuchsia Executor Instrumentation.
//!
//! This module contains types used for instrumenting the fuchsia-async executor.
//! It exposes an event style API, intended to be invoked at certain points
//! during execution time, agnostic to the implementation of the executor.
//! The current implementation records counts and time slices with a focus
//! on minimal overhead.

use fuchsia_zircon as zx;
use std::{
    cmp, mem,
    panic::Location,
    sync::atomic::{AtomicU64, AtomicUsize, Ordering},
};

/// A low-overhead metrics collection type intended for use by an async executor.
#[derive(Default)]
pub struct Collector {
    tasks_created: AtomicUsize,
    tasks_completed: AtomicUsize,
    tasks_pending_max: AtomicUsize,
    polls: AtomicUsize,
    wakeups_io: AtomicUsize,
    wakeups_deadline: AtomicUsize,
    wakeups_notification: AtomicUsize,
    ticks_awake: AtomicU64,
    ticks_asleep: AtomicU64,
}

impl Collector {
    /// Create a new blank collector.
    pub fn new() -> Self {
        Self::default()
    }

    /// Called when a task is created (usually, this means spawned).
    pub fn task_created(&self, _id: usize, _source: Option<&Location<'_>>) {
        #[cfg(trace_level_logging)]
        tracing::trace!(
            tag = "fuchsia_async",
            id = _id,
            source = %_source.unwrap(),
            "Task spawned"
        );
        self.tasks_created.fetch_add(1, Ordering::Relaxed);
    }

    /// Called when a task is complete.
    pub fn task_completed(&self, _id: usize, _source: Option<&Location<'_>>) {
        #[cfg(trace_level_logging)]
        tracing::trace!(
            tag = "fuchsia_async",
            id = _id,
            source = %_source.unwrap(),
            "Task completed"
        );
        self.tasks_completed.fetch_add(1, Ordering::Relaxed);
    }

    /// Creates a local collector. Each run loop should have its own local collector.
    /// The local collector is initially awake, and has no recorded events.
    pub fn create_local_collector(&self) -> LocalCollector<'_> {
        LocalCollector {
            collector: &self,
            last_ticks: zx::ticks_get(),
            polls: 0,
            tasks_pending_max: 0, // Loading not necessary, handled by first update with same cost
        }
    }

    #[allow(dead_code)]
    /// Create a snapshot of the currently collected metrics. If the executor is running
    /// in multi-threaded mode, the snapshot is non-atomic. That said, the following
    /// invariant will hold: tasks_pending_max and tasks_completed <= tasks_created.
    pub fn snapshot(&self) -> Snapshot {
        Snapshot {
            tasks_pending_max: self.tasks_pending_max.load(Ordering::Acquire),
            tasks_completed: self.tasks_completed.load(Ordering::Acquire),
            tasks_created: self.tasks_created.load(Ordering::Acquire),
            polls: self.polls.load(Ordering::Acquire),
            wakeups_io: self.wakeups_io.load(Ordering::Acquire),
            wakeups_deadline: self.wakeups_deadline.load(Ordering::Acquire),
            wakeups_notification: self.wakeups_notification.load(Ordering::Acquire),
            ticks_awake: self.ticks_awake.load(Ordering::Acquire),
            ticks_asleep: self.ticks_asleep.load(Ordering::Acquire),
        }
    }
}

/// A logical sub-collector type of a `Collector`, for a local run-loop.
pub struct LocalCollector<'a> {
    /// The main collector of this local collector
    collector: &'a Collector,

    /// Ticks since the awake state was last toggled
    last_ticks: i64,

    /// Number of polls since last `will_wait`
    polls: usize,

    /// Last observed `tasks_pending_max` from main Collector
    tasks_pending_max: usize,
}

impl<'a> LocalCollector<'a> {
    /// Called after a task was polled. If the task completed, `complete`
    /// should be true. `pending_tasks` is the observed size of the pending task
    /// queue (excluding the currently polled task).
    pub fn task_polled(
        &mut self,
        id: usize,
        source: Option<&Location<'_>>,
        complete: bool,
        tasks_pending: usize,
    ) {
        self.polls += 1;
        let new_local_max = cmp::max(self.tasks_pending_max, tasks_pending);
        if new_local_max > self.tasks_pending_max {
            let prev_upstream_max =
                self.collector.tasks_pending_max.fetch_max(new_local_max, Ordering::Relaxed);
            self.tasks_pending_max = cmp::max(new_local_max, prev_upstream_max);
        }
        if complete {
            self.collector.task_completed(id, source);
        }
    }

    /// Called before the loop waits. Must be followed by a `woke_up` call.
    pub fn will_wait(&mut self) {
        let delta = self.bump_ticks();
        self.collector.ticks_awake.fetch_add(delta, Ordering::Relaxed);
        self.collector.polls.fetch_add(mem::replace(&mut self.polls, 0), Ordering::Relaxed);
    }

    /// Called after the loop wakes up from waiting, containing the reason
    /// for the wakeup. Must follow a `will_wait` call.
    pub fn woke_up(&mut self, wakeup_reason: WakeupReason) {
        let delta = self.bump_ticks();
        let counter = match wakeup_reason {
            WakeupReason::Io => &self.collector.wakeups_io,
            WakeupReason::Deadline => &self.collector.wakeups_deadline,
            WakeupReason::Notification => &self.collector.wakeups_notification,
        };
        counter.fetch_add(1, Ordering::Relaxed);
        self.collector.ticks_asleep.fetch_add(delta, Ordering::Relaxed);
    }

    /// Helper which replaces `last_ticks` with the current ticks.
    /// Returns the ticks elapsed since `last_ticks`.
    fn bump_ticks(&mut self) -> u64 {
        let current_ticks = zx::ticks_get();
        let delta = current_ticks - self.last_ticks;
        assert!(delta >= 0, "time moved backwards in zx::ticks_get()");
        self.last_ticks = current_ticks;
        delta as u64
    }
}

impl<'a> Drop for LocalCollector<'a> {
    fn drop(&mut self) {
        let delta = self.bump_ticks();
        self.collector.polls.fetch_add(self.polls, Ordering::Release);
        self.collector.ticks_awake.fetch_add(delta, Ordering::Release);
    }
}

/// The reason a waiting run-loop was woken up.
pub enum WakeupReason {
    /// An external io packet was received on the port.
    Io,

    /// Deadline from a user-space timer.
    Deadline,

    /// An executor-internal notification.
    Notification,
}

/// A snapshot of all metrics collected at a specific point in time.
#[derive(Debug)]
pub struct Snapshot {
    pub tasks_created: usize,
    pub tasks_completed: usize,
    pub tasks_pending_max: usize,
    pub polls: usize,
    pub wakeups_io: usize,
    pub wakeups_deadline: usize,
    pub wakeups_notification: usize,
    pub ticks_awake: u64,
    pub ticks_asleep: u64,
}

#[cfg(test)]
mod tests {
    use fuchsia_zircon::{self as zx, DurationNum};
    use futures::future;
    use pin_utils::pin_mut;

    use super::*;
    use crate::runtime::fuchsia::executor::{instrumentation::Snapshot, Time};
    use crate::{handle::channel::Channel, LocalExecutor, SendExecutor, TestExecutor, Timer};

    const MICROSECOND: std::time::Duration = std::time::Duration::from_micros(1);
    use std::thread::sleep;

    /// Helper which keeps track of last observed tick counts, and reports
    /// changes.
    struct Ticker<'a> {
        c: &'a Collector,
        awake: u64,  // Last observed awake ticks
        asleep: u64, // Last observed asleep ticks
    }

    impl<'a> Ticker<'a> {
        fn new(c: &'a Collector) -> Self {
            let result = Self { c, awake: 0, asleep: 0 };
            // Ensure that the next interaction with the collector that's supposed to accumulate
            // ticks will actually observe time having passed.
            sleep(1 * MICROSECOND);
            result
        }

        /// Updates awake and asleep ticks to current values. Returns a bool
        /// 2-tuple indicating whether awake and asleep time, respectively,
        /// progressed since last cycle.
        fn update(&mut self) -> (bool, bool) {
            let old_awake =
                mem::replace(&mut self.awake, self.c.ticks_awake.load(Ordering::Relaxed));
            let old_asleep =
                mem::replace(&mut self.asleep, self.c.ticks_asleep.load(Ordering::Relaxed));

            // Ensure that the next interaction with the collector that's supposed to accumulate
            // ticks will actually observe time having passed.
            sleep(1 * MICROSECOND);

            (self.awake > old_awake, self.asleep > old_asleep)
        }
    }

    #[test]
    fn debug_snapshot() {
        let collector = Collector {
            tasks_created: 10.into(),
            tasks_completed: 9.into(),
            tasks_pending_max: 3.into(),
            polls: 1000.into(),
            wakeups_io: 123.into(),
            wakeups_deadline: 456.into(),
            wakeups_notification: 789.into(),
            ticks_awake: 100000.into(),
            ticks_asleep: 200000.into(),
        };

        assert_eq!(
            format!("{:?}", collector.snapshot()),
            "\
        Snapshot { tasks_created: 10, tasks_completed: 9, tasks_pending_max: 3, \
        polls: 1000, wakeups_io: 123, wakeups_deadline: 456, wakeups_notification: 789, \
        ticks_awake: 100000, ticks_asleep: 200000 }"
        );
    }

    #[test]
    fn collector() {
        let collector = Collector::new();
        collector.task_created(0, Some(Location::caller()));
        collector.task_created(1, Some(Location::caller()));
        collector.task_completed(0, Some(Location::caller()));
        assert_eq!(collector.tasks_created.load(Ordering::Relaxed), 2);
        assert_eq!(collector.tasks_completed.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn task_polled() {
        let collector = Collector::new();
        collector.tasks_pending_max.store(6, Ordering::Relaxed);
        collector.polls.store(1, Ordering::Relaxed);
        let mut local_collector = collector.create_local_collector();
        local_collector.task_polled(0, Some(Location::caller()), false, /* pending_tasks */ 5);
        assert_eq!(collector.tasks_pending_max.load(Ordering::Relaxed), 6);
        local_collector.task_polled(0, Some(Location::caller()), false, /* pending_tasks */ 7);
        assert_eq!(collector.tasks_pending_max.load(Ordering::Relaxed), 7);
        assert_eq!(local_collector.polls, 2);

        // Polls not yet flushed
        assert_eq!(collector.polls.load(Ordering::Relaxed), 1);

        // Collector polls: 1 -> 3
        local_collector.will_wait();
        assert_eq!(collector.polls.load(Ordering::Relaxed), 3);

        // Check that local collector was reset
        assert_eq!(local_collector.tasks_pending_max, 7);
        assert_eq!(local_collector.polls, 0);

        // One more cycle to check that max is idempotent when main collector is greater
        collector.tasks_pending_max.store(10, Ordering::Relaxed);
        local_collector.woke_up(WakeupReason::Io);
        local_collector.task_polled(0, Some(Location::caller()), false, /* pending_tasks */ 8);
        assert_eq!(local_collector.tasks_pending_max, 10);

        // Flush with drop this time. Polls 3 -> 4
        drop(local_collector);

        assert_eq!(collector.tasks_pending_max.load(Ordering::Relaxed), 10);
        assert_eq!(collector.polls.load(Ordering::Relaxed), 4);
    }

    #[test]
    fn ticks() {
        let collector = Collector::new();
        let mut ticker = Ticker::new(&collector);
        let mut local_collector = collector.create_local_collector();
        assert_eq!(ticker.update(), (false, false));

        // Will wait should move awake time forward
        local_collector.will_wait();
        assert_eq!(ticker.update(), (true, false));

        // Woke up should move asleep time forward
        local_collector.woke_up(WakeupReason::Io);
        assert_eq!(ticker.update(), (false, true));

        // Poll should NOT move awake time forward
        local_collector.task_polled(0, Some(Location::caller()), true, 1);
        assert_eq!(ticker.update(), (false, false));

        // Drop should move awake time forward
        drop(local_collector);
        assert_eq!(ticker.update(), (true, false));
    }

    #[test]
    fn wakeups() {
        let collector = Collector::new();
        let mut local_collector = collector.create_local_collector();

        local_collector.will_wait();
        local_collector.woke_up(WakeupReason::Io);
        local_collector.will_wait();
        local_collector.woke_up(WakeupReason::Deadline);
        local_collector.will_wait();
        local_collector.woke_up(WakeupReason::Deadline);
        local_collector.will_wait();
        local_collector.woke_up(WakeupReason::Notification);
        local_collector.will_wait();
        local_collector.woke_up(WakeupReason::Notification);
        local_collector.will_wait();
        local_collector.woke_up(WakeupReason::Notification);

        assert_eq!(collector.wakeups_io.load(Ordering::Relaxed), 1);
        assert_eq!(collector.wakeups_deadline.load(Ordering::Relaxed), 2);
        assert_eq!(collector.wakeups_notification.load(Ordering::Relaxed), 3);
    }

    // Check that multiple local collectors coalesce into expected aggregates.
    // Covers some permutations of interleaved calls.
    #[test]
    fn multiple_local_collectors() {
        let collector = Collector::new();
        let mut ticker = Ticker::new(&collector);

        // tasks_created += 1
        collector.task_created(0, Some(Location::caller()));
        let mut local_1 = collector.create_local_collector();

        // tasks_polled += 1, tasks_pending_max = 5
        local_1.task_polled(0, Some(Location::caller()), false, 5);

        // ticks_awake += T
        local_1.will_wait();
        assert_eq!(ticker.update(), (true, false));

        let mut local_2 = collector.create_local_collector();

        // tasks_created += 1, tasks_polled += 2, tasks_complete += 1, tasks_pending_max = 7
        collector.task_created(1, Some(Location::caller()));
        local_2.task_polled(0, Some(Location::caller()), false, 7);
        local_2.task_polled(0, Some(Location::caller()), true, 3);

        // ticks_awake += T
        local_2.will_wait();
        assert_eq!(ticker.update(), (true, false));

        // ticks_asleep += T
        local_1.woke_up(WakeupReason::Io);
        assert_eq!(ticker.update(), (false, true));

        // ticks_asleep += T
        local_1.woke_up(WakeupReason::Deadline);
        assert_eq!(ticker.update(), (false, true));

        // ticks_awake += T
        drop(local_1);
        assert_eq!(ticker.update(), (true, false));

        // ticks_awake += T
        drop(local_2);
        assert_eq!(ticker.update(), (true, false));

        // Finally, check that counters match their expected values
        assert_eq!(collector.tasks_created.load(Ordering::Relaxed), 2);
        assert_eq!(collector.tasks_completed.load(Ordering::Relaxed), 1);
        assert_eq!(collector.polls.load(Ordering::Relaxed), 3);
        assert_eq!(collector.tasks_pending_max.load(Ordering::Relaxed), 7);
        assert_eq!(collector.wakeups_io.load(Ordering::Relaxed), 1);
        assert_eq!(collector.wakeups_deadline.load(Ordering::Relaxed), 1);
        assert_eq!(collector.wakeups_notification.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn instrumentation_single_smoke_test() {
        let mut executor = LocalExecutor::new();
        executor.run_singlethreaded(simple_task_for_snapshot());
        let snapshot = executor.snapshot();
        snapshot_sanity_check(&snapshot, 0);
    }

    #[test]
    fn instrumentation_stepwise_smoke_test() {
        let mut executor = TestExecutor::new();
        let fut = simple_task_for_snapshot();
        pin_mut!(fut);
        assert!(executor.run_until_stalled(&mut fut).is_pending());
        executor.wake_expired_timers();
        assert!(executor.run_until_stalled(&mut fut).is_ready());
        let snapshot = executor.snapshot();
        snapshot_sanity_check(&snapshot, 0);
    }

    #[test]
    fn instrumentation_multi_smoke_test() {
        let mut executor = SendExecutor::new(2);
        executor.run(simple_task_for_snapshot());
        let snapshot = executor.snapshot();
        snapshot_sanity_check(&snapshot, /* extra_tasks */ 1);
    }

    // This task spawns another tasks, which completes. It should wake up from IO, notification
    // and deadline at least once each. Min polls is 4.
    pub async fn simple_task_for_snapshot() {
        let bytes = &[0, 1, 2, 3];
        let (tx, rx) = zx::Channel::create();
        let f_rx = Channel::from_channel(rx).unwrap();
        let mut buffer = zx::MessageBuf::new();
        let read_fut = f_rx.recv_msg(&mut buffer);

        // This extra poll ensures a happens-before relationship between the read and the write
        // future in order to trigger an IO wakeup. This registers a waker with the executor
        // which guarantees that the IO wakeup will be skipped by short circuiting.
        let pending_read_fut = match future::select(read_fut, future::ready(())).await {
            future::Either::Right((_, pending)) => pending,
            _ => panic!("read future complete before write"),
        };
        let t = crate::Task::spawn(async move {
            let mut handles = Vec::new();
            tx.write(bytes, &mut handles).expect("failed to write message");
            Timer::new(Time::after(0.nanos())).await;
        });
        pending_read_fut.await.expect("read future did not complete");
        t.await;
    }

    // Sanity check for running simple_task on an executor. `extra_tasks` represents
    // synthetic tasks that are added as an impl detail of the execution - e.g. a multithreaded
    // execution run creates an extra synthetic task for the main future.
    pub fn snapshot_sanity_check(snapshot: &Snapshot, extra_tasks: usize) {
        assert!(snapshot.polls >= 4);
        assert_eq!(snapshot.tasks_created - extra_tasks, 2);
        assert_eq!(snapshot.tasks_completed - extra_tasks, 1);
        assert!(snapshot.wakeups_io >= 1);
        assert!(snapshot.wakeups_deadline >= 1);

        // Future optimizations of the executor could theoretically lead to notifications
        // being eliminated in some cases.
        assert!(snapshot.wakeups_notification >= 1);
        assert!(snapshot.ticks_awake >= 1);
        assert!(snapshot.ticks_asleep >= 1);
    }
}
