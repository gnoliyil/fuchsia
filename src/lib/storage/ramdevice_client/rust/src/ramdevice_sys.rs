// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Generated by ./src/lib/storage/ramdevice_client/rust/bindgen.sh using bindgen 0.60.1

#![allow(dead_code)]

pub type __uint8_t = ::std::os::raw::c_uchar;
pub type __int32_t = ::std::os::raw::c_int;
pub type __uint32_t = ::std::os::raw::c_uint;
pub type __uint64_t = ::std::os::raw::c_ulong;
pub type size_t = ::std::os::raw::c_ulong;
pub type zx_handle_t = u32;
pub type zx_status_t = i32;
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct ramdisk_client {
    _unused: [u8; 0],
}
pub type ramdisk_client_t = ramdisk_client;
extern "C" {
    pub fn ramdisk_create(
        blk_size: u64,
        blk_count: u64,
        out: *mut *mut ramdisk_client_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_create_at(
        dev_root_fd: ::std::os::raw::c_int,
        blk_size: u64,
        blk_count: u64,
        out: *mut *mut ramdisk_client_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_create_with_guid(
        blk_size: u64,
        blk_count: u64,
        type_guid: *const u8,
        guid_len: size_t,
        out: *mut *mut ramdisk_client_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_create_at_with_guid(
        dev_root_fd: ::std::os::raw::c_int,
        blk_size: u64,
        blk_count: u64,
        type_guid: *const u8,
        guid_len: size_t,
        out: *mut *mut ramdisk_client_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_create_from_vmo(
        vmo: zx_handle_t,
        out: *mut *mut ramdisk_client_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_create_at_from_vmo(
        dev_root_fd: ::std::os::raw::c_int,
        vmo: zx_handle_t,
        out: *mut *mut ramdisk_client_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_create_from_vmo_with_params(
        vmo: zx_handle_t,
        block_size: u64,
        type_guid: *const u8,
        guid_len: size_t,
        out: *mut *mut ramdisk_client_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_create_at_from_vmo_with_params(
        dev_root_fd: ::std::os::raw::c_int,
        vmo: zx_handle_t,
        block_size: u64,
        type_guid: *const u8,
        guid_len: size_t,
        out: *mut *mut ramdisk_client_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_get_block_interface(client: *const ramdisk_client_t) -> zx_handle_t;
}
extern "C" {
    pub fn ramdisk_get_path(client: *const ramdisk_client_t) -> *const ::std::os::raw::c_char;
}
extern "C" {
    pub fn ramdisk_sleep_after(client: *const ramdisk_client_t, blk_count: u64) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_wake(client: *const ramdisk_client_t) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_grow(client: *const ramdisk_client_t, required_size: u64) -> zx_status_t;
}
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct ramdisk_block_write_counts {
    pub received: u64,
    pub successful: u64,
    pub failed: u64,
}
pub type ramdisk_block_write_counts_t = ramdisk_block_write_counts;
extern "C" {
    pub fn ramdisk_get_block_counts(
        client: *const ramdisk_client_t,
        out_counts: *mut ramdisk_block_write_counts_t,
    ) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_set_flags(client: *const ramdisk_client_t, flags: u32) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_rebind(client: *mut ramdisk_client_t) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_destroy(client: *mut ramdisk_client_t) -> zx_status_t;
}
extern "C" {
    pub fn ramdisk_forget(client: *mut ramdisk_client_t) -> zx_status_t;
}
