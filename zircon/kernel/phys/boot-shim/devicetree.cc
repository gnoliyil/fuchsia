// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "phys/boot-shim/devicetree.h"

#include <lib/boot-shim/devicetree.h>
#include <lib/uart/all.h>

#include <ktl/array.h>
#include <ktl/type_traits.h>
#include <phys/address-space.h>
#include <phys/allocation.h>
#include <phys/boot-options.h>
#include <phys/main.h>
#include <phys/uart.h>

void DevicetreeInitUart(const boot_shim::DevicetreeBootstrapChosenNodeItem<>& chosen_item,
                        BootOptions& boot_opts) {
  // Overwrite devicetree results with bootloader provided UART driver.
  boot_opts.serial = chosen_item.uart().uart();
  SetBootOptions(boot_opts, chosen_item.zbi(), chosen_item.cmdline().value_or(""));
  SetUartConsole(boot_opts.serial);
}

void DevicetreeInitMemory(const boot_shim::DevicetreeBootstrapChosenNodeItem<>& chosen_item,
                          const boot_shim::DevicetreeMemoryItem& memory_item) {
  auto zbi = chosen_item.zbi();
  uint64_t phys_start = reinterpret_cast<uint64_t>(PHYS_LOAD_ADDRESS);
  uint64_t phys_end = reinterpret_cast<uint64_t>(_end);

  ktl::array<memalloc::Range, 2> special_ranges = {
      memalloc::Range{
          .addr = phys_start,
          .size = phys_end - phys_start,
          .type = memalloc::Type::kPhysKernel,
      },
  };
  cpp20::span<memalloc::Range> special_ranges_view(special_ranges.data(), 1);

  if (!zbi.empty()) {
    special_ranges[1] = memalloc::Range{
        .addr = reinterpret_cast<uint64_t>(zbi.data()),
        .size = zbi.size(),
        .type = memalloc::Type::kDataZbi,
    };
    special_ranges_view = special_ranges;
  }

  auto ranges = memory_item.memory_ranges();
  auto memory_ranges =
      cpp20::span<memalloc::Range>(const_cast<memalloc::Range*>(ranges.data()), ranges.size());
  Allocation::Init(memory_ranges, special_ranges_view);

  ArchSetUpAddressSpaceEarly();

  Allocation::GetPool().PrintMemoryRanges(ProgramName());
}
