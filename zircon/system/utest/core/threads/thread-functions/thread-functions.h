// Copyright 2017 The Fuchsia Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_UTEST_CORE_THREADS_THREAD_FUNCTIONS_THREAD_FUNCTIONS_H_
#define ZIRCON_SYSTEM_UTEST_CORE_THREADS_THREAD_FUNCTIONS_THREAD_FUNCTIONS_H_

#include <zircon/types.h>

// This file contains thread functions that do various things useful for testing thread behavior.

// The arg is a zx_time_t which is passed to zx_nanosleep.
void threads_test_sleep_fn(void* arg);

// The arg is an event. It will first be waited on for signal 0, then it will issue signal 1 to
// notify completion.
void threads_test_wait_fn(void* arg);
void threads_test_wait_detach_fn(void* arg);

// The arg is an event which will be waited on for signal 0 (to synchronize the beginning), then
// it will issue a debug break instruction (causing a SW_BREAKPOINT exception), then it will exit.
void threads_test_wait_break_fn(void* arg);

// How far the PC must be advanced to skip over a breakpoint after it hits.
extern const int kBreakpointPcAdjustment;

// This thread issues an infinite wait on signal 0 of the event whose handle is passed in arg.
void threads_test_infinite_wait_fn(void* arg);

// The arg is a port handle which is waited on. When a packet is received, it will send a packet
// to the port whose key is 5 greater than the input key.
void threads_test_port_fn(void* arg);

// The arg is a pointer to channel_call_suspend_test_arg (below). The function will send a small
// message and expects to receive the same contents in a reply.
//
// On completion, arg->call_status will be set to the success of the operation.
void threads_test_channel_call_fn(void* arg);

struct channel_call_suspend_test_arg {
  zx_handle_t channel;
  zx_status_t call_status;
};

// The arg is a pointer to bad_syscall_arg (below). The function will wait for ZX_USER_SIGNAL_0
// on the given event and then issue the given (bad) syscall.
void threads_bad_syscall_fn(void* arg);

struct bad_syscall_arg {
  zx_handle_t event;
  uint64_t syscall_number;
};

// Implementation of atomic store and atomic load.
//
// Used by |threads_test_atomic_store|, because functions in
// |thread-functions.cc| can't use standard library functions.
void atomic_store(volatile int* addr, int value);
int atomic_load(volatile int* addr);
int atomic_exchange(volatile int* addr, int value);

constexpr int kTestAtomicSetValue = 1;
constexpr int kTestAtomicExitValue = 2;
constexpr int kTestAtomicClobberValue = 3;

// The arg is a |volatile int*|. The function loops storing |kTestAtomicSetValue| there
// until it sees |kTestAtomicExitValue| then exits.
void threads_test_atomic_store(void* arg);

// The arg is an event. It will first send a signal 0 to indicate begin running then wiat for a
// signal 1 to stop running.
void threads_test_run_fn(void* arg);

struct syscall_suspended_reg_state_test_arg {
  zx_handle_t event;
  zx_signals_t observed;
  zx_status_t status;
};

// Waits on |event| for ZX_USER_SIGNAL_0, stores the observed signals in |observed|, stores the
// syscall result in |status|.
//
// |arg| is a syscall_suspended_reg_state_test_arg.
void threads_test_wait_event_fn(void* arg);

#endif  // ZIRCON_SYSTEM_UTEST_CORE_THREADS_THREAD_FUNCTIONS_THREAD_FUNCTIONS_H_
