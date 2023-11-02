// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    fs::{
        fs_node_impl_not_dir, CacheMode, FileHandle, FileObject, FileOps, FileSystem,
        FileSystemHandle, FileSystemOps, FileSystemOptions, FsNode, FsNodeInfo, FsNodeOps, FsStr,
    },
    task::{CurrentTask, Kernel},
    types::{error, ino_t, statfs, Errno, FileMode, OpenFlags, ANON_INODE_FS_MAGIC},
};
use std::sync::Arc;

pub struct Anon;

impl FsNodeOps for Anon {
    fs_node_impl_not_dir!();

    fn create_file_ops(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        error!(ENOSYS)
    }
}

impl Anon {
    pub fn new_file_extended(
        kernel: &Arc<Kernel>,
        ops: Box<dyn FileOps>,
        flags: OpenFlags,
        info: impl FnOnce(ino_t) -> FsNodeInfo,
    ) -> FileHandle {
        let fs = anon_fs(kernel);
        FileObject::new_anonymous(ops, fs.create_node(Anon, info), flags)
    }

    pub fn new_file(
        current_task: &CurrentTask,
        ops: Box<dyn FileOps>,
        flags: OpenFlags,
    ) -> FileHandle {
        Self::new_file_extended(
            current_task.kernel(),
            ops,
            flags,
            FsNodeInfo::new_factory(FileMode::from_bits(0o600), current_task.as_fscred()),
        )
    }
}

struct AnonFs;
impl FileSystemOps for AnonFs {
    fn statfs(&self, _fs: &FileSystem, _current_task: &CurrentTask) -> Result<statfs, Errno> {
        Ok(statfs::default(ANON_INODE_FS_MAGIC))
    }
    fn name(&self) -> &'static FsStr {
        b"anon"
    }
}
pub fn anon_fs(kernel: &Arc<Kernel>) -> &FileSystemHandle {
    kernel.anon_fs.get_or_init(|| {
        FileSystem::new(kernel, CacheMode::Uncached, AnonFs, FileSystemOptions::default())
    })
}
