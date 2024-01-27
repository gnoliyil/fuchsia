// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_PHYS_LIB_BOOT_SHIM_INCLUDE_LIB_BOOT_SHIM_ACPI_H_
#define ZIRCON_KERNEL_PHYS_LIB_BOOT_SHIM_INCLUDE_LIB_BOOT_SHIM_ACPI_H_

#include <lib/zbi-format/driver-config.h>
#include <stdio.h>

#include "item-base.h"

// Forward declaration for <lib/acpi_lite.h>.
namespace acpi_lite {
class AcpiParserInterface;
class AcpiParser;
}  // namespace acpi_lite

namespace boot_shim {

// This can supply an item with the ACPI root table pointer, if available.
// Its set_payload method takes a uint64_t physical address argument.
class AcpiRsdpItem : public SingleOptionalItem<uint64_t, ZBI_TYPE_ACPI_RSDP> {
 public:
  // This sets a payload of the physical address the parser is using.
  // If Init is not called, no item will be produced.
  void Init(const acpi_lite::AcpiParser& parser, const char* shim_name, FILE* log);
};

// This can supply a ZBI_TYPE_KERNEL_DRIVER item based on the serial console
// details in ACPI's DBG2 table.
class AcpiUartItem : public boot_shim::SingleVariantItemBase<AcpiUartItem, zbi_dcfg_simple_t,
                                                             zbi_dcfg_simple_pio_t> {
 public:
  // This initializes the data from ACPI tables.
  void Init(const acpi_lite::AcpiParserInterface& parser, const char* shim_name, FILE* log);

  static constexpr zbi_header_t ItemHeader(const zbi_dcfg_simple_t& cfg) {
    return {.type = ZBI_TYPE_KERNEL_DRIVER, .extra = ZBI_KERNEL_DRIVER_I8250_MMIO_UART};
  }

  static constexpr zbi_header_t ItemHeader(const zbi_dcfg_simple_pio_t& cfg) {
    return {.type = ZBI_TYPE_KERNEL_DRIVER, .extra = ZBI_KERNEL_DRIVER_I8250_PIO_UART};
  }
};

}  // namespace boot_shim

#endif  // ZIRCON_KERNEL_PHYS_LIB_BOOT_SHIM_INCLUDE_LIB_BOOT_SHIM_ACPI_H_
