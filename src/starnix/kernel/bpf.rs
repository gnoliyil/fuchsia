// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Implementation of (e)BPF.
//!
//! BPF stands for Berkeley Packet Filter and is an API introduced in BSD that allows filtering
//! network packets by running little programs in the kernel. eBPF stands for extended BFP and
//! is a Linux extension of BPF that allows hooking BPF programs into many different
//! non-networking-related contexts.

// TODO(https://github.com/rust-lang/rust/issues/39371): remove
#![allow(non_upper_case_globals)]

use crate::{
    auth::FsCred,
    fs::{
        buffers::{InputBuffer, OutputBuffer},
        fileops_impl_nonseekable, fs_node_impl_not_dir, fs_node_impl_xattr_delegate, Anon,
        CacheMode, FdFlags, FdNumber, FileObject, FileOps, FileSystem, FileSystemHandle,
        FileSystemOps, FileSystemOptions, FsNode, FsNodeHandle, FsNodeInfo, FsNodeOps, FsStr,
        FsString, LookupContext, MemoryDirectoryFile, MemoryXattrStorage, NamespaceNode, XattrOp,
    },
    logging::{log_trace, not_implemented},
    mm::{MemoryAccessor, MemoryAccessorExt},
    task::{CurrentTask, Kernel},
};
use lock_sequence::{Locked, Unlocked};
use starnix_lock::{declare_lock_levels, OrderedMutex};
use starnix_syscalls::{SyscallResult, SUCCESS};
use starnix_uapi::{
    as_any::AsAny,
    bpf_attr__bindgen_ty_1, bpf_attr__bindgen_ty_10, bpf_attr__bindgen_ty_12,
    bpf_attr__bindgen_ty_2, bpf_attr__bindgen_ty_4, bpf_attr__bindgen_ty_5, bpf_attr__bindgen_ty_9,
    bpf_cmd, bpf_cmd_BPF_BTF_LOAD, bpf_cmd_BPF_MAP_CREATE, bpf_cmd_BPF_MAP_GET_NEXT_KEY,
    bpf_cmd_BPF_MAP_UPDATE_ELEM, bpf_cmd_BPF_OBJ_GET, bpf_cmd_BPF_OBJ_GET_INFO_BY_FD,
    bpf_cmd_BPF_OBJ_PIN, bpf_cmd_BPF_PROG_ATTACH, bpf_cmd_BPF_PROG_LOAD, bpf_cmd_BPF_PROG_QUERY,
    bpf_map_info, bpf_map_type, bpf_map_type_BPF_MAP_TYPE_DEVMAP,
    bpf_map_type_BPF_MAP_TYPE_DEVMAP_HASH, bpf_prog_info,
    device_type::DeviceType,
    errno, error,
    errors::Errno,
    file_mode::{mode, FileMode},
    open_flags::OpenFlags,
    statfs,
    user_address::{UserAddress, UserCString},
    BPF_FS_MAGIC, BPF_F_RDONLY_PROG, PATH_MAX,
};
use std::{collections::BTreeMap, ops::Bound, sync::Arc};
use zerocopy::{AsBytes, FromBytes};

declare_lock_levels![BpfMapEntries];
use self::lock_levels::BpfMapEntries;

/// The default selinux context to use for each BPF object.
const DEFAULT_BPF_SELINUX_CONTEXT: &FsStr = b"u:object_r:fs_bpf:s0";

trait BpfObject: Send + Sync + AsAny + 'static {}

/// A reference to a BPF object that can be stored in either an FD or an entry in the /sys/fs/bpf
/// filesystem.
#[derive(Clone)]
struct BpfHandle(Arc<dyn BpfObject>);

impl FileOps for BpfHandle {
    fileops_impl_nonseekable!();
    fn read(
        &self,
        _file: &FileObject,
        _current_task: &crate::task::CurrentTask,
        offset: usize,
        _data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        debug_assert!(offset == 0);
        error!(EINVAL) // TODO
    }
    fn write(
        &self,
        _file: &FileObject,
        _current_task: &crate::task::CurrentTask,
        offset: usize,
        _data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        debug_assert!(offset == 0);
        error!(EINVAL) // TODO
    }
}

impl BpfHandle {
    fn new(obj: impl BpfObject) -> Self {
        Self(Arc::new(obj))
    }

    fn downcast<T: BpfObject>(&self) -> Option<&T> {
        (*self.0).as_any().downcast_ref::<T>()
    }
}

/// A BPF Type Format object, often referred to as BTF. See
/// https://www.kernel.org/doc/html/latest/bpf/btf.html
struct BpfTypeFormat {
    #[allow(dead_code)]
    data: Vec<u8>,
}
impl BpfObject for BpfTypeFormat {}

/// A BPF map. This is a hashtable that can be accessed both by BPF programs and userspace.
struct Map {
    map_type: bpf_map_type,
    key_size: u32,
    value_size: u32,
    max_entries: u32,
    flags: u32,

    // TODO(tbodt): Linux actually has 30 different implementations of a BPF map, from hashmap to
    // array to bloom filter. BTreeMap is probably the correct semantics for none of them. This
    // will ultimately need to be a trait object.
    entries: OrderedMutex<BTreeMap<Vec<u8>, Vec<u8>>, BpfMapEntries>,
}
impl BpfObject for Map {}

/// A BPF program. Currently empty because none of the state of a program actually matters to us
/// yet.
struct Program;
impl BpfObject for Program {}

/// Read the arguments for a BPF command. The ABI works like this: If the arguments struct
/// passed is larger than the kernel knows about, the excess must be zeros. Similarly, if the
/// arguments struct is smaller than the kernel knows about, the kernel fills the excess with
/// zero.
fn read_attr<Attr: FromBytes>(
    current_task: &CurrentTask,
    attr_addr: UserAddress,
    attr_size: u32,
) -> Result<Attr, Errno> {
    let attr_size = attr_size as usize;
    let sizeof_attr = std::mem::size_of::<Attr>();

    // Verify that the extra is all zeros.
    if attr_size > sizeof_attr {
        let tail =
            current_task.read_memory_to_vec(attr_addr + sizeof_attr, attr_size - sizeof_attr)?;
        for byte in tail {
            if byte != 0 {
                return error!(E2BIG);
            }
        }
    }

    // If the struct passed is smaller than our definition of the struct, let whatever is not
    // passed be zero.
    let mut attr = Attr::new_zeroed();
    // SAFETY: attr is FromBytes, meaning it is safe to write any bit pattern to its storage. (The
    // unsafe slice construction is necessary because it's not necessarily safe to read from its
    // storage directly.)
    current_task.read_memory_to_slice(attr_addr, unsafe {
        std::slice::from_raw_parts_mut(&mut attr as *mut Attr as *mut u8, sizeof_attr)
    })?;

    Ok(attr)
}

fn install_bpf_fd(current_task: &CurrentTask, obj: impl BpfObject) -> Result<SyscallResult, Errno> {
    install_bpf_handle_fd(current_task, BpfHandle::new(obj))
}

fn install_bpf_handle_fd(
    current_task: &CurrentTask,
    handle: BpfHandle,
) -> Result<SyscallResult, Errno> {
    // All BPF FDs have the CLOEXEC flag turned on by default.
    let file = Anon::new_file(current_task, Box::new(handle), OpenFlags::CLOEXEC);
    Ok(current_task.add_file(file, FdFlags::CLOEXEC)?.into())
}

fn get_bpf_fd(current_task: &CurrentTask, fd: u32) -> Result<BpfHandle, Errno> {
    Ok(current_task
        .files
        .get(FdNumber::from_raw(fd as i32))?
        .downcast_file::<BpfHandle>()
        .ok_or_else(|| errno!(EBADF))?
        .clone())
}

// TODO(b/278731253): Returns custom selinux context for everything under /sys/fs/bpf/net_shared/*.
// Otherwise, returns default selinux context for BPF objects.
fn get_selinux_context(path: &FsStr) -> FsString {
    if String::from_utf8_lossy(path).contains("net_shared") {
        b"u:object_r:fs_bpf_net_shared:s0".to_vec()
    } else {
        DEFAULT_BPF_SELINUX_CONTEXT.to_vec()
    }
}

pub fn sys_bpf(
    locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    cmd: bpf_cmd,
    attr_addr: UserAddress,
    attr_size: u32,
) -> Result<SyscallResult, Errno> {
    // TODO(security): Implement the actual security semantics of BPF. This is commented out
    // because Android calls bpf from unprivileged processes.
    // if !current_task.creds().has_capability(CAP_SYS_ADMIN) {
    //     return error!(EPERM);
    // }

    // The best available documentation on the various BPF commands is at
    // https://www.kernel.org/doc/html/latest/userspace-api/ebpf/syscall.html.
    // Comments on commands are copied from there.

    match cmd {
        // Create a map and return a file descriptor that refers to the map.
        bpf_cmd_BPF_MAP_CREATE => {
            let map_attr: bpf_attr__bindgen_ty_1 = read_attr(current_task, attr_addr, attr_size)?;
            log_trace!("BPF_MAP_CREATE {:?}", map_attr);
            let mut map = Map {
                map_type: map_attr.map_type,
                key_size: map_attr.key_size,
                value_size: map_attr.value_size,
                max_entries: map_attr.max_entries,
                flags: map_attr.map_flags,
                entries: Default::default(),
            };

            // To quote
            // https://cs.android.com/android/platform/superproject/+/master:system/bpf/libbpf_android/Loader.cpp;l=670;drc=28e295395471b33e662b7116378d15f1e88f0864
            // "DEVMAPs are readonly from the bpf program side's point of view, as such the kernel
            // in kernel/bpf/devmap.c dev_map_init_map() will set the flag"
            if map.map_type == bpf_map_type_BPF_MAP_TYPE_DEVMAP
                || map.map_type == bpf_map_type_BPF_MAP_TYPE_DEVMAP_HASH
            {
                map.flags |= BPF_F_RDONLY_PROG;
            }

            install_bpf_fd(current_task, map)
        }

        // Create or update an element (key/value pair) in a specified map.
        bpf_cmd_BPF_MAP_UPDATE_ELEM => {
            let elem_attr: bpf_attr__bindgen_ty_2 = read_attr(current_task, attr_addr, attr_size)?;
            log_trace!("BPF_MAP_UPDATE_ELEM");
            let map = get_bpf_fd(current_task, elem_attr.map_fd)?;
            let map = map.downcast::<Map>().ok_or_else(|| errno!(EINVAL))?;

            let key = current_task
                .mm
                .read_memory_to_vec(UserAddress::from(elem_attr.key), map.key_size as usize)?;
            // SAFETY: this union object was created with FromBytes so it's safe to access any
            // variant because all variants must be valid with all bit patterns.
            let value_addr = unsafe { elem_attr.__bindgen_anon_1.value };
            let value = current_task
                .mm
                .read_memory_to_vec(UserAddress::from(value_addr), map.value_size as usize)?;

            map.entries.lock(locked).insert(key, value);
            Ok(SUCCESS)
        }

        // Look up an element by key in a specified map and return the key of the next element. Can
        // be used to iterate over all elements in the map.
        bpf_cmd_BPF_MAP_GET_NEXT_KEY => {
            let elem_attr: bpf_attr__bindgen_ty_2 = read_attr(current_task, attr_addr, attr_size)?;
            log_trace!("BPF_MAP_GET_NEXT_KEY");
            let map = get_bpf_fd(current_task, elem_attr.map_fd)?;
            let map = map.downcast::<Map>().ok_or_else(|| errno!(EINVAL))?;
            let key = if elem_attr.key != 0 {
                let key = current_task
                    .mm
                    .read_memory_to_vec(UserAddress::from(elem_attr.key), map.key_size as usize)?;
                Some(key)
            } else {
                None
            };

            let entries = map.entries.lock(locked);
            let next_entry = match key {
                Some(key) if entries.contains_key(&key) => {
                    entries.range((Bound::Excluded(key), Bound::Unbounded)).next()
                }
                _ => entries.iter().next(),
            };
            let (next_key, _next_value) = next_entry.ok_or_else(|| errno!(ENOENT))?;
            // SAFETY: this union object was created with FromBytes so it's safe to access any
            // variant (right?)
            let next_key_addr = unsafe { elem_attr.__bindgen_anon_1.next_key };
            current_task.write_memory(UserAddress::from(next_key_addr), next_key)?;
            Ok(SUCCESS)
        }

        // Verify and load an eBPF program, returning a new file descriptor associated with the
        // program.
        bpf_cmd_BPF_PROG_LOAD => {
            let _prog_attr: bpf_attr__bindgen_ty_4 = read_attr(current_task, attr_addr, attr_size)?;
            log_trace!("BPF_PROG_LOAD");
            // Just pretend to load the program. We certainly can't execute it.
            install_bpf_fd(current_task, Program)
        }

        // Attach an eBPF program to a target_fd at the specified attach_type hook.
        bpf_cmd_BPF_PROG_ATTACH => {
            log_trace!("BPF_PROG_ATTACH");
            not_implemented!("Bpf::BPF_PROG_ATTACH is stubbed");
            Ok(SUCCESS)
        }

        // Obtain information about eBPF programs associated with the specified attach_type hook.
        bpf_cmd_BPF_PROG_QUERY => {
            let mut prog_attr: bpf_attr__bindgen_ty_10 =
                read_attr(current_task, attr_addr, attr_size)?;
            log_trace!("BPF_PROG_QUERY");
            not_implemented!("Bpf::BPF_PROG_QUERY is stubbed");
            current_task.write_memory(UserAddress::from(prog_attr.prog_ids), 1.as_bytes())?;
            prog_attr.prog_cnt = std::mem::size_of::<u64>() as u32;
            current_task.write_memory(attr_addr, prog_attr.as_bytes())?;
            Ok(SUCCESS)
        }

        // Pin an eBPF program or map referred by the specified bpf_fd to the provided pathname on
        // the filesystem.
        bpf_cmd_BPF_OBJ_PIN => {
            let pin_attr: bpf_attr__bindgen_ty_5 = read_attr(current_task, attr_addr, attr_size)?;
            log_trace!("BPF_OBJ_PIN {:?}", pin_attr);
            let object = get_bpf_fd(current_task, pin_attr.bpf_fd)?;
            let path_addr = UserCString::new(UserAddress::from(pin_attr.pathname));
            let pathname = current_task.read_c_string_to_vec(path_addr, PATH_MAX as usize)?;
            let (parent, basename) = current_task.lookup_parent_at(
                &mut LookupContext::default(),
                FdNumber::AT_FDCWD,
                &pathname,
            )?;
            let bpf_dir =
                parent.entry.node.downcast_ops::<BpfFsDir>().ok_or_else(|| errno!(EINVAL))?;
            let selinux_context = get_selinux_context(&pathname);
            bpf_dir.register_pin(current_task, &parent, basename, object, &selinux_context)?;
            Ok(SUCCESS)
        }

        // Open a file descriptor for the eBPF object pinned to the specified pathname.
        bpf_cmd_BPF_OBJ_GET => {
            let path_attr: bpf_attr__bindgen_ty_5 = read_attr(current_task, attr_addr, attr_size)?;
            log_trace!("BPF_OBJ_GET {:?}", path_attr);
            let path_addr = UserCString::new(UserAddress::from(path_attr.pathname));
            let pathname = current_task.read_c_string_to_vec(path_addr, PATH_MAX as usize)?;
            let node = current_task.lookup_path_from_root(&pathname)?;
            // TODO(tbodt): This might be the wrong error code, write a test program to find out
            let node =
                node.entry.node.downcast_ops::<BpfFsObject>().ok_or_else(|| errno!(EINVAL))?;
            install_bpf_handle_fd(current_task, node.handle.clone())
        }

        // Obtain information about the eBPF object corresponding to bpf_fd.
        bpf_cmd_BPF_OBJ_GET_INFO_BY_FD => {
            let mut get_info_attr: bpf_attr__bindgen_ty_9 =
                read_attr(current_task, attr_addr, attr_size)?;
            log_trace!("BPF_OBJ_GET_INFO_BY_FD {:?}", get_info_attr);
            let fd = get_bpf_fd(current_task, get_info_attr.bpf_fd)?;

            let mut info = if let Some(map) = fd.downcast::<Map>() {
                bpf_map_info {
                    type_: map.map_type,
                    id: 0, // not used by android as far as I can tell
                    key_size: map.key_size,
                    value_size: map.value_size,
                    max_entries: map.max_entries,
                    map_flags: map.flags,
                    ..Default::default()
                }
                .as_bytes()
                .to_owned()
            } else if let Some(_prog) = fd.downcast::<Program>() {
                #[allow(unknown_lints, clippy::unnecessary_struct_initialization)]
                bpf_prog_info {
                    // Doesn't matter yet
                    ..Default::default()
                }
                .as_bytes()
                .to_owned()
            } else {
                return error!(EINVAL);
            };

            // If info_len is larger than info, write out the full length of info and write the
            // smaller size into info_len. If info_len is smaller, truncate info.
            // TODO(tbodt): This is just a guess for the behavior. Works with BpfSyscallWrappers.h,
            // but could be wrong.
            info.truncate(get_info_attr.info_len as usize);
            get_info_attr.info_len = info.len() as u32;
            current_task.write_memory(UserAddress::from(get_info_attr.info), &info)?;
            current_task.write_memory(attr_addr, get_info_attr.as_bytes())?;
            Ok(SUCCESS)
        }

        // Verify and load BPF Type Format (BTF) metadata into the kernel, returning a new file
        // descriptor associated with the metadata. BTF is described in more detail at
        // https://www.kernel.org/doc/html/latest/bpf/btf.html.
        bpf_cmd_BPF_BTF_LOAD => {
            let btf_attr: bpf_attr__bindgen_ty_12 = read_attr(current_task, attr_addr, attr_size)?;
            log_trace!("BPF_BTF_LOAD {:?}", btf_attr);
            let data = current_task
                .mm
                .read_memory_to_vec(UserAddress::from(btf_attr.btf), btf_attr.btf_size as usize)?;
            install_bpf_fd(current_task, BpfTypeFormat { data })
        }

        _ => {
            not_implemented!("bpf command {}", cmd);
            error!(EINVAL)
        }
    }
}

pub struct BpfFs;
impl BpfFs {
    pub fn new_fs(
        kernel: &Arc<Kernel>,
        options: FileSystemOptions,
    ) -> Result<FileSystemHandle, Errno> {
        let fs = FileSystem::new(kernel, CacheMode::Permanent, BpfFs, options);
        let node =
            FsNode::new_root_with_properties(BpfFsDir::new(DEFAULT_BPF_SELINUX_CONTEXT), |info| {
                info.mode |= FileMode::ISVTX;
            });
        fs.set_root_node(node);
        Ok(fs)
    }
}

impl FileSystemOps for BpfFs {
    fn statfs(&self, _fs: &FileSystem, _current_task: &CurrentTask) -> Result<statfs, Errno> {
        Ok(statfs::default(BPF_FS_MAGIC))
    }
    fn name(&self) -> &'static FsStr {
        b"bpf"
    }

    fn rename(
        &self,
        _fs: &FileSystem,
        _current_task: &CurrentTask,
        _old_parent: &FsNodeHandle,
        _old_name: &FsStr,
        _new_parent: &FsNodeHandle,
        _new_name: &FsStr,
        _renamed: &FsNodeHandle,
        _replaced: Option<&FsNodeHandle>,
    ) -> Result<(), Errno> {
        Ok(())
    }
}

struct BpfFsDir {
    xattrs: MemoryXattrStorage,
}

impl BpfFsDir {
    fn new(selinux_context: &FsStr) -> Self {
        let xattrs = MemoryXattrStorage::default();
        xattrs
            .set_xattr(b"security.selinux", selinux_context, XattrOp::Create)
            .expect("Failed to set selinux context.");
        Self { xattrs }
    }

    fn register_pin(
        &self,
        current_task: &CurrentTask,
        node: &NamespaceNode,
        name: &FsStr,
        object: BpfHandle,
        selinux_context: &FsStr,
    ) -> Result<(), Errno> {
        node.entry.create_entry(current_task, &node.mount, name, |dir, _mount, _name| {
            Ok(dir.fs().create_node(
                current_task,
                BpfFsObject::new(object, &selinux_context),
                FsNodeInfo::new_factory(mode!(IFREG, 0o600), FsCred::root()),
            ))
        })?;
        Ok(())
    }
}

impl FsNodeOps for BpfFsDir {
    fs_node_impl_xattr_delegate!(self, self.xattrs);

    fn create_file_ops(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        Ok(Box::new(MemoryDirectoryFile::new()))
    }

    fn mkdir(
        &self,
        node: &FsNode,
        current_task: &CurrentTask,
        name: &FsStr,
        mode: FileMode,
        owner: FsCred,
    ) -> Result<FsNodeHandle, Errno> {
        let selinux_context = get_selinux_context(name);
        Ok(node.fs().create_node(
            current_task,
            BpfFsDir::new(&selinux_context),
            FsNodeInfo::new_factory(mode | FileMode::ISVTX, owner),
        ))
    }

    fn mknod(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
        _name: &FsStr,
        _mode: FileMode,
        _dev: DeviceType,
        _owner: FsCred,
    ) -> Result<FsNodeHandle, Errno> {
        error!(EPERM)
    }

    fn create_symlink(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
        _name: &FsStr,
        _target: &FsStr,
        _owner: FsCred,
    ) -> Result<FsNodeHandle, Errno> {
        error!(EPERM)
    }

    fn link(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
        _name: &FsStr,
        _child: &FsNodeHandle,
    ) -> Result<(), Errno> {
        Ok(())
    }

    fn unlink(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
        _name: &FsStr,
        _child: &FsNodeHandle,
    ) -> Result<(), Errno> {
        Ok(())
    }
}

struct BpfFsObject {
    handle: BpfHandle,
    xattrs: MemoryXattrStorage,
}

impl BpfFsObject {
    fn new(handle: BpfHandle, selinux_context: &FsStr) -> Self {
        let xattrs = MemoryXattrStorage::default();
        xattrs
            .set_xattr(b"security.selinux", selinux_context, XattrOp::Create)
            .expect("Failed to set selinux context.");
        Self { handle, xattrs }
    }
}

impl FsNodeOps for BpfFsObject {
    fs_node_impl_not_dir!();
    fs_node_impl_xattr_delegate!(self, self.xattrs);

    fn create_file_ops(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        error!(EIO)
    }
}
