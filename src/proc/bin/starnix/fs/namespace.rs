// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::collections::{HashMap, HashSet};
use std::fmt;
use std::hash::{Hash, Hasher};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Weak};

use ref_cast::RefCast;

use super::devpts::dev_pts_fs;
use super::devtmpfs::dev_tmp_fs;
use super::proc::proc_fs;
use super::sysfs::sys_fs;
use super::tmpfs::TmpFs;
use super::*;
use crate::bpf::BpfFs;
use crate::device::BinderFs;
use crate::fs::buffers::{InputBuffer, OutputBuffer};
use crate::lock::{Mutex, RwLock};
use crate::mutable_state::*;
use crate::selinux::selinux_fs;
use crate::task::*;
use crate::types::*;

/// A mount namespace.
///
/// The namespace records at which entries filesystems are mounted.
#[derive(Debug)]
pub struct Namespace {
    root_mount: MountHandle,
}

impl Namespace {
    pub fn new(fs: FileSystemHandle) -> Arc<Namespace> {
        Arc::new(Self { root_mount: Mount::new(WhatToMount::Fs(fs), MountFlags::empty()) })
    }

    pub fn root(&self) -> NamespaceNode {
        self.root_mount.root()
    }

    pub fn clone_namespace(&self) -> Arc<Namespace> {
        Arc::new(Self { root_mount: self.root_mount.clone_mount_recursive() })
    }

    /// Assuming new_ns is a clone of the namespace that node is from, return the equivalent of
    /// node in new_ns. If this assumption is violated, returns None.
    pub fn translate_node(mut node: NamespaceNode, new_ns: &Namespace) -> Option<NamespaceNode> {
        // Collect the list of mountpoints that leads to this node's mount
        let mut mountpoints = vec![];
        let mut mount = node.mount;
        while let Some(mountpoint) = mount.and_then(|m| m.mountpoint()) {
            mountpoints.push(mountpoint.entry);
            mount = mountpoint.mount;
        }

        // Follow the same path in the new namespace
        let mut mount = Arc::clone(&new_ns.root_mount);
        for mountpoint in mountpoints.iter().rev() {
            let next_mount = Arc::clone(mount.read().submounts.get(ArcKey::ref_cast(mountpoint))?);
            mount = next_mount;
        }
        node.mount = Some(mount);
        Some(node)
    }
}

/// An instance of a filesystem mounted in a namespace.
///
/// At a mount, path traversal switches from one filesystem to another.
/// The client sees a composed directory structure that glues together the
/// directories from the underlying FsNodes from those filesystems.
///
/// The mounts in a namespace form a mount tree, with `mountpoint` pointing to the parent and
/// `submounts` pointing to the children.
pub struct Mount {
    root: DirEntryHandle,
    flags: MountFlags,
    _fs: FileSystemHandle,

    /// A unique identifier for this mount reported in /proc/pid/mountinfo.
    id: u64,

    /// If this is a bind mount, the mount of what it was bind mounted from, otherwise None.
    origin_mount: Option<MountHandle>,

    // Lock ordering: mount -> submount
    state: RwLock<MountState>,
    // Mount used to contain a Weak<Namespace>. It no longer does because since the mount point
    // hash was moved from Namespace to Mount, nothing actually uses it. Now that
    // Namespace::clone_namespace() is implemented in terms of Mount::clone_mount_recursive, it
    // won't be trivial to add it back. I recommend turning the mountpoint field into an enum of
    // Mountpoint or Namespace, maybe called "parent", and then traverse up to the top of the tree
    // if you need to find a Mount's Namespace.
}
type MountHandle = Arc<Mount>;

#[derive(Default)]
pub struct MountState {
    /// The namespace node that this mount is mounted on. This is a tuple instead of a
    /// NamespaceNode because the Mount pointer has to be weak because this is the pointer to the
    /// parent mount, the parent has a pointer to the children too, and making both strong would be
    /// a cycle.
    mountpoint: Option<(Weak<Mount>, DirEntryHandle)>,

    // The keys of this map are always descendants of this mount's root.
    //
    // Each directory entry can only have one mount attached. Mount shadowing works by using the
    // root of the inner mount as a mountpoint. For example, if filesystem A is mounted at /foo,
    // mounting filesystem B on /foo will create the mount as a child of the A mount, attached to
    // A's root, instead of the root mount.
    submounts: HashMap<ArcKey<DirEntry>, MountHandle>,

    /// The membership of this mount in its peer group. Do not access directly. Instead use
    /// peer_group(), take_from_peer_group(), and set_peer_group().
    // TODO(tbodt): Refactor the links into, some kind of extra struct or something? This is hard
    // because setting this field requires the Arc<Mount>.
    peer_group_: Option<(Arc<PeerGroup>, PtrKey<Mount>)>,
    /// The membership of this mount in a PeerGroup's downstream. Do not access directly. Instead
    /// use upstream(), take_from_upstream(), and set_upstream().
    upstream_: Option<(Weak<PeerGroup>, PtrKey<Mount>)>,
}

/// A group of mounts. Setting MS_SHARED on a mount puts it in its own peer group. Any bind mounts
/// of a mount in the group are also added to the group. A mount created in any mount in a peer
/// group will be automatically propagated (recreated) in every other mount in the group.
#[derive(Default)]
struct PeerGroup {
    id: u64,
    state: RwLock<PeerGroupState>,
}
#[derive(Default)]
struct PeerGroupState {
    mounts: HashSet<WeakKey<Mount>>,
    downstream: HashSet<WeakKey<Mount>>,
}

pub enum WhatToMount {
    Fs(FileSystemHandle),
    Bind(NamespaceNode),
}

static NEXT_MOUNT_ID: AtomicU64 = AtomicU64::new(1);
static NEXT_PEER_GROUP_ID: AtomicU64 = AtomicU64::new(1);

impl Mount {
    fn new(what: WhatToMount, flags: MountFlags) -> MountHandle {
        match what {
            WhatToMount::Fs(fs) => Self::new_with_root(fs.root().clone(), None, flags),
            WhatToMount::Bind(node) => {
                let mount = node.mount.expect("can't bind mount from an anonymous node");
                mount.clone_mount(&node.entry, Some(&mount), flags)
            }
        }
    }

    fn new_with_root(
        root: DirEntryHandle,
        origin_mount: Option<MountHandle>,
        flags: MountFlags,
    ) -> MountHandle {
        assert!(
            !flags.intersects(!MountFlags::STORED_FLAGS),
            "mount created with extra flags {:?}",
            flags - MountFlags::STORED_FLAGS
        );
        let fs = root.node.fs();
        Arc::new(Self {
            id: NEXT_MOUNT_ID.fetch_add(1, Ordering::Relaxed),
            root,
            origin_mount,
            flags,
            _fs: fs,
            state: Default::default(),
        })
    }

    /// A namespace node referring to the root of the mount.
    pub fn root(self: &MountHandle) -> NamespaceNode {
        NamespaceNode { mount: Some(Arc::clone(self)), entry: Arc::clone(&self.root) }
    }

    /// The NamespaceNode on which this Mount is mounted.
    fn mountpoint(&self) -> Option<NamespaceNode> {
        let state = self.state.read();
        let (ref mount, ref entry) = state.mountpoint.as_ref()?;
        Some(NamespaceNode { mount: Some(mount.upgrade()?), entry: entry.clone() })
    }

    /// Create the specified mount as a child. Also propagate it to the mount's peer group.
    fn create_submount(
        self: &MountHandle,
        dir: &DirEntryHandle,
        what: WhatToMount,
        flags: MountFlags,
    ) {
        // TODO(tbodt): Making a copy here is necessary for lock ordering, because the peer group
        // lock nests inside all mount locks (it would be impractical to reverse this because you
        // need to lock a mount to get its peer group.) But it opens the door to race conditions
        // where if a peer are concurrently being added, the mount might not get propagated to the
        // new peer. The only true solution to this is bigger locks, somehow using the same lock
        // for the peer group and all of the mounts in the group. Since peer groups are fluid and
        // can have mounts constantly joining and leaving and then joining other groups, the only
        // sensible locking option is to use a single global lock for all mounts and peer groups.
        // This is almost impossible to express in rust. Help.
        //
        // Update: Also necessary to make a copy to prevent excess replication, see the comment on
        // the following Mount::new call.
        let peers = {
            let state = self.state.read();
            state.peer_group().map(|g| g.copy_propagation_targets()).unwrap_or_default()
        };

        // Create the mount after copying the peer groups, because in the case of creating a bind
        // mount inside itself, the new mount would get added to our peer group during the
        // Mount::new call, but we don't want to replicate into it already. For an example see
        // MountTest.QuizBRecursion.
        let mount = Mount::new(what, flags);

        if self.read().is_shared() {
            mount.write().make_shared();
        }

        for peer in peers {
            if Arc::ptr_eq(self, &peer) {
                continue;
            }
            let clone = mount.clone_mount_recursive();
            peer.write().add_submount_internal(dir, clone);
        }

        self.write().add_submount_internal(dir, mount)
    }

    /// Create a new mount with the same filesystem, flags, and peer group. Used to implement bind
    /// mounts.
    fn clone_mount(
        self: &MountHandle,
        new_root: &DirEntryHandle,
        new_origin: Option<&MountHandle>,
        flags: MountFlags,
    ) -> MountHandle {
        assert!(new_root.is_descendant_of(&self.root));
        // According to mount(2) on bind mounts, all flags other than MS_REC are ignored when doing
        // a bind mount.
        let clone =
            Self::new_with_root(Arc::clone(new_root), new_origin.map(Arc::clone), self.flags);

        if flags.contains(MountFlags::REC) {
            // This is two steps because the alternative (locking clone.state while iterating over
            // self.state.submounts) trips tracing_mutex. The lock ordering is parent -> child, and
            // if the clone is eventually made a child of self, this looks like an ordering
            // violation. I'm not convinced it's a real issue, but I can't convince myself it's not
            // either.
            let mut submounts = vec![];
            for (dir, mount) in &self.state.read().submounts {
                submounts.push((dir.clone(), mount.clone_mount_recursive()));
            }
            let mut clone_state = clone.write();
            for (dir, submount) in submounts {
                clone_state.add_submount_internal(&dir, submount);
            }
        }

        // Put the clone in the same peer group
        let peer_group = self.state.read().peer_group().map(Arc::clone);
        if let Some(peer_group) = peer_group {
            clone.write().set_peer_group(peer_group);
        }

        clone
    }

    /// Do a clone of the full mount hierarchy below this mount. Used for creating mount
    /// namespaces and creating copies to use for propagation.
    fn clone_mount_recursive(self: &MountHandle) -> MountHandle {
        self.clone_mount(&self.root, self.origin_mount.as_ref(), MountFlags::REC)
    }

    pub fn change_propagation(self: &MountHandle, flag: MountFlags, recursive: bool) {
        let mut state = self.write();
        match flag {
            MountFlags::SHARED => state.make_shared(),
            MountFlags::PRIVATE => state.make_private(),
            MountFlags::DOWNSTREAM => state.make_downstream(),
            _ => {
                tracing::warn!("mount propagation {:?}", flag);
                return;
            }
        }

        if recursive {
            for mount in state.submounts.values() {
                mount.change_propagation(flag, recursive);
            }
        }
    }

    state_accessor!(Mount, state);
}

impl MountState {
    /// Return this mount's current peer group.
    fn peer_group(&self) -> Option<&Arc<PeerGroup>> {
        let (ref group, _) = self.peer_group_.as_ref()?;
        Some(group)
    }

    /// Remove this mount from its peer group and return the peer group.
    fn take_from_peer_group(&mut self) -> Option<Arc<PeerGroup>> {
        let (old_group, old_mount) = self.peer_group_.take()?;
        old_group.remove(old_mount);
        if let Some(upstream) = self.take_from_upstream() {
            let next_mount =
                old_group.state.read().mounts.iter().next().map(|w| w.0.upgrade().unwrap());
            if let Some(next_mount) = next_mount {
                // TODO(fxbug.dev/114002): Fix the lock ordering here. We've locked next_mount
                // while self is locked, and since the propagation tree and mount tree are
                // separate, this could violate the mount -> submount order previously established.
                next_mount.write().set_upstream(upstream);
            }
        }
        Some(old_group)
    }

    fn upstream(&self) -> Option<Arc<PeerGroup>> {
        self.upstream_.as_ref().and_then(|g| g.0.upgrade())
    }

    fn take_from_upstream(&mut self) -> Option<Arc<PeerGroup>> {
        let (old_upstream, old_mount) = self.upstream_.take()?;
        // TODO(tbodt): Reason about whether the upgrade() could possibly return None, and what we
        // should actually do in that case.
        let old_upstream = old_upstream.upgrade()?;
        old_upstream.remove_downstream(old_mount);
        Some(old_upstream)
    }
}

#[apply(state_implementation!)]
impl MountState<Base = Mount> {
    /// Add a child mount *without propagating it to the peer group*. For internal use only.
    fn add_submount_internal(&mut self, dir: &DirEntryHandle, mount: MountHandle) {
        if !dir.is_descendant_of(&self.base.root) {
            return;
        }

        dir.register_mount();
        let old_mountpoint =
            mount.state.write().mountpoint.replace((Arc::downgrade(self.base), Arc::clone(dir)));
        assert!(old_mountpoint.is_none(), "add_submount can only take a newly created mount");
        // Mount shadowing is implemented by mounting onto the root of the first mount, not by
        // creating two mounts on the same mountpoint.
        let old_mount = self.submounts.insert(ArcKey(dir.clone()), Arc::clone(&mount));

        // In rare cases, mount propagation might result in a request to mount on a directory where
        // something is already mounted. MountTest.LotsOfShadowing will trigger this. Linux handles
        // this by inserting the new mount between the old mount and the current mount.
        if let Some(old_mount) = old_mount {
            // Previous state: self[dir] = old_mount
            // New state: self[dir] = new_mount, new_mount[new_mount.root] = old_mount
            // The new mount has already been inserted into self, now just update the old mount to
            // be a child of the new mount.
            old_mount.write().mountpoint = Some((Arc::downgrade(&mount), Arc::clone(dir)));
            mount.write().submounts.insert(ArcKey(Arc::clone(&mount.root)), old_mount);
        }
    }

    /// Set this mount's peer group.
    fn set_peer_group(&mut self, group: Arc<PeerGroup>) {
        self.take_from_peer_group();
        group.add(self.base);
        self.peer_group_ = Some((group, Arc::as_ptr(self.base).into()));
    }

    fn set_upstream(&mut self, group: Arc<PeerGroup>) {
        self.take_from_upstream();
        group.add_downstream(self.base);
        self.upstream_ = Some((Arc::downgrade(&group), Arc::as_ptr(self.base).into()));
    }

    /// Is the mount in a peer group? Corresponds to MS_SHARED.
    pub fn is_shared(&self) -> bool {
        self.peer_group().is_some()
    }

    /// Put the mount in a peer group. Implements MS_SHARED.
    pub fn make_shared(&mut self) {
        if self.is_shared() {
            return;
        }
        self.set_peer_group(PeerGroup::new());
    }

    /// Take the mount out of its peer group, also remove upstream if any. Implements MS_PRIVATE.
    pub fn make_private(&mut self) {
        self.take_from_peer_group();
        self.take_from_upstream();
    }

    /// Take the mount out of its peer group and make it downstream instead. Implements
    /// MountFlags::DOWNSTREAM (MS_SLAVE).
    pub fn make_downstream(&mut self) {
        if let Some(peer_group) = self.take_from_peer_group() {
            self.set_upstream(peer_group);
        }
    }
}

impl PeerGroup {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            id: NEXT_PEER_GROUP_ID.fetch_add(1, Ordering::Relaxed),
            state: Default::default(),
        })
    }

    fn add(&self, mount: &Arc<Mount>) {
        self.state.write().mounts.insert(WeakKey::from(mount));
    }

    fn remove(&self, mount: PtrKey<Mount>) {
        self.state.write().mounts.remove(&mount);
    }

    fn add_downstream(&self, mount: &Arc<Mount>) {
        self.state.write().downstream.insert(WeakKey::from(mount));
    }

    fn remove_downstream(&self, mount: PtrKey<Mount>) {
        self.state.write().downstream.remove(&mount);
    }

    fn copy_propagation_targets(&self) -> Vec<MountHandle> {
        let mut buf = vec![];
        self.collect_propagation_targets(&mut buf);
        buf
    }

    fn collect_propagation_targets(&self, buf: &mut Vec<MountHandle>) {
        let downstream_mounts: Vec<_> = {
            let state = self.state.read();
            buf.extend(state.mounts.iter().filter_map(|m| m.0.upgrade()));
            state.downstream.iter().filter_map(|m| m.0.upgrade()).collect()
        };
        for mount in downstream_mounts {
            let peer_group = mount.read().peer_group().map(Arc::clone);
            match peer_group {
                Some(group) => group.collect_propagation_targets(buf),
                None => buf.push(mount),
            }
        }
    }
}

impl Drop for Mount {
    fn drop(&mut self) {
        let state = self.state.get_mut();
        if let Some((_mount, node)) = &state.mountpoint {
            node.unregister_mount()
        }
        state.take_from_peer_group();
        state.take_from_upstream();
    }
}

impl fmt::Debug for Mount {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let state = self.state.read();
        f.debug_struct("Mount")
            .field("id", &(self as *const Mount))
            .field("root", &self.root)
            .field("mountpoint", &state.mountpoint)
            .field("submounts", &state.submounts)
            .finish()
    }
}

pub fn create_filesystem(
    task: &CurrentTask,
    _source: &FsStr,
    fs_type: &FsStr,
    data: &FsStr,
) -> Result<WhatToMount, Errno> {
    let kernel = task.kernel();
    let fs = match fs_type {
        b"devtmpfs" => dev_tmp_fs(task).clone(),
        b"devpts" => dev_pts_fs(kernel).clone(),
        b"proc" => proc_fs(kernel.clone()),
        b"selinuxfs" => selinux_fs(kernel).clone(),
        b"sysfs" => sys_fs(kernel).clone(),
        b"tmpfs" => TmpFs::new_fs_with_data(kernel, data),
        b"binder" => BinderFs::new_fs(task)?,
        b"bpf" => BpfFs::new_fs(kernel)?,
        _ => return error!(ENODEV, String::from_utf8_lossy(fs_type)),
    };

    if kernel.selinux_enabled() {
        (|| {
            let label = match fs_type {
                b"tmpfs" => b"u:object_r:tmpfs:s0",
                _ => return,
            };
            fs.selinux_context.set(label.to_vec()).unwrap();
        })();
    }

    Ok(WhatToMount::Fs(fs))
}

#[derive(Default)]
pub struct ProcMountsFile {
    seq: Mutex<SeqFileState<()>>,
}

impl ProcMountsFile {
    pub fn new_node() -> impl FsNodeOps {
        SimpleFileNode::new(|| Ok(ProcMountsFile::default()))
    }
}

impl FileOps for ProcMountsFile {
    fileops_impl_seekable!();

    fn read_at(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        // TODO(tbodt): We should figure out a way to have a real iterator instead of grabbing the
        // entire list in one go. Should we have a BTreeMap<u64, Weak<Mount>> in the Namespace?
        // Also has the benefit of correct (i.e. chronological) ordering. But then we have to do
        // extra work to maintain it.
        let ns = current_task.fs().namespace();
        let iter = move |_cursor, sink: &mut SeqFileBuf| {
            for_each_mount(&ns.root_mount, &mut |mount| {
                let mountpoint = mount.mountpoint().unwrap_or_else(|| mount.root());
                let origin = NamespaceNode {
                    mount: mount.origin_mount.clone(),
                    entry: Arc::clone(&mount.root),
                };
                let fs_spec = String::from_utf8_lossy(&origin.path()).into_owned();
                let fs_file = String::from_utf8_lossy(&mountpoint.path()).into_owned();
                let fs_vfstype = "TODO";
                let fs_mntopts = "TODO";
                writeln!(sink, "{fs_spec} {fs_file} {fs_vfstype} {fs_mntopts} 0 0")?;
                Ok(())
            })?;
            Ok(None)
        };
        self.seq.lock().read_at(current_task, iter, offset, data)
    }

    fn write_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        error!(ENOSYS)
    }

    fn wait_async(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        waiter: &Waiter,
        _events: FdEvents,
        _handler: EventHandler,
    ) -> WaitKey {
        // Polling this file gives notifications when any change to mounts occurs. This is not
        // implemented yet, but stubbed for Android init.
        waiter.fake_wait()
    }

    fn cancel_wait(&self, _current_task: &CurrentTask, _waiter: &Waiter, _key: WaitKey) {}

    fn query_events(&self, _current_task: &CurrentTask) -> FdEvents {
        FdEvents::empty()
    }
}

pub struct ProcMountinfoFile {
    task: Arc<Task>,
    seq: Mutex<SeqFileState<()>>,
}

impl ProcMountinfoFile {
    pub fn new_node(task: &Arc<Task>) -> impl FsNodeOps {
        let task = Arc::clone(task);
        SimpleFileNode::new(move || {
            Ok(ProcMountinfoFile { task: Arc::clone(&task), seq: Default::default() })
        })
    }
}

impl FileOps for ProcMountinfoFile {
    fileops_impl_seekable!();

    fn read_at(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        // TODO(tbodt): We should figure out a way to have a real iterator instead of grabbing the
        // entire list in one go. Should we have a BTreeMap<u64, Weak<Mount>> in the Namespace?
        // Also has the benefit of correct (i.e. chronological) ordering. But then we have to do
        // extra work to maintain it.
        let ns = self.task.fs().namespace();
        let iter = move |_cursor, sink: &mut SeqFileBuf| {
            for_each_mount(&ns.root_mount, &mut |mount| {
                let mountpoint = mount.mountpoint().unwrap_or_else(|| mount.root());
                // Can't fail, mountpoint() and root() can't return a NamespaceNode with no mount
                let parent = mountpoint.mount.as_ref().unwrap();
                let origin = NamespaceNode {
                    mount: mount.origin_mount.clone(),
                    entry: Arc::clone(&mount.root),
                };
                write!(
                    sink,
                    "{} {} {} {} {}",
                    mount.id,
                    parent.id,
                    mount.root.node.fs().dev_id,
                    String::from_utf8_lossy(&origin.path()),
                    String::from_utf8_lossy(&mountpoint.path())
                )?;
                if let Some(peer_group) = mount.read().peer_group() {
                    write!(sink, " shared:{}", peer_group.id)?;
                }
                if let Some(upstream) = mount.read().upstream() {
                    write!(sink, " master:{}", upstream.id)?;
                }
                writeln!(sink)?;
                Ok(())
            })?;
            Ok(None)
        };
        self.seq.lock().read_at(current_task, iter, offset, data)
    }

    fn write_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        error!(ENOSYS)
    }
}

fn for_each_mount<E>(
    mount: &Arc<Mount>,
    callback: &mut impl FnMut(&MountHandle) -> Result<(), E>,
) -> Result<(), E> {
    callback(mount)?;
    // Collect list first to avoid self deadlock when ProcMountinfoFile::read_at tries to call
    // NamespaceNode::path()
    let submounts: Vec<_> = mount.read().submounts.values().map(Arc::clone).collect();
    for submount in submounts {
        for_each_mount(&submount, callback)?;
    }
    Ok(())
}

/// The `SymlinkMode` enum encodes how symlinks are followed during path traversal.
#[derive(PartialEq, Eq, Copy, Clone, Debug)]
pub enum SymlinkMode {
    /// Follow a symlink at the end of a path resolution.
    Follow,

    /// Do not follow a symlink at the end of a path resolution.
    NoFollow,
}

/// The maximum number of symlink traversals that can be made during path resolution.
const MAX_SYMLINK_FOLLOWS: u8 = 40;

/// The context passed during namespace lookups.
///
/// Namespace lookups need to mutate a shared context in order to correctly
/// count the number of remaining symlink traversals.
pub struct LookupContext {
    /// The SymlinkMode for the lookup.
    ///
    /// As the lookup proceeds, the follow count is decremented each time the
    /// lookup traverses a symlink.
    pub symlink_mode: SymlinkMode,

    /// The number of symlinks remaining the follow.
    ///
    /// Each time path resolution calls readlink, this value is decremented.
    pub remaining_follows: u8,

    /// Whether the result of the lookup must be a directory.
    ///
    /// For example, if the path ends with a `/` or if userspace passes
    /// O_DIRECTORY. This flag can be set to true if the lookup encounters a
    /// symlink that ends with a `/`.
    pub must_be_directory: bool,
}

impl LookupContext {
    pub fn new(symlink_mode: SymlinkMode) -> LookupContext {
        LookupContext {
            symlink_mode,
            remaining_follows: MAX_SYMLINK_FOLLOWS,
            must_be_directory: false,
        }
    }

    pub fn with(&self, symlink_mode: SymlinkMode) -> LookupContext {
        LookupContext {
            symlink_mode,
            remaining_follows: self.remaining_follows,
            must_be_directory: self.must_be_directory,
        }
    }

    pub fn update_for_path(&mut self, path: &FsStr) {
        if path.last() == Some(&b'/') {
            self.must_be_directory = true;
        }
    }
}

impl Default for LookupContext {
    fn default() -> Self {
        LookupContext::new(SymlinkMode::Follow)
    }
}

/// A node in a mount namespace.
///
/// This tree is a composite of the mount tree and the FsNode tree.
///
/// These nodes are used when traversing paths in a namespace in order to
/// present the client the directory structure that includes the mounted
/// filesystems.
#[derive(Clone)]
pub struct NamespaceNode {
    /// The mount where this namespace node is mounted.
    ///
    /// A given FsNode can be mounted in multiple places in a namespace. This
    /// field distinguishes between them.
    mount: Option<MountHandle>,

    /// The FsNode that corresponds to this namespace entry.
    pub entry: DirEntryHandle,
}

impl NamespaceNode {
    /// Create a namespace node that is not mounted in a namespace.
    ///
    /// The returned node does not have a name.
    pub fn new_anonymous(dir_entry: DirEntryHandle) -> Self {
        Self { mount: None, entry: dir_entry }
    }

    /// Create a FileObject corresponding to this namespace node.
    ///
    /// This function is the primary way of instantiating FileObjects. Each
    /// FileObject records the NamespaceNode that created it in order to
    /// remember its path in the Namespace.
    pub fn open(
        &self,
        current_task: &CurrentTask,
        flags: OpenFlags,
        check_access: bool,
    ) -> Result<FileHandle, Errno> {
        Ok(FileObject::new(
            self.entry.node.open(current_task, flags, check_access)?,
            self.clone(),
            flags,
        ))
    }

    pub fn open_create_node(
        &self,
        current_task: &CurrentTask,
        name: &FsStr,
        mode: FileMode,
        dev: DeviceType,
        flags: OpenFlags,
    ) -> Result<NamespaceNode, Errno> {
        let owner = current_task.as_fscred();
        let mode = current_task.fs().apply_umask(mode);
        Ok(self.with_new_entry(self.entry.open_create_node(
            current_task,
            name,
            mode,
            dev,
            owner,
            flags,
        )?))
    }

    pub fn create_node(
        &self,
        current_task: &CurrentTask,
        name: &FsStr,
        mode: FileMode,
        dev: DeviceType,
    ) -> Result<NamespaceNode, Errno> {
        let owner = current_task.as_fscred();
        let mode = current_task.fs().apply_umask(mode);
        Ok(self.with_new_entry(self.entry.create_node(current_task, name, mode, dev, owner)?))
    }

    pub fn symlink(
        &self,
        current_task: &CurrentTask,
        name: &FsStr,
        target: &FsStr,
    ) -> Result<NamespaceNode, Errno> {
        let owner = current_task.as_fscred();
        Ok(self.with_new_entry(self.entry.create_symlink(current_task, name, target, owner)?))
    }

    pub fn unlink(
        &self,
        current_task: &CurrentTask,
        name: &FsStr,
        kind: UnlinkKind,
        must_be_directory: bool,
    ) -> Result<(), Errno> {
        if DirEntry::is_reserved_name(name) {
            match kind {
                UnlinkKind::Directory => {
                    if name == b".." {
                        error!(ENOTEMPTY)
                    } else if self.parent().is_none() {
                        // The client is attempting to remove the root.
                        error!(EBUSY)
                    } else {
                        error!(EINVAL)
                    }
                }
                UnlinkKind::NonDirectory => error!(ENOTDIR),
            }
        } else {
            self.entry.unlink(current_task, name, kind, must_be_directory)
        }
    }

    /// Traverse down a parent-to-child link in the namespace.
    pub fn lookup_child(
        &self,
        current_task: &CurrentTask,
        context: &mut LookupContext,
        basename: &FsStr,
    ) -> Result<NamespaceNode, Errno> {
        if !self.entry.node.is_dir() {
            return error!(ENOTDIR);
        }

        let child = if basename == b"." || basename == b"" {
            self.clone()
        } else if basename == b".." {
            // Make sure this can't escape a chroot
            if *self == current_task.fs().root() {
                self.clone()
            } else {
                self.parent().unwrap_or_else(|| self.clone())
            }
        } else {
            let mut child =
                self.with_new_entry(self.entry.component_lookup(current_task, basename)?);
            while child.entry.node.is_lnk() {
                match context.symlink_mode {
                    SymlinkMode::NoFollow => {
                        break;
                    }
                    SymlinkMode::Follow => {
                        if context.remaining_follows == 0 {
                            return error!(ELOOP);
                        }
                        context.remaining_follows -= 1;
                        child = match child.entry.node.readlink(current_task)? {
                            SymlinkTarget::Path(link_target) => {
                                let link_directory = if link_target[0] == b'/' {
                                    current_task.fs().root()
                                } else {
                                    self.clone()
                                };
                                current_task.lookup_path(context, link_directory, &link_target)?
                            }
                            SymlinkTarget::Node(node) => node,
                        }
                    }
                };
            }

            child.enter_mount()
        };

        if context.must_be_directory && !child.entry.node.is_dir() {
            return error!(ENOTDIR);
        }

        Ok(child)
    }

    /// Traverse up a child-to-parent link in the namespace.
    ///
    /// This traversal matches the child-to-parent link in the underlying
    /// FsNode except at mountpoints, where the link switches from one
    /// filesystem to another.
    pub fn parent(&self) -> Option<NamespaceNode> {
        let mountpoint_or_self = self.escape_mount();
        Some(mountpoint_or_self.with_new_entry(mountpoint_or_self.entry.parent()?))
    }

    /// If this is a mount point, return the root of the mount. Otherwise return self.
    fn enter_mount(&self) -> NamespaceNode {
        // While the child is a mountpoint, replace child with the mount's root.
        fn enter_one_mount(node: &NamespaceNode) -> Option<NamespaceNode> {
            if let Some(mount) = &node.mount {
                if let Some(mount) = mount.state.read().submounts.get(ArcKey::ref_cast(&node.entry))
                {
                    return Some(mount.root());
                }
            }
            None
        }
        let mut inner = self.clone();
        while let Some(inner_root) = enter_one_mount(&inner) {
            inner = inner_root;
        }
        inner
    }

    /// If this is the root of a mount, return the mount point. Otherwise return self.
    ///
    /// This is not exactly the same as parent(). If parent() is called on a root, it will escape
    /// the mount, but then return the parent of the mount point instead of the mount point.
    fn escape_mount(&self) -> NamespaceNode {
        let mut mountpoint_or_self = self.clone();
        while let Some(mountpoint) = mountpoint_or_self.mountpoint() {
            mountpoint_or_self = mountpoint;
        }
        mountpoint_or_self
    }

    /// If this node is the root of a mount, return it. Otherwise EINVAL.
    pub fn mount_if_root(&self) -> Result<&MountHandle, Errno> {
        if let Some(mount) = &self.mount {
            if Arc::ptr_eq(&self.entry, &mount.root) {
                return Ok(mount);
            }
        }
        error!(EINVAL)
    }

    /// Returns the mountpoint at this location in the namespace.
    ///
    /// If this node is mounted in another node, this function returns the node
    /// at which this node is mounted. Otherwise, returns None.
    fn mountpoint(&self) -> Option<NamespaceNode> {
        self.mount_if_root().ok()?.mountpoint()
    }

    /// The path from the root of the namespace to this node.
    pub fn path(&self) -> FsString {
        if self.mount.is_none() {
            return self.entry.local_name().to_vec();
        }
        let mut components = vec![];
        let mut current = self.escape_mount();
        while let Some(parent) = current.parent() {
            components.push(current.entry.local_name().to_vec());
            current = parent.escape_mount();
        }
        if components.is_empty() {
            return b"/".to_vec();
        }
        components.push(vec![]);
        components.reverse();
        components.join(&b'/')
    }

    pub fn mount(&self, what: WhatToMount, flags: MountFlags) -> Result<(), Errno> {
        let mountpoint = self.enter_mount();
        let mount = mountpoint.mount.as_ref().expect("a mountpoint must be part of a mount");
        mount.create_submount(&mountpoint.entry, what, flags);
        Ok(())
    }

    /// If this is the root of a filesystem, unmount. Otherwise return EINVAL.
    pub fn unmount(&self) -> Result<(), Errno> {
        // TODO(fxbug.dev/115333): Propagate unmounts the same way we propagate mounts
        let mountpoint = self.enter_mount().mountpoint().ok_or_else(|| errno!(EINVAL))?;
        let mount = mountpoint.mount.as_ref().expect("a mountpoint must be part of a mount");
        let mut mount_state = mount.state.write();
        // TODO(tbodt): EBUSY
        mount_state.submounts.remove(mountpoint.mount_hash_key()).ok_or_else(|| errno!(EINVAL))?;
        Ok(())
    }

    pub fn mount_eq(a: &NamespaceNode, b: &NamespaceNode) -> bool {
        a.mount.as_ref().map(Arc::as_ptr) == b.mount.as_ref().map(Arc::as_ptr)
    }

    fn with_new_entry(&self, entry: DirEntryHandle) -> NamespaceNode {
        NamespaceNode { mount: self.mount.clone(), entry }
    }

    fn mount_hash_key(&self) -> &ArcKey<DirEntry> {
        ArcKey::ref_cast(&self.entry)
    }
}

impl fmt::Debug for NamespaceNode {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("NamespaceNode")
            .field("path", &String::from_utf8_lossy(&self.path()))
            .field("mount", &self.mount)
            .field("entry", &self.entry)
            .finish()
    }
}

// Eq/Hash impls intended for the MOUNT_POINTS hash
impl PartialEq for NamespaceNode {
    fn eq(&self, other: &Self) -> bool {
        self.mount.as_ref().map(Arc::as_ptr).eq(&other.mount.as_ref().map(Arc::as_ptr))
            && Arc::ptr_eq(&self.entry, &other.entry)
    }
}
impl Eq for NamespaceNode {}
impl Hash for NamespaceNode {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.mount.as_ref().map(Arc::as_ptr).hash(state);
        Arc::as_ptr(&self.entry).hash(state);
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::fs::tmpfs::TmpFs;
    use crate::testing::*;

    #[::fuchsia::test]
    fn test_namespace() -> anyhow::Result<()> {
        let (kernel, current_task) = create_kernel_and_task();
        let root_fs = TmpFs::new_fs(&kernel);
        let root_node = Arc::clone(root_fs.root());
        let _dev_node = root_node.create_dir(&current_task, b"dev").expect("failed to mkdir dev");
        let dev_fs = TmpFs::new_fs(&kernel);
        let dev_root_node = Arc::clone(dev_fs.root());
        let _dev_pts_node =
            dev_root_node.create_dir(&current_task, b"pts").expect("failed to mkdir pts");

        let ns = Namespace::new(root_fs);
        let mut context = LookupContext::default();
        let dev = ns
            .root()
            .lookup_child(&current_task, &mut context, b"dev")
            .expect("failed to lookup dev");
        dev.mount(WhatToMount::Fs(dev_fs), MountFlags::empty())
            .expect("failed to mount dev root node");

        let mut context = LookupContext::default();
        let dev = ns
            .root()
            .lookup_child(&current_task, &mut context, b"dev")
            .expect("failed to lookup dev");
        let mut context = LookupContext::default();
        let pts =
            dev.lookup_child(&current_task, &mut context, b"pts").expect("failed to lookup pts");
        let pts_parent =
            pts.parent().ok_or_else(|| errno!(ENOENT)).expect("failed to get parent of pts");
        assert!(Arc::ptr_eq(&pts_parent.entry, &dev.entry));

        let dev_parent =
            dev.parent().ok_or_else(|| errno!(ENOENT)).expect("failed to get parent of dev");
        assert!(Arc::ptr_eq(&dev_parent.entry, &ns.root().entry));
        Ok(())
    }

    #[::fuchsia::test]
    fn test_mount_does_not_upgrade() -> anyhow::Result<()> {
        let (kernel, current_task) = create_kernel_and_task();
        let root_fs = TmpFs::new_fs(&kernel);
        let root_node = Arc::clone(root_fs.root());
        let _dev_node = root_node.create_dir(&current_task, b"dev").expect("failed to mkdir dev");
        let dev_fs = TmpFs::new_fs(&kernel);
        let dev_root_node = Arc::clone(dev_fs.root());
        let _dev_pts_node =
            dev_root_node.create_dir(&current_task, b"pts").expect("failed to mkdir pts");

        let ns = Namespace::new(root_fs);
        let mut context = LookupContext::default();
        let dev = ns
            .root()
            .lookup_child(&current_task, &mut context, b"dev")
            .expect("failed to lookup dev");
        dev.mount(WhatToMount::Fs(dev_fs), MountFlags::empty())
            .expect("failed to mount dev root node");
        let mut context = LookupContext::default();
        let new_dev = ns
            .root()
            .lookup_child(&current_task, &mut context, b"dev")
            .expect("failed to lookup dev again");
        assert!(!Arc::ptr_eq(&dev.entry, &new_dev.entry));
        assert_ne!(&dev, &new_dev);

        let mut context = LookupContext::default();
        let _new_pts = new_dev
            .lookup_child(&current_task, &mut context, b"pts")
            .expect("failed to lookup pts");
        let mut context = LookupContext::default();
        assert!(dev.lookup_child(&current_task, &mut context, b"pts").is_err());

        Ok(())
    }

    #[::fuchsia::test]
    fn test_path() -> anyhow::Result<()> {
        let (kernel, current_task) = create_kernel_and_task();
        let root_fs = TmpFs::new_fs(&kernel);
        let root_node = Arc::clone(root_fs.root());
        let _dev_node = root_node.create_dir(&current_task, b"dev").expect("failed to mkdir dev");
        let dev_fs = TmpFs::new_fs(&kernel);
        let dev_root_node = Arc::clone(dev_fs.root());
        let _dev_pts_node =
            dev_root_node.create_dir(&current_task, b"pts").expect("failed to mkdir pts");

        let ns = Namespace::new(root_fs);
        let mut context = LookupContext::default();
        let dev = ns
            .root()
            .lookup_child(&current_task, &mut context, b"dev")
            .expect("failed to lookup dev");
        dev.mount(WhatToMount::Fs(dev_fs), MountFlags::empty())
            .expect("failed to mount dev root node");

        let mut context = LookupContext::default();
        let dev = ns
            .root()
            .lookup_child(&current_task, &mut context, b"dev")
            .expect("failed to lookup dev");
        let mut context = LookupContext::default();
        let pts =
            dev.lookup_child(&current_task, &mut context, b"pts").expect("failed to lookup pts");

        assert_eq!(b"/".to_vec(), ns.root().path());
        assert_eq!(b"/dev".to_vec(), dev.path());
        assert_eq!(b"/dev/pts".to_vec(), pts.path());
        Ok(())
    }

    #[::fuchsia::test]
    fn test_shadowing() -> anyhow::Result<()> {
        let (kernel, current_task) = create_kernel_and_task();
        let root_fs = TmpFs::new_fs(&kernel);
        let ns = Namespace::new(root_fs.clone());
        let _foo_node = root_fs.root().create_dir(&current_task, b"foo")?;
        let mut context = LookupContext::default();
        let foo_dir = ns.root().lookup_child(&current_task, &mut context, b"foo")?;

        let foofs1 = TmpFs::new_fs(&kernel);
        foo_dir.mount(WhatToMount::Fs(foofs1.clone()), MountFlags::empty())?;
        let mut context = LookupContext::default();
        assert!(Arc::ptr_eq(
            &ns.root().lookup_child(&current_task, &mut context, b"foo")?.entry,
            foofs1.root()
        ));
        let foo_dir = ns.root().lookup_child(&current_task, &mut context, b"foo")?;

        let ns_clone = ns.clone_namespace();

        let foofs2 = TmpFs::new_fs(&kernel);
        foo_dir.mount(WhatToMount::Fs(foofs2.clone()), MountFlags::empty())?;
        let mut context = LookupContext::default();
        assert!(Arc::ptr_eq(
            &ns.root().lookup_child(&current_task, &mut context, b"foo")?.entry,
            foofs2.root()
        ));

        assert!(Arc::ptr_eq(
            &ns_clone
                .root()
                .lookup_child(&current_task, &mut LookupContext::default(), b"foo")?
                .entry,
            foofs1.root()
        ));

        Ok(())
    }

    #[::fuchsia::test]
    fn test_unlink_mounted_directory() -> anyhow::Result<()> {
        let (kernel, current_task) = create_kernel_and_task();
        let root_fs = TmpFs::new_fs(&kernel);
        let ns1 = Namespace::new(root_fs.clone());
        let ns2 = Namespace::new(root_fs.clone());
        let _foo_node = root_fs.root().create_dir(&current_task, b"foo")?;
        let mut context = LookupContext::default();
        let foo_dir = ns1.root().lookup_child(&current_task, &mut context, b"foo")?;

        let foofs = TmpFs::new_fs(&kernel);
        foo_dir.mount(WhatToMount::Fs(foofs), MountFlags::empty())?;

        assert_eq!(
            errno!(EBUSY),
            ns2.root().unlink(&current_task, b"foo", UnlinkKind::Directory, false).unwrap_err()
        );

        Ok(())
    }
}
