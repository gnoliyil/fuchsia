// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Type-safe bindings for Zircon rights.

use bitflags::bitflags;
use fuchsia_zircon_sys as sys;

bitflags! {
    /// Rights associated with a handle.
    ///
    /// See [rights](https://fuchsia.dev/fuchsia-src/concepts/kernel/rights) for more information.
    #[repr(C)]
    pub struct Rights: sys::zx_rights_t {
        const NONE            = sys::ZX_RIGHT_NONE;
        const DUPLICATE       = sys::ZX_RIGHT_DUPLICATE;
        const TRANSFER        = sys::ZX_RIGHT_TRANSFER;
        const READ            = sys::ZX_RIGHT_READ;
        const WRITE           = sys::ZX_RIGHT_WRITE;
        const EXECUTE         = sys::ZX_RIGHT_EXECUTE;
        const MAP             = sys::ZX_RIGHT_MAP;
        const GET_PROPERTY    = sys::ZX_RIGHT_GET_PROPERTY;
        const SET_PROPERTY    = sys::ZX_RIGHT_SET_PROPERTY;
        const ENUMERATE       = sys::ZX_RIGHT_ENUMERATE;
        const DESTROY         = sys::ZX_RIGHT_DESTROY;
        const SET_POLICY      = sys::ZX_RIGHT_SET_POLICY;
        const GET_POLICY      = sys::ZX_RIGHT_GET_POLICY;
        const SIGNAL          = sys::ZX_RIGHT_SIGNAL;
        const SIGNAL_PEER     = sys::ZX_RIGHT_SIGNAL_PEER;
        const WAIT            = sys::ZX_RIGHT_WAIT;
        const INSPECT         = sys::ZX_RIGHT_INSPECT;
        const MANAGE_JOB      = sys::ZX_RIGHT_MANAGE_JOB;
        const MANAGE_PROCESS  = sys::ZX_RIGHT_MANAGE_PROCESS;
        const MANAGE_THREAD   = sys::ZX_RIGHT_MANAGE_THREAD;
        const APPLY_PROFILE   = sys::ZX_RIGHT_APPLY_PROFILE;
        const MANAGE_SOCKET   = sys::ZX_RIGHT_MANAGE_SOCKET;
        const RESIZE          = sys::ZX_RIGHT_RESIZE;
        const SAME_RIGHTS     = sys::ZX_RIGHT_SAME_RIGHTS;

        const BASIC           = sys::ZX_RIGHT_TRANSFER | sys::ZX_RIGHT_DUPLICATE |
                                sys::ZX_RIGHT_WAIT | sys::ZX_RIGHT_INSPECT;
        const IO              = sys::ZX_RIGHT_READ | sys::ZX_RIGHT_WRITE;
        const PROPERTY        = sys::ZX_RIGHT_GET_PROPERTY | sys::ZX_RIGHT_SET_PROPERTY;
        const POLICY          = sys::ZX_RIGHT_GET_POLICY | sys::ZX_RIGHT_SET_POLICY;
        const RESOURCE_BASIC  = sys::ZX_RIGHT_TRANSFER | sys::ZX_RIGHT_DUPLICATE |
                                sys::ZX_RIGHT_WRITE | sys::ZX_RIGHT_INSPECT;

        // Default rights for a newly created object of a particular type.
        // See zircon/system/public/zircon/rights.h
        const CHANNEL_DEFAULT = sys::ZX_RIGHT_TRANSFER | sys::ZX_RIGHT_WAIT |
                                sys::ZX_RIGHT_INSPECT |sys::ZX_RIGHT_READ |
                                sys::ZX_RIGHT_WRITE | sys::ZX_RIGHT_SIGNAL |
                                sys::ZX_RIGHT_SIGNAL_PEER;
        const VMO_DEFAULT     = Self::BASIC.bits() | Self::IO.bits() | Self::PROPERTY.bits() | Self::MAP.bits() | sys::ZX_RIGHT_SIGNAL;
    }
}

impl Rights {
    /// Same as from_bits() but a const fn.
    #[inline]
    pub const fn from_bits_const(bits: sys::zx_rights_t) -> Option<Rights> {
        if (bits & !Rights::all().bits()) == 0 {
            return Some(Rights { bits });
        } else {
            None
        }
    }
}
