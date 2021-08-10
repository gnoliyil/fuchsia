// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use parking_lot::RwLock;
use std::collections::btree_map::Entry;
use std::collections::BTreeMap;
use std::sync::{Arc, Weak};

use crate::fs::*;
use crate::types::*;

struct DirEntryState {
    /// The parent DirEntry.
    ///
    /// The DirEntry tree has strong references from child-to-parent and weak
    /// references from parent-to-child. This design ensures that the parent
    /// chain is always populated in the cache, but some children might be
    /// missing from the cache.
    parent: Option<DirEntryHandle>,

    /// The name that this parent calls this child.
    ///
    /// This name might not be reflected in the full path in the namespace that
    /// contains this DirEntry. For example, this DirEntry might be the root of
    /// a chroot.
    ///
    /// Most callers that want to work with names for DirEntries should use the
    /// NamespaceNodes.
    local_name: FsString,

    /// A partial cache of the children of this DirEntry.
    ///
    /// DirEntries are added to this cache when they are looked up and removed
    /// when they are no longer referenced.
    children: BTreeMap<FsString, Weak<DirEntry>>,
}

/// An entry in a directory.
///
/// This structure assigns a name to an FsNode in a given file system. An
/// FsNode might have multiple directory entries, for example if there are more
/// than one hard link to the same FsNode. In those cases, each hard link will
/// have a different parent and a different local_name because each hard link
/// has its own DirEntry object.
///
/// A directory cannot have more than one hard link, which means there is a
/// single DirEntry for each Directory FsNode. That invariant lets us store the
/// children for a directory in the DirEntry rather than in the FsNode.
pub struct DirEntry {
    /// The FsNode referenced by this DirEntry.
    ///
    /// A given FsNode can be referenced by multiple DirEntry objects, for
    /// example if there are multiple hard links to a given FsNode.
    pub node: FsNodeHandle,

    /// The mutable state for this DirEntry.
    state: RwLock<DirEntryState>,
}

pub type DirEntryHandle = Arc<DirEntry>;

impl DirEntry {
    pub fn new_root<T: FsNodeOps + 'static>(ops: T, fs: &FileSystemHandle) -> DirEntryHandle {
        // TODO: apply_umask
        DirEntry::new(
            Arc::new(FsNode::new(Some(Box::new(ops)), FileMode::IFDIR | FileMode::ALLOW_ALL, fs)),
            None,
            FsString::new(),
        )
    }

    pub fn new(
        node: FsNodeHandle,
        parent: Option<DirEntryHandle>,
        local_name: FsString,
    ) -> DirEntryHandle {
        Arc::new(DirEntry {
            node,
            state: RwLock::new(DirEntryState { parent, local_name, children: BTreeMap::new() }),
        })
    }

    /// The name that this node's parent calls this node.
    ///
    /// If this node is mounted in a namespace, the parent of this node in that
    /// namespace might have a different name for the point in the namespace at
    /// which this node is mounted.
    pub fn local_name(&self) -> FsString {
        self.state.read().local_name.clone()
    }

    /// The parent DirEntry object.
    ///
    /// Returns None if this DirEntry is the root of its file system.
    ///
    /// Be aware that the root of one file system might be mounted as a child
    /// in another file system. For that reason, consider walking the
    /// NamespaceNode tree (which understands mounts) rather than the DirEntry
    /// tree.
    pub fn parent(self: &DirEntryHandle) -> Option<DirEntryHandle> {
        self.state.read().parent.clone()
    }

    /// The parent DirEntry object or this DirEntry if this entry is the root.
    ///
    /// Useful when traversing up the tree if you always want to find a parent
    /// (e.g., for "..").
    ///
    /// Be aware that the root of one file system might be mounted as a child
    /// in another file system. For that reason, consider walking the
    /// NamespaceNode tree (which understands mounts) rather than the DirEntry
    /// tree.
    pub fn parent_or_self(self: &DirEntryHandle) -> DirEntryHandle {
        self.state.read().parent.as_ref().unwrap_or(self).clone()
    }

    pub fn component_lookup(self: &DirEntryHandle, name: &FsStr) -> Result<DirEntryHandle, Errno> {
        let (node, _) = self.get_or_create_child(name, |child| self.node.lookup(name, child))?;
        Ok(node)
    }

    /// Creates a new DirEntry
    ///
    /// The create_fn function is called to create the underlying FsNode for
    /// the DirEntry.
    ///
    /// If the entry already exists, create_fn is not called, and EEXIST is
    /// returned.
    fn create_entry<F>(
        self: &DirEntryHandle,
        name: &FsStr,
        mode: FileMode,
        dev: DeviceType,
        mk_callback: F,
    ) -> Result<DirEntryHandle, Errno>
    where
        F: FnOnce(&mut FsNode) -> Result<(), Errno>,
    {
        assert!(mode & FileMode::IFMT != FileMode::EMPTY, "mknod called without node type.");
        let (entry, exists) = self.get_or_create_child(name, |child| {
            let info = child.info_mut();
            info.mode = mode;
            if mode.is_blk() || mode.is_chr() {
                info.rdev = dev;
            }
            std::mem::drop(info);
            mk_callback(child)
        })?;
        if exists {
            return Err(EEXIST);
        }
        let now = fuchsia_runtime::utc_time();
        let mut info = self.node.info_write();
        info.time_access = now;
        info.time_modify = now;
        std::mem::drop(info);
        entry.node.file_system().did_create_dir_entry(&entry);
        Ok(entry)
    }

    #[cfg(test)]
    pub fn create_dir(self: &DirEntryHandle, name: &FsStr) -> Result<DirEntryHandle, Errno> {
        // TODO: apply_umask
        self.create_entry(name, FileMode::IFDIR | FileMode::ALLOW_ALL, DeviceType::NONE, |child| {
            self.node.mkdir(name, child)
        })
    }

    pub fn create_node(
        self: &DirEntryHandle,
        name: &FsStr,
        mode: FileMode,
        dev: DeviceType,
    ) -> Result<DirEntryHandle, Errno> {
        self.create_entry(name, mode, dev, |child| {
            if mode.is_dir() {
                self.node.mkdir(name, child)
            } else {
                self.node.mknod(name, child)
            }
        })
    }

    pub fn create_symlink(
        self: &DirEntryHandle,
        name: &FsStr,
        target: &FsStr,
    ) -> Result<DirEntryHandle, Errno> {
        self.create_entry(name, FileMode::IFLNK | FileMode::ALLOW_ALL, DeviceType::NONE, |child| {
            self.node.create_symlink(name, target, child)
        })
    }

    pub fn unlink(self: &DirEntryHandle, name: &FsStr, _kind: UnlinkKind) -> Result<(), Errno> {
        let mut state = self.state.write();
        let child = state.children.get(name).ok_or(ENOENT)?.upgrade().unwrap();
        // TODO: Check _kind against the child's mode.
        self.node.unlink(name, &child.node)?;
        state.children.remove(name);

        // We drop the state lock before we drop the child so that we do
        // not trigger a deadlock in the Drop trait for FsNode, which attempts
        // to remove the FsNode from its parent's child list.
        std::mem::drop(state);

        // TODO: When we have hard links, we'll need to check the link count.
        self.node.file_system().will_destroy_dir_entry(&child);

        std::mem::drop(child);
        Ok(())
    }

    pub fn get_children<F, T>(&self, callback: F) -> T
    where
        F: FnOnce(&BTreeMap<FsString, Weak<DirEntry>>) -> T,
    {
        let state = self.state.read();
        callback(&state.children)
    }

    fn get_or_create_child<F>(
        self: &DirEntryHandle,
        name: &FsStr,
        init_fn: F,
    ) -> Result<(DirEntryHandle, bool), Errno>
    where
        F: FnOnce(&mut FsNode) -> Result<(), Errno>,
    {
        // Check if the child is already in children. In that case, we can
        // simply return the child and we do not need to call init_fn.
        let state = self.state.read();
        if let Some(child) = state.children.get(name).and_then(Weak::upgrade) {
            return Ok((child, true));
        }
        std::mem::drop(state);

        let create_child = || {
            let mut node = FsNode::new(None, FileMode::EMPTY, &self.node.file_system());

            init_fn(&mut node)?;
            assert!(
                node.info().mode & FileMode::IFMT != FileMode::EMPTY,
                "FsNode initialization did not populate the FileMode in FsNodeInfo."
            );
            assert!(node.has_ops(), "FsNodeOps initialization did not populate ops");
            let node = Arc::new(node);
            Ok(DirEntry::new(node, Some(self.clone()), name.to_vec()))
        };

        let mut state = self.state.write();
        match state.children.entry(name.to_vec()) {
            Entry::Vacant(entry) => {
                let child = create_child()?;
                entry.insert(Arc::downgrade(&child));
                Ok((child, false))
            }
            Entry::Occupied(mut entry) => {
                // It's possible that the upgrade will succeed this time around
                // because we dropped the read lock before acquiring the write
                // lock. Another thread might have populated this entry while
                // we were not holding any locks.
                if let Some(child) = Weak::upgrade(entry.get()) {
                    return Ok((child, true));
                }
                let child = create_child()?;
                entry.insert(Arc::downgrade(&child));
                Ok((child, false))
            }
        }
    }

    // This function is only useful for tests and has some oddities.
    //
    // For example, not all the children might have been looked up yet, which
    // means the returned vector could be missing some names.
    //
    // Also, the vector might have "extra" names that are in the process of
    // being looked up. If the lookup fails, they'll be removed.
    #[cfg(test)]
    pub fn copy_child_names(&self) -> Vec<FsString> {
        self.state
            .read()
            .children
            .values()
            .filter_map(|child| Weak::upgrade(child).map(|c| c.local_name()))
            .collect()
    }

    fn internal_remove_child(&self, child: &mut DirEntry) {
        let local_name = child.local_name();
        let mut state = self.state.write();
        if let Some(weak_child) = state.children.get(&local_name) {
            // If this entry is occupied, we need to check whether child is
            // the current occupant. If so, we should remove the entry
            // because the child no longer exists.
            if std::ptr::eq(weak_child.as_ptr(), child) {
                state.children.remove(&local_name);
            }
        }
    }
}

/// The Drop trait for DirEntry removes the entry from the child list of the
/// parent entry, which means we cannot drop DirEntry objects while holding a
/// lock on the parent's child list.
impl Drop for DirEntry {
    fn drop(&mut self) {
        let maybe_parent = self.state.write().parent.take();
        if let Some(parent) = maybe_parent {
            parent.internal_remove_child(self);
        }
    }
}
