// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <zircon/process.h>
#include <zircon/status.h>
#include <zircon/threads.h>

#include <atomic>

#include <test-utils/test-utils.h>
#include <zxtest/zxtest.h>

#include "inferior-control.h"
#include "inferior.h"
#include "utils.h"

#if defined(__aarch64__)

#include <zircon/hw/debug/arm64.h>

#endif

namespace {

// This is the variable we set the hw watchpoint on.
volatile int gVariableToChange = 0;

std::atomic<bool> gWatchpointThreadShouldContinue;

int watchpoint_function(void* user) {
  while (gWatchpointThreadShouldContinue) {
    gVariableToChange = gVariableToChange + 1;
    zx_nanosleep(zx_deadline_after(ZX_SEC(1)));
  }

  return 0;
}

#if defined(__x86_64__)

zx_status_t set_watchpoint(zx_handle_t thread_handle) {
  zx_thread_state_debug_regs_t debug_regs = {};
  // TODO(donosoc): Unify this under one public arch header.
  debug_regs.dr7 = 0b1 |         // L0 = 1 (watchpoint is active).
                   0b01 << 16 |  // R/W0 = 01 (Only data write triggers).
                   0b11 << 18;   // LEN0 = 11 (4 byte watchpoint).

  uint64_t addr = reinterpret_cast<uint64_t>(&gVariableToChange);
  // 4 byte aligned.
  assert((addr & 0b11) == 0);
  debug_regs.dr[0] = reinterpret_cast<uint64_t>(addr);

  return zx_thread_write_state(thread_handle, ZX_THREAD_STATE_DEBUG_REGS, &debug_regs,
                               sizeof(debug_regs));
}

#elif defined(__aarch64__)

zx_status_t set_watchpoint(zx_handle_t thread_handle) {
  zx_thread_state_debug_regs_t debug_regs = {};
  ARM64_DBGWCR_E_SET(&debug_regs.hw_wps[0].dbgwcr, 1);
  ARM64_DBGWCR_BAS_SET(&debug_regs.hw_wps[0].dbgwcr, 0xff);
  ARM64_DBGWCR_LSC_SET(&debug_regs.hw_wps[0].dbgwcr, 0b11);

  debug_regs.hw_wps[0].dbgwvr = reinterpret_cast<uint64_t>(&gVariableToChange);

  return zx_thread_write_state(thread_handle, ZX_THREAD_STATE_DEBUG_REGS, &debug_regs,
                               sizeof(debug_regs));
}

zx_status_t get_far(zx_handle_t thread_handle, uint64_t* far) {
  zx_thread_state_debug_regs_t debug_regs = {};
  zx_status_t status = zx_thread_read_state(thread_handle, ZX_THREAD_STATE_DEBUG_REGS, &debug_regs,
                                            sizeof(debug_regs));
  if (status != ZX_OK) {
    *far = 0;
    return status;
  }

  *far = debug_regs.far;
  return ZX_OK;
}

#elif defined(__riscv)

zx_status_t set_watchpoint(zx_handle_t thread_handle) { return ZX_ERR_NOT_SUPPORTED; }

#else
#error Unsupported arch.
#endif

zx_status_t unset_watchpoint(zx_handle_t thread_handle) {
  zx_thread_state_debug_regs_t debug_regs = {};
  return zx_thread_write_state(thread_handle, ZX_THREAD_STATE_DEBUG_REGS, &debug_regs,
                               sizeof(debug_regs));
}

}  // namespace

void test_watchpoint_impl(zx_handle_t excp_channel) {
  gWatchpointThreadShouldContinue = true;

  thrd_t thread;
  thrd_create(&thread, watchpoint_function, nullptr);
  zx_handle_t thread_handle = 0;
  thread_handle = thrd_get_zx_handle(thread);

  zx_status_t status;
  zx_handle_t suspend_token;
  status = zx_task_suspend(thread_handle, &suspend_token);
  ASSERT_EQ(status, ZX_OK);

  zx_signals_t observed;
  status = zx_object_wait_one(thread_handle, ZX_THREAD_SUSPENDED,
                              zx_deadline_after(ZX_TIME_INFINITE), &observed);
  ASSERT_EQ(status, ZX_OK);
  ASSERT_NE((observed & ZX_THREAD_SUSPENDED), 0);

  // Verify that the thread is suspended.
  zx_info_thread thread_info;
  status = zx_object_get_info(thread_handle, ZX_INFO_THREAD, &thread_info, sizeof(thread_info),
                              nullptr, nullptr);
  ASSERT_EQ(status, ZX_OK);
  ASSERT_EQ(thread_info.state, ZX_THREAD_STATE_SUSPENDED);

  printf("Watchpoint: Writing debug registers.\n");

  status = set_watchpoint(thread_handle);
#ifdef __riscv
  EXPECT_EQ(status, ZX_ERR_NOT_SUPPORTED);
  printf("Watchpoint: Not supported on RISC-V\n");
  return;
#endif
  ASSERT_EQ(status, ZX_OK);

  printf("Watchpoint: Resuming thread.\n");

  zx_handle_close(suspend_token);

  // We wait for the exception.
  tu_channel_wait_readable(excp_channel);

  zx_handle_t exception;
  zx_exception_info_t info;
  uint32_t num_bytes = sizeof(info);
  uint32_t num_handles = 1;
  status =
      zx_channel_read(excp_channel, 0, &info, &exception, num_bytes, num_handles, nullptr, nullptr);
  ASSERT_EQ(status, ZX_OK);

  ASSERT_EQ(info.type, ZX_EXCP_HW_BREAKPOINT);

#if defined(__aarch64__)
  uint64_t far = 0;
  ASSERT_EQ(get_far(thread_handle, &far), ZX_OK);
  ASSERT_NE(far, 0);

  // We can also get the FAR from the exception report. Just assert it matches
  // what we find in the debug regs.
  zx_exception_report_t report = {};
  ASSERT_OK(zx_object_get_info(thread_handle, ZX_INFO_THREAD_EXCEPTION_REPORT, &report,
                               sizeof(report), nullptr, nullptr));
  EXPECT_EQ(report.context.arch.u.arm_64.far, far);
#endif

  // Clear the state and resume the thread.
  status = unset_watchpoint(thread_handle);
  ASSERT_EQ(status, ZX_OK);
  gWatchpointThreadShouldContinue = false;

  uint32_t state = ZX_EXCEPTION_STATE_HANDLED;
  status = zx_object_set_property(exception, ZX_PROP_EXCEPTION_STATE, &state, sizeof(state));
  ASSERT_EQ(status, ZX_OK);

  zx_handle_close(exception);

  // join the thread.
  int res = -1;
  ASSERT_EQ(thrd_join(thread, &res), thrd_success);
  ASSERT_EQ(res, 0);
}

TEST(WatchpointStartTests, WatchpointTest) {
  zx_handle_t excp_channel = ZX_HANDLE_INVALID;
  ASSERT_EQ(zx_task_create_exception_channel(zx_process_self(), 0, &excp_channel), ZX_OK);

  test_watchpoint_impl(excp_channel);

  zx_handle_close(excp_channel);
}
