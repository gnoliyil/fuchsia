// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_X86_PHYS_LEGACY_BOOT_H_
#define ZIRCON_KERNEL_ARCH_X86_PHYS_LEGACY_BOOT_H_

#include <lib/stdcompat/span.h>
#include <lib/uart/all.h>
#include <lib/zbi-format/zbi.h>

#include <cstdint>
#include <string_view>

// This holds information collected from a legacy boot loader protocol.
struct LegacyBoot {
  std::string_view bootloader;
  std::string_view cmdline;
  cpp20::span<std::byte> ramdisk;
  cpp20::span<zbi_mem_range_t> mem_config;
  // Physical address of the ACPI RSDP.
  uint64_t acpi_rsdp = 0;
  uart::all::Driver uart;
};

// InitMemory() initializes this.
//
// The space pointed to by the members is safe from reclamation by the memory
// allocator after InitMemory().
extern LegacyBoot gLegacyBoot;

// InitMemory() calls this to adjust gLegacyBoot before using its data.
// It need not be defined.
[[gnu::weak]] void LegacyBootQuirks();

// Wires up the associated UART to stdout, and possibly finishes initializing
// it (which in the non-legacy case is assumed to be properly done by the
// bootloader).
void LegacyBootSetUartConsole(const uart::all::Driver& uart);

// This is a subroutine of InitMemory().  It primes the allocator and reserves
// ranges based on the data in gLegacyBoot.
void LegacyBootInitMemory();

#endif  // ZIRCON_KERNEL_ARCH_X86_PHYS_LEGACY_BOOT_H_
