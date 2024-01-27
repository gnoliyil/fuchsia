// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include <lib/tbi/tbi.h>
#include <link.h>
#include <zircon/status.h>
#include <zircon/syscalls/port.h>

#include <test-utils/test-utils.h>
#include <zxtest/zxtest.h>

#include "inferior-control.h"
#include "inferior.h"
#include "utils.h"

namespace {

// A ZX_EXCP_SW_BREAKPOINT requires some registers tune-up in order to be handled correctly
// depending on the architecture. This functions takes care of the correct setup of the program
// counter so that the exception can be resumed successfully.
zx_status_t cleanup_breakpoint(zx_handle_t thread) {
#if defined(__x86_64__)
  // On x86, the pc is left at one past the s/w break insn,
  // so there's nothing more we need to do.
  return ZX_OK;
#elif defined(__aarch64__)
  // Skip past the brk instruction.
  zx_thread_state_general_regs_t regs = {};
  zx_status_t status =
      zx_thread_read_state(thread, ZX_THREAD_STATE_GENERAL_REGS, &regs, sizeof(regs));
  if (status != ZX_OK)
    return status;

  regs.pc += 4;
  return zx_thread_write_state(thread, ZX_THREAD_STATE_GENERAL_REGS, &regs, sizeof(regs));
#else
  return ZX_ERR_NOT_SUPPORTED;
#endif
}

struct dyn_break_on_load_state_t {
  zx_handle_t process_handle = ZX_HANDLE_INVALID;
  int dyn_load_count = 0;
};

void dyn_break_on_load_test_handler(inferior_data_t* data, const zx_port_packet_t* packet,
                                    void* handler_arg) {
  auto* test_state = reinterpret_cast<dyn_break_on_load_state_t*>(handler_arg);

  // This test is supposed to only get an exception and nothing else.
  zx_info_handle_basic_t basic_info;
  zx_status_t status = zx_object_get_info(data->exception_channel, ZX_INFO_HANDLE_BASIC,
                                          &basic_info, sizeof(basic_info), nullptr, nullptr);
  ASSERT_EQ(status, ZX_OK);
  ASSERT_EQ(basic_info.koid, packet->key);

  zx::exception exception;
  zx_exception_info_t info;
  uint32_t num_bytes = sizeof(info);
  uint32_t num_handles = 1;
  status = zx_channel_read(data->exception_channel, 0, &info, exception.reset_and_get_address(),
                           num_bytes, num_handles, nullptr, nullptr);
  ASSERT_EQ(status, ZX_OK);

  switch (info.type) {
    case ZX_EXCP_SW_BREAKPOINT: {
      printf("Got ld.so breakpoint.\n");
      test_state->dyn_load_count++;

      // Get the debug break address
      uintptr_t r_debug_address;
      zx_status_t status =
          zx_object_get_property(test_state->process_handle, ZX_PROP_PROCESS_DEBUG_ADDR,
                                 &r_debug_address, sizeof(r_debug_address));
      ASSERT_EQ(status, ZX_OK);

      // Syscalls do not accept tagged addresses, so if this user pointer
      // contains a tag, the debugger should strip it.
      r_debug_address = tbi::RemoveTag(r_debug_address);

      size_t actual = 0;
      r_debug dl_debug = {};
      status = zx_process_read_memory(test_state->process_handle, r_debug_address, &dl_debug,
                                      sizeof(dl_debug), &actual);
      ASSERT_EQ(status, ZX_OK);
      ASSERT_EQ(actual, sizeof(dl_debug));

      // Get the registers.
      zx::thread thread;
      status = exception.get_thread(&thread);
      ASSERT_EQ(status, ZX_OK);

      zx_thread_state_general_regs_t regs = {};
      read_inferior_gregs(thread.get(), &regs);

      uint64_t rip = 0;
#if defined(__x86_64__)
      // x64 will report the exception address after execution the software breakpoint instruction.
      rip = regs.rip - 1;
#elif defined(__aarch64__)
      rip = regs.pc;
#endif

      // The address of the breakpoint should euqal to the value of ZX_PROP_PROCESS_BREAK_ON_LOAD.
      uintptr_t break_on_load_addr;
      status = zx_object_get_property(test_state->process_handle, ZX_PROP_PROCESS_BREAK_ON_LOAD,
                                      &break_on_load_addr, sizeof(break_on_load_addr));
      ASSERT_EQ(rip, break_on_load_addr);

      ASSERT_EQ(cleanup_breakpoint(thread.get()), ZX_OK);

      break;
    }
    default:
      printf("Unexpected exception %s (%u) on thread %lu\n", tu_exception_to_string(info.type),
             info.type, info.tid);
      break;
  }

  uint32_t state = ZX_EXCEPTION_STATE_HANDLED;
  status = exception.set_property(ZX_PROP_EXCEPTION_STATE, &state, sizeof(state));
  ASSERT_EQ(status, ZX_OK);
}

TEST(DynBreakOnLoadTests, DynBreakOnLoadTest) {
  springboard_t* sb;
  zx_handle_t inferior, channel;
  ASSERT_NO_FATAL_FAILURE(setup_inferior(kTestDynBreakOnLoad, &sb, &inferior, &channel));

  dyn_break_on_load_state_t test_state = {};
  test_state.process_handle = inferior;

  const uintptr_t kBreakOnLoad = 1;
  zx_status_t status = zx_object_set_property(inferior, ZX_PROP_PROCESS_BREAK_ON_LOAD,
                                              &kBreakOnLoad, sizeof(kBreakOnLoad));
  if (status != ZX_OK) {
    fprintf(stderr, "Could not set dynamic linker break on load property: %s\n",
            zx_status_get_string(status));
    ASSERT_EQ(status, ZX_OK);
  }

  // Attach to the inferior now because we want to see thread starting exceptions.
  zx_handle_t port = ZX_HANDLE_INVALID;
  EXPECT_EQ(zx_port_create(0, &port), ZX_OK);
  size_t max_threads = 2;
  inferior_data_t* inferior_data = attach_inferior(inferior, port, max_threads);

  thrd_t wait_inf_thread =
      start_wait_inf_thread(inferior_data, dyn_break_on_load_test_handler, &test_state);
  EXPECT_NE(port, ZX_HANDLE_INVALID);

  ASSERT_NO_FATAL_FAILURE(start_inferior(sb));

  // The remaining testing happens at this point as threads start.
  // This testing is done in |dyn_break_on_load_test_handler()|.

  ASSERT_NO_FATAL_FAILURE(shutdown_inferior(channel, inferior));

  // Stop the waiter thread before closing the port that it's waiting on.
  join_wait_inf_thread(wait_inf_thread);

  detach_inferior(inferior_data, true);

  zx_handle_close(port);
  zx_handle_close(channel);
  zx_handle_close(inferior);

  // Verify how many loads there were.
  ASSERT_EQ(test_state.dyn_load_count, 10);
}

}  // namespace
