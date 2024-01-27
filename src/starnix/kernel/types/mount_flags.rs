// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use bitflags::bitflags;

use crate::types::uapi;

bitflags! {
    pub struct MountFlags: u32 {
        const RDONLY = uapi::MS_RDONLY;
        const NOEXEC = uapi::MS_NOEXEC;
        const NOSUID = uapi::MS_NOSUID;
        const NODEV = uapi::MS_NODEV;
        const NOATIME = uapi::MS_NOATIME;
        const SILENT = uapi::MS_SILENT;
        const BIND = uapi::MS_BIND;
        const REC = uapi::MS_REC;
        const DOWNSTREAM = uapi::MS_SLAVE;
        const SHARED = uapi::MS_SHARED;
        const PRIVATE = uapi::MS_PRIVATE;
        const LAZYTIME = uapi::MS_LAZYTIME;

        /// Flags that can be stored in Mount state.
        const STORED_FLAGS = Self::RDONLY.bits | Self::NOEXEC.bits | Self::NOSUID.bits | Self::NODEV.bits | Self::NOATIME.bits | Self::LAZYTIME.bits;
    }
}
