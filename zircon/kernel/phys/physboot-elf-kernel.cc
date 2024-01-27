// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <inttypes.h>
#include <lib/boot-options/boot-options.h>
#include <lib/code-patching/code-patches.h>
#include <lib/fit/function.h>
#include <lib/zbitl/error-stdio.h>
#include <stdlib.h>
#include <zircon/assert.h>

#include <arch/code-patches/case-id.h>
#include <arch/kernel_aspace.h>
#include <ktl/initializer_list.h>
#include <ktl/move.h>
#include <ktl/string_view.h>
#include <phys/allocation.h>
#include <phys/boot-zbi.h>
#include <phys/elf-image.h>
#include <phys/handoff.h>
#include <phys/kernel-package.h>
#include <phys/stdio.h>
#include <phys/symbolize.h>

#include "handoff-prep.h"
#include "physboot.h"
#include "physload.h"

#include <ktl/enforce.h>

namespace {

constexpr ktl::string_view kElfPhysKernel = "physzircon";

fit::result<ElfImage::Error> ApplyKernelPatch(code_patching::Patcher& patcher, CodePatchId id,
                                              ktl::span<ktl::byte> code_to_patch,
                                              ElfImage::PrintPatchFunction print) {
  if (ArchPatchCode(patcher, code_to_patch, id, ktl::move(print))) {
    return fit::ok();
  }
  print({"unrecognized patch case ID"});
  ZX_PANIC("%s: code-patching: unrecognized patch case ID %" PRIu32, gSymbolize->name(),
           static_cast<uint32_t>(id));
}

void PatchElfKernel(ElfImage& elf_kernel) {
  // Apply patches to the kernel image.
  auto result = elf_kernel.ForEachPatch<CodePatchId>(ApplyKernelPatch);
  if (result.is_error()) {
    zbitl::PrintBootfsError(result.error_value());
    abort();
  }

  // There's always the self-test patch, so there should never be none.
  ZX_ASSERT(elf_kernel.has_patches());
}

}  // namespace

PhysBootTimes gBootTimes;

[[noreturn]] void PhysLoadModuleMain(UartDriver& uart, PhysBootTimes boot_times,
                                     KernelStorage kernel_storage) {
  gBootTimes = boot_times;

  gSymbolize->set_name("physboot");

  // Now we're ready for the main physboot logic.
  BootZircon(uart, ktl::move(kernel_storage));
}

[[noreturn]] void BootZircon(UartDriver& uart, KernelStorage kernel_storage) {
  KernelStorage::Bootfs package = kernel_storage.GetKernelPackage();

  ElfImage elf_kernel;
  debugf("%s: Locating ELF kernel in kernel package...\n", gSymbolize->name());
  if (auto result = elf_kernel.Init(package, kElfPhysKernel, true); result.is_error()) {
    printf("%s: Cannot load ELF kernel \"%.*s/%.*s\" from STORAGE_KERNEL item BOOTFS: ",
           gSymbolize->name(), static_cast<int>(package.directory().size()),
           package.directory().data(), static_cast<int>(kElfPhysKernel.size()),
           kElfPhysKernel.data());
    zbitl::PrintBootfsError(result.error_value());
    abort();
  }

  // Make sure the kernel was built to match this physboot binary.
  // TODO(mcgrathr): Use gSymbolize->build_id() after fxrev.dev/795906 lands.
  elf_kernel.AssertInterpMatchesBuildId(gSymbolize->name(),
                                        *gSymbolize->modules().back()->build_id());

  // Though we're loading an ELF kernel now, currently we are still loading an
  // old-style kernel entered in physical memory that may still use the bad old
  // boot_alloc ways, so make sure there's some excess memory free off the end
  // of the proper kernel load image.
  // TODO(mcgrathr): When boot_alloc is gone, just Load() here will do.
  Allocation loaded_elf_kernel =
      elf_kernel.Load(ktl::nullopt, true, BootZbi::kKernelBootAllocReserve);

  PatchElfKernel(elf_kernel);

  // Use the putative eventual virtual address to relocate the kernel.
  elf_kernel.set_load_address(KERNEL_ASPACE_BASE);
  elf_kernel.Relocate();

  // Prepare the handoff data structures.
  HandoffPrep prep;

  // Repurpose the storage item as a place to put the handoff payload.  The
  // KERNEL_STORAGE payload was already decompressed elsewhere, so it's no
  // longer in use.
  prep.Init(kernel_storage.item()->payload);

  // For now we're loading an ELF kernel in physical address mode at an
  // arbitrary load address, even though it's been relocated for its final
  // virtual address.  The kernel's entry point is expected to be purely
  // position independent long enough to switch to virtual addressing.
  //
  // NOTE: For real handoff with virtual addresses, this will need some inline
  // asm to switch stacks and such. For interim hack kernels doing physical
  // address mode handoff, they can either use the phys stack momentarily
  // or have asm entry code that sets up its own stack.
  elf_kernel.set_load_address(elf_kernel.physical_load_address());
  auto start_elf_kernel = [&elf_kernel](PhysHandoff* handoff) {
    elf_kernel.Handoff<void(PhysHandoff*)>(handoff);
  };
  prep.DoHandoff(uart, kernel_storage.zbi().storage(), start_elf_kernel);
}
