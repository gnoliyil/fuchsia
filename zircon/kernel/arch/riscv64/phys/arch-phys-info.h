// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_RISCV64_PHYS_ARCH_PHYS_INFO_H_
#define ZIRCON_KERNEL_ARCH_RISCV64_PHYS_ARCH_PHYS_INFO_H_

#include <stdint.h>

// This is data that physboot et al might need from phys early start-up.
// It's initialized in physload and then referred to by reference elsewhere.
struct ArchPhysInfo {
  uint64_t boot_hart_id;
};

#endif  // ZIRCON_KERNEL_ARCH_RISCV64_PHYS_ARCH_PHYS_INFO_H_
