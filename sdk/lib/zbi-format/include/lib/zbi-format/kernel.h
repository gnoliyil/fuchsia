// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_ZBI_FORMAT_KERNEL_H_
#define LIB_ZBI_FORMAT_KERNEL_H_

#include <stdint.h>

#include "zbi.h"

#define ZBI_IS_KERNEL_BOOTITEM(x) (((x)&ZBI_TYPE_KERNEL_MASK) == ZBI_TYPE_KERNEL_PREFIX)

// The kernel image.  In a bootable ZBI this item must always be first,
// immediately after the ZBI_TYPE_CONTAINER header.  The contiguous memory
// image of the kernel is formed from the ZBI_TYPE_CONTAINER header, the
// ZBI_TYPE_KERNEL_{ARCH} header, and the payload.
//
// The boot loader loads the whole image starting with the container header
// through to the end of the kernel item's payload into contiguous physical
// memory.  It then constructs a partial ZBI elsewhere in memory, which has
// a ZBI_TYPE_CONTAINER header of its own followed by all the other items
// that were in the booted ZBI plus other items synthesized by the boot
// loader to describe the machine.  This partial ZBI must be placed at an
// address (where the container header is found) that is aligned to the
// machine's page size.  The precise protocol for transferring control to
// the kernel's entry point varies by machine.
//
// On all machines, the kernel requires some amount of scratch memory to be
// available immediately after the kernel image at boot.  It needs this
// space for early setup work before it has a chance to read any memory-map
// information from the boot loader.  The `reserve_memory_size` field tells
// the boot loader how much space after the kernel's load image it must
// leave available for the kernel's use.  The boot loader must place its
// constructed ZBI or other reserved areas at least this many bytes after
// the kernel image.
//
// x86-64
//
//     The kernel assumes it was loaded at a fixed physical address of
//     0x100000 (1MB).  zbi_kernel_t.entry is the absolute physical address
//     of the PC location where the kernel will start.
//     TODO(fxbug.dev/24762): Perhaps this will change??
//     The processor is in 64-bit mode with direct virtual to physical
//     mapping covering the physical memory where the kernel and
//     bootloader-constructed ZBI were loaded.
//     The %rsi register holds the physical address of the
//     bootloader-constructed ZBI.
//     All other registers are unspecified.
//
//  ARM64
//
//     zbi_kernel_t.entry is an offset from the beginning of the image
//     (i.e., the ZBI_TYPE_CONTAINER header before the ZBI_TYPE_KERNEL_ARM64
//     header) to the PC location in the image where the kernel will
//     start.  The processor is in physical address mode at EL1 or
//     above.  The kernel image and the bootloader-constructed ZBI each
//     can be loaded anywhere in physical memory.  The x0 register
//     holds the physical address of the bootloader-constructed ZBI.
//     All other registers are unspecified.
//
//  RISCV64
//
//     zbi_kernel_t.entry is an offset from the beginning of the image (i.e.,
//     the ZBI_TYPE_CONTAINER header before the ZBI_TYPE_KERNEL_RISCV64 header)
//     to the PC location in the image where the kernel will start.  The
//     processor is in S mode, satp is zero, sstatus.SIE is zero.  The kernel
//     image and the bootloader-constructed ZBI each can be loaded anywhere in
//     physical memory, aligned to 4KiB.  The a0 register holds the HART ID,
//     and the a1 register holds the 4KiB-aligned physical address of the
//     bootloader-constructed ZBI.  All other registers are unspecified.
//
typedef struct {
  // Entry-point address.  The interpretation of this differs by machine.
  uint64_t entry;
  // Minimum amount (in bytes) of scratch memory that the kernel requires
  // immediately after its load image.
  uint64_t reserve_memory_size;
} zbi_kernel_t;

// The whole contiguous image loaded into memory by the boot loader.
typedef struct {
  zbi_header_t hdr_file;
  zbi_header_t hdr_kernel;
  zbi_kernel_t data_kernel;
  uint8_t contents[/*hdr_kernel.length - sizeof(zbi_kernel_t)*/];
  // data_kernel.reserve_memory_size bytes in memory are free after contents.
} zircon_kernel_t;

#endif  // LIB_ZBI_FORMAT_KERNEL_H_
