// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Provides utilities on top of regular inspect properties to enable convenient features such as:
//!
//! - Directly mutating a value and dumping it to inspect
//!
//!   ```rust
//!   use fuchsia_inspect::component;
//!   use fuchsia_inspect::contrib::inspectable::InspectableU64;
//!
//!   let inspectable = InspectableU64::new(0, component::inspector::root(), "foo");
//!   *inspectable.get_mut() += 1;
//!   // The value of the u64 is 1 now and it was updated automatically in the inspect vmo.
//!   ```
//!
//! - Dump lengths automatically
//!
//!   ```rust
//!   use fuchsia_inspect::component;
//!   use fuchsia_inspect::contrib::inspectable::InspectableLen;
//!
//!   let inspectable = InspectableLen::new(vec![0], component::inspector::root(), "foo");
//!   // The value of the vector is [0] and the value in the inspect vmo under the foo property
//!   // is 1.
//!   *inspectable.get_mut().push(3);
//!   // The value of the vector is [0, 3] and the value in the inspect vmo under the foo property
//!   // is 2.
//!   ```
//!
//! - Dump debug representations automatically, etc.
//!
//!   ```rust
//!   use fuchsia_inspect::component;
//!   use fuchsia_inspect::contrib::inspectable::InspectableDebugString;
//!
//!   let inspectable = InspectableDebugString::new(vec![0], component::inspector::root(), "foo");
//!   // The value of the vector is vec![0] and the value in the inspect vmo under the foo property
//!   // is the debug string of the vector "[0]".
//!   *inspectable.get_mut().push(3);
//!   // The value of the vector is vec![0, 3] and the value in the inspect vmo under the foo
//!   // property is the debug string of the vector "[0, 3]".
//!   ```

use {
    core::ops::{Deref, DerefMut},
    derivative::Derivative,
    fuchsia_inspect::{
        BoolProperty, Node, Property, StringProperty, StringReference, UintProperty,
    },
    std::{borrow::Borrow, collections::HashSet},
};

/// Generic wrapper for exporting variables via Inspect. Mutations to
/// the wrapped `value` occur through an `InspectableGuard`, which
/// notifies the `watcher` when `Drop`ped, transparently keeping the Inspect
/// state up-to-date.
///
/// How the `value` is exported is determined by the `Watch` implementation,
/// see e.g. `InspectableDebugString`.
///
/// Not correct for `V`s with interior mutability, because `Inspectable`
/// `Deref`s to `V`, mutations to which will bypass the `watcher`.
#[derive(Derivative)]
#[derivative(Debug, Eq, PartialEq, Hash)]
pub struct Inspectable<V, W>
where
    W: Watch<V>,
{
    value: V,
    #[derivative(Debug = "ignore", PartialEq = "ignore", Hash = "ignore")]
    watcher: W,
}

impl<V, W> Inspectable<V, W>
where
    W: Watch<V>,
{
    /// Creates an `Inspectable` wrapping `value`. Exports `value` via Inspect.
    pub fn new<'b>(value: V, node: &Node, name: impl Into<StringReference<'b>>) -> Self {
        let watcher = W::new(&value, node, name);
        Self { value, watcher }
    }

    /// Returns a guard that `DerefMut`s to the wrapped `value`. The `watcher`
    /// will receive the updated `value` when the guard is `Drop`ped.
    pub fn get_mut(&mut self) -> InspectableGuard<'_, V, W> {
        InspectableGuard { value: &mut self.value, watcher: &mut self.watcher }
    }
}

impl<V, W> Deref for Inspectable<V, W>
where
    W: Watch<V>,
{
    type Target = V;

    fn deref(&self) -> &Self::Target {
        &self.value
    }
}

impl<V, W> Borrow<V> for Inspectable<V, W>
where
    W: Watch<V>,
{
    fn borrow(&self) -> &V {
        &self.value
    }
}

/// Used for exporting an `[Inspectable`][Inspectable]'s wrapped value .
pub trait Watch<V> {
    /// Used by [`Inspectable::new()`][Inspectable::new] to create a `Watch`er that exports via
    /// Inspect the [`Inspectable`][Inspectable]'s wrapped `value`.
    fn new<'b>(value: &V, node: &Node, name: impl Into<StringReference<'b>>) -> Self;

    /// Called by [`InspectableGuard`][InspectableGuard] when the guard is dropped, letting the
    /// `Watch`er update its state with the updated `value`.
    fn watch(&mut self, value: &V);
}

/// Calls self.watcher.watch(self.value) when dropped.
#[must_use]
pub struct InspectableGuard<'a, V, W>
where
    W: Watch<V>,
{
    value: &'a mut V,
    watcher: &'a mut W,
}

impl<'a, V, W> Deref for InspectableGuard<'a, V, W>
where
    W: Watch<V>,
{
    type Target = V;

    fn deref(&self) -> &Self::Target {
        self.value
    }
}

impl<'a, V, W> DerefMut for InspectableGuard<'a, V, W>
where
    W: Watch<V>,
{
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.value
    }
}

impl<'a, V, W> Drop for InspectableGuard<'a, V, W>
where
    W: Watch<V>,
{
    fn drop(&mut self) {
        self.watcher.watch(self.value);
    }
}

/// Exports via an Inspect `UintProperty` the `len` of the wrapped container.
#[derive(Debug)]
pub struct InspectableLenWatcher {
    len: fuchsia_inspect::UintProperty,
}

/// Trait implemented by types that can provide a length. Values used for
/// [`InspectableLen`][InspectableLen] must implement this.
pub trait Len {
    fn len(&self) -> usize;
}

impl<V> Watch<V> for InspectableLenWatcher
where
    V: Len,
{
    fn new<'b>(value: &V, node: &Node, name: impl Into<StringReference<'b>>) -> Self {
        Self { len: node.create_uint(name, value.len() as u64) }
    }

    fn watch(&mut self, value: &V) {
        self.len.set(value.len() as u64);
    }
}

/// Exports via an Inspect `UintProperty` the `len` of the wrapped value `V`.
pub type InspectableLen<V> = Inspectable<V, InspectableLenWatcher>;

impl<V> Len for Vec<V> {
    fn len(&self) -> usize {
        self.len()
    }
}

impl<V> Len for HashSet<V> {
    fn len(&self) -> usize {
        self.len()
    }
}

/// Exports via an Inspect `StringProperty` the `Debug` representation of
/// the wrapped `value`.
#[derive(Debug)]
pub struct InspectableDebugStringWatcher {
    debug_string: StringProperty,
}

impl<V> Watch<V> for InspectableDebugStringWatcher
where
    V: std::fmt::Debug,
{
    fn new<'b>(value: &V, node: &Node, name: impl Into<StringReference<'b>>) -> Self {
        Self { debug_string: node.create_string(name, format!("{:?}", value)) }
    }

    fn watch(&mut self, value: &V) {
        self.debug_string.set(&format!("{:?}", value))
    }
}

/// Exports via an Inspect `StringProperty` the `Debug` representation of the wrapped value `V`.
pub type InspectableDebugString<V> = Inspectable<V, InspectableDebugStringWatcher>;

/// Exports via an Inspect `UintProperty` a `u64`. Useful because the wrapped `u64`
/// value can be read.
#[derive(Debug)]
pub struct InspectableU64Watcher {
    uint_property: UintProperty,
}

impl Watch<u64> for InspectableU64Watcher {
    fn new<'b>(value: &u64, node: &Node, name: impl Into<StringReference<'b>>) -> Self {
        Self { uint_property: node.create_uint(name, *value) }
    }

    fn watch(&mut self, value: &u64) {
        self.uint_property.set(*value);
    }
}

/// Exports via an Inspect `UintProperty` a `u64`. Useful because the wrapped `u64`
/// value can be read.
pub type InspectableU64 = Inspectable<u64, InspectableU64Watcher>;

pub struct InspectableBoolWatcher {
    bool_property: BoolProperty,
}

impl Watch<bool> for InspectableBoolWatcher {
    fn new<'b>(value: &bool, node: &Node, name: impl Into<StringReference<'b>>) -> Self {
        Self { bool_property: node.create_bool(name, *value) }
    }

    fn watch(&mut self, value: &bool) {
        self.bool_property.set(*value);
    }
}

pub type InspectableBool = Inspectable<bool, InspectableBoolWatcher>;

#[cfg(test)]
mod test {
    use {
        super::*,
        fuchsia_inspect::{assert_data_tree, Inspector, IntProperty},
        std::collections::HashSet,
    };

    struct InspectableIntWatcher {
        i: IntProperty,
    }
    impl Watch<i64> for InspectableIntWatcher {
        fn new<'b>(value: &i64, node: &Node, name: impl Into<StringReference<'b>>) -> Self {
            Self { i: node.create_int(name, *value) }
        }
        fn watch(&mut self, value: &i64) {
            self.i.set(*value)
        }
    }

    type InspectableInt = Inspectable<i64, InspectableIntWatcher>;
    fn make_inspectable(i: i64) -> (InspectableInt, Inspector) {
        let inspector = Inspector::default();
        let inspectable = InspectableInt::new(i, inspector.root(), "inspectable-int");
        (inspectable, inspector)
    }

    #[fuchsia::test]
    fn test_inspectable_deref() {
        let inspectable = make_inspectable(1).0;

        assert_eq!(*inspectable, 1);
    }

    #[fuchsia::test]
    fn test_guard_deref() {
        let mut inspectable = make_inspectable(1).0;

        assert_eq!(*inspectable.get_mut(), 1);
    }

    #[fuchsia::test]
    fn test_guard_deref_mut() {
        let mut inspectable = make_inspectable(1).0;

        *inspectable.get_mut() = 2;

        assert_eq!(*inspectable, 2);
    }

    #[fuchsia::test]
    fn test_guard_drop_calls_callback() {
        let (mut inspectable, inspector) = make_inspectable(1);

        let mut guard = inspectable.get_mut();
        *guard = 2;
        drop(guard);

        assert_data_tree!(
            inspector,
            root: {
                "inspectable-int": 2i64
            }
        );
    }

    #[fuchsia::test]
    fn test_partial_eq() {
        let inspectable0 = make_inspectable(1).0;
        let inspectable1 = make_inspectable(1).0;

        assert_eq!(inspectable0, inspectable1);
    }

    #[fuchsia::test]
    fn test_partial_eq_not() {
        let inspectable0 = make_inspectable(1).0;
        let inspectable1 = make_inspectable(2).0;

        assert_ne!(inspectable0, inspectable1);
    }

    #[fuchsia::test]
    fn test_borrow() {
        let inspectable = make_inspectable(1).0;

        assert_eq!(Borrow::<i64>::borrow(&inspectable), &1);
    }

    #[fuchsia::test]
    fn test_inspectable_in_hash_set() {
        let inspectable = make_inspectable(1).0;
        let mut set = HashSet::new();

        set.insert(inspectable);
        assert!(set.contains(&1));

        set.remove(&1);
        assert!(set.is_empty());
    }
}

#[cfg(test)]
mod test_inspectable_len {
    use super::*;
    use fuchsia_inspect::assert_data_tree;

    #[fuchsia::test]
    fn test_initialization() {
        let inspector = fuchsia_inspect::Inspector::default();
        let _inspectable = InspectableLen::new(vec![0], inspector.root(), "test-property");

        assert_data_tree!(
            inspector,
            root: {
                "test-property": 1u64
            }
        );
    }

    #[fuchsia::test]
    fn test_watcher() {
        let inspector = fuchsia_inspect::Inspector::default();
        let mut inspectable = InspectableLen::new(vec![0], inspector.root(), "test-property");

        inspectable.get_mut().push(1);

        assert_data_tree!(
            inspector,
            root: {
                "test-property": 2u64
            }
        );
    }
}

#[cfg(test)]
mod test_inspectable_debug_string {
    use super::*;
    use fuchsia_inspect::assert_data_tree;

    #[fuchsia::test]
    fn test_initialization() {
        let inspector = fuchsia_inspect::Inspector::default();
        let _inspectable = InspectableDebugString::new(vec![0], inspector.root(), "test-property");

        assert_data_tree!(
            inspector,
            root: {
                "test-property": "[0]"
            }
        );
    }

    #[fuchsia::test]
    fn test_watcher() {
        let inspector = fuchsia_inspect::Inspector::default();
        let mut inspectable =
            InspectableDebugString::new(vec![0], inspector.root(), "test-property");

        inspectable.get_mut().push(1);

        assert_data_tree!(
            inspector,
            root: {
                "test-property": "[0, 1]"
            }
        );
    }
}

#[cfg(test)]
mod test_inspectable_u64 {
    use super::*;
    use fuchsia_inspect::assert_data_tree;

    #[fuchsia::test]
    fn test_initialization() {
        let inspector = fuchsia_inspect::Inspector::default();
        let _inspectable = InspectableU64::new(0, inspector.root(), "test-property");

        assert_data_tree!(
            inspector,
            root: {
                "test-property": 0u64,
            }
        );
    }

    #[fuchsia::test]
    fn test_watcher() {
        let inspector = fuchsia_inspect::Inspector::default();
        let mut inspectable = InspectableU64::new(0, inspector.root(), "test-property");

        *inspectable.get_mut() += 1;

        assert_data_tree!(
            inspector,
            root: {
                "test-property": 1u64,
            }
        );
    }
}

#[cfg(test)]
mod test_inspectable_bool {
    use super::*;
    use fuchsia_inspect::assert_data_tree;

    #[fuchsia::test]
    fn test_initialization() {
        let inspector = fuchsia_inspect::Inspector::default();
        let _inspectable = InspectableBool::new(false, inspector.root(), "test-property");

        assert_data_tree!(
            inspector,
            root: {
                "test-property": false,
            }
        );
    }

    #[fuchsia::test]
    fn test_watcher() {
        let inspector = fuchsia_inspect::Inspector::default();
        let mut inspectable = InspectableBool::new(false, inspector.root(), "test-property");

        *inspectable.get_mut() = true;

        assert_data_tree!(
            inspector,
            root: {
                "test-property": true,
            }
        );
    }
}
