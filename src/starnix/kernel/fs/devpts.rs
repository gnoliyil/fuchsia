// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    auth::FsCred,
    device::{
        terminal::{TTYState, Terminal},
        DeviceMode, DeviceOps,
    },
    fs::{
        buffers::{InputBuffer, OutputBuffer},
        devtmpfs::{devtmpfs_create_symlink, devtmpfs_mkdir, devtmpfs_remove_child},
        fileops_impl_nonseekable, fs_node_impl_dir_readonly,
        kobject::KObjectDeviceAttribute,
        CacheMode, DirEntryHandle, DirectoryEntryType, FdEvents, FileHandle, FileObject, FileOps,
        FileSystem, FileSystemHandle, FileSystemOps, FileSystemOptions, FsNode, FsNodeHandle,
        FsNodeInfo, FsNodeOps, FsStr, SpecialNode, VecDirectory, VecDirectoryEntry,
    },
    logging::not_implemented,
    mm::MemoryAccessorExt,
    syscalls::{SyscallArg, SyscallResult, SUCCESS},
    task::{CurrentTask, EventHandler, Kernel, WaitCanceler, Waiter},
};
use starnix_uapi::{
    device_type::{DeviceType, TTY_ALT_MAJOR},
    errno, error,
    errors::Errno,
    file_mode::mode,
    ino_t,
    open_flags::OpenFlags,
    pid_t,
    signals::SIGWINCH,
    statfs, uapi,
    user_address::{UserAddress, UserRef},
    DEVPTS_SUPER_MAGIC, FIOASYNC, FIOCLEX, FIONBIO, FIONCLEX, FIONREAD, FIOQSIZE, TCFLSH, TCGETA,
    TCGETS, TCGETX, TCSBRK, TCSBRKP, TCSETA, TCSETAF, TCSETAW, TCSETS, TCSETSF, TCSETSW, TCSETX,
    TCSETXF, TCSETXW, TCXONC, TIOCCBRK, TIOCCONS, TIOCEXCL, TIOCGETD, TIOCGICOUNT, TIOCGLCKTRMIOS,
    TIOCGPGRP, TIOCGPTLCK, TIOCGPTN, TIOCGRS485, TIOCGSERIAL, TIOCGSID, TIOCGSOFTCAR, TIOCGWINSZ,
    TIOCLINUX, TIOCMBIC, TIOCMBIS, TIOCMGET, TIOCMIWAIT, TIOCMSET, TIOCNOTTY, TIOCNXCL, TIOCOUTQ,
    TIOCPKT, TIOCSBRK, TIOCSCTTY, TIOCSERCONFIG, TIOCSERGETLSR, TIOCSERGETMULTI, TIOCSERGSTRUCT,
    TIOCSERGWILD, TIOCSERSETMULTI, TIOCSERSWILD, TIOCSETD, TIOCSLCKTRMIOS, TIOCSPGRP, TIOCSPTLCK,
    TIOCSRS485, TIOCSSERIAL, TIOCSSOFTCAR, TIOCSTI, TIOCSWINSZ, TIOCVHANGUP,
};
use std::sync::{Arc, Weak};

// See https://www.kernel.org/doc/Documentation/admin-guide/devices.txt
const DEVPTS_FIRST_MAJOR: u32 = 136;
const DEVPTS_MAJOR_COUNT: u32 = 4;
// The device identifier is encoded through the major and minor device identifier of the
// device. Each major identifier can contain 256 pts replicas.
pub const DEVPTS_COUNT: u32 = DEVPTS_MAJOR_COUNT * 256;
// The block size of the node in the devpts file system. Value has been taken from
// https://github.com/google/gvisor/blob/master/test/syscalls/linux/pty.cc
const BLOCK_SIZE: usize = 1024;

// The node identifier of the different node in the devpts filesystem.
const ROOT_NODE_ID: ino_t = 1;
const PTMX_NODE_ID: ino_t = 2;
const FIRST_PTS_NODE_ID: ino_t = 3;

pub fn dev_pts_fs(kernel: &Arc<Kernel>, options: FileSystemOptions) -> &FileSystemHandle {
    kernel.dev_pts_fs.get_or_init(|| init_devpts(kernel, options))
}

/// Creates a terminal and returns the main pty and an associated replica pts.
///
/// This function assumes that `/dev/ptmx` is the `DevPtmxFile` and that devpts
/// is mounted at `/dev/pts`. These assumptions are necessary so that the
/// `FileHandle` objects returned have appropriate `NamespaceNode` objects.
pub fn create_main_and_replica(
    current_task: &CurrentTask,
    window_size: uapi::winsize,
) -> Result<(FileHandle, FileHandle), Errno> {
    let pty_file = current_task.open_file(b"/dev/ptmx", OpenFlags::RDWR)?;
    let pty = pty_file.downcast_file::<DevPtmxFile>().ok_or_else(|| errno!(ENOTTY))?;
    {
        let mut terminal = pty.terminal.write();
        terminal.locked = false;
        terminal.window_size = window_size;
    }
    let pts_path = format!("/dev/pts/{}", pty.terminal.id);
    let pts_file = current_task.open_file(pts_path.as_bytes(), OpenFlags::RDWR)?;
    Ok((pty_file, pts_file))
}

fn init_devpts(kernel: &Arc<Kernel>, options: FileSystemOptions) -> FileSystemHandle {
    let state = Arc::new(TTYState::new());
    let device = DevPtsDevice::new(state.clone());

    // Register /dev/pts/X device type.
    for n in 0..DEVPTS_MAJOR_COUNT {
        kernel
            .device_registry
            .register_chrdev_major(DEVPTS_FIRST_MAJOR + n, device.clone())
            .expect("Registering pts device");
    }
    // Register tty and ptmx device types.
    kernel.device_registry.register_chrdev_major(TTY_ALT_MAJOR, device).unwrap();

    let fs = FileSystem::new(kernel, CacheMode::Uncached, DevPtsFs, options);
    let mut root = FsNode::new_root_with_properties(DevPtsRootDir { state }, |info| {
        info.ino = ROOT_NODE_ID;
    });
    root.node_id = ROOT_NODE_ID;
    fs.set_root_node(root);
    fs
}

pub fn tty_device_init(kernel: &Arc<Kernel>) {
    let tty_class = kernel.device_registry.add_class(b"tty", kernel.device_registry.virtual_bus());
    let tty = KObjectDeviceAttribute::new(
        tty_class.clone(),
        b"tty",
        b"tty",
        DeviceType::TTY,
        DeviceMode::Char,
    );
    let ptmx = KObjectDeviceAttribute::new(
        tty_class,
        b"ptmx",
        b"ptmx",
        DeviceType::PTMX,
        DeviceMode::Char,
    );
    kernel.add_device(tty);
    kernel.add_device(ptmx);

    devtmpfs_mkdir(kernel, b"pts").unwrap();

    // Create a symlink from /dev/ptmx to /dev/pts/ptmx for pseudo-tty subsystem.
    devtmpfs_remove_child(kernel, b"ptmx");
    devtmpfs_create_symlink(kernel, b"ptmx", b"pts/ptmx").unwrap();
}

struct DevPtsFs;
impl FileSystemOps for DevPtsFs {
    fn statfs(&self, _fs: &FileSystem, _current_task: &CurrentTask) -> Result<statfs, Errno> {
        Ok(statfs::default(DEVPTS_SUPER_MAGIC))
    }
    fn name(&self) -> &'static FsStr {
        b"devpts"
    }

    fn generate_node_ids(&self) -> bool {
        false
    }
}

// Construct the DeviceType associated with the given pts replicas.
pub fn get_device_type_for_pts(id: u32) -> DeviceType {
    DeviceType::new(DEVPTS_FIRST_MAJOR + id / 256, id % 256)
}

struct DevPtsRootDir {
    state: Arc<TTYState>,
}

impl FsNodeOps for DevPtsRootDir {
    fs_node_impl_dir_readonly!();

    fn create_file_ops(
        &self,
        _node: &FsNode,
        _current_task: &CurrentTask,
        _flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        let mut result = vec![];
        result.push(VecDirectoryEntry {
            entry_type: DirectoryEntryType::CHR,
            name: b"ptmx".to_vec(),
            inode: Some(PTMX_NODE_ID),
        });
        for (id, terminal) in self.state.terminals.read().iter() {
            if let Some(terminal) = terminal.upgrade() {
                if !terminal.read().is_main_closed() {
                    result.push(VecDirectoryEntry {
                        entry_type: DirectoryEntryType::CHR,
                        name: format!("{id}").as_bytes().to_vec(),
                        inode: Some((*id as ino_t) + FIRST_PTS_NODE_ID),
                    });
                }
            }
        }
        Ok(VecDirectory::new_file(result))
    }

    fn lookup(
        &self,
        node: &FsNode,
        current_task: &CurrentTask,
        name: &FsStr,
    ) -> Result<FsNodeHandle, Errno> {
        let name = std::str::from_utf8(name).map_err(|_| errno!(ENOENT))?;
        if name == "ptmx" {
            let mut info = FsNodeInfo::new(PTMX_NODE_ID, mode!(IFCHR, 0o666), FsCred::root());
            info.rdev = DeviceType::PTMX;
            info.blksize = BLOCK_SIZE;
            let node = node.fs().create_node_with_id(current_task, SpecialNode, info.ino, info);
            return Ok(node);
        }
        if let Ok(id) = name.parse::<u32>() {
            let terminal = self.state.terminals.read().get(&id).and_then(Weak::upgrade);
            if let Some(terminal) = terminal {
                if !terminal.read().is_main_closed() {
                    let mut info = FsNodeInfo::new(
                        (id as ino_t) + FIRST_PTS_NODE_ID,
                        mode!(IFCHR, 0o620),
                        terminal.fscred.clone(),
                    );
                    info.rdev = get_device_type_for_pts(id);
                    info.blksize = BLOCK_SIZE;
                    // TODO(qsr): set gid to the tty group
                    info.gid = 0;
                    let node =
                        node.fs().create_node_with_id(current_task, SpecialNode, info.ino, info);
                    return Ok(node);
                }
            }
        }
        error!(ENOENT)
    }
}

struct DevPtsDevice {
    state: Arc<TTYState>,
}

impl DevPtsDevice {
    pub fn new(state: Arc<TTYState>) -> Arc<Self> {
        Arc::new(Self { state })
    }
}

impl DeviceOps for Arc<DevPtsDevice> {
    fn open(
        &self,
        current_task: &CurrentTask,
        id: DeviceType,
        _node: &FsNode,
        flags: OpenFlags,
    ) -> Result<Box<dyn FileOps>, Errno> {
        match id {
            // /dev/ptmx
            DeviceType::PTMX => {
                let terminal = self.state.get_next_terminal(current_task)?;
                let dev_pts_root =
                    dev_pts_fs(current_task.kernel(), Default::default()).root().clone();

                Ok(Box::new(DevPtmxFile::new(dev_pts_root, terminal)))
            }
            // /dev/tty
            DeviceType::TTY => {
                let controlling_terminal = current_task
                    .thread_group
                    .read()
                    .process_group
                    .session
                    .read()
                    .controlling_terminal
                    .clone();
                if let Some(controlling_terminal) = controlling_terminal {
                    if controlling_terminal.is_main {
                        let dev_pts_root =
                            dev_pts_fs(current_task.kernel(), Default::default()).root().clone();
                        Ok(Box::new(DevPtmxFile::new(dev_pts_root, controlling_terminal.terminal)))
                    } else {
                        Ok(Box::new(DevPtsFile::new(controlling_terminal.terminal)))
                    }
                } else {
                    error!(ENXIO)
                }
            }
            _ if id.major() < DEVPTS_FIRST_MAJOR
                || id.major() >= DEVPTS_FIRST_MAJOR + DEVPTS_MAJOR_COUNT =>
            {
                error!(ENODEV)
            }
            // /dev/pts/??
            _ => {
                let pts_id = (id.major() - DEVPTS_FIRST_MAJOR) * 256 + id.minor();
                let terminal = self
                    .state
                    .terminals
                    .read()
                    .get(&pts_id)
                    .and_then(Weak::upgrade)
                    .ok_or_else(|| errno!(EIO))?;
                if terminal.read().locked {
                    return error!(EIO);
                }
                if !flags.contains(OpenFlags::NOCTTY) {
                    // Opening a replica sets the process' controlling TTY when possible. An error indicates it cannot
                    // be set, and is ignored silently.
                    let _ = current_task.thread_group.set_controlling_terminal(
                        current_task,
                        &terminal,
                        false, /* is_main */
                        false, /* steal */
                        flags.can_read(),
                    );
                }
                Ok(Box::new(DevPtsFile::new(terminal)))
            }
        }
    }
}

struct DevPtmxFile {
    dev_pts_root: DirEntryHandle,
    terminal: Arc<Terminal>,
}

impl DevPtmxFile {
    pub fn new(dev_pts_root: DirEntryHandle, terminal: Arc<Terminal>) -> Self {
        terminal.main_open();
        Self { dev_pts_root, terminal }
    }
}

impl FileOps for DevPtmxFile {
    fileops_impl_nonseekable!();

    fn close(&self, _file: &FileObject) {
        self.terminal.main_close();
        self.dev_pts_root.remove_child(format!("{}", self.terminal.id).as_bytes());
    }

    fn read(
        &self,
        file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        debug_assert!(offset == 0);
        file.blocking_op(current_task, FdEvents::POLLIN | FdEvents::POLLHUP, None, || {
            self.terminal.main_read(current_task, data)
        })
    }

    fn write(
        &self,
        file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        debug_assert!(offset == 0);
        file.blocking_op(current_task, FdEvents::POLLOUT | FdEvents::POLLHUP, None, || {
            self.terminal.main_write(current_task, data)
        })
    }

    fn wait_async(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        waiter: &Waiter,
        events: FdEvents,
        handler: EventHandler,
    ) -> Option<WaitCanceler> {
        Some(self.terminal.main_wait_async(waiter, events, handler))
    }

    fn query_events(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
    ) -> Result<FdEvents, Errno> {
        Ok(self.terminal.main_query_events())
    }

    fn ioctl(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        request: u32,
        arg: SyscallArg,
    ) -> Result<SyscallResult, Errno> {
        let user_addr = UserAddress::from(arg);
        match request {
            TIOCGPTN => {
                // Get the therminal id.
                let value: u32 = self.terminal.id;
                current_task.write_object(UserRef::<u32>::new(user_addr), &value)?;
                Ok(SUCCESS)
            }
            TIOCGPTLCK => {
                // Get the lock status.
                let value = i32::from(self.terminal.read().locked);
                current_task.write_object(UserRef::<i32>::new(user_addr), &value)?;
                Ok(SUCCESS)
            }
            TIOCSPTLCK => {
                // Lock/Unlock the terminal.
                let value = current_task.read_object(UserRef::<i32>::new(user_addr))?;
                self.terminal.write().locked = value != 0;
                Ok(SUCCESS)
            }
            _ => shared_ioctl(&self.terminal, true, _file, current_task, request, arg),
        }
    }
}

struct DevPtsFile {
    terminal: Arc<Terminal>,
}

impl DevPtsFile {
    pub fn new(terminal: Arc<Terminal>) -> Self {
        terminal.replica_open();
        Self { terminal }
    }
}

impl FileOps for DevPtsFile {
    fileops_impl_nonseekable!();

    fn close(&self, _file: &FileObject) {
        self.terminal.replica_close();
    }

    fn read(
        &self,
        file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        debug_assert!(offset == 0);
        file.blocking_op(current_task, FdEvents::POLLIN | FdEvents::POLLHUP, None, || {
            self.terminal.replica_read(current_task, data)
        })
    }

    fn write(
        &self,
        file: &FileObject,
        current_task: &CurrentTask,
        offset: usize,
        data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        debug_assert!(offset == 0);
        file.blocking_op(current_task, FdEvents::POLLOUT | FdEvents::POLLHUP, None, || {
            self.terminal.replica_write(current_task, data)
        })
    }

    fn wait_async(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        waiter: &Waiter,
        events: FdEvents,
        handler: EventHandler,
    ) -> Option<WaitCanceler> {
        Some(self.terminal.replica_wait_async(waiter, events, handler))
    }

    fn query_events(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
    ) -> Result<FdEvents, Errno> {
        Ok(self.terminal.replica_query_events())
    }

    fn ioctl(
        &self,
        file: &FileObject,
        current_task: &CurrentTask,
        request: u32,
        arg: SyscallArg,
    ) -> Result<SyscallResult, Errno> {
        shared_ioctl(&self.terminal, false, file, current_task, request, arg)
    }
}

/// The ioctl behaviour common to main and replica terminal file descriptors.
fn shared_ioctl(
    terminal: &Arc<Terminal>,
    is_main: bool,
    file: &FileObject,
    current_task: &CurrentTask,
    request: u32,
    arg: SyscallArg,
) -> Result<SyscallResult, Errno> {
    let user_addr = UserAddress::from(arg);
    match request {
        FIONREAD => {
            // Get the main terminal available bytes for reading.
            let value = terminal.read().get_available_read_size(is_main) as u32;
            current_task.write_object(UserRef::<u32>::new(user_addr), &value)?;
            Ok(SUCCESS)
        }
        TIOCSCTTY => {
            // Make the given terminal the controlling terminal of the calling process.
            let steal = bool::from(arg);
            current_task.thread_group.set_controlling_terminal(
                current_task,
                terminal,
                is_main,
                steal,
                file.can_read(),
            )?;
            Ok(SUCCESS)
        }
        TIOCNOTTY => {
            // Release the controlling terminal.
            current_task.thread_group.release_controlling_terminal(
                current_task,
                terminal,
                is_main,
            )?;
            Ok(SUCCESS)
        }
        TIOCGPGRP => {
            // Get the foreground process group.
            let pgid = current_task.thread_group.get_foreground_process_group(terminal, is_main)?;
            current_task.write_object(UserRef::<pid_t>::new(user_addr), &pgid)?;
            Ok(SUCCESS)
        }
        TIOCSPGRP => {
            // Set the foreground process group.
            let pgid = current_task.read_object(UserRef::<pid_t>::new(user_addr))?;
            current_task.thread_group.set_foreground_process_group(
                current_task,
                terminal,
                is_main,
                pgid,
            )?;
            Ok(SUCCESS)
        }
        TIOCGWINSZ => {
            // Get the window size
            current_task.write_object(
                UserRef::<uapi::winsize>::new(user_addr),
                &terminal.read().window_size,
            )?;
            Ok(SUCCESS)
        }
        TIOCSWINSZ => {
            // Set the window size
            terminal.write().window_size =
                current_task.read_object(UserRef::<uapi::winsize>::new(user_addr))?;

            // Send a SIGWINCH signal to the foreground process group.
            let foreground_process_group = terminal
                .read()
                .get_controlling_session(is_main)
                .as_ref()
                .and_then(|cs| cs.foregound_process_group.upgrade());
            if let Some(process_group) = foreground_process_group {
                process_group.send_signals(&[SIGWINCH]);
            }
            Ok(SUCCESS)
        }
        TCGETS => {
            // N.B. TCGETS on the main terminal actually returns the configuration of the replica
            // end.
            current_task.write_object(
                UserRef::<uapi::termios>::new(user_addr),
                terminal.read().termios(),
            )?;
            Ok(SUCCESS)
        }
        TCSETS => {
            // N.B. TCSETS on the main terminal actually affects the configuration of the replica
            // end.
            let termios = current_task.read_object(UserRef::<uapi::termios>::new(user_addr))?;
            terminal.set_termios(termios);
            Ok(SUCCESS)
        }
        TCSETSF => {
            // This should drain the output queue and discard the pending input first.
            let termios = current_task.read_object(UserRef::<uapi::termios>::new(user_addr))?;
            terminal.set_termios(termios);
            Ok(SUCCESS)
        }
        TCSETSW => {
            // TODO(qsr): This should drain the output queue first.
            let termios = current_task.read_object(UserRef::<uapi::termios>::new(user_addr))?;
            terminal.set_termios(termios);
            Ok(SUCCESS)
        }

        TIOCSETD => {
            not_implemented!(
                "{}: setting line discipline not implemented",
                if is_main { "ptmx" } else { "pts" }
            );
            error!(EINVAL)
        }

        TCGETA | TCSETA | TCSETAW | TCSETAF | TCSBRK | TCXONC | TCFLSH | TIOCEXCL | TIOCNXCL
        | TIOCOUTQ | TIOCSTI | TIOCMGET | TIOCMBIS | TIOCMBIC | TIOCMSET | TIOCGSOFTCAR
        | TIOCSSOFTCAR | TIOCLINUX | TIOCCONS | TIOCGSERIAL | TIOCSSERIAL | TIOCPKT | FIONBIO
        | TIOCGETD | TCSBRKP | TIOCSBRK | TIOCCBRK | TIOCGSID | TIOCGRS485 | TIOCSRS485
        | TCGETX | TCSETX | TCSETXF | TCSETXW | TIOCVHANGUP | FIONCLEX | FIOCLEX | FIOASYNC
        | TIOCSERCONFIG | TIOCSERGWILD | TIOCSERSWILD | TIOCGLCKTRMIOS | TIOCSLCKTRMIOS
        | TIOCSERGSTRUCT | TIOCSERGETLSR | TIOCSERGETMULTI | TIOCSERSETMULTI | TIOCMIWAIT
        | TIOCGICOUNT | FIOQSIZE => {
            not_implemented!(
                "{}: ioctl request 0x{:08x} not implemented",
                if is_main { "ptmx" } else { "pts" },
                request
            );
            error!(ENOSYS)
        }

        _ => error!(EINVAL),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        auth::{Credentials, FsCred},
        fs::{
            buffers::{VecInputBuffer, VecOutputBuffer},
            tmpfs::TmpFs,
            MountInfo, NamespaceNode,
        },
        testing::*,
    };
    use starnix_uapi::{
        file_mode::FileMode,
        signals::{SIGCHLD, SIGTTOU},
    };

    fn ioctl<T: zerocopy::AsBytes + zerocopy::FromBytes + Copy>(
        current_task: &CurrentTask,
        file: &FileHandle,
        command: u32,
        value: &T,
    ) -> Result<T, Errno> {
        let address =
            map_memory(current_task, UserAddress::default(), std::mem::size_of::<T>() as u64);
        let address_ref = UserRef::<T>::new(address);
        current_task.write_object(address_ref, value)?;
        file.ioctl(current_task, command, address.into())?;
        current_task.read_object(address_ref)
    }

    fn set_controlling_terminal(
        current_task: &CurrentTask,
        file: &FileHandle,
        steal: bool,
    ) -> Result<SyscallResult, Errno> {
        #[allow(clippy::bool_to_int_with_if)]
        file.ioctl(current_task, TIOCSCTTY, steal.into())
    }

    fn lookup_node(
        task: &CurrentTask,
        fs: &FileSystemHandle,
        name: &FsStr,
    ) -> Result<NamespaceNode, Errno> {
        let root = NamespaceNode::new_anonymous(fs.root().clone());
        root.lookup_child(task, &mut Default::default(), name)
    }

    fn open_file_with_flags(
        current_task: &CurrentTask,
        fs: &FileSystemHandle,
        name: &FsStr,
        flags: OpenFlags,
    ) -> Result<FileHandle, Errno> {
        let node = lookup_node(current_task, fs, name)?;
        node.open(current_task, flags, true)
    }

    fn open_file(
        current_task: &CurrentTask,
        fs: &FileSystemHandle,
        name: &FsStr,
    ) -> Result<FileHandle, Errno> {
        open_file_with_flags(current_task, fs, name, OpenFlags::RDWR | OpenFlags::NOCTTY)
    }

    fn open_ptmx_and_unlock(
        current_task: &CurrentTask,
        fs: &FileSystemHandle,
    ) -> Result<FileHandle, Errno> {
        let file = open_file_with_flags(current_task, fs, b"ptmx", OpenFlags::RDWR)?;

        // Unlock terminal
        ioctl::<i32>(current_task, &file, TIOCSPTLCK, &0)?;

        Ok(file)
    }

    #[::fuchsia::test]
    async fn opening_ptmx_creates_pts() {
        let (kernel, task) = create_kernel_and_task();
        let fs = dev_pts_fs(&kernel, Default::default());
        lookup_node(&task, fs, b"0").unwrap_err();
        let _ptmx = open_ptmx_and_unlock(&task, fs).expect("ptmx");
        lookup_node(&task, fs, b"0").expect("pty");
    }

    #[::fuchsia::test]
    async fn closing_ptmx_closes_pts() {
        let (kernel, task) = create_kernel_and_task();
        let fs = dev_pts_fs(&kernel, Default::default());
        lookup_node(&task, fs, b"0").unwrap_err();
        let ptmx = open_ptmx_and_unlock(&task, fs).expect("ptmx");
        let _pts = open_file(&task, fs, b"0").expect("open file");
        std::mem::drop(ptmx);
        lookup_node(&task, fs, b"0").unwrap_err();
    }

    #[::fuchsia::test]
    async fn pts_are_reused() {
        let (kernel, task) = create_kernel_and_task();
        let fs = dev_pts_fs(&kernel, Default::default());

        let _ptmx0 = open_ptmx_and_unlock(&task, fs).expect("ptmx");
        let mut _ptmx1 = open_ptmx_and_unlock(&task, fs).expect("ptmx");
        let _ptmx2 = open_ptmx_and_unlock(&task, fs).expect("ptmx");

        lookup_node(&task, fs, b"0").expect("component_lookup");
        lookup_node(&task, fs, b"1").expect("component_lookup");
        lookup_node(&task, fs, b"2").expect("component_lookup");

        std::mem::drop(_ptmx1);
        lookup_node(&task, fs, b"1").unwrap_err();

        _ptmx1 = open_ptmx_and_unlock(&task, fs).expect("ptmx");
        lookup_node(&task, fs, b"1").expect("component_lookup");
    }

    #[::fuchsia::test]
    async fn opening_inexistant_replica_fails() {
        let (kernel, task) = create_kernel_and_task();
        // Initialize pts devices
        dev_pts_fs(&kernel, Default::default());
        let fs = TmpFs::new_fs(&kernel);
        let mount = MountInfo::detached();
        let pts = fs
            .root()
            .create_entry(&task, &mount, b"custom_pts", |dir, mount, name| {
                dir.mknod(
                    &task,
                    mount,
                    name,
                    mode!(IFCHR, 0o666),
                    DeviceType::new(DEVPTS_FIRST_MAJOR, 0),
                    FsCred::root(),
                )
            })
            .expect("custom_pts");
        let node = NamespaceNode::new_anonymous(pts.clone());
        assert!(node.open(&task, OpenFlags::RDONLY, true).is_err());
    }

    #[::fuchsia::test]
    async fn test_open_tty() {
        let (kernel, task) = create_kernel_and_task();
        let fs = dev_pts_fs(&kernel, Default::default());
        let devfs = crate::fs::devtmpfs::dev_tmp_fs(&kernel);

        let ptmx = open_ptmx_and_unlock(&task, fs).expect("ptmx");
        set_controlling_terminal(&task, &ptmx, false).expect("set_controlling_terminal");
        let tty = open_file_with_flags(&task, devfs, b"tty", OpenFlags::RDWR).expect("tty");
        // Check that tty is the main terminal by calling the ioctl TIOCGPTN and checking it is
        // has the same result as on ptmx.
        assert_eq!(
            ioctl::<i32>(&task, &tty, TIOCGPTN, &0),
            ioctl::<i32>(&task, &ptmx, TIOCGPTN, &0)
        );

        // Detach the controlling terminal.
        ioctl::<i32>(&task, &ptmx, TIOCNOTTY, &0).expect("detach terminal");
        let pts = open_file(&task, fs, b"0").expect("open file");
        set_controlling_terminal(&task, &pts, false).expect("set_controlling_terminal");
        let tty = open_file_with_flags(&task, devfs, b"tty", OpenFlags::RDWR).expect("tty");
        // TIOCGPTN is not implemented on replica terminals
        assert!(ioctl::<i32>(&task, &tty, TIOCGPTN, &0).is_err());
    }

    #[::fuchsia::test]
    async fn test_unknown_ioctl() {
        let (kernel, task) = create_kernel_and_task();
        let fs = dev_pts_fs(&kernel, Default::default());

        let ptmx = open_ptmx_and_unlock(&task, fs).expect("ptmx");
        assert_eq!(ptmx.ioctl(&task, 42, Default::default()), error!(EINVAL));

        let pts_file = open_file(&task, fs, b"0").expect("open file");
        assert_eq!(pts_file.ioctl(&task, 42, Default::default()), error!(EINVAL));
    }

    #[::fuchsia::test]
    async fn test_tiocgptn_ioctl() {
        let (kernel, task) = create_kernel_and_task();
        let fs = dev_pts_fs(&kernel, Default::default());
        let ptmx0 = open_ptmx_and_unlock(&task, fs).expect("ptmx");
        let ptmx1 = open_ptmx_and_unlock(&task, fs).expect("ptmx");

        let pts0 = ioctl::<u32>(&task, &ptmx0, TIOCGPTN, &0).expect("ioctl");
        assert_eq!(pts0, 0);

        let pts1 = ioctl::<u32>(&task, &ptmx1, TIOCGPTN, &0).expect("ioctl");
        assert_eq!(pts1, 1);
    }

    #[::fuchsia::test]
    async fn test_new_terminal_is_locked() {
        let (kernel, task) = create_kernel_and_task();
        let fs = dev_pts_fs(&kernel, Default::default());
        let _ptmx_file = open_file(&task, fs, b"ptmx").expect("open file");

        let pts = lookup_node(&task, fs, b"0").expect("component_lookup");
        assert_eq!(pts.open(&task, OpenFlags::RDONLY, true).map(|_| ()), error!(EIO));
    }

    #[::fuchsia::test]
    async fn test_lock_ioctls() {
        let (kernel, task) = create_kernel_and_task();
        let fs = dev_pts_fs(&kernel, Default::default());
        let ptmx = open_ptmx_and_unlock(&task, fs).expect("ptmx");
        let pts = lookup_node(&task, fs, b"0").expect("component_lookup");

        // Check that the lock is not set.
        assert_eq!(ioctl::<i32>(&task, &ptmx, TIOCGPTLCK, &0), Ok(0));
        // /dev/pts/0 can be opened
        pts.open(&task, OpenFlags::RDONLY, true).expect("open");

        // Lock the terminal
        ioctl::<i32>(&task, &ptmx, TIOCSPTLCK, &42).expect("ioctl");
        // Check that the lock is set.
        assert_eq!(ioctl::<i32>(&task, &ptmx, TIOCGPTLCK, &0), Ok(1));
        // /dev/pts/0 cannot be opened
        assert_eq!(pts.open(&task, OpenFlags::RDONLY, true).map(|_| ()), error!(EIO));
    }

    #[::fuchsia::test]
    async fn test_ptmx_stats() {
        let (kernel, task) = create_kernel_and_task();
        task.set_creds(Credentials::with_ids(22, 22));
        let fs = dev_pts_fs(&kernel, Default::default());
        let ptmx = open_ptmx_and_unlock(&task, fs).expect("ptmx");
        let ptmx_stat = ptmx.node().stat(&task).expect("stat");
        assert_eq!(ptmx_stat.st_blksize as usize, BLOCK_SIZE);
        let pts = open_file(&task, fs, b"0").expect("open file");
        let pts_stats = pts.node().stat(&task).expect("stat");
        assert_eq!(pts_stats.st_mode & FileMode::PERMISSIONS.bits(), 0o620);
        assert_eq!(pts_stats.st_uid, 22);
        // TODO(qsr): Check that gid is tty.
    }

    #[::fuchsia::test]
    async fn test_attach_terminal_when_open() {
        let (kernel, task) = create_kernel_and_task();
        let fs = dev_pts_fs(&kernel, Default::default());
        let _opened_main = open_ptmx_and_unlock(&task, fs).expect("ptmx");
        // Opening the main terminal should not set the terminal of the session.
        assert!(task
            .thread_group
            .read()
            .process_group
            .session
            .read()
            .controlling_terminal
            .is_none());
        // Opening the terminal should not set the terminal of the session with the NOCTTY flag.
        let _opened_replica2 =
            open_file_with_flags(&task, fs, b"0", OpenFlags::RDWR | OpenFlags::NOCTTY)
                .expect("open file");
        assert!(task
            .thread_group
            .read()
            .process_group
            .session
            .read()
            .controlling_terminal
            .is_none());

        // Opening the replica terminal should set the terminal of the session.
        let _opened_replica2 =
            open_file_with_flags(&task, fs, b"0", OpenFlags::RDWR).expect("open file");
        assert!(task
            .thread_group
            .read()
            .process_group
            .session
            .read()
            .controlling_terminal
            .is_some());
    }

    #[::fuchsia::test]
    async fn test_attach_terminal() {
        let (kernel, task1) = create_kernel_and_task();
        let task2 = task1.clone_task_for_test(0, Some(SIGCHLD));
        task2.thread_group.setsid().expect("setsid");

        let fs = dev_pts_fs(&kernel, Default::default());
        let opened_main = open_ptmx_and_unlock(&task1, fs).expect("ptmx");
        let opened_replica = open_file(&task2, fs, b"0").expect("open file");

        assert_eq!(ioctl::<i32>(&task1, &opened_main, TIOCGPGRP, &0), error!(ENOTTY));
        assert_eq!(ioctl::<i32>(&task2, &opened_replica, TIOCGPGRP, &0), error!(ENOTTY));

        set_controlling_terminal(&task1, &opened_main, false).unwrap();
        assert_eq!(
            ioctl::<i32>(&task1, &opened_main, TIOCGPGRP, &0),
            Ok(task1.thread_group.read().process_group.leader)
        );
        assert_eq!(ioctl::<i32>(&task2, &opened_replica, TIOCGPGRP, &0), error!(ENOTTY));

        set_controlling_terminal(&task2, &opened_replica, false).unwrap();
        assert_eq!(
            ioctl::<i32>(&task2, &opened_replica, TIOCGPGRP, &0),
            Ok(task2.thread_group.read().process_group.leader)
        );
    }

    #[::fuchsia::test]
    async fn test_steal_terminal() {
        let (kernel, task1) = create_kernel_and_task();
        task1.set_creds(Credentials::with_ids(1, 1));

        let task2 = task1.clone_task_for_test(0, Some(SIGCHLD));

        let fs = dev_pts_fs(&kernel, Default::default());
        let _opened_main = open_ptmx_and_unlock(&task1, fs).expect("ptmx");
        let wo_opened_replica =
            open_file_with_flags(&task1, fs, b"0", OpenFlags::WRONLY | OpenFlags::NOCTTY)
                .expect("open file");
        assert!(!wo_opened_replica.can_read());

        // FD must be readable for setting the terminal.
        assert_eq!(set_controlling_terminal(&task1, &wo_opened_replica, false), error!(EPERM));

        let opened_replica = open_file(&task2, fs, b"0").expect("open file");
        // Task must be session leader for setting the terminal.
        assert_eq!(set_controlling_terminal(&task2, &opened_replica, false), error!(EINVAL));

        // Associate terminal to task1.
        set_controlling_terminal(&task1, &opened_replica, false)
            .expect("Associate terminal to task1");

        // One cannot associate a terminal to a process that has already one
        assert_eq!(set_controlling_terminal(&task1, &opened_replica, false), error!(EINVAL));

        task2.thread_group.setsid().expect("setsid");

        // One cannot associate a terminal that is already associated with another process.
        assert_eq!(set_controlling_terminal(&task2, &opened_replica, false), error!(EPERM));

        // One cannot steal a terminal without the CAP_SYS_ADMIN capacility
        assert_eq!(set_controlling_terminal(&task2, &opened_replica, true), error!(EPERM));

        // One can steal a terminal with the CAP_SYS_ADMIN capacility
        task2.set_creds(Credentials::with_ids(0, 0));
        // But not without specifying that one wants to steal it.
        assert_eq!(set_controlling_terminal(&task2, &opened_replica, false), error!(EPERM));
        set_controlling_terminal(&task2, &opened_replica, true)
            .expect("Associate terminal to task2");

        assert!(task1
            .thread_group
            .read()
            .process_group
            .session
            .read()
            .controlling_terminal
            .is_none());
    }

    #[::fuchsia::test]
    async fn test_set_foreground_process() {
        let (kernel, init) = create_kernel_and_task();
        let task1 = init.clone_task_for_test(0, Some(SIGCHLD));
        task1.thread_group.setsid().expect("setsid");
        let task2 = task1.clone_task_for_test(0, Some(SIGCHLD));
        task2.thread_group.setpgid(&task2, 0).expect("setpgid");
        let task2_pgid = task2.thread_group.read().process_group.leader;

        assert_ne!(task2_pgid, task1.thread_group.read().process_group.leader);

        let fs = dev_pts_fs(&kernel, Default::default());
        let _opened_main = open_ptmx_and_unlock(&init, fs).expect("ptmx");
        let opened_replica = open_file(&task2, fs, b"0").expect("open file");

        // Cannot change the foreground process group if the terminal is not the controlling
        // terminal
        assert_eq!(ioctl::<i32>(&task2, &opened_replica, TIOCSPGRP, &task2_pgid), error!(ENOTTY));

        // Attach terminal to task1 and task2 session.
        set_controlling_terminal(&task1, &opened_replica, false).unwrap();
        // The foreground process group should be the one of task1
        assert_eq!(
            ioctl::<i32>(&task1, &opened_replica, TIOCGPGRP, &0),
            Ok(task1.thread_group.read().process_group.leader)
        );

        // Cannot change the foreground process group to a negative pid.
        assert_eq!(ioctl::<i32>(&task2, &opened_replica, TIOCSPGRP, &-1), error!(EINVAL));

        // Cannot change the foreground process group to a invalid process group.
        assert_eq!(ioctl::<i32>(&task2, &opened_replica, TIOCSPGRP, &255), error!(ESRCH));

        // Cannot change the foreground process group to a process group in another session.
        let init_pgid = init.thread_group.read().process_group.leader;
        assert_eq!(ioctl::<i32>(&task2, &opened_replica, TIOCSPGRP, &init_pgid), error!(EPERM));

        // Changing the foreground process while being in background generates SIGTTOU and fails.
        assert_eq!(ioctl::<i32>(&task2, &opened_replica, TIOCSPGRP, &task2_pgid), error!(EINTR));
        assert!(task2.read().signals.has_queued(SIGTTOU));

        // Set the foregound process to task2 process group
        ioctl::<i32>(&task1, &opened_replica, TIOCSPGRP, &task2_pgid).unwrap();

        // Check that the foreground process has been changed.
        let terminal = Arc::clone(
            &task1
                .thread_group
                .read()
                .process_group
                .session
                .read()
                .controlling_terminal
                .as_ref()
                .unwrap()
                .terminal,
        );
        assert_eq!(
            terminal
                .read()
                .get_controlling_session(false)
                .as_ref()
                .unwrap()
                .foregound_process_group_leader,
            task2_pgid
        );
    }

    #[::fuchsia::test]
    async fn test_detach_session() {
        let (kernel, task1) = create_kernel_and_task();
        let task2 = task1.clone_task_for_test(0, Some(SIGCHLD));
        task2.thread_group.setsid().expect("setsid");

        let fs = dev_pts_fs(&kernel, Default::default());
        let _opened_main = open_ptmx_and_unlock(&task1, fs).expect("ptmx");
        let opened_replica = open_file(&task1, fs, b"0").expect("open file");

        // Cannot detach the controlling terminal when none is attached terminal
        assert_eq!(ioctl::<i32>(&task1, &opened_replica, TIOCNOTTY, &0), error!(ENOTTY));

        set_controlling_terminal(&task2, &opened_replica, false).expect("set controlling terminal");

        // Cannot detach the controlling terminal when not the session leader.
        assert_eq!(ioctl::<i32>(&task1, &opened_replica, TIOCNOTTY, &0), error!(ENOTTY));

        // Detach the terminal
        ioctl::<i32>(&task2, &opened_replica, TIOCNOTTY, &0).expect("detach terminal");
        assert!(task2
            .thread_group
            .read()
            .process_group
            .session
            .read()
            .controlling_terminal
            .is_none());
    }

    #[::fuchsia::test]
    async fn test_send_data_back_and_forth() {
        let (kernel, task) = create_kernel_and_task();
        let fs = dev_pts_fs(&kernel, Default::default());
        let ptmx = open_ptmx_and_unlock(&task, fs).expect("ptmx");
        let pts = open_file(&task, fs, b"0").expect("open file");

        let has_data_ready_to_read = |fd: &FileHandle| {
            fd.query_events(&task).expect("query_events").contains(FdEvents::POLLIN)
        };

        let write_and_assert = |fd: &FileHandle, data: &[u8]| {
            assert_eq!(fd.write(&task, &mut VecInputBuffer::new(data)).expect("write"), data.len());
        };

        let read_and_check = |fd: &FileHandle, data: &[u8]| {
            assert!(has_data_ready_to_read(fd));
            let mut buffer = VecOutputBuffer::new(data.len() + 1);
            assert_eq!(fd.read(&task, &mut buffer).expect("read"), data.len());
            assert_eq!(data, buffer.data());
        };

        let hello_buffer = b"hello\n";
        let hello_transformed_buffer = b"hello\r\n";

        // Main to replica
        write_and_assert(&ptmx, hello_buffer);
        read_and_check(&pts, hello_buffer);

        // Data has been echoed
        read_and_check(&ptmx, hello_transformed_buffer);

        // Replica to main
        write_and_assert(&pts, hello_buffer);
        read_and_check(&ptmx, hello_transformed_buffer);

        // Data has not been echoed
        assert!(!has_data_ready_to_read(&pts));
    }
}
