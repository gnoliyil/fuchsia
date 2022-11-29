// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod client;

use std::{fmt::Debug, time::Duration};

/// A type representing an instant in time.
pub trait Instant: Sized + Ord + Copy + Clone + Debug + Send + Sync + 'static {
    // Note that this trait intentionally does not have a `now` method to return
    // the current time instant to force the platform to provide the time instant
    // when an event is handled.

    /// Returns the amount of time elapsed from another instant to this one.
    ///
    /// # Panics
    ///
    /// This function will panic if `earlier` is later than `self`.
    fn duration_since(&self, earlier: Self) -> Duration;
}

impl Instant for std::time::Instant {
    fn duration_since(&self, earlier: Self) -> Duration {
        std::time::Instant::duration_since(self, earlier)
    }
}
