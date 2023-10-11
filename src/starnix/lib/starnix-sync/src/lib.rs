// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Synchronization objects used by Starnix

mod interruptible_event;
mod port_event;

pub use interruptible_event::*;
pub use port_event::*;
