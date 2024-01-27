// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::writer::{ArithmeticArrayProperty, ArrayProperty, Inner, InnerValueType, InspectType};
use tracing::error;

/// Inspect double array data type.
///
/// NOTE: do not rely on PartialEq implementation for true comparison.
/// Instead leverage the reader.
///
/// NOTE: Operations on a Default value are no-ops.
#[derive(Debug, PartialEq, Eq, Default)]
pub struct DoubleArrayProperty {
    pub(crate) inner: Inner<InnerValueType>,
}

impl InspectType for DoubleArrayProperty {}

crate::impl_inspect_type_internal!(DoubleArrayProperty);

impl ArrayProperty for DoubleArrayProperty {
    type Type = f64;

    fn set(&self, index: usize, value: impl Into<Self::Type>) {
        if let Some(ref inner_ref) = self.inner.inner_ref() {
            inner_ref
                .state
                .try_lock()
                .and_then(|mut state| {
                    state.set_array_double_slot(inner_ref.block_index, index, value.into())
                })
                .unwrap_or_else(|err| {
                    error!(?err, "Failed to set property");
                });
        }
    }

    fn clear(&self) {
        if let Some(ref inner_ref) = self.inner.inner_ref() {
            inner_ref
                .state
                .try_lock()
                .and_then(|mut state| state.clear_array(inner_ref.block_index, 0))
                .unwrap_or_else(|err| {
                    error!(?err, "Failed to clear property.");
                });
        }
    }
}

impl ArithmeticArrayProperty for DoubleArrayProperty {
    fn add(&self, index: usize, value: f64) {
        if let Some(ref inner_ref) = self.inner.inner_ref() {
            inner_ref
                .state
                .try_lock()
                .and_then(|mut state| {
                    state.add_array_double_slot(inner_ref.block_index, index, value)
                })
                .unwrap_or_else(|err| {
                    error!(?err, "Failed to add property");
                });
        }
    }

    fn subtract(&self, index: usize, value: f64) {
        if let Some(ref inner_ref) = self.inner.inner_ref() {
            inner_ref
                .state
                .try_lock()
                .and_then(|mut state| {
                    state.subtract_array_double_slot(inner_ref.block_index, index, value)
                })
                .unwrap_or_else(|err| {
                    error!(?err, "Failed to subtract property");
                });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        writer::{testing_utils::GetBlockExt, Inspector},
        Length,
    };

    #[fuchsia::test]
    fn test_double_array() {
        // Create and use a default value.
        let default = DoubleArrayProperty::default();
        default.add(1, 1.0);

        let inspector = Inspector::default();
        let root = inspector.root();
        let node = root.create_child("node");
        {
            let array = node.create_double_array("array_property", 5);
            assert_eq!(array.len().unwrap(), 5);

            array.set(0, 5.0);
            array.get_block(|block| {
                assert_eq!(block.array_get_double_slot(0).unwrap(), 5.0);
            });

            array.add(0, 5.3);
            array.get_block(|array_block| {
                assert_eq!(array_block.array_get_double_slot(0).unwrap(), 10.3);
            });

            array.subtract(0, 3.4);
            array.get_block(|array_block| {
                assert_eq!(array_block.array_get_double_slot(0).unwrap(), 6.9);
            });

            array.set(1, 2.5);
            array.set(3, -3.1);

            array.get_block(|array_block| {
                for (i, value) in [6.9, 2.5, 0.0, -3.1, 0.0].iter().enumerate() {
                    assert_eq!(array_block.array_get_double_slot(i).unwrap(), *value);
                }
            });

            array.clear();
            array.get_block(|array_block| {
                for i in 0..5 {
                    assert_eq!(0.0, array_block.array_get_double_slot(i).unwrap());
                }
            });

            node.get_block(|block| {
                assert_eq!(block.child_count().unwrap(), 1);
            });
        }
        node.get_block(|block| {
            assert_eq!(block.child_count().unwrap(), 0);
        });
    }
}
