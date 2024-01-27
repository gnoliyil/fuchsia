// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::collections::hash_map::Entry;
use std::collections::HashMap;
use std::fmt::Debug;
use std::hash::Hash;
use std::ops::{Deref, DerefMut};

use async_utils::futures::{FutureExt as _, ReplaceValue};
use derivative::Derivative;
use fuchsia_async as fasync;
use futures::{
    channel::mpsc,
    future::{AbortHandle, Abortable, Aborted},
    stream::{FuturesUnordered, StreamExt as _},
};
use log::{trace, warn};

use netstack3_core::sync::RwLock as CoreRwLock;

use super::StackTime;

#[derive(Copy, Clone, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
struct InternalId(u64);

impl InternalId {
    fn increment(&self) -> Self {
        let Self(id) = self;
        Self(id.wrapping_add(1))
    }
}

/// Internal information to keep tabs on timers.
struct TimerInfo {
    id: InternalId,
    instant: StackTime,
    abort_handle: AbortHandle,
}

/// A context for specified for a timer type `T` that provides access to a
/// [`TimerHandler`].
pub(crate) trait TimerContext<T: Hash + Eq>: 'static + Clone {
    type Handler: TimerHandler<T>;

    fn handler(&self) -> Self::Handler;
}

/// An entity responsible for receiving expired timers.
///
/// `TimerHandler` is used to communicate expired timers from a
/// [`TimerDispatcher`] that was spawned with some [`TimerContext`].
pub(crate) trait TimerHandler<T: Hash + Eq>: Sized + 'static {
    /// The provided `timer` is expired (its deadline arrived and it wasn't
    /// cancelled or rescheduled).
    fn handle_expired_timer(&mut self, timer: T);
    /// Retrieve a mutable reference to the [`TimerDispatcher`] associated with
    /// this `TimerHandler`. It *must* be the same `TimerDispatcher` instance
    /// for which this handler's [`TimerContext`] was spawned with
    /// [`TimerDispatcher::spawn`].
    fn get_timer_dispatcher(&mut self) -> &TimerDispatcher<T>;
}

type TimerFut = ReplaceValue<fasync::Timer, InternalId>;

/// Shorthand for the type of futures used by [`TimerDispatcher`] internally.
type InternalFut = Abortable<TimerFut>;

#[derive(Derivative)]
#[derivative(Default(bound = ""))]
struct TimerDispatcherInner<T: Hash + Eq> {
    // Invariant: This dispatcher uses a HashMap keyed on an external identifier
    // T and assigns an internal "versioning" ID every time a timer is
    // scheduled. The "versioning" ID is just monotonically incremented and it
    // is used to disambiguate different scheduling events of the same timer T.
    // TimerInfo in the HashMap will always hold the latest allocated
    // "versioning" identifier, meaning that:
    //  - When a timer is rescheduled, we update TimerInfo::id to a new value
    //  - To "commit" a timer firing (through commit_timer), the timer ID
    //    given must carry the same "versioning" identifier as the one currently
    //    held by the HashMap in TimerInfo::id.
    //  - timer_ids will always/only hold the timer IDs that are still scheduled
    //    keyed by their most recent "versioning" ID.
    // The committing mechanism is invisible to external users, which just
    // receive the expired timers through TimerHandler::handle_expired_timer.
    // See TimerDispatcher::spawn for the critical section that makes versioning
    // required.
    timer_ids: HashMap<InternalId, T>,
    timers: HashMap<T, TimerInfo>,
    next_id: InternalId,
    futures_sender: Option<mpsc::UnboundedSender<InternalFut>>,
}

/// Helper struct to keep track of timers for the event loop.
#[derive(Derivative)]
#[derivative(Default(bound = ""))]
pub(crate) struct TimerDispatcher<T: Hash + Eq> {
    inner: CoreRwLock<TimerDispatcherInner<T>>,
}

impl<T> TimerDispatcher<T>
where
    T: Hash + Debug + Eq + Clone + Send + Sync + Unpin + 'static,
{
    /// Spawns a [`TimerContext`] that will observe events on this
    /// `TimerDispatcher` through its [`TimerHandler`].
    ///
    /// # Panics
    ///
    /// Panics if this `TimerDispatcher` was already spawned.
    pub(crate) fn spawn<C: TimerContext<T> + Send + Sync>(&self, ctx: C) {
        let (sender, mut recv) = mpsc::unbounded();
        {
            let Self { inner } = self;
            let mut inner = inner.write();
            let TimerDispatcherInner { timer_ids: _, timers: _, next_id: _, futures_sender } =
                inner.deref_mut();
            assert!(futures_sender.replace(sender).is_none(), "TimerDispatcher already spawned");
        };

        fasync::Task::spawn(async move {
            let mut futures = FuturesUnordered::<InternalFut>::new();

            #[derive(Debug)]
            enum PollResult {
                InstallFuture(InternalFut),
                TimerFired(InternalId),
                Aborted,
                ReceiverClosed,
                FuturesClosed,
            }

            loop {
                // avoid polling `futures` if it is empty
                let r = if futures.is_empty() {
                    match recv.next().await {
                        Some(next_fut) => PollResult::InstallFuture(next_fut),
                        None => PollResult::ReceiverClosed,
                    }
                } else {
                    futures::select! {
                        r = recv.next() => match r {
                            Some(next_fut) => PollResult::InstallFuture(next_fut),
                            None => PollResult::ReceiverClosed
                        },
                        t = futures.next() => match t {
                            Some(Ok(t)) => PollResult::TimerFired(t),
                            Some(Err(Aborted)) => PollResult::Aborted,
                            None => PollResult::FuturesClosed
                        }
                    }
                };
                // NB: This is the critical section that makes it so that we
                // need to version timers and verify the versioning through
                // `commit_timer` before passing those over to the handler. At
                // this point, the timer future has already resolved. It may
                // already have been aborted, in which case the version ID
                // doesn't matter. But it may also have been already fulfilled.
                // The race comes from the fact that we don't currently have a
                // lock on the context. If we are woken up to handle a timer,
                // the timer ID we're currently holding may have been
                // invalidated by another Task, before this task was woken up so
                // it must NOT be given to to the handler.
                //
                // After this point, we know nothing else can cancel the timer
                // since (as of writing) we use a single-threaded executor and
                // we have no await points below.
                //
                // TODO(https://fxbug.dev/122725): Fix timer dispatcher race
                // with multithreaded executor/concurrent tasks.

                trace!("TimerDispatcher work: {:?}", r);
                match r {
                    PollResult::InstallFuture(fut) => futures.push(fut),
                    PollResult::TimerFired(t) => {
                        let mut handler = ctx.handler();
                        let disp = handler.get_timer_dispatcher();

                        match disp.commit_timer(t) {
                            Ok(t) => {
                                trace!("TimerDispatcher: firing timer {:?}", t);
                                handler.handle_expired_timer(t);
                            }
                            Err(e) => {
                                trace!("TimerDispatcher: timer was stale {:?}", e);
                            }
                        }
                    }
                    PollResult::Aborted => {}
                    PollResult::ReceiverClosed | PollResult::FuturesClosed => break,
                }
            }
        })
        .detach();
    }

    /// Schedule a new timer with identifier `timer_id` at `time`.
    ///
    /// If a timer with the same `timer_id` was already scheduled, the old timer
    /// is unscheduled and its expiry time is returned.
    pub(crate) fn schedule_timer(&self, timer_id: T, time: StackTime) -> Option<StackTime> {
        let Self { inner } = self;
        let mut inner = inner.write();
        let TimerDispatcherInner { timer_ids, timers, next_id, futures_sender } = inner.deref_mut();

        let next_id = {
            let id = *next_id;

            // Overflowing next_id should be safe enough to hold TimerDispatcher's
            // invariant about around "versioning" timer identifiers. We'll
            // overlflow after 2^64 timers are scheduled (which can take a while)
            // and, even then, for it to break the invariant we'd need to still have
            // a timer scheduled from long ago and be unlucky enough that ordering
            // ends up giving it the same ID. That seems unlikely, so we just wrap
            // around and overflow next_id.
            *next_id = next_id.increment();

            id
        };

        let (abort_handle, abort_registration) = AbortHandle::new_pair();
        let timeout = {
            let StackTime(time) = time;
            Abortable::new(fasync::Timer::new(time).replace_value(next_id), abort_registration)
        };

        if let Some(sender) = futures_sender.as_ref() {
            sender.unbounded_send(timeout).expect("TimerDispatcher's task receiver is gone");
        } else {
            // Timers are always stopped in tests.
            warn!("TimerDispatcher not spawned, timer {:?} will not fire", timer_id);
        }

        assert_eq!(timer_ids.insert(next_id, timer_id.clone()), None);

        match timers.entry(timer_id) {
            Entry::Vacant(e) => {
                // If we don't have any currently scheduled timers with this
                // timer_id, we're just going to insert a new value into the
                // vacant entry, marking it with next_id.
                let _: &mut TimerInfo =
                    e.insert(TimerInfo { id: next_id, instant: time, abort_handle });
                None
            }
            Entry::Occupied(mut e) => {
                // If we already have a scheduled timer with this timer_id, we
                // must...
                let info = e.get_mut();
                // ...call the abort handle on the old timer, to prevent it from
                // firing if it hasn't already:
                info.abort_handle.abort();
                // ...update the abort handle with the new one:
                info.abort_handle = abort_handle;
                // ...store the new "versioning" timer_id next_id, effectively
                // marking this newest version as the only valid one, in case
                // the old timer had already fired as is currently waiting to be
                // committed.
                let prev_id = info.id;
                info.id = next_id;
                // ...finally, we get the old instant information to be returned
                // and update the TimerInfo entry with the new time value:
                let old = Some(info.instant);
                info.instant = time;

                assert_eq!(timer_ids.remove(&prev_id).as_ref(), Some(e.key()));

                old
            }
        }
    }

    /// Cancels a timer with identifier `timer_id`.
    ///
    /// If a timer with the provided `timer_id` was scheduled, returns the
    /// expiry time for it after having cancelled it.
    pub(crate) fn cancel_timer(&self, timer_id: &T) -> Option<StackTime> {
        let Self { inner } = self;
        let mut inner = inner.write();
        let TimerDispatcherInner { timer_ids, timers, next_id: _, futures_sender: _ } =
            inner.deref_mut();
        if let Some(t) = timers.remove(timer_id) {
            // call the abort handle, in case the future hasn't fired yet:
            t.abort_handle.abort();
            assert_eq!(timer_ids.remove(&t.id).as_ref(), Some(timer_id));
            Some(t.instant)
        } else {
            None
        }
    }

    /// Cancels all timers with given filter.
    ///
    /// `f` will be called sequentially for all the currently scheduled timers.
    /// If `f(id)` returns `true`, the timer with `id` will be cancelled.
    pub(crate) fn cancel_timers_with<F: FnMut(&T) -> bool>(&self, mut f: F) {
        let Self { inner } = self;
        let mut inner = inner.write();
        let TimerDispatcherInner { timer_ids: _, timers, next_id: _, futures_sender: _ } =
            inner.deref_mut();
        timers.retain(|id, info| {
            let discard = f(&id);
            if discard {
                info.abort_handle.abort();
            }
            !discard
        });
    }

    /// Gets the time a timer with identifier `timer_id` will be invoked.
    ///
    /// If a timer with the provided `timer_id` exists, returns the expiry
    /// time for it; `None` otherwise.
    pub(crate) fn scheduled_time(&self, timer_id: &T) -> Option<StackTime> {
        let Self { inner } = self;
        let inner = inner.read();
        let TimerDispatcherInner { timer_ids: _, timers, next_id: _, futures_sender: _ } =
            inner.deref();
        timers.get(timer_id).map(|t| t.instant)
    }

    /// Retrieves the internal timer value of a timer ID.
    ///
    /// `commit_timer` will "commit" `id` for consumption, if `id` is
    /// still valid to be triggered. If `commit_timer` returns `Ok`, then
    /// `TimerDispatcher` will "forget" about the timer identifier referenced
    /// by `id`, meaning subsequent calls to [`cancel_timer`] or
    /// [`schedule_timer`] will return `Err`.
    ///
    /// [`cancel_timer`]: TimerDispatcher::cancel_timer
    /// [`schedule_timer`]: TimerDispatcher::schedule_timer
    fn commit_timer(&self, id: InternalId) -> Result<T, InternalId> {
        let Self { inner } = self;
        let mut inner = inner.write();
        let TimerDispatcherInner { timer_ids, timers, next_id: _, futures_sender: _ } =
            inner.deref_mut();
        let external_id = timer_ids.remove(&id).ok_or(id)?;
        let info = timers.remove(&external_id).ok_or(id)?;
        assert_eq!(info.id, id);
        Ok(external_id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bindings::integration_tests::set_logger_for_test;
    use assert_matches::assert_matches;
    use fuchsia_zircon::{self as zx, DurationNum};
    use futures::{channel::mpsc, task::Poll, Future, StreamExt};
    use std::sync::Arc;

    type TestDispatcher = TimerDispatcher<usize>;

    #[derive(Clone)]
    struct TestContext {
        dispatcher: Arc<TestDispatcher>,
        fired: mpsc::UnboundedSender<usize>,
    }

    impl TimerHandler<usize> for TestContext {
        fn handle_expired_timer(&mut self, timer: usize) {
            self.fired.unbounded_send(timer).expect("Can't fire timer")
        }
        fn get_timer_dispatcher(&mut self) -> &TimerDispatcher<usize> {
            &self.dispatcher
        }
    }

    impl TestContext {
        fn new() -> (Self, mpsc::UnboundedReceiver<usize>) {
            let (fired, receiver) = mpsc::unbounded();
            let this = TestContext { dispatcher: TestDispatcher::default().into(), fired };
            this.dispatcher.spawn(this.clone());
            (this, receiver)
        }

        fn with_disp_sync<R, F: FnOnce(&TestDispatcher) -> R>(&self, f: F) -> R {
            f(&self.dispatcher)
        }
    }

    impl TimerContext<usize> for TestContext {
        type Handler = Self;
        fn handler(&self) -> Self {
            self.clone()
        }
    }

    fn nanos_from_now(nanos: i64) -> StackTime {
        StackTime(fasync::Time::after(zx::Duration::from_nanos(nanos)))
    }

    fn run_in_executor<R, Fut: Future<Output = R>>(
        executor: &mut fasync::TestExecutor,
        f: Fut,
    ) -> R {
        futures::pin_mut!(f);
        loop {
            executor.wake_main_future();
            match executor.run_one_step(&mut f) {
                Some(Poll::Ready(x)) => break x,
                None => panic!("Executor stalled"),
                Some(Poll::Pending) => (),
            }
        }
    }

    #[test]
    fn test_timers_fire() {
        set_logger_for_test();
        let mut executor = fasync::TestExecutor::new_with_fake_time();

        let (t, mut fired) = TestContext::new();
        run_in_executor(&mut executor, async {
            let d = t.handler();
            assert_eq!(d.dispatcher.schedule_timer(1, nanos_from_now(1)), None);
            assert_eq!(d.dispatcher.schedule_timer(2, nanos_from_now(2)), None);
        });
        assert_matches!(fired.try_next(), Err(mpsc::TryRecvError { .. }));
        executor.set_fake_time(fasync::Time::after(1.nanos()));
        assert_eq!(run_in_executor(&mut executor, fired.next()).unwrap(), 1);
        assert_matches!(fired.try_next(), Err(mpsc::TryRecvError { .. }));
        executor.set_fake_time(fasync::Time::after(1.nanos()));
        assert_eq!(run_in_executor(&mut executor, fired.next()).unwrap(), 2);
    }

    #[test]
    fn test_get_scheduled_instant() {
        set_logger_for_test();
        let mut _executor = fasync::TestExecutor::new_with_fake_time();
        let (t, _) = TestContext::new();

        let d = &t.dispatcher;

        // Timer 1 is scheduled.
        let time1 = nanos_from_now(1);
        assert_eq!(d.schedule_timer(1, time1), None);
        assert_eq!(d.scheduled_time(&1).unwrap(), time1);

        // Timer 2 does not exist yet.
        assert_eq!(d.scheduled_time(&2), None);

        // Timer 1 is scheduled.
        let time2 = nanos_from_now(2);
        assert_eq!(d.schedule_timer(2, time2), None);
        assert_eq!(d.scheduled_time(&1).unwrap(), time1);
        assert_eq!(d.scheduled_time(&2).unwrap(), time2);

        // Cancel Timer 1.
        assert_eq!(d.cancel_timer(&1).unwrap(), time1);
        assert_eq!(d.scheduled_time(&1), None);

        // Timer 2 should still be scheduled.
        assert_eq!(d.scheduled_time(&2).unwrap(), time2);
    }

    #[test]
    fn test_cancel() {
        set_logger_for_test();
        let mut executor = fasync::TestExecutor::new_with_fake_time();
        let (t, mut rcv) = TestContext::new();

        // timer 1 and 2 are scheduled.
        // timer 1 is going to be cancelled even before we allow the loop to
        // run.

        let time1 = nanos_from_now(1);
        let time2 = nanos_from_now(2);
        let time3 = nanos_from_now(5);
        t.with_disp_sync(|d| {
            assert_eq!(d.schedule_timer(1, time1), None);
            assert_eq!(d.schedule_timer(2, time2), None);

            assert_eq!(d.cancel_timer(&1).unwrap(), time1);
        });
        executor.set_fake_time(time2.0);
        let r = run_in_executor(&mut executor, rcv.next()).unwrap();

        t.with_disp_sync(|d| {
            // can't cancel 2 anymore, it has already fired
            assert_eq!(d.cancel_timer(&2), None);
        });
        // only event 2 should come out because 1 was cancelled:
        assert_eq!(r, 2);

        // schedule another timer and wait for it, just to prove that timer 1's
        // event never gets fired:
        t.with_disp_sync(|d| {
            assert_eq!(d.schedule_timer(3, time3), None);
        });
        executor.set_fake_time(time3.0);
        let r = run_in_executor(&mut executor, rcv.next()).unwrap();
        assert_eq!(r, 3);
    }

    #[test]
    fn test_reschedule() {
        set_logger_for_test();
        let mut executor = fasync::TestExecutor::new_with_fake_time();
        let (t, mut rcv) = TestContext::new();

        // timer 1 and 2 are scheduled.
        // timer 1 is going to be rescheduled even before we allow the loop to
        // run.
        let time1 = nanos_from_now(1);
        let time2 = nanos_from_now(2);
        let resched1 = nanos_from_now(3);
        let resched2 = nanos_from_now(4);

        t.with_disp_sync(|d| {
            assert_eq!(d.schedule_timer(1, time1), None);
            assert_eq!(d.schedule_timer(2, time2), None);
            assert_eq!(d.schedule_timer(1, resched1).unwrap(), time1);
        });
        executor.set_fake_time(time2.0);
        let r = run_in_executor(&mut executor, rcv.next()).unwrap();
        // only event 2 should come out:
        assert_eq!(r, 2);

        t.with_disp_sync(|d| {
            // we can schedule timer 2 again, and it returns None because it has
            // already fired.
            assert_eq!(d.schedule_timer(2, resched2), None);
        });

        // now we can go at it again and get the rescheduled timers:
        executor.set_fake_time(resched2.0);
        assert_eq!(run_in_executor(&mut executor, rcv.next()).unwrap(), 1);
        assert_eq!(run_in_executor(&mut executor, rcv.next()).unwrap(), 2);
    }

    #[test]
    fn test_cancel_with() {
        set_logger_for_test();
        let mut executor = fasync::TestExecutor::new_with_fake_time();
        let (t, mut rcv) = TestContext::new();

        t.with_disp_sync(|d| {
            // schedule 4 timers:
            assert_eq!(d.schedule_timer(1, nanos_from_now(1)), None);
            assert_eq!(d.schedule_timer(2, nanos_from_now(2)), None);
            assert_eq!(d.schedule_timer(3, nanos_from_now(3)), None);
            assert_eq!(d.schedule_timer(4, nanos_from_now(4)), None);

            // cancel timers 1, 3, and 4.
            d.cancel_timers_with(|id| *id != 2);
            // check that only one timer remains
            assert_eq!(d.inner.read().timers.len(), 1);
        });
        // advance time so that all timers would've been fired.
        executor.set_fake_time(nanos_from_now(4).0);
        // get the timer and assert that it is the timer with id == 2.
        let r = run_in_executor(&mut executor, rcv.next()).unwrap();
        assert_eq!(r, 2);
    }
}
