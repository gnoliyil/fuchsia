// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use paste::paste;

use crate::arch::syscalls::*;
use crate::fs::FdNumber;
use crate::syscalls::{decls::Syscall, CurrentTask, SyscallResult};
use crate::types::*;

macro_rules! syscall_match {
    {
        $current_task:ident; $syscall_number:expr; $args:ident;
        $($(#[$match:meta])? $call:ident [$num_args:tt],)*
    } => {
        paste! {
            match $syscall_number as u32 {
                $(
                    $(#[$match])?
                    crate::types::[<__NR_ $call>] => {
                        match syscall_match!(@call $current_task; $args; [<sys_ $call>][$num_args]) {
                            Ok(x) => Ok(SyscallResult::from(x)),
                            Err(err) => Err(err),
                        }
                    },
                )*
                _ => sys_unknown($current_task, $syscall_number),
            }
        }
    };

    (@call $current_task:ident; $args:ident; $func:ident [0]) => ($func($current_task));
    (@call $current_task:ident; $args:ident; $func:ident [1]) => ($func($current_task, $args.0.into_arg()));
    (@call $current_task:ident; $args:ident; $func:ident [2]) => ($func($current_task, $args.0.into_arg(), $args.1.into_arg()));
    (@call $current_task:ident; $args:ident; $func:ident [3]) => ($func($current_task, $args.0.into_arg(), $args.1.into_arg(), $args.2.into_arg()));
    (@call $current_task:ident; $args:ident; $func:ident [4]) => ($func($current_task, $args.0.into_arg(), $args.1.into_arg(), $args.2.into_arg(), $args.3.into_arg()));
    (@call $current_task:ident; $args:ident; $func:ident [5]) => ($func($current_task, $args.0.into_arg(), $args.1.into_arg(), $args.2.into_arg(), $args.3.into_arg(), $args.4.into_arg()));
    (@call $current_task:ident; $args:ident; $func:ident [6]) => ($func($current_task, $args.0.into_arg(), $args.1.into_arg(), $args.2.into_arg(), $args.3.into_arg(), $args.4.into_arg(), $args.5.into_arg()));
}

pub fn dispatch_syscall(
    current_task: &mut CurrentTask,
    syscall: &Syscall,
) -> Result<SyscallResult, Errno> {
    use crate::bpf::*;
    use crate::fs::socket::syscalls::*;
    use crate::fs::syscalls::*;
    use crate::mm::syscalls::*;
    use crate::signals::syscalls::*;
    use crate::syscalls::misc::*;
    use crate::syscalls::time::*;
    use crate::task::syscalls::*;

    let args = (syscall.arg0, syscall.arg1, syscall.arg2, syscall.arg3, syscall.arg4, syscall.arg5);
    syscall_match! {
        current_task; syscall.decl.number; args;
        accept4[4],
        accept[3],
        #[cfg(target_arch = "x86_64")] access[2],
        #[cfg(target_arch = "x86_64")] arch_prctl[2],
        bind[3],
        bpf[3],
        brk[1],
        capget[2],
        capset[2],
        chdir[1],
        #[cfg(target_arch = "x86_64")] chmod[2],
        #[cfg(target_arch = "x86_64")] chown[3],
        chroot[1],
        clock_getres[2],
        clock_gettime[2],
        clock_nanosleep[4],
        clone[5],
        clone3[2],
        close[1],
        close_range[3],
        connect[3],
        #[cfg(target_arch = "x86_64")] creat[2],
        #[cfg(target_arch = "x86_64")] dup2[2],
        dup3[3],
        dup[1],
        epoll_create1[1],
        #[cfg(target_arch = "x86_64")] epoll_create[1],
        epoll_ctl[4],
        epoll_pwait[5],
        epoll_pwait2[5],
        #[cfg(target_arch = "x86_64")] epoll_wait[4],
        eventfd2[2],
        #[cfg(target_arch = "x86_64")] eventfd[1],
        execve[3],
        execveat[5],
        exit[1],
        exit_group[1],
        faccessat2[4],
        faccessat[3],
        fadvise64[4],
        fallocate[4],
        fchdir[1],
        fchmod[2],
        fchmodat[3],
        fchown[3],
        fchownat[5],
        fcntl[3],
        fdatasync[1],
        fgetxattr[4],
        flistxattr[3],
        flock[2],
        fremovexattr[2],
        fsetxattr[5],
        fstat[2],
        fstatfs[2],
        fsync[1],
        ftruncate[2],
        futex[6],
        getcpu[2],
        getcwd[2],
        getdents64[3],
        #[cfg(target_arch = "x86_64")] getdents[3],
        getegid[0],
        geteuid[0],
        getgid[0],
        getgroups[2],
        getitimer[2],
        getpeername[3],
        getpgid[1],
        #[cfg(target_arch = "x86_64")] getpgrp[0],
        getpid[0],
        getppid[0],
        getpriority[2],
        getrandom[3],
        getresgid[3],
        getresuid[3],
        getrlimit[2],
        getrusage[2],
        getsid[1],
        getsockname[3],
        getsockopt[5],
        gettid[0],
        gettimeofday[2],
        getuid[0],
        getxattr[4],
        inotify_add_watch[3],
        inotify_init1[1],
        #[cfg(target_arch = "x86_64")] inotify_init[0],
        inotify_rm_watch[2],
        ioctl[3],
        kill[2],
        #[cfg(target_arch = "x86_64")] lchown[3],
        lgetxattr[4],
        #[cfg(target_arch = "x86_64")] link[2],
        linkat[5],
        listen[2],
        listxattr[3],
        llistxattr[3],
        lremovexattr[2],
        lseek[3],
        lsetxattr[5],
        #[cfg(target_arch = "x86_64")] lstat[2],
        madvise[3],
        membarrier[3],
        memfd_create[2],
        #[cfg(target_arch = "x86_64")] mkdir[2],
        mkdirat[3],
        #[cfg(target_arch = "x86_64")] mknod[3],
        mknodat[4],
        mmap[6],
        mount[5],
        mprotect[3],
        mremap[5],
        msync[3],
        munmap[2],
        nanosleep[2],
        newfstatat[4],
        #[cfg(target_arch = "x86_64")] open[3],
        openat[4],
        // pidfd_getfd[3],  // TODO(fxbug.dev/119476) implement pidfd support.
        // pidfd_open[2],
        // pidfd_send_signal[4],
        pipe2[2],
        #[cfg(target_arch = "x86_64")] pipe[1],
        #[cfg(target_arch = "x86_64")] poll[3],
        ppoll[4],
        prctl[5],
        pread64[4],
        preadv[4],
        prlimit64[4],
        process_vm_readv[6],
        pselect6[6],
        pwrite64[4],
        pwritev[4],
        read[3],
        #[cfg(target_arch = "x86_64")] readlink[3],
        readlinkat[4],
        readv[3],
        reboot[4],
        recvfrom[6],
        recvmmsg[5],
        recvmsg[3],
        removexattr[2],
        #[cfg(target_arch = "x86_64")] rename[2],
        renameat2[5],
        renameat[4],
        restart_syscall[0],
        #[cfg(target_arch = "x86_64")] rmdir[1],
        rt_sigaction[4],
        rt_sigprocmask[4],
        rt_sigreturn[0],
        rt_sigsuspend[2],
        rt_sigtimedwait[4],
        rt_tgsigqueueinfo[4],
        sched_get_priority_min[1],
        sched_get_priority_max[1],
        sched_getaffinity[3],
        sched_getparam[2],
        sched_getscheduler[1],
        sched_setaffinity[3],
        sched_setscheduler[3],
        sched_yield[0],
        seccomp[3],
        #[cfg(target_arch = "x86_64")] select[5],
        sendmmsg[4],
        sendmsg[3],
        sendto[6],
        sendfile[4],
        set_tid_address[1],
        setgid[1],
        setgroups[2],
        setdomainname[2],
        sethostname[2],
        setitimer[3],
        setpgid[2],
        setpriority[3],
        setresgid[3],
        setresuid[3],
        setregid[2],
        setreuid[2],
        setrlimit[2],
        setsid[0],
        setsockopt[5],
        setuid[1],
        setxattr[5],
        shutdown[2],
        sigaltstack[2],
        signalfd4[4],
        socket[3],
        socketpair[4],
        splice[6],
        #[cfg(target_arch = "x86_64")] stat[2],
        statfs[2],
        statx[5],
        #[cfg(target_arch = "x86_64")] symlink[2],
        symlinkat[3],
        syncfs[1],
        sysinfo[1],
        tgkill[3],
        #[cfg(target_arch = "x86_64")] time[1],
        timer_create[3],
        timer_delete[1],
        timer_gettime[2],
        timer_getoverrun[1],
        timer_settime[4],
        timerfd_create[2],
        timerfd_gettime[2],
        timerfd_settime[4],
        tkill[2],
        truncate[2],
        umask[1],
        umount2[2],
        uname[1],
        #[cfg(target_arch = "x86_64")] unlink[1],
        unlinkat[3],
        unshare[1],
        utimensat[4],
        #[cfg(target_arch = "x86_64")] vfork[0],
        wait4[4],
        waitid[4],
        write[3],
        writev[3],
    }
}

trait IntoSyscallArg<T> {
    fn into_arg(self) -> T;
}
impl<T> IntoSyscallArg<T> for u64
where
    T: FromSyscallArg,
{
    fn into_arg(self) -> T {
        T::from_arg(self)
    }
}
trait FromSyscallArg {
    fn from_arg(arg: u64) -> Self;
}
macro_rules! impl_from_syscall_arg {
    { for $ty:ty: $arg:ident => $($body:tt)* } => {
        impl FromSyscallArg for $ty {
            fn from_arg($arg: u64) -> Self { $($body)* }
        }
    }
}

impl_from_syscall_arg! { for i32: arg => arg as Self }
impl_from_syscall_arg! { for i64: arg => arg as Self }
impl_from_syscall_arg! { for u32: arg => arg as Self }
impl_from_syscall_arg! { for usize: arg => arg as Self }
impl_from_syscall_arg! { for u64: arg => arg as Self }
impl_from_syscall_arg! { for UserAddress: arg => Self::from(arg) }
impl_from_syscall_arg! { for UserCString: arg => Self::new(arg.into_arg()) }

impl_from_syscall_arg! { for FdNumber: arg => Self::from_raw(arg as i32) }
impl_from_syscall_arg! { for FileMode: arg => Self::from_bits(arg as u32) }
impl_from_syscall_arg! { for DeviceType: arg => Self::from_bits(arg) }
impl_from_syscall_arg! { for UncheckedSignal: arg => Self::new(arg) }

impl<T> FromSyscallArg for UserRef<T> {
    fn from_arg(arg: u64) -> UserRef<T> {
        Self::new(UserAddress::from(arg))
    }
}
