// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::ffi::c_void;
use std::ptr::null_mut;
use tpm2_tss_sys as tss_sys;

/// The `EsysHeapWrapper` is a generic type that wraps any `tpm2_tss_sys`
/// pointer that can be freed using the `Esys_Free` function. This prevents
/// accidental memory leaks in the low level Rust layer that calls through to
/// the underlying unsafe bindings.
pub struct EsysHeapWrapper<T> {
    pub inner: *mut T,
}

impl<T> EsysHeapWrapper<T> {
    /// Creates a pointer for the provided type and assigns it to
    /// null. The assumption is the inner memory will be allocated by
    /// the C library in an internal function call.
    pub fn null() -> Self {
        Self { inner: null_mut() }
    }
}

/// Drop casts the inner pointer to a `ffi::c_void` and frees the underlying
/// memory using `Esys_Free`.
impl<T> Drop for EsysHeapWrapper<T> {
    fn drop(&mut self) {
        unsafe { tss_sys::Esys_Free(self.inner as *mut c_void) }
    }
}
