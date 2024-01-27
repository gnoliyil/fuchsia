// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::writer::{Inner, InnerPropertyType, InspectType, Property};
use tracing::error;

#[cfg(test)]
use inspect_format::{Block, Container};

/// Inspect String Property data type.
///
/// NOTE: do not rely on PartialEq implementation for true comparison.
/// Instead leverage the reader.
///
/// NOTE: Operations on a Default value are no-ops.
#[derive(Debug, PartialEq, Eq, Default)]
pub struct StringProperty {
    inner: Inner<InnerPropertyType>,
}

impl<'t> Property<'t> for StringProperty {
    type Type = &'t str;

    fn set(&self, value: &'t str) {
        if let Some(ref inner_ref) = self.inner.inner_ref() {
            inner_ref
                .state
                .try_lock()
                .and_then(|mut state| state.set_property(inner_ref.block_index, value.as_bytes()))
                .unwrap_or_else(|e| error!("Failed to set property. Error: {:?}", e));
        }
    }
}

impl InspectType for StringProperty {}

crate::impl_inspect_type_internal!(StringProperty);

#[cfg(test)]
impl StringProperty {
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
    use inspect_format::{BlockType, PropertyFormat};

    #[fuchsia::test]
    fn string_property() {
        // Create and use a default value.
        let default = StringProperty::default();
        default.set("test");

        let state = get_state(4096);
        let root = Node::new_root(state);
        let node = root.create_child("node");
        let node_block = node.get_block().unwrap();
        {
            let property = node.create_string("property", "test");
            let property_block = property.get_block().unwrap();
            assert_eq!(property_block.block_type(), BlockType::BufferValue);
            assert_eq!(property_block.total_length().unwrap(), 4);
            assert_eq!(property_block.property_format().unwrap(), PropertyFormat::String);
            assert_eq!(node_block.child_count().unwrap(), 1);

            property.set("test-set");
            assert_eq!(property_block.total_length().unwrap(), 8);
        }
        assert_eq!(node_block.child_count().unwrap(), 0);
    }
}
