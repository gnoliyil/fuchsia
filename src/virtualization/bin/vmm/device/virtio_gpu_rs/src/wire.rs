// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Keep all consts and type defs for completeness.
#![allow(dead_code)]

use zerocopy::{AsBytes, FromBytes, LittleEndian, U32, U64};

pub type LE32 = U32<LittleEndian>;
pub type LE64 = U64<LittleEndian>;

//
// 5.7.2 Virtqueues
//
pub const CONTROLQ: u16 = 0;
pub const CURSORQ: u16 = 1;

//
// 5.7.6.7: Request Header
//

// 2D Commands
pub const VIRTIO_GPU_CMD_GET_DISPLAY_INFO: u32 = 0x0100;
pub const VIRTIO_GPU_CMD_RESOURCE_CREATE_2D: u32 = 0x0101;
pub const VIRTIO_GPU_CMD_RESOURCE_UNREF: u32 = 0x0102;
pub const VIRTIO_GPU_CMD_SET_SCANOUT: u32 = 0x0103;
pub const VIRTIO_GPU_CMD_RESOURCE_FLUSH: u32 = 0x0104;
pub const VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D: u32 = 0x0105;
pub const VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: u32 = 0x0106;
pub const VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: u32 = 0x0107;
pub const VIRTIO_GPU_CMD_GET_CAPSET_INFO: u32 = 0x0108;
pub const VIRTIO_GPU_CMD_GET_CAPSET: u32 = 0x0109;
pub const VIRTIO_GPU_CMD_GET_EDID: u32 = 0x010a;
pub const VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID: u32 = 0x010b;
pub const VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB: u32 = 0x010c;
pub const VIRTIO_GPU_CMD_SET_SCANOUT_BLOB: u32 = 0x010d;

// 3D Commands
pub const VIRTIO_GPU_CMD_CTX_CREATE: u32 = 0x0200;
pub const VIRTIO_GPU_CMD_CTX_DESTROY: u32 = 0x0201;
pub const VIRTIO_GPU_CMD_CTX_ATTACH_RESOURC: u32 = 0x0202;
pub const VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE: u32 = 0x0203;
pub const VIRTIO_GPU_CMD_RESOURCE_CREATE_3D: u32 = 0x0204;
pub const VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D: u32 = 0x0205;
pub const VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D: u32 = 0x0206;
pub const VIRTIO_GPU_CMD_SUBMIT_3D: u32 = 0x0207;
pub const VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB: u32 = 0x0208;
pub const VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB: u32 = 0x0209;

// Cursor Commands
pub const VIRTIO_GPU_CMD_UPDATE_CURSOR: u32 = 0x0300;
pub const VIRTIO_GPU_CMD_MOVE_CURSOR: u32 = 0x0301;

// Success Responses
pub const VIRTIO_GPU_RESP_OK_NODATA: u32 = 0x1100;
pub const VIRTIO_GPU_RESP_OK_DISPLAY_INFO: u32 = 0x1101;
pub const VIRTIO_GPU_RESP_OK_CAPSET_INFO: u32 = 0x1102;
pub const VIRTIO_GPU_RESP_OK_CAPSET: u32 = 0x1103;
pub const VIRTIO_GPU_RESP_OK_EDID: u32 = 0x1104;
pub const VIRTIO_GPU_RESP_OK_RESOURCE_UUID: u32 = 0x1105;
pub const VIRTIO_GPU_RESP_OK_MAP_INFO: u32 = 0x1106;

// Error Responses
pub const VIRTIO_GPU_RESP_ERR_UNSPEC: u32 = 0x1200;
pub const VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY: u32 = 0x1201;
pub const VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID: u32 = 0x1202;
pub const VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID: u32 = 0x1203;
pub const VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID: u32 = 0x1204;
pub const VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER: u32 = 0x1205;

// Flags
pub const VIRTIO_GPU_FLAG_FENCE: u32 = 0x01;
pub const VIRTIO_GPU_FLAG_INFO_RING_IDX: u32 = 0x02;

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuCtrlHeader {
    pub ty: LE32,
    pub flags: LE32,
    pub fence_id: LE64,
    pub ctx_id: LE32,
    pub ring_idx: u8,
    pub padding: [u8; 3],
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_GET_DISPLAY_INFO
//
pub const VIRTIO_GPU_MAX_SCANOUTS: usize = 16;

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuRect {
    x: LE32,
    y: LE32,
    width: LE32,
    height: LE32,
}

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuDisplayOne {
    r: VirtioGpuRect,
    enabled: LE32,
    flags: LE32,
}

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuRespDisplayInfo {
    hdr: VirtioGpuCtrlHeader,
    pmodes: [VirtioGpuDisplayOne; VIRTIO_GPU_MAX_SCANOUTS],
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_GET_EDID
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuGetEdid {
    hdr: VirtioGpuCtrlHeader,
    scanout: LE32,
    padding: LE32,
}

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuRespEdid {
    hdr: VirtioGpuCtrlHeader,
    size: LE32,
    padding: LE32,
    edid: [u8; 1024],
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_RESOURCE_CREATE_2D
//
pub const VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM: u32 = 1;
pub const VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM: u32 = 2;
pub const VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM: u32 = 3;
pub const VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM: u32 = 4;
pub const VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM: u32 = 67;
pub const VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM: u32 = 68;
pub const VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM: u32 = 121;
pub const VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM: u32 = 134;

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuResourceCreate2d {
    hdr: VirtioGpuCtrlHeader,
    resource_id: LE32,
    format: LE32,
    width: LE32,
    height: LE32,
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_RESOURCE_UNREF
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuResourceUnref {
    hdr: VirtioGpuCtrlHeader,
    resource_id: LE32,
    padding: LE32,
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_SET_SCANOUT
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuSetScanout {
    hdr: VirtioGpuCtrlHeader,
    r: VirtioGpuRect,
    scanout_id: LE32,
    resource_id: LE32,
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_RESOURCE_FLUSH
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuResourceFlush {
    hdr: VirtioGpuCtrlHeader,
    r: VirtioGpuRect,
    resource_id: LE32,
    padding: LE32,
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_RESOURCE_FLUSH
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuTransferToHost2d {
    hdr: VirtioGpuCtrlHeader,
    r: VirtioGpuRect,
    offset: LE64,
    resource_id: LE32,
    padding: LE32,
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuResourceAttachBacking {
    hdr: VirtioGpuCtrlHeader,
    resource_id: LE32,
    nr_entries: LE32,
}

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuMemEntry {
    addr: LE64,
    length: LE32,
    padding: LE32,
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuResourceDetachBacking {
    hdr: VirtioGpuCtrlHeader,
    resource_id: LE32,
    padding: LE32,
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_GET_CAPSET_INFO
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuGetCapsetInfo {
    hdr: VirtioGpuCtrlHeader,
    capset_index: LE32,
    padding: LE32,
}

pub const VIRTIO_GPU_CAPSET_VIRGL: u32 = 1;
pub const VIRTIO_GPU_CAPSET_VIRGL2: u32 = 2;
pub const VIRTIO_GPU_CAPSET_GFXSTREAM: u32 = 3;
pub const VIRTIO_GPU_CAPSET_VENUS: u32 = 4;
pub const VIRTIO_GPU_CAPSET_CROSS_DOMAIN: u32 = 5;

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuRespCapsetInfo {
    hdr: VirtioGpuCtrlHeader,
    capset_id: LE32,
    capset_max_version: LE32,
    capset_max_size: LE32,
    padding: LE32,
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_GET_CAPSET
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuGetCapset {
    hdr: VirtioGpuCtrlHeader,
    capset_id: LE32,
    capset_version: LE32,
}

// The response structure is just a header followed by a flexible data member. There's no
// reasonable way to model this as a rust struct; the implementation should just write the header
// and then the data.
//
//     pub struct VirtioGpuRespCapset {
//         hdr: VirtioGpuCtrlHeader,
//         capset_data: [u8; ??];
//     }

//
// 5.7.6.8: VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuResourceAssignUuid {
    hdr: VirtioGpuCtrlHeader,
    resource_id: LE32,
    padding: LE32,
}

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuRespResourceUuid {
    hdr: VirtioGpuCtrlHeader,
    uuid: [u8; 16],
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB
//
pub const VIRTIO_GPU_BLOB_MEM_GUEST: u32 = 0x0001;
pub const VIRTIO_GPU_BLOB_MEM_HOST3D2: u32 = 0x0002;
pub const VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST: u32 = 0x0003;

pub const VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE: u32 = 0x0001;
pub const VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE: u32 = 0x0002;
pub const VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE: u32 = 0x0004;

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuResourceCreateBlob {
    hdr: VirtioGpuCtrlHeader,
    resource_id: LE32,
    blob_mem: LE32,
    blob_flags: LE32,
    nr_entries: LE32,
    blob_id: LE64,
    size: LE64,
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_SET_SCANOUT_BLOB
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuSetScanoutBlob {
    hdr: VirtioGpuCtrlHeader,
    r: VirtioGpuRect,
    scanout_id: LE32,
    resource_id: LE32,
    width: LE32,
    height: LE32,
    format: LE32,
    padding: LE32,
    strides: [LE32; 4],
    offsets: [LE32; 4],
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_CTX_CREATE
//
pub const VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK: u32 = 0x000000ff;

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuCtxCreate {
    hdr: VirtioGpuCtrlHeader,
    nlen: LE32,
    context_init: LE32,
    debug_name: [u8; 64],
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuResourceMapBlob {
    hdr: VirtioGpuCtrlHeader,
    resource_id: LE32,
    padding: LE32,
    offset: LE64,
}

pub const VIRTIO_GPU_MAP_CACHE_MASK: u32 = 0x0f;
pub const VIRTIO_GPU_MAP_CACHE_NONE: u32 = 0;
pub const VIRTIO_GPU_MAP_CACHE_CACHED: u32 = 1;
pub const VIRTIO_GPU_MAP_CACHE_UNCACHED: u32 = 2;
pub const VIRTIO_GPU_MAP_CACHE_WC: u32 = 3;

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuRespMapInfo {
    hdr: VirtioGpuCtrlHeader,
    // These are specified in the spec as u32 and not le32. Unclear if this is oversight or
    // intentional. For now we'll leave them to match the spec.
    map_info: u32,
    padding: u32,
}

//
// 5.7.6.8: VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuResourceUnmapBlob {
    hdr: VirtioGpuCtrlHeader,
    resource_id: LE32,
    padding: LE32,
}

//
// 5.7.6.10: cursorq
//
#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuCursorPos {
    scanout_id: LE32,
    x: LE32,
    y: LE32,
    padding: LE32,
}

#[derive(Debug, Copy, Clone, AsBytes, FromBytes)]
#[repr(C, packed)]
pub struct VirtioGpuUpdateCursor {
    hdr: VirtioGpuCtrlHeader,
    pos: VirtioGpuCursorPos,
    resource_id: LE32,
    hot_x: LE32,
    hot_y: LE32,
    padding: LE32,
}
