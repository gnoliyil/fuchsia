// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        auth::FsCred,
        fs::{
            fuchsia::{update_into_from_attrs, RemoteFileObject},
            *,
        },
        lock::{RwLockReadGuard, RwLockWriteGuard},
        logging::log_warn,
        task::{CurrentTask, Kernel},
        types::*,
    },
    anyhow::{anyhow, ensure, Error},
    ext4_metadata::{Node, NodeInfo},
    fidl_fuchsia_io as fio,
    fuchsia_zircon::{self as zx, HandleBased},
    std::{io::Read, sync::Arc},
    syncio::Zxio,
};

use ext4_metadata::Metadata;

/// RemoteBundle is a remote, immutable filesystem that stores additional metadata that would
/// otherwise not be available.  The metadata exists in the "metadata.v1" file, which contains
/// directory, symbolic link and extended attribute information.  Only the content for files are
/// accessed remotely as normal.
pub struct RemoteBundle {
    metadata: Metadata,
    root: Arc<syncio::Zxio>,
    rights: fio::OpenFlags,
}

impl RemoteBundle {
    /// Returns a new RemoteBundle filesystem that can be found at `path` relative to `base`.
    pub fn new_fs(
        kernel: &Kernel,
        base: &fio::DirectorySynchronousProxy,
        rights: fio::OpenFlags,
        path: &str,
    ) -> Result<FileSystemHandle, Error> {
        let root = syncio::directory_open_directory_async(base, path, rights)
            .map_err(|e| anyhow!("Failed to open root: {}", e))?
            .into_channel();

        let (metadata_file, server) = zx::Channel::create();
        fdio::open_at(&root, "metadata.v1", fio::OpenFlags::RIGHT_READABLE, server)?;
        let mut metadata_file: std::fs::File = fdio::create_fd(metadata_file.into())?;
        let mut buf = Vec::new();
        metadata_file.read_to_end(&mut buf)?;
        let metadata = Metadata::deserialize(&buf)?;

        // Make sure the root node exists.
        ensure!(
            metadata.get(ext4_metadata::ROOT_INODE_NUM).is_some(),
            "Root node does not exist in remote bundle"
        );

        let root = Arc::new(
            Zxio::create(root.into_handle()).map_err(|status| from_status_like_fdio!(status))?,
        );
        let mut root_node = FsNode::new_root(DirectoryObject);
        root_node.inode_num = ext4_metadata::ROOT_INODE_NUM;
        Ok(FileSystem::new_with_root(kernel, RemoteBundle { metadata, root, rights }, root_node))
    }

    // Returns the bundle from the filesystem.  Panics if the filesystem isn't associated with a
    // RemoteBundle.
    fn from_fs(fs: &FileSystem) -> &RemoteBundle {
        fs.downcast_ops::<RemoteBundle>().unwrap()
    }

    // Returns a reference to the node identified by `inode_num`.  Panics if the node is not found
    // so this should only be used if the node is known to exist (e.g. the node must exist after
    // `lookup` has run for the relevant node).
    fn get_node(&self, inode_num: u64) -> &Node {
        self.metadata.get(inode_num).unwrap()
    }
}

impl FileSystemOps for RemoteBundle {
    fn statfs(&self, _fs: &FileSystem) -> Result<statfs, Errno> {
        const REMOTE_BUNDLE_FS_MAGIC: u32 = u32::from_be_bytes(*b"bndl");
        Ok(statfs::default(REMOTE_BUNDLE_FS_MAGIC))
    }
}

struct File {
    /// The underlying Zircon I/O object for this remote file.
    ///
    /// We delegate to the zxio library for actually doing I/O with remote
    /// file objects.
    zxio: Arc<syncio::Zxio>,
}

impl FsNodeOps for File {
    fn create_file_ops(
        &self,
        _node: &FsNode,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        let zxio = (*self.zxio).clone().map_err(|status| from_status_like_fdio!(status))?;
        Ok(Box::new(RemoteFileObject::new(zxio)))
    }

    fn update_info<'a>(&self, node: &'a FsNode) -> Result<RwLockReadGuard<'a, FsNodeInfo>, Errno> {
        let attrs = self.zxio.attr_get().map_err(|status| from_status_like_fdio!(status))?;
        let mut info = node.info_write();
        update_into_from_attrs(&mut info, &attrs);
        Ok(RwLockWriteGuard::downgrade(info))
    }

    fn get_xattr(
        &self,
        node: &FsNode,
        name: &crate::fs::FsStr,
    ) -> Result<FsString, crate::types::Errno> {
        let fs = node.fs();
        let bundle = RemoteBundle::from_fs(&fs);
        Ok(bundle
            .get_node(node.inode_num)
            .extended_attributes
            .get(name)
            .ok_or(errno!(ENOENT))?
            .to_vec())
    }

    fn list_xattrs(&self, node: &FsNode) -> Result<Vec<crate::fs::FsString>, crate::types::Errno> {
        let fs = node.fs();
        let bundle = RemoteBundle::from_fs(&fs);
        Ok(bundle
            .get_node(node.inode_num)
            .extended_attributes
            .keys()
            .map(|k| k.clone().to_vec())
            .collect())
    }
}

struct DirectoryObject;

impl FileOps for DirectoryObject {
    fileops_impl_directory!();

    fn seek(
        &self,
        file: &FileObject,
        _current_task: &CurrentTask,
        offset: off_t,
        whence: SeekOrigin,
    ) -> Result<off_t, Errno> {
        let mut current_offset = file.offset.lock();
        let new_offset = match whence {
            SeekOrigin::Set => offset,
            SeekOrigin::Cur => current_offset.checked_add(offset).ok_or(errno!(EINVAL))?,
            SeekOrigin::End => return error!(EINVAL),
        };
        if new_offset < 0 {
            return error!(EINVAL);
        }
        *current_offset = new_offset;
        Ok(new_offset)
    }

    fn readdir(
        &self,
        file: &FileObject,
        _current_task: &CurrentTask,
        sink: &mut dyn DirentSink,
    ) -> Result<(), Errno> {
        let bundle = RemoteBundle::from_fs(&file.fs);
        let mut child_iter =
            bundle.get_node(file.node().inode_num).directory().ok_or(errno!(EIO))?.children.iter();

        // Skip to the iterator position.
        for _ in 0..sink.offset() {
            if child_iter.next().is_none() {
                return Ok(());
            }
        }

        for (name, inode_num) in child_iter {
            let node = bundle.metadata.get(*inode_num).ok_or(errno!(EIO))?;
            sink.add(
                *inode_num,
                sink.offset() + 1,
                DirectoryEntryType::from_mode(FileMode::from_bits(node.mode.into())),
                name.as_bytes(),
            )?;
        }

        Ok(())
    }
}

impl FsNodeOps for DirectoryObject {
    fn create_file_ops(
        &self,
        _node: &FsNode,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        Ok(Box::new(DirectoryObject))
    }

    fn lookup(
        &self,
        node: &FsNode,
        _current_task: &CurrentTask,
        name: &FsStr,
    ) -> Result<FsNodeHandle, Errno> {
        let name = std::str::from_utf8(name).map_err(|_| {
            log_warn!("bad utf8 in pathname! remote filesystems can't handle this");
            errno!(EINVAL)
        })?;

        let fs = node.fs();
        let bundle = RemoteBundle::from_fs(&fs);
        let metadata = &bundle.metadata;
        let inode_num = metadata.lookup(node.inode_num, name).map_err(|_| errno!(ENOENT))?;
        let metadata_node = metadata.get(inode_num).ok_or(errno!(EIO))?;
        let mode = FileMode::from_bits(metadata_node.mode.into());
        let owner = FsCred { uid: metadata_node.uid.into(), gid: metadata_node.gid.into() };

        match metadata_node.info() {
            NodeInfo::Symlink(_) => {
                let child = fs.create_node_with_id(Box::new(SymlinkObject), inode_num, mode, owner);
                {
                    let mut info = child.info_write();
                    // For now, we just use some made up values. We might need to revisit this.
                    info.size = 1;
                    info.storage_size = 1;
                    info.blksize = 512;
                    info.link_count = 1;
                }
                Ok(child)
            }
            NodeInfo::Directory(_) => {
                let child =
                    fs.create_node_with_id(Box::new(DirectoryObject), inode_num, mode, owner);
                {
                    let mut info = child.info_write();
                    // For now, we just use some made up values. We might need to revisit this.
                    info.size = 1;
                    info.storage_size = 1;
                    info.blksize = 512;
                    info.link_count = 1;
                }
                Ok(child)
            }
            NodeInfo::File(_) => {
                let zxio = Arc::new(
                    bundle
                        .root
                        .open(bundle.rights, &format!("{inode_num}"))
                        .map_err(|status| from_status_like_fdio!(status, name))?,
                );

                // TODO: It's unfortunate to have another round-trip. We should be able
                // to set the mode based on the information we get during open.
                let attrs = zxio.attr_get().map_err(|status| from_status_like_fdio!(status))?;

                let child = fs.create_node_with_id(Box::new(File { zxio }), inode_num, mode, owner);

                update_into_from_attrs(&mut child.info_write(), &attrs);
                Ok(child)
            }
        }
    }

    fn update_info<'a>(&self, node: &'a FsNode) -> Result<RwLockReadGuard<'a, FsNodeInfo>, Errno> {
        Ok(node.info())
    }

    fn get_xattr(
        &self,
        node: &FsNode,
        name: &crate::fs::FsStr,
    ) -> Result<FsString, crate::types::Errno> {
        let fs = node.fs();
        let bundle = RemoteBundle::from_fs(&fs);
        let value = bundle
            .get_node(node.inode_num)
            .extended_attributes
            .get(name)
            .ok_or(errno!(ENOENT))?
            .to_vec();
        Ok(value)
    }

    fn list_xattrs(&self, node: &FsNode) -> Result<Vec<crate::fs::FsString>, crate::types::Errno> {
        let fs = node.fs();
        let bundle = RemoteBundle::from_fs(&fs);
        Ok(bundle
            .get_node(node.inode_num)
            .extended_attributes
            .keys()
            .map(|k| k.clone().to_vec())
            .collect())
    }
}

struct SymlinkObject;

impl FsNodeOps for SymlinkObject {
    fs_node_impl_symlink!();

    fn readlink(&self, node: &FsNode, _current_task: &CurrentTask) -> Result<SymlinkTarget, Errno> {
        let fs = node.fs();
        let bundle = RemoteBundle::from_fs(&fs);
        let target = bundle.get_node(node.inode_num).symlink().ok_or(errno!(EIO))?.target.clone();
        Ok(SymlinkTarget::Path(target.into_bytes()))
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::fs::buffers::VecOutputBuffer;
    use crate::fs::SymlinkTarget;
    use crate::testing::*;
    use fidl_fuchsia_io as fio;
    use std::collections::{HashMap, HashSet};

    #[::fuchsia::test]
    fn test_read_image() {
        let (kernel, current_task) = create_kernel_and_task();
        let rights = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE;
        let (server, client) = zx::Channel::create();
        fdio::open("/pkg", rights, server).expect("failed to open /pkg");
        let fs = RemoteBundle::new_fs(
            &kernel,
            &fio::DirectorySynchronousProxy::new(client),
            rights,
            "data/test-image",
        )
        .expect("new_fs failed");
        let ns = Namespace::new(fs);
        let root = ns.root();
        let mut context = LookupContext::default().with(SymlinkMode::NoFollow);

        let test_dir =
            root.lookup_child(&current_task, &mut context, b"foo").expect("lookup failed");

        let test_file = test_dir
            .lookup_child(&current_task, &mut context, b"file")
            .expect("lookup failed")
            .open(&current_task, OpenFlags::RDONLY, true)
            .expect("open failed");

        let mut buffer = VecOutputBuffer::new(64);
        assert_eq!(test_file.read(&current_task, &mut buffer).expect("read failed"), 6);
        let buffer: Vec<u8> = buffer.into();
        assert_eq!(&buffer[..6], b"hello\n");

        assert_eq!(
            &test_file.node().get_xattr(&current_task, b"user.a").expect("get_xattr failed"),
            b"apple"
        );
        assert_eq!(
            &test_file.node().get_xattr(&current_task, b"user.b").expect("get_xattr failed"),
            b"ball"
        );
        assert_eq!(
            test_file
                .node()
                .list_xattrs(&current_task)
                .expect("list_xattr failed")
                .into_iter()
                .collect::<HashSet<_>>(),
            [b"user.a".to_vec(), b"user.b".to_vec()].into(),
        );

        {
            let info = test_file.node().info();
            assert_eq!(info.mode, FileMode::from_bits(0o100640));
            assert_eq!(info.uid, 49152); // These values come from the test image generated in
            assert_eq!(info.gid, 24403); // ext4_to_pkg.
        }

        let test_symlink =
            test_dir.lookup_child(&current_task, &mut context, b"symlink").expect("lookup failed");

        if let SymlinkTarget::Path(target) =
            test_symlink.readlink(&current_task).expect("readlink failed")
        {
            assert_eq!(&target, b"file");
        } else {
            panic!("unexpected symlink type");
        }

        let opened_dir =
            test_dir.open(&current_task, OpenFlags::RDONLY, true).expect("open failed");

        struct Sink {
            offset: off_t,
            entries: HashMap<Vec<u8>, (ino_t, DirectoryEntryType)>,
        }

        impl DirentSink for Sink {
            fn add(
                &mut self,
                inode_num: ino_t,
                offset: off_t,
                entry_type: DirectoryEntryType,
                name: &FsStr,
            ) -> Result<(), Errno> {
                assert_eq!(offset, self.offset + 1);
                self.entries.insert(name.to_vec(), (inode_num, entry_type));
                self.offset = offset;
                Ok(())
            }

            fn offset(&self) -> off_t {
                self.offset
            }

            fn actual(&self) -> usize {
                0
            }
        }

        let mut sink = Sink { offset: 0, entries: HashMap::new() };
        opened_dir.readdir(&current_task, &mut sink).expect("readdir failed");

        assert_eq!(
            sink.entries,
            [
                (b"file".to_vec(), (test_file.node().inode_num, DirectoryEntryType::REG)),
                (b"symlink".to_vec(), (test_symlink.entry.node.inode_num, DirectoryEntryType::LNK))
            ]
            .into()
        );
    }
}
