// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_RISCV64_INCLUDE_ARCH_RISCV64_MMU_H_
#define ZIRCON_KERNEL_ARCH_RISCV64_INCLUDE_ARCH_RISCV64_MMU_H_

#include <lib/zircon-internal/macros.h>

#include <arch/defines.h>
#include <arch/kernel_aspace.h>

// This macros is needed for building the tests.
#define MMU_LX_X(page_shift, level) ((3 - (level)) * ((page_shift)-3) + 3)

// These macros assume the sv39 virtual memory scheme which maps 39-bit
// virtual addresses to 56-bit physical addresses.  For details see sections
// 4.1.12, 4.2, and 4.3 in the RISC-V Privileged Spec.
//
// https://github.com/riscv/riscv-isa-manual/releases/download/Ratified-IMFDQC-and-Priv-v1.11/riscv-privileged-20190608.pdf

#define MMU_KERNEL_PAGE_SIZE_SHIFT (PAGE_SIZE_SHIFT)
#define MMU_USER_PAGE_SIZE_SHIFT 12
#define MMU_USER_SIZE_SHIFT 39

#define MMU_GUEST_SIZE_SHIFT 36
#define MMU_GUEST_PAGE_SIZE_SHIFT (USER_PAGE_SIZE_SHIFT)

#define RISCV64_MMU_PT_LEVELS 3
#define RISCV64_MMU_PT_SHIFT 9
#define RISCV64_MMU_PT_ENTRIES 512  // 1 << PT_SHIFT
#define RISCV64_MMU_PT_KERNEL_BASE_INDEX (RISCV64_MMU_PT_ENTRIES / 2)
#define RISCV64_MMU_CANONICAL_MASK ((1ul << 48) - 1)
#define RISCV64_MMU_PPN_BITS 56

// page table bits
#define RISCV64_PTE_V (1ul << 0)  // valid
#define RISCV64_PTE_R (1ul << 1)  // read
#define RISCV64_PTE_W (1ul << 2)  // write
#define RISCV64_PTE_X (1ul << 3)  // execute
#define RISCV64_PTE_PERM_MASK (7ul << 1)
#define RISCV64_PTE_U (1ul << 4)         // user
#define RISCV64_PTE_G (1ul << 5)         // global
#define RISCV64_PTE_A (1ul << 6)         // accessed
#define RISCV64_PTE_D (1ul << 7)         // dirty
#define RISCV64_PTE_RSW_MASK (3ul << 8)  // reserved for software
#define RISCV64_PTE_PPN_SHIFT (10)
#define RISCV64_PTE_PPN_MASK \
  (((1ul << (RISCV64_MMU_PPN_BITS - PAGE_SIZE_SHIFT)) - 1) << RISCV64_PTE_PPN_SHIFT)

// riscv PPN is stored shifed over 2 from the natural alignment
#define RISCV64_PTE_IS_VALID(pte) ((pte)&RISCV64_PTE_V)
#define RISCV64_PTE_IS_LEAF(pte) ((pte)&RISCV64_PTE_PERM_MASK)

#define RISCV64_PTE_PPN(pte) \
  (((pte)&RISCV64_PTE_PPN_MASK) << (PAGE_SIZE_SHIFT - RISCV64_PTE_PPN_SHIFT))
#define RISCV64_PTE_PPN_TO_PTE(paddr) (((paddr) >> PAGE_SIZE_SHIFT) << RISCV64_PTE_PPN_SHIFT)

// SATP register, contains the current mmu mode, address space id, and
// pointer to root page table
#define RISCV64_SATP_MODE_NONE (0ul)
#define RISCV64_SATP_MODE_SV32 (1ul)
#define RISCV64_SATP_MODE_SV39 (8ul)
#define RISCV64_SATP_MODE_SV48 (9ul)
#define RISCV64_SATP_MODE_SV57 (10ul)
#define RISCV64_SATP_MODE_SV64 (11ul)

#define RISCV64_SATP_MODE_SHIFT (60)
#define RISCV64_SATP_ASID_SHIFT (44)
#define RISCV64_SATP_ASID_SIZE (16)
#define RISCV64_SATP_ASID_MASK ((1ul << RISCV64_SATP_ASID_SIZE) - 1)

#ifndef __ASSEMBLER__

#include <inttypes.h>
#include <sys/types.h>

typedef uintptr_t pte_t;

const size_t MMU_RISCV64_ASID_BITS = 16;
const uint16_t MMU_RISCV64_GLOBAL_ASID = (1u << MMU_RISCV64_ASID_BITS) - 1;
const uint16_t MMU_RISCV64_UNUSED_ASID = 0;
const uint16_t MMU_RISCV64_FIRST_USER_ASID = 1;
const uint16_t MMU_RISCV64_MAX_USER_ASID = MMU_RISCV64_GLOBAL_ASID - 1;

void riscv64_mmu_early_init();
void riscv64_mmu_init();

#endif  // __ASSEMBLER__

#endif  // ZIRCON_KERNEL_ARCH_RISCV64_INCLUDE_ARCH_RISCV64_MMU_H_
