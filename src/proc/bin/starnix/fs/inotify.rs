// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::fs::buffers::{InputBuffer, OutputBuffer};
use crate::fs::*;
use crate::logging::not_implemented;
use crate::task::{CurrentTask, EventHandler, WaitKey, Waiter};
use crate::types::*;
use fuchsia_zircon as zx;

pub struct InotifyFileObject {}

impl InotifyFileObject {
    /// Allocate a new, empty inotify object.
    pub fn new_file(current_task: &CurrentTask, non_blocking: bool) -> FileHandle {
        let flags =
            OpenFlags::RDONLY | if non_blocking { OpenFlags::NONBLOCK } else { OpenFlags::empty() };
        Anon::new_file(current_task, Box::new(InotifyFileObject {}), flags)
    }
}

impl FileOps for InotifyFileObject {
    fileops_impl_nonseekable!();

    fn write(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        _data: &mut dyn InputBuffer,
    ) -> Result<usize, Errno> {
        error!(EINVAL)
    }

    fn read(
        &self,
        _file: &FileObject,
        current_task: &CurrentTask,
        _data: &mut dyn OutputBuffer,
    ) -> Result<usize, Errno> {
        not_implemented!(current_task, "InotifyFileObject.read() is stubbed.");
        Waiter::new().wait_until(current_task, zx::Time::INFINITE)?;
        error!(EAGAIN)
    }

    fn wait_async(
        &self,
        _file: &FileObject,
        _current_task: &CurrentTask,
        waiter: &Waiter,
        _events: FdEvents,
        _handler: EventHandler,
    ) -> Result<WaitKey, Errno> {
        Ok(waiter.fake_wait())
    }

    fn cancel_wait(&self, _current_task: &CurrentTask, _waiter: &Waiter, _key: WaitKey) {}

    fn query_events(&self, _current_task: &CurrentTask) -> FdEvents {
        FdEvents::empty()
    }
}
