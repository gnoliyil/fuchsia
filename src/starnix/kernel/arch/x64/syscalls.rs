// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::fs::{
    syscalls::{
        poll, sys_dup3, sys_epoll_create1, sys_epoll_pwait, sys_eventfd2, sys_faccessat,
        sys_fchmodat, sys_inotify_init1, sys_mkdirat, sys_mknodat, sys_newfstatat, sys_openat,
        sys_pipe2, sys_readlinkat, sys_renameat, sys_symlinkat, sys_unlinkat,
    },
    DirentSink, DirentSink32, EpollEvent, FdNumber,
};
use crate::task::{syscalls::do_clone, CurrentTask};
use crate::types::{
    clone_args, error, pid_t, pollfd, stat_t, DeviceType, Errno, FileMode, OpenFlags, SigSet,
    UserAddress, UserCString, UserRef, AT_REMOVEDIR, AT_SYMLINK_NOFOLLOW, CSIGNAL,
};

pub fn sys_access(
    current_task: &CurrentTask,
    user_path: UserCString,
    mode: u32,
) -> Result<(), Errno> {
    sys_faccessat(current_task, FdNumber::AT_FDCWD, user_path, mode)
}

pub fn sys_chmod(
    current_task: &CurrentTask,
    user_path: UserCString,
    mode: FileMode,
) -> Result<(), Errno> {
    sys_fchmodat(current_task, FdNumber::AT_FDCWD, user_path, mode)
}

/// The parameter order for `clone` varies by architecture.
pub fn sys_clone(
    current_task: &CurrentTask,
    flags: u64,
    user_stack: UserAddress,
    user_parent_tid: UserRef<pid_t>,
    user_child_tid: UserRef<pid_t>,
    user_tls: UserAddress,
) -> Result<pid_t, Errno> {
    // Our flags parameter uses the low 8 bits (CSIGNAL mask) of flags to indicate the exit
    // signal. The CloneArgs struct separates these as `flags` and `exit_signal`.
    do_clone(
        current_task,
        &clone_args {
            flags: flags & !(CSIGNAL as u64),
            child_tid: user_child_tid.addr().ptr() as u64,
            parent_tid: user_parent_tid.addr().ptr() as u64,
            exit_signal: flags & (CSIGNAL as u64),
            stack: user_stack.ptr() as u64,
            tls: user_tls.ptr() as u64,
            ..Default::default()
        },
    )
}

// https://pubs.opengroup.org/onlinepubs/9699919799/functions/creat.html
pub fn sys_creat(
    current_task: &CurrentTask,
    user_path: UserCString,
    mode: FileMode,
) -> Result<FdNumber, Errno> {
    sys_open(
        current_task,
        user_path,
        (OpenFlags::WRONLY | OpenFlags::CREAT | OpenFlags::TRUNC).bits(),
        mode,
    )
}

pub fn sys_dup2(
    current_task: &CurrentTask,
    oldfd: FdNumber,
    newfd: FdNumber,
) -> Result<FdNumber, Errno> {
    if oldfd == newfd {
        current_task.files.get(oldfd)?;
        return Ok(newfd);
    }
    sys_dup3(current_task, oldfd, newfd, 0)
}

pub fn sys_epoll_create(current_task: &CurrentTask, size: i32) -> Result<FdNumber, Errno> {
    if size < 1 {
        // The man page for epoll_create says the size was used in a previous implementation as
        // a hint but no longer does anything. But it's still required to be >= 1 to ensure
        // programs are backwards-compatible.
        return error!(EINVAL);
    }
    sys_epoll_create1(current_task, 0)
}

pub fn sys_epoll_wait(
    current_task: &mut CurrentTask,
    epfd: FdNumber,
    events: UserRef<EpollEvent>,
    max_events: i32,
    timeout: i32,
) -> Result<usize, Errno> {
    sys_epoll_pwait(current_task, epfd, events, max_events, timeout, UserRef::<SigSet>::default())
}

pub fn sys_eventfd(current_task: &CurrentTask, value: u32) -> Result<FdNumber, Errno> {
    sys_eventfd2(current_task, value, 0)
}

pub fn sys_getdents(
    current_task: &CurrentTask,
    fd: FdNumber,
    user_buffer: UserAddress,
    user_capacity: usize,
) -> Result<usize, Errno> {
    let file = current_task.files.get(fd)?;
    let mut offset = file.offset.lock();
    let mut sink = DirentSink32::new(current_task, &mut offset, user_buffer, user_capacity);
    file.readdir(current_task, &mut sink)?;
    Ok(sink.actual())
}

pub fn sys_inotify_init(current_task: &CurrentTask) -> Result<FdNumber, Errno> {
    sys_inotify_init1(current_task, 0)
}

pub fn sys_lstat(
    current_task: &CurrentTask,
    user_path: UserCString,
    buffer: UserRef<stat_t>,
) -> Result<(), Errno> {
    // TODO(fxbug.dev/91430): Add the `AT_NO_AUTOMOUNT` flag once it is supported in
    // `sys_newfstatat`.
    sys_newfstatat(current_task, FdNumber::AT_FDCWD, user_path, buffer, AT_SYMLINK_NOFOLLOW)
}

pub fn sys_mkdir(
    current_task: &CurrentTask,
    user_path: UserCString,
    mode: FileMode,
) -> Result<(), Errno> {
    sys_mkdirat(current_task, FdNumber::AT_FDCWD, user_path, mode)
}

pub fn sys_mknod(
    current_task: &CurrentTask,
    user_path: UserCString,
    mode: FileMode,
    dev: DeviceType,
) -> Result<(), Errno> {
    sys_mknodat(current_task, FdNumber::AT_FDCWD, user_path, mode, dev)
}

pub fn sys_open(
    current_task: &CurrentTask,
    user_path: UserCString,
    flags: u32,
    mode: FileMode,
) -> Result<FdNumber, Errno> {
    sys_openat(current_task, FdNumber::AT_FDCWD, user_path, flags, mode)
}

pub fn sys_pipe(current_task: &CurrentTask, user_pipe: UserRef<FdNumber>) -> Result<(), Errno> {
    sys_pipe2(current_task, user_pipe, 0)
}

pub fn sys_poll(
    current_task: &mut CurrentTask,
    user_fds: UserRef<pollfd>,
    num_fds: i32,
    timeout: i32,
) -> Result<usize, Errno> {
    poll(current_task, user_fds, num_fds, None, timeout)
}

pub fn sys_readlink(
    current_task: &CurrentTask,
    user_path: UserCString,
    buffer: UserAddress,
    buffer_size: usize,
) -> Result<usize, Errno> {
    sys_readlinkat(current_task, FdNumber::AT_FDCWD, user_path, buffer, buffer_size)
}

pub fn sys_rmdir(current_task: &CurrentTask, user_path: UserCString) -> Result<(), Errno> {
    sys_unlinkat(current_task, FdNumber::AT_FDCWD, user_path, AT_REMOVEDIR)
}

pub fn sys_rename(
    current_task: &CurrentTask,
    old_user_path: UserCString,
    new_user_path: UserCString,
) -> Result<(), Errno> {
    sys_renameat(current_task, FdNumber::AT_FDCWD, old_user_path, FdNumber::AT_FDCWD, new_user_path)
}

pub fn sys_stat(
    current_task: &CurrentTask,
    user_path: UserCString,
    buffer: UserRef<stat_t>,
) -> Result<(), Errno> {
    // TODO(fxbug.dev/91430): Add the `AT_NO_AUTOMOUNT` flag once it is supported in
    // `sys_newfstatat`.
    sys_newfstatat(current_task, FdNumber::AT_FDCWD, user_path, buffer, 0)
}

// https://man7.org/linux/man-pages/man2/symlink.2.html
pub fn sys_symlink(
    current_task: &CurrentTask,
    user_target: UserCString,
    user_path: UserCString,
) -> Result<(), Errno> {
    sys_symlinkat(current_task, user_target, FdNumber::AT_FDCWD, user_path)
}

pub fn sys_unlink(current_task: &CurrentTask, user_path: UserCString) -> Result<(), Errno> {
    sys_unlinkat(current_task, FdNumber::AT_FDCWD, user_path, 0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fs::FdFlags;
    use crate::mm::{MemoryAccessor, PAGE_SIZE};
    use crate::testing::*;

    #[::fuchsia::test]
    fn test_sys_dup2() {
        // Most tests are handled by test_sys_dup3, only test the case where both fds are equals.
        let (_kernel, current_task) = create_kernel_and_task_with_pkgfs();
        let fd = FdNumber::from_raw(42);
        assert_eq!(sys_dup2(&current_task, fd, fd), error!(EBADF));
        let file_handle =
            current_task.open_file(b"data/testfile.txt", OpenFlags::RDONLY).expect("open_file");
        let fd = current_task.add_file(file_handle, FdFlags::empty()).expect("add");
        assert_eq!(sys_dup2(&current_task, fd, fd), Ok(fd));
    }

    #[::fuchsia::test]
    fn test_sys_creat() -> Result<(), Errno> {
        let (_kernel, current_task) = create_kernel_and_task();
        let path_addr = map_memory(&current_task, UserAddress::default(), *PAGE_SIZE);
        let path = b"newfile.txt";
        current_task.mm.write_memory(path_addr, path)?;
        let fd = sys_creat(&current_task, UserCString::new(path_addr), FileMode::default())?;
        let _file_handle = current_task.open_file(path, OpenFlags::RDONLY)?;
        assert!(!current_task.files.get_fd_flags(fd)?.contains(FdFlags::CLOEXEC));
        Ok(())
    }
}
