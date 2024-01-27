// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSCALLS_DEBUG_H_
#define ZIRCON_SYSCALLS_DEBUG_H_

#include <stdint.h>
#include <zircon/compiler.h>

__BEGIN_CDECLS

// Value for ZX_THREAD_STATE_GENERAL_REGS on x86-64 platforms.
typedef struct zx_x86_64_thread_state_general_regs {
  uint64_t rax;
  uint64_t rbx;
  uint64_t rcx;
  uint64_t rdx;
  uint64_t rsi;
  uint64_t rdi;
  uint64_t rbp;
  uint64_t rsp;
  uint64_t r8;
  uint64_t r9;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
  uint64_t rip;
  uint64_t rflags;
  uint64_t fs_base;
  uint64_t gs_base;
} zx_x86_64_thread_state_general_regs_t;

// Value for ZX_THREAD_STATE_FP_REGS on x64. Holds x87 and MMX state.
typedef struct zx_x86_64_thread_state_fp_regs {
  uint16_t fcw;  // Control word.
  uint16_t fsw;  // Status word.
  uint8_t ftw;   // Tag word.
  uint8_t reserved;
  uint16_t fop;  // Opcode.
  uint64_t fip;  // Instruction pointer.
  uint64_t fdp;  // Data pointer.

  uint8_t padding1[8];

  // The x87/MMX state. For x87 the each "st" entry has the low 80 bits used for the register
  // contents. For MMX, the low 64 bits are used. The higher bits are unused.
  __ALIGNED(16)
  struct {
    uint64_t low;
    uint64_t high;
  } st[8];
} zx_x86_64_thread_state_fp_regs_t;

// Value for ZX_THREAD_STATE_VECTOR_REGS on x64. Holds SSE and AVX registers.
//
// Setting vector registers will only work for threads that have previously executed an
// instruction using the corresponding register class.
typedef struct zx_x86_64_thread_state_vector_regs {
  // When only 16 registers are supported (pre-AVX-512), zmm[16-31] will be 0.
  // YMM registers (256 bits) are v[0-4], XMM registers (128 bits) are v[0-2].
  struct {
    uint64_t v[8];
  } zmm[32];

  // AVX-512 opmask registers. Will be 0 unless AVX-512 is supported.
  uint64_t opmask[8];

  // SIMD control and status register.
  uint32_t mxcsr;

  uint8_t padding1[4];
} zx_x86_64_thread_state_vector_regs_t;

// Value for ZX_THREAD_STATE_DEBUG_REGS on x64 platforms.
typedef struct zx_x86_64_thread_state_debug_regs {
  uint64_t dr[4];
  // DR4 and D5 are not used.
  uint64_t dr6;  // Status register.
  uint64_t dr7;  // Control register.
} zx_x86_64_thread_state_debug_regs_t;

// Value for ZX_THREAD_STATE_GENERAL_REGS on ARM64 platforms.
typedef struct zx_arm64_thread_state_general_regs {
  uint64_t r[30];
  uint64_t lr;
  uint64_t sp;
  uint64_t pc;
  uint64_t cpsr;
  uint64_t tpidr;
} zx_arm64_thread_state_general_regs_t;

// Value for ZX_THREAD_STATE_FP_REGS on ARM64 platforms.
// This is unused because vector state is used for all floating point on ARM64.
typedef struct zx_arm64_thread_state_fp_regs {
  // Avoids sizing differences for empty structs between C and C++.
  uint32_t unused;
} zx_arm64_thread_state_fp_regs_t;

// Value for ZX_THREAD_STATE_VECTOR_REGS on ARM64 platforms.
typedef struct zx_arm64_thread_state_vector_regs {
  uint32_t fpcr;
  uint32_t fpsr;
  struct {
    uint64_t low;
    uint64_t high;
  } v[32];
} zx_arm64_thread_state_vector_regs_t;

// ARMv8-A provides 2 to 16 hardware breakpoint registers.
// The number is obtained by the BRPs field in the EDDFR register.
#define AARCH64_MAX_HW_BREAKPOINTS 16
// ARMv8-A provides 2 to 16 watchpoint breakpoint registers.
// The number is obtained by the WRPs field in the EDDFR register.
#define AARCH64_MAX_HW_WATCHPOINTS 16

// Value for ZX_THREAD_STATE_DEBUG_REGS for ARM64 platforms.
typedef struct zx_arm64_thread_state_debug_regs {
  struct {
    uint32_t dbgbcr;  //  HW Breakpoint Control register.
    uint8_t padding1[4];
    uint64_t dbgbvr;  //  HW Breakpoint Value register.
  } hw_bps[AARCH64_MAX_HW_BREAKPOINTS];
  // Number of HW Breakpoints in the platform.
  // Will be set on read and ignored on write.

  struct {
    uint32_t dbgwcr;  // HW Watchpoint Control register.
    uint8_t padding1[4];
    uint64_t dbgwvr;  // HW Watchpoint Value register.
  } hw_wps[AARCH64_MAX_HW_WATCHPOINTS];

  // Faulting Virtual Address for watchpoint exceptions.
  // Read-only, values are ignored on write.
  uint64_t far;

  // The esr value since the last exception.
  // Read-only, values are ignored on write.
  uint32_t esr;

  // Number of HW Breakpoints/Watchpoints in the platform.
  // Will be set on read and ignored on write.
  uint8_t hw_bps_count;
  uint8_t hw_wps_count;

  uint8_t padding1[2];

} zx_arm64_thread_state_debug_regs_t;

// Value for ZX_THREAD_STATE_GENERAL_REGS on RISC-V platforms.
typedef struct {
  // Note this could also be considered a uint64_t x[32], with the PC in the
  // x[0] slot in place of the always-zero x0 register.  But this uses
  // individual members with the ABI aliases rather than the x<n> names.
  uint64_t pc;
  uint64_t ra;   // x1
  uint64_t sp;   // x2
  uint64_t gp;   // x3
  uint64_t tp;   // x4
  uint64_t t0;   // x5
  uint64_t t1;   // x6
  uint64_t t2;   // x7
  uint64_t s0;   // x8
  uint64_t s1;   // x9
  uint64_t a0;   // x10
  uint64_t a1;   // x11
  uint64_t a2;   // x12
  uint64_t a3;   // x13
  uint64_t a4;   // x14
  uint64_t a5;   // x15
  uint64_t a6;   // x16
  uint64_t a7;   // x17
  uint64_t s2;   // x18
  uint64_t s3;   // x19
  uint64_t s4;   // x20
  uint64_t s5;   // x21
  uint64_t s6;   // x22
  uint64_t s7;   // x23
  uint64_t s8;   // x24
  uint64_t s9;   // x25
  uint64_t s10;  // x26
  uint64_t s11;  // x27
  uint64_t t3;   // x28
  uint64_t t4;   // x29
  uint64_t t5;   // x30
  uint64_t t6;   // x31
} zx_riscv64_thread_state_general_regs_t;

// Value for ZX_THREAD_STATE_FP_REGS on RISC-V platforms.  This represents
// the F, D, and/or Q extension register state.  Each register is always
// represented as 128 bits wide (Q) here, even if hardware only supports 32
// bits(F) or 64 bits (D).  The smaller register values are represented
// with "NaN-boxing", which means all ones in the extra high bits. When the
// hardware does not support wider sizes, then zx_thread_write_state with
// higher bits anything but all ones is an error.
typedef struct {
  __ALIGNED(16) struct {
    uint64_t low;  // aka d<n> register, low 32 bits are f<n> register
    uint64_t high;
  } q[32];
  uint32_t fcsr;
  uint32_t reserved[3];
} zx_riscv64_thread_state_fp_regs_t;

// Value for ZX_THREAD_STATE_VECTOR_REGS on RISC-V platforms.
// Not supported yet.
// In future, see https://github.com/riscv/riscv-v-spec/blob/master/v-spec.adoc
typedef struct {
  // Avoids sizing differences for empty structs between C and C++.
  uint32_t unused;
} zx_riscv64_thread_state_vector_regs_t;

// Value for ZX_THREAD_STATE_DEBUG_REGS on RISC-V platforms.
// This is unused so far.
typedef struct {
  // Avoids sizing differences for empty structs between C and C++.
  uint32_t unused;
} zx_riscv64_thread_state_debug_regs_t;

#if defined(__x86_64__)

typedef zx_x86_64_thread_state_general_regs_t zx_thread_state_general_regs_t;
typedef zx_x86_64_thread_state_fp_regs_t zx_thread_state_fp_regs_t;
typedef zx_x86_64_thread_state_vector_regs_t zx_thread_state_vector_regs_t;
typedef zx_x86_64_thread_state_debug_regs_t zx_thread_state_debug_regs_t;

#elif defined(__aarch64__)

typedef zx_arm64_thread_state_general_regs_t zx_thread_state_general_regs_t;
typedef zx_arm64_thread_state_fp_regs_t zx_thread_state_fp_regs_t;
typedef zx_arm64_thread_state_vector_regs_t zx_thread_state_vector_regs_t;
typedef zx_arm64_thread_state_debug_regs_t zx_thread_state_debug_regs_t;

#elif defined(__riscv)

typedef zx_riscv64_thread_state_general_regs_t zx_thread_state_general_regs_t;
typedef zx_riscv64_thread_state_fp_regs_t zx_thread_state_fp_regs_t;
typedef zx_riscv64_thread_state_vector_regs_t zx_thread_state_vector_regs_t;
typedef zx_riscv64_thread_state_debug_regs_t zx_thread_state_debug_regs_t;

#endif

// Value for ZX_THREAD_STATE_SINGLE_STEP. The value can be 0 (not single-stepping), or 1
// (single-stepping). Other values will give ZX_ERR_INVALID_ARGS.
typedef uint32_t zx_thread_state_single_step_t;

// Possible values for "kind" in zx_thread_read_state and zx_thread_write_state.
typedef uint32_t zx_thread_state_topic_t;
#define ZX_THREAD_STATE_GENERAL_REGS ((uint32_t)0)  // zx_thread_state_general_regs_t value.
#define ZX_THREAD_STATE_FP_REGS ((uint32_t)1)       // zx_thread_state_fp_regs_t value.
#define ZX_THREAD_STATE_VECTOR_REGS ((uint32_t)2)   // zx_thread_state_vector_regs_t value.
#define ZX_THREAD_STATE_DEBUG_REGS ((uint32_t)4)    // zx_thread_state_debug_regs_t value.
#define ZX_THREAD_STATE_SINGLE_STEP ((uint32_t)5)   // zx_thread_state_single_step_t value.

__END_CDECLS

#endif  // ZIRCON_SYSCALLS_DEBUG_H_
