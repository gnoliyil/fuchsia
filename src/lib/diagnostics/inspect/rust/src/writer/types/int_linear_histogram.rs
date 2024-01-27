// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::writer::{
    ArithmeticArrayProperty, ArrayProperty, HistogramProperty, IntArrayProperty, Node,
    StringReference,
};
use diagnostics_hierarchy::{ArrayFormat, LinearHistogramParams};
use inspect_format::constants;
use tracing::error;

#[derive(Debug)]
/// A linear histogram property for integer values.
pub struct IntLinearHistogramProperty {
    array: IntArrayProperty,
    floor: i64,
    slots: usize,
    step_size: i64,
}

impl IntLinearHistogramProperty {
    pub(crate) fn new(
        name: StringReference,
        params: LinearHistogramParams<i64>,
        parent: &Node,
    ) -> Self {
        let slots = params.buckets + constants::LINEAR_HISTOGRAM_EXTRA_SLOTS;
        let array = parent.create_int_array_internal(name, slots, ArrayFormat::LinearHistogram);
        array.set(0, params.floor);
        array.set(1, params.step_size);
        Self { floor: params.floor, step_size: params.step_size, slots, array }
    }

    fn get_index(&self, value: i64) -> usize {
        let mut current_floor = self.floor;
        // Start in the underflow index.
        let mut index = constants::LINEAR_HISTOGRAM_EXTRA_SLOTS - 2;
        while value >= current_floor && index < self.slots - 1 {
            current_floor += self.step_size;
            index += 1;
        }
        index as usize
    }
}

impl HistogramProperty for IntLinearHistogramProperty {
    type Type = i64;

    fn insert(&self, value: i64) {
        self.insert_multiple(value, 1);
    }

    fn insert_multiple(&self, value: i64, count: usize) {
        self.array.add(self.get_index(value), count as i64);
    }

    fn clear(&self) {
        if let Some(ref inner_ref) = self.array.inner.inner_ref() {
            // Ensure we don't delete the array slots that contain histogram metadata.
            inner_ref
                .state
                .try_lock()
                .and_then(|mut state| {
                    // -2 = the overflow and underflow slots which still need to be cleared.
                    state.clear_array(
                        inner_ref.block_index,
                        constants::LINEAR_HISTOGRAM_EXTRA_SLOTS - 2,
                    )
                })
                .unwrap_or_else(|err| {
                    error!(?err, "Failed to clear property");
                });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        writer::{testing_utils::GetBlockExt, Inspector},
        LinearHistogramParams,
    };

    #[fuchsia::test]
    fn int_linear_histogram() {
        let inspector = Inspector::default();
        let root = inspector.root();
        let node = root.create_child("node");
        {
            let int_histogram = node.create_int_linear_histogram(
                "int-histogram",
                LinearHistogramParams { floor: 10, step_size: 5, buckets: 5 },
            );
            int_histogram.insert_multiple(-1, 2); // underflow
            int_histogram.insert(25);
            int_histogram.insert(500); // overflow
            int_histogram.array.get_block(|block| {
                for (i, value) in [10, 5, 2, 0, 0, 0, 1, 0, 1].iter().enumerate() {
                    assert_eq!(block.array_get_int_slot(i).unwrap(), *value);
                }
            });

            node.get_block(|node_block| {
                assert_eq!(node_block.child_count().unwrap(), 1);
            });
        }
        node.get_block(|node_block| {
            assert_eq!(node_block.child_count().unwrap(), 0);
        });
    }
}
