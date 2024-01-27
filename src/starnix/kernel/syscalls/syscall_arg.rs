// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    fs::{FdNumber, WdNumber},
    types::*,
};

#[derive(Debug, Default, Copy, Clone, Eq, PartialEq)]
pub struct SyscallArg(u64);

impl SyscallArg {
    pub fn from_raw(raw: u64) -> Self {
        Self(raw)
    }

    pub fn raw(&self) -> u64 {
        self.0
    }
}

impl From<UserAddress> for SyscallArg {
    fn from(value: UserAddress) -> Self {
        Self::from_raw(value.ptr() as u64)
    }
}

impl From<u64> for SyscallArg {
    fn from(value: u64) -> Self {
        Self::from_raw(value)
    }
}

impl From<usize> for SyscallArg {
    fn from(value: usize) -> Self {
        Self::from_raw(value as u64)
    }
}

impl From<bool> for SyscallArg {
    fn from(value: bool) -> Self {
        Self::from_raw(if value { 1 } else { 0 })
    }
}

impl std::fmt::LowerHex for SyscallArg {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        std::fmt::LowerHex::fmt(&self.0, f)
    }
}

macro_rules! impl_from_syscall_arg {
    { for $ty:ty: $arg:ident => $($body:tt)* } => {
        impl From<SyscallArg> for $ty {
            fn from($arg: SyscallArg) -> Self { $($body)* }
        }
    }
}

impl_from_syscall_arg! { for bool: arg => arg.raw() != 0 }
impl_from_syscall_arg! { for i32: arg => arg.raw() as Self }
impl_from_syscall_arg! { for i64: arg => arg.raw() as Self }
impl_from_syscall_arg! { for u32: arg => arg.raw() as Self }
impl_from_syscall_arg! { for usize: arg => arg.raw() as Self }
impl_from_syscall_arg! { for u64: arg => arg.raw() as Self }
impl_from_syscall_arg! { for UserAddress: arg => Self::from(arg.raw()) }
impl_from_syscall_arg! { for UserCString: arg => Self::new(arg.into()) }

impl_from_syscall_arg! { for FdNumber: arg => Self::from_raw(arg.raw() as i32) }
impl_from_syscall_arg! { for FileMode: arg => Self::from_bits(arg.raw() as u32) }
impl_from_syscall_arg! { for DeviceType: arg => Self::from_bits(arg.raw()) }
impl_from_syscall_arg! { for UncheckedSignal: arg => Self::new(arg.raw()) }
impl_from_syscall_arg! { for WdNumber: arg => Self::from_raw(arg.raw() as i32) }

impl<T> From<SyscallArg> for UserRef<T> {
    fn from(arg: SyscallArg) -> UserRef<T> {
        Self::new(arg.into())
    }
}
