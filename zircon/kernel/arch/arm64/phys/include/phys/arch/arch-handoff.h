// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_ARM64_PHYS_INCLUDE_PHYS_ARCH_ARCH_HANDOFF_H_
#define ZIRCON_KERNEL_ARCH_ARM64_PHYS_INCLUDE_PHYS_ARCH_ARCH_HANDOFF_H_

#include <zircon/boot/driver-config.h>

#include <ktl/optional.h>
#include <ktl/variant.h>

struct ZbiAmlogicRng {
  enum class Version {
    kV1,  // ZBI_KERNEL_DRIVER_AMLOGIC_RNG_V1
    kV2,  // ZBI_KERNEL_DRIVER_AMLOGIC_RNG_V2
  };

  zbi_dcfg_amlogic_rng_driver_t config;
  Version version;
};

// This holds (or points to) all arm64-specific data that is handed off from
// physboot to the kernel proper at boot time.
struct ArchPhysHandoff {
  // (ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_AMLOGIC_HDCP) payload.
  ktl::optional<zbi_dcfg_amlogic_hdcp_driver_t> amlogic_hdcp_driver;

  // (ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_AMLOGIC_RNG_V1) or
  // (ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_AMLOGIC_RNG_V2) payload
  ktl::optional<ZbiAmlogicRng> amlogic_rng_driver;

  // (ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_ARM_GENERIC_TIMER) payload.
  ktl::optional<zbi_dcfg_arm_generic_timer_driver_t> generic_timer_driver;

  // (ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_ARM_GIC_V2/ZBI_KERNEL_DRIVER_ARM_GIC_V3) payload.
  ktl::variant<ktl::monostate, zbi_dcfg_arm_gic_v2_driver_t, zbi_dcfg_arm_gic_v3_driver_t>
      gic_driver;

  // (ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_ARM_PSCI) payload.
  ktl::optional<zbi_dcfg_arm_psci_driver_t> psci_driver;

  // (ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_GENERIC32_WATCHDOG) payload.
  ktl::optional<zbi_dcfg_generic32_watchdog_t> generic32_watchdog_driver;

  // (ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_AS370_POWER) payload.
  bool as370_power_driver = false;

  // (ZBI_TYPE_KERNEL_DRIVER, ZBI_KERNEL_DRIVER_MOTMOT_POWER) payload.
  bool motmot_power_driver = false;
};

// This must match what the kernel's page-table bootstrapping actually uses as
// the virtual address of the kernel load image.
// TODO(fxbug.dev/84107): Matches //zircon/kernel/arch/arm64/mmu.cc, which
// will no longer need to define it when the ELF kernel is the only kernel.
#if DISABLE_KASLR
inline constexpr uint64_t kArchHandoffVirtualAddress = KERNEL_BASE;
#else
inline constexpr uint64_t kArchHandoffVirtualAddress = 0xffffffff10000000;
#endif

#endif  // ZIRCON_KERNEL_ARCH_ARM64_PHYS_INCLUDE_PHYS_ARCH_ARCH_HANDOFF_H_
