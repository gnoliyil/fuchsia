// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <align.h>
#include <zircon/syscalls/hypervisor.h>

#include <arch/x86/apic.h>
#include <arch/x86/feature.h>

#include "vmx_cpu_state_priv.h"

static void ignore_msr(VmxPage* msr_bitmaps_page, bool ignore_writes, uint32_t msr) {
  // From Volume 3, Section 24.6.9.
  uint8_t* msr_bitmaps = msr_bitmaps_page->VirtualAddress<uint8_t>();
  if (msr >= 0xc0000000) {
    msr_bitmaps += 1 << 10;
  }

  uint16_t msr_low = msr & 0x1fff;
  uint16_t msr_byte = msr_low / 8;
  uint8_t msr_bit = msr_low % 8;

  // Ignore reads to the MSR.
  msr_bitmaps[msr_byte] &= static_cast<uint8_t>(~(1u << msr_bit));

  if (ignore_writes) {
    // Ignore writes to the MSR.
    msr_bitmaps += 2 << 10;
    msr_bitmaps[msr_byte] &= static_cast<uint8_t>(~(1u << msr_bit));
  }
}

// static
zx_status_t Guest::Create(ktl::unique_ptr<Guest>* out) {
  // Check that the CPU supports VMX.
  if (!x86_feature_test(X86_FEATURE_VMX)) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t status = alloc_vmx_state();
  if (status != ZX_OK) {
    return status;
  }
  auto defer = fit::defer([] { free_vmx_state(); });

  fbl::AllocChecker ac;
  ktl::unique_ptr<Guest> guest(new (&ac) Guest);
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  defer.cancel();

  status = hypervisor::GuestPhysicalAddressSpace::Create(&guest->gpas_);
  if (status != ZX_OK) {
    return status;
  }

  // Setup common MSR bitmaps.
  VmxInfo vmx_info;
  status = guest->msr_bitmaps_page_.Alloc(vmx_info, UINT8_MAX);
  if (status != ZX_OK) {
    return status;
  }

  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_PAT);
  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_EFER);
  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_FS_BASE);
  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_GS_BASE);
  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_KERNEL_GS_BASE);
  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_STAR);
  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_LSTAR);
  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_FMASK);
  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_TSC_ADJUST);
  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_TSC_AUX);
  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_SYSENTER_CS);
  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_SYSENTER_ESP);
  ignore_msr(&guest->msr_bitmaps_page_, true, X86_MSR_IA32_SYSENTER_EIP);

  *out = ktl::move(guest);
  return ZX_OK;
}

Guest::~Guest() { free_vmx_state(); }

zx_status_t Guest::SetTrap(uint32_t kind, zx_vaddr_t addr, size_t len,
                           fbl::RefPtr<PortDispatcher> port, uint64_t key) {
  switch (kind) {
    case ZX_GUEST_TRAP_MEM:
      if (port) {
        return ZX_ERR_INVALID_ARGS;
      }
      break;
    case ZX_GUEST_TRAP_BELL:
      if (!port) {
        return ZX_ERR_INVALID_ARGS;
      }
      break;
    case ZX_GUEST_TRAP_IO:
      if (port) {
        return ZX_ERR_INVALID_ARGS;
      }
      return traps_.InsertTrap(kind, addr, len, ktl::move(port), key);
    default:
      return ZX_ERR_INVALID_ARGS;
  }

  // Common logic for memory-based traps.
  if (!IS_PAGE_ALIGNED(addr) || !IS_PAGE_ALIGNED(len)) {
    return ZX_ERR_INVALID_ARGS;
  }
  zx_status_t status = gpas_->UnmapRange(addr, len);
  if (status != ZX_OK) {
    return status;
  }
  return traps_.InsertTrap(kind, addr, len, ktl::move(port), key);
}

zx_status_t Guest::AllocVpid(uint16_t* vpid) {
  Guard<Mutex> lock{&vcpu_mutex_};
  return vpid_allocator_.AllocId(vpid);
}

zx_status_t Guest::FreeVpid(uint16_t vpid) {
  Guard<Mutex> lock{&vcpu_mutex_};
  return vpid_allocator_.FreeId(vpid);
}
