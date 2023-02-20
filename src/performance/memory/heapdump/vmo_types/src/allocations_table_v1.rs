// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_zircon as zx;
use static_assertions::const_assert;
use std::mem::{align_of, size_of};
use std::sync::atomic::{
    AtomicU32,
    Ordering::{Relaxed, SeqCst},
};

use crate::memory_mapped_vmo::{MemoryMappable, MemoryMappedVmo};

type NodeIndex = u32;
type AtomicNodeIndex = AtomicU32;
type BucketHeads = [AtomicNodeIndex; NUM_BUCKETS];
const NUM_BUCKETS: usize = 1 << 16;
const NODE_INVALID: NodeIndex = NodeIndex::MAX;

/// Minimum memory alignment of an allocation table.
pub const MIN_ALIGNMENT: usize = align_of::<Node>();
const_assert!(MIN_ALIGNMENT % align_of::<BucketHeads>() == 0);

#[repr(C)]
#[derive(Debug)]
pub struct Node {
    next: AtomicNodeIndex,
    pub address: u64,
    pub size: u64,
}

// SAFETY: Our accessor functions never access this type's memory non-atomically.
unsafe impl MemoryMappable for AtomicNodeIndex {}
unsafe impl MemoryMappable for Node {}

/// Computes the capacity (number of nodes) of a memory region given its size (bytes).
fn compute_nodes_count(num_bytes: usize) -> Result<usize, crate::Error> {
    let Some(nodes_size) = num_bytes.checked_sub(size_of::<BucketHeads>()) else {
        return Err(crate::Error::BufferTooSmall);
    };

    let num_nodes = nodes_size / size_of::<Node>();
    if num_nodes > NODE_INVALID as usize {
        return Err(crate::Error::BufferTooBig);
    }

    Ok(num_nodes)
}

/// Mediates write access to a VMO containing an hash table of allocated blocks indexed by block
/// address.
///
/// All the updates happen atomically so that, at any time, snapshotting the VMO always results in a
/// coherent snapshot of the table.
///
/// Hash collisions are handled by maintaining per-bucket linked lists. Specifically, an array of
/// list heads, one for each bucket, is stored at the beginning of the VMO. The remaining part of
/// the VMO contains the linked list nodes.
pub struct AllocationsTableWriter {
    storage: MemoryMappedVmo,

    // Free nodes are managed by a simple watermark allocator and the capacity of the hash table
    // (i.e. the maximum number of nodes) is fixed.
    //
    // In order to make it possible to reuse nodes that have been freed, we also keep a linked list
    // of free nodes in addition to the watermark. When it is not empty, nodes are allocated by
    // popping the head of this list instead of incrementing the watermark.
    //
    // The watermark and the free list are not necessary for reading a snapshot. Therefore, we do
    // not need to store them in the VMO or to offer any snapshot guarantee about them, and they do
    // not need to be updated atomically.
    watermark: NodeIndex,
    max_num_nodes: usize,
    free_list_head: NodeIndex,
}

impl AllocationsTableWriter {
    /// Initializes a VMO as an empty table and creates an AllocationsTableWriter to write into it.
    ///
    /// # Safety
    /// The caller must guarantee that the `vmo` is not accessed by others while the returned
    /// instance is alive.
    pub fn new(vmo: &zx::Vmo) -> Result<AllocationsTableWriter, crate::Error> {
        let storage = MemoryMappedVmo::new_readwrite(vmo)?;
        let max_num_nodes = compute_nodes_count(storage.vmo_size())?;

        let mut result = AllocationsTableWriter {
            storage,
            watermark: 0,
            max_num_nodes,
            free_list_head: NODE_INVALID,
        };

        // Clear the hash table.
        for bucket_index in 0..NUM_BUCKETS {
            result.bucket_head_at(bucket_index).store(NODE_INVALID, SeqCst);
        }

        Ok(result)
    }

    /// This is the hash function: it turns a block address into a bucket number.
    fn compute_bucket_index(address: u64) -> usize {
        // TODO(fdurso): The hash values generated by this function are not uniformly distributed.
        let tmp = (address >> 4) as usize;
        tmp % NUM_BUCKETS
    }

    /// Returns a mutable reference to the head of a given bucket's linked list.
    fn bucket_head_at(&mut self, bucket_index: usize) -> &mut AtomicNodeIndex {
        // The bucket heads are stored at the beginning of the VMO.
        let bucket_heads = self.storage.get_object_mut::<BucketHeads>(0).unwrap();
        &mut bucket_heads[bucket_index]
    }

    /// Returns a mutable reference to a given node.
    fn node_at(&mut self, node_index: NodeIndex) -> &mut Node {
        // The nodes are stored consecutively immediately after the bucket heads.
        let byte_offset = size_of::<BucketHeads>() + node_index as usize * size_of::<Node>();
        self.storage.get_object_mut::<Node>(byte_offset).unwrap()
    }

    /// Inserts a new entry in the hash table.
    pub fn insert_allocation(&mut self, address: u64, size: u64) -> Result<bool, crate::Error> {
        let bucket_index = Self::compute_bucket_index(address);
        let old_head = self.bucket_head_at(bucket_index).load(Relaxed);

        // Verify that no entry with the same address already exists.
        let mut curr_index = old_head;
        while curr_index != NODE_INVALID {
            let curr_data = self.node_at(curr_index);
            if curr_data.address == address {
                return Ok(false);
            }
            curr_index = curr_data.next.load(Relaxed);
        }

        // Insert a new entry at the head of the list.
        let new_index = self.pop_free_node()?;
        *self.node_at(new_index) = Node { address, size, next: AtomicNodeIndex::new(old_head) };
        self.bucket_head_at(bucket_index).store(new_index, SeqCst);
        Ok(true)
    }

    /// Removes an entry from the hash table and returns the value of the removed entry's size
    /// field.
    pub fn erase_allocation(&mut self, address: u64) -> Option<u64> {
        let bucket_index = Self::compute_bucket_index(address);

        // Search the entry to be removed.
        let mut prev_index = None;
        let mut curr_index = self.bucket_head_at(bucket_index).load(Relaxed);
        while curr_index != NODE_INVALID {
            let curr_data = self.node_at(curr_index);
            let curr_data_size = curr_data.size;
            let curr_data_next = curr_data.next.load(Relaxed);

            // Is this the entry we were looking for?
            if curr_data.address == address {
                if let Some(prev_index) = prev_index {
                    self.node_at(prev_index).next.store(curr_data_next, SeqCst);
                } else {
                    self.bucket_head_at(bucket_index).store(curr_data_next, SeqCst);
                }
                self.push_free_node(curr_index);
                return Some(curr_data_size);
            }

            prev_index = Some(curr_index);
            curr_index = curr_data.next.load(Relaxed);
        }

        // Not found.
        None
    }

    /// Inserts a node into the free list.
    fn push_free_node(&mut self, index: NodeIndex) {
        let current_head = self.free_list_head;

        let node_data = self.node_at(index);
        node_data.next.store(current_head, Relaxed);

        self.free_list_head = index;
    }

    /// Takes a node from the free list or allocates a new one if the free list is empty.
    fn pop_free_node(&mut self) -> Result<NodeIndex, crate::Error> {
        if self.free_list_head != NODE_INVALID {
            // Pop a node from the free list.
            let result = self.free_list_head;
            self.free_list_head = self.node_at(result).next.load(Relaxed);
            Ok(result)
        } else if (self.watermark as usize) < self.max_num_nodes {
            // Allocate one node with the watermark allocator.
            let result = self.watermark;
            self.watermark += 1;
            Ok(result)
        } else {
            // We are out of space.
            Err(crate::Error::OutOfSpace)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::alloc::Layout;

    // Some tests below use this constant to ensure that each bucket is been used at least 10 times.
    const NUM_ITERATIONS: usize = NUM_BUCKETS * 10;

    // Ensure we allocate enough nodes to store all the blocks the tests need plus some buffer.
    const NUM_NODES: usize = NUM_ITERATIONS + 100;

    struct TestStorage {
        vmo: zx::Vmo,
    }

    impl TestStorage {
        pub fn new(num_nodes: usize) -> TestStorage {
            let nodes_layout = Layout::array::<Node>(num_nodes).unwrap();
            let (layout, nodes_offset) = Layout::new::<BucketHeads>().extend(nodes_layout).unwrap();
            assert_eq!(nodes_offset, size_of::<BucketHeads>());

            let vmo = zx::Vmo::create(layout.size() as u64).unwrap();
            TestStorage { vmo }
        }

        fn create_writer(&self) -> AllocationsTableWriter {
            AllocationsTableWriter::new(&self.vmo).unwrap()
        }
    }

    #[test]
    fn test_cannot_insert_twice() {
        let storage = TestStorage::new(NUM_NODES);
        let mut writer = storage.create_writer();

        let result = writer.insert_allocation(0x1234, 0x5678);
        assert_eq!(result, Ok(true));

        let result = writer.insert_allocation(0x1234, 0x5678);
        assert_eq!(result, Ok(false));
    }

    #[test]
    fn test_cannot_erase_twice() {
        let storage = TestStorage::new(NUM_NODES);
        let mut writer = storage.create_writer();

        let result = writer.insert_allocation(0x1234, 0x5678);
        assert_eq!(result, Ok(true));

        let result = writer.erase_allocation(0x1234);
        assert_eq!(result, Some(0x5678));

        let result = writer.erase_allocation(0x1234);
        assert_eq!(result, None);
    }

    #[test]
    fn test_out_of_space() {
        let storage = TestStorage::new(NUM_NODES);
        let mut writer = storage.create_writer();

        // Test that inserting up to `NUM_NODES` works.
        for i in 0..NUM_NODES {
            let result = writer.insert_allocation(i as u64, 1);
            assert_eq!(result, Ok(true));
        }

        // Test that inserting an extra node fails.
        let result = writer.insert_allocation(NUM_NODES as u64, 1);
        assert_eq!(result, Err(crate::Error::OutOfSpace));

        // Test that removing an element and then inserting again succeeds.
        let result = writer.erase_allocation(0);
        assert_eq!(result, Some(1));
        let result = writer.insert_allocation(NUM_NODES as u64, 1);
        assert_eq!(result, Ok(true));
    }

    #[test]
    fn test_loop_insert_then_erase() {
        let storage = TestStorage::new(NUM_NODES);
        let mut writer = storage.create_writer();

        for i in 0..NUM_ITERATIONS {
            let result = writer.insert_allocation(i as u64, 1);
            assert_eq!(result, Ok(true), "failed to insert 0x{:x}", i);

            let result = writer.erase_allocation(i as u64);
            assert_eq!(result, Some(1), "failed to erase 0x{:x}", i);
        }
    }

    #[test]
    fn test_bulk_insert_then_erase_same_order() {
        let storage = TestStorage::new(NUM_NODES);
        let mut writer = storage.create_writer();

        for i in 0..NUM_ITERATIONS {
            let result = writer.insert_allocation(i as u64, 1);
            assert_eq!(result, Ok(true), "failed to insert 0x{:x}", i);
        }
        for i in 0..NUM_ITERATIONS {
            let result = writer.erase_allocation(i as u64);
            assert_eq!(result, Some(1), "failed to erase 0x{:x}", i);
        }
    }

    #[test]
    fn test_bulk_insert_then_erase_reverse_order() {
        let storage = TestStorage::new(NUM_NODES);
        let mut writer = storage.create_writer();

        for i in 0..NUM_ITERATIONS {
            let result = writer.insert_allocation(i as u64, 1);
            assert_eq!(result, Ok(true), "failed to insert 0x{:x}", i);
        }
        for i in (0..NUM_ITERATIONS).rev() {
            let result = writer.erase_allocation(i as u64);
            assert_eq!(result, Some(1), "failed to erase 0x{:x}", i);
        }
    }
}
