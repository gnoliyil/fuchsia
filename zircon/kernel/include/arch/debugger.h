// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_INCLUDE_ARCH_DEBUGGER_H_
#define ZIRCON_KERNEL_INCLUDE_ARCH_DEBUGGER_H_

#include <stdbool.h>
#include <sys/types.h>
#include <zircon/compiler.h>
#include <zircon/syscalls/debug.h>
#include <zircon/types.h>

struct Thread;

// The caller is responsible for making sure the thread is in an exception
// or is suspended, and stays so.
zx_status_t arch_get_general_regs(Thread* thread, zx_thread_state_general_regs_t* out);
zx_status_t arch_set_general_regs(Thread* thread, const zx_thread_state_general_regs_t* in);

zx_status_t arch_get_fp_regs(Thread* thread, zx_thread_state_fp_regs_t* out);
zx_status_t arch_set_fp_regs(Thread* thread, const zx_thread_state_fp_regs_t* in);

zx_status_t arch_get_vector_regs(Thread* thread, zx_thread_state_vector_regs_t* out);
zx_status_t arch_set_vector_regs(Thread* thread, const zx_thread_state_vector_regs_t* in);

zx_status_t arch_get_debug_regs(Thread* thread, zx_thread_state_debug_regs_t* out);
zx_status_t arch_set_debug_regs(Thread* thread, const zx_thread_state_debug_regs_t* in);

zx_status_t arch_get_single_step(Thread* thread, zx_thread_state_single_step_t* out);
zx_status_t arch_set_single_step(Thread* thread, const zx_thread_state_single_step_t* in);

#endif  // ZIRCON_KERNEL_INCLUDE_ARCH_DEBUGGER_H_
