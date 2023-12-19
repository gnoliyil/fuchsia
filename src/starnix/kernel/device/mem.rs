// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    device::{kobject::DeviceMetadata, simple_device_ops, DeviceMode},
    fs::sysfs::DeviceDirectory,
    mm::{
        create_anonymous_mapping_vmo, DesiredAddress, MappingName, MappingOptions, ProtectionFlags,
    },
    task::{CurrentTask, LogSubscription},
    vfs::{
        buffers::{InputBuffer, InputBufferExt as _, OutputBuffer},
        fileops_impl_seekless, Anon, FileHandle, FileObject, FileOps, FileWriteGuardRef, FsNode,
        FsNodeInfo, NamespaceNode,
    },
};
use fuchsia_zircon::{
    cprng_draw_uninit, {self as zx},
};
use starnix_logging::log_info;
use starnix_sync::Mutex;
use starnix_uapi::{
    auth::FsCred, device_type::DeviceType, error, errors::Errno, file_mode::FileMode,
    open_flags::OpenFlags, user_address::UserAddress,
};
use std::mem::MaybeUninit;

#[derive(Default)]
pub struct DevNull;

pub fn new_null_file(current_task: &CurrentTask, flags: OpenFlags) -> FileHandle {
    Anon::new_file_extended(
        current_task,
        Box::new(DevNull),
        flags,
        FsNodeInfo::new_factory(FileMode::from_bits(0o666), FsCred::root()),
    )
}

impl FileOps for DevNull {
    fileops_impl_seekless!();

    fn write(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        // Writes to /dev/null on Linux treat the input buffer in an unconventional way. The actual
        // data is not touched and if the input parameters are plausible the device claims to
        // successfully write up to MAX_RW_COUNT bytes.  If the input parameters are outside of the
        // user accessible address space, writes will return EFAULT.

        // For debugging log up to 4096 bytes from the input buffer. We don't care about errors when
        // trying to read data to log. The amount of data logged is chosen arbitrarily.
        let bytes_to_log = std::cmp::min(4096, data.available());
        let log_buffer = data.read_to_vec_limited(bytes_to_log);
        let bytes_logged = match log_buffer {
            Ok(bytes) => {
                log_info!("write to devnull: {:?}", String::from_utf8_lossy(&bytes));
                bytes.len()
            }
            Err(_) => 0,
        };

        Ok(bytes_logged + data.drain())
    }

    fn read(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        Ok(0)
    }

    fn to_handle(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
    ) -> Result<Option<zx::Handle>, Errno> {
        Ok(None)
    }
}

#[derive(Default)]
struct DevZero;
impl FileOps for DevZero {
    fileops_impl_seekless!();

    fn mmap(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        addr: DesiredAddress,
        vmo_offset: u64,
        length: usize,
        prot_flags: ProtectionFlags,
        mut options: MappingOptions,
        filename: NamespaceNode,
    ) -> Result<UserAddress, Errno> {
        // All /dev/zero mappings behave as anonymous mappings.
        //
        // This means that we always create a new zero-filled VMO for this mmap request.
        // Memory is never shared between two mappings of /dev/zero, even if
        // `MappingOptions::SHARED` is set.
        //
        // Similar to anonymous mappings, if this process were to request a shared mapping
        // of /dev/zero and then fork, the child and the parent process would share the
        // VMO created here.
        let vmo = create_anonymous_mapping_vmo(length as u64)?;

        options |= MappingOptions::ANONYMOUS;

        current_task.mm().map_vmo(
            addr,
            vmo.clone(),
            vmo_offset,
            length,
            prot_flags,
            options,
            // We set the filename here, even though we are creating what is
            // functionally equivalent to an anonymous mapping. Doing so affects
            // the output of `/proc/self/maps` and identifies this mapping as
            // file-based.
            MappingName::File(filename),
            FileWriteGuardRef(None),
        )
    }

    fn write(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        Ok(data.drain())
    }

    fn read(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        data.zero()
    }
}

#[derive(Default)]
struct DevFull;
impl FileOps for DevFull {
    fileops_impl_seekless!();

    fn write(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        _data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        error!(ENOSPC)
    }

    fn read(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        data.write_each(&mut |bytes| {
            bytes.fill(MaybeUninit::new(0));
            Ok(bytes.len())
        })
    }
}

#[derive(Default)]
pub struct DevRandom;
impl FileOps for DevRandom {
    fileops_impl_seekless!();

    fn write(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        Ok(data.drain())
    }

    fn read(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        data.write_each(&mut |bytes| {
            let read_bytes = cprng_draw_uninit(bytes);
            Ok(read_bytes.len())
        })
    }
}

pub fn open_kmsg(
    current_task: &CurrentTask,
    _id: DeviceType,
    _node: &FsNode,
    flags: OpenFlags,
) -> Result<Box<dyn FileOps>, Errno> {
    let subscription = if flags.can_read() {
        Some(Mutex::new(
            current_task
                .kernel()
                .syslog
                .snapshot_then_subscribe(&current_task, flags.contains(OpenFlags::NONBLOCK))?,
        ))
    } else {
        None
    };
    Ok(Box::new(DevKmsg(subscription)))
}

struct DevKmsg(Option<Mutex<LogSubscription>>);

impl FileOps for DevKmsg {
    fileops_impl_seekless!();

    fn read(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        let result = match self.0.as_ref().unwrap().lock().next() {
            Some(Ok(log)) => data.write(&log),
            Some(Err(err)) => Err(err),
            None => Ok(0),
        };
        result
    }

    fn write(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _offset: usize,
        data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        let bytes = data.read_all()?;
        log_info!(tag = "kmsg", "{}", String::from_utf8_lossy(&bytes).trim_end_matches('\n'));
        Ok(bytes.len())
    }
}

pub fn mem_device_init(system_task: &CurrentTask) {
    let kernel = system_task.kernel();
    let registry = &kernel.device_registry;

    let mem_class = registry.get_or_create_class(b"mem", registry.virtual_bus());
    registry.add_and_register_device(
        system_task,
        b"null",
        DeviceMetadata::new(b"null", DeviceType::NULL, DeviceMode::Char),
        mem_class.clone(),
        DeviceDirectory::new,
        simple_device_ops::<DevNull>,
    );
    registry.add_and_register_device(
        system_task,
        b"zero",
        DeviceMetadata::new(b"zero", DeviceType::ZERO, DeviceMode::Char),
        mem_class.clone(),
        DeviceDirectory::new,
        simple_device_ops::<DevZero>,
    );
    registry.add_and_register_device(
        system_task,
        b"full",
        DeviceMetadata::new(b"full", DeviceType::FULL, DeviceMode::Char),
        mem_class.clone(),
        DeviceDirectory::new,
        simple_device_ops::<DevFull>,
    );
    registry.add_and_register_device(
        system_task,
        b"random",
        DeviceMetadata::new(b"random", DeviceType::RANDOM, DeviceMode::Char),
        mem_class.clone(),
        DeviceDirectory::new,
        simple_device_ops::<DevRandom>,
    );
    registry.add_and_register_device(
        system_task,
        b"urandom",
        DeviceMetadata::new(b"urandom", DeviceType::URANDOM, DeviceMode::Char),
        mem_class.clone(),
        DeviceDirectory::new,
        simple_device_ops::<DevRandom>,
    );
    registry.add_and_register_device(
        system_task,
        b"kmsg",
        DeviceMetadata::new(b"kmsg", DeviceType::KMSG, DeviceMode::Char),
        mem_class,
        DeviceDirectory::new,
        open_kmsg,
    );
}
