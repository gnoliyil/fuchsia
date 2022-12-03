// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/stdcompat/span.h>
#include <lib/zbi/zbi.h>
#include <lib/zircon_boot/zbi_utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <zircon/limits.h>

#include <numeric>

#include <efi/boot-services.h>
#include <efi/system-table.h>
#include <fbl/vector.h>

#include "acpi.h"
#include "phys/efi/main.h"
#include "utils.h"
#include "zircon/kernel/lib/efi/include/efi/types.h"

namespace gigaboot {

const efi_guid kAcpiTableGuid = ACPI_TABLE_GUID;
const efi_guid kAcpi20TableGuid = ACPI_20_TABLE_GUID;
const uint8_t kAcpiRsdpSignature[8] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};

namespace {

uint8_t scratch_buffer[32 * 1024];

bool AddMemoryRanges(void* zbi, size_t capacity) {
  uint32_t dversion = 0;
  size_t mkey = 0;
  size_t dsize = 0;
  size_t msize = sizeof(scratch_buffer);
  efi_status status = gEfiSystemTable->BootServices->GetMemoryMap(
      &msize, reinterpret_cast<efi_memory_descriptor*>(scratch_buffer), &mkey, &dsize, &dversion);
  if (status != EFI_SUCCESS) {
    printf("boot: cannot GetMemoryMap(). %s\n", EfiStatusToString(status));
    return false;
  }

  // Convert the memory map in place to a range of zbi_mem_range_t, the
  // preferred ZBI memory format. In-place conversion can safely be done
  // one-by-one, given that zbi_mem_range_t is smaller than a descriptor.
  static_assert(sizeof(zbi_mem_range_t) <= sizeof(efi_memory_descriptor),
                "Cannot assume that sizeof(zbi_mem_range_t) <= dsize");
  size_t num_ranges = msize / dsize;
  zbi_mem_range_t* ranges = reinterpret_cast<zbi_mem_range_t*>(scratch_buffer);
  for (size_t i = 0; i < num_ranges; ++i) {
    const efi_memory_descriptor* desc =
        reinterpret_cast<efi_memory_descriptor*>(scratch_buffer + i * dsize);
    const zbi_mem_range_t range = {
        .paddr = desc->PhysicalStart,
        .length = desc->NumberOfPages * kUefiPageSize,
        .type = EfiToZbiMemRangeType(desc->Type),
    };
    memcpy(&ranges[i], &range, sizeof(range));
  }

  // TODO(b/236039205): Add memory ranges for uart peripheral. Refer to
  // `src/firmware/gigaboot/src/zircon.c` at line 477.

  // TODO(b/236039205): Add memory ranges for GIC. Rfer to `src/firmware/gigaboot/src/zircon.c` at
  // line 488.

  zbi_result_t result = zbi_create_entry_with_payload(zbi, capacity, ZBI_TYPE_MEM_CONFIG, 0, 0,
                                                      ranges, num_ranges * sizeof(zbi_mem_range_t));
  if (result != ZBI_RESULT_OK) {
    printf("Failed to create entry, %d\n", result);
    return false;
  }

  return true;
}

bool operator==(const efi_guid& l, const efi_guid& r) {
  return memcmp(&l, &r, sizeof(efi_guid)) == 0;
}

bool AppendAcpiRsdp(zbi_header_t* image, size_t capacity) {
  acpi_rsdp_t const* rsdp = nullptr;
  cpp20::span<const efi_configuration_table> entries(gEfiSystemTable->ConfigurationTable,
                                                     gEfiSystemTable->NumberOfTableEntries);

  for (const efi_configuration_table& entry : entries) {
    // Check if this entry is an ACPI RSD PTR.
    if ((entry.VendorGuid == kAcpiTableGuid || entry.VendorGuid == kAcpi20TableGuid) &&
        // Verify the signature of the ACPI RSD PTR.
        !memcmp(entry.VendorTable, kAcpiRsdpSignature, sizeof(kAcpiRsdpSignature))) {
      rsdp = reinterpret_cast<const acpi_rsdp_t*>(entry.VendorTable);
      break;
    }
  }

  // Verify an ACPI table was found.
  if (rsdp == nullptr) {
    printf("RSDP was not found\n");
    return false;
  }

  // Verify the checksum of this table. Both V1 and V2 RSDPs should pass the
  // V1 checksum, which only covers the first 20 bytes of the table.
  // The checksum adds all the bytes and passes if the result is 0.
  cpp20::span<const uint8_t> acpi_bytes(reinterpret_cast<const uint8_t*>(rsdp), kAcpiRsdpV1Size);
  if (std::accumulate(acpi_bytes.begin(), acpi_bytes.end(), uint8_t{0})) {
    printf("RSDP V1 checksum failed\n");
    return false;
  }

  // V2 RSDPs should additionally pass a checksum of the entire table.
  if (rsdp->revision > 0) {
    acpi_bytes = {acpi_bytes.begin(), rsdp->length};
    if (std::accumulate(acpi_bytes.begin(), acpi_bytes.end(), uint8_t{0})) {
      printf("RSDP V2 checksum failed\n");
      return false;
    }
  }

  if (zbi_result_t result = zbi_create_entry_with_payload(image, capacity, ZBI_TYPE_ACPI_RSDP, 0, 0,
                                                          &rsdp, sizeof(rsdp));
      result != ZBI_RESULT_OK) {
    printf("Failed to create ACPI rsdp entry, %d\n", result);
    return false;
  }

  return true;
}

}  // namespace

bool AddGigabootZbiItems(zbi_header_t* image, size_t capacity, AbrSlotIndex slot) {
  if (!AddMemoryRanges(image, capacity)) {
    return false;
  }

  if (AppendCurrentSlotZbiItem(image, capacity, slot) != ZBI_RESULT_OK) {
    return false;
  }

  if (!AppendAcpiRsdp(image, capacity)) {
    return false;
  }

  return true;
}

}  // namespace gigaboot
