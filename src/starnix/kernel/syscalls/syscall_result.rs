// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::fs::{FdFlags, FdNumber};
use crate::task::CurrentTask;
use crate::types::{FileMode, OpenFlags, SealFlags, Signal, UserAddress};

#[derive(Eq, PartialEq, Debug)]
pub struct SyscallResult(u64);
pub const SUCCESS: SyscallResult = SyscallResult(0);

impl SyscallResult {
    /// TODO document
    #[cfg(target_arch = "x86_64")]
    pub fn keep_regs(current_task: &CurrentTask) -> Self {
        SyscallResult(current_task.registers.rax)
    }

    #[cfg(target_arch = "aarch64")]
    pub fn keep_regs(current_task: &CurrentTask) -> Self {
        SyscallResult(current_task.registers.r[0])
    }

    pub fn value(&self) -> u64 {
        self.0
    }
}

impl From<UserAddress> for SyscallResult {
    fn from(value: UserAddress) -> Self {
        SyscallResult(value.ptr() as u64)
    }
}

impl From<FileMode> for SyscallResult {
    fn from(value: FileMode) -> Self {
        SyscallResult(value.bits() as u64)
    }
}

impl From<SealFlags> for SyscallResult {
    fn from(value: SealFlags) -> Self {
        SyscallResult(value.bits() as u64)
    }
}

impl From<FdFlags> for SyscallResult {
    fn from(value: FdFlags) -> Self {
        SyscallResult(value.bits() as u64)
    }
}

impl From<OpenFlags> for SyscallResult {
    fn from(value: OpenFlags) -> Self {
        SyscallResult(value.bits() as u64)
    }
}

impl From<FdNumber> for SyscallResult {
    fn from(value: FdNumber) -> Self {
        SyscallResult(value.raw() as u64)
    }
}

impl From<Signal> for SyscallResult {
    fn from(value: Signal) -> Self {
        SyscallResult(value.number() as u64)
    }
}

impl From<bool> for SyscallResult {
    fn from(value: bool) -> Self {
        #[allow(clippy::bool_to_int_with_if)]
        SyscallResult(if value { 1 } else { 0 })
    }
}

impl From<u8> for SyscallResult {
    fn from(value: u8) -> Self {
        SyscallResult(value as u64)
    }
}

impl From<i32> for SyscallResult {
    fn from(value: i32) -> Self {
        SyscallResult(value as u64)
    }
}

impl From<u32> for SyscallResult {
    fn from(value: u32) -> Self {
        SyscallResult(value as u64)
    }
}

impl From<i64> for SyscallResult {
    fn from(value: i64) -> Self {
        SyscallResult(value as u64)
    }
}

impl From<u64> for SyscallResult {
    fn from(value: u64) -> Self {
        SyscallResult(value)
    }
}

impl From<usize> for SyscallResult {
    fn from(value: usize) -> Self {
        SyscallResult(value as u64)
    }
}

impl From<()> for SyscallResult {
    fn from(_value: ()) -> Self {
        SyscallResult(0)
    }
}
