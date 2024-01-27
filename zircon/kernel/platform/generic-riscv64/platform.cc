// Copyright 2023 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <arch.h>
#include <debug.h>
#include <lib/affine/ratio.h>
#include <lib/arch/intrin.h>
#include <lib/boot-options/boot-options.h>
#include <lib/boot-options/types.h>
#include <lib/console.h>
#include <lib/crashlog.h>
#include <lib/debuglog.h>
#include <lib/jtrace/jtrace.h>
#include <lib/memory_limit.h>
#include <lib/persistent-debuglog.h>
#include <lib/system-topology.h>
#include <mexec.h>
#include <platform.h>
#include <reg.h>
#include <string-file.h>
#include <trace.h>

#include <arch/arch_ops.h>
#include <arch/mp.h>
#include <arch/riscv64.h>
#include <arch/riscv64/sbi.h>
#include <dev/display.h>
#include <dev/hw_rng.h>
#include <dev/interrupt.h>
#include <dev/power.h>
#include <dev/uart.h>
#include <explicit-memory/bytes.h>
#include <fbl/auto_lock.h>
#include <fbl/ref_ptr.h>
#include <kernel/cpu_distance_map.h>
#include <kernel/dpc.h>
#include <kernel/jtrace_config.h>
#include <kernel/persistent_ram.h>
#include <kernel/spinlock.h>
#include <kernel/topology.h>
#include <ktl/algorithm.h>
#include <ktl/atomic.h>
#include <ktl/byte.h>
#include <ktl/span.h>
#include <lk/init.h>
#include <object/resource_dispatcher.h>
#include <phys/handoff.h>
#include <platform/crashlog.h>
#include <platform/ram_mappable_crashlog.h>
#include <platform/timer.h>
#include <vm/bootreserve.h>
#include <vm/kstack.h>
#include <vm/physmap.h>
#include <vm/vm.h>
#include <vm/vm_aspace.h>
#if WITH_PANIC_BACKTRACE
#include <kernel/thread.h>
#endif

// #include <lib/zbitl/error_stdio.h>
#include <lib/zbitl/image.h>
#include <lib/zbitl/memory.h>
#include <zircon/boot/image.h>
#include <zircon/errors.h>
#include <zircon/rights.h>
#include <zircon/syscalls/smc.h>
#include <zircon/types.h>

#define LOCAL_TRACE 0

// TODO-rvbringup: remove this when building lib/syscalls/ddk.cc
#ifdef KERNEL_NO_USERABI
zx_paddr_t gAcpiRsdp = 0;
zx_paddr_t gSmbiosPhys = 0;
#endif

// Defined in start.S.
extern paddr_t kernel_secondary_entry_paddr;

static void* ramdisk_base;
static size_t ramdisk_size;

static bool uart_disabled = false;

// all of the configured memory arenas from the zbi
static constexpr size_t kNumArenas = 16;
static pmm_arena_info_t mem_arena[kNumArenas];
static size_t arena_count = 0;

static ktl::atomic<int> panic_started;
static ktl::atomic<int> halted;

namespace {
lazy_init::LazyInit<RamMappableCrashlog, lazy_init::CheckType::None,
                    lazy_init::Destructor::Disabled>
    ram_mappable_crashlog;
}

bool IsEfiExpected() { return false; }

static void halt_other_cpus(void) {
  if (halted.exchange(1) == 0) {
    // stop the other cpus
    printf("stopping other cpus\n");
    arch_mp_send_ipi(MP_IPI_TARGET_ALL_BUT_LOCAL, 0, MP_IPI_HALT);

    // spin for a while
    // TODO: find a better way to spin at this low level
    for (volatile int i = 0; i < 100000000; i++) {
      __asm volatile("nop");
    }
  }
}

// TODO(fxbug.dev/98351): Refactor platform_panic_start.
void platform_panic_start(PanicStartHaltOtherCpus option) {
  arch_disable_ints();
  dlog_panic_start();

  if (option == PanicStartHaltOtherCpus::Yes) {
    halt_other_cpus();
  }

  if (panic_started.exchange(1) == 0) {
    dlog_bluescreen_init();
    // Attempt to dump the current debug trace buffer, if we have one.
    jtrace_dump(jtrace::TraceBufferType::Current);
  }
}

void* platform_get_ramdisk(size_t* size) {
  if (ramdisk_base) {
    *size = ramdisk_size;
    return ramdisk_base;
  } else {
    *size = 0;
    return nullptr;
  }
}

void platform_halt_cpu(void) {}

static zx_status_t platform_start_cpu(cpu_num_t cpu_num, uint64_t hart_id) {
  vaddr_t sp = 0;

  zx_status_t status = riscv64_create_secondary_stack(cpu_num, &sp);
  if (status != ZX_OK) {
    return status;
  }

  // Issue memory barrier before starting to ensure previous stores will be visible to new CPU
  arch::ThreadMemoryBarrier();

  unsigned long ret = sbi_hart_start(hart_id, kernel_secondary_entry_paddr, sp).error;
  dprintf(INFO, "Trying to start cpu %u, hart_id %lu returned: %d\n", cpu_num, hart_id, (int)ret);
  if (ret != 0) {
    // start failed, free the stack
    status = riscv64_free_secondary_stack(cpu_num);
    DEBUG_ASSERT(status == ZX_OK);
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

static void topology_cpu_init(void) {
  for (auto* node : system_topology::GetSystemTopology().processors()) {
    if (node->entity_type != ZBI_TOPOLOGY_ENTITY_PROCESSOR ||
        node->entity.processor.architecture != ZBI_TOPOLOGY_ARCH_RISCV) {
      panic("Invalid processor node.");
    }

    const auto& processor = node->entity.processor;
    for (uint8_t i = 0; i < processor.logical_id_count; i++) {
      // Skip the current (boot) hart, we are only starting secondary harts.
      if (processor.flags == ZBI_TOPOLOGY_PROCESSOR_PRIMARY) {
        continue;
      }
      // start the hart (ignore the return value)
      const uint64_t hart_id = processor.architecture_info.riscv.hart_id;
      platform_start_cpu(processor.logical_ids[i], hart_id);
    }
  }

  // Create a thread that checks that the secondary processors actually
  // started. Since the secondary cpus are defined in the bootloader by humans
  // it is possible they don't match the hardware.
  constexpr auto check_cpus_booted = [](void*) -> int {
    // We wait for secondary cpus to start up.
    Thread::Current::SleepRelative(ZX_SEC(5));

    // Check that all cpus in the topology are now online.
    const auto online_mask = mp_get_online_mask();
    for (auto* node : system_topology::GetSystemTopology().processors()) {
      const auto& processor = node->entity.processor;
      for (int i = 0; i < processor.logical_id_count; i++) {
        const auto logical_id = node->entity.processor.logical_ids[i];
        if ((cpu_num_to_mask(logical_id) & online_mask) == 0) {
          printf("ERROR: CPU %d did not start!\n", logical_id);
        }
      }
    }
    return 0;
  };

  auto* warning_thread = Thread::Create("platform-cpu-boot-check-thread", check_cpus_booted,
                                        nullptr, DEFAULT_PRIORITY);
  warning_thread->DetachAndResume();
}

static constexpr zbi_topology_node_t fallback_topology = {
    .entity_type = ZBI_TOPOLOGY_ENTITY_PROCESSOR,
    .parent_index = ZBI_TOPOLOGY_NO_PARENT,
    .entity = {.processor = {.logical_ids = {0},
                             .logical_id_count = 1,
                             .flags = 0,
                             .architecture = ZBI_TOPOLOGY_ARCH_RISCV}}};

static void init_topology(uint level) {
  ktl::span handoff = gPhysHandoff->cpu_topology.get();

  auto result = system_topology::Graph::InitializeSystemTopology(handoff.data(), handoff.size());
  if (result != ZX_OK) {
    printf("Failed to initialize system topology! error: %d\n", result);

    // Try to fallback to a topology of just this processor.
    result = system_topology::Graph::InitializeSystemTopology(&fallback_topology, 1);
    ASSERT(result == ZX_OK);
  }

  arch_set_num_cpus(static_cast<uint>(system_topology::GetSystemTopology().processor_count()));

  // TODO(fxbug.dev/32903) Print the whole topology of the system.
  if (DPRINTF_ENABLED_FOR_LEVEL(INFO)) {
    for (auto* proc : system_topology::GetSystemTopology().processors()) {
      auto& info = proc->entity.processor.architecture_info.arm;
      dprintf(INFO, "System topology: CPU %u:%u:%u:%u\n", info.cluster_3_id, info.cluster_2_id,
              info.cluster_1_id, info.cpu_id);
    }
  }
}

LK_INIT_HOOK(init_topology, init_topology, LK_INIT_LEVEL_VM)

static void allocate_persistent_ram(paddr_t pa, size_t length) {
  // Figure out how to divide up our persistent RAM.  Right now there are
  // three potential users:
  //
  // 1) The crashlog.
  // 2) Persistent debug logging.
  // 3) Persistent debug tracing.
  //
  // Persistent debug logging and tracing have target amounts of RAM they would
  // _like_ to have, and crash-logging has a minimum amount it is guaranteed to
  // get.  Additionally, all allocated are made in a chunks of the minimum
  // persistent RAM allocation granularity.
  //
  // Make sure that the crashlog gets as much of its minimum allocation as is
  // possible.  Then attempt to satisfy the target for persistent debug logging,
  // followed by persistent debug tracing.  Finally, give anything leftovers to
  // the crashlog.
  size_t crashlog_size = 0;
  size_t pdlog_size = 0;
  size_t jtrace_size = 0;
  {
    // start by figuring out how many chunks of RAM we have available to
    // us total.
    size_t persistent_chunks_available = length / kPersistentRamAllocationGranularity;

    // If we have not already configured a non-trivial crashlog implementation
    // for the platform, make sure that crashlog gets its minimum allocation, or
    // all of the RAM if it cannot meet even its minimum allocation.
    size_t crashlog_chunks = !PlatformCrashlog::HasNonTrivialImpl()
                                 ? ktl::min(persistent_chunks_available,
                                            kMinCrashlogSize / kPersistentRamAllocationGranularity)
                                 : 0;
    persistent_chunks_available -= crashlog_chunks;

    // Next in line is persistent debug logging.
    size_t pdlog_chunks =
        ktl::min(persistent_chunks_available,
                 kTargetPersistentDebugLogSize / kPersistentRamAllocationGranularity);
    persistent_chunks_available -= pdlog_chunks;

    // Next up is persistent debug tracing.
    size_t jtrace_chunks =
        ktl::min(persistent_chunks_available,
                 kJTraceTargetPersistentBufferSize / kPersistentRamAllocationGranularity);
    persistent_chunks_available -= jtrace_chunks;

    // Finally, anything left over can go to the crashlog.
    crashlog_chunks += persistent_chunks_available;

    crashlog_size = crashlog_chunks * kPersistentRamAllocationGranularity;
    pdlog_size = pdlog_chunks * kPersistentRamAllocationGranularity;
    jtrace_size = jtrace_chunks * kPersistentRamAllocationGranularity;
  }

  // Configure up the crashlog RAM
  if (crashlog_size > 0) {
    dprintf(INFO, "Crashlog configured with %" PRIu64 " bytes\n", crashlog_size);
    ram_mappable_crashlog.Initialize(pa, crashlog_size);
    PlatformCrashlog::Bind(ram_mappable_crashlog.Get());
  }
  size_t offset = crashlog_size;

  // Configure the persistent debuglog RAM (if we have any)
  if (pdlog_size > 0) {
    dprintf(INFO, "Persistent debug logging enabled and configured with %" PRIu64 " bytes\n",
            pdlog_size);
    persistent_dlog_set_location(paddr_to_physmap(pa + offset), pdlog_size);
    offset += pdlog_size;
  }

  // Do _not_ attempt to set the location of the debug trace buffer if this is
  // not a persistent debug trace buffer.  The location of a non-persistent
  // trace buffer would have been already set during (very) early init.
  if constexpr (kJTraceIsPersistent == jtrace::IsPersistent::Yes) {
    jtrace_set_location(paddr_to_physmap(pa + offset), jtrace_size);
    offset += jtrace_size;
  }
}

#if 0
// Called during platform_init_early, the heap is not yet present.
void ProcessZbiEarly(zbi_header_t* zbi) {
  DEBUG_ASSERT(zbi);

  // Writable bytes, as we will need to edit CMDLINE items (see below).
  ktl::span span{reinterpret_cast<ktl::byte*>(zbi), SIZE_MAX};
  zbitl::View view(span);

  for (auto it = view.begin(); it != view.end(); ++it) {
    auto [header, payload] = *it;
    switch (header->type) {
      case ZBI_TYPE_STORAGE_KERNEL: {
        gPhysHandoff = PhysHandoff::FromPayload(payload);
        break;
      }
      case ZBI_TYPE_KERNEL_DRIVER:
      case ZBI_TYPE_PLATFORM_ID:
        break;
      case ZBI_TYPE_CMDLINE: {
        if (payload.empty()) {
          break;
        }
        payload.back() = ktl::byte{'\0'};

        ParseBootOptions(
            ktl::string_view{reinterpret_cast<const char*>(payload.data()), payload.size()});

        // The CMDLINE might include entropy for the zircon cprng.
        // We don't want that information to be accesible after it has
        // been added to the kernel cmdline.
        // Editing the header of a ktl::span will not result in an error.
        // TODO(fxbug.dev/64272): Inline the following once the GCC bug is fixed.
        static_cast<void>(view.EditHeader(it, zbi_header_t{
                                          .type = ZBI_TYPE_DISCARD,
                                      }));
        mandatory_memset(payload.data(), 0, payload.size());
        break;
      }
      case ZBI_TYPE_MEM_CONFIG: {
        zbi_mem_range_t* mem_range = reinterpret_cast<zbi_mem_range_t*>(payload.data());
        size_t count = payload.size() / sizeof(zbi_mem_range_t);
        for (size_t i = 0; i < count; i++) {
          process_mem_range(mem_range++);
        }
        break;
      }
      case ZBI_TYPE_NVRAM: {
        zbi_nvram_t info;
        memcpy(&info, payload.data(), sizeof(info));

        dprintf(INFO, "boot reserve NVRAM range: phys base %#" PRIx64 " length %#" PRIx64 "\n",
                info.base, info.length);

        allocate_persistent_ram(info.base, info.length);
        boot_reserve_add_range(info.base, info.length);
        break;
      }
      case ZBI_TYPE_HW_REBOOT_REASON: {
        zbi_hw_reboot_reason_t reason;
        memcpy(&reason, payload.data(), sizeof(reason));
        platform_set_hw_reboot_reason(reason);
        break;
      }
      case ZBI_TYPE_CPU_TOPOLOGY: {
        const size_t node_count = payload.size() / static_cast<size_t>(header->extra);
        const auto* nodes = reinterpret_cast<const zbi_topology_node_t*>(payload.data());
        for (int i = 0; i < (int)node_count; i ++) {
          if (nodes[i].entity_type == ZBI_TOPOLOGY_ENTITY_PROCESSOR &&
              nodes[i].entity.processor.architecture == ZBI_TOPOLOGY_ARCH_RISCV &&
              nodes[i].entity.processor.flags == ZBI_TOPOLOGY_PROCESSOR_PRIMARY) {
            uint boot_hart_id = (uint)nodes[i].entity.processor.architecture_info.riscv.hart_id;
            riscv64_read_percpu_ptr()->hart_id = boot_hart_id;
            arch_register_hart(0, boot_hart_id);
            break;
          }
        }
        break;
      }
    };
  }

  if (auto result = view.take_error(); result.is_error()) {
    printf("ProcessZbiEarly: encountered error iterating through data ZBI: ");
    zbitl::PrintViewError(result.error_value());
  }
}

// Called after the heap is up, but before multithreading.
void ProcessZbiLate(const zbi_header_t* zbi) {
  DEBUG_ASSERT(zbi);

  zbitl::View view(zbitl::AsBytes(zbi, SIZE_MAX));

  for (auto it = view.begin(); it != view.end(); ++it) {
    auto [header, payload] = *it;
    switch (header->type) {
      case ZBI_TYPE_CPU_TOPOLOGY: {
        const size_t node_count = payload.size() / static_cast<size_t>(header->extra);
        const auto* nodes = reinterpret_cast<const zbi_topology_node_t*>(payload.data());
        init_topology(nodes, node_count);
        break;
      }
    };
  }

  if (auto result = view.take_error(); result.is_error()) {
    printf("ProcessZbiLate: encountered error iterating through data ZBI: ");
    zbitl::PrintViewError(result.error_value());
  }
}
#endif

static void process_mem_ranges(ktl::span<const zbi_mem_range_t> ranges) {
  // First process all the reserved ranges. We do this in case there are reserved regions that
  // overlap with the RAM regions that occur later in the list. If we didn't process the reserved
  // regions first, then we might add a pmm arena and have it carve out its vm_page_t array from
  // what we will later learn is reserved memory.
  for (const zbi_mem_range_t& mem_range : ranges) {
    if (mem_range.type == ZBI_MEM_RANGE_RESERVED) {
      dprintf(INFO, "ZBI: reserve mem range base %#" PRIx64 " size %#" PRIx64 "\n", mem_range.paddr,
              mem_range.length);
      boot_reserve_add_range(mem_range.paddr, mem_range.length);
    }
  }
  for (const zbi_mem_range_t& mem_range : ranges) {
    switch (mem_range.type) {
      case ZBI_MEM_RANGE_RAM:
        dprintf(INFO, "ZBI: mem arena base %#" PRIx64 " size %#" PRIx64 "\n", mem_range.paddr,
                mem_range.length);
        if (arena_count >= kNumArenas) {
          printf("ZBI: Warning, too many memory arenas, dropping additional\n");
          break;
        }
        mem_arena[arena_count] = pmm_arena_info_t{"ram", 0, mem_range.paddr, mem_range.length};
        arena_count++;
        break;
      case ZBI_MEM_RANGE_PERIPHERAL: {
        // We shouldn't be dealing with peripheral mappings on riscv.
        PANIC_UNIMPLEMENTED;
        break;
      }
      case ZBI_MEM_RANGE_RESERVED:
        // Already handled the reserved ranges.
        break;
      default:
        // Treat unknown memory range types as reserved.
        dprintf(INFO,
                "ZBI: unknown mem range base %#" PRIx64 " size %#" PRIx64 " (type %" PRIu32 ")\n",
                mem_range.paddr, mem_range.length, mem_range.type);
        boot_reserve_add_range(mem_range.paddr, mem_range.length);
        break;
    }
  }
}

// Called during platform_init_early.
static void ProcessPhysHandoff() {
  if (gPhysHandoff->nvram) {
    const zbi_nvram_t& nvram = gPhysHandoff->nvram.value();
    dprintf(INFO, "boot reserve NVRAM range: phys base %#" PRIx64 " length %#" PRIx64 "\n",
            nvram.base, nvram.length);
    allocate_persistent_ram(nvram.base, nvram.length);
    boot_reserve_add_range(nvram.base, nvram.length);
  }

  process_mem_ranges(gPhysHandoff->mem_config.get());
}

void platform_early_init(void) {
  // initialize the boot memory reservation system
  boot_reserve_init();

  ProcessPhysHandoff();

  // is the cmdline option to bypass dlog set ?
  dlog_bypass_init();

  // Serial port should be active now

  // Check if serial should be enabled
  ktl::visit([](const auto& uart) { uart_disabled = uart.extra() == 0; }, gBootOptions->serial);

  // Initialize the PmmChecker now that the cmdline has been parsed.
  pmm_checker_init_from_cmdline();

  // Add the data ZBI ramdisk to the boot reserve memory list.
  ktl::span zbi = ZbiInPhysmap();
  paddr_t ramdisk_start_phys = physmap_to_paddr(zbi.data());
  paddr_t ramdisk_end_phys = ramdisk_start_phys + ROUNDUP_PAGE_SIZE(zbi.size_bytes());
  dprintf(INFO, "reserving ramdisk phys range [%#" PRIx64 ", %#" PRIx64 "]\n", ramdisk_start_phys,
          ramdisk_end_phys - 1);
  boot_reserve_add_range(ramdisk_start_phys, ramdisk_end_phys - ramdisk_start_phys);

  // check if a memory limit was passed in via kernel.memory-limit-mb and
  // find memory ranges to use if one is found.
  zx_status_t status = memory_limit_init();
  bool have_limit = (status == ZX_OK);
  for (size_t i = 0; i < arena_count; i++) {
    if (have_limit) {
      // Figure out and add arenas based on the memory limit and our range of DRAM
      status = memory_limit_add_range(mem_arena[i].base, mem_arena[i].size, mem_arena[i]);
    }

    // If no memory limit was found, or adding arenas from the range failed, then add
    // the existing global arena.
    if (!have_limit || status != ZX_OK) {
      // Init returns not supported if no limit exists
      if (status != ZX_ERR_NOT_SUPPORTED) {
        dprintf(INFO, "memory limit lib returned an error (%d), falling back to defaults\n",
                status);
      }
      pmm_add_arena(&mem_arena[i]);
    }
  }

  // add any pending memory arenas the memory limit library has pending
  if (have_limit) {
    status = memory_limit_add_arenas(mem_arena[0]);
    DEBUG_ASSERT(status == ZX_OK);
  }

  // tell the boot allocator to mark ranges we've reserved as off limits
  boot_reserve_wire();
}

void platform_prevm_init() {}

// Called after the heap is up but before the system is multithreaded.
void platform_init_pre_thread(uint) { topology_cpu_init(); }

LK_INIT_HOOK(platform_init_pre_thread, platform_init_pre_thread, LK_INIT_LEVEL_VM)

void platform_init() {}

// after the fact create a region to reserve the peripheral map(s)
static void platform_init_postvm(uint level) {}

LK_INIT_HOOK(platform_postvm, platform_init_postvm, LK_INIT_LEVEL_VM)

void platform_dputs_thread(const char* str, size_t len) {
  if (uart_disabled) {
    return;
  }
  uart_puts(str, len, true);
}

void platform_dputs_irq(const char* str, size_t len) {
  if (uart_disabled) {
    return;
  }
  uart_puts(str, len, false);
}

int platform_dgetc(char* c, bool wait) {
  if (uart_disabled) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  int ret = uart_getc(wait);
  // uart_getc returns ZX_ERR_INTERNAL if no input was read
  if (!wait && ret == ZX_ERR_INTERNAL)
    return 0;
  if (ret < 0)
    return ret;
  *c = static_cast<char>(ret);
  return 1;
}

void platform_pputc(char c) {
  if (uart_disabled) {
    return;
  }
  uart_pputc(c);
}

int platform_pgetc(char* c, bool wait) {
  if (uart_disabled) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  int r = uart_pgetc();
  if (r < 0) {
    return r;
  }

  *c = static_cast<char>(r);
  return 0;
}

/* no built in framebuffer */
zx_status_t display_get_info(struct display_info* info) { return ZX_ERR_NOT_FOUND; }

void platform_specific_halt(platform_halt_action suggested_action, zircon_crash_reason_t reason,
                            bool halt_on_panic) {
  sbi_call(SBI_SHUTDOWN);
  __UNREACHABLE;
}

zx_status_t platform_mexec_patch_zbi(uint8_t* zbi, const size_t len) { return ZX_OK; }

void platform_mexec_prep(uintptr_t new_bootimage_addr, size_t new_bootimage_len) {}

void platform_mexec(mexec_asm_func mexec_assembly, memmov_ops_t* ops, uintptr_t new_bootimage_addr,
                    size_t new_bootimage_len, uintptr_t entry64_addr) {}

bool platform_serial_enabled(void) { return !uart_disabled; }

bool platform_early_console_enabled() { return false; }

// Initialize Resource system after the heap is initialized.
static void riscv64_resource_dispatcher_init_hook(unsigned int rl) {
  // 64 bit address space for MMIO on RISCV64
  zx_status_t status = ResourceDispatcher::InitializeAllocator(ZX_RSRC_KIND_MMIO, 0, UINT64_MAX);
  if (status != ZX_OK) {
    printf("Resources: Failed to initialize MMIO allocator: %d\n", status);
  }
  // Set up IRQs based on values from the PLIC
  status = ResourceDispatcher::InitializeAllocator(ZX_RSRC_KIND_IRQ, interrupt_get_base_vector(),
                                                   interrupt_get_max_vector());
  if (status != ZX_OK) {
    printf("Resources: Failed to initialize IRQ allocator: %d\n", status);
  }
  // Set up range of valid system resources.
  status = ResourceDispatcher::InitializeAllocator(ZX_RSRC_KIND_SYSTEM, 0, ZX_RSRC_SYSTEM_COUNT);
  if (status != ZX_OK) {
    printf("Resources: Failed to initialize system allocator: %d\n", status);
  }
}

LK_INIT_HOOK(riscv64_resource_init, riscv64_resource_dispatcher_init_hook, LK_INIT_LEVEL_HEAP)

void topology_init() {
  // Setup the CPU distance map with the already initialized topology.
  const auto processor_count =
      static_cast<uint>(system_topology::GetSystemTopology().processor_count());
  CpuDistanceMap::Initialize(processor_count, [](cpu_num_t from_id, cpu_num_t to_id) { return 0; });

  const CpuDistanceMap::Distance kDistanceThreshold = 2u;
  CpuDistanceMap::Get().set_distance_threshold(kDistanceThreshold);

  CpuDistanceMap::Get().Dump();
}

zx_status_t platform_mp_prep_cpu_unplug(cpu_num_t cpu_id) {
  return arch_mp_prep_cpu_unplug(cpu_id);
}

zx_status_t platform_mp_cpu_unplug(cpu_num_t cpu_id) { return arch_mp_cpu_unplug(cpu_id); }

zx_status_t platform_append_mexec_data(ktl::span<ktl::byte> data_zbi) { return ZX_OK; }
