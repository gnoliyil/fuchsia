// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "psci.h"

#include <lib/arch/arm64/psci.h>
#include <lib/arch/arm64/system.h>
#include <lib/boot-options/boot-options.h>
#include <lib/zbi-format/driver-config.h>
#include <stdint.h>
#include <zircon/assert.h>

#include <phys/main.h>
#include <phys/stdio.h>

#include "arch-phys-info.h"

// These are defined in assembly along with ArmPsciReset (see psci.S).  The
// first argument is the operation and the other arguments vary by operation.
extern "C" uint64_t ArmPsciCall(arch::ArmPsciFunction function, uint64_t arg1 = 0,
                                uint64_t arg2 = 0, uint64_t arg3 = 0);

constexpr uint64_t kReset2 = static_cast<uint64_t>(arch::ArmPsciFunction::kSystemReset2);

void ArmPsciSetup(const zbi_dcfg_arm_psci_driver_t* cfg) {
  gArchPhysInfo->psci_reset_registers = {
      static_cast<uint64_t>(arch::ArmPsciFunction::kSystemReset),
  };

  if (!cfg) {
    gArchPhysInfo->psci_disabled = true;
    debugf("%s: No ZBI_KERNEL_DRIVER_ARM_PSCI item found in ZBI.  Early PSCI disabled.\n",
           ProgramName());
    return;
  }

  gArchPhysInfo->psci_use_hvc = cfg->use_hvc && arch::ArmCurrentEl::Read().el() < 2;

  const uint64_t* reset_args = nullptr;
  switch (gBootOptions->phys_psci_reset) {
    case Arm64PhysPsciReset::kDisabled:
      gArchPhysInfo->psci_disabled = true;
      debugf("%s: Early PSCI disabled by boot option.\n", ProgramName());
      return;
    case Arm64PhysPsciReset::kShutdown:
      reset_args = cfg->shutdown_args;
      break;
    case Arm64PhysPsciReset::kReboot:
      reset_args = cfg->reboot_args;
      break;
    case Arm64PhysPsciReset::kRebootBootloader:
      reset_args = cfg->reboot_bootloader_args;
      break;
    case Arm64PhysPsciReset::kRebootRecovery:
      reset_args = cfg->reboot_recovery_args;
      break;
    default:
      ZX_PANIC("impossible phys_psci_reset value %#x",
               static_cast<uint32_t>(gBootOptions->phys_psci_reset));
  }

  gArchPhysInfo->psci_reset_registers[1] = reset_args[0];
  gArchPhysInfo->psci_reset_registers[2] = reset_args[1];
  gArchPhysInfo->psci_reset_registers[3] = reset_args[2];

  const uint64_t version = ArmPsciCall(arch::ArmPsciFunction::kPsciVersion);
  const uint16_t major_version = static_cast<uint16_t>(version >> 16);
  const uint16_t minor_version = static_cast<uint16_t>(version);
  const bool have_reset2 = major_version >= 1 && minor_version != 0xffff &&
                           ArmPsciCall(arch::ArmPsciFunction::kPsciFeatures, kReset2) == 0;

  if (have_reset2) {
    gArchPhysInfo->psci_reset_registers.front() = kReset2;
  }

  const char* insn = gArchPhysInfo->psci_use_hvc ? "HVC" : "SMC";
  const char* cmd = have_reset2 ? "RESET2" : "RESET";
  debugf("%s: Early PSCI via %s insn and %s with arguments: {%#zx, %#zx, %#zx}\n", ProgramName(),
         insn, cmd, gArchPhysInfo->psci_reset_registers[1], gArchPhysInfo->psci_reset_registers[2],
         gArchPhysInfo->psci_reset_registers[3]);
}
