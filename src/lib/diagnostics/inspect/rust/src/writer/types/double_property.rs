// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::writer::{Error, Inner, InnerValueType, InspectType, NumericProperty, Property};
use tracing::error;

/// Inspect double property type.
///
/// NOTE: do not rely on PartialEq implementation for true comparison.
/// Instead leverage the reader.
///
/// NOTE: Operations on a Default value are no-ops.
#[derive(Debug, PartialEq, Eq, Default)]
pub struct DoubleProperty {
    inner: Inner<InnerValueType>,
}

impl InspectType for DoubleProperty {}

crate::impl_inspect_type_internal!(DoubleProperty);

impl<'t> Property<'t> for DoubleProperty {
    type Type = f64;

    fn set(&self, value: f64) {
        if let Some(ref inner_ref) = self.inner.inner_ref() {
            inner_ref
                .state
                .try_lock()
                .and_then(|mut state| state.set_double_metric(inner_ref.block_index, value))
                .unwrap_or_else(|err| {
                    error!(?err, "Failed to set property");
                });
        }
    }
}

impl NumericProperty<'_> for DoubleProperty {
    fn add(&self, value: f64) {
        if let Some(ref inner_ref) = self.inner.inner_ref() {
            inner_ref
                .state
                .try_lock()
                .and_then(|mut state| state.add_double_metric(inner_ref.block_index, value))
                .unwrap_or_else(|err| {
                    error!(?err, "Failed to set property");
                });
        }
    }

    fn subtract(&self, value: f64) {
        if let Some(ref inner_ref) = self.inner.inner_ref() {
            inner_ref
                .state
                .try_lock()
                .and_then(|mut state| state.subtract_double_metric(inner_ref.block_index, value))
                .unwrap_or_else(|err| {
                    error!(?err, "Failed to set property");
                });
        }
    }

    fn get(&self) -> Result<f64, Error> {
        if let Some(ref inner_ref) = self.inner.inner_ref() {
            inner_ref
                .state
                .try_lock()
                .and_then(|state| state.get_double_metric(inner_ref.block_index))
        } else {
            Err(Error::NoOp("Property"))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::writer::{
        testing_utils::{get_state, GetBlockExt},
        Node,
    };
    use inspect_format::BlockType;

    #[fuchsia::test]
    fn double_property() {
        // Create and use a default value.
        let default = DoubleProperty::default();
        default.add(1.0);

        let state = get_state(4096);
        let root = Node::new_root(state);
        let node = root.create_child("node");
        {
            let property = node.create_double("property", 1.0);
            property.get_block(|property_block| {
                assert_eq!(property_block.block_type(), BlockType::DoubleValue);
                assert_eq!(property_block.double_value().unwrap(), 1.0);
            });
            node.get_block(|node_block| {
                assert_eq!(node_block.child_count().unwrap(), 1);
            });

            property.set(2.0);
            property.get_block(|property_block| {
                assert_eq!(property_block.double_value().unwrap(), 2.0);
            });
            assert_eq!(property.get().unwrap(), 2.0);

            property.subtract(5.5);
            property.get_block(|property_block| {
                assert_eq!(property_block.double_value().unwrap(), -3.5);
            });

            property.add(8.1);
            property.get_block(|property_block| {
                assert_eq!(property_block.double_value().unwrap(), 4.6);
            });
        }
        node.get_block(|node_block| {
            assert_eq!(node_block.child_count().unwrap(), 0);
        });
    }
}
