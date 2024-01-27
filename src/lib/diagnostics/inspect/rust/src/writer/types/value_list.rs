// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::writer::types::InspectType;
use derivative::Derivative;
use parking_lot::Mutex;

type InspectTypeList = Vec<Box<dyn InspectType>>;

/// Holds a list of inspect types that won't change.
#[derive(Derivative)]
#[derivative(Debug, PartialEq, Eq)]
pub struct ValueList {
    #[derivative(PartialEq = "ignore")]
    #[derivative(Debug = "ignore")]
    values: Mutex<Option<InspectTypeList>>,
}

impl Default for ValueList {
    fn default() -> Self {
        ValueList::new()
    }
}

impl ValueList {
    /// Creates a new empty value list.
    pub fn new() -> Self {
        Self { values: Mutex::new(None) }
    }

    /// Stores an inspect type that won't change.
    pub fn record(&self, value: impl InspectType + 'static) {
        let boxed_value = Box::new(value);
        let mut values_lock = self.values.lock();
        if let Some(ref mut values) = *values_lock {
            values.push(boxed_value);
        } else {
            *values_lock = Some(vec![boxed_value]);
        }
    }

    /// Clears all values from ValueList, rendering it empty.
    /// `InspectType` values contained will be dropped.
    pub fn clear(&self) {
        let mut values_lock = self.values.lock();
        *values_lock = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::assert_json_diff;
    use crate::hierarchy::DiagnosticsHierarchy;
    use crate::writer::types::Inspector;

    #[fuchsia::test]
    fn value_list_record() {
        let inspector = Inspector::default();
        let child = inspector.root().create_child("test");
        let value_list = ValueList::new();
        assert!(value_list.values.lock().is_none());
        value_list.record(child);
        assert_eq!(value_list.values.lock().as_ref().unwrap().len(), 1);
    }

    #[fuchsia::test]
    fn value_list_drop_recorded() {
        let inspector = Inspector::default();
        let child = inspector.root().create_child("test");
        let value_list = ValueList::new();
        assert!(value_list.values.lock().is_none());
        value_list.record(child);
        assert_eq!(value_list.values.lock().as_ref().unwrap().len(), 1);
        assert_json_diff!(inspector, root: {
            test: {},
        });

        value_list.clear();
        assert!(value_list.values.lock().is_none());
        assert_json_diff!(inspector, root: {});
    }
}
