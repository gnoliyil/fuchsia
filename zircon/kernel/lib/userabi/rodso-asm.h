// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_USERABI_RODSO_ASM_H_
#define ZIRCON_KERNEL_LIB_USERABI_RODSO_ASM_H_

#include <asm.h>

#include <arch/defines.h>

// clang-format off

// Define a special read-only, page-aligned data section called NAME
// anchored with a symbol NAME_image to contain the first SIZE bytes
// (whole pages) of FILENAME.
.macro rodso_image_section name, size
  // We can't use PAGE_SIZE here because on some machines
  // that uses C syntax like 1L instead of plain integers
  // and arithmetic operations that the assembler can handle.
  .if \size % (1 << PAGE_SIZE_SHIFT)
    .error "\name size \size is not a multiple of PAGE_SIZE"
  .endif
  .section .rodata.rodso_image.\name,"a"
  .p2align PAGE_SIZE_SHIFT
.endm

// The whole thing can't be just an assembler macro because a macro
// operand cannot be a string like .incbin needs for the filename.
#define RODSO_IMAGE(name, NAME, CODE_or_DATA) \
  rodso_image_section name, NAME##_##CODE_or_DATA##_END; \
  DATA(name##_image) \
  .incbin NAME##_FILENAME, 0, NAME##_##CODE_or_DATA##_END; \
  .p2align PAGE_SIZE_SHIFT; \
  END_DATA(name##_image)

#endif  // ZIRCON_KERNEL_LIB_USERABI_RODSO_ASM_H_
