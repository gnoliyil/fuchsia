// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_X86_PHYS_INCLUDE_PHYS_ARCH_ARCH_HANDOFF_H_
#define ZIRCON_KERNEL_ARCH_X86_PHYS_INCLUDE_PHYS_ARCH_ARCH_HANDOFF_H_

#include <lib/zbi-format/zbi.h>

#include <ktl/optional.h>

// This holds (or points to) all x86-specific data that is handed off from
// physboot to the kernel proper at boot time.
struct ArchPhysHandoff {
  // ZBI_TYPE_FRAMEBUFFER payload.
  // Framebuffer parameters.
  ktl::optional<zbi_swfb_t> framebuffer;
};

// This is not actually used for real on x86, but needs to be defined.
inline constexpr uint64_t kArchHandoffVirtualAddress = 0;

#endif  // ZIRCON_KERNEL_ARCH_X86_PHYS_INCLUDE_PHYS_ARCH_ARCH_HANDOFF_H_
