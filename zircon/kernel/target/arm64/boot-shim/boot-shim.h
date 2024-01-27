// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_TARGET_ARM64_BOOT_SHIM_BOOT_SHIM_H_
#define ZIRCON_KERNEL_TARGET_ARM64_BOOT_SHIM_BOOT_SHIM_H_

#include <lib/zbi-format/zbi.h>

// This symbol is defined in boot-shim.ld.
extern zircon_kernel_t embedded_zbi;

// This type is tailored for the ARM64 C ABI returning to assembly code.
typedef struct {
  zbi_header_t* zbi;  // Returned in x0.
  uint64_t entry;     // Returned in x1.
} boot_shim_return_t;

boot_shim_return_t boot_shim(void* device_tree);

#endif  // ZIRCON_KERNEL_TARGET_ARM64_BOOT_SHIM_BOOT_SHIM_H_
