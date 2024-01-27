// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::WriteInspect,
    fuchsia_inspect::{Node, StringReference},
    std::convert::AsRef,
};

/// Wrapper to log bytes in an `inspect_log!` or `inspect_insert!` macro.
///
/// This wrapper is defined because a default `WriteInspect` implementation isn't provided for
/// an array or slice of bytes. Such default implementation was left out so that the user has
/// to explicitly choose whether to log bytes slice as a string or a byte vector in Inspect.
pub struct InspectBytes<T: AsRef<[u8]>>(pub T);

impl<T: AsRef<[u8]>> WriteInspect for InspectBytes<T> {
    fn write_inspect(&self, writer: &Node, key: impl Into<StringReference>) {
        writer.record_bytes(key, self.0.as_ref());
    }
}

/// Wrapper to log a list of items in `inspect_log!` or `inspect_insert!` macro. Each item
/// in the list must be a type that implements `WriteInspect`
///
/// Example:
/// ```
/// let list = ["foo", "bar", "baz"];
/// inspect_insert!(node_writer, some_list: InspectList(&list));
/// ```
///
/// The above code snippet would create the following child under node_writer:
/// ```
/// some_list:
///   0: "foo"
///   1: "bar"
///   2: "baz"
/// ```
pub struct InspectList<'a, T>(pub &'a [T]);

impl<'a, T> WriteInspect for InspectList<'a, T>
where
    T: WriteInspect,
{
    fn write_inspect(&self, writer: &Node, key: impl Into<StringReference>) {
        let child = writer.create_child(key);
        for (i, val) in self.0.iter().enumerate() {
            val.write_inspect(&child, &i.to_string());
        }

        writer.record(child);
    }
}

/// Wrapper around a list `[T]` and a closure function `F` that determines how to map
/// and log each value of `T` in `inspect_log!` or `inspect_insert!` macro.
///
/// Example:
/// ```
/// let list = ["foo", "bar", "baz"]
/// let list_mapped = InspectListClosure(&list, |node_writer, key, item| {
///     let mapped_item = format!("super{}", item);
///     inspect_insert!(node_writer, var key: mapped_item);
/// });
/// inspect_insert!(node_writer, some_list: list_mapped);
/// ```
///
/// The above code snippet would create the following child under node_writer:
/// ```
/// some_list:
///   0: "superfoo"
///   1: "superbar"
///   2: "superbaz"
/// ```
pub struct InspectListClosure<'a, T, F>(pub &'a [T], pub F)
where
    F: Fn(&Node, &str, &T);

impl<'a, T, F> WriteInspect for InspectListClosure<'a, T, F>
where
    F: Fn(&Node, &str, &T),
{
    fn write_inspect(&self, writer: &Node, key: impl Into<StringReference>) {
        let child = writer.create_child(key);
        for (i, val) in self.0.iter().enumerate() {
            self.1(&child, &i.to_string(), val);
        }

        writer.record(child);
    }
}
