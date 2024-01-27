// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/component/incoming/cpp/protocol.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fdio.h>
#include <lib/fit/defer.h>
#include <lib/zx/channel.h>
#include <lib/zx/resource.h>
#include <lib/zx/time.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>
#include <zircon/syscalls-next.h>
#include <zircon/syscalls.h>
#include <zircon/testonly-syscalls.h>
#include <zircon/types.h>

#include <thread>

#include <pretty/hexdump.h>
#include <zxtest/zxtest.h>

#if defined(__x86_64__) || defined(__aarch64__)

// Verify that restricted_enter handles invalid args.
TEST(RestrictedMode, EnterInvalidArgs) {
  // Flags must be zero.
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, zx_restricted_enter(0xffffffff, 0, 0));

  // Enter restricted mode with invalid args.
  // Vector table must be valid user pointer.
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, zx_restricted_enter(0, -1, 0));
}

TEST(RestrictedMode, BindState) {
  // Bad options.
  zx::vmo v_invalid;
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx_restricted_bind_state(1, v_invalid.reset_and_get_address()));
  ASSERT_FALSE(v_invalid.is_valid());
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx_restricted_bind_state(2, v_invalid.reset_and_get_address()));
  ASSERT_FALSE(v_invalid.is_valid());
  ASSERT_EQ(ZX_ERR_INVALID_ARGS,
            zx_restricted_bind_state(0xffffffff, v_invalid.reset_and_get_address()));
  ASSERT_FALSE(v_invalid.is_valid());

  // Bad out_handle address.
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx_restricted_bind_state(0, nullptr));
  ASSERT_FALSE(v_invalid.is_valid());

  // Happy case.
  zx::vmo vmo;
  ASSERT_OK(zx_restricted_bind_state(0, vmo.reset_and_get_address()));
  auto cleanup = fit::defer([]() { EXPECT_OK(zx_restricted_unbind_state(0)); });

  // Binding again is fine and replaces any previously bound VMO.
  ASSERT_OK(zx_restricted_bind_state(0, vmo.reset_and_get_address()));
  ASSERT_TRUE(vmo.is_valid());

  // Map the vmo and verify the state follows.
  zx_vaddr_t ptr = 0;
  ASSERT_OK(zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo, 0, getpagesize(),
                                       &ptr));
  zx_restricted_state_t* state2 = reinterpret_cast<zx_restricted_state*>(ptr);

  // Read the state out of the vmo and compare with memory map.
  zx_restricted_state_t state = {};
  ASSERT_OK(vmo.read(&state, 0, sizeof(state)));
  EXPECT_EQ(0, memcmp(state2, &state, sizeof(state)));

  // Fill the state with garbage and make sure it follows.
  memset(state2, 0x99, sizeof(state));
  ASSERT_OK(vmo.read(&state, 0, sizeof(state)));
  EXPECT_EQ(0, memcmp(state2, &state, sizeof(state)));

  // Write garbage via the write syscall and read it back out of the mapping.
  memset(&state, 0x55, sizeof(state));
  ASSERT_OK(vmo.write(&state, 0, sizeof(state)));
  EXPECT_EQ(0, memcmp(state2, &state, sizeof(state)));

  // Teardown the mapping.
  zx::vmar::root_self()->unmap(ptr, getpagesize());
}

TEST(RestrictedMode, UnbindState) {
  // Repeated unbind is OK.
  ASSERT_OK(zx_restricted_unbind_state(0));
  ASSERT_OK(zx_restricted_unbind_state(0));
  ASSERT_OK(zx_restricted_unbind_state(0));

  // Options must be 0.
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx_restricted_unbind_state(1));
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx_restricted_unbind_state(1));
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, zx_restricted_unbind_state(0xffffffff));
}

#endif  // defined(__x86_64__) || defined(__aarch64__)

#if defined(__x86_64__)

asm(R"(
.globl vectab
vectab:
  // back from restricted mode
  // rdi holds the context
  // rsi holds the error code
  mov  %rdi,%rsp
  pop  %rsp
  pop  %r15
  pop  %r14
  pop  %r13
  pop  %r12
  pop  %rbp
  pop  %rbx

  // pop the error code return slot
  pop  %rdx

  // return the error code from this function
  mov  %rax,(%rdx)

  // return back to whatever the address was on the stack
  // make it appear as if the wrapper had returned ZX_OK
  xor  %rax,%rax
  ret
)");

asm(R"(
.globl bounce
bounce:
  // do something to all the registers so we can read
  // the state on the way out
  inc  %rax
  inc  %rbx
  inc  %rcx
  inc  %rdx
  inc  %rsi
  inc  %rdi
  inc  %rbp
  inc  %rsp
  inc  %r8
  inc  %r9
  inc  %r10
  inc  %r11
  inc  %r12
  inc  %r13
  inc  %r14
  inc  %r15

  // write rcx and r11 to fs and gs base since they are both
  // trashed by the syscall. also tests that fs and gs base are
  // set properly.
  mov   %rcx, %fs:0
  mov   %r11, %gs:0

0:
  syscall
.globl bounce_post_syscall
bounce_post_syscall:
  jmp 0b
)");

asm(R"(
.globl restricted_enter_wrapper
restricted_enter_wrapper:
  // args 0 - 1 are already in place in rdi, rsi

  // save the return code pointer on the stack
  push  %rdx

  // save the callee saved regs since the return from restricted mode
  // will zero out all of the registers except rdi and rsi
  push  %rbx
  push  %rbp
  push  %r12
  push  %r13
  push  %r14
  push  %r15
  push  %rsp

  // save the pointer the stack as the context pointer in the syscall
  mov   %rsp,%rdx

  // call the syscall
  call  zx_restricted_enter

  // if we got here it must have failed
  add   $(8*8),%rsp // pop the previous state on the stack
  ret
)");

extern "C" void vectab();
extern "C" void bounce();
extern "C" void bounce_post_syscall();
extern "C" zx_status_t restricted_enter_wrapper(uint32_t options, uintptr_t vector_table,
                                                uint64_t* exit_code);

// This is the happy case.
TEST(RestrictedMode, Basic) {
  zx::vmo vmo;
  ASSERT_OK(zx_restricted_bind_state(0, vmo.reset_and_get_address()));
  auto cleanup = fit::defer([]() { EXPECT_OK(zx_restricted_unbind_state(0)); });

  // Configure the state for x86.
  uint64_t fs_val = 0;
  uint64_t gs_val = 0;
  zx_restricted_state_t state{};
  state.ip = (uint64_t)bounce;
  state.flags = 0;
  state.rax = 0x0101010101010101;
  state.rbx = 0x0202020202020202;
  state.rcx = 0x0303030303030303;
  state.rdx = 0x0404040404040404;
  state.rsi = 0x0505050505050505;
  state.rdi = 0x0606060606060606;
  state.rbp = 0x0707070707070707;
  state.rsp = 0x0808080808080808;
  state.r8 = 0x0909090909090909;
  state.r9 = 0x0a0a0a0a0a0a0a0a;
  state.r10 = 0x0b0b0b0b0b0b0b0b;
  state.r11 = 0x0c0c0c0c0c0c0c0c;
  state.r12 = 0x0d0d0d0d0d0d0d0d;
  state.r13 = 0x0e0e0e0e0e0e0e0e;
  state.r14 = 0x0f0f0f0f0f0f0f0f;
  state.r15 = 0x1010101010101010;
  state.fs_base = (uintptr_t)&fs_val;
  state.gs_base = (uintptr_t)&gs_val;

  // Set the state.
  ASSERT_OK(vmo.write(&state, 0, sizeof(state)));

  // Enter restricted mode with reasonable args, expect a bounce back.
  uint64_t exit_code = 99;
  ASSERT_OK(restricted_enter_wrapper(0, (uintptr_t)vectab, &exit_code));
  ASSERT_EQ(0, exit_code);

  // Read the state out of the thread.
  state = {};
  ASSERT_OK(vmo.read(&state, 0, sizeof(state)));

  // Validate that the instruction pointer is right after the syscall instruction.
  EXPECT_EQ((uintptr_t)&bounce_post_syscall, state.ip);

  // Validate the state of the registers is what was written inside restricted mode.
  //
  // NOTE: Each of the registers was incremented by one before exiting restricted mode.
  EXPECT_EQ(0x0101010101010102, state.rax);
  EXPECT_EQ(0x0202020202020203, state.rbx);
  EXPECT_EQ(0, state.rcx);  // RCX is trashed by the syscall and set to zero
  EXPECT_EQ(0x0404040404040405, state.rdx);
  EXPECT_EQ(0x0505050505050506, state.rsi);
  EXPECT_EQ(0x0606060606060607, state.rdi);
  EXPECT_EQ(0x0707070707070708, state.rbp);
  EXPECT_EQ(0x0808080808080809, state.rsp);
  EXPECT_EQ(0x090909090909090a, state.r8);
  EXPECT_EQ(0x0a0a0a0a0a0a0a0b, state.r9);
  EXPECT_EQ(0x0b0b0b0b0b0b0b0c, state.r10);
  EXPECT_EQ(0, state.r11);  // r11 is trashed by the syscall and set to zero
  EXPECT_EQ(0x0d0d0d0d0d0d0d0e, state.r12);
  EXPECT_EQ(0x0e0e0e0e0e0e0e0f, state.r13);
  EXPECT_EQ(0x0f0f0f0f0f0f0f10, state.r14);
  EXPECT_EQ(0x1010101010101011, state.r15);

  // Validate that it was able to write to fs:0 and gs:0 while inside restricted mode the post
  // incremented values of rcx and r11 were written here.
  EXPECT_EQ(0x0303030303030304, fs_val);
  EXPECT_EQ(0x0c0c0c0c0c0c0c0d, gs_val);
}

// This is a simple benchmark test that prints some rough performance numbers.
TEST(RestrictedMode, Bench) {
  // Run the test 5 times to help filter out noise.
  for (auto i = 0; i < 5; i++) {
    zx::vmo vmo;
    ASSERT_OK(zx_restricted_bind_state(0, vmo.reset_and_get_address()));
    auto cleanup = fit::defer([]() { EXPECT_OK(zx_restricted_unbind_state(0)); });

    // Set the state.
    uint64_t fs_val = 0;
    uint64_t gs_val = 0;
    zx_restricted_state_t state{};
    state.ip = (uint64_t)bounce;
    state.flags = 0;
    state.fs_base = (uintptr_t)&fs_val;
    state.gs_base = (uintptr_t)&gs_val;
    ASSERT_OK(vmo.write(&state, 0, sizeof(state)));

    // Go through a full restricted syscall entry/exit cycle iter times and show the time.
    constexpr int iter = 1000000;  // about a second worth of iterations on a mid range x86
    uint64_t exit_code;
    auto t = zx::ticks::now();
    for (int i = 0; i < iter; i++) {
      ASSERT_OK(restricted_enter_wrapper(0, (uintptr_t)vectab, &exit_code));
    }
    t = zx::ticks::now() - t;

    printf("restricted call %ld ns per round trip (%ld raw ticks)\n",
           t / iter * ZX_SEC(1) / zx::ticks::per_second(), t.get());

    // For way of comparison, time a null syscall.
    t = zx::ticks::now();
    for (int i = 0; i < iter; i++) {
      ASSERT_OK(zx_syscall_test_0());
    }
    t = zx::ticks::now() - t;

    printf("test syscall %ld ns per call (%ld raw ticks)\n",
           t / iter * ZX_SEC(1) / zx::ticks::per_second(), t.get());
  }
}

// Verify that restricted_enter fails on invalid zx_restricted_state_t values.
TEST(RestrictedMode, EnterBadStateStruct) {
  zx::vmo vmo;
  ASSERT_OK(zx_restricted_bind_state(0, vmo.reset_and_get_address()));
  auto cleanup = fit::defer([]() { EXPECT_OK(zx_restricted_unbind_state(0)); });

  zx_restricted_state_t state{};
  [[maybe_unused]] auto set_state_and_enter = [&]() {
    // Set the state.
    ASSERT_OK(vmo.write(&state, 0, sizeof(state)));

    // This should fail with bad state.
    ASSERT_EQ(ZX_ERR_BAD_STATE, zx_restricted_enter(0, (uintptr_t)&vectab, 0));
  };

  state = {};
  state.ip = -1;  // ip is outside of user space
  set_state_and_enter();

  state = {};
  state.ip = (uint64_t)bounce;
  state.flags = (1UL << 31);  // set an invalid flag
  set_state_and_enter();

  state = {};
  state.ip = (uint64_t)bounce;
  state.fs_base = (1UL << 63);  // invalid fs (non canonical)
  set_state_and_enter();

  state = {};
  state.ip = (uint64_t)bounce;
  state.gs_base = (1UL << 63);  // invalid gs (non canonical)
  set_state_and_enter();
}

#elif defined(__aarch64__)  // defined(__x86_64__)

asm(R"(
.globl vectab
vectab:
  // Back from restricted mode
  // x0 holds the context, which is the stack pointer
  // x1 holds the error code

  // Restore the stack pointer at the point of the restricted enter wrapper.
  mov  sp,x0

  // Load the frame pointer and return address from the wrapper.
  ldp x30, x29, [sp], #16

  // Restore the callee saved registers.
  ldp x28, x27, [sp], #16
  ldp x26, x25, [sp], #16
  ldp x24, x23, [sp], #16
  ldp x22, x21, [sp], #16
  ldp x20, x19, [sp], #16

  // Load the shadow call stack pointer and exit code pointer.
  ldp x2, x18, [sp], #16

  // Return the error code from this function by setting the exit code pointer.
  str  x1, [x2]

  // Return back to whatever the address was in the link register.
  // Make it appear as if the wrapper had returned ZX_OK
  mov  x0, xzr
  ret
)");

asm(R"(
.globl bounce
bounce:
  // do something to all the registers so we can read
  // the state on the way out
  add x0, x0, #1
  add x1, x1, #1
  add x2, x2, #1
  add x3, x3, #1
  add x4, x4, #1
  add x5, x5, #1
  add x6, x6, #1
  add x7, x7, #1
  add x8, x8, #1
  add x9, x9, #1
  add x10, x10, #1
  add x11, x11, #1
  add x12, x12, #1
  add x13, x13, #1
  add x14, x14, #1
  add x15, x15, #1
  add x16, x16, #1
  add x17, x17, #1
  add x18, x18, #1
  add x19, x19, #1
  add x20, x20, #1
  add x21, x21, #1
  add x22, x22, #1
  add x23, x23, #1
  add x24, x24, #1
  add x25, x25, #1
  add x26, x26, #1
  add x27, x27, #1
  add x28, x28, #1
  add x29, x29, #1
  add x30, x30, #1
  add sp, sp, #1

  // Save the contents of x16 to TLS prior to running a syscall.
  mrs x0, tpidr_el0
  str x16, [x0]
0:
  mov x16, xzr
  add x16, x16, #64
  svc #0
.globl bounce_post_syscall
bounce_post_syscall:
  bl bounce
)");

asm(R"(
.globl restricted_enter_wrapper
restricted_enter_wrapper:
  // Args 0 - 1 are already in place in X0 and X1.

  // Save the exit code pointer and shadow call stack pointer on the stack.
  stp x2, x18, [sp, #-16]!

  // Save the callee saved regs since the return from restricted mode
  // will modify all registers.
  stp x20, x19, [sp, #-16]!
  stp x22, x21, [sp, #-16]!
  stp x24, x23, [sp, #-16]!
  stp x26, x25, [sp, #-16]!
  stp x28, x27, [sp, #-16]!

  // Save the frame pointer and return address to the stack.
  stp x30, x29, [sp, #-16]!

  // Pass the stack pointer as the context argument to the syscall.
  mov x2, sp

  bl zx_restricted_enter

  // if we got here it must have failed
  add  sp, sp, #(14*8) // pop the previous state on the stack
  ret
)");

extern "C" void vectab();
extern "C" void bounce();
extern "C" void bounce_post_syscall();
extern "C" zx_status_t restricted_enter_wrapper(uint32_t options, uintptr_t vector_table,
                                                uint64_t* exit_code);

// This is the happy case.
TEST(RestrictedMode, Basic) {
  zx::vmo vmo;
  ASSERT_OK(zx_restricted_bind_state(0, vmo.reset_and_get_address()));
  auto cleanup = fit::defer([]() { EXPECT_OK(zx_restricted_unbind_state(0)); });

  // Configure the state for ARM64.
  zx_restricted_state_t state{};

  // Initialize all standard registers to arbitrary values.
  state.x[0] = 0x0101010101010101;
  state.x[1] = 0x0202020202020202;
  state.x[2] = 0x0303030303030303;
  state.x[3] = 0x0404040404040404;
  state.x[4] = 0x0505050505050505;
  state.x[5] = 0x0606060606060606;
  state.x[6] = 0x0707070707070707;
  state.x[7] = 0x0808080808080808;
  state.x[8] = 0x0909090909090909;
  state.x[9] = 0x0a0a0a0a0a0a0a0a;
  state.x[10] = 0x0b0b0b0b0b0b0b0b;
  state.x[11] = 0x0c0c0c0c0c0c0c0c;
  state.x[12] = 0x0d0d0d0d0d0d0d0d;
  state.x[13] = 0x0e0e0e0e0e0e0e0e;
  state.x[14] = 0x0f0f0f0f0f0f0f0f;
  state.x[15] = 0x0101010101010101;
  state.x[16] = 0x0202020202020202;
  state.x[17] = 0x0303030303030303;
  state.x[18] = 0x0404040404040404;
  state.x[19] = 0x0505050505050505;
  state.x[20] = 0x0606060606060606;
  state.x[21] = 0x0707070707070707;
  state.x[22] = 0x0808080808080808;
  state.x[23] = 0x0909090909090909;
  state.x[24] = 0x0a0a0a0a0a0a0a0a;
  state.x[25] = 0x0b0b0b0b0b0b0b0b;
  state.x[26] = 0x0c0c0c0c0c0c0c0c;
  state.x[27] = 0x0d0d0d0d0d0d0d0d;
  state.x[28] = 0x0e0e0e0e0e0e0e0e;
  state.x[29] = 0x0f0f0f0f0f0f0f0f;
  state.x[30] = 0x0101010101010101;
  state.sp = 0x0808080808080808;
  state.cpsr = 0;

  // Initialize a new thread local storage for the restricted mode routine.
  uint64_t tls_val = 0;
  state.tpidr_el0 = (uintptr_t)&tls_val;

  // Set the PC to the bounce routine, as the PC is where zx_restricted_enter
  // will jump to.
  state.pc = (uint64_t)bounce;

  // Write the state to the state VMO.
  ASSERT_OK(vmo.write(&state, 0, sizeof(state)));

  // Enter restricted mode and expect a bounce back.
  uint64_t exit_code = 99;
  ASSERT_OK(restricted_enter_wrapper(0, (uintptr_t)vectab, &exit_code));
  ASSERT_EQ(0, exit_code);

  // Read the state out of the thread.
  state = {};
  ASSERT_OK(vmo.read(&state, 0, sizeof(state)));

  // Validate that the instruction pointer is right after the syscall instruction.
  EXPECT_EQ((uintptr_t)&bounce_post_syscall, state.pc);

  // Validate the state of the registers is what was written inside restricted mode.
  //
  // NOTE: Each of the registers was incremented by one before exiting restricted mode.
  // x0 was used as temp space by bounce, so skip that one.
  EXPECT_EQ(0x0202020202020203, state.x[1]);
  EXPECT_EQ(0x0303030303030304, state.x[2]);
  EXPECT_EQ(0x0404040404040405, state.x[3]);
  EXPECT_EQ(0x0505050505050506, state.x[4]);
  EXPECT_EQ(0x0606060606060607, state.x[5]);
  EXPECT_EQ(0x0707070707070708, state.x[6]);
  EXPECT_EQ(0x0808080808080809, state.x[7]);
  EXPECT_EQ(0x090909090909090a, state.x[8]);
  EXPECT_EQ(0x0a0a0a0a0a0a0a0b, state.x[9]);
  EXPECT_EQ(0x0b0b0b0b0b0b0b0c, state.x[10]);
  EXPECT_EQ(0x0c0c0c0c0c0c0c0d, state.x[11]);
  EXPECT_EQ(0x0d0d0d0d0d0d0d0e, state.x[12]);
  EXPECT_EQ(0x0e0e0e0e0e0e0e0f, state.x[13]);
  EXPECT_EQ(0x0f0f0f0f0f0f0f10, state.x[14]);
  EXPECT_EQ(0x0101010101010102, state.x[15]);
  EXPECT_EQ(0x40, state.x[16]);  // bounce ran syscall 0x40
  EXPECT_EQ(0x0303030303030304, state.x[17]);
  EXPECT_EQ(0x0404040404040405, state.x[18]);
  EXPECT_EQ(0x0505050505050506, state.x[19]);
  EXPECT_EQ(0x0606060606060607, state.x[20]);
  EXPECT_EQ(0x0707070707070708, state.x[21]);
  EXPECT_EQ(0x0808080808080809, state.x[22]);
  EXPECT_EQ(0x090909090909090a, state.x[23]);
  EXPECT_EQ(0x0a0a0a0a0a0a0a0b, state.x[24]);
  EXPECT_EQ(0x0b0b0b0b0b0b0b0c, state.x[25]);
  EXPECT_EQ(0x0c0c0c0c0c0c0c0d, state.x[26]);
  EXPECT_EQ(0x0d0d0d0d0d0d0d0e, state.x[27]);
  EXPECT_EQ(0x0e0e0e0e0e0e0e0f, state.x[28]);
  EXPECT_EQ(0x0f0f0f0f0f0f0f10, state.x[29]);
  EXPECT_EQ(0x0101010101010102, state.x[30]);
  EXPECT_EQ(0x0808080808080809, state.sp);

  // Check that thread local storage was updated correctly in restricted mode.
  EXPECT_EQ(0x0202020202020203, tls_val);
}

// This is a simple benchmark test that prints some rough performance numbers.
TEST(RestrictedMode, Bench) {
  // Run the test 5 times to help filter out noise.
  for (auto i = 0; i < 5; i++) {
    zx::vmo vmo;
    ASSERT_OK(zx_restricted_bind_state(0, vmo.reset_and_get_address()));
    auto cleanup = fit::defer([]() { EXPECT_OK(zx_restricted_unbind_state(0)); });

    // Set the state.
    uint64_t tls_val = 0;
    zx_restricted_state_t state{};
    state.pc = (uint64_t)bounce;
    state.cpsr = 0;
    state.tpidr_el0 = (uintptr_t)&tls_val;
    ASSERT_OK(vmo.write(&state, 0, sizeof(state)));

    // Go through a full restricted syscall entry/exit cycle iter times and show the time.
    constexpr int iter = 1000000;
    uint64_t exit_code;
    auto t = zx::ticks::now();
    for (int i = 0; i < iter; i++) {
      ASSERT_OK(restricted_enter_wrapper(0, (uintptr_t)vectab, &exit_code));
    }
    t = zx::ticks::now() - t;

    printf("restricted call %ld ns per round trip (%ld raw ticks)\n",
           t / iter * ZX_SEC(1) / zx::ticks::per_second(), t.get());

    // For way of comparison, time a null syscall.
    t = zx::ticks::now();
    for (int i = 0; i < iter; i++) {
      ASSERT_OK(zx_syscall_test_0());
    }
    t = zx::ticks::now() - t;

    printf("test syscall %ld ns per call (%ld raw ticks)\n",
           t / iter * ZX_SEC(1) / zx::ticks::per_second(), t.get());
  }
}

// Verify that restricted_enter fails on invalid zx_restricted_state_t values.
TEST(RestrictedMode, EnterBadStateStruct) {
  zx::vmo vmo;
  ASSERT_OK(zx_restricted_bind_state(0, vmo.reset_and_get_address()));
  auto cleanup = fit::defer([]() { EXPECT_OK(zx_restricted_unbind_state(0)); });

  zx_restricted_state_t state{};
  state.pc = -1;  // PC is outside of user space
  ASSERT_OK(vmo.write(&state, 0, sizeof(state)));
  ASSERT_EQ(ZX_ERR_BAD_STATE, zx_restricted_enter(0, (uintptr_t)&vectab, 0));

  state = {};
  state.pc = (uintptr_t)bounce;
  state.cpsr = 0x1;  // CPSR contains non-user settable flags.
  ASSERT_OK(vmo.write(&state, 0, sizeof(state)));
  ASSERT_EQ(ZX_ERR_BAD_STATE, zx_restricted_enter(0, (uintptr_t)&vectab, 0));
}

#else  // defined(__x86_64__)

// Restricted mode is only implemented on x86 and arm64.  See that the syscalls fail on other
// arches.
TEST(RestrictedMode, NotSupported) {
  zx::vmo vmo;
  ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zx_restricted_bind_state(0, vmo.reset_and_get_address()));
  ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zx_restricted_unbind_state(0));
  ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zx_restricted_enter(0, 0, 0));
}

#endif  // defined(__x86_64__)
