// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/fitx/result.h>
#include <lib/zbitl/error-stdio.h>
#include <lib/zbitl/image.h>
#include <lib/zbitl/memory.h>
#include <mexec.h>
#include <zircon/boot/driver-config.h>
#include <zircon/boot/image.h>

#include <fbl/array.h>
#include <ktl/byte.h>
#include <phys/handoff.h>

fitx::result<fitx::failed> ArchAppendMexecDataFromHandoff(MexecDataImage& image,
                                                          PhysHandoff& handoff) {
  if (handoff.arch_handoff.amlogic_hdcp_driver) {
    const zbi_header_t header = {
        .type = ZBI_TYPE_KERNEL_DRIVER,
        .extra = KDRV_AMLOGIC_HDCP,
    };
    auto result =
        image.Append(header, zbitl::AsBytes(handoff.arch_handoff.amlogic_hdcp_driver.value()));
    if (result.is_error()) {
      printf("mexec: could not append AMLogic HDCP driver config: ");
      zbitl::PrintViewError(result.error_value());
      return fitx::failed();
    }
  }

  if (handoff.arch_handoff.amlogic_rng_driver) {
    const zbi_header_t header = {
        .type = ZBI_TYPE_KERNEL_DRIVER,
        .extra = KDRV_AMLOGIC_RNG,
    };
    auto result =
        image.Append(header, zbitl::AsBytes(handoff.arch_handoff.amlogic_rng_driver.value()));
    if (result.is_error()) {
      printf("mexec: could not append AMLogic RNG driver config: ");
      zbitl::PrintViewError(result.error_value());
      return fitx::failed();
    }
  }

  if (handoff.arch_handoff.generic_timer_driver) {
    const zbi_header_t header = {
        .type = ZBI_TYPE_KERNEL_DRIVER,
        .extra = KDRV_ARM_GENERIC_TIMER,
    };
    auto result =
        image.Append(header, zbitl::AsBytes(handoff.arch_handoff.generic_timer_driver.value()));
    if (result.is_error()) {
      printf("mexec: could not append generic ARM timer driver config: ");
      zbitl::PrintViewError(result.error_value());
      return fitx::failed();
    }
  }

  if (handoff.arch_handoff.psci_driver) {
    const zbi_header_t header = {
        .type = ZBI_TYPE_KERNEL_DRIVER,
        .extra = KDRV_ARM_PSCI,
    };
    auto result = image.Append(header, zbitl::AsBytes(handoff.arch_handoff.psci_driver.value()));
    if (result.is_error()) {
      printf("mexec: could not append PCI driver config: ");
      zbitl::PrintViewError(result.error_value());
      return fitx::failed();
    }
  }

  if (handoff.arch_handoff.generic_32bit_watchdog_driver) {
    const zbi_header_t header = {
        .type = ZBI_TYPE_KERNEL_DRIVER,
        .extra = KDRV_GENERIC_32BIT_WATCHDOG,
    };
    auto result = image.Append(
        header, zbitl::AsBytes(handoff.arch_handoff.generic_32bit_watchdog_driver.value()));
    if (result.is_error()) {
      printf("mexec: could not append generic 32-bit watchdog driver config: ");
      zbitl::PrintViewError(result.error_value());
      return fitx::failed();
    }
  }

  return fitx::ok();
}
