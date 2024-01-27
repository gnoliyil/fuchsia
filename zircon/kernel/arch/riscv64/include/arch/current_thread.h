// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_RISCV64_INCLUDE_ARCH_CURRENT_THREAD_H_
#define ZIRCON_KERNEL_ARCH_RISCV64_INCLUDE_ARCH_CURRENT_THREAD_H_

#include <stdint.h>

// Routines to directly access the current thread pointer out of the current
// CPU structure pointed the tp register.

// NOTE: must be included after the definition of Thread due to the offsetof
// applied in the following routines.

// Use the cpu local thread context pointer to store current_thread.
static inline Thread* arch_get_current_thread() {
  uint8_t* tp = static_cast<uint8_t*>(__builtin_thread_pointer());

  // The Thread structure isn't standard layout, but it's "POD enough"
  // for us to rely on computing this member offset via offsetof.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
  tp -= offsetof(Thread, arch_.thread_pointer_location);
#pragma GCC diagnostic pop

  return reinterpret_cast<Thread*>(tp);
}

static inline void arch_set_current_thread(Thread* t) {
  __asm__("mv tp, %0" : : "r"(&t->arch().thread_pointer_location));
}

#endif  // ZIRCON_KERNEL_ARCH_RISCV64_INCLUDE_ARCH_CURRENT_THREAD_H_
