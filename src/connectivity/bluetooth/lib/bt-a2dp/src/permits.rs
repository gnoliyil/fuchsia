// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A collection which hands out Permits, of which a limited number of are allowed exist at once.
//! Permits can be granted immediately, or can be reserved in first-come-first-serve order, with a
//! Future which resolves to a Permit once one becomes available.
//!
//! Permits are released upon drop and will be automatically handed off to the next reservation or
//! returned to the available pool if no one is waiting.
//!
//! Permits can be revokable by providing a function which will return a valid Permit when called.
//! That function can also make a reservation for a future Permit if desired. (see examples in tests)
//!
//! A client can take a permit (revoking one if necessary), by using `Permits::take`.
//! A client can also seize all permits, taking out all permits left unclaimed and revoking all
//! revokable permits.
//!
//! Permits taken or seized are not revokable.

use anyhow::{format_err, Error};
use futures::{
    channel::oneshot,
    future::FusedFuture,
    ready,
    task::{Context, Poll},
    Future, FutureExt,
};
use parking_lot::Mutex;
use slab::Slab;
use std::collections::VecDeque;
use std::pin::Pin;
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Weak,
};

type BoxRevokeFn = Box<dyn FnOnce() -> Permit + Send>;

struct RevokeFnHolder {
    f: Mutex<Option<BoxRevokeFn>>,
    label: Mutex<String>,
}

impl RevokeFnHolder {
    fn new(f: Option<BoxRevokeFn>) -> Arc<Self> {
        Arc::new(Self { f: Mutex::new(f), label: Mutex::new(String::default()) })
    }

    // Replaces the revokable function within, returning the previously stored fn, if there was one.
    fn replace(&self, f: BoxRevokeFn) -> Option<BoxRevokeFn> {
        self.f.lock().replace(f)
    }

    // Replaces the label
    fn relabel(&self, label: String) {
        *(self.label.lock()) = label;
    }

    fn label(&self) -> String {
        self.label.lock().clone()
    }

    fn take(&self) -> Option<BoxRevokeFn> {
        self.f.lock().take()
    }

    fn is_revokable(&self) -> bool {
        self.f.lock().is_some()
    }

    fn extract(weak: &Weak<Self>) -> BoxRevokeFn {
        weak.upgrade().expect("should be resolvable").take().expect("revokable fn missing")
    }
}

impl std::fmt::Debug for RevokeFnHolder {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RevokeFnHolder")
            .field("revokable", &self.is_revokable())
            .field("label", &self.label())
            .finish()
    }
}

struct WaitingReservation {
    sender: futures::channel::oneshot::Sender<Permit>,
}

struct PermitsInner {
    // The maximum number of permits allowed.
    limit: usize,
    // The current permits out. Permits are indexed by their key.
    // If the permit is out, then Weak::upgrade() will return Some.
    out: Slab<Weak<RevokeFnHolder>>,
    // A queue of oneshot senders who are waiting for permits.
    waiting: VecDeque<WaitingReservation>,
    // An ordered queue of indexes into `out` which are revokable.
    // If a permit index is listed here, the Weak at out.get(i) should be upgradable.
    revocations: VecDeque<usize>,
    weak: Weak<Mutex<Self>>,
}

impl std::fmt::Debug for PermitsInner {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let mut debug = f.debug_struct("PermitsInner");
        let _ = debug.field("limit", &self.limit).field("waiting", &self.waiting.len());
        for (k, holder) in &self.out {
            let h = holder.upgrade().unwrap();
            let holder_str = format!("{}: {}", if h.is_revokable() { "R" } else { "I" }, h.label());
            let _ = debug.field(format!("permit{k}").as_str(), &holder_str);
        }
        debug.finish()
    }
}

impl PermitsInner {
    fn new(limit: usize) -> Arc<Mutex<Self>> {
        Arc::new_cyclic(|weak| {
            Mutex::new(Self {
                limit,
                out: Slab::with_capacity(limit),
                waiting: VecDeque::new(),
                revocations: VecDeque::new(),
                weak: weak.clone(),
            })
        })
    }

    // Try to reserve a key in the permits. If `revoke_fn` is Some then this permit is
    // revokable. Returns the permit, or an Error if there are no permits available.
    fn try_get(&mut self, revoke_fn: Option<BoxRevokeFn>) -> Result<Permit, Error> {
        if self.out.len() == self.out.capacity() {
            return Err(format_err!("No permits left"));
        }
        let is_revokable = revoke_fn.is_some();
        let fn_holder = RevokeFnHolder::new(revoke_fn);
        let key = self.out.insert(Arc::downgrade(&fn_holder));
        if is_revokable {
            self.revocations.push_back(key);
        }
        Ok(Permit {
            inner: Some(self.weak.upgrade().unwrap()),
            committed: Arc::new(AtomicBool::new(true)),
            fn_holder,
            key,
        })
    }

    // Release a permit that is out. Permits call this function when they are dropped
    // to hand off their permit to the next waiting reservation.
    // `key` is the key of the permit being released.
    //
    // Panics: if `key` is not currently out.
    fn release(&mut self, key: usize) {
        // Not revoked, so drop tracking a possible revocation.
        self.revocations.retain(|k| *k != key);
        // Holder should be resolvable as it's held by the releasing Permit.
        let holder = self.out.get(key).expect("reservation present").upgrade().unwrap();
        // Drop the revoke fn if it's present.
        drop(holder.take());
        let this = self.weak.upgrade().unwrap();
        while let Some(sender) = self.waiting.pop_front() {
            if let Ok(()) = Permit::handoff(sender, this.clone(), holder.clone(), key) {
                return;
            }
        }
        // No permits were handed off, so this one gets turned in.
        drop(self.out.remove(key));
    }

    // Create a Reservation future that will complete with a Permit when one becomes available.
    fn reservation(&mut self, revoke_fn: Option<BoxRevokeFn>) -> Reservation {
        let (sender, receiver) = oneshot::channel();
        // If we can get a permit immediately, send it right away.
        match self.try_get(None).ok() {
            Some(permit) => sender.send(permit).ok().unwrap(),
            None => self.waiting.push_back(WaitingReservation { sender }),
        }
        Reservation { receiver, revoke_fn, inner: self.weak.clone() }
    }

    /// Make a previously unrevokable permit revokable by supplying a function to revoke it.
    fn make_revokable(&mut self, key: usize, revoke_fn: BoxRevokeFn) {
        let prev = self
            .out
            .get(key)
            .expect("reservation should be out")
            .upgrade()
            .expect("holder should resolve")
            .replace(revoke_fn);
        assert!(prev.is_none(), "shouldn't be replacing a previous revocation function");
        self.revocations.push_back(key);
    }

    /// Get the next revokable permit function.
    fn pop_revoke(&mut self) -> Option<BoxRevokeFn> {
        self.revocations.pop_front().map(|idx| RevokeFnHolder::extract(&self.out[idx]))
    }

    /// Empty the queue of revokable permit functions, returning them all for revocation.
    fn revoke_all(&mut self) -> Vec<BoxRevokeFn> {
        let mut indices = std::mem::take(&mut self.revocations);
        indices.drain(..).map(|idx| RevokeFnHolder::extract(&self.out[idx])).collect()
    }
}

/// A Reservation is a future that will eventually receive a permit once one becomes available.
pub struct Reservation {
    // Receiver for the Permit when it is granted.
    receiver: oneshot::Receiver<Permit>,
    // The revocation function if this reservation will result in a revokable permit
    revoke_fn: Option<BoxRevokeFn>,
    // Pointer to the shared permits if it still exists.
    inner: Weak<Mutex<PermitsInner>>,
}

impl Future for Reservation {
    type Output = Permit;
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let res = ready!(self.receiver.poll_unpin(cx));
        let permit = res.expect("sender shouldn't be dropped, polled after termination?");
        if let (Some(f), Some(inner)) = (self.revoke_fn.take(), self.inner.upgrade()) {
            inner.lock().make_revokable(permit.key, f);
        }
        Poll::Ready(permit)
    }
}

impl FusedFuture for Reservation {
    fn is_terminated(&self) -> bool {
        self.receiver.is_terminated()
    }
}

#[derive(Debug, Clone)]
pub struct Permits {
    inner: Arc<Mutex<PermitsInner>>,
    limit: usize,
}

impl Permits {
    /// Make a new set of permits with `limit` maximum concurrent permits available.
    pub fn new(limit: usize) -> Self {
        Self { inner: PermitsInner::new(limit), limit }
    }

    /// Returns the maximum number of permits allowed.
    pub fn limit(&self) -> usize {
        self.limit
    }

    /// Attempts to get a permit. Returns None if there are no permits available.
    pub fn get(&self) -> Option<Permit> {
        Permit::try_issue(self.inner.clone(), None)
    }

    /// Attempts to get a permit that is revokable.  Revokable permits can be revoked at any time
    /// after this function returns and must return the Permit when asked. `revoked_fn` will be
    /// called to retrieve the permit when it is revoked.
    pub fn get_revokable(
        &self,
        revoked_fn: impl FnOnce() -> Permit + 'static + Send,
    ) -> Option<Permit> {
        Permit::try_issue(self.inner.clone(), Some(Box::new(revoked_fn)))
    }

    /// Attempts to get a permit. If a permit isn't available, but one can be revoked, one will
    /// be revoked to return a permit.  Revoked permits are returned here before reservations are
    /// filled.
    pub fn take(&self) -> Option<Permit> {
        if let Some(permit) = self.get() {
            return Some(permit);
        }
        let revoke_fn = self.inner.lock().pop_revoke();
        revoke_fn.map(|f| f())
    }

    /// Attempts to reserve all permits, including revoking any permits to do so.
    /// Permits that are seized are prioritized over any reservations.
    pub fn seize(&self) -> Vec<Permit> {
        let mut bunch = Vec::new();
        let mut revoke_fns = {
            let mut lock = self.inner.lock();
            // First get all the unclaimed permits.
            loop {
                match lock.try_get(None).ok() {
                    Some(permit) => bunch.push(permit),
                    None => break,
                }
            }
            lock.revoke_all()
        };
        for f in revoke_fns.drain(..) {
            bunch.push(f())
        }
        bunch
    }

    /// Reserve a spot in line to receive a permit once one becomes available.
    /// Returns a future that resolves to a permit
    /// Reservations are first-come-first-serve, but permits that are revoked ignore reservations.
    pub fn reserve(&self) -> Reservation {
        self.inner.lock().reservation(None)
    }

    /// Reserve a spot in line to receive a permit once one becomes available.
    /// Returns a future that resovles to a revokable permit.
    /// Once the permit has been returned from the Reservation, it can be revoked at any time
    /// afterwards.
    pub fn reserve_revokable(
        &self,
        revoked_fn: impl FnOnce() -> Permit + 'static + Send,
    ) -> Reservation {
        self.inner.lock().reservation(Some(Box::new(revoked_fn)))
    }
}

#[derive(Debug)]
pub struct Permit {
    // The shared permits
    inner: Option<Arc<Mutex<PermitsInner>>>,
    committed: Arc<AtomicBool>,
    fn_holder: Arc<RevokeFnHolder>,
    key: usize,
}

impl Permit {
    /// Relabels this permit, making it easier to track in Debug
    pub fn relabel(&self, new_label: String) {
        self.fn_holder.relabel(new_label);
    }

    /// Issues a permit using the given `inner`. Returns none if there are no permits available or
    /// in the case of lock contention.
    fn try_issue(inner: Arc<Mutex<PermitsInner>>, revoke_fn: Option<BoxRevokeFn>) -> Option<Self> {
        inner.lock().try_get(revoke_fn).ok()
    }

    // Tries to hand off a permit to a reservation. This creates a new Permit and makes sure it
    // is in the channel before it returns. The permit is then "real", and guarantees it will
    // release itself when dropped.
    fn handoff(
        waiting: WaitingReservation,
        inner: Arc<Mutex<PermitsInner>>,
        fn_holder: Arc<RevokeFnHolder>,
        key: usize,
    ) -> Result<(), Error> {
        let committed = Arc::new(AtomicBool::new(false));
        let commit_clone = committed.clone();
        let potential = Self { inner: Some(inner), committed, key, fn_holder };
        match waiting.sender.send(potential) {
            Ok(()) => {
                commit_clone.store(true, Ordering::Relaxed);
                Ok(())
            }
            Err(_) => Err(format_err!("failed to handoff")),
        }
    }
}

impl Drop for Permit {
    fn drop(&mut self) {
        let inner = match self.inner.take() {
            None => return, // Dropped twice, nothing to do.
            Some(inner) => inner,
        };
        let committed = self.committed.load(Ordering::Relaxed);
        if committed {
            inner.lock().release(self.key);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use async_utils::PollExt;
    use fuchsia_async as fasync;

    #[track_caller]
    fn expect_none<T>(opt: Option<T>, msg: &str) {
        if let Some(_) = opt {
            panic!("{}", msg);
        }
    }

    #[track_caller]
    fn expect_no_permits(exec: &mut fasync::TestExecutor, reservation: &mut Reservation) {
        exec.run_until_stalled(reservation)
            .expect_pending("expected reservation to have no permits");
    }

    #[track_caller]
    fn expect_permit_available(
        exec: &mut fasync::TestExecutor,
        reservation: &mut Reservation,
    ) -> Permit {
        exec.run_until_stalled(reservation).expect("reservation to have available permit")
    }

    #[test]
    fn no_permits_available() {
        // Not super useful...
        let permits = Permits::new(0);

        expect_none(permits.get(), "shouldn't be able to get a permit");

        // Can still get a reservation, that will never complete.
        let _reservation = permits.reserve();
    }

    #[test]
    fn permit_dropping() {
        // Two permits allowed.
        let permits = Permits::new(2);

        assert_eq!(2, permits.limit());

        let one = permits.get().expect("first permit");
        let two = permits.get().expect("second permit");
        expect_none(permits.get(), "shouln't get a third permit");

        drop(two);

        let three = permits.get().expect("third permit");
        drop(one);
        let four = permits.get().expect("fourth permit");

        drop(three);
        drop(four);

        let _five = permits.get().expect("fifth permit");
    }

    #[test]
    fn permit_reservations() {
        let mut exec = fasync::TestExecutor::new();

        // Two permits allowed.
        let permits = Permits::new(2);

        let one = permits.get().expect("permit one should be available");
        let two = permits.get().expect("second permit is also okay");
        expect_none(permits.get(), "can't get a third item");

        let mut first = permits.reserve();
        let second = permits.reserve();
        let mut third = permits.reserve();
        let mut fourth = permits.reserve();

        // We should be able to drop any of these reservations before they become a Permit
        drop(second);

        expect_no_permits(&mut exec, &mut first);
        expect_no_permits(&mut exec, &mut third);
        expect_no_permits(&mut exec, &mut fourth);

        drop(one);

        let first_out = expect_permit_available(&mut exec, &mut first);
        expect_no_permits(&mut exec, &mut third);
        expect_no_permits(&mut exec, &mut fourth);

        drop(first_out);

        let third_out = expect_permit_available(&mut exec, &mut third);
        expect_no_permits(&mut exec, &mut fourth);

        drop(fourth);

        drop(two);

        // There should be one available now. Let's get it through a reservation.
        let mut fifth = permits.reserve();

        // Permits out: third_out (a permit) and fifth (a reservation which has a permit waiting to be retrieved).

        // Even though we haven't polled the reservation yet, we can't get another one (fifth has it)
        expect_none(permits.get(), "no items should be available");

        // Let's get two last reservations, which won't be filled yet.
        let sixth = permits.reserve();
        let mut seventh = permits.reserve();
        let _eighth = permits.reserve();

        // We can drop the Permits structure at any time, the permits and reservations that are out.
        // will work fine (we just won't be able to make any more)
        drop(permits);

        let _fifth_out = expect_permit_available(&mut exec, &mut fifth);

        drop(third_out);

        drop(sixth);

        let _seventh_out = expect_permit_available(&mut exec, &mut seventh);
    }

    #[test]
    fn revoke_permits() {
        const TOTAL_PERMITS: usize = 2;
        let permits = Permits::new(TOTAL_PERMITS);

        let permit_holder = Arc::new(Mutex::new(None));

        let revoke_from_holder_fn = {
            let holder = permit_holder.clone();
            move || holder.lock().take().expect("should be holding Permit")
        };

        let revokable_permit =
            permits.get_revokable(revoke_from_holder_fn.clone()).expect("permit available");
        *permit_holder.lock() = Some(revokable_permit);

        let seized_permits = permits.seize();

        // We have two permits.
        assert_eq!(TOTAL_PERMITS, seized_permits.len());
        // The permit has been revoked
        assert!(permit_holder.lock().is_none());

        // Drop the permits
        drop(seized_permits);

        // Should be able to take a permit when one is just available.
        let _nonrevokable_permit = permits.take().expect("permit available");
        let revokable_permit =
            permits.get_revokable(revoke_from_holder_fn.clone()).expect("two permits");
        *permit_holder.lock() = Some(revokable_permit);

        // Seizing all the (remaining) permits doesn't get the non-revokable one.
        let seized_permits = permits.seize();
        assert_eq!(1, seized_permits.len());
        // The permit has been revoked
        assert!(permit_holder.lock().is_none());

        drop(seized_permits);

        let revokable_permit = permits.get_revokable(revoke_from_holder_fn).expect("permit");
        *permit_holder.lock() = Some(revokable_permit);

        // Can take the permit from the revokable one.
        let _taken = permits.take().expect("can take the permit");
        assert!(permit_holder.lock().is_none());

        // Can't take a permit if none are available.
        assert!(permits.take().is_none());
    }

    #[test]
    fn revokable_dropped_before_revokation() {
        const TOTAL_PERMITS: usize = 2;
        let permits = Permits::new(TOTAL_PERMITS);

        let permit_holder = Arc::new(Mutex::new(None));

        let revoke_from_holder_fn = {
            let holder = permit_holder.clone();
            move || holder.lock().take().expect("should be holding Permit when revoked")
        };

        let revokable_permit =
            permits.get_revokable(revoke_from_holder_fn).expect("permit available");
        // Drop it before we have a chance to revoke it.
        drop(revokable_permit);

        let seized_permits = permits.seize();
        // We have both permits.
        assert_eq!(TOTAL_PERMITS, seized_permits.len());
    }

    // It's turtles all the way down.
    // This function revokes a permit by taking it from the permits_holder
    // Then makes a reservation using itself as the revocation function.
    // Putting the reservation into reservations_holder.
    fn revoke_then_reserve_again(
        permits: Permits,
        holder: Arc<Mutex<Vec<Permit>>>,
        reservations: Arc<Mutex<Vec<Reservation>>>,
    ) -> Permit {
        let permit = holder.lock().pop().expect("should have a permit");
        let recurse_fn = {
            let permits = permits.clone();
            let reservations = reservations.clone();
            move || revoke_then_reserve_again(permits, holder, reservations)
        };
        let reservation = permits.reserve_revokable(recurse_fn);
        reservations.lock().push(reservation);
        permit
    }

    #[fuchsia::test]
    fn revokable_reservations() {
        let mut exec = fasync::TestExecutor::new();
        const TOTAL_PERMITS: usize = 2;
        let permits = Permits::new(TOTAL_PERMITS);

        let permits_holder = Arc::new(Mutex::new(Vec::new()));
        let reservations_holder = Arc::new(Mutex::new(Vec::new()));

        let revoke_from_holder_fn = {
            let holder = permits_holder.clone();
            move || holder.lock().pop().expect("should have a Permit")
        };

        let revokable =
            permits.get_revokable(revoke_from_holder_fn.clone()).expect("got revokable");
        permits_holder.lock().push(revokable);

        let revoke_then_reserve_fn = {
            let permits = permits.clone();
            let holder = permits_holder.clone();
            let reservations = reservations_holder.clone();
            move || revoke_then_reserve_again(permits, holder, reservations)
        };

        let mut revokable_reservation = permits.reserve_revokable(revoke_then_reserve_fn);
        let revokable_permit = expect_permit_available(&mut exec, &mut revokable_reservation);
        permits_holder.lock().push(revokable_permit);

        let seized_permits = permits.seize();
        // We have both permits.
        assert_eq!(TOTAL_PERMITS, seized_permits.len());
        assert_eq!(0, permits_holder.lock().len());

        // But! also a reservation.
        let mut another_reservation = reservations_holder.lock().pop().expect("reservation");
        // This one won't get us another reservation, but is still revokable.
        let mut revokable_reservation_two = permits.reserve_revokable(revoke_from_holder_fn);

        // Dropping both seized permits will deliver both reservations.
        drop(seized_permits);

        let revokable_permit = expect_permit_available(&mut exec, &mut another_reservation);
        permits_holder.lock().push(revokable_permit);
        let revokable_permit = expect_permit_available(&mut exec, &mut revokable_reservation_two);
        permits_holder.lock().push(revokable_permit);

        // We can seize both of these again! Hah!
        let seized_permits = permits.seize();
        // We have both permits.
        assert_eq!(TOTAL_PERMITS, seized_permits.len());
        assert_eq!(0, permits_holder.lock().len());
        // But we have yet another reservation (from the recycling one)
        let mut yet_another = reservations_holder.lock().pop().expect("reservation");
        expect_no_permits(&mut exec, &mut yet_another);

        // If we drop both seized permits..
        drop(seized_permits);

        // We can get one permit (this time unseizable)
        let one = permits.get().expect("one is available");

        // But not a second one, since the reservation has it.
        expect_none(permits.get(), "none should be available");

        let revokable_permit = expect_permit_available(&mut exec, &mut yet_another);
        permits_holder.lock().push(revokable_permit);

        // Seizing now will only seize the revokable one.
        let seized_permits = permits.seize();
        assert_eq!(1, seized_permits.len());
        assert_eq!(0, permits_holder.lock().len());

        // We still get another reservation (from the recycling one)
        let mut yet_another = reservations_holder.lock().pop().expect("reservation");
        expect_no_permits(&mut exec, &mut yet_another);

        // Dropping the unrevokable one will fulfill the revokable reservation.
        drop(one);

        let revokable_permit = expect_permit_available(&mut exec, &mut yet_another);
        permits_holder.lock().push(revokable_permit);

        // And we can take that one away too.
        let taken_permit = permits.take().expect("should be able to take one");

        // Drop so that ASAN is happy.
        drop(taken_permit);
        // Need to empty the permits holder before dropping it, otherwise the permits
        // inside will hold a reference loop (the permits hold the revocation function which hold
        // a ref to the holder)
        permits_holder.lock().clear();
        drop(permits_holder);
        // Same for the reservations.
        reservations_holder.lock().clear();
        drop(reservations_holder);
        drop(permits);
    }
}
