// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fit/defer.h>
#include <lib/zx/exception.h>
#include <lib/zx/process.h>
#include <lib/zx/thread.h>
#include <lib/zx/vmar.h>
#include <lib/zx/vmo.h>
#include <zircon/syscalls-next.h>
#include <zircon/syscalls/debug.h>
#include <zircon/syscalls/exception.h>
#include <zircon/testonly-syscalls.h>

#include <thread>

#include <zxtest/zxtest.h>

#if defined(__x86_64__) || defined(__aarch64__)

extern "C" void vectab();
extern "C" void syscall_bounce();
extern "C" void syscall_bounce_post_syscall();
extern "C" void exception_bounce();
extern "C" void exception_bounce_exception_address();
// TODO(https://fxb/121511): We need this to break out of exception_bounce. We
// should be able to remove this once kick is implemented.
extern "C" void exception_bounce_exit();
extern "C" zx_status_t restricted_enter_wrapper(uint32_t options, uintptr_t vector_table,
                                                zx_restricted_reason_t* reason_code);

// The normal-mode view of restricted-mode state will change slightly depending on
// if the exit to normal-mode was caused by a syscall or an exception.
enum class RegisterMutation {
  kFromSyscall,
  kFromException,
};

#if defined(__x86_64__)

class ArchRegisterState {
 public:
  static void WritePcToGeneralRegs(zx_thread_state_general_regs_t& regs, uint64_t rip) {
    regs.rip = rip;
  }
  void InitializeRegisters() {
    state_.flags = 0;
    state_.rax = 0x0101010101010101;
    state_.rbx = 0x0202020202020202;
    state_.rcx = 0x0303030303030303;
    state_.rdx = 0x0404040404040404;
    state_.rsi = 0x0505050505050505;
    state_.rdi = 0x0606060606060606;
    state_.rbp = 0x0707070707070707;
    state_.rsp = 0x0808080808080808;
    state_.r8 = 0x0909090909090909;
    state_.r9 = 0x0a0a0a0a0a0a0a0a;
    state_.r10 = 0x0b0b0b0b0b0b0b0b;
    state_.r11 = 0x0c0c0c0c0c0c0c0c;
    state_.r12 = 0x0d0d0d0d0d0d0d0d;
    state_.r13 = 0x0e0e0e0e0e0e0e0e;
    state_.r14 = 0x0f0f0f0f0f0f0f0f;
    state_.r15 = 0x1010101010101010;
    state_.fs_base = reinterpret_cast<uintptr_t>(&fs_val_);
    state_.gs_base = reinterpret_cast<uintptr_t>(&gs_val_);
    fs_val_ = 0;
    gs_val_ = 0;
  }

  void InitializeFromThreadState(const zx_thread_state_general_regs_t& regs) {
    state_.flags = regs.rflags;
    state_.rax = regs.rax;
    state_.rbx = regs.rbx;
    state_.rcx = regs.rcx;
    state_.rdx = regs.rdx;
    state_.rsi = regs.rsi;
    state_.rdi = regs.rdi;
    state_.rbp = regs.rbp;
    state_.rsp = regs.rsp;
    state_.r8 = regs.r8;
    state_.r9 = regs.r9;
    state_.r10 = regs.r10;
    state_.r11 = regs.r11;
    state_.r12 = regs.r12;
    state_.r13 = regs.r13;
    state_.r14 = regs.r14;
    state_.r15 = regs.r15;
    state_.fs_base = regs.fs_base;
    state_.gs_base = regs.gs_base;
    state_.ip = regs.rip;
  }

  void VerifyTwiddledRestrictedState(RegisterMutation mutation) const {
    // Validate the state of the registers is what was written inside restricted mode.
    //
    // NOTE: Each of the registers was incremented by one before exiting restricted mode.
    EXPECT_EQ(0x0101010101010102, state_.rax);
    EXPECT_EQ(0x0202020202020203, state_.rbx);
    if (mutation == RegisterMutation::kFromSyscall) {
      EXPECT_EQ(0, state_.rcx);  // RCX is trashed by the syscall and set to zero
    } else {
      EXPECT_EQ(0x0303030303030304, state_.rcx);
    }
    EXPECT_EQ(0x0404040404040405, state_.rdx);
    EXPECT_EQ(0x0505050505050506, state_.rsi);
    EXPECT_EQ(0x0606060606060607, state_.rdi);
    EXPECT_EQ(0x0707070707070708, state_.rbp);
    EXPECT_EQ(0x0808080808080809, state_.rsp);
    EXPECT_EQ(0x090909090909090a, state_.r8);
    EXPECT_EQ(0x0a0a0a0a0a0a0a0b, state_.r9);
    EXPECT_EQ(0x0b0b0b0b0b0b0b0c, state_.r10);
    if (mutation == RegisterMutation::kFromSyscall) {
      EXPECT_EQ(0, state_.r11);  // r11 is trashed by the syscall and set to zero
    } else {
      EXPECT_EQ(0x0c0c0c0c0c0c0c0d, state_.r11);
    }
    EXPECT_EQ(0x0d0d0d0d0d0d0d0e, state_.r12);
    EXPECT_EQ(0x0e0e0e0e0e0e0e0f, state_.r13);
    EXPECT_EQ(0x0f0f0f0f0f0f0f10, state_.r14);
    EXPECT_EQ(0x1010101010101011, state_.r15);

    // Validate that it was able to write to fs:0 and gs:0 while inside restricted mode the post
    // incremented values of rcx and r11 were written here.
    EXPECT_EQ(0x0303030303030304, fs_val_);
    EXPECT_EQ(0x0c0c0c0c0c0c0c0d, gs_val_);
  }
  uintptr_t ip() { return state_.ip; }
  void set_ip(uintptr_t ip) { state_.ip = ip; }
  zx_restricted_state_t& restricted_state() { return state_; }

 private:
  uint64_t fs_val_ = 0;
  uint64_t gs_val_ = 0;
  zx_restricted_state_t state_{};
};

asm(R"(
.globl vectab
vectab:
  // back from restricted mode
  // rdi holds the context
  // rsi holds the reason code
  mov  %rdi,%rsp
  pop  %rsp
  pop  %r15
  pop  %r14
  pop  %r13
  pop  %r12
  pop  %rbp
  pop  %rbx

  // pop the reason code return slot
  pop  %rdx

  // return the reason code from this function
  mov  %rsi,(%rdx)

  // return back to whatever the address was on the stack
  // make it appear as if the wrapper had returned ZX_OK
  xor  %eax,%eax
  ret
)");

asm(R"(
.globl syscall_bounce
syscall_bounce:
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
.globl syscall_bounce_post_syscall
syscall_bounce_post_syscall:
  jmp 0b
)");

asm(R"(
.globl exception_bounce
exception_bounce:
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

.globl exception_bounce_exception_address
exception_bounce_exception_address:
  ud2
  jmp exception_bounce_exception_address
.globl exception_bounce_exit
exception_bounce_exit:
  syscall
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

#elif defined(__aarch64__)

class ArchRegisterState {
 public:
  static void WritePcToGeneralRegs(zx_thread_state_general_regs_t& regs, uint64_t pc) {
    regs.pc = pc;
  }

  void InitializeRegisters() {
    // Initialize all standard registers to arbitrary values.
    state_.x[0] = 0x0101010101010101;
    state_.x[1] = 0x0202020202020202;
    state_.x[2] = 0x0303030303030303;
    state_.x[3] = 0x0404040404040404;
    state_.x[4] = 0x0505050505050505;
    state_.x[5] = 0x0606060606060606;
    state_.x[6] = 0x0707070707070707;
    state_.x[7] = 0x0808080808080808;
    state_.x[8] = 0x0909090909090909;
    state_.x[9] = 0x0a0a0a0a0a0a0a0a;
    state_.x[10] = 0x0b0b0b0b0b0b0b0b;
    state_.x[11] = 0x0c0c0c0c0c0c0c0c;
    state_.x[12] = 0x0d0d0d0d0d0d0d0d;
    state_.x[13] = 0x0e0e0e0e0e0e0e0e;
    state_.x[14] = 0x0f0f0f0f0f0f0f0f;
    state_.x[15] = 0x0101010101010101;
    state_.x[16] = 0x0202020202020202;
    state_.x[17] = 0x0303030303030303;
    state_.x[18] = 0x0404040404040404;
    state_.x[19] = 0x0505050505050505;
    state_.x[20] = 0x0606060606060606;
    state_.x[21] = 0x0707070707070707;
    state_.x[22] = 0x0808080808080808;
    state_.x[23] = 0x0909090909090909;
    state_.x[24] = 0x0a0a0a0a0a0a0a0a;
    state_.x[25] = 0x0b0b0b0b0b0b0b0b;
    state_.x[26] = 0x0c0c0c0c0c0c0c0c;
    state_.x[27] = 0x0d0d0d0d0d0d0d0d;
    state_.x[28] = 0x0e0e0e0e0e0e0e0e;
    state_.x[29] = 0x0f0f0f0f0f0f0f0f;
    state_.x[30] = 0x0101010101010101;
    state_.sp = 0x0808080808080808;
    state_.cpsr = 0;

    // Initialize a new thread local storage for the restricted mode routine.
    state_.tpidr_el0 = reinterpret_cast<uintptr_t>(&tls_val_);
  }

  void InitializeFromThreadState(const zx_thread_state_general_regs_t& regs) {
    static_assert(sizeof(regs.r) <= sizeof(state_.x));
    memcpy(state_.x, regs.r, sizeof(regs.r));
    state_.x[30] = regs.lr;
    state_.pc = regs.pc;
    state_.tpidr_el0 = regs.tpidr;
    state_.sp = regs.sp;
    state_.cpsr = static_cast<uint32_t>(regs.cpsr);
  }

  void VerifyTwiddledRestrictedState(RegisterMutation mutation) const {
    // Validate the state of the registers is what was written inside restricted mode.
    //
    // NOTE: Each of the registers was incremented by one before exiting restricted mode.
    // x0 was used as temp space by syscall_bounce, so skip that one.
    EXPECT_EQ(0x0202020202020203, state_.x[1]);
    EXPECT_EQ(0x0303030303030304, state_.x[2]);
    EXPECT_EQ(0x0404040404040405, state_.x[3]);
    EXPECT_EQ(0x0505050505050506, state_.x[4]);
    EXPECT_EQ(0x0606060606060607, state_.x[5]);
    EXPECT_EQ(0x0707070707070708, state_.x[6]);
    EXPECT_EQ(0x0808080808080809, state_.x[7]);
    EXPECT_EQ(0x090909090909090a, state_.x[8]);
    EXPECT_EQ(0x0a0a0a0a0a0a0a0b, state_.x[9]);
    EXPECT_EQ(0x0b0b0b0b0b0b0b0c, state_.x[10]);
    EXPECT_EQ(0x0c0c0c0c0c0c0c0d, state_.x[11]);
    EXPECT_EQ(0x0d0d0d0d0d0d0d0e, state_.x[12]);
    EXPECT_EQ(0x0e0e0e0e0e0e0e0f, state_.x[13]);
    EXPECT_EQ(0x0f0f0f0f0f0f0f10, state_.x[14]);
    EXPECT_EQ(0x0101010101010102, state_.x[15]);
    if (mutation == RegisterMutation::kFromSyscall) {
      EXPECT_EQ(0x40, state_.x[16]);  // syscall_bounce ran syscall 0x40
    } else {
      EXPECT_EQ(0x0202020202020203, state_.x[16]);
    }
    EXPECT_EQ(0x0303030303030304, state_.x[17]);
    EXPECT_EQ(0x0404040404040405, state_.x[18]);
    EXPECT_EQ(0x0505050505050506, state_.x[19]);
    EXPECT_EQ(0x0606060606060607, state_.x[20]);
    EXPECT_EQ(0x0707070707070708, state_.x[21]);
    EXPECT_EQ(0x0808080808080809, state_.x[22]);
    EXPECT_EQ(0x090909090909090a, state_.x[23]);
    EXPECT_EQ(0x0a0a0a0a0a0a0a0b, state_.x[24]);
    EXPECT_EQ(0x0b0b0b0b0b0b0b0c, state_.x[25]);
    EXPECT_EQ(0x0c0c0c0c0c0c0c0d, state_.x[26]);
    EXPECT_EQ(0x0d0d0d0d0d0d0d0e, state_.x[27]);
    EXPECT_EQ(0x0e0e0e0e0e0e0e0f, state_.x[28]);
    EXPECT_EQ(0x0f0f0f0f0f0f0f10, state_.x[29]);
    EXPECT_EQ(0x0101010101010102, state_.x[30]);
    EXPECT_EQ(0x0808080808080809, state_.sp);

    // Check that thread local storage was updated correctly in restricted mode.
    EXPECT_EQ(0x0202020202020203, tls_val_);
  }
  uintptr_t ip() { return state_.pc; }
  void set_ip(uintptr_t ip) { state_.pc = ip; }
  zx_restricted_state_t& restricted_state() { return state_; }

 private:
  uint64_t tls_val_ = 0;
  zx_restricted_state_t state_{};
};

asm(R"(
.globl vectab
vectab:
  // Back from restricted mode
  // x0 holds the context, which is the stack pointer
  // x1 holds the reason code

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

  // Load the shadow call stack pointer and reason code pointer.
  ldp x2, x18, [sp], #16

  // Return the reason code from this function by setting the reason code pointer.
  str  x1, [x2]

  // Return back to whatever the address was in the link register.
  // Make it appear as if the wrapper had returned ZX_OK
  mov  x0, xzr
  ret
)");

asm(R"(
.globl syscall_bounce
syscall_bounce:
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
.globl syscall_bounce_post_syscall
syscall_bounce_post_syscall:
  bl syscall_bounce
)");

asm(R"(
.globl exception_bounce
exception_bounce:
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
.globl exception_bounce_exception_address
exception_bounce_exception_address:
  udf #0
  b exception_bounce_exception_address
.globl exception_bounce_exit
exception_bounce_exit:
  // Perform a syscall to exit. This is needed if we're using channel-based
  // exception handlers.
  mov x16, xzr
  add x16, x16, #64
  svc #0
)");

asm(R"(
.globl restricted_enter_wrapper
restricted_enter_wrapper:
  // Args 0 - 1 are already in place in X0 and X1.

  // Save the reason code pointer and shadow call stack pointer on the stack.
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

#endif

// Verify that restricted_enter handles invalid args.
TEST(RestrictedMode, EnterInvalidArgs) {
  // Invalid options.
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, zx_restricted_enter(0xffffffff, 0, 0));

  // In-thread exceptions are not yet implemented (no ZX_RESTRICTED_OPT_EXCEPTION_CHANNEL).
  EXPECT_EQ(ZX_ERR_NOT_SUPPORTED, zx_restricted_enter(0, 0, 0));

  // Enter restricted mode with invalid args.
  // Vector table must be valid user pointer.
  EXPECT_EQ(ZX_ERR_INVALID_ARGS, zx_restricted_enter(ZX_RESTRICTED_OPT_EXCEPTION_CHANNEL, -1, 0));
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
  ASSERT_OK(zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, vmo, 0,
                                       zx_system_get_page_size(), &ptr));
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
  zx::vmar::root_self()->unmap(ptr, zx_system_get_page_size());
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

// This is the happy case.
TEST(RestrictedMode, Basic) {
  zx::vmo vmo;
  ASSERT_OK(zx_restricted_bind_state(0, vmo.reset_and_get_address()));
  auto cleanup = fit::defer([]() { EXPECT_OK(zx_restricted_unbind_state(0)); });

  // Configure the initial register state.
  ArchRegisterState state;
  state.InitializeRegisters();

  // Set the PC to the syscall_bounce routine, as the PC is where zx_restricted_enter
  // will jump to.
  state.set_ip(reinterpret_cast<uint64_t>(syscall_bounce));

  // Write the state to the state VMO.
  ASSERT_OK(vmo.write(&state.restricted_state(), 0, sizeof(state.restricted_state())));

  // Enter restricted mode with reasonable args, expect a bounce back.
  zx_restricted_reason_t reason_code = 99;
  ASSERT_OK(restricted_enter_wrapper(ZX_RESTRICTED_OPT_EXCEPTION_CHANNEL, (uintptr_t)vectab,
                                     &reason_code));
  ASSERT_EQ(ZX_RESTRICTED_REASON_SYSCALL, reason_code);

  // Read the state out of the thread.
  ASSERT_OK(vmo.read(&state.restricted_state(), 0, sizeof(state.restricted_state())));

  // Validate that the instruction pointer is right after the syscall instruction.
  EXPECT_EQ((uintptr_t)&syscall_bounce_post_syscall, state.ip());
  state.VerifyTwiddledRestrictedState(RegisterMutation::kFromSyscall);
}

void ReadExceptionFromChannel(const zx::channel& exception_channel,
                              zx_thread_state_general_regs_t& general_regs,
                              std::optional<uintptr_t> new_pc) {
  zx_signals_t pending;
  ASSERT_OK(exception_channel.wait_one(ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                       zx::time::infinite(), &pending));
  ASSERT_NE(0, pending & ZX_CHANNEL_READABLE, "exception channel peer closed (pending 0x%08x)",
            pending);

  // This test will cover the 'worst case' exception latency, so we read all the data that we
  // can also access when using in-thread exception handlers. In other words we read all the
  // data that we would otherwise get from the restricted mode state VMO when using in-thread
  // exception handling.
  zx::exception exception;
  zx_exception_info_t exc_info;
  uint32_t nbytes, nhandles;
  ASSERT_OK(exception_channel.read(0, &exc_info, exception.reset_and_get_address(),
                                   sizeof(exc_info), 1, &nbytes, &nhandles));

  zx::thread thread;
  ASSERT_OK(exception.get_thread(&thread));

  zx_exception_report_t report = {};
  ASSERT_OK(
      thread.get_info(ZX_INFO_THREAD_EXCEPTION_REPORT, &report, sizeof(report), nullptr, nullptr));
  ASSERT_OK(thread.read_state(ZX_THREAD_STATE_GENERAL_REGS, &general_regs, sizeof(general_regs)));

  // Update the PC if necessary.
  if (new_pc.has_value()) {
    ArchRegisterState::WritePcToGeneralRegs(general_regs, *new_pc);
  }
  ASSERT_OK(thread.write_state(ZX_THREAD_STATE_GENERAL_REGS, &general_regs, sizeof(general_regs)));
  constexpr uint32_t kExceptionState = ZX_EXCEPTION_STATE_HANDLED;
  ASSERT_OK(
      exception.set_property(ZX_PROP_EXCEPTION_STATE, &kExceptionState, sizeof(kExceptionState)));
}

// This is a simple benchmark test that prints some rough performance numbers.
TEST(RestrictedMode, Bench) {
  // Run the test 5 times to help filter out noise.
  for (auto i = 0; i < 5; i++) {
    zx::vmo vmo;
    ASSERT_OK(zx_restricted_bind_state(0, vmo.reset_and_get_address()));
    auto cleanup = fit::defer([]() { EXPECT_OK(zx_restricted_unbind_state(0)); });

    // Set the state.
    ArchRegisterState state;
    state.InitializeRegisters();
    state.set_ip(reinterpret_cast<uintptr_t>(syscall_bounce));
    ASSERT_OK(vmo.write(&state.restricted_state(), 0, sizeof(state.restricted_state())));

    // Go through a full restricted syscall entry/exit cycle iter times and show the time.
    {
      constexpr int iter = 1000000;  // about a second worth of iterations on a mid range x86
      zx_restricted_reason_t reason_code;
      auto t = zx::ticks::now();
      for (int i = 0; i < iter; i++) {
        ASSERT_OK(restricted_enter_wrapper(ZX_RESTRICTED_OPT_EXCEPTION_CHANNEL, (uintptr_t)vectab,
                                           &reason_code));
      }
      t = zx::ticks::now() - t;

      printf("restricted call %ld ns per round trip (%ld raw ticks)\n",
             t / iter * ZX_SEC(1) / zx::ticks::per_second(), t.get());
    }

    {
      // For way of comparison, time a null syscall.
      constexpr int iter = 1000000;
      auto t = zx::ticks::now();
      for (int i = 0; i < iter; i++) {
        ASSERT_OK(zx_syscall_test_0());
      }
      t = zx::ticks::now() - t;

      printf("test syscall %ld ns per call (%ld raw ticks)\n",
             t / iter * ZX_SEC(1) / zx::ticks::per_second(), t.get());
    }

    // Perform channel-based exception handling.
    {
      constexpr int iter = 10000;
      auto t = zx::ticks::now();
      zx_restricted_reason_t reason_code;

      zx::channel exception_channel;
      ZX_ASSERT(zx::process::self()->create_exception_channel(0, &exception_channel) == ZX_OK);
      std::thread exception_handler([&]() {
        for (int i = 0; i < iter; ++i) {
          zx_thread_state_general_regs_t general_regs = {};
          ReadExceptionFromChannel(
              exception_channel, general_regs,
              (i == iter - 1)
                  ? std::make_optional(reinterpret_cast<uintptr_t>(exception_bounce_exit))
                  : std::nullopt);
        }
      });
      state.set_ip(reinterpret_cast<uintptr_t>(exception_bounce_exception_address));
      ASSERT_OK(vmo.write(&state.restricted_state(), 0, sizeof(state.restricted_state())));
      t = zx::ticks::now();
      // Iteration happens in the handler thread; we only have a single restricted_enter
      // when using channel-based exception handling.
      ASSERT_OK(restricted_enter_wrapper(ZX_RESTRICTED_OPT_EXCEPTION_CHANNEL, (uintptr_t)vectab,
                                         &reason_code));
      t = zx::ticks::now() - t;
      exception_handler.join();

      printf("channel-based exceptions %ld ns per round trip (%ld raw ticks)\n",
             t / iter * ZX_SEC(1) / zx::ticks::per_second(), t.get());
    }
  }
}

// Verify we can receive restricted exceptions via exception channels.
TEST(RestrictedMode, ExceptionChannel) {
  zx::vmo vmo;
  ASSERT_OK(zx_restricted_bind_state(0, vmo.reset_and_get_address()));
  auto cleanup = fit::defer([]() { EXPECT_OK(zx_restricted_unbind_state(0)); });

  // Configure the state for x86.
  ArchRegisterState state;
  state.InitializeRegisters();
  state.set_ip(reinterpret_cast<uint64_t>(exception_bounce));

  // Spawn a thread to receive exceptions.
  zx::channel exception_channel;
  ZX_ASSERT(zx::process::self()->create_exception_channel(0, &exception_channel) == ZX_OK);

  auto restricted_thread = zx::thread::self();

  bool exception_handled = false;
  std::thread exception_thread([&]() {
    zx_thread_state_general_regs_t general_regs = {};
    ReadExceptionFromChannel(exception_channel, general_regs,
                             reinterpret_cast<uintptr_t>(exception_bounce_exit));

    state.InitializeFromThreadState(general_regs);
    state.VerifyTwiddledRestrictedState(RegisterMutation::kFromException);
    exception_handled = true;
  });

  // Enter restricted mode. The restricted code will twiddle some registers
  // and generate an exception.
  ASSERT_OK(vmo.write(&state.restricted_state(), 0, sizeof(state.restricted_state())));
  zx_restricted_reason_t reason_code = 99;
  ASSERT_OK(restricted_enter_wrapper(ZX_RESTRICTED_OPT_EXCEPTION_CHANNEL, (uintptr_t)vectab,
                                     &reason_code));
  exception_thread.join();
  ASSERT_TRUE(exception_handled);
}

// Verify that restricted_enter fails on invalid zx_restricted_state_t values.
TEST(RestrictedMode, EnterBadStateStruct) {
  zx::vmo vmo;
  ASSERT_OK(zx_restricted_bind_state(0, vmo.reset_and_get_address()));
  auto cleanup = fit::defer([]() { EXPECT_OK(zx_restricted_unbind_state(0)); });

  ArchRegisterState state;
  state.InitializeRegisters();

  [[maybe_unused]] auto set_state_and_enter = [&]() {
    // Set the state.
    ASSERT_OK(vmo.write(&state.restricted_state(), 0, sizeof(state.restricted_state())));

    // This should fail with bad state.
    ASSERT_EQ(ZX_ERR_BAD_STATE,
              zx_restricted_enter(ZX_RESTRICTED_OPT_EXCEPTION_CHANNEL, (uintptr_t)&vectab, 0));
  };

  state.set_ip(-1);  // ip is outside of user space
  set_state_and_enter();

#ifdef __x86_64__
  state.InitializeRegisters();
  state.set_ip((uint64_t)syscall_bounce);
  state.restricted_state().flags = (1UL << 31);  // set an invalid flag
  set_state_and_enter();

  state.InitializeRegisters();
  state.set_ip((uint64_t)syscall_bounce);
  state.restricted_state().fs_base = (1UL << 63);  // invalid fs (non canonical)
  set_state_and_enter();

  state.InitializeRegisters();
  state.set_ip((uint64_t)syscall_bounce);
  state.restricted_state().gs_base = (1UL << 63);  // invalid gs (non canonical)
  set_state_and_enter();
#endif

#ifdef __aarch64__
  state.InitializeRegisters();
  state.set_ip((uint64_t)syscall_bounce);
  state.restricted_state().cpsr = 0x1;  // CPSR contains non-user settable flags.
  set_state_and_enter();
#endif
}

#else  // defined(__x86_64__) || defined(__aarch64__)

// Restricted mode is only implemented on x86 and arm64.  See that the syscalls fail on other
// arches.
TEST(RestrictedMode, NotSupported) {
  zx::vmo vmo;
  ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zx_restricted_bind_state(0, vmo.reset_and_get_address()));
  ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zx_restricted_unbind_state(0));
  ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zx_restricted_enter(0, 0, 0));
  ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, zx_restricted_enter(ZX_RESTRICTED_OPT_EXCEPTION_CHANNEL, 0, 0));
}

#endif  // defined(__x86_64__) || defined(__aarch64__)
