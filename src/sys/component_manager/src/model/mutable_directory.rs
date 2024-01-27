// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::model::error::VfsError,
    std::sync::Arc,
    vfs::directory::{entry::DirectoryEntry, helper::DirectlyMutable, immutable::simple as pfs},
};

type Directory = Arc<pfs::Simple>;

/// Trait that allows directory to be mutated.
pub trait MutableDirectory {
    /// Adds an entry to a directory.
    fn add_node(&self, name: &str, entry: Arc<dyn DirectoryEntry>) -> Result<(), VfsError>;

    /// Removes an entry in the directory.
    ///
    /// Returns the entry to the caller if the entry was present in the directory.
    fn remove_node(&self, name: &str) -> Result<Option<Arc<dyn DirectoryEntry>>, VfsError>;
}

impl MutableDirectory for Directory {
    fn add_node(&self, name: &str, entry: Arc<dyn DirectoryEntry>) -> Result<(), VfsError> {
        self.add_entry(name, entry)
            .map_err(|status| VfsError::AddNodeError { name: name.to_string(), status })
    }

    fn remove_node(&self, name: &str) -> Result<Option<Arc<dyn DirectoryEntry>>, VfsError> {
        self.remove_entry(name, false)
            .map_err(|status| VfsError::RemoveNodeError { name: name.to_string(), status })
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*, assert_matches::assert_matches, std::convert::TryInto,
        vfs::file::vmo::read_only_static,
    };

    #[fuchsia::test]
    fn addable_with_result_add_ok() {
        let dir = vfs::directory::immutable::simple();

        assert!(
            dir.add_node("node_name", read_only_static(b"test"),).is_ok(),
            "add node with valid name should succeed"
        );
    }

    #[fuchsia::test]
    fn addable_with_result_add_error() {
        let dir = vfs::directory::immutable::simple();

        let err = dir
            .add_node("node_name/with/separators", read_only_static(b"test"))
            .expect_err("add entry with path separator should fail");
        assert_matches!(err, VfsError::AddNodeError { .. });
    }

    #[fuchsia::test]
    fn addable_with_result_remove_ok() {
        let dir = vfs::directory::immutable::simple();

        dir.add_node("node_name", read_only_static(b"test"))
            .expect("add node with valid name should succeed");

        let entry = dir.remove_node("node_name").expect("remove node should succeed");
        assert!(entry.is_some(), "entry should have existed before remove");
    }

    #[fuchsia::test]
    fn addable_with_result_remove_missing_ok() {
        let dir = vfs::directory::immutable::simple();

        let entry = dir.remove_node("does_not_exist").expect("remove node should succeed");
        assert!(entry.is_none(), "entry should not have existed before remove");
    }

    #[fuchsia::test]
    fn addable_with_result_remove_error() {
        let dir = vfs::directory::immutable::simple();

        let entry_name = "x".repeat((vfs::MAX_NAME_LENGTH + 1).try_into().unwrap());
        assert!(dir.remove_node(&entry_name).is_err(), "remove node should fail");
    }
}
