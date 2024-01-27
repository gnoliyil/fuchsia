// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_INCLUDE_ARCH_EXCEPTION_H_
#define ZIRCON_KERNEL_INCLUDE_ARCH_EXCEPTION_H_

#include <sys/types.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

struct Thread;
typedef struct arch_exception_context arch_exception_context_t;
typedef struct zx_exception_report zx_exception_report_t;

// Called by arch code when it cannot handle an exception.
// |context| is architecture-specific, and can be dumped to the console
// using arch_dump_exception_context(). Implemented by non-arch code.
zx_status_t dispatch_user_exception(uint exception_type, const arch_exception_context_t* context);

// Dispatches an exception that was raised by a syscall using
// thread_signal_policy_exception() (see <kernel/thread.h>), causing
// dispatch_user_exception() to be called with the current context. Implemented
// by arch code. |policy_exception_code| is information about the policy error
// which is stored into the zx_exception_report_t.
zx_status_t arch_dispatch_user_policy_exception(uint32_t policy_exception_code,
                                                uint32_t policy_exception_data);

// Dumps architecture-specific state to the console. |context| typically comes
// from a call to dispatch_user_exception(). Implemented by arch code.
void arch_dump_exception_context(const arch_exception_context_t* context);

// Helper to dump fields in |context| that are not architecture-specific. Called from
// arch_dump_exception_context().
void dump_common_exception_context(const arch_exception_context_t* context);

// Sets |report| using architecture-specific information from |context|.
// Implemented by arch code.
void arch_fill_in_exception_context(const arch_exception_context_t* context,
                                    zx_exception_report_t* report);

// Record registers in |context| as being available to |zx_thread_read_state(),
// zx_thread_write_state()|.
//
// This is called prior to the thread stopping in an exception, and must be matched with a
// corresponding call to |arch_remove_exception_context()| prior to the thread resuming execution if
// it returns true.
__WARN_UNUSED_RESULT
bool arch_install_exception_context(Thread* thread, const arch_exception_context_t* context);

// Undo a previous call to |arch_install_exception_context()|.
void arch_remove_exception_context(Thread* thread);

#endif  // ZIRCON_KERNEL_INCLUDE_ARCH_EXCEPTION_H_
