// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Fxfs is a log-structured filesystem for [Fuchsia](https://fuchsia.dev/).
//!
//! For a high-level overview, please refer to the
//! [RFC](/docs/contribute/governance/rfcs/0136_fxfs.md).
//!
//! Where possible, Fxfs code tries to be target agnostic.
//! Fuchsia specific bindings are primarily found under [server].

pub mod checksum;
pub mod data_buffer;
pub mod drop_event;

#[macro_use]
mod debug_assert_not_too_long;

pub mod errors;
pub mod filesystem;
pub mod fsck;
pub mod log;
mod lsm_tree;
pub mod metrics;
pub mod object_handle;
pub mod object_store;
pub mod range;
pub mod round;
pub mod serialized_types;
#[cfg(test)]
mod testing;
#[macro_use]
mod trace;
