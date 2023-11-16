// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_zircon as zx;

use fidl_fuchsia_buildinfo as buildinfo;
use fidl_fuchsia_hardware_power_statecontrol as fpower;
use fuchsia_component::client::connect_to_protocol_sync;
use lock_sequence::{Locked, Unlocked};

use crate::{
    logging::{log_error, log_info, not_implemented},
    mm::{MemoryAccessor, MemoryAccessorExt},
    syscalls::{decls::SyscallDecl, SyscallResult, SUCCESS},
    task::CurrentTask,
    types::{
        auth::{CAP_SYS_ADMIN, CAP_SYS_BOOT},
        errno::{errno, error, from_status_like_fdio, Errno},
        personality::PersonalityFlags,
        uapi,
        user_address::{UserAddress, UserCString, UserRef},
        utsname_t, GRND_NONBLOCK, GRND_RANDOM, LINUX_REBOOT_CMD_CAD_OFF, LINUX_REBOOT_CMD_CAD_ON,
        LINUX_REBOOT_CMD_HALT, LINUX_REBOOT_CMD_KEXEC, LINUX_REBOOT_CMD_RESTART,
        LINUX_REBOOT_CMD_RESTART2, LINUX_REBOOT_CMD_SW_SUSPEND, LINUX_REBOOT_MAGIC1,
        LINUX_REBOOT_MAGIC2, LINUX_REBOOT_MAGIC2A, LINUX_REBOOT_MAGIC2B, LINUX_REBOOT_MAGIC2C,
    },
};

pub fn sys_uname(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    name: UserRef<utsname_t>,
) -> Result<(), Errno> {
    fn init_array(fixed: &mut [u8; 65], init: &[u8]) {
        let len = init.len();
        fixed[..len].copy_from_slice(init)
    }

    let mut result = utsname_t {
        sysname: [0; 65],
        nodename: [0; 65],
        release: [0; 65],
        version: [0; 65],
        machine: [0; 65],
        domainname: [0; 65],
    };

    init_array(&mut result.sysname, b"Linux");
    if current_task.thread_group.read().personality.contains(PersonalityFlags::UNAME26) {
        init_array(&mut result.release, b"2.6.40-starnix");
    } else {
        init_array(&mut result.release, b"5.7.17-starnix");
    }

    let version = current_task.kernel().build_version.get_or_try_init(|| {
        let proxy =
            connect_to_protocol_sync::<buildinfo::ProviderMarker>().map_err(|_| errno!(ENOENT))?;
        let buildinfo = proxy.get_build_info(zx::Time::INFINITE).map_err(|e| {
            log_error!("FIDL error getting build info: {e}");
            errno!(EIO)
        })?;
        Ok(buildinfo.version.unwrap_or("starnix".to_string()))
    })?;

    init_array(&mut result.version, version.as_bytes());
    init_array(&mut result.machine, b"x86_64");

    {
        // Get the UTS namespace from the perspective of this task.
        let task_state = current_task.read();
        let uts_ns = task_state.uts_ns.read();
        init_array(&mut result.nodename, uts_ns.hostname.as_slice());
        init_array(&mut result.domainname, uts_ns.domainname.as_slice());
    }

    current_task.write_object(name, &result)?;
    Ok(())
}

pub fn sys_sysinfo(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    info: UserRef<uapi::sysinfo>,
) -> Result<(), Errno> {
    let page_size = zx::system_get_page_size();
    let total_ram_pages = zx::system_get_physmem() / (page_size as u64);
    let num_procs = current_task.kernel().pids.read().len();
    let result = uapi::sysinfo {
        uptime: (zx::Time::get_monotonic() - zx::Time::ZERO).into_seconds(),

        // TODO(fxbug.dev/125626): Report system load.
        loads: [0; 3],

        totalram: total_ram_pages,

        // TODO(fxbug.dev/125625): Return actual memory usage.
        freeram: total_ram_pages / 8,

        procs: num_procs.try_into().map_err(|_| errno!(EINVAL))?,
        mem_unit: page_size,

        ..Default::default()
    };

    current_task.write_object(info, &result)?;
    Ok(())
}

// Used to read a hostname or domainname from task memory
fn read_name(current_task: &CurrentTask, name: UserCString, len: u64) -> Result<Vec<u8>, Errno> {
    const MAX_LEN: usize = 64;
    let len = len as usize;

    if len > MAX_LEN {
        return error!(EINVAL);
    }

    // Read maximum characters and mark the null terminator.
    let mut name = current_task.read_c_string_to_vec(name, MAX_LEN)?;

    // Syscall may have specified an even smaller length, so trim to the requested length.
    if len < name.len() {
        name.truncate(len);
    }
    Ok(name)
}

pub fn sys_sethostname(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    hostname: UserCString,
    len: u64,
) -> Result<SyscallResult, Errno> {
    if !current_task.creds().has_capability(CAP_SYS_ADMIN) {
        return error!(EPERM);
    }

    let hostname = read_name(current_task, hostname, len)?;

    let task_state = current_task.read();
    let mut uts_ns = task_state.uts_ns.write();
    uts_ns.hostname = hostname;

    Ok(SUCCESS)
}

pub fn sys_setdomainname(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    domainname: UserCString,
    len: u64,
) -> Result<SyscallResult, Errno> {
    if !current_task.creds().has_capability(CAP_SYS_ADMIN) {
        return error!(EPERM);
    }

    let domainname = read_name(current_task, domainname, len)?;

    let task_state = current_task.read();
    let mut uts_ns = task_state.uts_ns.write();
    uts_ns.domainname = domainname;

    Ok(SUCCESS)
}

pub fn sys_getrandom(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    buf_addr: UserAddress,
    size: usize,
    flags: u32,
) -> Result<usize, Errno> {
    if flags & !(GRND_RANDOM | GRND_NONBLOCK) != 0 {
        return error!(EINVAL);
    }
    let mut buf = vec![0; size];
    zx::cprng_draw(&mut buf);
    current_task.write_memory(buf_addr, &buf[0..size])?;
    Ok(size)
}

pub fn sys_reboot(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    magic: u32,
    magic2: u32,
    cmd: u32,
    arg: UserAddress,
) -> Result<(), Errno> {
    if magic != LINUX_REBOOT_MAGIC1
        || (magic2 != LINUX_REBOOT_MAGIC2
            && magic2 != LINUX_REBOOT_MAGIC2A
            && magic2 != LINUX_REBOOT_MAGIC2B
            && magic2 != LINUX_REBOOT_MAGIC2C)
    {
        return error!(EINVAL);
    }
    if !current_task.creds().has_capability(CAP_SYS_BOOT) {
        return error!(EPERM);
    }

    match cmd {
        // CAD on/off commands turn Ctrl-Alt-Del keystroke on or off without halting the system.
        LINUX_REBOOT_CMD_CAD_ON | LINUX_REBOOT_CMD_CAD_OFF => Ok(()),

        // `kexec_load()` is not supported.
        LINUX_REBOOT_CMD_KEXEC => error!(ENOSYS),

        // Suspend is not implemented.
        LINUX_REBOOT_CMD_SW_SUSPEND => error!(ENOSYS),

        LINUX_REBOOT_CMD_HALT | LINUX_REBOOT_CMD_RESTART | LINUX_REBOOT_CMD_RESTART2 => {
            // This is an arbitrary limit that should be large enough.
            const MAX_REBOOT_ARG_LEN: usize = 256;
            let arg_bytes = current_task
                .read_c_string_to_vec(UserCString::new(arg), MAX_REBOOT_ARG_LEN)
                .unwrap_or_default();
            let reboot_args: Vec<_> = arg_bytes.split(|byte| byte == &b',').collect();

            let proxy = connect_to_protocol_sync::<fpower::AdminMarker>()
                .expect("couldn't connect to fuchsia.hardware.power.statecontrol.Admin");

            let reboot_reason = if reboot_args.contains(&&b"ota_update"[..]) {
                fpower::RebootReason::SystemUpdate
            } else if reboot_args.is_empty()
                || reboot_args.contains(&&b"shell"[..])
                || reboot_args.contains(&&b"bootloader"[..])
            {
                fpower::RebootReason::UserRequest
            } else {
                not_implemented!(
                    "Unsupported reboot args: '{}'",
                    String::from_utf8_lossy(&arg_bytes)
                );
                return error!(ENOSYS);
            };
            match proxy.reboot(reboot_reason, zx::Time::INFINITE) {
                Ok(_) => {
                    log_info!("Rebooting... reason: {:?}", reboot_reason);
                    // System is rebooting... wait until runtime ends.
                    zx::Time::INFINITE.sleep();
                }
                Err(e) => panic!("Failed to reboot, status: {e}"),
            }
            Ok(())
        }

        _ => error!(EINVAL),
    }
}

pub fn sys_sched_yield(
    _locked: &mut Locked<'_, Unlocked>,
    _current_task: &CurrentTask,
) -> Result<(), Errno> {
    // SAFETY: This is unsafe because it is a syscall. zx_thread_legacy_yield is always safe.
    let status = unsafe { zx::sys::zx_thread_legacy_yield(0) };
    zx::Status::ok(status).map_err(|status| from_status_like_fdio!(status))
}

pub fn sys_unknown(
    _locked: &mut Locked<'_, Unlocked>,
    _current_task: &CurrentTask,
    syscall_number: u64,
) -> Result<SyscallResult, Errno> {
    not_implemented!("unknown syscall {:?}", SyscallDecl::from_number(syscall_number));
    // TODO: We should send SIGSYS once we have signals.
    error!(ENOSYS)
}

pub fn sys_personality(
    _locked: &mut Locked<'_, Unlocked>,
    current_task: &CurrentTask,
    persona: u32,
) -> Result<SyscallResult, Errno> {
    let mut state = current_task.task.thread_group.write();
    let previous_value = state.personality.bits();
    if persona != 0xffffffff {
        // Use `from_bits_unchecked()` since we want to keep unknown flags.
        state.personality = unsafe { PersonalityFlags::from_bits_unchecked(persona) };
    }
    Ok(previous_value.into())
}
