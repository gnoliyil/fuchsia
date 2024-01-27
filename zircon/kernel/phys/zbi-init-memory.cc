// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/boot-options/boot-options.h>
#include <lib/memalloc/pool.h>
#include <lib/zbi-format/memory.h>
#include <lib/zbi-format/zbi.h>
#include <lib/zbitl/view.h>
#include <zircon/assert.h>

#include <ktl/array.h>
#include <phys/address-space.h>
#include <phys/allocation.h>
#include <phys/main.h>
#include <phys/symbolize.h>

#include <ktl/enforce.h>

void ZbiInitMemory(void* zbi, ktl::span<zbi_mem_range_t> mem_config,
                   ktl::optional<memalloc::Range> extra_special_range) {
  zbitl::ByteView zbi_storage = zbitl::StorageFromRawHeader(static_cast<zbi_header_t*>(zbi));

  uint64_t phys_start = reinterpret_cast<uint64_t>(PHYS_LOAD_ADDRESS);
  uint64_t phys_end = reinterpret_cast<uint64_t>(_end);
  memalloc::Range special_memory_ranges[3] = {
      {
          .addr = phys_start,
          .size = phys_end - phys_start,
          .type = memalloc::Type::kPhysKernel,
      },
      {
          .addr = reinterpret_cast<uint64_t>(zbi_storage.data()),
          .size = zbi_storage.size_bytes(),
          .type = memalloc::Type::kDataZbi,
      },
  };

  ktl::span<memalloc::Range> zbi_ranges(memalloc::AsRanges(mem_config));
  ktl::span<memalloc::Range> special_ranges(special_memory_ranges);
  if (extra_special_range) {
    special_ranges.back() = *extra_special_range;
  } else {
    special_ranges = special_ranges.subspan(0, special_ranges.size() - 1);
  }

  Allocation::Init(zbi_ranges, special_ranges);

  // Set up our own address space.
  ArchSetUpAddressSpaceEarly();

  if (gBootOptions->phys_verbose) {
    Allocation::GetPool().PrintMemoryRanges(ProgramName());
  }
}
