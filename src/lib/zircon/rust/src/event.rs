// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Type-safe bindings for Zircon event objects.

use crate::ok;
use crate::{AsHandleRef, Handle, HandleBased, HandleRef, Status};
use fuchsia_zircon_sys as sys;

/// An object representing a Zircon
/// [event object](https://fuchsia.dev/fuchsia-src/concepts/objects/event.md).
///
/// As essentially a subtype of `Handle`, it can be freely interconverted.
#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[repr(transparent)]
pub struct Event(Handle);
impl_handle_based!(Event);

impl Event {
    /// Create an event object, an object which is signalable but nothing else. Wraps the
    /// [zx_event_create](https://fuchsia.dev/fuchsia-src/reference/syscalls/event_create.md)
    /// syscall.
    ///
    /// # Panics
    ///
    /// If the kernel reports no memory available or the process' job policy denies event creation.
    #[allow(deprecated)]
    pub fn create() -> Self {
        Self::try_create()
            .expect("event creation always succeeds except with OOM or when job policy denies it")
    }

    /// Create an event object, an object which is signalable but nothing else. Wraps the
    /// [zx_event_create](https://fuchsia.dev/fuchsia-src/reference/syscalls/event_create.md)
    /// syscall.
    #[deprecated = "creation APIs will no longer be fallible in the future. Users should prefer `create`"]
    pub fn try_create() -> Result<Event, Status> {
        let mut out = 0;
        let opts = 0;
        let status = unsafe { sys::zx_event_create(opts, &mut out) };
        ok(status)?;
        unsafe { Ok(Self::from(Handle::from_raw(out))) }
    }
}
