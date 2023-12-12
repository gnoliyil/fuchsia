// Copyright 2023 The Fuchsia Authors
//
// Licensed under a BSD-style license <LICENSE-BSD>, Apache License, Version 2.0
// <LICENSE-APACHE or https://www.apache.org/licenses/LICENSE-2.0>, or the MIT
// license <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your option.
// This file may not be copied, modified, or distributed except according to
// those terms.

extern crate zerocopy;

use zerocopy::{transmute_mut, AsBytes, FromBytes, NoCell};

fn main() {}

fn transmute_mut<T: AsBytes + FromBytes + NoCell, U: AsBytes + FromBytes + NoCell>(
    t: &mut T,
) -> &mut U {
    // `transmute_mut!` requires the source and destination types to be
    // concrete.
    transmute_mut!(t)
}
