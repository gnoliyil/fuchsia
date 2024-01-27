// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;
use crate::auth::FsCred;
use crate::fs::buffers::{InputBuffer, OutputBuffer};
use crate::fs::cgroup::CgroupDirectoryNode;
use crate::task::*;
use crate::types::*;
use std::sync::Arc;

struct SysFs;
impl FileSystemOps for SysFs {
    fn statfs(&self, _fs: &FileSystem) -> Result<statfs, Errno> {
        Ok(statfs::default(SYSFS_MAGIC))
    }
}

impl SysFs {
    fn new_fs(kernel: &Arc<Kernel>) -> Result<FileSystemHandle, Errno> {
        let fs = FileSystem::new_with_permanent_entries(kernel, SysFs);
        let mut dir = StaticDirectoryBuilder::new(&fs);
        dir.subdir(b"fs", 0o755, |dir| {
            dir.subdir(b"selinux", 0o755, |_| ());
            dir.subdir(b"bpf", 0o755, |_| ());
            dir.node(
                b"cgroup",
                fs.create_node(CgroupDirectoryNode::new(), mode!(IFDIR, 0o755), FsCred::root()),
            );
            dir.subdir(b"fuse", 0o755, |dir| dir.subdir(b"connections", 0o755, |_| ()));
        });
        // TODO(fxb/119437): Create a dynamic directory that depends on registered devices.
        dir.subdir(b"devices", 0o755, |dir| {
            dir.subdir(b"virtual", 0o755, |dir| {
                dir.subdir(b"misc", 0o755, |dir| {
                    dir.entry(
                        b"device-mapper",
                        DeviceDirectory::new(kernel.clone(), DeviceType::DEVICE_MAPPER),
                        mode!(IFDIR, 0o755),
                    );
                })
            })
        });
        dir.build_root();
        Ok(fs)
    }
}

pub fn sys_fs(kern: &Arc<Kernel>) -> &FileSystemHandle {
    kern.sys_fs.get_or_init(|| SysFs::new_fs(kern).expect("failed to construct sysfs!"))
}

struct UEventFile {
    device: DeviceType,
}

impl FileOps for UEventFile {
    fileops_impl_seekable!();

    fn read_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        // TODO(fxb/119437): Retrieve DEVNAME from DeviceRegistry
        let content = format!(
            "MAJOR={}\nMINOR={}\nDEVNAME=mapper/control\n",
            self.device.major(),
            self.device.minor()
        );
        data.write(content[offset..].as_bytes())
    }

    fn write_at(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        offset: usize,
        _data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        if offset != 0 {
            return error!(EINVAL);
        }
        // TODO(fxb/119437): Transmit command to DeviceRegistry
        error!(EINVAL)
    }
}

struct DeviceDirectory {
    _kernel: Arc<Kernel>,
    device: DeviceType,
}

impl DeviceDirectory {
    fn new(kernel: Arc<Kernel>, device: DeviceType) -> Arc<Self> {
        Arc::new(Self { _kernel: kernel, device })
    }
}

impl FsNodeOps for Arc<DeviceDirectory> {
    fs_node_impl_dir_readonly!();

    fn create_file_ops(
        &self,
        _node: &FsNode,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        // TODO(fxb/119437): Add power and subsystem nodes.
        Ok(VecDirectory::new_file(vec![
            VecDirectoryEntry {
                entry_type: DirectoryEntryType::REG,
                name: b"dev".to_vec(),
                inode: None,
            },
            VecDirectoryEntry {
                entry_type: DirectoryEntryType::REG,
                name: b"uevent".to_vec(),
                inode: None,
            },
        ]))
    }

    fn lookup(
        &self,
        node: &FsNode,
        _current_task: &CurrentTask,
        name: &FsStr,
    ) -> Result<Arc<FsNode>, Errno> {
        match name {
            b"dev" => Ok(node.fs().create_node(
                BytesFile::new_node(
                    format!("{}:{}\n", self.device.major(), self.device.minor()).into_bytes(),
                ),
                mode!(IFREG, 0o444),
                FsCred::root(),
            )),
            b"uevent" => {
                let device = self.device;
                Ok(node.fs().create_node(
                    SimpleFileNode::new(move || Ok(UEventFile { device })),
                    mode!(IFREG, 0o644),
                    FsCred::root(),
                ))
            }
            _ => error!(ENOENT),
        }
    }
}
