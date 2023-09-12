// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Synchronized reference counting primitives.
//!
//! This module introduces a family of reference counted types that allows
//! marking the underlying data for destruction before all strongly references
//! to the data are dropped. This enables the following features:
//!   * Upgrading a weak reference to a strong reference succeeds iff at least
//!     one strong reference exists _and_ the data has not been marked for
//!     destruction.
//!   * Allow waiting for all strongly-held references to be dropped after
//!     marking the data. (TODO: https://fxbug.dev/122388).

use core::{
    convert::AsRef,
    hash::{Hash, Hasher},
    ops::Deref,
    panic::Location,
    sync::atomic::{AtomicBool, Ordering},
};

use derivative::Derivative;

mod caller {
    //! Provides tracking of instances via tracked caller location.
    //!
    //! Callers are only tracked in debug builds. All operations and types
    //! are no-ops and empty unless the `rc-debug-names` feature is enabled.

    use core::panic::Location;

    /// Records reference-counted names of instances.
    #[derive(Default)]
    pub(super) struct Callers {
        /// The names that were inserted and aren't known to be gone.
        ///
        /// This holds weak references to allow callers to drop without
        /// synchronizing. Invalid weak pointers are cleaned up periodically but
        /// are not logically present.
        #[cfg(feature = "rc-debug-names")]
        pub(super) callers: crate::Mutex<std::collections::HashMap<Location<'static>, usize>>,
    }

    impl core::fmt::Debug for Callers {
        #[cfg(not(feature = "rc-debug-names"))]
        fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
            write!(f, "(Not Tracked)")
        }
        #[cfg(feature = "rc-debug-names")]
        fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
            let Self { callers } = self;
            let callers = callers.lock();
            write!(f, "[\n")?;
            for (l, c) in callers.iter() {
                write!(f, "   {l} => {c},\n")?;
            }
            write!(f, "]")
        }
    }

    impl Callers {
        /// Creates a new [`Callers`] from the given [`Location`].
        ///
        /// On non-debug builds, this is a no-op.
        pub(super) fn insert(&self, caller: &Location<'static>) -> TrackedCaller {
            #[cfg(not(feature = "rc-debug-names"))]
            {
                let _ = caller;
                TrackedCaller {}
            }
            #[cfg(feature = "rc-debug-names")]
            {
                let Self { callers } = self;
                let mut callers = callers.lock();
                let count = callers.entry(caller.clone()).or_insert(0);
                *count += 1;
                TrackedCaller { location: caller.clone() }
            }
        }
    }

    #[derive(Debug)]
    pub(super) struct TrackedCaller {
        #[cfg(feature = "rc-debug-names")]
        pub(super) location: Location<'static>,
    }

    impl TrackedCaller {
        #[cfg(not(feature = "rc-debug-names"))]
        pub(super) fn release(&mut self, Callers {}: &Callers) {
            let Self {} = self;
        }

        #[cfg(feature = "rc-debug-names")]
        pub(super) fn release(&mut self, Callers { callers }: &Callers) {
            let Self { location } = self;
            let mut callers = callers.lock();
            let mut entry = match callers.entry(location.clone()) {
                std::collections::hash_map::Entry::Vacant(_) => {
                    panic!("location {location:?} was not in the callers map")
                }
                std::collections::hash_map::Entry::Occupied(o) => o,
            };

            let sub = entry
                .get()
                .checked_sub(1)
                .unwrap_or_else(|| panic!("zero-count location {location:?} in map"));
            if sub == 0 {
                let _: usize = entry.remove();
            } else {
                *entry.get_mut() = sub;
            }
        }
    }
}

#[derive(Debug)]
struct Inner<T> {
    marked_for_destruction: AtomicBool,
    callers: caller::Callers,
    data: core::mem::ManuallyDrop<T>,
}

impl<T> Inner<T> {
    fn pre_drop_check(marked_for_destruction: &AtomicBool) {
        // `Ordering::Acquire` because we want to synchronize with with the
        // `Ordering::Release` write to `marked_for_destruction` so that all
        // memory writes before the reference was marked for destruction is
        // visible here.
        assert!(marked_for_destruction.load(Ordering::Acquire), "Must be marked for destruction");
    }

    fn unwrap(mut self) -> T {
        // We cannot destructure `self` by value since `Inner` implements
        // `Drop`. So we must manually drop all the fields but data and then
        // forget self.
        let Inner { marked_for_destruction, data, callers: holders } = &mut self;

        // Make sure that `inner` is in a valid state for destruction.
        //
        // Note that we do not actually destroy all of `self` here; we decompose
        // it into its parts, keeping what we need & throwing away what we
        // don't. Regardless, we perform the same checks.
        Inner::<T>::pre_drop_check(marked_for_destruction);

        // SAFETY: Safe since we own `self` and `self` is immediately forgotten
        // below so the its destructor (and those of its fields) will not be run
        // as a result of `self` being dropped.
        let data = unsafe {
            // Explicitly drop since we do not need these anymore.
            core::ptr::drop_in_place(marked_for_destruction);
            core::ptr::drop_in_place(holders);

            core::mem::ManuallyDrop::take(data)
        };
        // Forget self now to prevent its `Drop::drop` impl from being run which
        // will attempt to destroy `data` but still perform pre-drop checks on
        // `Inner`'s state.
        core::mem::forget(self);

        data
    }
}

impl<T> Drop for Inner<T> {
    fn drop(&mut self) {
        let Inner { marked_for_destruction, data, callers: _ } = self;
        // Take data out of ManuallyDrop in case we panic in pre_drop_check.
        // That'll ensure data is dropped if we hit the panic.
        //
        //  SAFETY: Safe because ManuallyDrop is not referenced again after
        // taking.
        let data = unsafe { core::mem::ManuallyDrop::take(data) };
        Self::pre_drop_check(marked_for_destruction);
        // TODO(https://fxbug.dev/122388): Instead of manually dropping here
        // we'll send data over a notifier to allow awaiting for all strong refs
        // to drop
        core::mem::drop(data);
    }
}

/// A primary reference.
///
/// Note that only one `Primary` may be associated with data. This is
/// enforced by not implementing [`Clone`].
///
/// For now, this reference is no different than a [`Strong`] but later changes
/// will enable blocking the destruction of a primary reference until all
/// strongly held references are dropped.
// TODO(https://fxbug.dev/122388): Implement the blocking.
#[derive(Debug)]
pub struct Primary<T> {
    inner: core::mem::ManuallyDrop<alloc::sync::Arc<Inner<T>>>,
}

impl<T> Drop for Primary<T> {
    fn drop(&mut self) {
        let was_marked = self.mark_for_destruction();
        let Self { inner } = self;
        // Take the inner out of ManuallyDrop early so its Drop impl will run in
        // case we panic here.
        // SAFETY: Safe because we don't reference ManuallyDrop again.
        let inner = unsafe { core::mem::ManuallyDrop::take(inner) };

        // Make debugging easier: don't panic if a panic is already happening
        // since double-panics are annoying to debug. This means that the
        // invariants provided by Primary are possibly violated during an
        // unwind, but we're sidestepping that problem because Fuchsia is our
        // only audience here.
        if !std::thread::panicking() {
            assert_eq!(was_marked, false, "Must not be marked for destruction yet");

            let Inner { marked_for_destruction: _, callers, data: _ } = &*inner;

            // Make sure that this `Primary` is the last thing to hold a strong
            // reference to the underlying data when it is being dropped.
            //
            // TODO(https://fxbug.dev/122388): Require explicit killing of the
            // reference and support blocking instead of this panic here.
            let refs = alloc::sync::Arc::strong_count(&inner).checked_sub(1).unwrap();
            assert!(
                refs == 0,
                "dropped Primary with {refs} strong refs remaining, \
                            Callers={callers:?}"
            );
        }
    }
}

impl<T> AsRef<T> for Primary<T> {
    fn as_ref(&self) -> &T {
        self.deref()
    }
}

impl<T> Deref for Primary<T> {
    type Target = T;

    fn deref(&self) -> &T {
        let Self { inner } = self;
        let Inner { marked_for_destruction: _, data, callers: _ } = &***inner;
        data
    }
}

impl<T> Primary<T> {
    // Marks this primary reference as ready for destruction. Used by all
    // dropping flows. We take &mut self here to ensure we have the only
    // possible reference to Primary. Returns whether it was already marked for
    // destruction.
    fn mark_for_destruction(&mut self) -> bool {
        let Self { inner } = self;
        // `Ordering::Release` because want to make sure that all memory writes
        // before dropping this `Primary` synchronizes with later attempts to
        // upgrade weak pointers and the `Drop::drop` impl of `Inner`.
        inner.marked_for_destruction.swap(true, Ordering::Release)
    }

    /// Returns a new strongly-held reference.
    pub fn new(data: T) -> Primary<T> {
        Primary {
            inner: core::mem::ManuallyDrop::new(alloc::sync::Arc::new(Inner {
                marked_for_destruction: AtomicBool::new(false),
                callers: caller::Callers::default(),
                data: core::mem::ManuallyDrop::new(data),
            })),
        }
    }

    /// Clones a strongly-held reference.
    #[cfg_attr(feature = "rc-debug-names", track_caller)]
    pub fn clone_strong(Self { inner }: &Self) -> Strong<T> {
        let Inner { data: _, callers, marked_for_destruction: _ } = &***inner;
        let caller = callers.insert(Location::caller());
        Strong { inner: alloc::sync::Arc::clone(inner), caller }
    }

    /// Returns a weak reference pointing to the same underlying data.
    pub fn downgrade(Self { inner }: &Self) -> Weak<T> {
        Weak(alloc::sync::Arc::downgrade(inner))
    }

    /// Returns true if the two pointers point to the same allocation.
    pub fn ptr_eq(
        Self { inner: this }: &Self,
        Strong { inner: other, caller: _ }: &Strong<T>,
    ) -> bool {
        alloc::sync::Arc::ptr_eq(this, other)
    }

    /// Returns the inner value if no [`Strong`] references are held.
    ///
    /// # Panics
    ///
    /// Panics if [`Strong`] references are held when this function is called.
    pub fn unwrap(mut this: Self) -> T {
        let inner = {
            // Prepare for destruction.
            assert!(!this.mark_for_destruction());
            let Self { inner } = &mut this;
            // SAFETY: Safe because inner can't be used after this. We forget
            // our Primary reference to prevent its Drop impl from running.
            let inner = unsafe { core::mem::ManuallyDrop::take(inner) };
            core::mem::forget(this);
            inner
        };

        match alloc::sync::Arc::try_unwrap(inner) {
            Ok(inner) => inner.unwrap(),
            Err(inner) => {
                let callers = &inner.callers;
                let refs = alloc::sync::Arc::strong_count(&inner).checked_sub(1).unwrap();
                panic!("can't unwrap, still had {refs} strong refs: {callers:?}");
            }
        }
    }
}

/// A strongly-held reference.
///
/// Similar to an [`alloc::sync::Arc`] but holding a `Strong` acts as a witness
/// to the live-ness of the underlying data. That is, holding a `Strong` implies
/// that the underlying data has not yet been destroyed.
///
/// Note that `Strong`'s implementation of [`Hash`] and [`PartialEq`] operate on
/// the pointer itself and not the underlying data.
#[derive(Debug, Derivative)]
pub struct Strong<T> {
    inner: alloc::sync::Arc<Inner<T>>,
    #[allow(dead_code)]
    caller: caller::TrackedCaller,
}

impl<T> Drop for Strong<T> {
    fn drop(&mut self) {
        let Self { inner, caller } = self;
        let Inner { marked_for_destruction: _, callers, data: _ } = &**inner;
        caller.release(callers);
    }
}

impl<T> AsRef<T> for Strong<T> {
    fn as_ref(&self) -> &T {
        self.deref()
    }
}

impl<T> Deref for Strong<T> {
    type Target = T;

    fn deref(&self) -> &T {
        let Self { inner, caller: _ } = self;
        let Inner { marked_for_destruction: _, data, callers: _ } = inner.deref();
        data
    }
}

impl<T> core::cmp::Eq for Strong<T> {}

impl<T> core::cmp::PartialEq for Strong<T> {
    fn eq(&self, other: &Self) -> bool {
        Self::ptr_eq(self, other)
    }
}

impl<T> Hash for Strong<T> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let Self { inner, caller: _ } = self;
        alloc::sync::Arc::as_ptr(inner).hash(state)
    }
}

impl<T> Clone for Strong<T> {
    #[cfg_attr(feature = "rc-debug-names", track_caller)]
    fn clone(&self) -> Self {
        let Self { inner, caller: _ } = self;
        let Inner { data: _, marked_for_destruction: _, callers } = &**inner;
        let caller = callers.insert(Location::caller());
        Self { inner: alloc::sync::Arc::clone(inner), caller }
    }
}

impl<T> Strong<T> {
    /// Returns a weak reference pointing to the same underlying data.
    pub fn downgrade(Self { inner, caller: _ }: &Self) -> Weak<T> {
        Weak(alloc::sync::Arc::downgrade(inner))
    }

    /// Returns true if the inner value has since been marked for destruction.
    pub fn marked_for_destruction(Self { inner, caller: _ }: &Self) -> bool {
        let Inner { marked_for_destruction, data: _, callers: _ } = inner.as_ref();
        // `Ordering::Acquire` because we want to synchronize with with the
        // `Ordering::Release` write to `marked_for_destruction` so that all
        // memory writes before the reference was marked for destruction is
        // visible here.
        marked_for_destruction.load(Ordering::Acquire)
    }

    /// Returns true if the two pointers point to the same allocation.
    pub fn weak_ptr_eq(Self { inner: this, caller: _ }: &Self, Weak(other): &Weak<T>) -> bool {
        core::ptr::eq(alloc::sync::Arc::as_ptr(this), other.as_ptr())
    }

    /// Returns true if the two pointers point to the same allocation.
    pub fn ptr_eq(
        Self { inner: this, caller: _ }: &Self,
        Self { inner: other, caller: _ }: &Self,
    ) -> bool {
        alloc::sync::Arc::ptr_eq(this, other)
    }

    /// Compares the two pointers.
    pub fn ptr_cmp(
        Self { inner: this, caller: _ }: &Self,
        Self { inner: other, caller: _ }: &Self,
    ) -> core::cmp::Ordering {
        let this = alloc::sync::Arc::as_ptr(this);
        let other = alloc::sync::Arc::as_ptr(other);
        this.cmp(&other)
    }
}

/// A weakly-held reference.
///
/// Similar to an [`alloc::sync::Weak`].
///
/// A `Weak` does not make any claim to the live-ness of the underlying data.
/// Holders of a `Weak` must attempt to upgrade to a [`Strong`] through
/// [`Weak::upgrade`] to access the underlying data.
///
/// Note that `Weak`'s implementation of [`Hash`] and [`PartialEq`] operate on
/// the pointer itself and not the underlying data.
#[derive(Debug, Derivative)]
pub struct Weak<T>(alloc::sync::Weak<Inner<T>>);

impl<T> core::cmp::Eq for Weak<T> {}

impl<T> core::cmp::PartialEq for Weak<T> {
    fn eq(&self, other: &Self) -> bool {
        Self::ptr_eq(self, other)
    }
}

impl<T> Hash for Weak<T> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        let Self(this) = self;
        this.as_ptr().hash(state)
    }
}

impl<T> Clone for Weak<T> {
    fn clone(&self) -> Self {
        let Self(this) = self;
        Weak(this.clone())
    }
}

impl<T> Weak<T> {
    /// Returns true if the two pointers point to the same allocation.
    pub fn ptr_eq(&self, Self(other): &Self) -> bool {
        let Self(this) = self;
        this.ptr_eq(other)
    }

    /// Attempts to upgrade to a [`Strong`].
    ///
    /// Returns `None` if the inner value has since been marked for destruction.
    #[cfg_attr(feature = "rc-debug-names", track_caller)]
    pub fn upgrade(&self) -> Option<Strong<T>> {
        let Self(weak) = self;
        let arc = weak.upgrade()?;
        let Inner { marked_for_destruction, data: _, callers } = arc.deref();

        // `Ordering::Acquire` because we want to synchronize with with the
        // `Ordering::Release` write to `marked_for_destruction` so that all
        // memory writes before the reference was marked for destruction is
        // visible here.
        if !marked_for_destruction.load(Ordering::Acquire) {
            let caller = callers.insert(Location::caller());
            Some(Strong { inner: arc, caller })
        } else {
            None
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zombie_weak() {
        let primary = Primary::new(());
        let weak = {
            let strong = Primary::clone_strong(&primary);
            Strong::downgrade(&strong)
        };
        core::mem::drop(primary);

        assert!(weak.upgrade().is_none());
    }

    #[test]
    fn rcs() {
        const INITIAL_VAL: u8 = 1;
        const NEW_VAL: u8 = 2;

        let primary = Primary::new(std::sync::Mutex::new(INITIAL_VAL));
        let strong = Primary::clone_strong(&primary);
        let weak = Strong::downgrade(&strong);

        *primary.lock().unwrap() = NEW_VAL;
        assert_eq!(*primary.deref().lock().unwrap(), NEW_VAL);
        assert_eq!(*strong.deref().lock().unwrap(), NEW_VAL);
        assert_eq!(*weak.upgrade().unwrap().deref().lock().unwrap(), NEW_VAL);
    }

    #[test]
    fn unwrap_primary_without_strong_held() {
        const VAL: u16 = 6;
        let primary = Primary::new(VAL);
        assert_eq!(Primary::unwrap(primary), VAL);
    }

    #[test]
    #[should_panic(expected = "can't unwrap, still had 1 strong refs")]
    fn unwrap_primary_with_strong_held() {
        let primary = Primary::new(8);
        let _strong: Strong<_> = Primary::clone_strong(&primary);
        let _: u16 = Primary::unwrap(primary);
    }

    #[test]
    #[should_panic(expected = "dropped Primary with 1 strong refs remaining")]
    fn drop_primary_with_strong_held() {
        let primary = Primary::new(9);
        let _strong: Strong<_> = Primary::clone_strong(&primary);
        core::mem::drop(primary);
    }

    // This test trips LSAN on Fuchsia for some unknown reason. The host-side
    // test should be enough to protect us against regressing on the panicking
    // check.
    #[cfg(not(target_os = "fuchsia"))]
    #[test]
    #[should_panic(expected = "oopsie")]
    fn double_panic_protect() {
        let primary = Primary::new(9);
        let strong = Primary::clone_strong(&primary);
        // This will cause primary to be dropped before strong and would yield a
        // double panic if we didn't protect against it in Primary's Drop impl.
        let _tuple_to_invert_drop_order = (primary, strong);
        panic!("oopsie");
    }

    #[cfg(feature = "rc-debug-names")]
    #[test]
    fn tracked_callers() {
        let primary = Primary::new(10);
        // Mark this position so we ensure all track_caller marks are correct in
        // the methods that support it.
        let here = Location::caller();
        let strong1 = Primary::clone_strong(&primary);
        let strong2 = strong1.clone();
        let weak = Strong::downgrade(&strong2);
        let strong3 = weak.upgrade().unwrap();

        let Primary { inner } = &primary;
        let Inner { marked_for_destruction: _, callers, data: _ } = &***inner;

        let strongs = [strong1, strong2, strong3];
        let _: &Location<'_> = strongs.iter().enumerate().fold(here, |prev, (i, cur)| {
            let Strong { inner: _, caller: caller::TrackedCaller { location: cur } } = cur;
            assert_eq!(prev.file(), cur.file(), "{i}");
            assert!(prev.line() < cur.line(), "{prev} < {cur}, {i}");
            {
                let callers = callers.callers.lock();
                assert_eq!(callers.get(cur).copied(), Some(1));
            }

            cur
        });

        // All callers must be removed from the callers map on drop.
        std::mem::drop(strongs);
        {
            let callers = callers.callers.lock();
            let callers = callers.deref();
            assert!(callers.is_empty(), "{callers:?}");
        }
    }
    #[cfg(feature = "rc-debug-names")]
    #[test]
    fn same_location_caller_tracking() {
        fn clone_in_fn<T>(p: &Primary<T>) -> Strong<T> {
            Primary::clone_strong(p)
        }

        let primary = Primary::new(10);
        let strong1 = clone_in_fn(&primary);
        let strong2 = clone_in_fn(&primary);
        assert_eq!(strong1.caller.location, strong2.caller.location);

        let Primary { inner } = &primary;
        let Inner { marked_for_destruction: _, callers, data: _ } = &***inner;

        {
            let callers = callers.callers.lock();
            assert_eq!(callers.get(&strong1.caller.location).copied(), Some(2));
        }

        std::mem::drop(strong1);
        std::mem::drop(strong2);

        {
            let callers = callers.callers.lock();
            let callers = callers.deref();
            assert!(callers.is_empty(), "{callers:?}");
        }
    }

    #[cfg(feature = "rc-debug-names")]
    #[test]
    #[should_panic(expected = "core/sync/src/rc.rs")]
    fn callers_in_panic() {
        let primary = Primary::new(10);
        let _strong = Primary::clone_strong(&primary);
        drop(primary);
    }
}
