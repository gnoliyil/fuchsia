// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "boot-shim.h"

#include <lib/ddk/platform-defs.h>
#include <lib/zbi-format/driver-config.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "debug.h"
#include "devicetree.h"
#include "util.h"
#include "zbi.h"

// used in boot-shim-config.h and in this file below
static void append_boot_item(zbi_header_t* container, uint32_t type, uint32_t extra,
                             const void* payload, uint32_t length) {
  zbi_result_t result =
      zbi_create_entry_with_payload(container, SIZE_MAX, type, extra, 0, payload, length);
  if (result != ZBI_RESULT_OK) {
    fail("zbi_append_section failed\n");
  }
}

// defined in boot-shim-config.h
static void append_board_boot_item(zbi_header_t* container);

#if USE_DEVICE_TREE_CPU_COUNT
static void set_cpu_count(uint32_t cpu_count);
#endif

// Include board specific definitions
#include "boot-shim-config.h"

// behavior switches that may be overridden by boot-shim-config.h

// uncomment to dump device tree at boot
#ifndef PRINT_DEVICE_TREE
#define PRINT_DEVICE_TREE 0
#endif

// Uncomment to list ZBI items.
#ifndef PRINT_ZBI
#define PRINT_ZBI 0
#endif

// When copying the kernel out of the ZBI as it was placed by previous loaders,
// remove the kernel ZBI section to reclaim some memory.
#ifndef REMOVE_KERNEL_FROM_ZBI
#define REMOVE_KERNEL_FROM_ZBI 1
#endif

#if HAS_DEVICE_TREE
typedef enum {
  NODE_NONE,
  NODE_CHOSEN,
  NODE_MEMORY,
  NODE_CPU,
  NODE_INTC,
} node_t;

typedef struct {
  dt_slice_t devicetree;
  node_t node;
  uintptr_t initrd_start;
  size_t memory_base;
  size_t memory_size;
  char* cmdline;
  size_t cmdline_length;
  uint32_t cpu_count;
  int gic_version;
} device_tree_context_t;

static int node_callback(int depth, const char* name, void* cookie) {
#if PRINT_DEVICE_TREE
  uart_puts("node: ");
  uart_puts(name);
  uart_puts("\n");
#endif

  device_tree_context_t* ctx = cookie;

  if (!strcmp(name, "chosen")) {
    ctx->node = NODE_CHOSEN;
  } else if (!strcmp(name, "memory") || !strncmp(name, "memory@", 7)) {
    ctx->node = NODE_MEMORY;
  } else if (!strncmp(name, "cpu@", 4)) {
    ctx->node = NODE_CPU;
    ctx->cpu_count++;
  } else if (!strcmp(name, "intc") || !strncmp(name, "intc@", 5)) {
    ctx->node = NODE_INTC;
  } else {
    ctx->node = NODE_NONE;
  }

  return 0;
}

static int prop_callback(const char* name, uint8_t* data, uint32_t size, void* cookie) {
#if PRINT_DEVICE_TREE
  uart_puts("    prop: ");
  uart_puts(name);
  uart_puts(" size: ");
  uart_print_hex(size);
#endif

  device_tree_context_t* ctx = cookie;

  switch (ctx->node) {
    case NODE_CHOSEN:
      if (!strcmp(name, "linux,initrd-start")) {
        if (size == sizeof(uint32_t)) {
          ctx->initrd_start = dt_rd32(data);
        } else if (size == sizeof(uint64_t)) {
          uint64_t most = dt_rd32(data);
          uint64_t least = dt_rd32(data + 4);
          ctx->initrd_start = (most << 32) | least;
        } else {
          fail("bad size for linux,initrd-start in device tree\n");
        }
      } else if (!strcmp(name, "bootargs")) {
        ctx->cmdline = (char*)data;
        ctx->cmdline_length = size;
      }
      break;
    case NODE_MEMORY:
      if (!strcmp(name, "reg") && size == 16) {
        // memory size is big endian uint64_t at offset 0
        uint64_t most = dt_rd32(data + 0);
        uint64_t least = dt_rd32(data + 4);
        ctx->memory_base = (most << 32) | least;
        // memory size is big endian uint64_t at offset 8
        most = dt_rd32(data + 8);
        least = dt_rd32(data + 12);
        ctx->memory_size = (most << 32) | least;
      }
      break;
    case NODE_INTC:
      if (!strcmp(name, "compatible")) {
        if (!strncmp((const char*)data, "arm,gic-v3", size)) {
          ctx->gic_version = 3;
        } else if (!strncmp((const char*)data, "arm,cortex-a15-gic", size)) {
          ctx->gic_version = 2;
        }
#if PRINT_DEVICE_TREE
        uart_puts(" gic version ");
        uart_print_hex(ctx->gic_version);
#endif
      }
      break;
    default:;
  }

#if PRINT_DEVICE_TREE
  uart_puts("\n");
#endif

  return 0;
}

// Parse the device tree to find our ZBI, kernel command line, and RAM size.
static void* read_device_tree(void* device_tree, device_tree_context_t* ctx) {
  ctx->node = NODE_NONE;
  ctx->initrd_start = 0;
  ctx->memory_base = 0;
  ctx->memory_size = 0;
  ctx->cmdline = NULL;
  ctx->cpu_count = 0;
  ctx->gic_version = -1;

  devicetree_t dt;
  dt.error = uart_puts;
  int ret = dt_init(&dt, device_tree, 0xffffffff);
  if (ret) {
    fail("dt_init failed\n");
  }
  ctx->devicetree.data = device_tree;
  ctx->devicetree.size = dt.hdr.size;
  dt_walk(&dt, node_callback, prop_callback, ctx);

#if USE_DEVICE_TREE_CPU_COUNT
  set_cpu_count(ctx->cpu_count);
#endif
#if USE_DEVICE_TREE_GIC_VERSION
  set_gic_version(ctx->gic_version);
#endif
#if USE_DEVICE_TREE_TOP_OF_RAM
  set_top_of_ram(ctx->memory_base + ctx->memory_size);
#endif

  // Use the device tree initrd as the ZBI.
  return (void*)ctx->initrd_start;
}

static void append_from_device_tree(zbi_header_t* zbi, device_tree_context_t* ctx) {
  // append kernel command line
  if (ctx->cmdline && ctx->cmdline_length) {
    const uint32_t length = (uint32_t)ctx->cmdline_length;
    append_boot_item(zbi, ZBI_TYPE_CMDLINE, 0, ctx->cmdline, length);
  }
  append_boot_item(zbi, ZBI_TYPE_DEVICETREE, 0, ctx->devicetree.data, ctx->devicetree.size);
}

static bool device_tree_memory_range(device_tree_context_t* ctx, zbi_mem_range_t* range) {
  if (!ctx->memory_size) {
    uart_puts("RAM size not found in device tree\n");
    return false;
  }

  uart_puts("Setting RAM base and size device tree value: ");
  uart_print_hex(ctx->memory_base);
  uart_puts(" ");
  uart_print_hex(ctx->memory_size);
  uart_puts("\n");

  range->paddr = ctx->memory_base;
  range->length = ctx->memory_size;
  range->type = ZBI_MEM_RANGE_RAM;
  return true;
}

#else

typedef struct {
} device_tree_context_t;
static void* read_device_tree(void* device_tree, device_tree_context_t* ctx) { return NULL; }
static void append_from_device_tree(zbi_header_t* zbi, device_tree_context_t* ctx) {}
static bool device_tree_memory_range(device_tree_context_t* ctx, zbi_mem_range_t* range) {
  return false;
}

#endif  // HAS_DEVICE_TREE

// Append board memory information to the ZBI.
//
// The memory information mostly consists of a number of static entries with
// (optionally) an additional single entry from the device tree at the end.
static void append_board_memory_info(zbi_header_t* zbi, device_tree_context_t* ctx) {
  // Fetch the device tree memory entry if present.
  zbi_mem_range_t device_tree_range;
  bool has_extra_range = device_tree_memory_range(ctx, &device_tree_range);

  // Allocate space in the ZBI for the memory ranges.
  char* output_buffer;
  zbi_result_t result =
      zbi_create_entry(zbi, SIZE_MAX, ZBI_TYPE_MEM_CONFIG, /*extra=*/0, /*flags=*/0,
                       sizeof(mem_config) + (has_extra_range ? sizeof(device_tree_range) : 0),
                       (void**)&output_buffer);
  if (result != ZBI_RESULT_OK) {
    fail("zbi_create_entry failed\n");
  }

  // Copy over the static entries.
  memcpy(output_buffer, mem_config, sizeof(mem_config));
  output_buffer += sizeof(mem_config);

  // Copy over the additional entry if required.
  if (has_extra_range) {
    memcpy(output_buffer, &device_tree_range, sizeof(device_tree_range));
  }
}

__attribute__((unused)) static void dump_words(const char* what, const void* data) {
  uart_puts(what);
  const uint64_t* words = data;
  for (int i = 0; i < 8; ++i) {
    uart_puts(i == 4 ? "\n       " : " ");
    uart_print_hex(words[i]);
  }
  uart_puts("\n");
}

static zbi_result_t list_zbi_cb(zbi_header_t* item, void* payload, void* ctx) {
  uart_print_hex((uintptr_t)item);
  uart_puts(": length=0x");
  uart_print_hex(item->length);
  uart_puts(" type=0x");
  uart_print_hex(item->type);
  uart_puts(" (");
  uart_putc(item->type & 0xff);
  uart_putc((item->type >> 8) & 0xff);
  uart_putc((item->type >> 16) & 0xff);
  // The cast below is needed to make GCC with -Wconversion happy.
  uart_putc((char)(item->type >> 24) & 0xff);
  uart_puts(") extra=0x");
  uart_print_hex(item->extra);
  uart_puts("\n");
  return ZBI_RESULT_OK;
}

static void list_zbi(zbi_header_t* zbi) {
  uart_puts("ZBI container length 0x");
  uart_print_hex(zbi->length);
  uart_puts("\n");
  zbi_for_each(zbi, &list_zbi_cb, NULL);
  uart_puts("ZBI container ends 0x");
  uart_print_hex((uintptr_t)(zbi + 1) + zbi->length);
  uart_puts("\n");
}

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define ARM64_READ_SYSREG(reg)                              \
  ({                                                        \
    uint64_t _val;                                          \
    __asm__ volatile("mrs %0," TOSTRING(reg) : "=r"(_val)); \
    _val;                                                   \
  })

#define dump_arm_reg(reg)                   \
  ({                                        \
    uart_puts(#reg " = ");                  \
    uart_print_hex(ARM64_READ_SYSREG(reg)); \
    uart_puts("\n");                        \
  })

boot_shim_return_t boot_shim(void* device_tree) {
  uart_puts("boot_shim: hi there!\n");

  zircon_kernel_t* kernel = NULL;

  // Check the ZBI from device tree.
  device_tree_context_t ctx;
  zbi_header_t* zbi = read_device_tree(device_tree, &ctx);
  if (zbi != NULL) {
    zbi_header_t* bad_hdr;
    zbi_result_t check = zbi_check(zbi, &bad_hdr);
    if (check == ZBI_RESULT_OK && zbi->length > sizeof(zbi_header_t) &&
        zbi[1].type == ZBI_TYPE_KERNEL_ARM64) {
      kernel = (zircon_kernel_t*)zbi;
    } else {
      // No valid ZBI in device tree.
      // We will look in embedded_zbi instead.
      zbi = NULL;
    }
  }

  // If there is a complete ZBI from device tree, ignore whatever might
  // have been appended to the shim image.  If not, the kernel is appended.
  if (kernel == NULL) {
    zbi_header_t* bad_hdr;
    zbi_result_t check = zbi_check(&embedded_zbi, &bad_hdr);
    if (check != ZBI_RESULT_OK) {
      fail("no ZBI from device tree and no valid ZBI embedded\n");
    }
    if (embedded_zbi.hdr_file.length > sizeof(zbi_header_t) &&
        embedded_zbi.hdr_kernel.type == ZBI_TYPE_KERNEL_ARM64) {
      kernel = &embedded_zbi;
    } else {
      fail("no ARM64 kernel in ZBI from device tree or embedded ZBI\n");
    }
  }

  // If there was no ZBI at all from device tree then use the embedded ZBI
  // along with the embedded kernel.  Otherwise always use the ZBI from
  // device tree, whether the kernel is in that ZBI or was embedded.
  if (zbi == NULL) {
    zbi = &kernel->hdr_file;
  }

  // Add board-specific ZBI items.
  append_board_boot_item(zbi);

  // Add memory information.
  append_board_memory_info(zbi, &ctx);

  // Append other items from device tree.
  append_from_device_tree(zbi, &ctx);

  uint8_t* const kernel_end = (uint8_t*)&kernel->data_kernel + kernel->hdr_kernel.length +
                              kernel->data_kernel.reserve_memory_size;

  uart_puts("Kernel at ");
  uart_print_hex((uintptr_t)kernel);
  uart_puts(" to ");
  uart_print_hex((uintptr_t)kernel_end);
  uart_puts(" reserved ");
  uart_print_hex(kernel->data_kernel.reserve_memory_size);
  uart_puts("\nZBI at ");
  uart_print_hex((uintptr_t)zbi);
  uart_puts(" to ");
  uart_print_hex((uintptr_t)(zbi + 1) + zbi->length);
  uart_puts("\n");

  if ((uint8_t*)zbi < kernel_end && zbi != &kernel->hdr_file) {
    fail("expected kernel to be loaded lower in memory than initrd\n");
  }

  if (PRINT_ZBI) {
    list_zbi(zbi);
  }

  if (zbi == &kernel->hdr_file || (uintptr_t)zbi % 4096 != 0) {
    // The ZBI needs to be page-aligned, so move it up.
    // If it's a complete ZBI, splice out the kernel and move it higher.
    zbi_header_t* old = zbi;
    zbi = (void*)(((uintptr_t)old + 4095) & -(uintptr_t)4096);
    if (old == &kernel->hdr_file) {
      // Length of the kernel item payload, without header.
      uint32_t kernel_len = kernel->hdr_kernel.length;

      // Length of the ZBI container, including header, without kernel.
      uint32_t zbi_len = kernel->hdr_file.length - kernel_len;

      uart_puts("Splitting kernel len ");
      uart_print_hex(kernel_len);
      uart_puts(" from ZBI len ");
      uart_print_hex(zbi_len);

      // First move the kernel up out of the way.
      uintptr_t zbi_end = (uintptr_t)(old + 1) + old->length;
      if (zbi_end < (uintptr_t)zbi + zbi_len) {
        zbi_end = (uintptr_t)zbi + zbi_len;
      }
#if RELOCATE_KERNEL
      // relocate the kernel to a new hard coded spot
      kernel = (void*)RELOCATE_KERNEL_ADDRESS;
#else
      kernel = (void*)((zbi_end + KERNEL_ALIGN - 1) & -(uintptr_t)KERNEL_ALIGN);
#endif

      uart_puts("\nKernel to ");
      uart_print_hex((uintptr_t)kernel);

      memcpy(kernel, old, (2 * sizeof(*zbi)) + kernel_len);
      // Fix up the kernel's solo container size.
      kernel->hdr_file.length = sizeof(*zbi) + kernel_len;

#if REMOVE_KERNEL_FROM_ZBI
      // Now move the ZBI into its aligned place and fix up the
      // container header to exclude the kernel. We can conditionally
      // disable this to avoid a fairly expensive memmove() with the
      // cpu cache disabled.
      uart_puts("\nZBI to ");
      uart_print_hex((uintptr_t)zbi);
      zbi_header_t header = *old;
      header.length -= kernel->hdr_file.length;
      void* payload = (uint8_t*)(old + 1) + kernel->hdr_file.length;

      memmove(zbi + 1, payload, header.length);
      *zbi = header;
#else
      // Just mark the original kernel item as to be ignored.
      ((zircon_kernel_t*)zbi)->hdr_kernel.type = ZBI_TYPE_DISCARD;
#endif

#if RELOCATE_KERNEL
      // move the final ZBI far away as well
      void* target = (void*)RELOCATE_ZBI_ADDRESS;
      memmove(target, zbi, zbi->length);
      zbi = target;
#endif

      uart_puts("\nKernel container length ");
      uart_print_hex(kernel->hdr_file.length);
      uart_puts(" ZBI container length ");
      uart_print_hex(zbi->length);
      uart_puts("\n");
    } else {
      uart_puts("Relocating whole ZBI for alignment\n");
      memmove(zbi, old, sizeof(*old) + old->length);
    }
  }

  if ((uintptr_t)kernel % KERNEL_ALIGN != 0) {
    // The kernel has to be relocated for alignment.
    uart_puts("Relocating kernel for alignment\n");
    zbi_header_t* old = &kernel->hdr_file;
    kernel =
        (void*)(((uintptr_t)(zbi + 1) + zbi->length + KERNEL_ALIGN - 1) & -(uintptr_t)KERNEL_ALIGN);
    memmove(kernel, old, sizeof(*old) + old->length);
  }

  boot_shim_return_t result = {
      .zbi = zbi,
      .entry = (uintptr_t)kernel + kernel->data_kernel.entry,
  };
  uart_puts("Entering kernel at ");
  uart_print_hex(result.entry);
  uart_puts(" with ZBI at ");
  uart_print_hex((uintptr_t)result.zbi);
  uart_puts("\n");

  return result;
}
