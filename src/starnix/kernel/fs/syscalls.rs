// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    fs::{
        buffers::{UserBuffersInputBuffer, UserBuffersOutputBuffer},
        eventfd::{new_eventfd, EventFdType},
        fuchsia::{TimerFile, TimerFileClock},
        inotify::InotifyFileObject,
        namespace::FileSystemCreator,
        new_memfd,
        pidfd::new_pidfd,
        pipe::{new_pipe, PipeFileObject},
        splice, DirentSink64, EpollFileObject, FallocMode, FdEvents, FdFlags, FdNumber,
        FileAsyncOwner, FileHandle, FileSystemOptions, FlockOperation, FsStr, InotifyMask,
        LookupContext, NamespaceNode, PathWithReachability, RecordLockCommand, RenameFlags,
        SeekTarget, StatxFlags, SymlinkMode, SymlinkTarget, TargetFdNumber, TimeUpdateType,
        UnlinkKind, ValueOrSize, WdNumber, WhatToMount, XattrOp,
    },
    logging::{log_trace, not_implemented, not_implemented_log_once},
    mm::{MemoryAccessor, MemoryAccessorExt},
    task::{CurrentTask, EnqueueEventHandler, EventHandler, ReadyItem, ReadyItemKey, Task, Waiter},
};
use fuchsia_zircon as zx;
use lock_sequence::{Locked, Unlocked};
use starnix_lock::Mutex;
use starnix_syscalls::{SyscallArg, SyscallResult, SUCCESS};
use starnix_uapi::{
    __kernel_fd_set,
    auth::{CAP_DAC_READ_SEARCH, CAP_SYS_ADMIN, CAP_WAKE_ALARM, PTRACE_MODE_ATTACH_REALCREDS},
    device_type::DeviceType,
    epoll_event, errno, error,
    errors::{Errno, ErrnoResultExt, EINTR, ENAMETOOLONG, ETIMEDOUT},
    f_owner_ex,
    file_mode::{Access, FileMode},
    itimerspec,
    mount_flags::MountFlags,
    off_t,
    open_flags::OpenFlags,
    personality::PersonalityFlags,
    pid_t, pollfd, pselect6_sigmask,
    resource_limits::Resource,
    seal_flags::SealFlags,
    signals::SigSet,
    sigset_t, statfs, statx,
    time::{
        duration_from_poll_timeout, duration_from_timespec, time_from_timespec,
        timespec_from_duration,
    },
    timespec, uapi, uid_t,
    user_address::{UserAddress, UserCString, UserRef},
    AT_EACCESS, AT_EMPTY_PATH, AT_NO_AUTOMOUNT, AT_REMOVEDIR, AT_SYMLINK_FOLLOW,
    AT_SYMLINK_NOFOLLOW, CLOCK_BOOTTIME, CLOCK_BOOTTIME_ALARM, CLOCK_MONOTONIC, CLOCK_REALTIME,
    CLOCK_REALTIME_ALARM, CLOSE_RANGE_CLOEXEC, CLOSE_RANGE_UNSHARE, EFD_CLOEXEC, EFD_NONBLOCK,
    EFD_SEMAPHORE, EPOLL_CLOEXEC, EPOLL_CTL_ADD, EPOLL_CTL_DEL, EPOLL_CTL_MOD, F_ADD_SEALS,
    F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD, F_GETFL, F_GETLK, F_GETOWN, F_GETOWN_EX, F_GET_SEALS,
    F_OFD_GETLK, F_OFD_SETLK, F_OFD_SETLKW, F_OWNER_PGRP, F_OWNER_PID, F_OWNER_TID, F_SETFD,
    F_SETFL, F_SETLK, F_SETLKW, F_SETOWN, F_SETOWN_EX, IN_CLOEXEC, IN_NONBLOCK, MFD_ALLOW_SEALING,
    MFD_CLOEXEC, MFD_HUGETLB, MFD_HUGE_MASK, MFD_HUGE_SHIFT, NAME_MAX, O_CLOEXEC, PATH_MAX,
    PIDFD_NONBLOCK, POLLERR, POLLHUP, POLLIN, POLLOUT, POLLPRI, POLLRDBAND, POLLRDNORM, POLLWRBAND,
    POLLWRNORM, POSIX_FADV_DONTNEED, POSIX_FADV_NOREUSE, POSIX_FADV_NORMAL, POSIX_FADV_RANDOM,
    POSIX_FADV_SEQUENTIAL, POSIX_FADV_WILLNEED, RWF_SUPPORTED, TFD_CLOEXEC, TFD_NONBLOCK,
    TFD_TIMER_ABSTIME, TFD_TIMER_CANCEL_ON_SET, UMOUNT_NOFOLLOW, XATTR_CREATE, XATTR_NAME_MAX,
    XATTR_REPLACE,
};
use std::{
    cmp::Ordering, collections::VecDeque, convert::TryInto, marker::PhantomData, sync::Arc, usize,
};

// Constants from bionic/libc/include/sys/stat.h
const UTIME_NOW: i64 = 0x3fffffff;
const UTIME_OMIT: i64 = 0x3ffffffe;

pub fn sys_read(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    address: UserAddress,
    length: usize,
) -> Result<usize, Errno> {
    let file = current_task.files.get(fd)?;
    file.read(
        current_task,
        &mut UserBuffersOutputBuffer::new_at(&current_task.mm, address, length)?,
    )
    .map_eintr(errno!(ERESTARTSYS))
}

pub fn sys_write(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    address: UserAddress,
    length: usize,
) -> Result<usize, Errno> {
    let file = current_task.files.get(fd)?;
    file.write(
        current_task,
        &mut UserBuffersInputBuffer::new_at(&current_task.mm, address, length)?,
    )
    .map_eintr(errno!(ERESTARTSYS))
}

pub fn sys_close(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
) -> Result<(), Errno> {
    current_task.files.close(fd)?;
    Ok(())
}

pub fn sys_close_range(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    first: u32,
    last: u32,
    flags: u32,
) -> Result<(), Errno> {
    if first > last || flags & !(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC) != 0 {
        return error!(EINVAL);
    }
    if flags & CLOSE_RANGE_UNSHARE != 0 {
        current_task.files.unshare();
    }
    let in_range = |fd: FdNumber| fd.raw() as u32 >= first && fd.raw() as u32 <= last;
    if flags & CLOSE_RANGE_CLOEXEC != 0 {
        current_task.files.retain(|fd, flags| {
            if in_range(fd) {
                *flags |= FdFlags::CLOEXEC;
            }
            true
        });
    } else {
        current_task.files.retain(|fd, _| !in_range(fd));
    }
    Ok(())
}

pub fn sys_lseek(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    offset: off_t,
    whence: u32,
) -> Result<off_t, Errno> {
    let file = current_task.files.get(fd)?;
    file.seek(current_task, SeekTarget::from_raw(whence, offset)?)
}

pub fn sys_fcntl(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    cmd: u32,
    arg: u64,
) -> Result<SyscallResult, Errno> {
    match cmd {
        F_DUPFD | F_DUPFD_CLOEXEC => {
            let fd_number = arg as i32;
            let flags = if cmd == F_DUPFD_CLOEXEC { FdFlags::CLOEXEC } else { FdFlags::empty() };
            let newfd = current_task.files.duplicate(
                current_task,
                fd,
                TargetFdNumber::Minimum(FdNumber::from_raw(fd_number)),
                flags,
            )?;
            Ok(newfd.into())
        }
        F_GETOWN => {
            let file = current_task.files.get_unless_opath(fd)?;
            match file.get_async_owner() {
                FileAsyncOwner::Unowned => Ok(0.into()),
                FileAsyncOwner::Thread(tid) => Ok(tid.into()),
                FileAsyncOwner::Process(pid) => Ok(pid.into()),
                FileAsyncOwner::ProcessGroup(pgid) => Ok((-pgid).into()),
            }
        }
        F_GETOWN_EX => {
            let file = current_task.files.get_unless_opath(fd)?;
            let maybe_owner = match file.get_async_owner() {
                FileAsyncOwner::Unowned => None,
                FileAsyncOwner::Thread(tid) => {
                    Some(uapi::f_owner_ex { type_: F_OWNER_TID as i32, pid: tid })
                }
                FileAsyncOwner::Process(pid) => {
                    Some(uapi::f_owner_ex { type_: F_OWNER_PID as i32, pid })
                }
                FileAsyncOwner::ProcessGroup(pgid) => {
                    Some(uapi::f_owner_ex { type_: F_OWNER_PGRP as i32, pid: pgid })
                }
            };
            if let Some(owner) = maybe_owner {
                let user_owner: UserRef<f_owner_ex> =
                    UserRef::<uapi::f_owner_ex>::new(UserAddress::from(arg));
                current_task.write_object(user_owner, &owner)?;
            }
            Ok(SUCCESS)
        }
        F_SETOWN => {
            let file = current_task.files.get_unless_opath(fd)?;
            let pid = (arg as u32) as i32;
            let owner = match pid.cmp(&0) {
                Ordering::Equal => FileAsyncOwner::Unowned,
                Ordering::Greater => FileAsyncOwner::Process(pid),
                Ordering::Less => {
                    FileAsyncOwner::ProcessGroup(pid.checked_neg().ok_or_else(|| errno!(EINVAL))?)
                }
            };
            owner.validate(current_task)?;
            file.set_async_owner(owner);
            Ok(SUCCESS)
        }
        F_SETOWN_EX => {
            let file = current_task.files.get_unless_opath(fd)?;
            let user_owner = UserRef::<uapi::f_owner_ex>::new(UserAddress::from(arg));
            let requested_owner = current_task.read_object(user_owner)?;
            let mut owner = match requested_owner.type_ as u32 {
                F_OWNER_TID => FileAsyncOwner::Thread(requested_owner.pid),
                F_OWNER_PID => FileAsyncOwner::Process(requested_owner.pid),
                F_OWNER_PGRP => FileAsyncOwner::ProcessGroup(requested_owner.pid),
                _ => return error!(EINVAL),
            };
            if requested_owner.pid == 0 {
                owner = FileAsyncOwner::Unowned;
            }
            owner.validate(current_task)?;
            file.set_async_owner(owner);
            Ok(SUCCESS)
        }
        F_GETFD => Ok(current_task.files.get_fd_flags(fd)?.into()),
        F_SETFD => {
            current_task.files.set_fd_flags(fd, FdFlags::from_bits_truncate(arg as u32))?;
            Ok(SUCCESS)
        }
        F_GETFL => {
            let file = current_task.files.get(fd)?;
            Ok(file.flags().into())
        }
        F_SETFL => {
            let settable_flags = OpenFlags::APPEND
                | OpenFlags::DIRECT
                | OpenFlags::NOATIME
                | OpenFlags::NONBLOCK
                | OpenFlags::ASYNC;
            let requested_flags =
                OpenFlags::from_bits_truncate((arg as u32) & settable_flags.bits());
            let file = current_task.files.get_unless_opath(fd)?;

            // If `NOATIME` flag is being set then check that it's allowed.
            if requested_flags.contains(OpenFlags::NOATIME)
                && !file.flags().contains(OpenFlags::NOATIME)
            {
                file.name.check_access(current_task, Access::NOATIME)?;
            }

            file.update_file_flags(requested_flags, settable_flags);
            Ok(SUCCESS)
        }
        F_SETLK | F_SETLKW | F_GETLK | F_OFD_GETLK | F_OFD_SETLK | F_OFD_SETLKW => {
            let file = current_task.files.get_unless_opath(fd)?;
            let flock_ref = UserRef::<uapi::flock>::new(arg.into());
            let flock = current_task.read_object(flock_ref)?;
            let cmd = RecordLockCommand::from_raw(cmd).ok_or_else(|| errno!(EINVAL))?;
            if let Some(flock) = file.record_lock(current_task, cmd, flock)? {
                current_task.write_object(flock_ref, &flock)?;
            }
            Ok(SUCCESS)
        }
        F_ADD_SEALS => {
            let file = current_task.files.get_unless_opath(fd)?;

            if !file.can_write() {
                // Cannot add seals if the file is not writable
                return error!(EPERM);
            }

            let mut state = file.name.entry.node.write_guard_state.lock();
            let flags = SealFlags::from_bits_truncate(arg as u32);
            state.try_add_seal(flags)?;
            Ok(SUCCESS)
        }
        F_GET_SEALS => {
            let file = current_task.files.get_unless_opath(fd)?;
            let state = file.name.entry.node.write_guard_state.lock();
            Ok(state.get_seals()?.into())
        }
        _ => {
            let file = current_task.files.get(fd)?;
            file.fcntl(current_task, cmd, arg)
        }
    }
}

pub fn sys_pread64(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    address: UserAddress,
    length: usize,
    offset: off_t,
) -> Result<usize, Errno> {
    let file = current_task.files.get(fd)?;
    let offset = offset.try_into().map_err(|_| errno!(EINVAL))?;
    file.read_at(
        current_task,
        offset,
        &mut UserBuffersOutputBuffer::new_at(&current_task.mm, address, length)?,
    )
}

pub fn sys_pwrite64(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    address: UserAddress,
    length: usize,
    offset: off_t,
) -> Result<usize, Errno> {
    let file = current_task.files.get(fd)?;
    let offset = offset.try_into().map_err(|_| errno!(EINVAL))?;
    file.write_at(
        current_task,
        offset,
        &mut UserBuffersInputBuffer::new_at(&current_task.mm, address, length)?,
    )
}

fn do_readv(
    current_task: &CurrentTask,
    fd: FdNumber,
    iovec_addr: UserAddress,
    iovec_count: i32,
    offset: Option<off_t>,
    flags: u32,
) -> Result<usize, Errno> {
    if flags & !RWF_SUPPORTED != 0 {
        return error!(EOPNOTSUPP);
    }
    if flags != 0 {
        not_implemented!("preadv2: flags: 0x{:x}", flags);
    }
    let file = current_task.files.get(fd)?;
    let iovec = current_task.read_iovec(iovec_addr, iovec_count)?;
    let mut data = UserBuffersOutputBuffer::new(&current_task.mm, iovec)?;
    if let Some(offset) = offset {
        file.read_at(current_task, offset.try_into().map_err(|_| errno!(EINVAL))?, &mut data)
    } else {
        file.read(current_task, &mut data)
    }
}

pub fn sys_readv(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    iovec_addr: UserAddress,
    iovec_count: i32,
) -> Result<usize, Errno> {
    do_readv(current_task, fd, iovec_addr, iovec_count, None, 0)
}

pub fn sys_preadv(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    iovec_addr: UserAddress,
    iovec_count: i32,
    offset: off_t,
) -> Result<usize, Errno> {
    do_readv(current_task, fd, iovec_addr, iovec_count, Some(offset), 0)
}

pub fn sys_preadv2(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    iovec_addr: UserAddress,
    iovec_count: i32,
    offset: off_t,
    _unused: SyscallArg, // On 32-bit systems, holds the upper 32 bits of offset.
    flags: u32,
) -> Result<usize, Errno> {
    let offset = if offset == -1 { None } else { Some(offset) };
    do_readv(current_task, fd, iovec_addr, iovec_count, offset, flags)
}

fn do_writev(
    current_task: &CurrentTask,
    fd: FdNumber,
    iovec_addr: UserAddress,
    iovec_count: i32,
    offset: Option<off_t>,
    flags: u32,
) -> Result<usize, Errno> {
    if flags & !RWF_SUPPORTED != 0 {
        return error!(EOPNOTSUPP);
    }
    if flags != 0 {
        not_implemented!("pwritev2: flags: 0x{:x}", flags);
    }
    // TODO(fxbug.dev/117677) Allow partial writes.
    let file = current_task.files.get(fd)?;
    let iovec = current_task.read_iovec(iovec_addr, iovec_count)?;
    let mut data = UserBuffersInputBuffer::new(&current_task.mm, iovec)?;
    if let Some(offset) = offset {
        file.write_at(current_task, offset.try_into().map_err(|_| errno!(EINVAL))?, &mut data)
    } else {
        file.write(current_task, &mut data)
    }
}

pub fn sys_writev(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    iovec_addr: UserAddress,
    iovec_count: i32,
) -> Result<usize, Errno> {
    do_writev(current_task, fd, iovec_addr, iovec_count, None, 0)
}

pub fn sys_pwritev(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    iovec_addr: UserAddress,
    iovec_count: i32,
    offset: off_t,
) -> Result<usize, Errno> {
    do_writev(current_task, fd, iovec_addr, iovec_count, Some(offset), 0)
}

pub fn sys_pwritev2(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    iovec_addr: UserAddress,
    iovec_count: i32,
    offset: off_t,
    _unused: SyscallArg, // On 32-bit systems, holds the upper 32 bits of offset.
    flags: u32,
) -> Result<usize, Errno> {
    let offset = if offset == -1 { None } else { Some(offset) };
    do_writev(current_task, fd, iovec_addr, iovec_count, offset, flags)
}

pub fn sys_fstatfs(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    user_buf: UserRef<statfs>,
) -> Result<(), Errno> {
    let file = current_task.files.get(fd)?;
    let stat = file.fs.statfs(current_task)?;
    current_task.write_object(user_buf, &stat)?;
    Ok(())
}

pub fn sys_statfs(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    user_path: UserCString,
    user_buf: UserRef<statfs>,
) -> Result<(), Errno> {
    let node = lookup_at(current_task, FdNumber::AT_FDCWD, user_path, LookupFlags::default())?;
    let file_system = node.entry.node.fs();
    let stat = file_system.statfs(current_task)?;
    current_task.write_object(user_buf, &stat)?;

    Ok(())
}

pub fn sys_sendfile(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    out_fd: FdNumber,
    in_fd: FdNumber,
    offset: UserAddress,
    count: i32,
) -> Result<usize, Errno> {
    splice::sendfile(current_task, out_fd, in_fd, offset, count)
}

/// A convenient wrapper for Task::open_file_at.
///
/// Reads user_path from user memory and then calls through to Task::open_file_at.
fn open_file_at(
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    flags: u32,
    mode: FileMode,
) -> Result<FileHandle, Errno> {
    let path = current_task.read_c_string_to_vec(user_path, PATH_MAX as usize)?;
    log_trace!("open_file_at(dir_fd={}, path={})", dir_fd, String::from_utf8_lossy(&path));
    current_task.open_file_at(dir_fd, &path, OpenFlags::from_bits_truncate(flags), mode)
}

fn lookup_parent_at<T, F>(
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    callback: F,
) -> Result<T, Errno>
where
    F: Fn(LookupContext, NamespaceNode, &FsStr) -> Result<T, Errno>,
{
    let path = current_task.read_c_string_to_vec(user_path, PATH_MAX as usize)?;
    log_trace!("lookup_parent_at(dir_fd={}, path={})", dir_fd, String::from_utf8_lossy(&path));
    if path.is_empty() {
        return error!(ENOENT);
    }
    let mut context = LookupContext::default();
    let (parent, basename) = current_task.lookup_parent_at(&mut context, dir_fd, &path)?;
    callback(context, parent, basename)
}

/// Options for lookup_at.
#[derive(Debug, Default)]
struct LookupFlags {
    /// Whether AT_EMPTY_PATH was supplied.
    allow_empty_path: bool,

    /// Used to implement AT_SYMLINK_NOFOLLOW.
    symlink_mode: SymlinkMode,

    /// Automount directories on the path.
    // TODO(fxbug.dev/91430): Support the `AT_NO_AUTOMOUNT` flag.
    #[allow(dead_code)]
    automount: bool,
}

impl LookupFlags {
    fn no_follow() -> Self {
        Self { symlink_mode: SymlinkMode::NoFollow, ..Default::default() }
    }

    fn from_bits(flags: u32, allowed_flags: u32) -> Result<Self, Errno> {
        if flags & !allowed_flags != 0 {
            return error!(EINVAL);
        }
        let follow_symlinks = if allowed_flags & AT_SYMLINK_FOLLOW != 0 {
            flags & AT_SYMLINK_FOLLOW != 0
        } else {
            flags & AT_SYMLINK_NOFOLLOW == 0
        };
        let automount =
            if allowed_flags & AT_NO_AUTOMOUNT != 0 { flags & AT_NO_AUTOMOUNT == 0 } else { false };
        if automount {
            not_implemented_log_once!("LookupFlags::automount is not implemented");
        }
        Ok(LookupFlags {
            allow_empty_path: flags & AT_EMPTY_PATH != 0,
            symlink_mode: if follow_symlinks { SymlinkMode::Follow } else { SymlinkMode::NoFollow },
            automount,
        })
    }
}

impl From<StatxFlags> for LookupFlags {
    fn from(flags: StatxFlags) -> Self {
        let lookup_flags = StatxFlags::AT_SYMLINK_NOFOLLOW
            | StatxFlags::AT_EMPTY_PATH
            | StatxFlags::AT_NO_AUTOMOUNT;
        Self::from_bits((flags & lookup_flags).bits(), lookup_flags.bits()).unwrap()
    }
}

fn lookup_at(
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    options: LookupFlags,
) -> Result<NamespaceNode, Errno> {
    let path = current_task.read_c_string_to_vec(user_path, PATH_MAX as usize)?;
    log_trace!("lookup_at(dir_fd={}, path={})", dir_fd, String::from_utf8_lossy(&path));
    if path.is_empty() {
        if options.allow_empty_path {
            let (node, _) = current_task.resolve_dir_fd(dir_fd, &path)?;
            return Ok(node);
        }
        return error!(ENOENT);
    }

    let mut parent_context = LookupContext::default();
    let (parent, basename) = current_task.lookup_parent_at(&mut parent_context, dir_fd, &path)?;

    let mut child_context = if parent_context.must_be_directory {
        // The child must resolve to a directory. This is because a trailing slash
        // was found in the path. If the child is a symlink, we should follow it.
        // See https://pubs.opengroup.org/onlinepubs/9699919799/xrat/V4_xbd_chap03.html#tag_21_03_00_75
        parent_context.with(SymlinkMode::Follow)
    } else {
        parent_context.with(options.symlink_mode)
    };

    parent.lookup_child(current_task, &mut child_context, basename)
}

pub fn sys_openat(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    flags: u32,
    mode: FileMode,
) -> Result<FdNumber, Errno> {
    let file = open_file_at(current_task, dir_fd, user_path, flags, mode)?;
    let fd_flags = get_fd_flags(flags);
    current_task.add_file(file, fd_flags)
}

pub fn sys_faccessat(
    locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    mode: u32,
) -> Result<(), Errno> {
    sys_faccessat2(locked, current_task, dir_fd, user_path, mode, 0)
}

pub fn sys_faccessat2(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    mode: u32,
    flags: u32,
) -> Result<(), Errno> {
    let mode = Access::from_bits(mode).ok_or_else(|| errno!(EINVAL))?;
    let lookup_flags = LookupFlags::from_bits(flags, AT_SYMLINK_NOFOLLOW | AT_EACCESS)?;
    let name = lookup_at(current_task, dir_fd, user_path, lookup_flags)?;
    name.check_access(current_task, mode)
}

pub fn sys_getdents64(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    user_buffer: UserAddress,
    user_capacity: usize,
) -> Result<usize, Errno> {
    let file = current_task.files.get(fd)?;
    let mut offset = file.offset.lock();
    let mut sink = DirentSink64::new(current_task, &mut offset, user_buffer, user_capacity);
    let result = file.readdir(current_task, &mut sink);
    sink.map_result_with_actual(result)
}

pub fn sys_chroot(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    user_path: UserCString,
) -> Result<(), Errno> {
    let name = lookup_at(current_task, FdNumber::AT_FDCWD, user_path, LookupFlags::default())?;
    if !name.entry.node.is_dir() {
        return error!(ENOTDIR);
    }

    current_task.fs().chroot(current_task, name)?;
    Ok(())
}

pub fn sys_chdir(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    user_path: UserCString,
) -> Result<(), Errno> {
    let name = lookup_at(current_task, FdNumber::AT_FDCWD, user_path, LookupFlags::default())?;
    if !name.entry.node.is_dir() {
        return error!(ENOTDIR);
    }
    current_task.fs().chdir(current_task, name)
}

pub fn sys_fchdir(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
) -> Result<(), Errno> {
    let file = current_task.files.get(fd)?;
    if !file.name.entry.node.is_dir() {
        return error!(ENOTDIR);
    }
    current_task.fs().chdir(current_task, file.name.clone())
}

pub fn sys_fstat(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    buffer: UserRef<uapi::stat>,
) -> Result<(), Errno> {
    let file = current_task.files.get(fd)?;
    let result = file.node().stat(current_task)?;
    current_task.write_object(buffer, &result)?;
    Ok(())
}

pub fn sys_newfstatat(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    buffer: UserRef<uapi::stat>,
    flags: u32,
) -> Result<(), Errno> {
    if flags & !(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH) != 0 {
        // TODO(fxbug.dev/91430): Support the `AT_NO_AUTOMOUNT` flag.
        not_implemented!("newfstatat: flags 0x{:x}", flags);
        return error!(ENOSYS);
    }
    let flags = LookupFlags::from_bits(flags, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)?;
    let name = lookup_at(current_task, dir_fd, user_path, flags)?;
    let result = name.entry.node.stat(current_task)?;
    current_task.write_object(buffer, &result)?;
    Ok(())
}

pub fn sys_statx(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    flags: u32,
    mask: u32,
    statxbuf: UserRef<statx>,
) -> Result<(), Errno> {
    let flags = StatxFlags::from_bits(flags).ok_or_else(|| errno!(EINVAL))?;
    if flags & (StatxFlags::AT_STATX_FORCE_SYNC | StatxFlags::AT_STATX_DONT_SYNC)
        == (StatxFlags::AT_STATX_FORCE_SYNC | StatxFlags::AT_STATX_DONT_SYNC)
    {
        return error!(EINVAL);
    }

    let name = lookup_at(current_task, dir_fd, user_path, LookupFlags::from(flags))?;
    let result = name.entry.node.statx(current_task, flags, mask)?;
    current_task.write_object(statxbuf, &result)?;
    Ok(())
}

pub fn sys_readlinkat(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    buffer: UserAddress,
    buffer_size: usize,
) -> Result<usize, Errno> {
    let name = lookup_at(current_task, dir_fd, user_path, LookupFlags::no_follow())?;

    let target = match name.readlink(current_task)? {
        SymlinkTarget::Path(path) => path,
        SymlinkTarget::Node(node) => node.path(current_task),
    };

    if buffer_size == 0 {
        return error!(EINVAL);
    }
    // Cap the returned length at buffer_size.
    let length = std::cmp::min(buffer_size, target.len());
    current_task.write_memory(buffer, &target[..length])?;
    Ok(length)
}

pub fn sys_truncate(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    user_path: UserCString,
    length: off_t,
) -> Result<(), Errno> {
    let length = length.try_into().map_err(|_| errno!(EINVAL))?;
    let name = lookup_at(current_task, FdNumber::AT_FDCWD, user_path, LookupFlags::default())?;
    name.truncate(current_task, length)?;
    Ok(())
}

pub fn sys_ftruncate(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    length: off_t,
) -> Result<(), Errno> {
    let length = length.try_into().map_err(|_| errno!(EINVAL))?;
    let file = current_task.files.get_unless_opath(fd)?;
    file.ftruncate(current_task, length)?;
    Ok(())
}

pub fn sys_mkdirat(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    mode: FileMode,
) -> Result<(), Errno> {
    let path = current_task.read_c_string_to_vec(user_path, PATH_MAX as usize)?;

    if path.is_empty() {
        return error!(ENOENT);
    }
    let (parent, basename) =
        current_task.lookup_parent_at(&mut LookupContext::default(), dir_fd, &path)?;
    parent.create_node(
        current_task,
        basename,
        mode.with_type(FileMode::IFDIR),
        DeviceType::NONE,
    )?;
    Ok(())
}

pub fn sys_mknodat(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    mode: FileMode,
    dev: DeviceType,
) -> Result<(), Errno> {
    let file_type = match mode.fmt() {
        FileMode::IFREG
        | FileMode::IFCHR
        | FileMode::IFBLK
        | FileMode::IFIFO
        | FileMode::IFSOCK => mode.fmt(),
        FileMode::EMPTY => FileMode::IFREG,
        _ => return error!(EINVAL),
    };
    lookup_parent_at(current_task, dir_fd, user_path, |_, parent, basename| {
        parent.create_node(current_task, basename, mode.with_type(file_type), dev)
    })?;
    Ok(())
}

pub fn sys_linkat(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    old_dir_fd: FdNumber,
    old_user_path: UserCString,
    new_dir_fd: FdNumber,
    new_user_path: UserCString,
    flags: u32,
) -> Result<(), Errno> {
    if flags & !(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH) != 0 {
        not_implemented!("linkat: flags 0x{:x}", flags);
        return error!(EINVAL);
    }

    if flags & AT_EMPTY_PATH != 0 && !current_task.creds().has_capability(CAP_DAC_READ_SEARCH) {
        return error!(ENOENT);
    }

    let flags = LookupFlags::from_bits(flags, AT_EMPTY_PATH | AT_SYMLINK_FOLLOW)?;
    let target = lookup_at(current_task, old_dir_fd, old_user_path, flags)?;
    lookup_parent_at(current_task, new_dir_fd, new_user_path, |context, parent, basename| {
        // The path to a new link cannot end in `/`. That would imply that we are dereferencing
        // the link to a directory.
        if context.must_be_directory {
            return error!(ENOENT);
        }
        if target.mount != parent.mount {
            return error!(EXDEV);
        }
        parent.link(current_task, basename, &target.entry.node)
    })?;

    Ok(())
}

pub fn sys_unlinkat(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    flags: u32,
) -> Result<(), Errno> {
    if flags & !AT_REMOVEDIR != 0 {
        return error!(EINVAL);
    }
    let kind =
        if flags & AT_REMOVEDIR != 0 { UnlinkKind::Directory } else { UnlinkKind::NonDirectory };
    lookup_parent_at(current_task, dir_fd, user_path, |context, parent, basename| {
        parent.unlink(current_task, basename, kind, context.must_be_directory)
    })?;
    Ok(())
}

pub fn sys_renameat2(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    old_dir_fd: FdNumber,
    old_user_path: UserCString,
    new_dir_fd: FdNumber,
    new_user_path: UserCString,
    flags: u32,
) -> Result<(), Errno> {
    let flags = RenameFlags::from_bits(flags).ok_or_else(|| errno!(EINVAL))?;
    if flags.intersects(RenameFlags::INTERNAL) {
        return error!(EINVAL);
    };

    // RENAME_EXCHANGE cannot be combined with the other flags.
    if flags.contains(RenameFlags::EXCHANGE)
        && flags.intersects(RenameFlags::NOREPLACE | RenameFlags::WHITEOUT)
    {
        return error!(EINVAL);
    }

    // RENAME_WHITEOUT is not supported.
    if flags.contains(RenameFlags::WHITEOUT) {
        not_implemented!("RENAME_WHITEOUT is not implemented");
        return error!(ENOSYS);
    };

    let lookup = |dir_fd, user_path| {
        lookup_parent_at(current_task, dir_fd, user_path, |_, parent, basename| {
            Ok((parent, basename.to_vec()))
        })
    };

    let (old_parent, old_basename) = lookup(old_dir_fd, old_user_path)?;
    let (new_parent, new_basename) = lookup(new_dir_fd, new_user_path)?;

    if new_basename.len() > NAME_MAX as usize {
        return error!(ENAMETOOLONG);
    }

    NamespaceNode::rename(
        current_task,
        &old_parent,
        &old_basename,
        &new_parent,
        &new_basename,
        flags,
    )
}

pub fn sys_fchmod(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    mode: FileMode,
) -> Result<(), Errno> {
    // Remove the filetype from the mode.
    let mode = mode & FileMode::PERMISSIONS;
    let file = current_task.files.get_unless_opath(fd)?;
    file.name.entry.node.chmod(current_task, &file.name.mount, mode)?;
    file.notify(InotifyMask::ATTRIB);
    Ok(())
}

pub fn sys_fchmodat(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    mode: FileMode,
) -> Result<(), Errno> {
    // Remove the filetype from the mode.
    let mode = mode & FileMode::PERMISSIONS;
    let name = lookup_at(current_task, dir_fd, user_path, LookupFlags::default())?;
    name.entry.node.chmod(current_task, &name.mount, mode)?;
    name.notify(InotifyMask::ATTRIB);
    Ok(())
}

fn maybe_uid(id: u32) -> Option<uid_t> {
    if id == u32::MAX {
        None
    } else {
        Some(id)
    }
}

pub fn sys_fchown(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    owner: u32,
    group: u32,
) -> Result<(), Errno> {
    let file = current_task.files.get_unless_opath(fd)?;
    file.name.entry.node.chown(
        current_task,
        &file.name.mount,
        maybe_uid(owner),
        maybe_uid(group),
    )?;
    file.notify(InotifyMask::ATTRIB);
    Ok(())
}

pub fn sys_fchownat(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    owner: u32,
    group: u32,
    flags: u32,
) -> Result<(), Errno> {
    let flags = LookupFlags::from_bits(flags, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)?;
    let name = lookup_at(current_task, dir_fd, user_path, flags)?;
    name.entry.node.chown(current_task, &name.mount, maybe_uid(owner), maybe_uid(group))?;
    name.notify(InotifyMask::ATTRIB);
    Ok(())
}

fn read_xattr_name(current_task: &CurrentTask, name_addr: UserCString) -> Result<Vec<u8>, Errno> {
    let name = current_task
        .mm
        .read_c_string_to_vec(name_addr, XATTR_NAME_MAX as usize + 1)
        .map_err(|e| if e == ENAMETOOLONG { errno!(ERANGE) } else { e })?;
    if name.is_empty() {
        return error!(ERANGE);
    }
    let dot_index = memchr::memchr(b'.', &name).ok_or_else(|| errno!(ENOTSUP))?;
    if name[dot_index + 1..].is_empty() {
        return error!(EINVAL);
    }
    match &name[..dot_index] {
        b"user" | b"security" | b"trusted" | b"system" => {}
        _ => return error!(ENOTSUP),
    }
    Ok(name)
}

fn do_getxattr(
    current_task: &CurrentTask,
    node: &NamespaceNode,
    name_addr: UserCString,
    value_addr: UserAddress,
    size: usize,
) -> Result<usize, Errno> {
    let name = read_xattr_name(current_task, name_addr)?;
    let value = match node.entry.node.get_xattr(current_task, &node.mount, &name, size)? {
        ValueOrSize::Size(s) => return Ok(s),
        ValueOrSize::Value(v) => v,
    };
    if size == 0 {
        return Ok(value.len());
    }
    if size < value.len() {
        return error!(ERANGE);
    }
    current_task.write_memory(value_addr, &value)
}

pub fn sys_getxattr(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    path_addr: UserCString,
    name_addr: UserCString,
    value_addr: UserAddress,
    size: usize,
) -> Result<usize, Errno> {
    let node = lookup_at(current_task, FdNumber::AT_FDCWD, path_addr, LookupFlags::default())?;
    do_getxattr(current_task, &node, name_addr, value_addr, size)
}

pub fn sys_fgetxattr(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    name_addr: UserCString,
    value_addr: UserAddress,
    size: usize,
) -> Result<usize, Errno> {
    let file = current_task.files.get_unless_opath(fd)?;
    do_getxattr(current_task, &file.name, name_addr, value_addr, size)
}

pub fn sys_lgetxattr(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    path_addr: UserCString,
    name_addr: UserCString,
    value_addr: UserAddress,
    size: usize,
) -> Result<usize, Errno> {
    let node = lookup_at(current_task, FdNumber::AT_FDCWD, path_addr, LookupFlags::no_follow())?;
    do_getxattr(current_task, &node, name_addr, value_addr, size)
}

fn do_setxattr(
    current_task: &CurrentTask,
    node: &NamespaceNode,
    name_addr: UserCString,
    value_addr: UserAddress,
    size: usize,
    flags: u32,
) -> Result<(), Errno> {
    if size > XATTR_NAME_MAX as usize {
        return error!(E2BIG);
    }
    let mode = node.entry.node.info().mode;
    if mode.is_chr() || mode.is_fifo() {
        return error!(EPERM);
    }

    let op = match flags {
        0 => XattrOp::Set,
        XATTR_CREATE => XattrOp::Create,
        XATTR_REPLACE => XattrOp::Replace,
        _ => return error!(EINVAL),
    };
    let name = read_xattr_name(current_task, name_addr)?;
    let value = current_task.read_memory_to_vec(value_addr, size)?;
    node.entry.node.set_xattr(current_task, &node.mount, &name, &value, op)
}

pub fn sys_fsetxattr(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    name_addr: UserCString,
    value_addr: UserAddress,
    size: usize,
    flags: u32,
) -> Result<(), Errno> {
    let file = current_task.files.get_unless_opath(fd)?;
    do_setxattr(current_task, &file.name, name_addr, value_addr, size, flags)
}

pub fn sys_lsetxattr(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    path_addr: UserCString,
    name_addr: UserCString,
    value_addr: UserAddress,
    size: usize,
    flags: u32,
) -> Result<(), Errno> {
    let node = lookup_at(current_task, FdNumber::AT_FDCWD, path_addr, LookupFlags::no_follow())?;
    do_setxattr(current_task, &node, name_addr, value_addr, size, flags)
}

pub fn sys_setxattr(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    path_addr: UserCString,
    name_addr: UserCString,
    value_addr: UserAddress,
    size: usize,
    flags: u32,
) -> Result<(), Errno> {
    let node = lookup_at(current_task, FdNumber::AT_FDCWD, path_addr, LookupFlags::default())?;
    do_setxattr(current_task, &node, name_addr, value_addr, size, flags)
}

fn do_removexattr(
    current_task: &CurrentTask,
    node: &NamespaceNode,
    name_addr: UserCString,
) -> Result<(), Errno> {
    let mode = node.entry.node.info().mode;
    if mode.is_chr() || mode.is_fifo() {
        return error!(EPERM);
    }
    let name = read_xattr_name(current_task, name_addr)?;
    node.entry.node.remove_xattr(current_task, &node.mount, &name)
}

pub fn sys_removexattr(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    path_addr: UserCString,
    name_addr: UserCString,
) -> Result<(), Errno> {
    let node = lookup_at(current_task, FdNumber::AT_FDCWD, path_addr, LookupFlags::default())?;
    do_removexattr(current_task, &node, name_addr)
}

pub fn sys_lremovexattr(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    path_addr: UserCString,
    name_addr: UserCString,
) -> Result<(), Errno> {
    let node = lookup_at(current_task, FdNumber::AT_FDCWD, path_addr, LookupFlags::no_follow())?;
    do_removexattr(current_task, &node, name_addr)
}

pub fn sys_fremovexattr(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    name_addr: UserCString,
) -> Result<(), Errno> {
    let file = current_task.files.get_unless_opath(fd)?;
    do_removexattr(current_task, &file.name, name_addr)
}

fn do_listxattr(
    current_task: &CurrentTask,
    node: &NamespaceNode,
    list_addr: UserAddress,
    size: usize,
) -> Result<usize, Errno> {
    let mut list = vec![];
    let xattrs = match node.entry.node.list_xattrs(current_task, size)? {
        ValueOrSize::Size(s) => return Ok(s),
        ValueOrSize::Value(v) => v,
    };
    for name in xattrs.iter() {
        list.extend_from_slice(name);
        list.push(b'\0');
    }
    if size == 0 {
        return Ok(list.len());
    }
    if size < list.len() {
        return error!(ERANGE);
    }
    current_task.write_memory(list_addr, &list)
}

pub fn sys_listxattr(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    path_addr: UserCString,
    list_addr: UserAddress,
    size: usize,
) -> Result<usize, Errno> {
    let node = lookup_at(current_task, FdNumber::AT_FDCWD, path_addr, LookupFlags::default())?;
    do_listxattr(current_task, &node, list_addr, size)
}

pub fn sys_llistxattr(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    path_addr: UserCString,
    list_addr: UserAddress,
    size: usize,
) -> Result<usize, Errno> {
    let node = lookup_at(current_task, FdNumber::AT_FDCWD, path_addr, LookupFlags::no_follow())?;
    do_listxattr(current_task, &node, list_addr, size)
}

pub fn sys_flistxattr(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    list_addr: UserAddress,
    size: usize,
) -> Result<usize, Errno> {
    let file = current_task.files.get_unless_opath(fd)?;
    do_listxattr(current_task, &file.name, list_addr, size)
}

pub fn sys_getcwd(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    buf: UserAddress,
    size: usize,
) -> Result<usize, Errno> {
    let root = current_task.fs().root();
    let cwd = current_task.fs().cwd();
    let mut user_cwd = match cwd.path_from_root(Some(&root)) {
        PathWithReachability::Reachable(path) => path,
        PathWithReachability::Unreachable(mut path) => {
            let mut combined = vec![];
            combined.extend_from_slice(b"(unreachable)");
            combined.append(&mut path);
            combined
        }
    };
    user_cwd.push(b'\0');
    if user_cwd.len() > size {
        return error!(ERANGE);
    }
    current_task.write_memory(buf, &user_cwd)?;
    Ok(user_cwd.len())
}

pub fn sys_umask(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    umask: FileMode,
) -> Result<FileMode, Errno> {
    Ok(current_task.fs().set_umask(umask))
}

fn get_fd_flags(flags: u32) -> FdFlags {
    if flags & O_CLOEXEC != 0 {
        FdFlags::CLOEXEC
    } else {
        FdFlags::empty()
    }
}

pub fn sys_pipe2(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    user_pipe: UserRef<FdNumber>,
    flags: u32,
) -> Result<(), Errno> {
    let supported_file_flags = OpenFlags::NONBLOCK | OpenFlags::DIRECT;
    if flags & !(O_CLOEXEC | supported_file_flags.bits()) != 0 {
        return error!(EINVAL);
    }
    let (read, write) = new_pipe(current_task)?;

    let file_flags = OpenFlags::from_bits_truncate(flags & supported_file_flags.bits());
    read.update_file_flags(file_flags, supported_file_flags);
    write.update_file_flags(file_flags, supported_file_flags);

    let fd_flags = get_fd_flags(flags);
    let fd_read = current_task.add_file(read, fd_flags)?;
    let fd_write = current_task.add_file(write, fd_flags)?;
    log_trace!("pipe2 -> [{:#x}, {:#x}]", fd_read.raw(), fd_write.raw());

    current_task.write_object(user_pipe, &fd_read)?;
    let user_pipe = user_pipe.next();
    current_task.write_object(user_pipe, &fd_write)?;

    Ok(())
}

pub fn sys_ioctl(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    request: u32,
    arg: SyscallArg,
) -> Result<SyscallResult, Errno> {
    let file = current_task.files.get(fd)?;
    file.ioctl(current_task, request, arg)
}

pub fn sys_symlinkat(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    user_target: UserCString,
    new_dir_fd: FdNumber,
    user_path: UserCString,
) -> Result<(), Errno> {
    let target = current_task.read_c_string_to_vec(user_target, PATH_MAX as usize)?;
    if target.is_empty() {
        return error!(ENOENT);
    }

    let path = current_task.read_c_string_to_vec(user_path, PATH_MAX as usize)?;
    // TODO: This check could probably be moved into parent.symlink(..).
    if path.is_empty() {
        return error!(ENOENT);
    }

    let res = lookup_parent_at(current_task, new_dir_fd, user_path, |context, parent, basename| {
        // The path to a new symlink cannot end in `/`. That would imply that we are dereferencing
        // the symlink to a directory.
        //
        // See https://pubs.opengroup.org/onlinepubs/9699919799/xrat/V4_xbd_chap03.html#tag_21_03_00_75
        if context.must_be_directory {
            return error!(ENOENT);
        }
        parent.create_symlink(current_task, basename, &target)
    });
    res?;
    Ok(())
}

pub fn sys_dup(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    oldfd: FdNumber,
) -> Result<FdNumber, Errno> {
    current_task.files.duplicate(current_task, oldfd, TargetFdNumber::Default, FdFlags::empty())
}

pub fn sys_dup3(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    oldfd: FdNumber,
    newfd: FdNumber,
    flags: u32,
) -> Result<FdNumber, Errno> {
    if oldfd == newfd {
        return error!(EINVAL);
    }
    if flags & !O_CLOEXEC != 0 {
        return error!(EINVAL);
    }
    let fd_flags = get_fd_flags(flags);
    current_task.files.duplicate(current_task, oldfd, TargetFdNumber::Specific(newfd), fd_flags)?;
    Ok(newfd)
}

/// A memfd file descriptor cannot have a name longer than 250 bytes, including
/// the null terminator.
///
/// See Errors section of https://man7.org/linux/man-pages/man2/memfd_create.2.html
const MEMFD_NAME_MAX_LEN: usize = 250;

pub fn sys_memfd_create(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    user_name: UserCString,
    flags: u32,
) -> Result<FdNumber, Errno> {
    const HUGE_SHIFTED_MASK: u32 = MFD_HUGE_MASK << MFD_HUGE_SHIFT;

    if flags & !(MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_HUGETLB | HUGE_SHIFTED_MASK) != 0 {
        return error!(EINVAL);
    }

    let _huge_page_size = if flags & MFD_HUGETLB != 0 {
        Some(flags & HUGE_SHIFTED_MASK)
    } else {
        if flags & HUGE_SHIFTED_MASK != 0 {
            return error!(EINVAL);
        }
        None
    };

    if flags & !(MFD_CLOEXEC | MFD_ALLOW_SEALING) != 0 {
        not_implemented!("memfd_create: flags: {}", flags);
    }

    let name = current_task
        .mm
        .read_c_string_to_vec(user_name, MEMFD_NAME_MAX_LEN)
        .map_err(|e| if e == ENAMETOOLONG { errno!(EINVAL) } else { e })?;

    let seals = if flags & MFD_ALLOW_SEALING != 0 {
        SealFlags::empty()
    } else {
        // Forbid sealing, by sealing the seal operation.
        SealFlags::SEAL
    };

    let file = new_memfd(current_task, name, seals, OpenFlags::RDWR)?;

    let mut fd_flags = FdFlags::empty();
    if flags & MFD_CLOEXEC != 0 {
        fd_flags |= FdFlags::CLOEXEC;
    }
    let fd = current_task.add_file(file, fd_flags)?;
    Ok(fd)
}

pub fn sys_mount(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    source_addr: UserCString,
    target_addr: UserCString,
    filesystemtype_addr: UserCString,
    flags: u32,
    data_addr: UserCString,
) -> Result<(), Errno> {
    if !current_task.creds().has_capability(CAP_SYS_ADMIN) {
        return error!(EPERM);
    }

    let flags = MountFlags::from_bits(flags).ok_or_else(|| {
        not_implemented!(
            "unsupported mount flags: {:#x}",
            flags & !MountFlags::from_bits_truncate(flags).bits()
        );
        errno!(EINVAL)
    })?;

    let target = lookup_at(current_task, FdNumber::AT_FDCWD, target_addr, LookupFlags::default())?;

    if flags.contains(MountFlags::REMOUNT) {
        do_mount_remount(target, flags, data_addr)
    } else if flags.contains(MountFlags::BIND) {
        do_mount_bind(current_task, source_addr, target, flags)
    } else if flags.intersects(MountFlags::SHARED | MountFlags::PRIVATE | MountFlags::DOWNSTREAM) {
        do_mount_change_propagation_type(current_task, target, flags)
    } else {
        do_mount_create(current_task, source_addr, target, filesystemtype_addr, data_addr, flags)
    }
}

fn do_mount_remount(
    target: NamespaceNode,
    flags: MountFlags,
    data_addr: UserCString,
) -> Result<(), Errno> {
    if !data_addr.is_null() {
        not_implemented!("mount: MS_REMOUNT: Updating data is not implemented.");
    }
    let mount = target.mount_if_root()?;
    let updated_flags = flags & MountFlags::CHANGEABLE_WITH_REMOUNT;
    mount.update_flags(updated_flags);
    if !flags.contains(MountFlags::BIND) {
        // From <https://man7.org/linux/man-pages/man2/mount.2.html>
        //
        //   Since Linux 2.6.26, the MS_REMOUNT flag can be used with MS_BIND
        //   to modify only the per-mount-point flags.  This is particularly
        //   useful for setting or clearing the "read-only" flag on a mount
        //   without changing the underlying filesystem.
        not_implemented!("mount: MS_REMOUNT: Updating superblock flags is not implemented.");
    }
    Ok(())
}

fn do_mount_bind(
    current_task: &CurrentTask,
    source_addr: UserCString,
    target: NamespaceNode,
    flags: MountFlags,
) -> Result<(), Errno> {
    let source = lookup_at(current_task, FdNumber::AT_FDCWD, source_addr, LookupFlags::default())?;
    log_trace!(
        "mount(source={:?}, target={:?}, flags={:?})",
        String::from_utf8_lossy(&source.path(current_task)),
        String::from_utf8_lossy(&target.path(current_task)),
        flags
    );
    target.mount(WhatToMount::Bind(source), flags)
}

fn do_mount_change_propagation_type(
    current_task: &CurrentTask,
    target: NamespaceNode,
    flags: MountFlags,
) -> Result<(), Errno> {
    log_trace!(
        "mount(target={:?}, flags={:?})",
        String::from_utf8_lossy(&target.path(current_task)),
        flags
    );

    // Flag validation. Of the three propagation type flags, exactly one must be passed. The only
    // valid flags other than propagation type are MS_SILENT and MS_REC.
    //
    // Use if statements to find the first propagation type flag, then check for valid flags using
    // only the first propagation flag and MS_REC / MS_SILENT as valid flags.
    let propagation_flag = if flags.contains(MountFlags::SHARED) {
        MountFlags::SHARED
    } else if flags.contains(MountFlags::PRIVATE) {
        MountFlags::PRIVATE
    } else if flags.contains(MountFlags::DOWNSTREAM) {
        MountFlags::DOWNSTREAM
    } else {
        return error!(EINVAL);
    };
    if flags.intersects(!(propagation_flag | MountFlags::REC | MountFlags::SILENT)) {
        return error!(EINVAL);
    }

    let mount = target.mount_if_root()?;
    mount.change_propagation(propagation_flag, flags.contains(MountFlags::REC));
    Ok(())
}

fn do_mount_create(
    current_task: &CurrentTask,
    source_addr: UserCString,
    target: NamespaceNode,
    filesystemtype_addr: UserCString,
    data_addr: UserCString,
    flags: MountFlags,
) -> Result<(), Errno> {
    let mut buf = [0u8; PATH_MAX as usize];
    let source = if source_addr.is_null() {
        b""
    } else {
        current_task.read_c_string_to_slice(source_addr, &mut buf)?
    };
    let mut buf = [0u8; PATH_MAX as usize];
    let fs_type = current_task.read_c_string_to_slice(filesystemtype_addr, &mut buf)?;
    let mut buf = [0u8; PATH_MAX as usize];
    let data = if data_addr.is_null() {
        b""
    } else {
        current_task.read_c_string_to_slice(data_addr, &mut buf)?
    };
    log_trace!(
        "mount(source={:?}, target={:?}, type={:?}, data={:?})",
        String::from_utf8_lossy(source),
        String::from_utf8_lossy(&target.path(current_task)),
        String::from_utf8_lossy(fs_type),
        String::from_utf8_lossy(data)
    );

    let options = FileSystemOptions {
        source: source.to_vec(),
        flags: flags & MountFlags::STORED_ON_FILESYSTEM,
        params: data.to_vec(),
    };

    let fs = current_task.create_filesystem(fs_type, options)?;
    target.mount(WhatToMount::Fs(fs), flags)
}

pub fn sys_umount2(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    target_addr: UserCString,
    flags: u32,
) -> Result<(), Errno> {
    if !current_task.creds().has_capability(CAP_SYS_ADMIN) {
        return error!(EPERM);
    }

    let flags = if flags & UMOUNT_NOFOLLOW != 0 {
        LookupFlags::no_follow()
    } else {
        LookupFlags::default()
    };
    let target = lookup_at(current_task, FdNumber::AT_FDCWD, target_addr, flags)?;
    target.unmount()
}

pub fn sys_eventfd2(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    value: u32,
    flags: u32,
) -> Result<FdNumber, Errno> {
    if flags & !(EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE) != 0 {
        return error!(EINVAL);
    }
    let blocking = (flags & EFD_NONBLOCK) == 0;
    let eventfd_type =
        if (flags & EFD_SEMAPHORE) == 0 { EventFdType::Counter } else { EventFdType::Semaphore };
    let file = new_eventfd(current_task, value, eventfd_type, blocking);
    let fd_flags = if flags & EFD_CLOEXEC != 0 { FdFlags::CLOEXEC } else { FdFlags::empty() };
    let fd = current_task.add_file(file, fd_flags)?;
    Ok(fd)
}

pub fn sys_pidfd_open(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    pid: pid_t,
    flags: u32,
) -> Result<FdNumber, Errno> {
    if flags & !PIDFD_NONBLOCK != 0 {
        return error!(EINVAL);
    }
    if pid <= 0 {
        return error!(EINVAL);
    }

    // Validate that the pid exists and that it belongs to a thread group leader.
    let task = current_task.get_task(pid);
    let task = Task::from_weak(&task)?;
    if !task.is_leader() {
        return error!(EINVAL);
    }

    let blocking = (flags & PIDFD_NONBLOCK) == 0;
    let open_flags = if blocking { OpenFlags::empty() } else { OpenFlags::NONBLOCK };
    let file = new_pidfd(current_task, pid, open_flags);
    current_task.add_file(file, FdFlags::CLOEXEC)
}

pub fn sys_pidfd_getfd(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    pidfd: FdNumber,
    targetfd: FdNumber,
    flags: u32,
) -> Result<FdNumber, Errno> {
    if flags != 0 {
        return error!(EINVAL);
    }

    let file = current_task.files.get(pidfd)?;
    let task = current_task.get_task(file.as_pid()?);
    let task = task.upgrade().ok_or_else(|| errno!(ESRCH))?;

    current_task.check_ptrace_access_mode(PTRACE_MODE_ATTACH_REALCREDS, &task)?;

    let target_file = task.files.get(targetfd)?;
    current_task.add_file(target_file, FdFlags::CLOEXEC)
}

pub fn sys_timerfd_create(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    clock_id: u32,
    flags: u32,
) -> Result<FdNumber, Errno> {
    let timer_file_clock = match clock_id {
        CLOCK_MONOTONIC | CLOCK_BOOTTIME => TimerFileClock::Monotonic,
        CLOCK_BOOTTIME_ALARM => {
            if !current_task.creds().has_capability(CAP_WAKE_ALARM) {
                return error!(EPERM);
            }
            // TODO(fxbug.dev/121415): Add proper support for _ALARM clocks.
            not_implemented!("timerfd_create: CLOCK_BOOTTIME_ALARM is mapped to CLOCK_BOOTTIME");
            TimerFileClock::Monotonic
        }
        CLOCK_REALTIME_ALARM => {
            if !current_task.creds().has_capability(CAP_WAKE_ALARM) {
                return error!(EPERM);
            }
            // TODO(fxbug.dev/121415): Add proper support for _ALARM clocks.
            not_implemented!("timerfd_create: CLOCK_REALTIME_ALARM is mapped to CLOCK_REALTIME");
            TimerFileClock::Realtime
        }
        CLOCK_REALTIME => TimerFileClock::Realtime,
        _ => return error!(EINVAL),
    };
    if flags & !(TFD_NONBLOCK | TFD_CLOEXEC) != 0 {
        not_implemented!("timerfd_create: flags {:#x}", flags);
        return error!(EINVAL);
    }
    log_trace!("timerfd_create(clock_id={:?}, flags={:#x})", clock_id, flags);

    let mut open_flags = OpenFlags::RDWR;
    if flags & TFD_NONBLOCK != 0 {
        open_flags |= OpenFlags::NONBLOCK;
    }

    let mut fd_flags = FdFlags::empty();
    if flags & TFD_CLOEXEC != 0 {
        fd_flags |= FdFlags::CLOEXEC;
    };

    let timer = TimerFile::new_file(current_task, timer_file_clock, open_flags)?;
    let fd = current_task.add_file(timer, fd_flags)?;
    Ok(fd)
}

pub fn sys_timerfd_gettime(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    user_current_value: UserRef<itimerspec>,
) -> Result<(), Errno> {
    let file = current_task.files.get(fd)?;
    let timer_file = file.downcast_file::<TimerFile>().ok_or_else(|| errno!(EINVAL))?;
    let timer_info = timer_file.current_timer_spec();
    log_trace!("timerfd_gettime(fd={:?}, current_value={:?})", fd, timer_info);
    current_task.write_object(user_current_value, &timer_info)?;
    Ok(())
}

pub fn sys_timerfd_settime(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    flags: u32,
    user_new_value: UserRef<itimerspec>,
    user_old_value: UserRef<itimerspec>,
) -> Result<(), Errno> {
    if flags & !(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET) != 0 {
        not_implemented!("timerfd_settime: flags {:#x}", flags);
        return error!(EINVAL);
    }

    if flags & TFD_TIMER_CANCEL_ON_SET != 0 {
        // TODO(fxbug.dev/121607): Respect the cancel on set.
        not_implemented!("timerfd_settime: TFD_TIMER_CANCEL_ON_SET accepted, but not implemented",);
    }

    let file = current_task.files.get(fd)?;
    let timer_file = file.downcast_file::<TimerFile>().ok_or_else(|| errno!(EINVAL))?;

    let new_timer_spec = current_task.read_object(user_new_value)?;
    let old_timer_spec = timer_file.set_timer_spec(new_timer_spec, flags)?;
    log_trace!(
        "timerfd_settime(fd={:?}, flags={:#x}, new_value={:?}, current_value={:?})",
        fd,
        flags,
        new_timer_spec,
        old_timer_spec
    );
    if !user_old_value.is_null() {
        current_task.write_object(user_old_value, &old_timer_spec)?;
    }
    Ok(())
}

fn select(
    current_task: &mut CurrentTask,
    nfds: u32,
    readfds_addr: UserRef<__kernel_fd_set>,
    writefds_addr: UserRef<__kernel_fd_set>,
    exceptfds_addr: UserRef<__kernel_fd_set>,
    deadline: zx::Time,
    sigmask_addr: UserRef<pselect6_sigmask>,
) -> Result<i32, Errno> {
    const BITS_PER_BYTE: usize = 8;

    fn sizeof<T>(_: &T) -> usize {
        BITS_PER_BYTE * std::mem::size_of::<T>()
    }
    fn is_fd_set(set: &__kernel_fd_set, fd: usize) -> bool {
        let index = fd / sizeof(&set.fds_bits[0]);
        let remainder = fd % sizeof(&set.fds_bits[0]);
        set.fds_bits[index] & (1 << remainder) > 0
    }
    fn add_fd_to_set(set: &mut __kernel_fd_set, fd: usize) {
        let index = fd / sizeof(&set.fds_bits[0]);
        let remainder = fd % sizeof(&set.fds_bits[0]);

        set.fds_bits[index] |= 1 << remainder;
    }
    let read_fd_set = |addr: UserRef<__kernel_fd_set>| {
        if addr.is_null() {
            Ok(Default::default())
        } else {
            current_task.read_object(addr)
        }
    };

    if nfds as usize >= BITS_PER_BYTE * std::mem::size_of::<__kernel_fd_set>() {
        return error!(EINVAL);
    }

    let read_events = POLLRDNORM | POLLRDBAND | POLLIN | POLLHUP | POLLERR;
    let write_events = POLLWRBAND | POLLWRNORM | POLLOUT | POLLERR;
    let except_events = POLLPRI;

    let readfds = read_fd_set(readfds_addr)?;
    let writefds = read_fd_set(writefds_addr)?;
    let exceptfds = read_fd_set(exceptfds_addr)?;

    let sets = &[(read_events, &readfds), (write_events, &writefds), (except_events, &exceptfds)];
    let waiter = FileWaiter::<FdNumber>::default();

    for fd in 0..nfds {
        let mut aggregated_events = 0;
        for (events, fds) in sets.iter() {
            if is_fd_set(fds, fd as usize) {
                aggregated_events |= events;
            }
        }
        if aggregated_events != 0 {
            let fd = FdNumber::from_raw(fd as i32);
            let file = current_task.files.get(fd)?;
            waiter.add(
                current_task,
                fd,
                Some(&file),
                FdEvents::from_bits_truncate(aggregated_events),
            )?;
        }
    }

    let mask = if !sigmask_addr.is_null() {
        let sigmask = current_task.read_object(sigmask_addr)?;
        let mask = if sigmask.ss.is_null() {
            current_task.read().signals.mask()
        } else {
            if sigmask.ss_len < std::mem::size_of::<sigset_t>() {
                return error!(EINVAL);
            }
            current_task.read_object(sigmask.ss.into())?
        };
        Some(mask)
    } else {
        None
    };

    waiter.wait(current_task, mask, deadline)?;

    let mut num_fds = 0;
    let mut readfds: __kernel_fd_set = Default::default();
    let mut writefds: __kernel_fd_set = Default::default();
    let mut exceptfds: __kernel_fd_set = Default::default();
    let mut sets = [
        (read_events, &mut readfds),
        (write_events, &mut writefds),
        (except_events, &mut exceptfds),
    ];

    let mut ready_items = waiter.ready_items.lock();
    for ReadyItem { key: ready_key, events: ready_events } in ready_items.drain(..) {
        let ready_key = assert_matches::assert_matches!(
            ready_key,
            ReadyItemKey::FdNumber(v) => v
        );

        sets.iter_mut().for_each(|entry| {
            let events = FdEvents::from_bits_truncate(entry.0);
            let fds: &mut __kernel_fd_set = entry.1;
            if events.intersects(ready_events) {
                add_fd_to_set(fds, ready_key.raw() as usize);
                num_fds += 1;
            }
        });
    }

    let write_fd_set =
        |addr: UserRef<__kernel_fd_set>, value: __kernel_fd_set| -> Result<(), Errno> {
            if !addr.is_null() {
                current_task.write_object(addr, &value)?;
            }
            Ok(())
        };
    write_fd_set(readfds_addr, readfds)?;
    write_fd_set(writefds_addr, writefds)?;
    write_fd_set(exceptfds_addr, exceptfds)?;
    Ok(num_fds)
}

pub fn sys_pselect6(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &mut CurrentTask,
    nfds: u32,
    readfds_addr: UserRef<__kernel_fd_set>,
    writefds_addr: UserRef<__kernel_fd_set>,
    exceptfds_addr: UserRef<__kernel_fd_set>,
    timeout_addr: UserRef<timespec>,
    sigmask_addr: UserRef<pselect6_sigmask>,
) -> Result<i32, Errno> {
    let start_time = zx::Time::get_monotonic();

    let deadline = if timeout_addr.is_null() {
        zx::Time::INFINITE
    } else {
        let timespec = current_task.read_object(timeout_addr)?;
        start_time + duration_from_timespec(timespec)?
    };

    let num_fds = select(
        current_task,
        nfds,
        readfds_addr,
        writefds_addr,
        exceptfds_addr,
        deadline,
        sigmask_addr,
    )?;

    if !timeout_addr.is_null()
        && !current_task.thread_group.read().personality.contains(PersonalityFlags::STICKY_TIMEOUTS)
    {
        let now = zx::Time::get_monotonic();
        let remaining = std::cmp::max(deadline - now, zx::Duration::from_seconds(0));
        current_task.write_object(timeout_addr, &timespec_from_duration(remaining))?;
    }

    Ok(num_fds)
}

#[cfg(target_arch = "x86_64")]
pub fn sys_select(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &mut CurrentTask,
    nfds: u32,
    readfds_addr: UserRef<__kernel_fd_set>,
    writefds_addr: UserRef<__kernel_fd_set>,
    exceptfds_addr: UserRef<__kernel_fd_set>,
    timeout_addr: UserRef<starnix_uapi::timeval>,
) -> Result<i32, Errno> {
    let start_time = zx::Time::get_monotonic();

    let deadline = if timeout_addr.is_null() {
        zx::Time::INFINITE
    } else {
        let timeval = current_task.read_object(timeout_addr)?;
        start_time + starnix_uapi::time::duration_from_timeval(timeval)?
    };

    let num_fds = select(
        current_task,
        nfds,
        readfds_addr,
        writefds_addr,
        exceptfds_addr,
        deadline,
        UserRef::<pselect6_sigmask>::default(),
    )?;

    if !timeout_addr.is_null()
        && !current_task.thread_group.read().personality.contains(PersonalityFlags::STICKY_TIMEOUTS)
    {
        let now = zx::Time::get_monotonic();
        let remaining = std::cmp::max(deadline - now, zx::Duration::from_seconds(0));
        current_task
            .write_object(timeout_addr, &starnix_uapi::time::timeval_from_duration(remaining))?;
    }

    Ok(num_fds)
}

pub fn sys_epoll_create1(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    flags: u32,
) -> Result<FdNumber, Errno> {
    if flags & !EPOLL_CLOEXEC != 0 {
        return error!(EINVAL);
    }
    let ep_file = EpollFileObject::new_file(current_task);
    let fd_flags = if flags & EPOLL_CLOEXEC != 0 { FdFlags::CLOEXEC } else { FdFlags::empty() };
    let fd = current_task.add_file(ep_file, fd_flags)?;
    Ok(fd)
}

pub fn sys_epoll_ctl(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    epfd: FdNumber,
    op: u32,
    fd: FdNumber,
    event: UserRef<epoll_event>,
) -> Result<(), Errno> {
    if epfd == fd {
        return error!(EINVAL);
    }

    let file = current_task.files.get(epfd)?;
    let epoll_file = file.downcast_file::<EpollFileObject>().ok_or_else(|| errno!(EINVAL))?;

    let ctl_file = current_task.files.get(fd)?;
    match op {
        EPOLL_CTL_ADD => {
            let epoll_event = current_task.read_object(event)?;
            epoll_file.add(current_task, &ctl_file, &file, epoll_event)?;
        }
        EPOLL_CTL_MOD => {
            let epoll_event = current_task.read_object(event)?;
            epoll_file.modify(current_task, &ctl_file, epoll_event)?;
        }
        EPOLL_CTL_DEL => epoll_file.delete(&ctl_file)?,
        _ => return error!(EINVAL),
    }
    Ok(())
}

// Backend for sys_epoll_pwait and sys_epoll_pwait2 that takes an already-decoded deadline.
fn do_epoll_pwait(
    current_task: &mut CurrentTask,
    epfd: FdNumber,
    events: UserRef<epoll_event>,
    unvalidated_max_events: i32,
    deadline: zx::Time,
    user_sigmask: UserRef<SigSet>,
) -> Result<usize, Errno> {
    let file = current_task.files.get(epfd)?;
    let epoll_file = file.downcast_file::<EpollFileObject>().ok_or_else(|| errno!(EINVAL))?;

    // Max_events must be greater than 0.
    let max_events: usize = unvalidated_max_events.try_into().map_err(|_| errno!(EINVAL))?;
    if max_events == 0 {
        return error!(EINVAL);
    }

    // Return early if the user passes an obviously invalid pointer. This avoids dropping events
    // for common pointer errors. When we catch bad pointers after the wait is complete when the
    // memory is actually written, the events will be lost. This check is not a guarantee.
    current_task
        .mm
        .check_plausible(events.addr(), max_events * std::mem::size_of::<epoll_event>())?;

    let active_events = if !user_sigmask.is_null() {
        let signal_mask = current_task.read_object(user_sigmask)?;
        current_task.wait_with_temporary_mask(signal_mask, |current_task| {
            epoll_file.wait(current_task, max_events, deadline)
        })?
    } else {
        epoll_file.wait(current_task, max_events, deadline)?
    };

    current_task.write_objects(events, &active_events)?;
    Ok(active_events.len())
}

pub fn sys_epoll_pwait(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &mut CurrentTask,
    epfd: FdNumber,
    events: UserRef<epoll_event>,
    max_events: i32,
    timeout: i32,
    user_sigmask: UserRef<SigSet>,
) -> Result<usize, Errno> {
    let deadline = zx::Time::after(duration_from_poll_timeout(timeout)?);
    do_epoll_pwait(current_task, epfd, events, max_events, deadline, user_sigmask)
}

pub fn sys_epoll_pwait2(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &mut CurrentTask,
    epfd: FdNumber,
    events: UserRef<epoll_event>,
    max_events: i32,
    user_timespec: UserRef<timespec>,
    user_sigmask: UserRef<SigSet>,
) -> Result<usize, Errno> {
    let deadline = if user_timespec.is_null() {
        zx::Time::INFINITE
    } else {
        let ts = current_task.read_object(user_timespec)?;
        zx::Time::after(duration_from_timespec(ts)?)
    };
    do_epoll_pwait(current_task, epfd, events, max_events, deadline, user_sigmask)
}

struct FileWaiter<Key: Into<ReadyItemKey>> {
    waiter: Waiter,
    ready_items: Arc<Mutex<VecDeque<ReadyItem>>>,
    _marker: PhantomData<Key>,
}

impl<Key: Into<ReadyItemKey>> Default for FileWaiter<Key> {
    fn default() -> Self {
        Self { waiter: Waiter::new(), ready_items: Default::default(), _marker: PhantomData }
    }
}

impl<Key: Into<ReadyItemKey>> FileWaiter<Key> {
    fn add(
        &self,
        current_task: &CurrentTask,
        key: Key,
        file: Option<&FileHandle>,
        requested_events: FdEvents,
    ) -> Result<(), Errno> {
        let key = key.into();

        if let Some(file) = file {
            let sought_events = requested_events | FdEvents::POLLERR | FdEvents::POLLHUP;

            let handler = EventHandler::Enqueue(EnqueueEventHandler {
                key,
                queue: self.ready_items.clone(),
                sought_events,
                mappings: Default::default(),
            });
            file.wait_async(current_task, &self.waiter, sought_events, handler);
            let current_events = file.query_events(current_task)? & sought_events;
            if !current_events.is_empty() {
                self.ready_items.lock().push_back(ReadyItem { key, events: current_events });
            }
        } else {
            self.ready_items.lock().push_back(ReadyItem { key, events: FdEvents::POLLNVAL });
        }
        Ok(())
    }

    fn wait(
        &self,
        current_task: &mut CurrentTask,
        signal_mask: Option<SigSet>,
        deadline: zx::Time,
    ) -> Result<(), Errno> {
        if self.ready_items.lock().is_empty() {
            // When wait_until() returns Ok() it means there was a wake up; however there may not
            // be a ready item, for example if waiting on a sync file with multiple sync points.
            // Keep waiting until there's at least one ready item.
            let signal_mask = signal_mask.unwrap_or_else(|| current_task.read().signals.mask());
            let mut result = current_task.wait_with_temporary_mask(signal_mask, |current_task| {
                self.waiter.wait_until(current_task, deadline)
            });
            loop {
                match result {
                    Err(err) if err == ETIMEDOUT => return Ok(()),
                    Ok(()) => {
                        if !self.ready_items.lock().is_empty() {
                            break;
                        }
                    }
                    result => result?,
                };
                result = self.waiter.wait_until(current_task, deadline);
            }
        }
        Ok(())
    }
}

pub fn poll(
    current_task: &mut CurrentTask,
    user_pollfds: UserRef<pollfd>,
    num_fds: i32,
    mask: Option<SigSet>,
    deadline: zx::Time,
) -> Result<usize, Errno> {
    if num_fds < 0 || num_fds as u64 > current_task.thread_group.get_rlimit(Resource::NOFILE) {
        return error!(EINVAL);
    }

    let mut pollfds = vec![pollfd::default(); num_fds as usize];
    let waiter = FileWaiter::<usize>::default();

    for (index, poll_descriptor) in pollfds.iter_mut().enumerate() {
        *poll_descriptor = current_task.read_object(user_pollfds.at(index))?;
        poll_descriptor.revents = 0;
        if poll_descriptor.fd < 0 {
            continue;
        }
        let file = current_task.files.get(FdNumber::from_raw(poll_descriptor.fd)).ok();
        waiter.add(
            current_task,
            index,
            file.as_ref(),
            FdEvents::from_bits_truncate(poll_descriptor.events as u32),
        )?;
    }

    waiter.wait(current_task, mask, deadline)?;

    let mut ready_items = waiter.ready_items.lock();
    let mut unique_ready_items =
        bit_vec::BitVec::from_elem(usize::try_from(num_fds).unwrap(), false);
    for ReadyItem { key: ready_key, events: ready_events } in ready_items.drain(..) {
        let ready_key = assert_matches::assert_matches!(
            ready_key,
            ReadyItemKey::Usize(v) => v
        );
        let interested_events = FdEvents::from_bits_truncate(pollfds[ready_key].events as u32)
            | FdEvents::POLLERR
            | FdEvents::POLLHUP
            | FdEvents::POLLNVAL;
        let return_events = (interested_events & ready_events).bits();
        pollfds[ready_key].revents = return_events as i16;
        unique_ready_items.set(ready_key, true);
    }

    for (index, poll_descriptor) in pollfds.iter().enumerate() {
        current_task.write_object(user_pollfds.at(index), poll_descriptor)?;
    }

    Ok(unique_ready_items.into_iter().filter(Clone::clone).count())
}

pub fn sys_ppoll(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &mut CurrentTask,
    user_fds: UserRef<pollfd>,
    num_fds: i32,
    user_timespec: UserRef<timespec>,
    user_mask: UserRef<SigSet>,
    sigset_size: usize,
) -> Result<usize, Errno> {
    let start_time = zx::Time::get_monotonic();

    let timeout = if user_timespec.is_null() {
        // Passing -1 to poll is equivalent to an infinite timeout.
        -1
    } else {
        let ts = current_task.read_object(user_timespec)?;
        duration_from_timespec(ts)?.into_millis() as i32
    };

    let deadline = start_time + duration_from_poll_timeout(timeout)?;

    let mask = if !user_mask.is_null() {
        if sigset_size != std::mem::size_of::<SigSet>() {
            return error!(EINVAL);
        }
        let mask = current_task.read_object(user_mask)?;
        Some(mask)
    } else {
        None
    };

    let poll_result = poll(current_task, user_fds, num_fds, mask, deadline);

    if user_timespec.is_null() {
        return poll_result;
    }

    let now = zx::Time::get_monotonic();
    let remaining = std::cmp::max(deadline - now, zx::Duration::from_seconds(0));
    let remaining_timespec = timespec_from_duration(remaining);

    // From gVisor: "ppoll is normally restartable if interrupted by something other than a signal
    // handled by the application (i.e. returns ERESTARTNOHAND). However, if
    // [copy out] failed, then the restarted ppoll would use the wrong timeout, so the
    // error should be left as EINTR."
    match (current_task.write_object(user_timespec, &remaining_timespec), poll_result) {
        // If write was ok, and poll was ok, return poll result.
        (Ok(_), Ok(num_events)) => Ok(num_events),
        // TODO: Here we should return an error that indicates the syscall should return EINTR if
        // interrupted by a signal with a user handler, and otherwise be restarted.
        (Ok(_), Err(e)) if e == EINTR => error!(EINTR),
        (Ok(_), poll_result) => poll_result,
        // If write was a failure, return the poll result unchanged.
        (Err(_), poll_result) => poll_result,
    }
}

pub fn sys_flock(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    operation: u32,
) -> Result<(), Errno> {
    let file = current_task.files.get(fd)?;
    let operation = FlockOperation::from_flags(operation)?;
    file.flock(current_task, operation)
}

pub fn sys_sync(
    _locked: &mut Locked<'_, Unlocked>,
    _current_task: &CurrentTask,
) -> Result<(), Errno> {
    not_implemented!("sync");
    Ok(())
}

pub fn sys_syncfs(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
) -> Result<(), Errno> {
    let _file = current_task.files.get_unless_opath(fd)?;
    not_implemented!("syncfs");
    Ok(())
}

pub fn sys_fsync(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
) -> Result<(), Errno> {
    let file = current_task.files.get(fd)?;
    file.sync(current_task)
}

pub fn sys_fdatasync(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
) -> Result<(), Errno> {
    let file = current_task.files.get(fd)?;
    file.data_sync(current_task)
}

pub fn sys_fadvise64(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    offset: off_t,
    len: off_t,
    advice: u32,
) -> Result<(), Errno> {
    // TODO(fxbug.dev/125680): Implement fadvise.
    match advice {
        POSIX_FADV_NORMAL
        | POSIX_FADV_RANDOM
        | POSIX_FADV_SEQUENTIAL
        | POSIX_FADV_WILLNEED
        | POSIX_FADV_DONTNEED
        | POSIX_FADV_NOREUSE => (),
        _ => return error!(EINVAL),
    }

    if offset < 0 || len < 0 {
        return error!(EINVAL);
    }

    let file = current_task.files.get(fd)?;
    // fadvise does not work on pipes.
    if file.downcast_file::<PipeFileObject>().is_some() {
        return error!(ESPIPE);
    }

    // fadvise does not work on paths.
    if file.flags().contains(OpenFlags::PATH) {
        return error!(EBADF);
    }

    Ok(())
}

pub fn sys_fallocate(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    mode: u32,
    offset: off_t,
    len: off_t,
) -> Result<(), Errno> {
    let file = current_task.files.get(fd)?;

    // Offset must not be less than 0.
    // Length must not be less than or equal to 0.
    // See https://man7.org/linux/man-pages/man2/fallocate.2.html#ERRORS
    if offset < 0 || len <= 0 {
        return error!(EINVAL);
    }

    let mode = FallocMode::from_bits(mode).ok_or_else(|| errno!(EINVAL))?;
    file.fallocate(current_task, mode, offset as u64, len as u64)?;

    Ok(())
}

pub fn sys_inotify_init1(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    flags: u32,
) -> Result<FdNumber, Errno> {
    if flags & !(IN_NONBLOCK | IN_CLOEXEC) != 0 {
        return error!(EINVAL);
    }
    let non_blocking = flags & IN_NONBLOCK != 0;
    let close_on_exec = flags & IN_CLOEXEC != 0;
    let inotify_file = InotifyFileObject::new_file(current_task, non_blocking);
    let fd_flags = if close_on_exec { FdFlags::CLOEXEC } else { FdFlags::empty() };
    current_task.add_file(inotify_file, fd_flags)
}

pub fn sys_inotify_add_watch(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    user_path: UserCString,
    mask: u32,
) -> Result<WdNumber, Errno> {
    let mask = InotifyMask::from_bits(mask).ok_or_else(|| errno!(EINVAL))?;
    if !mask.intersects(InotifyMask::ALL_EVENTS) {
        // Mask must include at least 1 event.
        return error!(EINVAL);
    }
    let file = current_task.files.get(fd)?;
    let inotify_file = file.downcast_file::<InotifyFileObject>().ok_or_else(|| errno!(EINVAL))?;
    let watched_node =
        lookup_at(current_task, FdNumber::AT_FDCWD, user_path, LookupFlags::default())?;
    inotify_file.add_watch(watched_node.entry, mask, &file)
}

pub fn sys_inotify_rm_watch(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd: FdNumber,
    watch_id: WdNumber,
) -> Result<(), Errno> {
    let file = current_task.files.get(fd)?;
    let inotify_file = file.downcast_file::<InotifyFileObject>().ok_or_else(|| errno!(EINVAL))?;
    inotify_file.remove_watch(watch_id, &file)
}

pub fn sys_utimensat(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    dir_fd: FdNumber,
    user_path: UserCString,
    user_times: UserRef<[timespec; 2]>,
    flags: u32,
) -> Result<(), Errno> {
    let (atime, mtime) = if user_times.addr().is_null() {
        // If user_times is null, the timestamps are updated to the current time.
        (TimeUpdateType::Now, TimeUpdateType::Now)
    } else {
        let ts: [timespec; 2] = current_task.read_object(user_times)?;
        let atime = ts[0];
        let mtime = ts[1];
        let parse_timespec = |spec: timespec| match spec.tv_nsec {
            UTIME_NOW => Ok(TimeUpdateType::Now),
            UTIME_OMIT => Ok(TimeUpdateType::Omit),
            _ => time_from_timespec(spec).map(TimeUpdateType::Time),
        };
        (parse_timespec(atime)?, parse_timespec(mtime)?)
    };

    if let (TimeUpdateType::Omit, TimeUpdateType::Omit) = (atime, mtime) {
        return Ok(());
    };

    // Non-standard feature: if user_path is null, the timestamps are updated on the file referred
    // to by dir_fd.
    // See https://man7.org/linux/man-pages/man2/utimensat.2.html
    let name = if user_path.addr().is_null() {
        if dir_fd == FdNumber::AT_FDCWD {
            return error!(EFAULT);
        }
        let (node, _) = current_task.resolve_dir_fd(dir_fd, b"")?;
        node
    } else {
        let lookup_flags = LookupFlags::from_bits(flags, AT_SYMLINK_NOFOLLOW)?;
        lookup_at(current_task, dir_fd, user_path, lookup_flags)?
    };
    name.entry.node.update_atime_mtime(current_task, &name.mount, atime, mtime)
}

pub fn sys_splice(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    fd_in: FdNumber,
    off_in: UserRef<off_t>,
    fd_out: FdNumber,
    off_out: UserRef<off_t>,
    len: usize,
    flags: u32,
) -> Result<usize, Errno> {
    splice::splice(current_task, fd_in, off_in, fd_out, off_out, len, flags)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{mm::PAGE_SIZE, testing::*};
    use starnix_uapi::{O_RDONLY, SEEK_CUR, SEEK_END, SEEK_SET};
    use std::sync::Arc;

    #[::fuchsia::test]
    async fn test_sys_lseek() -> Result<(), Errno> {
        let (_kernel, current_task, mut locked) = create_kernel_task_and_unlocked_with_pkgfs();
        let fd = FdNumber::from_raw(10);
        let file_handle = current_task.open_file(b"data/testfile.txt", OpenFlags::RDONLY)?;
        let file_size = file_handle.node().stat(&current_task).unwrap().st_size;
        current_task.files.insert(&current_task, fd, file_handle).unwrap();

        assert_eq!(sys_lseek(&mut locked, &current_task, fd, 0, SEEK_CUR)?, 0);
        assert_eq!(sys_lseek(&mut locked, &current_task, fd, 1, SEEK_CUR)?, 1);
        assert_eq!(sys_lseek(&mut locked, &current_task, fd, 3, SEEK_SET)?, 3);
        assert_eq!(sys_lseek(&mut locked, &current_task, fd, -3, SEEK_CUR)?, 0);
        assert_eq!(sys_lseek(&mut locked, &current_task, fd, 0, SEEK_END)?, file_size);
        assert_eq!(sys_lseek(&mut locked, &current_task, fd, -5, SEEK_SET), error!(EINVAL));

        // Make sure that the failed call above did not change the offset.
        assert_eq!(sys_lseek(&mut locked, &current_task, fd, 0, SEEK_CUR)?, file_size);

        // Prepare for an overflow.
        assert_eq!(sys_lseek(&mut locked, &current_task, fd, 3, SEEK_SET)?, 3);

        // Check for overflow.
        assert_eq!(sys_lseek(&mut locked, &current_task, fd, i64::MAX, SEEK_CUR), error!(EINVAL));

        Ok(())
    }

    #[::fuchsia::test]
    async fn test_sys_dup() -> Result<(), Errno> {
        let (_kernel, current_task, mut locked) = create_kernel_task_and_unlocked_with_pkgfs();
        let file_handle = current_task.open_file(b"data/testfile.txt", OpenFlags::RDONLY)?;
        let oldfd = current_task.add_file(file_handle, FdFlags::empty())?;
        let newfd = sys_dup(&mut locked, &current_task, oldfd)?;

        assert_ne!(oldfd, newfd);
        let files = &current_task.files;
        assert!(Arc::ptr_eq(&files.get(oldfd).unwrap(), &files.get(newfd).unwrap()));

        assert_eq!(sys_dup(&mut locked, &current_task, FdNumber::from_raw(3)), error!(EBADF));

        Ok(())
    }

    #[::fuchsia::test]
    async fn test_sys_dup3() -> Result<(), Errno> {
        let (_kernel, current_task, mut locked) = create_kernel_task_and_unlocked_with_pkgfs();
        let file_handle = current_task.open_file(b"data/testfile.txt", OpenFlags::RDONLY)?;
        let oldfd = current_task.add_file(file_handle, FdFlags::empty())?;
        let newfd = FdNumber::from_raw(2);
        sys_dup3(&mut locked, &current_task, oldfd, newfd, O_CLOEXEC)?;

        assert_ne!(oldfd, newfd);
        let files = &current_task.files;
        assert!(Arc::ptr_eq(&files.get(oldfd).unwrap(), &files.get(newfd).unwrap()));
        assert_eq!(files.get_fd_flags(oldfd).unwrap(), FdFlags::empty());
        assert_eq!(files.get_fd_flags(newfd).unwrap(), FdFlags::CLOEXEC);

        assert_eq!(sys_dup3(&mut locked, &current_task, oldfd, oldfd, O_CLOEXEC), error!(EINVAL));

        // Pass invalid flags.
        let invalid_flags = 1234;
        assert_eq!(
            sys_dup3(&mut locked, &current_task, oldfd, newfd, invalid_flags),
            error!(EINVAL)
        );

        // Makes sure that dup closes the old file handle before the fd points
        // to the new file handle.
        let second_file_handle = current_task.open_file(b"data/testfile.txt", OpenFlags::RDONLY)?;
        let different_file_fd = current_task.add_file(second_file_handle, FdFlags::empty())?;
        assert!(!Arc::ptr_eq(&files.get(oldfd).unwrap(), &files.get(different_file_fd).unwrap()));
        sys_dup3(&mut locked, &current_task, oldfd, different_file_fd, O_CLOEXEC)?;
        assert!(Arc::ptr_eq(&files.get(oldfd).unwrap(), &files.get(different_file_fd).unwrap()));

        Ok(())
    }

    #[::fuchsia::test]
    async fn test_sys_open_cloexec() -> Result<(), Errno> {
        let (_kernel, current_task, mut locked) = create_kernel_task_and_unlocked_with_pkgfs();
        let path_addr = map_memory(&current_task, UserAddress::default(), *PAGE_SIZE);
        let path = b"data/testfile.txt\0";
        current_task.write_memory(path_addr, path)?;
        let fd = sys_openat(
            &mut locked,
            &current_task,
            FdNumber::AT_FDCWD,
            UserCString::new(path_addr),
            O_RDONLY | O_CLOEXEC,
            FileMode::default(),
        )?;
        assert!(current_task.files.get_fd_flags(fd)?.contains(FdFlags::CLOEXEC));
        Ok(())
    }

    #[::fuchsia::test]
    async fn test_sys_epoll() -> Result<(), Errno> {
        let (_kernel, current_task, mut locked) = create_kernel_task_and_unlocked_with_pkgfs();

        let epoll_fd =
            sys_epoll_create1(&mut locked, &current_task, 0).expect("sys_epoll_create1 failed");
        sys_close(&mut locked, &current_task, epoll_fd).expect("sys_close failed");

        Ok(())
    }

    #[::fuchsia::test]
    async fn test_fstat_tmp_file() {
        let (_kernel, current_task, mut locked) = create_kernel_task_and_unlocked_with_pkgfs();

        // Create the file that will be used to stat.
        let file_path = b"data/testfile.txt";
        let _file_handle = current_task.open_file(file_path, OpenFlags::RDONLY).unwrap();

        // Write the path to user memory.
        let path_addr = map_memory(&current_task, UserAddress::default(), *PAGE_SIZE);
        current_task.write_memory(path_addr, file_path).expect("failed to clear struct");

        let user_stat = UserRef::new(path_addr + file_path.len());
        current_task
            .mm
            .write_object(user_stat, &statfs::default(0))
            .expect("failed to clear struct");

        let user_path = UserCString::new(path_addr);

        assert_eq!(sys_statfs(&mut locked, &current_task, user_path, user_stat), Ok(()));

        let returned_stat = current_task.read_object(user_stat).expect("failed to read struct");
        assert_eq!(returned_stat, statfs::default(u32::from_be_bytes(*b"f.io")));
    }

    #[::fuchsia::test]
    async fn test_unlinkat_dir() {
        let (_kernel, current_task, mut locked) = create_kernel_task_and_unlocked();

        // Create the dir that we will attempt to unlink later.
        let no_slash_path = b"testdir";
        let no_slash_path_addr = map_memory(&current_task, UserAddress::default(), *PAGE_SIZE);
        current_task
            .mm
            .write_memory(no_slash_path_addr, no_slash_path)
            .expect("failed to write path");
        let no_slash_user_path = UserCString::new(no_slash_path_addr);
        sys_mkdirat(
            &mut locked,
            &current_task,
            FdNumber::AT_FDCWD,
            no_slash_user_path,
            FileMode::ALLOW_ALL.with_type(FileMode::IFDIR),
        )
        .unwrap();

        let slash_path = b"testdir/";
        let slash_path_addr = map_memory(&current_task, UserAddress::default(), *PAGE_SIZE);
        current_task.write_memory(slash_path_addr, slash_path).expect("failed to write path");
        let slash_user_path = UserCString::new(slash_path_addr);

        // Try to remove a directory without specifying AT_REMOVEDIR.
        // This should fail with EISDIR, irrespective of the terminating slash.
        let error =
            sys_unlinkat(&mut locked, &current_task, FdNumber::AT_FDCWD, slash_user_path, 0)
                .unwrap_err();
        assert_eq!(error, errno!(EISDIR));
        let error =
            sys_unlinkat(&mut locked, &current_task, FdNumber::AT_FDCWD, no_slash_user_path, 0)
                .unwrap_err();
        assert_eq!(error, errno!(EISDIR));

        // Success with AT_REMOVEDIR.
        sys_unlinkat(&mut locked, &current_task, FdNumber::AT_FDCWD, slash_user_path, AT_REMOVEDIR)
            .unwrap();
    }

    #[::fuchsia::test]
    async fn test_rename_noreplace() {
        let (_kernel, current_task, mut locked) = create_kernel_task_and_unlocked_with_pkgfs();

        // Create the file that will be renamed.
        let old_user_path = b"data/testfile.txt";
        let _old_file_handle = current_task.open_file(old_user_path, OpenFlags::RDONLY).unwrap();

        // Write the path to user memory.
        let old_path_addr = map_memory(&current_task, UserAddress::default(), *PAGE_SIZE);
        current_task.write_memory(old_path_addr, old_user_path).expect("failed to clear struct");

        // Create a second file that we will attempt to rename to.
        let new_user_path = b"data/testfile2.txt";
        let _new_file_handle = current_task.open_file(new_user_path, OpenFlags::RDONLY).unwrap();

        // Write the path to user memory.
        let new_path_addr = map_memory(&current_task, UserAddress::default(), *PAGE_SIZE);
        current_task.write_memory(new_path_addr, new_user_path).expect("failed to clear struct");

        // Try to rename first file to second file's name with RENAME_NOREPLACE flag.
        // This should fail with EEXIST.
        let error = sys_renameat2(
            &mut locked,
            &current_task,
            FdNumber::AT_FDCWD,
            UserCString::new(old_path_addr),
            FdNumber::AT_FDCWD,
            UserCString::new(new_path_addr),
            RenameFlags::NOREPLACE.bits(),
        )
        .unwrap_err();
        assert_eq!(error, errno!(EEXIST));
    }
}
