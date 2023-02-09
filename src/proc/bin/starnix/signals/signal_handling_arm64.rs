// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_zircon as zx;

use crate::logging::{log_trace, log_warn};
use crate::mm::MemoryAccessor;
use crate::signals::*;
use crate::syscalls::SyscallResult;
use crate::task::*;
use crate::types::*;

/// A `SignalStackFrame` contains all the state that is stored on the stack prior to executing a
/// signal handler. The exact layout of this structure is part of the platform's ABI.
#[repr(C)]
struct SignalStackFrame {
    siginfo_bytes: [u8; std::mem::size_of::<siginfo_t>()],
    context: ucontext,
}

const SIG_STACK_SIZE: usize = std::mem::size_of::<SignalStackFrame>();

impl SignalStackFrame {
    fn new(siginfo: &SignalInfo, context: ucontext) -> SignalStackFrame {
        SignalStackFrame { context, siginfo_bytes: siginfo.as_siginfo_bytes() }
    }

    fn as_bytes(&self) -> &[u8; SIG_STACK_SIZE] {
        unsafe { std::mem::transmute(self) }
    }

    fn from_bytes(bytes: [u8; SIG_STACK_SIZE]) -> SignalStackFrame {
        unsafe { std::mem::transmute(bytes) }
    }
}

/// Prepares `current` state to execute the signal handler stored in `action`.
///
/// This function stores the state required to restore after the signal handler on the stack.
pub fn dispatch_signal_handler(
    task: &Task,
    registers: &mut RegisterState,
    signal_state: &mut SignalState,
    siginfo: SignalInfo,
    action: sigaction_t,
) {
    // TODO implement this on ARM.
}

pub fn restore_from_signal_handler(current_task: &mut CurrentTask) -> Result<(), Errno> {
    // TODO implement this on ARM.
    error!(ENOSYS)
}

/// Maybe adjust a task's registers to restart a syscall once the task switches back to userspace,
/// based on whether the return value is one of the restartable error codes such as ERESTARTSYS.
fn prepare_to_restart_syscall(current_task: &mut CurrentTask, sigaction: Option<sigaction_t>) {
    // TODO implement this on ARM.
}