// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::writer::{Inner, InnerValueType, InspectType, Property};
use tracing::error;

#[cfg(test)]
use inspect_format::{Block, Container};

/// Inspect API Bool Property data type.
///
/// NOTE: do not rely on PartialEq implementation for true comparison.
/// Instead leverage the reader.
///
/// NOTE: Operations on a Default value are no-ops.
#[derive(Debug, PartialEq, Eq, Default)]
pub struct BoolProperty {
    inner: Inner<InnerValueType>,
}

impl<'t> Property<'t> for BoolProperty {
    type Type = bool;

    fn set(&self, value: bool) {
        if let Some(ref inner_ref) = self.inner.inner_ref() {
            inner_ref
                .state
                .try_lock()
                .and_then(|state| state.set_bool(inner_ref.block_index, value))
                .unwrap_or_else(|e| {
                    error!("Failed to set property. Error: {:?}", e);
                });
        }
    }
}

impl InspectType for BoolProperty {}

crate::impl_inspect_type_internal!(BoolProperty);

#[cfg(test)]
impl BoolProperty {
    /// Returns the [`Block`][Block] associated with this value.
    pub fn get_block(&self) -> Option<Block<Container>> {
        self.inner.inner_ref().and_then(|inner_ref| {
            inner_ref
                .state
                .try_lock()
                .and_then(|state| state.heap().get_block(inner_ref.block_index))
                .ok()
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::writer::{testing_utils::get_state, Node};
    use inspect_format::BlockType;

    #[fuchsia::test]
    fn bool_property() {
        // Create and use a default value.
        let default = BoolProperty::default();
        default.set(true);

        let state = get_state(4096);
        let root = Node::new_root(state);
        let node = root.create_child("node");
        let node_block = node.get_block().unwrap();
        {
            let property = node.create_bool("property", true);
            let property_block = property.get_block().unwrap();
            assert_eq!(property_block.block_type(), BlockType::BoolValue);
            assert_eq!(property_block.bool_value().unwrap(), true);
            assert_eq!(node_block.child_count().unwrap(), 1);

            property.set(false);
            assert_eq!(property_block.bool_value().unwrap(), false);
        }
        assert_eq!(node_block.child_count().unwrap(), 0);
    }
}
