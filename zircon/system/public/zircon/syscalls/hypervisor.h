// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSCALLS_HYPERVISOR_H_
#define ZIRCON_SYSCALLS_HYPERVISOR_H_

#include <zircon/types.h>

__BEGIN_CDECLS

// clang-format off
typedef uint32_t zx_guest_option_t;

#define ZX_GUEST_OPT_NORMAL ((zx_guest_option_t) 0u)
#define ZX_GUEST_OPT_DIRECT ((zx_guest_option_t) 1u)

typedef uint32_t zx_guest_trap_t;

#define ZX_GUEST_TRAP_BELL ((zx_guest_trap_t) 0u)
#define ZX_GUEST_TRAP_MEM  ((zx_guest_trap_t) 1u)
#define ZX_GUEST_TRAP_IO   ((zx_guest_trap_t) 2u)

typedef uint32_t zx_vcpu_state_topic_t;

// Rename to ZX_VCPU_STATE_GENERAL_REGS
#define ZX_VCPU_STATE ((zx_vcpu_state_topic_t) 0u)
// Rename to ZX_VCPU_STATE_IO_REGS
#define ZX_VCPU_IO    ((zx_vcpu_state_topic_t) 1u)
// clang-format on

// Rename to zx_vcpu_state_general_regs_t
// Structure to read and write VCPU state.
typedef struct zx_vcpu_state {
#if defined(__aarch64__)
  uint64_t x[31];
  uint64_t sp;
  // Contains only the user-controllable upper 4-bits (NZCV).
  uint32_t cpsr;
  uint8_t padding1[4];
#elif defined(__x86_64__)
  uint64_t rax;
  uint64_t rcx;
  uint64_t rdx;
  uint64_t rbx;
  uint64_t rsp;
  uint64_t rbp;
  uint64_t rsi;
  uint64_t rdi;
  uint64_t r8;
  uint64_t r9;
  uint64_t r10;
  uint64_t r11;
  uint64_t r12;
  uint64_t r13;
  uint64_t r14;
  uint64_t r15;
  // Contains only the user-controllable lower 32-bits.
  uint64_t rflags;
#else
  // Placeholder to avoid warnings about C vs C++ struct size differences.
  uint32_t empty;
#endif
} zx_vcpu_state_t;

// Rename to zx_vcpu_state_io_regs_t
// Structure to read and write VCPU state for IO ports.
typedef struct zx_vcpu_io {
  uint8_t access_size;
  uint8_t padding1[3];
  union {
    struct {
      uint8_t u8;
      uint8_t padding2[3];
    };
    struct {
      uint16_t u16;
      uint8_t padding3[2];
    };
    uint32_t u32;
    uint8_t data[4];
  };
} zx_vcpu_io_t;

__END_CDECLS

#endif  // ZIRCON_SYSCALLS_HYPERVISOR_H_
