// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_zircon::{self as zx, VmoOptions};
use parking_lot::Mutex;
use std::collections::HashMap;
use std::ops::Bound;
use std::sync::{Arc, Weak};

use crate::devices::*;
use crate::fd_impl_nonseekable;
use crate::fs::pipe::Pipe;
use crate::fs::*;
use crate::mm::vmo::round_up_to_system_page_size;
use crate::task::*;
use crate::types::*;

pub struct TmpFs {
    nodes: Mutex<HashMap<ino_t, FsNodeHandle>>,
}
impl FileSystemOps for Arc<TmpFs> {}

impl TmpFs {
    pub fn new() -> FileSystemHandle {
        let tmpfs_dev = AnonNodeDevice::new(0);
        let fs = Arc::new(TmpFs { nodes: Mutex::new(HashMap::new()) });
        FileSystem::new(
            fs.clone(),
            FsNode::new_root(TmpfsDirectory { fs: Arc::downgrade(&fs) }, tmpfs_dev),
        )
    }

    pub fn register(&self, node: &FsNodeHandle) {
        self.nodes.lock().insert(node.info().inode_num, Arc::clone(node));
    }

    pub fn unregister(&self, node: &FsNodeHandle) {
        self.nodes.lock().remove(&node.info().inode_num);
    }
}

struct TmpfsDirectory {
    /// The file system to which this directory belongs.
    fs: Weak<TmpFs>,
}

impl FsNodeOps for TmpfsDirectory {
    fn open(&self, _node: &FsNode, _flags: OpenFlags) -> Result<Box<dyn FileOps>, Errno> {
        Ok(Box::new(DirectoryFileObject::new()))
    }

    fn lookup(&self, _parent: &FsNode, _child: FsNode) -> Result<FsNodeHandle, Errno> {
        Err(ENOENT)
    }

    fn mkdir(&self, _parent: &FsNode, mut child: FsNode) -> Result<FsNodeHandle, Errno> {
        child.set_ops(TmpfsDirectory { fs: self.fs.clone() });
        let child = child.into_handle();
        self.fs.upgrade().unwrap().register(&child);
        Ok(child)
    }

    fn mknod(&self, _parent: &FsNode, mut child: FsNode) -> Result<FsNodeHandle, Errno> {
        match child.info_mut().mode.fmt() {
            FileMode::IFREG => child.set_ops(VmoFileNode::new()?),
            FileMode::IFIFO => child.set_ops(FifoNode::new()),
            _ => return Err(EACCES),
        }
        let child = child.into_handle();
        self.fs.upgrade().unwrap().register(&child);
        Ok(child)
    }

    fn create_symlink(&self, mut child: FsNode, target: &FsStr) -> Result<FsNodeHandle, Errno> {
        assert!(child.info_mut().mode.fmt() == FileMode::IFLNK);
        child.set_ops(SymlinkNode::new(target));
        let child = child.into_handle();
        self.fs.upgrade().unwrap().register(&child);
        Ok(child)
    }

    fn unlink(
        &self,
        _parent: &FsNode,
        child: &FsNodeHandle,
        _kind: UnlinkKind,
    ) -> Result<(), Errno> {
        // TODO: When we have hard links, we'll need to check the link count.
        self.fs.upgrade().unwrap().unregister(child);
        Ok(())
    }
}

struct DirectoryFileObject {
    /// The current position for readdir.
    ///
    /// When readdir is called multiple times, we need to return subsequent
    /// directory entries. This field records where the previous readdir
    /// stopped.
    ///
    /// The state is actually recorded twice: once in the offset for this
    /// FileObject and again here. Recovering the state from the offset is slow
    /// because we would need to iterate through the keys of the BTree. Having
    /// the FsString cached lets us search the keys of the BTree faster.
    ///
    /// The initial "." and ".." entries are not recorded here. They are
    /// represented only in the offset field in the FileObject.
    readdir_position: Mutex<Bound<FsString>>,
}

impl DirectoryFileObject {
    fn new() -> DirectoryFileObject {
        DirectoryFileObject { readdir_position: Mutex::new(Bound::Unbounded) }
    }
}

impl FileOps for DirectoryFileObject {
    // TODO: seek should interact with directory listing.
    fd_impl_nonseekable!();

    fn read(&self, _file: &FileObject, _task: &Task, _data: &[UserBuffer]) -> Result<usize, Errno> {
        Err(EISDIR)
    }

    fn write(
        &self,
        _file: &FileObject,
        _task: &Task,
        _data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        Err(EISDIR)
    }

    fn readdir(
        &self,
        file: &FileObject,
        _task: &Task,
        sink: &mut dyn DirentSink,
    ) -> Result<(), Errno> {
        let mut offset = file.offset.lock();
        let mut readdir_position = self.readdir_position.lock();
        if *offset == 0 {
            *readdir_position = Bound::Unbounded;
            sink.add(file.node().info().inode_num, 1, DirectoryEntryType::DIR, b".")?;
            *offset += 1;
        }
        if *offset == 1 {
            sink.add(
                file.node().parent().unwrap_or_else(|| file.node()).info().inode_num,
                2,
                DirectoryEntryType::DIR,
                b"..",
            )?;
            *offset += 1;
        }
        let children = file.node().children();
        for (name, node_cell) in children.range((readdir_position.clone(), Bound::Unbounded)) {
            if let Some(node) = node_cell.get().and_then(|weak| weak.upgrade()) {
                let next_offset = *offset + 1;
                let info = node.info();
                sink.add(
                    info.inode_num,
                    next_offset,
                    DirectoryEntryType::from_mode(info.mode),
                    &name,
                )?;
                *offset = next_offset;
                *readdir_position = Bound::Excluded(name.to_vec());
            }
        }
        Ok(())
    }
}

struct VmoFileNode {
    /// The memory that backs this file.
    vmo: Arc<zx::Vmo>,
}

impl VmoFileNode {
    fn new() -> Result<VmoFileNode, Errno> {
        let vmo = zx::Vmo::create_with_opts(VmoOptions::RESIZABLE, 0).map_err(|_| ENOMEM)?;
        Ok(VmoFileNode { vmo: Arc::new(vmo) })
    }
}

impl FsNodeOps for VmoFileNode {
    fn open(&self, _node: &FsNode, _flags: OpenFlags) -> Result<Box<dyn FileOps>, Errno> {
        Ok(Box::new(VmoFileObject::new(self.vmo.clone())))
    }

    fn truncate(&self, node: &FsNode, length: u64) -> Result<(), Errno> {
        let mut info = node.info_write();
        let storage = round_up_to_system_page_size(length as usize);
        let shrink = storage < info.storage_size;
        if storage != info.storage_size {
            self.vmo.set_size(storage as u64).map_err(|_| ENOMEM)?;
            info.storage_size = storage;
        }
        info.size = length as usize;
        let time = fuchsia_runtime::utc_time();
        info.time_access = time;
        info.time_modify = time;
        // Make sure there is no stale data if the file
        // is truncated smaller then larger.
        let padding = (info.storage_size - info.size) as u64;
        if shrink && padding > 0 {
            self.vmo.op_range(zx::VmoOp::ZERO, length, padding).map_err(|_| EIO)?;
        }
        Ok(())
    }
}

struct FifoNode {
    /// The pipe located at this node.
    pipe: Arc<Mutex<Pipe>>,
}

impl FifoNode {
    fn new() -> FifoNode {
        FifoNode { pipe: Pipe::new() }
    }
}

impl FsNodeOps for FifoNode {
    fn open(&self, _node: &FsNode, flags: OpenFlags) -> Result<Box<dyn FileOps>, Errno> {
        Ok(Pipe::open(&self.pipe, flags))
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use fuchsia_async as fasync;
    use zerocopy::AsBytes;

    use crate::mm::*;
    use crate::testing::*;

    #[test]
    fn test_tmpfs() {
        let fs = TmpFs::new();
        let root = fs.root();
        let usr = root.mkdir(b"usr").unwrap();
        let _etc = root.mkdir(b"etc").unwrap();
        let _usr_bin = usr.mkdir(b"bin").unwrap();
        let mut names = root.copy_child_names();
        names.sort();
        assert!(names.iter().eq([b"etc", b"usr"].iter()));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_write_read() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let task = &task_owner.task;

        let test_mem_size = 0x10000;
        let test_vmo = zx::Vmo::create(test_mem_size).unwrap();

        let path = b"test.bin";
        let _file = task.fs.root.mknod(path, FileMode::IFREG | FileMode::ALLOW_ALL, 0).unwrap();

        let wr_file = task.open_file(path, OpenFlags::RDWR).unwrap();

        let flags = zx::VmarFlags::PERM_READ | zx::VmarFlags::PERM_WRITE;
        let test_addr = task
            .mm
            .map(
                UserAddress::default(),
                test_vmo,
                0,
                test_mem_size as usize,
                flags,
                MappingOptions::empty(),
            )
            .unwrap();

        let seq_addr = UserAddress::from_ptr(test_addr.ptr() + path.len());
        let test_seq = 0..10000u16;
        let test_vec = test_seq.collect::<Vec<_>>();
        let test_bytes = test_vec.as_slice().as_bytes();
        task.mm.write_memory(seq_addr, test_bytes).unwrap();
        let buf = [UserBuffer { address: seq_addr, length: test_bytes.len() }];

        let written = wr_file.write(task, &buf).unwrap();
        assert_eq!(written, test_bytes.len());

        let mut read_vec = vec![0u8; test_bytes.len()];
        task.mm.read_memory(seq_addr, read_vec.as_bytes_mut()).unwrap();

        assert_eq!(test_bytes, &*read_vec);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_permissions() {
        let (_kernel, task_owner) = create_kernel_and_task();
        let task = &task_owner.task;

        let path = b"test.bin";
        let file = task
            .open_file_at(
                FdNumber::AT_FDCWD,
                path,
                OpenFlags::CREAT | OpenFlags::RDONLY,
                FileMode::ALLOW_ALL,
            )
            .expect("failed to create file");
        assert_eq!(0, file.read(task, &[]).expect("failed to read"));
        assert!(file.write(task, &[]).is_err());

        let file = task
            .open_file_at(FdNumber::AT_FDCWD, path, OpenFlags::WRONLY, FileMode::ALLOW_ALL)
            .expect("failed to open file WRONLY");
        assert!(file.read(task, &[]).is_err());
        assert_eq!(0, file.write(task, &[]).expect("failed to write"));

        let file = task
            .open_file_at(FdNumber::AT_FDCWD, path, OpenFlags::RDWR, FileMode::ALLOW_ALL)
            .expect("failed to open file RDWR");
        assert_eq!(0, file.read(task, &[]).expect("failed to read"));
        assert_eq!(0, file.write(task, &[]).expect("failed to write"));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_persistence() {
        let fs = TmpFs::new();
        {
            let root = fs.root();
            let usr = root.mkdir(b"usr").expect("failed to create usr");
            root.mkdir(b"etc").expect("failed to create usr/etc");
            usr.mkdir(b"bin").expect("failed to create usr/bin");
        }

        // At this point, all the nodes are dropped.

        let (_kernel, task_owner) = create_kernel_and_task_with_fs(FsContext::new(fs));
        let task = &task_owner.task;

        task.open_file(b"/usr/bin", OpenFlags::RDONLY | OpenFlags::DIRECTORY)
            .expect("failed to open /usr/bin");
        assert_eq!(ENOENT, task.open_file(b"/usr/bin/test.txt", OpenFlags::RDWR).unwrap_err());
        task.open_file_at(
            FdNumber::AT_FDCWD,
            b"/usr/bin/test.txt",
            OpenFlags::RDWR | OpenFlags::CREAT,
            FileMode::ALLOW_ALL,
        )
        .expect("failed to create test.txt");
        let txt =
            task.open_file(b"/usr/bin/test.txt", OpenFlags::RDWR).expect("failed to open test.txt");

        let usr_bin =
            task.open_file(b"/usr/bin", OpenFlags::RDONLY).expect("failed to open /usr/bin");
        usr_bin
            .name()
            .unlink(&task.fs, b"test.txt", UnlinkKind::NonDirectory)
            .expect("failed to unlink test.text");
        assert_eq!(ENOENT, task.open_file(b"/usr/bin/test.txt", OpenFlags::RDWR).unwrap_err());
        assert_eq!(
            ENOENT,
            usr_bin.name().unlink(&task.fs, b"test.txt", UnlinkKind::NonDirectory).unwrap_err()
        );

        assert_eq!(0, txt.read(task, &[]).expect("failed to read"));
        std::mem::drop(txt);
        assert_eq!(ENOENT, task.open_file(b"/usr/bin/test.txt", OpenFlags::RDWR).unwrap_err());
        std::mem::drop(usr_bin);

        let usr = task.open_file(b"/usr", OpenFlags::RDONLY).expect("failed to open /usr");
        assert_eq!(ENOENT, task.open_file(b"/usr/foo", OpenFlags::RDONLY).unwrap_err());
        usr.name()
            .unlink(&task.fs, b"bin", UnlinkKind::Directory)
            .expect("failed to unlink /usr/bin");
        assert_eq!(ENOENT, task.open_file(b"/usr/bin", OpenFlags::RDONLY).unwrap_err());
    }
}
