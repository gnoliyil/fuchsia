// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A suite of utilities for inspecting processes.

#ifndef INSPECTOR_INSPECTOR_H_
#define INSPECTOR_INSPECTOR_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <zircon/compiler.h>
#include <zircon/syscalls/debug.h>
#include <zircon/syscalls/exception.h>
#include <zircon/types.h>

__BEGIN_CDECLS

// Opaque handle for fetching and using the list of the process's DSOs.
typedef struct inspector_dsoinfo inspector_dsoinfo_t;

// The type of the buffer that holds the general registers, and exception info.
#if defined(__x86_64__)
typedef zx_x86_64_exc_data_t inspector_excp_data_t;
#elif defined(__aarch64__)
typedef zx_arm64_exc_data_t inspector_excp_data_t;
#else  // unsupported arch
typedef int inspector_excp_data_t;
#endif

// Set internal verbosity level, for debugging inspector itself.
extern void inspector_set_verbosity(int level);

// Print a backtrace of |thread| to |f|.
// |thread| must currently be stopped: either suspended or in an exception.
// The format of the output is verify specific: It outputs the format
// documented at //docs/reference/kernel/symbolizer_markup.md
extern void inspector_print_backtrace_markup(FILE* f, zx_handle_t process, zx_handle_t thread,
                                             inspector_dsoinfo_t* dso_list, uintptr_t pc,
                                             uintptr_t sp, uintptr_t fp);

// Fetch the list of the DSOs of |process|.
// |name| is the name of the application binary.
extern inspector_dsoinfo_t* inspector_dso_fetch_list(zx_handle_t process);

// Free the value returned by dso_fetch_list().
extern void inspector_dso_free_list(inspector_dsoinfo_t*);

// Return the DSO that contains |pc|.
// Returns NULL if not found.
extern inspector_dsoinfo_t* inspector_dso_lookup(inspector_dsoinfo_t* dso_list, zx_vaddr_t pc);

// Print markup context to |f|. This includes every module and every mapped
// region of memory derived from those modules.
void inspector_print_markup_context(FILE* f, zx_handle_t process);

// Try to find the copy of |dso| that contains debug information.
// On success returns ZX_OK with the path of the file stored in
// |out_debug_file|. On failure returns an error code.
extern zx_status_t inspector_dso_find_debug_file(inspector_dsoinfo_t* dso,
                                                 const char** out_debug_file);

// Fetch the general registers of |thread|.
zx_status_t inspector_read_general_regs(zx_handle_t thread, zx_thread_state_general_regs_t* regs);

// Print general registers |regs| to |f|.
// If |excp_data| is non-NULL then print useful related exception data
// along with the registers.
void inspector_print_general_regs(FILE* f, const zx_thread_state_general_regs_t* regs,
                                  const inspector_excp_data_t* excp_data);

// Hex8 - Print individual bytes, in hex.
// Hex32 - Print 32-bit words, in hex.
enum inspector_print_memory_format { Hex8, Hex32 };
// Print the contents of memory, typically the bottom of a thread's stack.
void inspector_print_memory(FILE* f, zx_handle_t process, zx_vaddr_t addr, size_t length,
                            enum inspector_print_memory_format format);

// Prints to |out| the debug info (registers, bottom of user stack, dso list, backtrace, etc.) of
// the given |thread| in |process|. The caller must be holding |thread| either in exception
// state or in a suspended state, otherwise obtaining the general registers will fail and this
// function will return without printing the state.
//
// If the |thread| is in an exception and it's not a backtrace request, the registers and the bottom
// of the stack will be printed. Otherwise, only the backtrace will be printed.
//
// Does NOT close the handles nor resume the thread.
void inspector_print_debug_info(FILE* out, zx_handle_t process, zx_handle_t thread);

// Prints to |out| the debug info for all the threads in the process (registers, bottom of user
// stack, dso list, backtrace, etc.).
//
// It will first print all the threads that are in an exception and then print the other threads.
//
// Does NOT close the |process| handle.
void inspector_print_debug_info_for_all_threads(FILE* out, zx_handle_t process);

__END_CDECLS

#endif  // INSPECTOR_INSPECTOR_H_
