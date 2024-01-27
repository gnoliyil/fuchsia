// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_ARM64_INCLUDE_ARCH_ARM64_INTERRUPT_H_
#define ZIRCON_KERNEL_ARCH_ARM64_INCLUDE_ARCH_ARM64_INTERRUPT_H_

#ifndef __ASSEMBLER__

#include <ktl/atomic.h>

// override of some routines

// Clear/set both I and A bits to prevent arbitrary nesting of IRQ and SError.
inline void arch_enable_ints() {
  ktl::atomic_signal_fence(ktl::memory_order_seq_cst);
  __asm__ volatile("msr daifclr, #6" ::: "memory");
}

inline void arch_disable_ints() {
  __asm__ volatile("msr daifset, #6" ::: "memory");
  ktl::atomic_signal_fence(ktl::memory_order_seq_cst);
}

inline bool arch_ints_disabled() {
  unsigned long state;

  __asm__ volatile("mrs %0, daif" : "=r"(state));
  state &= (1 << 7);

  return !!state;
}

inline void arch_enable_fiqs() {
  ktl::atomic_signal_fence(ktl::memory_order_seq_cst);
  __asm__ volatile("msr daifclr, #1" ::: "memory");
}

inline void arch_disable_fiqs() {
  __asm__ volatile("msr daifset, #1" ::: "memory");
  ktl::atomic_signal_fence(ktl::memory_order_seq_cst);
}

// XXX
inline bool arch_fiqs_disabled() {
  unsigned long state;

  __asm__ volatile("mrs %0, daif" : "=r"(state));
  state &= (1 << 6);

  return !!state;
}

#endif  // __ASSEMBLER__

#endif  // ZIRCON_KERNEL_ARCH_ARM64_INCLUDE_ARCH_ARM64_INTERRUPT_H_
