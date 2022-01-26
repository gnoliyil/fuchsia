// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![allow(non_upper_case_globals)]

use fuchsia_zircon as zx;
use fuchsia_zircon::{AsHandleRef, HandleBased};
use magma::*;
use parking_lot::Mutex;
use std::collections::HashMap;
use std::sync::Arc;

use super::magma::*;
use crate::device::wayland::image_file::*;
use crate::errno;
use crate::error;
use crate::fd_impl_nonblocking;
use crate::fd_impl_nonseekable;
use crate::fs::*;
use crate::logging::impossible_error;
use crate::syscalls::*;
use crate::task::{CurrentTask, EventHandler, Waiter};
use crate::types::*;

pub struct MagmaNode {}
impl FsNodeOps for MagmaNode {
    fn open(&self, _node: &FsNode, _flags: OpenFlags) -> Result<Box<dyn FileOps>, Errno> {
        MagmaFile::new()
    }
}

pub struct MagmaFile {
    channel: Arc<Mutex<Option<zx::Channel>>>,

    infos: Arc<Mutex<HashMap<magma_buffer_t, ImageInfo>>>,
}

impl MagmaFile {
    pub fn new() -> Result<Box<dyn FileOps>, Errno> {
        Ok(Box::new(Self {
            channel: Arc::new(Mutex::new(None)),
            infos: Arc::new(Mutex::new(HashMap::new())),
        }))
    }
}

impl FileOps for MagmaFile {
    fd_impl_nonseekable!();
    fd_impl_nonblocking!();

    fn ioctl(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        _request: u32,
        in_addr: UserAddress,
        _out_addr: UserAddress,
    ) -> Result<SyscallResult, Errno> {
        let (command, command_type) = read_magma_command_and_type(current_task, in_addr)?;
        let response_address = UserAddress::from(command.response_address);

        match command_type {
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_DEVICE_IMPORT => {
                let (_control, mut response): (
                    virtio_magma_device_import_ctrl_t,
                    virtio_magma_device_import_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let (client_channel, server_channel) =
                    zx::Channel::create().map_err(|_| errno!(EINVAL))?;
                fdio::service_connect(&"/dev/class/gpu/000", server_channel)
                    .map_err(|_| errno!(EINVAL))?;

                let device_channel = client_channel.raw_handle();
                *self.channel.lock() = Some(client_channel);

                let mut device_out: usize = 0;
                response.result_return = unsafe {
                    magma_device_import(device_channel, &mut device_out as *mut usize) as u64
                };

                response.device_out = device_out;
                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_DEVICE_IMPORT as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_CREATE_CONNECTION2 => {
                let (control, mut response): (
                    virtio_magma_create_connection2_ctrl,
                    virtio_magma_create_connection2_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let mut connection_out: magma_connection_t = std::ptr::null_mut();
                response.result_return = unsafe {
                    magma_create_connection2(control.device as usize, &mut connection_out) as u64
                };

                response.connection_out = connection_out as usize;
                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_CREATE_CONNECTION2 as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_RELEASE_CONNECTION => {
                let (control, mut response): (
                    virtio_magma_release_connection_ctrl_t,
                    virtio_magma_release_connection_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let connection = control.connection as magma_connection_t;
                unsafe { magma_release_connection(connection) };

                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_RELEASE_CONNECTION as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_DEVICE_RELEASE => {
                let (control, mut response): (
                    virtio_magma_device_release_ctrl_t,
                    virtio_magma_device_release_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                unsafe { magma_device_release(control.device as usize) };

                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_DEVICE_RELEASE as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_VIRT_CREATE_IMAGE => {
                let (control, mut response): (
                    virtio_magma_virt_create_image_ctrl_t,
                    virtio_magma_virt_create_image_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let mut create_info_ptr: u64 = 0;
                let create_info_address = UserAddress::from(control.create_info as u64);
                current_task
                    .mm
                    .read_object(UserRef::new(create_info_address), &mut create_info_ptr)?;

                let mut create_info = magma_image_create_info_t::default();
                let create_info_address = UserAddress::from(create_info_ptr);
                current_task.mm.read_object(UserRef::new(create_info_address), &mut create_info)?;

                let (vmo, token, info) =
                    create_drm_image(0, &create_info).map_err(|_| errno!(EINVAL))?;
                let vmo = Arc::new(vmo);
                let vmo_dup =
                    vmo.duplicate_handle(zx::Rights::SAME_RIGHTS).map_err(impossible_error)?;

                let mut buffer_out = magma_buffer_t::default();
                response.result_return = unsafe {
                    magma_import(
                        control.connection as magma_connection_t,
                        vmo_dup.into_raw(),
                        &mut buffer_out,
                    ) as u64
                };
                self.infos.lock().insert(buffer_out, ImageInfo { info, token, vmo });

                response.image_out = buffer_out;
                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_VIRT_CREATE_IMAGE as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_VIRT_GET_IMAGE_INFO => {
                let (control, mut response): (
                    virtio_magma_virt_get_image_info_ctrl_t,
                    virtio_magma_virt_get_image_info_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                // TODO(fxb/90145): Store images per connection.
                let _connection = control.connection;
                let image = control.image as usize;

                let mut image_info_ptr: u64 = 0;
                let image_info_address = UserAddress::from(control.image_info_out as u64);
                current_task
                    .mm
                    .read_object(UserRef::new(image_info_address), &mut image_info_ptr)?;

                let infos = self.infos.lock();
                let image_info = infos.get(&image).ok_or(errno!(EINVAL))?;
                let image_info_out = UserAddress::from(image_info_ptr as u64);
                current_task.mm.write_object(UserRef::new(image_info_out), &image_info.info)?;

                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_VIRT_GET_IMAGE_INFO as u32;
                response.result_return = MAGMA_STATUS_OK as u64;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_GET_BUFFER_SIZE => {
                let (control, mut response): (
                    virtio_magma_get_buffer_size_ctrl_t,
                    virtio_magma_get_buffer_size_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                response.result_return = unsafe { magma_get_buffer_size(control.buffer as usize) };
                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_GET_BUFFER_SIZE as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_FLUSH => {
                let (control, mut response): (
                    virtio_magma_flush_ctrl_t,
                    virtio_magma_flush_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                response.result_return =
                    unsafe { magma_flush(control.connection as magma_connection_t) as u64 };

                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_FLUSH as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_GET_BUFFER_HANDLE2 => {
                let (control, mut response): (
                    virtio_magma_get_buffer_handle2_ctrl_t,
                    virtio_magma_get_buffer_handle2_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let infos = self.infos.lock();
                // Note that this only stores handles for images. Once other buffer types are
                // supported this will need to be updated if they aren't stored in the same
                // collection.
                let image_info = infos.get(&(control.buffer as usize)).ok_or(errno!(EINVAL))?;
                let file = ImageFile::new(current_task.kernel(), image_info.clone());
                let fd = current_task.files.add_with_flags(file, FdFlags::empty())?;

                response.handle_out = fd.raw() as usize;
                response.result_return = MAGMA_STATUS_OK as u64;
                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_GET_BUFFER_HANDLE2 as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_RELEASE_BUFFER => {
                let (control, mut response): (
                    virtio_magma_release_buffer_ctrl_t,
                    virtio_magma_release_buffer_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                self.infos.lock().remove(&(control.buffer as usize));

                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_RELEASE_BUFFER as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_EXPORT => {
                let (control, mut response): (
                    virtio_magma_export_ctrl_t,
                    virtio_magma_export_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let infos = self.infos.lock();
                let image_info = infos.get(&(control.buffer as usize)).ok_or(errno!(EINVAL))?;
                let file = ImageFile::new(current_task.kernel(), image_info.clone());
                let fd = current_task.files.add_with_flags(file, FdFlags::empty())?;

                response.buffer_handle_out = fd.raw() as usize;
                response.result_return = MAGMA_STATUS_OK as u64;
                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_EXPORT as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_IMPORT => {
                let (control, mut response): (
                    virtio_magma_import_ctrl_t,
                    virtio_magma_import_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let file =
                    current_task.files.get(FdNumber::from_raw(control.buffer_handle as i32))?;
                let file = file.downcast_file::<ImageFile>().ok_or(errno!(EINVAL))?;
                let vmo = file
                    .info
                    .vmo
                    .duplicate_handle(zx::Rights::SAME_RIGHTS)
                    .map_err(impossible_error)?;

                let mut buffer_out = magma_buffer_t::default();
                response.result_return = unsafe {
                    magma_import(
                        control.connection as magma_connection_t,
                        vmo.into_raw(),
                        &mut buffer_out,
                    ) as u64
                };

                let _ = current_task.files.close(FdNumber::from_raw(control.buffer_handle as i32));
                self.infos.lock().insert(buffer_out, file.info.clone());

                response.buffer_out = buffer_out;
                response.result_return = MAGMA_STATUS_OK as u64;
                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_IMPORT as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_QUERY2 => {
                let (control, mut response): (
                    virtio_magma_query2_ctrl_t,
                    virtio_magma_query2_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let mut value_out = 0;
                response.result_return = unsafe {
                    magma_query2(control.device as usize, control.id, &mut value_out) as u64
                };
                response.value_out = value_out as usize;

                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_QUERY2 as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_GET_NOTIFICATION_CHANNEL_HANDLE => {
                let (control, mut response): (
                    virtio_magma_get_notification_channel_handle_ctrl_t,
                    virtio_magma_get_notification_channel_handle_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                response.result_return = unsafe {
                    magma_get_notification_channel_handle(control.connection as magma_connection_t)
                };

                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_GET_NOTIFICATION_CHANNEL_HANDLE as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_CREATE_CONTEXT => {
                let (control, mut response): (
                    virtio_magma_create_context_ctrl_t,
                    virtio_magma_create_context_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let mut context_id_out = 0;
                response.result_return = unsafe {
                    magma_create_context(
                        control.connection as magma_connection_t,
                        &mut context_id_out,
                    ) as u64
                };
                response.context_id_out = context_id_out as usize;

                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_CREATE_CONTEXT as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_RELEASE_CONTEXT => {
                let (control, mut response): (
                    virtio_magma_release_context_ctrl_t,
                    virtio_magma_release_context_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                unsafe {
                    magma_release_context(
                        control.connection as magma_connection_t,
                        control.context_id as u32,
                    );
                }

                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_RELEASE_CONTEXT as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_CREATE_BUFFER => {
                let (control, mut response): (
                    virtio_magma_create_buffer_ctrl_t,
                    virtio_magma_create_buffer_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let mut size_out = 0;
                let mut buffer_out = 0;
                response.result_return = unsafe {
                    magma_create_buffer(
                        control.connection as magma_connection_t,
                        control.size,
                        &mut size_out,
                        &mut buffer_out,
                    ) as u64
                };
                response.size_out = size_out as usize;
                response.buffer_out = buffer_out as usize;

                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_CREATE_BUFFER as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_GET_BUFFER_ID => {
                let (control, mut response): (
                    virtio_magma_get_buffer_id_ctrl_t,
                    virtio_magma_get_buffer_id_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                response.result_return =
                    unsafe { magma_get_buffer_id(control.buffer as magma_buffer_t) };

                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_GET_BUFFER_ID as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_CREATE_SEMAPHORE => {
                let (control, mut response): (
                    virtio_magma_create_semaphore_ctrl_t,
                    virtio_magma_create_semaphore_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let mut semaphore_out = 0;
                response.result_return = unsafe {
                    magma_create_semaphore(
                        control.connection as magma_connection_t,
                        &mut semaphore_out,
                    ) as u64
                };

                response.semaphore_out = semaphore_out;
                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_CREATE_SEMAPHORE as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_QUERY_RETURNS_BUFFER2 => {
                let (control, mut response): (
                    virtio_magma_query_returns_buffer2_ctrl_t,
                    virtio_magma_query_returns_buffer2_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let mut handle_out = 0;
                response.result_return = unsafe {
                    magma_query_returns_buffer2(
                        control.device as magma_device_t,
                        control.id,
                        &mut handle_out,
                    ) as u64
                };
                let vmo = unsafe { zx::Vmo::from(zx::Handle::from_raw(handle_out)) };
                let file = Anon::new_file(
                    anon_fs(current_task.kernel()),
                    Box::new(VmoFileObject::new(Arc::new(vmo))),
                    OpenFlags::RDWR,
                );
                let fd = current_task.files.add_with_flags(file, FdFlags::empty())?;

                response.handle_out = fd.raw() as usize;
                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_QUERY_RETURNS_BUFFER2 as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_GET_ERROR => {
                let (control, mut response): (
                    virtio_magma_get_error_ctrl_t,
                    virtio_magma_get_error_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                response.result_return =
                    unsafe { magma_get_error(control.connection as magma_connection_t) as u64 };

                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_GET_ERROR as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_IMPORT_SEMAPHORE => {
                let (control, mut response): (
                    virtio_magma_import_semaphore_ctrl_t,
                    virtio_magma_import_semaphore_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let mut semaphore_out = 0;
                response.result_return = unsafe {
                    magma_import_semaphore(
                        control.connection as magma_connection_t,
                        control.semaphore_handle,
                        &mut semaphore_out,
                    ) as u64
                };
                response.semaphore_out = semaphore_out;

                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_IMPORT_SEMAPHORE as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_GET_SEMAPHORE_ID => {
                let (control, mut response): (
                    virtio_magma_get_semaphore_id_ctrl_t,
                    virtio_magma_get_semaphore_id_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                response.result_return = unsafe {
                    magma_get_semaphore_id(control.semaphore as magma_semaphore_t) as u64
                };

                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_GET_SEMAPHORE_ID as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_RELEASE_SEMAPHORE => {
                let (control, mut response): (
                    virtio_magma_release_semaphore_ctrl_t,
                    virtio_magma_release_semaphore_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                unsafe {
                    magma_release_semaphore(
                        control.connection as magma_connection_t,
                        control.semaphore as magma_semaphore_t,
                    );
                };

                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_RELEASE_SEMAPHORE as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_EXPORT_SEMAPHORE => {
                let (control, mut response): (
                    virtio_magma_export_semaphore_ctrl_t,
                    virtio_magma_export_semaphore_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                let mut semaphore_handle_out = 0;
                response.result_return = unsafe {
                    magma_export_semaphore(
                        control.connection as magma_connection_t,
                        control.semaphore as magma_semaphore_t,
                        &mut semaphore_handle_out,
                    ) as u64
                };
                response.semaphore_handle_out = semaphore_handle_out as usize;

                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_EXPORT_SEMAPHORE as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_RESET_SEMAPHORE => {
                let (control, mut response): (
                    virtio_magma_reset_semaphore_ctrl_t,
                    virtio_magma_reset_semaphore_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                unsafe {
                    magma_reset_semaphore(control.semaphore as magma_semaphore_t);
                };

                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_RESET_SEMAPHORE as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_SIGNAL_SEMAPHORE => {
                let (control, mut response): (
                    virtio_magma_signal_semaphore_ctrl_t,
                    virtio_magma_signal_semaphore_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                unsafe {
                    magma_signal_semaphore(control.semaphore as magma_semaphore_t);
                };

                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_SIGNAL_SEMAPHORE as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_MAP_BUFFER_GPU => {
                let (control, mut response): (
                    virtio_magma_map_buffer_gpu_ctrl_t,
                    virtio_magma_map_buffer_gpu_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                response.result_return = unsafe {
                    magma_map_buffer_gpu(
                        control.connection as magma_connection_t,
                        control.buffer as usize,
                        control.page_offset,
                        control.page_count,
                        control.gpu_va,
                        control.map_flags,
                    ) as u64
                };

                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_MAP_BUFFER_GPU as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_POLL => {
                let (control, mut response): (virtio_magma_poll_ctrl_t, virtio_magma_poll_resp_t) =
                    read_control_and_response(current_task, &command)?;

                let num_items = control.count as usize / std::mem::size_of::<StarnixPollItem>();
                let items_ref = UserRef::new(UserAddress::from(control.items as u64));
                // Read the poll items as `StarnixPollItem`, since they contain a union. Also note
                // that the minimum length of the vector is 1, to always have a valid reference for
                // `magma_poll`.
                let mut starnix_items =
                    vec![StarnixPollItem::default(); std::cmp::max(num_items, 1)];
                current_task.mm.read_objects(items_ref, &mut starnix_items)?;
                // Then convert each item "manually" into `magma_poll_item_t`.
                let mut magma_items: Vec<magma_poll_item_t> =
                    starnix_items.iter().map(|item| item.into_poll_item()).collect();

                response.result_return = unsafe {
                    magma_poll(
                        &mut magma_items[0] as *mut magma_poll_item,
                        num_items as u32,
                        control.timeout_ns,
                    ) as u64
                };

                // Convert the poll items back to a serializable version after the `magma_poll`
                // call.
                let starnix_items: Vec<StarnixPollItem> =
                    magma_items.iter().map(StarnixPollItem::new).collect();
                current_task.mm.write_objects(items_ref, &starnix_items)?;

                response.hdr.type_ = virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_POLL as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            virtio_magma_ctrl_type_VIRTIO_MAGMA_CMD_EXECUTE_COMMAND_BUFFER_WITH_RESOURCES2 => {
                let (control, mut response): (
                    virtio_magma_execute_command_buffer_with_resources2_ctrl_t,
                    virtio_magma_execute_command_buffer_with_resources2_resp_t,
                ) = read_control_and_response(current_task, &command)?;

                // First read the `virtmagma_command_buffer`, this contains pointers to all the
                // remaining structures needed by `magma_execute_command_buffer_with_resources2`.
                let virt_command_buffer_ref =
                    UserRef::new(UserAddress::from(control.command_buffer as u64));
                let mut virt_command_buffer = virtmagma_command_buffer::default();
                current_task.mm.read_object(virt_command_buffer_ref, &mut virt_command_buffer)?;

                let command_buffer_ref = UserRef::<magma_command_buffer>::new(UserAddress::from(
                    virt_command_buffer.command_buffer,
                ));
                let mut command_buffer = magma_command_buffer::default();
                current_task.mm.read_object(command_buffer_ref, &mut command_buffer)?;

                let resources_ref = UserRef::<magma_exec_resource>::new(UserAddress::from(
                    virt_command_buffer.resources as u64,
                ));
                let mut resources = vec![
                    magma_exec_resource::default();
                    std::cmp::max(1, command_buffer.resource_count as usize)
                ];
                // The resources vector is of length >= 1, since we need a valid reference to pass
                // to the magma function even when there is no data. This check is here to prevent
                // us from reading objects when the length is really 0.
                if command_buffer.resource_count > 0 {
                    current_task.mm.read_objects(resources_ref, &mut resources)?;
                }

                let semaphore_ids_ref =
                    UserRef::<u64>::new(UserAddress::from(virt_command_buffer.semaphores as u64));
                let semaphore_count = (command_buffer.wait_semaphore_count
                    + command_buffer.signal_semaphore_count)
                    as usize;
                let mut semaphore_ids = vec![0; std::cmp::max(1, semaphore_count)];
                // This check exists for the same reason as the command_buffer.resource_count check
                // above (to avoid reading when the actual count is 0).
                if semaphore_count > 0 {
                    current_task.mm.read_objects(semaphore_ids_ref, &mut semaphore_ids)?;
                }
                response.result_return = unsafe {
                    magma_execute_command_buffer_with_resources2(
                        control.connection as magma_connection_t,
                        control.context_id,
                        &mut command_buffer,
                        &mut resources[0] as *mut magma_exec_resource,
                        &mut semaphore_ids[0] as *mut u64,
                    ) as u64
                };

                response.hdr.type_ =
                    virtio_magma_ctrl_type_VIRTIO_MAGMA_RESP_EXECUTE_COMMAND_BUFFER_WITH_RESOURCES2
                        as u32;
                current_task.mm.write_object(UserRef::new(response_address), &response)
            }
            t => {
                log::warn!("Got unknown request: {:?}", t);
                error!(ENOSYS)
            }
        }?;

        Ok(SUCCESS)
    }

    fn read(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        error!(EINVAL)
    }

    fn write(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _data: &[UserBuffer],
    ) -> Result<usize, Errno> {
        error!(EINVAL)
    }
}
