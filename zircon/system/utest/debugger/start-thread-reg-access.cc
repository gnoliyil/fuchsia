// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/thread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <zircon/syscalls/debug.h>
#include <zircon/syscalls/port.h>
#include <zircon/types.h>

#include <atomic>

#include <fbl/algorithm.h>
#include <test-utils/test-utils.h>
#include <zxtest/zxtest.h>

#include "debugger.h"
#include "inferior-control.h"
#include "inferior.h"
#include "utils.h"

namespace {

constexpr uint64_t kMagicRegisterValue = 0x0123456789abcdefull;
#define MAGIC_REGISTER_VALUE_ASM "0x0123456789abcdef"

// State that is maintained across the register access tests.

struct reg_access_test_state_t {
  // The PC of the first thread can't be validated until we can get the
  // inferior's libc load address. Save it here for later validation.
  zx_vaddr_t inferior_libc_entry_point;

  // The load addresses of libc and executable are obtained from the
  // inferior after it has started.
  zx_vaddr_t inferior_libc_load_addr;
  zx_vaddr_t inferior_exec_load_addr;
};

typedef void(raw_thread_func_t)(void* arg1, void* arg2);

// Worker thread entry point so that we can exercise the setting of register
// values. We want to grab the register values at the start of the thread to
// see if they were set correctly, but we can't (or at least shouldn't) make
// any assumptions about what libc's thread entry will do to them before we're
// able to see them.  It's defined in pure assembly so that there are no
// issues with compiler-generated code's assumptions about the proper ABI
// setup, instrumentation, etc.
extern "C" [[noreturn]] void raw_capture_regs_thread_func(void* arg1, void* arg2,
                                                          raw_thread_func_t* func,
                                                          uint64_t magic_value);
__asm__(
    ".pushsection .text.raw_capture_regs_thread_func,\"ax\",%progbits\n"
    ".balign 4\n"
    ".type raw_capture_regs_thread_func,%function\n"
    "raw_capture_regs_thread_func:\n"
#ifdef __aarch64__
    "  mov x8, #(" MAGIC_REGISTER_VALUE_ASM
    " & 0xffff)\n"
    "  movk x8, #((" MAGIC_REGISTER_VALUE_ASM
    " >> 16) & 0xffff), lsl #16\n"
    "  movk x8, #((" MAGIC_REGISTER_VALUE_ASM
    " >> 32) & 0xffff), lsl #32\n"
    "  movk x8, #((" MAGIC_REGISTER_VALUE_ASM
    " >> 48) & 0xffff), lsl #48\n"
    "  cmp x3, x8\n"
    "  bne 0f\n"
    "  br x2\n"
    "0:brk #0\n"
#elif defined(__x86_64__)
    "  movabs $" MAGIC_REGISTER_VALUE_ASM
    ", %rax\n"
    "  cmpq %rax, %rcx\n"
    "  jne 0f\n"
    "  jmp *%rdx\n"
    "0:ud2\n"
#elif defined(__riscv)
    "  li a0, " MAGIC_REGISTER_VALUE_ASM
    "\n"
    "  bne a0, a1, 0f\n"
    "  jr a2\n"
    "0:unimp\n"
#else
#error "what machine?"
#endif
    ".size raw_capture_regs_thread_func, . - raw_capture_regs_thread_func\n"
    ".popsection");

// Helper function to test register access when a thread starts.

void test_thread_start_register_access(reg_access_test_state_t* test_state, zx_handle_t inferior,
                                       zx_koid_t tid) {
  zx::thread thread;
  zx_status_t status =
      zx_object_get_child(inferior, tid, ZX_RIGHT_SAME_RIGHTS, thread.reset_and_get_address());
  if (status == ZX_ERR_NOT_FOUND) {
    thread.reset();
  } else {
    ASSERT_EQ(status, ZX_OK);
  }
  ASSERT_TRUE(thread.is_valid());

  zx_info_thread_t info;
  status = thread.get_info(ZX_INFO_THREAD, &info, sizeof(info), nullptr, nullptr);
  ASSERT_EQ(status, ZX_OK);
  EXPECT_EQ(info.state, ZX_THREAD_STATE_BLOCKED_EXCEPTION, "");

  zx_thread_state_general_regs_t regs;
  read_inferior_gregs(thread.get(), &regs);
  uint64_t pc = extract_pc_reg(&regs);

  // If we're the first thread the pc should be the ELF entry point.
  // If not the pc should be the thread's entry point.
  zx_koid_t threads[1 + kNumExtraThreads];
  size_t num_threads;
  status = zx_object_get_info(inferior, ZX_INFO_PROCESS_THREADS, threads, sizeof(threads),
                              &num_threads, nullptr);
  if (num_threads == 1) {
    // We don't know the inferior's load address yet so we can't do a full
    // validation of the PC yet. Save it for later when we can.
    test_state->inferior_libc_entry_point = pc;
  }

  // Verify the initial values of all the other general regs.
  zx_thread_state_general_regs_t expected_regs{};

  // We don't know what these are, but they're non-zero.
  // The rest are generally zero.
#if defined(__x86_64__)
  expected_regs.rip = regs.rip;
  expected_regs.rsp = regs.rsp;
  expected_regs.rdi = regs.rdi;
  expected_regs.rsi = regs.rsi;
#elif defined(__aarch64__)
  expected_regs.pc = regs.pc;
  expected_regs.sp = regs.sp;
  expected_regs.r[0] = regs.r[0];
  expected_regs.r[1] = regs.r[1];
#elif defined(__riscv)
  expected_regs.pc = regs.pc;
  expected_regs.sp = regs.sp;
  expected_regs.a0 = regs.a0;
  expected_regs.a1 = regs.a1;
#endif

  // These values we know with certainty.
  // See arch_setup_uspace_iframe().
#if defined(__x86_64__)

#define X86_FLAGS_IF (1 << 9)
#define X86_FLAGS_IOPL_SHIFT (12)
  expected_regs.rflags = (0 << X86_FLAGS_IOPL_SHIFT) | X86_FLAGS_IF;

#elif defined(__aarch64__)

#define ARM64_CPSR_MASK_SERROR (1UL << 8)
  // TODO(dje): See TODO in arch_setup_uspace_iframe.
  // cpsr is read as 0x0 but it's set as 0x100;
  expected_regs.cpsr = regs.cpsr & ARM64_CPSR_MASK_SERROR;

#endif

  EXPECT_BYTES_EQ(&regs, &expected_regs, sizeof(regs));

  // If this is one of the extra threads, redirect its entry point and
  // set additional registers for the thread to pick up.
  if (num_threads > 1) {
    EXPECT_NE(test_state->inferior_exec_load_addr, 0);
    zx_vaddr_t our_exec_load_addr = get_exec_load_addr();
    zx_vaddr_t raw_thread_func_addr = reinterpret_cast<zx_vaddr_t>(&raw_capture_regs_thread_func);
    raw_thread_func_addr -= our_exec_load_addr;
    raw_thread_func_addr += test_state->inferior_exec_load_addr;
#if defined(__x86_64__)
    regs.rdx = regs.rip;
    regs.rip = raw_thread_func_addr;
    regs.rcx = kMagicRegisterValue;
#elif defined(__aarch64__)
    regs.r[2] = regs.pc;
    regs.pc = raw_thread_func_addr;
    regs.r[3] = kMagicRegisterValue;
#elif defined(__riscv)
    regs.a2 = regs.pc;
    regs.pc = raw_thread_func_addr;
    regs.a1 = kMagicRegisterValue;
#endif
  }

  write_inferior_gregs(thread.get(), &regs);
}

// N.B. This runs on the wait-inferior thread.

void thread_start_test_exception_handler_worker(inferior_data_t* data,
                                                const zx_port_packet_t* packet, void* handler_arg) {
  auto test_state = reinterpret_cast<reg_access_test_state_t*>(handler_arg);

  zx_info_handle_basic_t basic_info;
  zx_status_t status = zx_object_get_info(data->exception_channel, ZX_INFO_HANDLE_BASIC,
                                          &basic_info, sizeof(basic_info), nullptr, nullptr);
  ASSERT_EQ(status, ZX_OK);
  zx_koid_t exception_channel_koid = basic_info.koid;

  if (packet->key != exception_channel_koid) {
    zx_object_get_info(data->inferior, ZX_INFO_HANDLE_BASIC, &basic_info, sizeof(basic_info),
                       nullptr, nullptr);
    ASSERT_EQ(status, ZX_OK);
    zx_koid_t inferior_koid = basic_info.koid;
    ASSERT_TRUE(packet->key != inferior_koid);
    // Must be a signal on one of the threads.
    // Here we're only expecting TERMINATED.
    ASSERT_TRUE(packet->signal.observed & ZX_THREAD_TERMINATED);
  } else {
    zx::exception exception;
    zx_exception_info_t info;
    uint32_t num_bytes = sizeof(info);
    uint32_t num_handles = 1;
    status = zx_channel_read(data->exception_channel, 0, &info, exception.reset_and_get_address(),
                             num_bytes, num_handles, nullptr, nullptr);
    ASSERT_EQ(status, ZX_OK);

    switch (info.type) {
      case ZX_EXCP_THREAD_STARTING:
        printf("wait-inf: thread %lu started\n", info.tid);
        test_thread_start_register_access(test_state, data->inferior, info.tid);
        break;

      case ZX_EXCP_THREAD_EXITING:
        handle_thread_exiting(data->inferior, &info, std::move(exception));
        break;

      default: {
        ASSERT_TRUE(false, "unexpected exception type: 0x%x", info.type);
        __UNREACHABLE;
      }
    }
  }
}

// N.B. This runs on the wait-inferior thread.

void thread_start_test_exception_handler(inferior_data_t* data, const zx_port_packet_t* packet,
                                         void* handler_arg) {
  thread_start_test_exception_handler_worker(data, packet, handler_arg);

  // If a test failed detach now so that a thread isn't left waiting in
  // ZX_EXCP_THREAD_STARTING for a response.
  if (CURRENT_TEST_HAS_FATAL_FAILURE()) {
    unbind_inferior(data);
  }
}

}  // namespace

int capture_regs_thread_func(void* arg) {
  auto thread_count_ptr = reinterpret_cast<std::atomic<int>*>(arg);
  atomic_fetch_add(thread_count_ptr, 1);
  printf("Extra thread started.\n");
  return 0;
}

TEST(ThreadStartTests, StoppedInThreadStartingRegAccessTest) {
  springboard_t* sb;
  zx_handle_t inferior, channel;
  ASSERT_NO_FATAL_FAILURE(setup_inferior(kTestInferiorChildName, &sb, &inferior, &channel));

  // Attach to the inferior now because we want to see thread starting
  // exceptions.
  zx_handle_t port = ZX_HANDLE_INVALID;
  EXPECT_EQ(zx_port_create(0, &port), ZX_OK);
  size_t max_threads = 10;
  inferior_data_t* inferior_data = attach_inferior(inferior, port, max_threads);

  // State we need to maintain across the handling of the various exceptions.
  reg_access_test_state_t test_state{};

  thrd_t wait_inf_thread =
      start_wait_inf_thread(inferior_data, thread_start_test_exception_handler, &test_state);
  EXPECT_NE(port, ZX_HANDLE_INVALID);

  ASSERT_NO_FATAL_FAILURE(start_inferior(sb));

  // The first test happens here as the main thread starts.
  // This testing is done in |thread_start_test_exception_handler()|.

  // Make sure the program successfully started.
  ASSERT_NO_FATAL_FAILURE(verify_inferior_running(channel));

  get_inferior_load_addrs(channel, &test_state.inferior_libc_load_addr,
                          &test_state.inferior_exec_load_addr);

  // Now that we have the inferior's libc load address we can verify the
  // executable's initial PC value (which is libc's entry point).
  // The inferior executable is us, so we can compute its entry point by
  // adding the offset of the entry point from our load address to the
  // inferior's load address.
  zx_vaddr_t expected_entry_point = test_state.inferior_libc_load_addr + get_libc_entry_point();
  EXPECT_EQ(test_state.inferior_libc_entry_point, expected_entry_point, "");

  send_simple_request(channel, RQST_START_LOOPING_THREADS);
  recv_simple_response(channel, RESP_THREADS_STARTED);

  // The remaining testing happens at this point as threads start.
  // This testing is done in |thread_start_test_exception_handler()|.

  ASSERT_NO_FATAL_FAILURE(shutdown_inferior(channel, inferior));

  // Stop the waiter thread before closing the port that it's waiting on.
  join_wait_inf_thread(wait_inf_thread);

  detach_inferior(inferior_data, true);

  zx_handle_close(port);
  zx_handle_close(channel);
  zx_handle_close(inferior);
}
