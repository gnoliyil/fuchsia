// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! `EntryContainer` is a trait implemented by directories that allow manipulation of their
//! content.

use crate::{
    directory::{dirents_sink, entry::DirectoryEntry, traversal_position::TraversalPosition},
    execution_scope::ExecutionScope,
    path::Path,
};

use {
    async_trait::async_trait,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io as fio,
    fuchsia_zircon::Status,
    std::{any::Any, sync::Arc},
};

mod private {
    use {fidl_fuchsia_io as fio, std::convert::TryFrom};

    /// A type-preserving wrapper around [`fuchsia_async::Channel`].
    #[derive(Debug)]
    pub struct DirectoryWatcher {
        channel: fuchsia_async::Channel,
    }

    impl DirectoryWatcher {
        /// Provides access to the underlying channel.
        pub fn channel(&self) -> &fuchsia_async::Channel {
            let Self { channel } = self;
            channel
        }
    }

    impl TryFrom<fidl::endpoints::ServerEnd<fio::DirectoryWatcherMarker>> for DirectoryWatcher {
        type Error = fuchsia_zircon::Status;

        fn try_from(
            server_end: fidl::endpoints::ServerEnd<fio::DirectoryWatcherMarker>,
        ) -> Result<Self, Self::Error> {
            let channel = fuchsia_async::Channel::from_channel(server_end.into_channel())?;
            Ok(Self { channel })
        }
    }
}

pub use private::DirectoryWatcher;

/// All directories implement this trait.  If a directory can be modified it should
/// also implement the `MutableDirectory` trait.
#[async_trait]
pub trait Directory: DirectoryEntry {
    /// Reads directory entries starting from `pos` by adding them to `sink`.
    /// Once finished, should return a sealed sink.
    // The lifetimes here are because of https://github.com/rust-lang/rust/issues/63033.
    async fn read_dirents<'a>(
        &'a self,
        pos: &'a TraversalPosition,
        sink: Box<dyn dirents_sink::Sink>,
    ) -> Result<(TraversalPosition, Box<dyn dirents_sink::Sealed>), Status>;

    /// Register a watcher for this directory.
    /// Implementations will probably want to use a `Watcher` to manage watchers.
    fn register_watcher(
        self: Arc<Self>,
        scope: ExecutionScope,
        mask: fio::WatchMask,
        watcher: DirectoryWatcher,
    ) -> Result<(), Status>;

    /// Unregister a watcher from this directory. The watcher should no longer
    /// receive events.
    fn unregister_watcher(self: Arc<Self>, key: usize);

    /// Get this directory's attributes.
    /// The "mode" field will be filled in by the connection.
    async fn get_attrs(&self) -> Result<fio::NodeAttributes, Status>;

    /// Called when the directory is closed.
    fn close(&self) -> Result<(), Status>;

    /// Returns information about the filesystem.
    fn query_filesystem(&self) -> Result<fio::FilesystemInfo, Status> {
        Err(Status::NOT_SUPPORTED)
    }
}

/// This trait indicates a directory that can be mutated by adding and removing entries.
/// This trait must be implemented to use a `MutableConnection`, however, a directory could also
/// implement the `DirectlyMutable` type, which provides a blanket implementation of this trait.
#[async_trait]
pub trait MutableDirectory: Directory + Send + Sync {
    /// Adds a child entry to this directory.  If the target exists, it should fail with
    /// ZX_ERR_ALREADY_EXISTS.
    async fn link(
        self: Arc<Self>,
        _name: String,
        _source_dir: Arc<dyn Any + Send + Sync>,
        _source_name: &str,
    ) -> Result<(), Status> {
        Err(Status::NOT_SUPPORTED)
    }

    /// Set the attributes of this directory based on the values in `attrs`.
    async fn set_attrs(
        &self,
        flags: fio::NodeAttributeFlags,
        attributes: fio::NodeAttributes,
    ) -> Result<(), Status>;

    /// Removes an entry from this directory.
    async fn unlink(self: Arc<Self>, name: &str, must_be_directory: bool) -> Result<(), Status>;

    /// Syncs the directory.
    async fn sync(&self) -> Result<(), Status>;

    /// Renames into this directory.
    async fn rename(
        self: Arc<Self>,
        src_dir: Arc<dyn MutableDirectory>,
        src_name: Path,
        dst_name: Path,
    ) -> Result<(), Status>;

    /// Creates a symbolic link.
    async fn create_symlink(
        &self,
        _name: String,
        _target: Vec<u8>,
        _connection: Option<ServerEnd<fio::SymlinkMarker>>,
    ) -> Result<(), Status> {
        Err(Status::NOT_SUPPORTED)
    }

    /// List extended attributes.
    async fn list_extended_attributes(&self) -> Result<Vec<Vec<u8>>, Status> {
        Err(Status::NOT_SUPPORTED)
    }

    /// Get the value for an extended attribute.
    async fn get_extended_attribute(&self, _name: Vec<u8>) -> Result<Vec<u8>, Status> {
        Err(Status::NOT_SUPPORTED)
    }

    /// Set the value for an extended attribute.
    async fn set_extended_attribute(&self, _name: Vec<u8>, _value: Vec<u8>) -> Result<(), Status> {
        Err(Status::NOT_SUPPORTED)
    }

    /// Remove the value for an extended attribute.
    async fn remove_extended_attribute(&self, _name: Vec<u8>) -> Result<(), Status> {
        Err(Status::NOT_SUPPORTED)
    }
}
