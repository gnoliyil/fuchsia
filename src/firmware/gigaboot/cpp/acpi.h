// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_FIRMWARE_GIGABOOT_CPP_ACPI_H_
#define SRC_FIRMWARE_GIGABOOT_CPP_ACPI_H_

#include <efi/types.h>

namespace gigaboot {

struct __attribute__((packed)) acpi_rsdp_t {
  uint64_t signature;
  uint8_t checksum;
  uint8_t oem_id[6];
  uint8_t revision;
  uint32_t rsdt_address;

  // Available in ACPI version 2.0.
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t extended_checksum;
  uint8_t reserved[3];
};
_Static_assert(sizeof(acpi_rsdp_t) == 36, "RSDP is the wrong size");

constexpr size_t kAcpiRsdpV1Size = offsetof(acpi_rsdp_t, length);

extern const efi_guid kAcpiTableGuid;
extern const efi_guid kAcpi20TableGuid;
extern const uint8_t kAcpiRsdpSignature[8];

}  // namespace gigaboot

#endif  // SRC_FIRMWARE_GIGABOOT_CPP_ACPI_H_
