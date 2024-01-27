// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_RISCV64_INCLUDE_ARCH_ASPACE_H_
#define ZIRCON_KERNEL_ARCH_RISCV64_INCLUDE_ARCH_ASPACE_H_

// RISC-V arch-specific declarations for VmAspace implementation.

#include <debug.h>

#include <arch/riscv64/mmu.h>
#include <vm/arch_vm_aspace.h>

enum class Riscv64AspaceType {
  kUser,        // Userspace address space.
  kKernel,      // Kernel address space.
  kGuest,       // Second-stage address space.
  kHypervisor,  // Hypervisor address space.
};

class Riscv64ArchVmAspace final : public ArchVmAspaceInterface {
 public:
  Riscv64ArchVmAspace(vaddr_t base, size_t size, Riscv64AspaceType type,
                      page_alloc_fn_t test_paf = nullptr);
  Riscv64ArchVmAspace(vaddr_t base, size_t size, uint mmu_flags,
                      page_alloc_fn_t test_paf = nullptr);
  virtual ~Riscv64ArchVmAspace();

  using ArchVmAspaceInterface::page_alloc_fn_t;

  zx_status_t Init() override;

  void DisableUpdates() override;

  zx_status_t Destroy() override;

  // main methods
  zx_status_t Map(vaddr_t vaddr, paddr_t* phys, size_t count, uint mmu_flags,
                  ExistingEntryAction existing_action, size_t* mapped) override;
  zx_status_t MapContiguous(vaddr_t vaddr, paddr_t paddr, size_t count, uint mmu_flags,
                            size_t* mapped) override;

  zx_status_t Unmap(vaddr_t vaddr, size_t count, EnlargeOperation enlarge,
                    size_t* unmapped) override;

  zx_status_t Protect(vaddr_t vaddr, size_t count, uint mmu_flags) override;

  zx_status_t Query(vaddr_t vaddr, paddr_t* paddr, uint* mmu_flags) override;

  vaddr_t PickSpot(vaddr_t base, vaddr_t end, vaddr_t align, size_t size, uint mmu_flags) override;

  zx_status_t MarkAccessed(vaddr_t vaddr, size_t count) override;

  zx_status_t HarvestAccessed(vaddr_t vaddr, size_t count, NonTerminalAction non_terminal_action,
                              TerminalAction terminal_action) override;

  bool ActiveSinceLastCheck(bool clear) override;

  paddr_t arch_table_phys() const override { return 0; }
  uint16_t arch_asid() const { return 0; }
  void arch_set_asid(uint16_t asid) {}

  static void ContextSwitch(Riscv64ArchVmAspace* from, Riscv64ArchVmAspace* to);

  static constexpr bool HasNonTerminalAccessedFlag() { return false; }

  static constexpr vaddr_t NextUserPageTableOffset(vaddr_t va) {
    // Work out the virtual address the next page table would start at by first masking the va down
    // to determine its index, then adding 1 and turning it back into a virtual address.
    const uint pt_bits = (MMU_USER_PAGE_SIZE_SHIFT - 3);
    const uint page_pt_shift = MMU_USER_PAGE_SIZE_SHIFT + pt_bits;
    return ((va >> page_pt_shift) + 1) << page_pt_shift;
  }

 private:
  class ConsistencyManager;
  inline bool IsValidVaddr(vaddr_t vaddr) const {
    return (vaddr >= base_ && vaddr <= base_ + size_ - 1);
  }

  // Page table management.
  volatile pte_t* GetPageTable(vaddr_t pt_index, volatile pte_t* page_table) TA_REQ(lock_);

  zx_status_t AllocPageTable(paddr_t* paddrp) TA_REQ(lock_);

  void FreePageTable(void* vaddr, paddr_t paddr, ConsistencyManager& cm) TA_REQ(lock_);

  ssize_t MapPageTable(vaddr_t vaddr_in, vaddr_t vaddr_rel, paddr_t paddr_in, size_t size_in,
                       pte_t attrs, uint level, volatile pte_t* page_table, ConsistencyManager& cm)
      TA_REQ(lock_);

  ssize_t UnmapPageTable(vaddr_t vaddr, vaddr_t vaddr_rel, size_t size, uint level,
                         volatile pte_t* page_table, ConsistencyManager& cm) TA_REQ(lock_);

  zx_status_t ProtectPageTable(vaddr_t vaddr, vaddr_t vaddr_rel, size_t size_in, pte_t attrs,
                               uint level, volatile pte_t* page_table, ConsistencyManager& cm)
      TA_REQ(lock_);

  void HarvestAccessedPageTable(vaddr_t vaddr_in, vaddr_t vaddr_rel_in, size_t size_in, uint level,
                                NonTerminalAction action, volatile pte_t* page_table,
                                ConsistencyManager& cm, bool* unmapped_out) TA_REQ(lock_);

  void MarkAccessedPageTable(vaddr_t vaddr, vaddr_t vaddr_rel, size_t size, uint level,
                             volatile pte_t* page_table, ConsistencyManager& cm) TA_REQ(lock_);

  // Splits a descriptor block into a set of next-level-down page blocks/pages.
  //
  // |vaddr| is the virtual address of the start of the block being split. |level|
  // is the level of the page table entry of the descriptor blocking being split.
  // |page_size_shift| is the page size shift of the current aspace. |page_table| is the
  // page table that contains the descriptor block being split, and |pt_index| is the index
  // into that table.
  zx_status_t SplitLargePage(vaddr_t vaddr, uint level, vaddr_t pt_index,
                             volatile pte_t* page_table, ConsistencyManager& cm) TA_REQ(lock_);

  void MmuParamsFromFlags(uint mmu_flags, pte_t* attrs);
  ssize_t MapPages(vaddr_t vaddr, paddr_t paddr, size_t size, pte_t attrs, ConsistencyManager& cm)
      TA_REQ(lock_);

  ssize_t UnmapPages(vaddr_t vaddr, size_t size, ConsistencyManager& cm) TA_REQ(lock_);

  zx_status_t ProtectPages(vaddr_t vaddr, size_t size, pte_t attrs) TA_REQ(lock_);
  zx_status_t QueryLocked(vaddr_t vaddr, paddr_t* paddr, uint* mmu_flags) TA_REQ(lock_);

  void FlushTLBEntry(vaddr_t vaddr, bool terminal) const TA_REQ(lock_);

  void FlushAsid() const TA_REQ(lock_);

  uint MmuFlagsFromPte(pte_t pte);

  // data fields
  fbl::Canary<fbl::magic("VAAS")> canary_;

  DECLARE_MUTEX(Riscv64ArchVmAspace) lock_;

  // Page allocate function, if set will be used instead of the default allocator
  const page_alloc_fn_t test_page_alloc_func_ = nullptr;

  uint16_t asid_ = MMU_RISCV64_UNUSED_ASID;

  // Pointer to the translation table.
  paddr_t tt_phys_ = 0;
  volatile pte_t* tt_virt_ = nullptr;

  // Upper bound of the number of pages allocated to back the translation
  // table.
  size_t pt_pages_ = 0;

  // Type of address space.
  const Riscv64AspaceType type_;

  // Range of address space.
  const vaddr_t base_ = 0;
  const size_t size_ = 0;

  // Number of CPUs this aspace is currently active on.
  ktl::atomic<uint32_t> num_active_cpus_ = 0;

  // Whether not this has been active since |ActiveSinceLastCheck| was called.
  ktl::atomic<bool> active_since_last_check_ = false;
};

class Riscv64VmICacheConsistencyManager final : public ArchVmICacheConsistencyManagerInterface {
 public:
  Riscv64VmICacheConsistencyManager() = default;
  ~Riscv64VmICacheConsistencyManager() override { Finish(); }

  void SyncAddr(vaddr_t start, size_t len) override;
  void Finish() override;

 private:
  bool need_invalidate_ = false;
};

using ArchVmAspace = Riscv64ArchVmAspace;
using ArchVmICacheConsistencyManager = Riscv64VmICacheConsistencyManager;
#endif  // ZIRCON_KERNEL_ARCH_RISCV64_INCLUDE_ARCH_ASPACE_H_
