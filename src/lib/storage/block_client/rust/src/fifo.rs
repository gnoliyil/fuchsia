/* automatically generated by rust-bindgen 0.65.1 */

// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Generated by src/lib/storage/block_client/rust/scripts/bindgen.sh
// Run the above script whenever src/devices/block/drivers/core/block-fifo.h
// has changed.

#![allow(dead_code)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]

use zerocopy::{AsBytes, FromBytes, FromZeroes};

pub type zx_status_t = i32;
pub type reqid_t = u32;
pub type groupid_t = u16;
pub type vmoid_t = u16;
#[repr(C)]
#[derive(Debug, Default, Copy, Clone, AsBytes, FromBytes, FromZeroes)]
pub struct BlockFifoCommand {
    pub opcode: u8,
    pub padding_to_satisfy_zerocopy: [u8; 3usize],
    pub flags: u32,
}
#[test]
fn bindgen_test_layout_BlockFifoCommand() {
    const UNINIT: ::std::mem::MaybeUninit<BlockFifoCommand> = ::std::mem::MaybeUninit::uninit();
    let ptr = UNINIT.as_ptr();
    assert_eq!(
        ::std::mem::size_of::<BlockFifoCommand>(),
        8usize,
        concat!("Size of: ", stringify!(BlockFifoCommand))
    );
    assert_eq!(
        ::std::mem::align_of::<BlockFifoCommand>(),
        4usize,
        concat!("Alignment of ", stringify!(BlockFifoCommand))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).opcode) as usize - ptr as usize },
        0usize,
        concat!("Offset of field: ", stringify!(BlockFifoCommand), "::", stringify!(opcode))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).padding_to_satisfy_zerocopy) as usize - ptr as usize },
        1usize,
        concat!(
            "Offset of field: ",
            stringify!(BlockFifoCommand),
            "::",
            stringify!(padding_to_satisfy_zerocopy)
        )
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).flags) as usize - ptr as usize },
        4usize,
        concat!("Offset of field: ", stringify!(BlockFifoCommand), "::", stringify!(flags))
    );
}
pub type block_fifo_command_t = BlockFifoCommand;
#[repr(C)]
#[derive(Debug, Default, Copy, Clone, AsBytes, FromBytes, FromZeroes)]
pub struct BlockFifoRequest {
    pub command: block_fifo_command_t,
    pub reqid: reqid_t,
    pub group: groupid_t,
    pub vmoid: vmoid_t,
    pub length: u32,
    pub padding_to_satisfy_zerocopy: u32,
    pub vmo_offset: u64,
    pub dev_offset: u64,
    pub trace_flow_id: u64,
}
#[test]
fn bindgen_test_layout_BlockFifoRequest() {
    const UNINIT: ::std::mem::MaybeUninit<BlockFifoRequest> = ::std::mem::MaybeUninit::uninit();
    let ptr = UNINIT.as_ptr();
    assert_eq!(
        ::std::mem::size_of::<BlockFifoRequest>(),
        48usize,
        concat!("Size of: ", stringify!(BlockFifoRequest))
    );
    assert_eq!(
        ::std::mem::align_of::<BlockFifoRequest>(),
        8usize,
        concat!("Alignment of ", stringify!(BlockFifoRequest))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).command) as usize - ptr as usize },
        0usize,
        concat!("Offset of field: ", stringify!(BlockFifoRequest), "::", stringify!(command))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).reqid) as usize - ptr as usize },
        8usize,
        concat!("Offset of field: ", stringify!(BlockFifoRequest), "::", stringify!(reqid))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).group) as usize - ptr as usize },
        12usize,
        concat!("Offset of field: ", stringify!(BlockFifoRequest), "::", stringify!(group))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).vmoid) as usize - ptr as usize },
        14usize,
        concat!("Offset of field: ", stringify!(BlockFifoRequest), "::", stringify!(vmoid))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).length) as usize - ptr as usize },
        16usize,
        concat!("Offset of field: ", stringify!(BlockFifoRequest), "::", stringify!(length))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).padding_to_satisfy_zerocopy) as usize - ptr as usize },
        20usize,
        concat!(
            "Offset of field: ",
            stringify!(BlockFifoRequest),
            "::",
            stringify!(padding_to_satisfy_zerocopy)
        )
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).vmo_offset) as usize - ptr as usize },
        24usize,
        concat!("Offset of field: ", stringify!(BlockFifoRequest), "::", stringify!(vmo_offset))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).dev_offset) as usize - ptr as usize },
        32usize,
        concat!("Offset of field: ", stringify!(BlockFifoRequest), "::", stringify!(dev_offset))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).trace_flow_id) as usize - ptr as usize },
        40usize,
        concat!("Offset of field: ", stringify!(BlockFifoRequest), "::", stringify!(trace_flow_id))
    );
}
#[repr(C)]
#[derive(Debug, Default, Copy, Clone, AsBytes, FromBytes, FromZeroes)]
pub struct BlockFifoResponse {
    pub status: zx_status_t,
    pub reqid: reqid_t,
    pub group: groupid_t,
    pub padding_to_satisfy_zerocopy: u16,
    pub count: u32,
    pub padding_to_match_request_size_and_alignment: [u64; 4usize],
}
#[test]
fn bindgen_test_layout_BlockFifoResponse() {
    const UNINIT: ::std::mem::MaybeUninit<BlockFifoResponse> = ::std::mem::MaybeUninit::uninit();
    let ptr = UNINIT.as_ptr();
    assert_eq!(
        ::std::mem::size_of::<BlockFifoResponse>(),
        48usize,
        concat!("Size of: ", stringify!(BlockFifoResponse))
    );
    assert_eq!(
        ::std::mem::align_of::<BlockFifoResponse>(),
        8usize,
        concat!("Alignment of ", stringify!(BlockFifoResponse))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).status) as usize - ptr as usize },
        0usize,
        concat!("Offset of field: ", stringify!(BlockFifoResponse), "::", stringify!(status))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).reqid) as usize - ptr as usize },
        4usize,
        concat!("Offset of field: ", stringify!(BlockFifoResponse), "::", stringify!(reqid))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).group) as usize - ptr as usize },
        8usize,
        concat!("Offset of field: ", stringify!(BlockFifoResponse), "::", stringify!(group))
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).padding_to_satisfy_zerocopy) as usize - ptr as usize },
        10usize,
        concat!(
            "Offset of field: ",
            stringify!(BlockFifoResponse),
            "::",
            stringify!(padding_to_satisfy_zerocopy)
        )
    );
    assert_eq!(
        unsafe { ::std::ptr::addr_of!((*ptr).count) as usize - ptr as usize },
        12usize,
        concat!("Offset of field: ", stringify!(BlockFifoResponse), "::", stringify!(count))
    );
    assert_eq!(
        unsafe {
            ::std::ptr::addr_of!((*ptr).padding_to_match_request_size_and_alignment) as usize
                - ptr as usize
        },
        16usize,
        concat!(
            "Offset of field: ",
            stringify!(BlockFifoResponse),
            "::",
            stringify!(padding_to_match_request_size_and_alignment)
        )
    );
}
