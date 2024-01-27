// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2008-2014 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_INCLUDE_ARCH_H_
#define ZIRCON_KERNEL_INCLUDE_ARCH_H_

#include <sys/types.h>
#include <zircon/compiler.h>

struct iframe_t;

// Early platform initialization, before UART, MMU, kernel command line args, etc.
void arch_early_init();

// Perform any set up required before virtual memory is enabled, or the heap is set up.
void arch_prevm_init();

// Perform any set up required after heap/MMU is available.
void arch_init();

// Perform any per-CPU set up required.
void arch_late_init_percpu();

// Called just before initiating a system suspend to give the arch layer a
// chance to save state.  Must be called with interrupts disabled.
void arch_prep_suspend();

// Called immediately after resuming from a system suspend to let the arch layer
// reinitialize arch components.  Must be called with interrupts disabled.
void arch_resume();

// Initialize |iframe| for running a userspace thread.
// The rest of the current thread's state must already have been
// appropriately initialized (as viewable from a debugger at the
// ZX_EXCP_THREAD_STARTING exception).
void arch_setup_uspace_iframe(iframe_t* iframe, uintptr_t entry_point, uintptr_t sp, uintptr_t arg1,
                              uintptr_t arg2);

// Enter userspace.
// |iframe| is generally initialized with |arch_setup_uspace_iframe()|.
//
// Must be called with interrupts disabled.
void arch_enter_uspace(iframe_t* iframe) __NO_RETURN;

// On x86, user mode general registers are stored in one of two structures depending on how the
// thread entered the kernel.  If via interrupt/exception, they are stored in an iframe_t.  If via
// syscall, they are stored in an syscall_regs_t.
//
// On arm64, user mode general registers are stored in an iframe_t regardless of how the thread
// entered the kernel.
enum class GeneralRegsSource : uint32_t {
  None = 0u,
  Iframe = 1u,
#if defined(__x86_64__)
  Syscall = 2u,
#endif
};

/* arch specific bits */
#include <arch/defines.h>

#endif  // ZIRCON_KERNEL_INCLUDE_ARCH_H_
