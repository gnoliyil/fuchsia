// Copyright 2017 The Fuchsia Authors
// Copyright (c) 2016, Google, Inc. All rights reserved
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_DEV_PSCI_INCLUDE_DEV_PSCI_H_
#define ZIRCON_KERNEL_DEV_PSCI_INCLUDE_DEV_PSCI_H_

#include <arch.h>
#include <lib/zbi-format/driver-config.h>

#include <arch/arm64/mp.h>
#include <dev/power.h>

#define PSCI64_PSCI_VERSION (0x84000000)
#define PSCI64_CPU_SUSPEND (0xC4000001)
#define PSCI64_CPU_OFF (0x84000002)
#define PSCI64_CPU_ON (0xC4000003)
#define PSCI64_AFFINITY_INFO (0xC4000004)
#define PSCI64_MIGRATE (0xC4000005)
#define PSCI64_MIGRATE_INFO_TYPE (0x84000006)
#define PSCI64_MIGRATE_INFO_UP_CPU (0xC4000007)
#define PSCI64_SYSTEM_OFF (0x84000008)
#define PSCI64_SYSTEM_RESET (0x84000009)
#define PSCI64_SYSTEM_RESET2 (0xC4000012)
#define PSCI64_PSCI_FEATURES (0x8400000A)
#define PSCI64_CPU_FREEZE (0x8400000B)
#define PSCI64_CPU_DEFAULT_SUSPEND (0xC400000C)
#define PSCI64_NODE_HW_STATE (0xC400000D)
#define PSCI64_SYSTEM_SUSPEND (0xC400000E)
#define PSCI64_PSCI_SET_SUSPEND_MODE (0x8400000F)
#define PSCI64_PSCI_STAT_RESIDENCY (0xC4000010)
#define PSCI64_PSCI_STAT_COUNT (0xC4000011)

// See: "Firmware interfaces for mitigating cache speculation vulnerabilities"
//      "System Software on Arm Systems"
#define PSCI64_SMCCC_VERSION (0x80000000)
#define PSCI64_SMCCC_ARCH_FEATURES (0x80000001)
#define PSCI64_SMCCC_ARCH_WORKAROUND_1 (0x80008000)
#define PSCI64_SMCCC_ARCH_WORKAROUND_2 (0x80007FFF)

#define PSCI_SUCCESS 0
#define PSCI_NOT_SUPPORTED -1
#define PSCI_INVALID_PARAMETERS -2
#define PSCI_DENIED -3
#define PSCI_ALREADY_ON -4
#define PSCI_ON_PENDING -5
#define PSCI_INTERNAL_FAILURE -6
#define PSCI_NOT_PRESENT -7
#define PSCI_DISABLED -8
#define PSCI_INVALID_ADDRESS -9

/* TODO NOTE: - currently these routines assume cpu topologies that are described only in AFF0 and
   AFF1. If a system is architected such that AFF2 or AFF3 are non-zero then this code will need to
   be revisited
*/

// Initializes the PSCI driver.
void PsciInit(const zbi_dcfg_arm_psci_driver_t& config);

uint32_t psci_get_version();
uint32_t psci_get_feature(uint32_t psci_call);

/* powers down the calling cpu - only returns if call fails */
uint32_t psci_cpu_off();
uint32_t psci_cpu_on(uint64_t mpid, paddr_t entry);
uint32_t psci_get_affinity_info(uint64_t cluster, uint64_t cpuid);

void psci_system_off();

/* called from assembly, mark as C external */
extern "C" void psci_system_reset(enum reboot_flags flags);

#endif  // ZIRCON_KERNEL_DEV_PSCI_INCLUDE_DEV_PSCI_H_
