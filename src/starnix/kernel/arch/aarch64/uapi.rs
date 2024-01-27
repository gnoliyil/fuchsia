// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(non_camel_case_types)]

use zerocopy::{AsBytes, FromBytes};

use crate::types::{dev_t, gid_t, ino_t, mode_t, off_t, timespec, uid_t};

pub type blksize_t = i32;
pub type nlink_t = u32;

#[derive(Debug, Default, Clone, Copy, AsBytes, FromBytes)]
#[repr(C)]
pub struct stat_t {
    pub st_dev: dev_t,
    pub st_ino: ino_t,
    pub st_mode: mode_t,
    pub st_nlink: nlink_t,
    pub st_uid: uid_t,
    pub st_gid: gid_t,
    pub st_rdev: dev_t,
    pub _pad1: u64,
    pub st_size: off_t,
    pub st_blksize: blksize_t,
    pub _pad2: i32,
    pub st_blocks: i64,
    pub st_atim: timespec,
    pub st_mtim: timespec,
    pub st_ctim: timespec,
    pub _pad3: [u32; 2],
}
