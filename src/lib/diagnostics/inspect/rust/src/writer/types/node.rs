// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::writer::{
    private::InspectTypeInternal, BoolProperty, BytesProperty, DoubleArrayProperty,
    DoubleExponentialHistogramProperty, DoubleLinearHistogramProperty, DoubleProperty, Error,
    Inner, InnerType, InspectType, InspectTypeReparentable, Inspector, IntArrayProperty,
    IntExponentialHistogramProperty, IntLinearHistogramProperty, IntProperty, LazyNode, State,
    StringArrayProperty, StringProperty, StringReference, UintArrayProperty,
    UintExponentialHistogramProperty, UintLinearHistogramProperty, UintProperty, ValueList,
};
use diagnostics_hierarchy::{ArrayFormat, ExponentialHistogramParams, LinearHistogramParams};
use futures::future::BoxFuture;
use inspect_format::{constants, LinkNodeDisposition, PropertyFormat};

#[cfg(test)]
use inspect_format::{Block, Container};

/// Inspect Node data type.
///
/// NOTE: do not rely on PartialEq implementation for true comparison.
/// Instead leverage the reader.
///
/// NOTE: Operations on a Default value are no-ops.
#[derive(Debug, PartialEq, Eq, Default)]
pub struct Node {
    pub(crate) inner: Inner<InnerNodeType>,
}

impl InspectType for Node {}

crate::impl_inspect_type_internal!(Node);

impl Node {
    /// Create a weak reference to the original node. All operations on a weak
    /// reference have identical semantics to the original node for as long
    /// as the original node is live. After that, all operations are no-ops.
    pub fn clone_weak(&self) -> Node {
        Self { inner: self.inner.clone_weak() }
    }

    /// Add a child to this node.
    #[must_use]
    pub fn create_child(&self, name: impl Into<StringReference>) -> Node {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| state.create_node(name, inner_ref.block_index))
                    .map(|block| Node::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(Node::new_no_op())
    }

    /// Creates and keeps track of a child with the given `name`.
    pub fn record_child<F>(&self, name: impl Into<StringReference>, initialize: F)
    where
        F: FnOnce(&Node),
    {
        self.atomic_update(move |n| {
            let child = n.create_child(name);
            initialize(&child);
            n.record(child);
        });
    }

    /// Takes a function to execute as under a single lock of the Inspect VMO. This function
    /// receives a reference to the `Node` where this is called.
    pub fn atomic_update<F, R>(&self, update_fn: F) -> R
    where
        F: FnOnce(&Node) -> R,
    {
        match self.inner.inner_ref() {
            None => {
                // If the node was a no-op we still execute the `update_fn` even if all operations
                // inside it will be no-ops to return `R`.
                update_fn(&self)
            }
            Some(inner_ref) => {
                // Silently ignore the error when fail to lock (as in any regular operation).
                // All operations performed in the `update_fn` won't update the vmo
                // generation count since we'll be holding one lock here.
                inner_ref.state.begin_transaction();
                let result = update_fn(&self);
                inner_ref.state.end_transaction();
                result
            }
        }
    }

    /// Keeps track of the given property for the lifetime of the node.
    pub fn record(&self, property: impl InspectType + 'static) {
        self.inner.inner_ref().map(|inner_ref| inner_ref.data.record(property));
    }

    /// Drop all recorded data from the node.
    pub fn clear_recorded(&self) {
        self.inner.inner_ref().map(|inner_ref| inner_ref.data.clear());
    }

    /// Creates a new `IntProperty` with the given `name` and `value`.
    #[must_use]
    pub fn create_int(&self, name: impl Into<StringReference>, value: i64) -> IntProperty {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| {
                        state.create_int_metric(name, value, inner_ref.block_index)
                    })
                    .map(|block| IntProperty::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(IntProperty::new_no_op())
    }

    /// Records a new `IntProperty` with the given `name` and `value`.
    pub fn record_int(&self, name: impl Into<StringReference>, value: i64) {
        let property = self.create_int(name, value);
        self.record(property);
    }

    /// Creates a new `UintProperty` with the given `name` and `value`.
    #[must_use]
    pub fn create_uint(&self, name: impl Into<StringReference>, value: u64) -> UintProperty {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| {
                        state.create_uint_metric(name, value, inner_ref.block_index)
                    })
                    .map(|block| UintProperty::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(UintProperty::new_no_op())
    }

    /// Records a new `UintProperty` with the given `name` and `value`.
    pub fn record_uint(&self, name: impl Into<StringReference>, value: u64) {
        let property = self.create_uint(name, value);
        self.record(property);
    }

    /// Creates a new `DoubleProperty` with the given `name` and `value`.
    #[must_use]
    pub fn create_double(&self, name: impl Into<StringReference>, value: f64) -> DoubleProperty {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| {
                        state.create_double_metric(name, value, inner_ref.block_index)
                    })
                    .map(|block| DoubleProperty::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(DoubleProperty::new_no_op())
    }

    /// Records a new `DoubleProperty` with the given `name` and `value`.
    pub fn record_double(&self, name: impl Into<StringReference>, value: f64) {
        let property = self.create_double(name, value);
        self.record(property);
    }

    /// Creates a new `StringArrayProperty` with the given `name` and `slots`.
    #[must_use]
    pub fn create_string_array(
        &self,
        name: impl Into<StringReference>,
        slots: usize,
    ) -> StringArrayProperty {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| {
                        state.create_string_array(name, slots, inner_ref.block_index)
                    })
                    .map(|block| StringArrayProperty::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(StringArrayProperty::new_no_op())
    }

    /// Creates a new `IntArrayProperty` with the given `name` and `slots`.
    #[must_use]
    pub fn create_int_array(
        &self,
        name: impl Into<StringReference>,
        slots: usize,
    ) -> IntArrayProperty {
        self.create_int_array_internal(name, slots, ArrayFormat::Default)
    }

    #[must_use]
    pub(crate) fn create_int_array_internal(
        &self,
        name: impl Into<StringReference>,
        slots: usize,
        format: ArrayFormat,
    ) -> IntArrayProperty {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| {
                        state.create_int_array(name, slots, format, inner_ref.block_index)
                    })
                    .map(|block| IntArrayProperty::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(IntArrayProperty::new_no_op())
    }

    /// Creates a new `UintArrayProperty` with the given `name` and `slots`.
    #[must_use]
    pub fn create_uint_array(
        &self,
        name: impl Into<StringReference>,
        slots: usize,
    ) -> UintArrayProperty {
        self.create_uint_array_internal(name, slots, ArrayFormat::Default)
    }

    #[must_use]
    pub(crate) fn create_uint_array_internal(
        &self,
        name: impl Into<StringReference>,
        slots: usize,
        format: ArrayFormat,
    ) -> UintArrayProperty {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| {
                        state.create_uint_array(name, slots, format, inner_ref.block_index)
                    })
                    .map(|block| UintArrayProperty::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(UintArrayProperty::new_no_op())
    }

    /// Creates a new `DoubleArrayProperty` with the given `name` and `slots`.
    #[must_use]
    pub fn create_double_array(
        &self,
        name: impl Into<StringReference>,
        slots: usize,
    ) -> DoubleArrayProperty {
        self.create_double_array_internal(name, slots, ArrayFormat::Default)
    }

    #[must_use]
    pub(crate) fn create_double_array_internal(
        &self,
        name: impl Into<StringReference>,
        slots: usize,
        format: ArrayFormat,
    ) -> DoubleArrayProperty {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| {
                        state.create_double_array(name, slots, format, inner_ref.block_index)
                    })
                    .map(|block| DoubleArrayProperty::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(DoubleArrayProperty::new_no_op())
    }

    /// Creates a new `IntLinearHistogramProperty` with the given `name` and `params`.
    #[must_use]
    pub fn create_int_linear_histogram(
        &self,
        name: impl Into<StringReference>,
        params: LinearHistogramParams<i64>,
    ) -> IntLinearHistogramProperty {
        IntLinearHistogramProperty::new(name, params, &self)
    }

    /// Creates a new `UintLinearHistogramProperty` with the given `name` and `params`.
    #[must_use]
    pub fn create_uint_linear_histogram(
        &self,
        name: impl Into<StringReference>,
        params: LinearHistogramParams<u64>,
    ) -> UintLinearHistogramProperty {
        UintLinearHistogramProperty::new(name, params, &self)
    }

    /// Creates a new `DoubleLinearHistogramProperty` with the given `name` and `params`.
    #[must_use]
    pub fn create_double_linear_histogram(
        &self,
        name: impl Into<StringReference>,
        params: LinearHistogramParams<f64>,
    ) -> DoubleLinearHistogramProperty {
        DoubleLinearHistogramProperty::new(name, params, &self)
    }

    /// Creates a new `IntExponentialHistogramProperty` with the given `name` and `params`.
    #[must_use]
    pub fn create_int_exponential_histogram(
        &self,
        name: impl Into<StringReference>,
        params: ExponentialHistogramParams<i64>,
    ) -> IntExponentialHistogramProperty {
        IntExponentialHistogramProperty::new(name, params, &self)
    }

    /// Creates a new `UintExponentialHistogramProperty` with the given `name` and `params`.
    #[must_use]
    pub fn create_uint_exponential_histogram(
        &self,
        name: impl Into<StringReference>,
        params: ExponentialHistogramParams<u64>,
    ) -> UintExponentialHistogramProperty {
        UintExponentialHistogramProperty::new(name, params, &self)
    }

    /// Creates a new `DoubleExponentialHistogramProperty` with the given `name` and `params`.
    #[must_use]
    pub fn create_double_exponential_histogram(
        &self,
        name: impl Into<StringReference>,
        params: ExponentialHistogramParams<f64>,
    ) -> DoubleExponentialHistogramProperty {
        DoubleExponentialHistogramProperty::new(name, params, &self)
    }

    /// Creates a new lazy child with the given `name` and `callback`.
    #[must_use]
    pub fn create_lazy_child<F>(&self, name: impl Into<StringReference>, callback: F) -> LazyNode
    where
        F: Fn() -> BoxFuture<'static, Result<Inspector, anyhow::Error>> + Sync + Send + 'static,
    {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| {
                        state.create_lazy_node(
                            name,
                            inner_ref.block_index,
                            LinkNodeDisposition::Child,
                            callback,
                        )
                    })
                    .map(|block| LazyNode::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(LazyNode::new_no_op())
    }

    /// Records a new lazy child with the given `name` and `callback`.
    pub fn record_lazy_child<F>(&self, name: impl Into<StringReference>, callback: F)
    where
        F: Fn() -> BoxFuture<'static, Result<Inspector, anyhow::Error>> + Sync + Send + 'static,
    {
        let property = self.create_lazy_child(name, callback);
        self.record(property);
    }

    /// Creates a new inline lazy node with the given `name` and `callback`.
    #[must_use]
    pub fn create_lazy_values<F>(&self, name: impl Into<StringReference>, callback: F) -> LazyNode
    where
        F: Fn() -> BoxFuture<'static, Result<Inspector, anyhow::Error>> + Sync + Send + 'static,
    {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| {
                        state.create_lazy_node(
                            name,
                            inner_ref.block_index,
                            LinkNodeDisposition::Inline,
                            callback,
                        )
                    })
                    .map(|block| LazyNode::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(LazyNode::new_no_op())
    }

    /// Records a new inline lazy node with the given `name` and `callback`.
    pub fn record_lazy_values<F>(&self, name: impl Into<StringReference>, callback: F)
    where
        F: Fn() -> BoxFuture<'static, Result<Inspector, anyhow::Error>> + Sync + Send + 'static,
    {
        let property = self.create_lazy_values(name, callback);
        self.record(property);
    }

    /// Add a string property to this node.
    #[must_use]
    pub fn create_string(
        &self,
        name: impl Into<StringReference>,
        value: impl AsRef<str>,
    ) -> StringProperty {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| {
                        state.create_property(
                            name,
                            value.as_ref().as_bytes(),
                            PropertyFormat::String,
                            inner_ref.block_index,
                        )
                    })
                    .map(|block| StringProperty::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(StringProperty::new_no_op())
    }

    /// Creates and saves a string property for the lifetime of the node.
    pub fn record_string(&self, name: impl Into<StringReference>, value: impl AsRef<str>) {
        let property = self.create_string(name, value);
        self.record(property);
    }

    /// Add a byte vector property to this node.
    #[must_use]
    pub fn create_bytes(
        &self,
        name: impl Into<StringReference>,
        value: impl AsRef<[u8]>,
    ) -> BytesProperty {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| {
                        state.create_property(
                            name,
                            value.as_ref(),
                            PropertyFormat::Bytes,
                            inner_ref.block_index,
                        )
                    })
                    .map(|block| BytesProperty::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(BytesProperty::new_no_op())
    }

    /// Creates and saves a bytes property for the lifetime of the node.
    pub fn record_bytes(&self, name: impl Into<StringReference>, value: impl AsRef<[u8]>) {
        let property = self.create_bytes(name, value);
        self.record(property);
    }

    /// Add a bool property to this node.
    #[must_use]
    pub fn create_bool(&self, name: impl Into<StringReference>, value: bool) -> BoolProperty {
        self.inner
            .inner_ref()
            .and_then(|inner_ref| {
                inner_ref
                    .state
                    .try_lock()
                    .and_then(|mut state| state.create_bool(name, value, inner_ref.block_index))
                    .map(|block| BoolProperty::new(inner_ref.state.clone(), block.index()))
                    .ok()
            })
            .unwrap_or(BoolProperty::new_no_op())
    }

    /// Creates and saves a bool property for the lifetime of the node.
    pub fn record_bool(&self, name: impl Into<StringReference>, value: bool) {
        let property = self.create_bool(name, value);
        self.record(property);
    }

    /// Takes a child from its parent and adopts it into its own tree.
    pub fn adopt<T: InspectTypeReparentable>(&self, child: &T) -> Result<(), Error> {
        child.reparent(self)
    }

    /// Returns the [`Block`][Block] associated with this value.
    #[cfg(test)]
    pub(crate) fn get_block(&self) -> Option<Block<Container>> {
        self.inner.inner_ref().and_then(|inner_ref| {
            inner_ref
                .state
                .try_lock()
                .and_then(|state| state.heap().get_block(inner_ref.block_index))
                .ok()
        })
    }

    /// Creates a new root node.
    pub(crate) fn new_root(state: State) -> Node {
        Node::new(state, constants::ROOT_INDEX)
    }
}

#[derive(Default, Debug)]
pub(crate) struct InnerNodeType;

impl InnerType for InnerNodeType {
    // Each node has a list of recorded values.
    type Data = ValueList;

    fn free(state: &State, block_index: u32) -> Result<(), Error> {
        if block_index == constants::ROOT_INDEX {
            return Ok(());
        }
        let mut state_lock = state.try_lock()?;
        state_lock.free_value(block_index).map_err(|err| Error::free("node", block_index, err))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        assert_json_diff, reader,
        writer::{private::InspectTypeInternal, testing_utils::get_state, ArrayProperty},
    };
    use diagnostics_hierarchy::{assert_data_tree, DiagnosticsHierarchy};
    use futures::FutureExt;
    use inspect_format::BlockType;

    #[fuchsia::test]
    fn node() {
        // Create and use a default value.
        let default = Node::default();
        default.record_int("a", 0);

        let state = get_state(4096);
        let root = Node::new_root(state);
        let node = root.create_child("node");
        let node_block = node.get_block().unwrap();
        assert_eq!(node_block.block_type(), BlockType::NodeValue);
        assert_eq!(node_block.child_count().unwrap(), 0);
        {
            let child = node.create_child("child");
            let child_block = child.get_block().unwrap();
            assert_eq!(child_block.block_type(), BlockType::NodeValue);
            assert_eq!(child_block.child_count().unwrap(), 0);
            assert_eq!(node_block.child_count().unwrap(), 1);
        }
        assert_eq!(node_block.child_count().unwrap(), 0);
    }

    #[fuchsia::test]
    async fn lazy_child() {
        let inspector = Inspector::default();
        let _lazy = inspector.root().create_lazy_child("lazy-1", || {
            async move {
                let insp = Inspector::default();
                insp.root().record_lazy_child("parent", || {
                    async move {
                        let insp2 = Inspector::default();
                        insp2.root().record_int("create-lazy-child", 0);
                        insp2.root().record_int("create-lazy-child-2", 2);
                        Ok(insp2)
                    }
                    .boxed()
                });
                Ok(insp)
            }
            .boxed()
        });

        inspector.root().record_lazy_child("lazy-2", || {
            async move {
                let insp = Inspector::default();
                insp.root().record_bool("recorded-lazy-child", true);
                Ok(insp)
            }
            .boxed()
        });

        inspector.root().record_lazy_values("lazy", || {
            async move {
                let insp = Inspector::default();
                insp.root().record_bool("recorded-lazy-values", true);
                Ok(insp)
            }
            .boxed()
        });

        let result = reader::read(&inspector).await.unwrap();

        assert_data_tree!(result, root: {
            "lazy-1": {
                "parent": {
                    "create-lazy-child": 0i64,
                    "create-lazy-child-2": 2i64,
                },
            },
            "lazy-2": {
                "recorded-lazy-child": true,
            },
            "recorded-lazy-values": true,
        });
    }

    #[fuchsia::test]
    fn test_adoption() {
        let insp = Inspector::default();
        let root = insp.root();
        let a = root.create_child("a");
        let b = root.create_child("b");
        let c = b.create_child("c");

        assert_data_tree!(insp, root: {
            a: {},
            b: {
                c: {},
            },
        });

        a.adopt(&b).unwrap();

        assert_data_tree!(insp, root: {
            a: {
                b: {
                    c: {},
                },
            },
        });

        assert!(c.adopt(&a).is_err());
        assert!(c.adopt(&b).is_err());
        assert!(b.adopt(&a).is_err());
        assert!(a.adopt(root).is_err());
        assert!(a.adopt(&a).is_err());

        {
            let d = root.create_int("d", 4);

            assert_data_tree!(insp, root: {
                a: {
                    b: {
                        c: {},
                    },
                },
                d: 4i64,
            });

            c.adopt(&d).unwrap();

            assert_data_tree!(insp, root: {
                a: {
                    b: {
                        c: {
                            d: 4i64,
                        },
                    },
                },
            });
        }

        assert_data_tree!(insp, root: {
            a: {
                b: {
                    c: {},
                },
            },
        });
    }

    #[fuchsia::test]
    fn node_no_op_clone_weak() {
        let default = Node::default();
        assert!(!default.is_valid());
        let weak = default.clone_weak();
        assert!(!weak.is_valid());
        let _ = weak.create_child("child");
        std::mem::drop(default);
        let _ = weak.create_uint("age", 1337);
        assert!(!weak.is_valid());
    }

    #[fuchsia::test]
    fn node_clone_weak() {
        let state = get_state(4096);
        let root = Node::new_root(state);
        let node = root.create_child("node");
        let node_weak = node.clone_weak();
        let node_weak_2 = node_weak.clone_weak(); // Weak from another weak

        let node_block = node.get_block().unwrap();
        assert_eq!(node_block.block_type(), BlockType::NodeValue);
        assert_eq!(node_block.child_count().unwrap(), 0);
        let node_weak_block = node.get_block().unwrap();
        assert_eq!(node_weak_block.block_type(), BlockType::NodeValue);
        assert_eq!(node_weak_block.child_count().unwrap(), 0);
        let node_weak_2_block = node.get_block().unwrap();
        assert_eq!(node_weak_2_block.block_type(), BlockType::NodeValue);
        assert_eq!(node_weak_2_block.child_count().unwrap(), 0);

        let child_from_strong = node.create_child("child");
        let child = node_weak.create_child("child_1");
        let child_2 = node_weak_2.create_child("child_2");
        std::mem::drop(node_weak_2);
        assert_eq!(node_weak_block.child_count().unwrap(), 3);
        std::mem::drop(child_from_strong);
        assert_eq!(node_weak_block.child_count().unwrap(), 2);
        std::mem::drop(child);
        assert_eq!(node_weak_block.child_count().unwrap(), 1);
        assert!(node_weak.is_valid());
        assert!(child_2.is_valid());
        std::mem::drop(node);
        assert!(!node_weak.is_valid());
        let _ = node_weak.create_child("orphan");
        let _ = child_2.create_child("orphan");
    }

    #[fuchsia::test]
    fn dummy_partialeq() {
        let inspector = Inspector::default();
        let root = inspector.root();

        // Types should all be equal to another type. This is to enable clients
        // with inspect types in their structs be able to derive PartialEq and
        // Eq smoothly.
        assert_eq!(root, &root.create_child("child1"));
        assert_eq!(root.create_int("property1", 1), root.create_int("property2", 2));
        assert_eq!(root.create_double("property1", 1.0), root.create_double("property2", 2.0));
        assert_eq!(root.create_uint("property1", 1), root.create_uint("property2", 2));
        assert_eq!(
            root.create_string("property1", "value1"),
            root.create_string("property2", "value2")
        );
        assert_eq!(
            root.create_bytes("property1", b"value1"),
            root.create_bytes("property2", b"value2")
        );
    }

    #[fuchsia::test]
    fn record() {
        let inspector = Inspector::default();
        let property = inspector.root().create_uint("a", 1);
        inspector.root().record_uint("b", 2);
        {
            let child = inspector.root().create_child("child");
            child.record(property);
            child.record_double("c", 3.25);
            assert_data_tree!(inspector, root: {
                a: 1u64,
                b: 2u64,
                child: {
                    c: 3.25,
                }
            });
        }
        // `child` went out of scope, meaning it was deleted.
        // Property `a` should be gone as well, given that it was being tracked by `child`.
        assert_data_tree!(inspector, root: {
            b: 2u64,
        });

        inspector.root().clear_recorded();
        assert_data_tree!(inspector, root: {});
    }

    #[fuchsia::test]
    fn clear_recorded() {
        let inspector = Inspector::default();
        let one = inspector.root().create_child("one");
        let two = inspector.root().create_child("two");
        let one_recorded = one.create_child("one_recorded");
        let two_recorded = two.create_child("two_recorded");

        one.record(one_recorded);
        two.record(two_recorded);

        assert_json_diff!(inspector, root: {
            one: {
                one_recorded: {},
            },
            two: {
                two_recorded: {},
            },
        });

        two.clear_recorded();

        assert_json_diff!(inspector, root: {
            one: {
                one_recorded: {},
            },
            two: {},
        });

        one.clear_recorded();

        assert_json_diff!(inspector, root: {
            one: {},
            two: {},
        });
    }

    #[fuchsia::test]
    fn record_child() {
        let inspector = Inspector::default();
        inspector.root().record_child("test", |node| {
            node.record_int("a", 1);
        });
        assert_data_tree!(inspector, root: {
            test: {
                a: 1i64,
            }
        })
    }

    #[fuchsia::test]
    fn record_weak() {
        let inspector = Inspector::default();
        let main = inspector.root().create_child("main");
        let main_weak = main.clone_weak();
        let property = main_weak.create_uint("a", 1);

        // Ensure either the weak or strong reference can be used for recording
        main_weak.record_uint("b", 2);
        main.record_uint("c", 3);
        {
            let child = main_weak.create_child("child");
            child.record(property);
            child.record_double("c", 3.25);
            assert_data_tree!(inspector, root: { main: {
                a: 1u64,
                b: 2u64,
                c: 3u64,
                child: {
                    c: 3.25,
                }
            }});
        }
        // `child` went out of scope, meaning it was deleted.
        // Property `a` should be gone as well, given that it was being tracked by `child`.
        assert_data_tree!(inspector, root: { main: {
            b: 2u64,
            c: 3u64
        }});
        std::mem::drop(main);
        // Recording after dropping a strong reference is a no-op
        main_weak.record_double("d", 1.0);
        // Verify that dropping a strong reference cleans up the state
        assert_data_tree!(inspector, root: { });
    }

    #[fuchsia::test]
    fn string_arrays_on_record() {
        let inspector = Inspector::default();
        inspector.root().record_child("child", |node| {
            node.record_int("my_int", 1i64);

            let arr: crate::StringArrayProperty = node.create_string_array("my_string_array", 1);
            arr.set(0, "test");
            node.record(arr);
        });
        assert_data_tree!(inspector, root: {
            child: {
                my_int: 1i64,
                my_string_array: vec!["test"]
            }
        });
    }
}

// Tests that either refer explicitly to VMOs or utilize zircon signals.
#[cfg(all(test, target_os = "fuchsia"))]
mod fuchsia_tests {
    use super::*;
    use crate::{assert_json_diff, hierarchy::DiagnosticsHierarchy, reader, NumericProperty};
    use fuchsia_zircon::{self as zx, AsHandleRef, Peered};
    use std::convert::TryFrom;

    #[fuchsia::test]
    async fn atomic_update_reader() {
        let inspector = Inspector::default();

        // Spawn a read thread that holds a duplicate handle to the VMO that will be written.
        let vmo = inspector.duplicate_vmo().expect("duplicate vmo handle");
        let (p1, p2) = zx::EventPair::create();

        macro_rules! notify_and_wait_reader {
            () => {
                p1.signal_peer(zx::Signals::NONE, zx::Signals::USER_0).unwrap();
                p1.wait_handle(zx::Signals::USER_0, zx::Time::INFINITE).unwrap();
                p1.signal_handle(zx::Signals::USER_0, zx::Signals::NONE).unwrap();
            };
        }

        macro_rules! wait_and_notify_writer {
            ($code:block) => {
              p2.wait_handle(zx::Signals::USER_0, zx::Time::INFINITE).unwrap();
              p2.signal_handle(zx::Signals::USER_0, zx::Signals::NONE).unwrap();
              $code
              p2.signal_peer(zx::Signals::NONE, zx::Signals::USER_0).unwrap();
            }
        }

        let thread = std::thread::spawn(move || {
            // Before running the atomic update.
            wait_and_notify_writer! {{
                let hierarchy: DiagnosticsHierarchy<String> =
                    reader::PartialNodeHierarchy::try_from(&vmo).unwrap().into();
                assert_eq!(hierarchy, DiagnosticsHierarchy::new_root());
            }};
            // After: create_child("child"): Assert that the VMO is in use (locked) and we can't
            // read.
            wait_and_notify_writer! {{
                assert!(reader::PartialNodeHierarchy::try_from(&vmo).is_err());
            }};
            // After: record_int("a"): Assert that the VMO is in use (locked) and we can't
            // read.
            wait_and_notify_writer! {{
                assert!(reader::PartialNodeHierarchy::try_from(&vmo).is_err());
            }};
            // After: record_int("b"): Assert that the VMO is in use (locked) and we can't
            // read.
            wait_and_notify_writer! {{
                assert!(reader::PartialNodeHierarchy::try_from(&vmo).is_err());
            }};
            // After atomic update
            wait_and_notify_writer! {{
                let hierarchy: DiagnosticsHierarchy<String> =
                    reader::PartialNodeHierarchy::try_from(&vmo).unwrap().into();
                assert_json_diff!(hierarchy, root: {
                   value: 2i64,
                   child: {
                       a: 1i64,
                       b: 2i64,
                   }
                });
            }};
        });

        // Perform the atomic update
        let mut child = Node::default();
        notify_and_wait_reader!();
        let int_val = inspector.root().create_int("value", 1);
        inspector
            .root()
            .atomic_update(|node| {
                // Intentionally make this slow to assert an atomic update in the reader.
                child = node.create_child("child");
                notify_and_wait_reader!();
                child.record_int("a", 1);
                notify_and_wait_reader!();
                child.record_int("b", 2);
                notify_and_wait_reader!();
                int_val.add(1);
                Ok::<(), Error>(())
            })
            .expect("successful atomic update");
        notify_and_wait_reader!();

        // Wait for the reader thread to successfully finish.
        let _ = thread.join();

        // Ensure that the variable that we mutated internally can be used.
        child.record_int("c", 3);
        assert_json_diff!(inspector, root: {
            value: 2i64,
            child: {
                a: 1i64,
                b: 2i64,
                c: 3i64,
            }
        });
    }
}
