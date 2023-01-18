/* automatically generated by rust-bindgen 0.59.1 */

// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Generated by src/connectivity/lib/network-device/rust/scripts/bindgen.sh
// Run the above script whenever src/connectivity/lib/network-device/buffer_descriptor/buffer_descriptor.h
// has changed.

pub const __NETWORK_DEVICE_DESCRIPTOR_VERSION: u32 = 1;
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct buffer_descriptor {
    pub frame_type: u8,
    pub chain_length: u8,
    pub nxt: u16,
    pub info_type: u32,
    pub port_id: buffer_descriptor_port_id,
    pub _reserved: [u8; 2usize],
    pub client_opaque_data: [u8; 4usize],
    pub offset: u64,
    pub head_length: u16,
    pub tail_length: u16,
    pub data_length: u32,
    pub inbound_flags: u32,
    pub return_flags: u32,
}
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct buffer_descriptor_port_id {
    pub base: u8,
    pub salt: u8,
}
