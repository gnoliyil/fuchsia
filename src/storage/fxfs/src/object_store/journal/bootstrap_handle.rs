// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        log::*,
        object_handle::{ObjectHandle, ReadObjectHandle},
        object_store::journal::JournalHandle,
        range::RangeExt,
    },
    anyhow::Error,
    async_trait::async_trait,
    std::{
        cmp::min,
        ops::Range,
        sync::{
            atomic::{AtomicBool, Ordering},
            Arc,
        },
    },
    storage_device::{
        buffer::{Buffer, MutableBufferRef},
        Device,
    },
};

/// To read the super-block and journal, we use this handle since we cannot use StoreObjectHandle
/// until we've replayed the whole journal.  Clients must supply the extents to be used.
pub struct BootstrapObjectHandle {
    object_id: u64,
    device: Arc<dyn Device>,
    start_offset: u64,
    // A list of extents we know of for the handle.  The extents are all logically contiguous and
    // start from |start_offset|, so we don't bother storing the logical offsets.
    extents: Vec<Range<u64>>,
    size: u64,
    trace: AtomicBool,
}

impl BootstrapObjectHandle {
    pub fn new(object_id: u64, device: Arc<dyn Device>) -> Self {
        Self {
            object_id,
            device,
            start_offset: 0,
            extents: Vec::new(),
            size: 0,
            trace: AtomicBool::new(false),
        }
    }

    pub fn new_with_start_offset(
        object_id: u64,
        device: Arc<dyn Device>,
        start_offset: u64,
    ) -> Self {
        Self {
            object_id,
            device,
            start_offset,
            extents: Vec::new(),
            size: 0,
            trace: AtomicBool::new(false),
        }
    }
}

impl ObjectHandle for BootstrapObjectHandle {
    fn object_id(&self) -> u64 {
        self.object_id
    }

    fn allocate_buffer(&self, size: usize) -> Buffer<'_> {
        self.device.allocate_buffer(size)
    }

    fn block_size(&self) -> u64 {
        self.device.block_size().into()
    }

    fn get_size(&self) -> u64 {
        self.size
    }

    fn set_trace(&self, trace: bool) {
        let old_value = self.trace.swap(trace, Ordering::Relaxed);
        if trace != old_value {
            info!(oid = self.object_id, trace, "JH: trace");
        }
    }
}

#[async_trait]
impl ReadObjectHandle for BootstrapObjectHandle {
    async fn read(&self, mut offset: u64, mut buf: MutableBufferRef<'_>) -> Result<usize, Error> {
        assert!(offset >= self.start_offset);
        let trace = self.trace.load(Ordering::Relaxed);
        if trace {
            info!(len = buf.len(), offset, "JH: read");
        }
        let len = buf.len();
        let mut buf_offset = 0;
        let mut file_offset = self.start_offset;
        for extent in &self.extents {
            let extent_len = extent.end - extent.start;
            if offset < file_offset + extent_len {
                if trace {
                    info!(?extent, "JH: matching extent");
                }
                let device_offset = extent.start + offset - file_offset;
                let to_read = min(extent.end - device_offset, (len - buf_offset) as u64) as usize;
                assert!(buf_offset % self.device.block_size() as usize == 0);
                self.device
                    .read(
                        device_offset,
                        buf.reborrow().subslice_mut(buf_offset..buf_offset + to_read),
                    )
                    .await?;
                buf_offset += to_read;
                if buf_offset == len {
                    break;
                }
                offset += to_read as u64;
            }
            file_offset += extent_len;
        }
        Ok(len)
    }
}

impl JournalHandle for BootstrapObjectHandle {
    fn start_offset(&self) -> Option<u64> {
        Some(self.start_offset)
    }

    fn push_extent(&mut self, device_range: Range<u64>) {
        self.size += device_range.length().unwrap();
        self.extents.push(device_range);
    }

    fn discard_extents(&mut self, discard_offset: u64) {
        let mut offset = self.start_offset + self.size;
        let mut num = 0;
        while let Some(extent) = self.extents.last() {
            let length = extent.length().unwrap();
            offset = offset.checked_sub(length).unwrap();
            if offset < discard_offset {
                break;
            }
            self.size -= length;
            self.extents.pop();
            num += 1;
        }
        if self.trace.load(Ordering::Relaxed) {
            info!(count = num, offset = discard_offset, "JH: Discarded extents");
        }
    }
}
