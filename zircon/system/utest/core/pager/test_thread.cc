// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test_thread.h"

#include <lib/fit/function.h>
#include <lib/zx/exception.h>
#include <string.h>
#include <threads.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/debug.h>
#include <zircon/threads.h>

#ifndef __riscv
#include <inspector/inspector.h>
#endif

namespace pager_tests {

static int test_thread_fn(void* arg) {
  reinterpret_cast<TestThread*>(arg)->Run();
  return 0;
}

TestThread::TestThread(fit::function<bool()> fn) : fn_(std::move(fn)) {}

TestThread::~TestThread() {
  // TODO: UserPagers need to be destroyed before TestThreads to ensure threads aren't blocked
  if (thrd_) {
    ZX_ASSERT(thrd_join(thrd_, nullptr) == thrd_success);
  }
}

bool TestThread::Start() {
  constexpr const char* kName = "test_thread";

  if (thrd_create_with_name(&thrd_, test_thread_fn, this, kName) != thrd_success) {
    return false;
  }
  if (zx_handle_duplicate(thrd_get_zx_handle(thrd_), ZX_RIGHT_SAME_RIGHTS,
                          zx_thread_.reset_and_get_address()) != ZX_OK) {
    return false;
  }

  if (zx_thread_.create_exception_channel(0, &exception_channel_) != ZX_OK) {
    return false;
  }

  sync_completion_signal(&startup_sync_);

  return true;
}

bool TestThread::Wait() { return Wait(false /* expect_failure */, false /* expect_crash */, 0); }

bool TestThread::WaitForFailure() {
  return Wait(true /* expect_failure */, false /* expect_crash */, 0);
}

bool TestThread::WaitForCrash(uintptr_t crash_addr, zx_status_t error_status) {
  return Wait(false /* expect_failure */, true /* expect_crash */, crash_addr, error_status);
}

bool TestThread::Wait(bool expect_failure, bool expect_crash, uintptr_t crash_addr,
                      zx_status_t error_status) {
  zx_signals_t signals;
  ZX_ASSERT(exception_channel_.wait_one(ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                        zx::time::infinite(), &signals) == ZX_OK);

  if (signals & ZX_CHANNEL_PEER_CLOSED) {
    // Thread has terminated.
    return !expect_crash && success_ != expect_failure;
  } else if (signals & ZX_CHANNEL_READABLE) {
    zx_exception_report_t report;
    ZX_ASSERT(zx_object_get_info(zx_thread_.get(), ZX_INFO_THREAD_EXCEPTION_REPORT, &report,
                                 sizeof(report), NULL, NULL) == ZX_OK);
    bool res = expect_crash && report.header.type == ZX_EXCP_FATAL_PAGE_FAULT;
    if (res) {
#if defined(__x86_64__)
      uintptr_t actual_crash_addr = report.context.arch.u.x86_64.cr2;
#elif defined(__aarch64__)
      uintptr_t actual_crash_addr = report.context.arch.u.arm_64.far;
#elif defined(__riscv)
      uintptr_t actual_crash_addr = report.context.arch.u.riscv_64.tval;
#else
#error Unsupported architecture
#endif
      res &= crash_addr == actual_crash_addr;
    }
    if (res) {
      res &= error_status == static_cast<zx_status_t>(report.context.synth_code);
    }

    if (!res) {
      // Print debug info if the crash wasn't expected.
      PrintDebugInfo(report);
    }

    // thrd_exit takes a parameter, but we don't actually read it when we join
    zx_thread_state_general_regs_t regs;
    ZX_ASSERT(zx_thread_.read_state(ZX_THREAD_STATE_GENERAL_REGS, &regs, sizeof(regs)) == ZX_OK);
#if defined(__x86_64__)
    regs.rip = reinterpret_cast<uintptr_t>(thrd_exit);
#elif defined(__aarch64__) || defined(__riscv)
    regs.pc = reinterpret_cast<uintptr_t>(thrd_exit);
#else
#error Unsupported architecture
#endif
    ZX_ASSERT(zx_thread_.write_state(ZX_THREAD_STATE_GENERAL_REGS, &regs, sizeof(regs)) == ZX_OK);

    zx_exception_info_t info;
    zx::exception exception;
    ZX_ASSERT(exception_channel_.read(0, &info, exception.reset_and_get_address(), sizeof(info), 1,
                                      nullptr, nullptr) == ZX_OK);

    uint32_t state = ZX_EXCEPTION_STATE_HANDLED;
    ZX_ASSERT(exception.set_property(ZX_PROP_EXCEPTION_STATE, &state, sizeof(state)) == ZX_OK);
    return res;
  } else {
    ZX_ASSERT_MSG(false, "Unexpected channel signals");
  }
}

void TestThread::PrintDebugInfo(const zx_exception_report_t& report) {
  printf("\nCrash info:\n");

#ifdef __riscv

  printf("*** inspector library not supported on riscv64 ***\n");

#else

  zx_thread_state_general_regs_t regs;

  ZX_ASSERT(inspector_read_general_regs(zx_thread_.get(), &regs) == ZX_OK);
  // Delay setting this until here so Fail will know we now have the regs.
#if defined(__x86_64__)
  inspector_print_general_regs(stdout, &regs, &report.context.arch.u.x86_64);
#elif defined(__aarch64__)
  inspector_print_general_regs(stdout, &regs, &report.context.arch.u.arm_64);
#else
#error Unsupported architecture
#endif
  inspector_print_backtrace_markup(stdout, zx_process_self(), zx_thread_.get());

#endif
}

bool TestThread::WaitForBlocked() {
  while (1) {
    zx_info_thread_t info;
    uint64_t actual, actual_count;
    if (zx_thread_.get_info(ZX_INFO_THREAD, &info, sizeof(info), &actual, &actual_count) != ZX_OK) {
      return false;
    } else if (info.state == ZX_THREAD_STATE_DEAD) {
      return false;
    } else if (info.state == ZX_THREAD_STATE_BLOCKED_PAGER) {
      return true;
    }
    // There's no signal to wait on, so just poll
    zx_nanosleep(zx_deadline_after(ZX_USEC(100)));
  }
}

void TestThread::Run() {
  if (sync_completion_wait(&startup_sync_, ZX_TIME_INFINITE) == ZX_OK) {
    success_ = fn_();
  }
}

}  // namespace pager_tests
