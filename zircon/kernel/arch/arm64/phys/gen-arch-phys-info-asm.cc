// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <stddef.h>

#include <hwreg/asm.h>

#include "arch-phys-info.h"

int main(int argc, char** argv) {
  return hwreg::AsmHeader()  //
      .Macro("ARCH_PHYS_INFO_PSCI_USE_HVC", offsetof(ArchPhysInfo, psci_use_hvc))
      .Macro("ARCH_PHYS_INFO_PSCI_DISABLED", offsetof(ArchPhysInfo, psci_disabled))
      .Macro("ARCH_PHYS_INFO_PSCI_RESET_REGISTERS", offsetof(ArchPhysInfo, psci_reset_registers))
      .Macro("ARCH_PHYS_INFO_PSCI_RESET_REGISTERS_COUNT",
             std::size(ArchPhysInfo{}.psci_reset_registers))
      .Main(argc, argv);
}
