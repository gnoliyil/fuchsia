// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Macros used in Netstack3.

macro_rules! log_unimplemented {
    ($nocrash:expr, $fmt:expr $(,$arg:expr)*) => {{

        #[cfg(feature = "crash_on_unimplemented")]
        unimplemented!($fmt, $($arg),*);

        #[cfg(not(feature = "crash_on_unimplemented"))]
        // Clippy doesn't like blocks explicitly returning ().
        #[allow(clippy::unused_unit)]
        {
            // log doesn't play well with the new macro system; it expects all
            // of its macros to be in scope.
            use ::log::*;
            trace!(concat!("Unimplemented: ", $fmt), $($arg),*);
            $nocrash
        }
    }};
}

/// Implement [`TimerContext`] for one ID type in terms of an existing
/// implementation for a different ID type.
///
/// `$outer_timer_id` is an enum where one variant contains an
/// `$inner_timer_id`. `impl_timer_context!` generates an impl of
/// `TimerContext<$inner_timer_id>` for any `C: TimerContext<$outer_timer_id>`.
///
/// An impl of `Into<$outer_timer_id> for `$inner_timer_id` must exist. `$pat`
/// is a pattern of type `$outer_timer_id` that binds the `$inner_timer_id`.
/// `$bound_variable` is the name of the bound `$inner_timer_id` from the
/// pattern. For example, if `$pat` is `OuterTimerId::Inner(id)`, then
/// `$bound_variable` would be `id`. This is required for macro hygiene.
///
/// If an extra first parameter, `$bound`, is provided, then it is added as an
/// extra bound on the `C` context type.
///
/// [`TimerContext`]: crate::context::TimerContext
macro_rules! impl_timer_context {
    ($outer_timer_id:ty, $inner_timer_id:ty, $pat:pat, $bound_variable:ident) => {
        impl<C: crate::context::TimerContext<$outer_timer_id>>
            crate::context::TimerContext<$inner_timer_id> for C
        {
            impl_timer_context!(@inner $outer_timer_id, $inner_timer_id, $pat, $bound_variable);
        }
    };
    (C: $c_bound:ident, $outer_timer_id:ty, $inner_timer_id:ty, $pat:pat, $bound_variable:ident) => {
        impl<C: $c_bound + crate::context::TimerContext<$outer_timer_id>>
            crate::context::TimerContext<$inner_timer_id> for C
        {
            impl_timer_context!(@inner $outer_timer_id, $inner_timer_id, $pat, $bound_variable);
        }
    };
    ($other_type_arg:ident, $outer_timer_id:ty, $inner_timer_id:ty, $pat:pat, $bound_variable:ident) => {
        impl<$other_type_arg, C: crate::context::TimerContext<$outer_timer_id>>
            crate::context::TimerContext<$inner_timer_id> for C
        {
            impl_timer_context!(@inner $outer_timer_id, $inner_timer_id, $pat, $bound_variable);
        }
    };
    (@inner $outer_timer_id:ty, $inner_timer_id:ty, $pat:pat, $bound_variable:ident) => {
        fn schedule_timer_instant(
            &mut self,
            time: Self::Instant,
            id: $inner_timer_id,
        ) -> Option<Self::Instant> {
            crate::context::TimerContext::<$outer_timer_id>::schedule_timer_instant(self, time, id.into())
        }

        fn cancel_timer(&mut self, id: $inner_timer_id) -> Option<Self::Instant> {
            crate::context::TimerContext::<$outer_timer_id>::cancel_timer(self, id.into())
        }

        fn cancel_timers_with<F: FnMut(&$inner_timer_id) -> bool>(&mut self, mut f: F) {
            crate::context::TimerContext::<$outer_timer_id>::cancel_timers_with(self, |id| match id {
                $pat => f($bound_variable),
                #[allow(unreachable_patterns)]
                _ => false,
            })
        }

        fn scheduled_instant(&self, id: $inner_timer_id) -> Option<Self::Instant> {
            crate::context::TimerContext::<$outer_timer_id>::scheduled_instant(self, id.into())
        }
    };
}

/// Declare a benchmark function.
///
/// The function will be named `$name`. If the `benchmark` cfg is enabled, the
/// provided `$fn` will be invoked with a `&mut criterion::Bencher` - in other
/// words, a real benchmark. If the `benchmark` cfg is disabled, the function
/// will be annotated with the `#[test]` attribute, and the provided `$fn` will
/// be invoked with a `&mut TestBencher`, which has the effect of creating a
/// test that runs the benchmarked function for a small, fixed number of
/// iterations. This test allows the use of debug assertions to verify the
/// correctness of the benchmark.
///
/// Note that `$fn` doesn't have to be a named function - it can also be an
/// anonymous closure.
#[cfg(test)]
macro_rules! bench {
    ($name:ident, $fn:expr) => {
        #[cfg(benchmark)]
        fn $name(b: &mut criterion::Bencher) {
            $fn(b);
        }

        #[cfg(not(benchmark))]
        #[test]
        fn $name() {
            $fn(&mut crate::testutil::benchmarks::TestBencher);
        }
    };
}
