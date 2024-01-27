// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2016 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_X86_INCLUDE_ARCH_ASPACE_H_
#define ZIRCON_KERNEL_ARCH_X86_INCLUDE_ARCH_ASPACE_H_

#include <lib/arch/intrin.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

#include <arch/x86/ioport.h>
#include <arch/x86/mmu.h>
#include <arch/x86/page_tables/page_tables.h>
#include <fbl/algorithm.h>
#include <fbl/canary.h>
#include <ktl/atomic.h>
#include <vm/arch_vm_aspace.h>
#include <vm/pmm.h>

constexpr uint16_t MMU_X86_UNUSED_VPID = 0;

// Implementation of page tables used by x86-64 CPUs.
class X86PageTableMmu final : public X86PageTableBase {
 public:
  using X86PageTableBase::Destroy;
  using X86PageTableBase::Init;

  // Initialize the kernel page table, assigning the given context to it.
  // This X86PageTable will be special in that its mappings will all have
  // the G (global) bit set, and are expected to be aliased across all page
  // tables used in the normal MMU.  See |AliasKernelMappings|.
  zx_status_t InitKernel(void* ctx, page_alloc_fn_t test_paf);

  // Used for normal MMU page tables so they can share the high kernel mapping
  zx_status_t AliasKernelMappings();

 private:
  using X86PageTableBase::ctx_;
  using X86PageTableBase::pages_;
  using X86PageTableBase::phys_;
  using X86PageTableBase::virt_;

  PageTableLevel top_level() final { return PageTableLevel::PML4_L; }
  bool allowed_flags(uint flags) final { return (flags & ARCH_MMU_FLAG_PERM_READ); }
  bool check_paddr(paddr_t paddr) final;
  bool check_vaddr(vaddr_t vaddr) final;
  bool supports_page_size(PageTableLevel level) final;
  IntermediatePtFlags intermediate_flags() final;
  PtFlags terminal_flags(PageTableLevel level, uint flags) final;
  PtFlags split_flags(PageTableLevel level, PtFlags flags) final;
  void TlbInvalidate(PendingTlbInvalidation* pending) final;
  uint pt_flags_to_mmu_flags(PtFlags flags, PageTableLevel level) final;
  bool needs_cache_flushes() final { return false; }

  // If true, all mappings will have the global bit set.
  bool use_global_mappings_ = false;
};

// Implementation of Intel's Extended Page Tables, for use in virtualization.
class X86PageTableEpt final : public X86PageTableBase {
 public:
  using X86PageTableBase::Destroy;
  using X86PageTableBase::Init;

 private:
  PageTableLevel top_level() final { return PageTableLevel::PML4_L; }
  bool allowed_flags(uint flags) final;
  bool check_paddr(paddr_t paddr) final;
  bool check_vaddr(vaddr_t vaddr) final;
  bool supports_page_size(PageTableLevel level) final;
  IntermediatePtFlags intermediate_flags() final;
  PtFlags terminal_flags(PageTableLevel level, uint flags) final;
  PtFlags split_flags(PageTableLevel level, PtFlags flags) final;
  void TlbInvalidate(PendingTlbInvalidation* pending) final;
  uint pt_flags_to_mmu_flags(PtFlags flags, PageTableLevel level) final;
  bool needs_cache_flushes() final { return false; }
};

class X86ArchVmAspace final : public ArchVmAspaceInterface {
 public:
  X86ArchVmAspace(vaddr_t base, size_t size, uint mmu_flags, page_alloc_fn_t test_paf = nullptr);
  virtual ~X86ArchVmAspace();

  using ArchVmAspaceInterface::page_alloc_fn_t;

  zx_status_t Init() override;

  void DisableUpdates() final {
    // This method is no-op on x86 as the feature is only needed on arm64.  See fxbug.dev/79118.
  }

  zx_status_t Destroy() override;

  // main methods
  zx_status_t MapContiguous(vaddr_t vaddr, paddr_t paddr, size_t count, uint mmu_flags,
                            size_t* mapped) override;
  zx_status_t Map(vaddr_t vaddr, paddr_t* phys, size_t count, uint mmu_flags,
                  ExistingEntryAction existing_action, size_t* mapped) override;
  zx_status_t Unmap(vaddr_t vaddr, size_t count, EnlargeOperation enlarge,
                    size_t* unmapped) override;
  zx_status_t Protect(vaddr_t vaddr, size_t count, uint mmu_flags) override;
  zx_status_t Query(vaddr_t vaddr, paddr_t* paddr, uint* mmu_flags) override;

  vaddr_t PickSpot(vaddr_t base, vaddr_t end, vaddr_t align, size_t size, uint mmu_flags) override;

  // On x86 the hardware can always set the accessed bit so we do not need to support the software
  // fault method.
  zx_status_t MarkAccessed(vaddr_t vaddr, size_t count) override { return ZX_ERR_NOT_SUPPORTED; }

  zx_status_t HarvestAccessed(vaddr_t vaddr, size_t count, NonTerminalAction non_terminal_action,
                              TerminalAction terminal_action) override;

  bool ActiveSinceLastCheck(bool clear) override;

  paddr_t arch_table_phys() const override { return pt_->phys(); }
  uint16_t arch_vpid() const { return vpid_; }
  void arch_set_vpid(uint16_t vpid) { vpid_ = vpid; }
  paddr_t pt_phys() const { return pt_->phys(); }
  size_t pt_pages() const { return pt_->pages(); }

  cpu_mask_t active_cpus() const { return active_cpus_.load(); }

  IoBitmap& io_bitmap() { return io_bitmap_; }

  static void ContextSwitch(X86ArchVmAspace* from, X86ArchVmAspace* to);

  static constexpr vaddr_t NextUserPageTableOffset(vaddr_t va) {
    // This logic only works for 'regular' page sizes that match the hardware page sizes.
    static_assert(PAGE_SIZE_SHIFT == 12 || PAGE_SIZE_SHIFT == 21);

    const uint pt_bits = 9;
    const uint page_pt_shift = PAGE_SIZE_SHIFT + pt_bits;
    return ((va >> page_pt_shift) + 1) << page_pt_shift;
  }

 private:
  // Test the vaddr against the address space's range.
  bool IsValidVaddr(vaddr_t vaddr) const { return (vaddr >= base_ && vaddr <= base_ + size_ - 1); }

  // Helper method to mark this aspace active.
  // This exists for clarity of call sites so that the comment explaining why this is done can be in
  // one location.
  void MarkAspaceModified() {
    // If an aspace has been manipulated via a direction operation, then we want to try it
    // equivalent to if it had been active on a CPU, since it may now have active/dirty information.
    active_since_last_check_.store(true, ktl::memory_order_relaxed);
  }

  fbl::Canary<fbl::magic("VAAS")> canary_;
  IoBitmap io_bitmap_;

  // Embedded storage for the object pointed to by |pt_|.
  union {
    alignas(X86PageTableMmu) char mmu[sizeof(X86PageTableMmu)];
    alignas(X86PageTableEpt) char ept[sizeof(X86PageTableEpt)];
  } page_table_storage_;

  // Page allocate function, if set will be used instead of the default allocator
  const page_alloc_fn_t test_page_alloc_func_ = nullptr;

  // This will be either a normal page table or an EPT, depending on whether
  // flags_ includes ARCH_ASPACE_FLAG_GUEST.
  X86PageTableBase* pt_;

  // If this address space has an associated VPID.
  uint16_t vpid_ = MMU_X86_UNUSED_VPID;

  const uint flags_ = 0;

  // Range of address space.
  const vaddr_t base_ = 0;
  const size_t size_ = 0;

  // CPUs that are currently executing in this aspace.
  ktl::atomic<cpu_mask_t> active_cpus_{0};

  // Whether not this has been active since |ActiveSinceLastCheck| was called.
  ktl::atomic<bool> active_since_last_check_ = false;
};

class X86VmICacheConsistencyManager final : public ArchVmICacheConsistencyManagerInterface {
 public:
  X86VmICacheConsistencyManager() = default;
  ~X86VmICacheConsistencyManager() override { Finish(); }

  void SyncAddr(vaddr_t start, size_t len) override { serialize_ = true; }

  void Finish() override {
    if (!serialize_) {
      return;
    }
    arch::SerializeInstructions();
    serialize_ = false;
  }

 private:
  bool serialize_ = false;
};

using ArchVmAspace = X86ArchVmAspace;
using ArchVmICacheConsistencyManager = X86VmICacheConsistencyManager;

#endif  // ZIRCON_KERNEL_ARCH_X86_INCLUDE_ARCH_ASPACE_H_
