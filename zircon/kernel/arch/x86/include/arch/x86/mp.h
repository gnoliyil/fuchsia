// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2016 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_X86_INCLUDE_ARCH_X86_MP_H_
#define ZIRCON_KERNEL_ARCH_X86_INCLUDE_ARCH_X86_MP_H_

/* describes the per cpu structure pointed to by gs: in the kernel */

/* offsets into this structure, used by assembly */
#define PERCPU_DIRECT_OFFSET 0x0
#define PERCPU_CURRENT_THREAD_OFFSET 0x8
//      ZX_TLS_STACK_GUARD_OFFSET      0x10
//      ZX_TLS_UNSAFE_SP_OFFSET        0x18
#define PERCPU_SAVED_USER_SP_OFFSET 0x20
#define PERCPU_IN_RESTRICTED_MODE 0x28
#define PERCPU_GPF_RETURN_OFFSET 0x50
#define PERCPU_CPU_NUM_OFFSET 0x58
#define PERCPU_HIGH_LEVEL_PERCPU_OFFSET 0x68
#define PERCPU_DEFAULT_TSS_OFFSET 0x70
#define PERCPU_INTERRUPT_STACKS_NMI_OFFSET 0x20e0

/* offset of default_tss.rsp0 */
#define PERCPU_KERNEL_SP_OFFSET (PERCPU_DEFAULT_TSS_OFFSET + 4)

#define INTERRUPT_STACK_SIZE (4096)

#ifndef __ASSEMBLER__

#include <assert.h>
#include <stdint.h>
#include <zircon/compiler.h>
#include <zircon/tls.h>
#include <zircon/types.h>

#include <arch/x86.h>
#include <arch/x86/idle_states.h>
#include <arch/x86/idt.h>
#include <kernel/align.h>
#include <kernel/cpu.h>
#include <ktl/atomic.h>
#include <ktl/declval.h>

struct Thread;
struct percpu;

struct x86_percpu {
  /* a direct pointer to ourselves */
  struct x86_percpu *direct;

  /* the current thread */
  Thread *current_thread;

  // The offsets of these two slots are published in
  // system/public/zircon/tls.h and known to the compiler.
  uintptr_t stack_guard;
  uintptr_t kernel_unsafe_sp;

  /* temporarily saved during a syscall */
  uintptr_t saved_user_sp;

  /* flag to track that we're in restricted mode */
  uint32_t in_restricted_mode;

  /* Whether blocking is disallowed.  See arch_blocking_disallowed(). */
  uint32_t blocking_disallowed;

  /* Memory for IPI-free rescheduling of idle CPUs with monitor/mwait. */
  volatile uint8_t *monitor;

  /* Interlock to avoid HLT on idle CPUs without monitor/mwait. */
  /* halt_interlock is never used on CPUs that have enabled monitor/mwait for idle. */
  ktl::atomic<uint32_t> halt_interlock;

  /* Supported mwait C-states for idle CPUs. */
  X86IdleStates *idle_states;

  /* local APIC id */
  uint32_t apic_id;

  /* If nonzero and we receive a GPF, change the return IP to this value. */
  uintptr_t gpf_return_target;

  /* CPU number */
  cpu_num_t cpu_num;

  /* Number of spinlocks currently held */
  uint32_t num_spinlocks;

  /* Last user VmAspace that was active on this core. Lazily updated. */
  void *last_user_aspace;

  /* A pointer providing fast access to the high-level arch-agnostic per-cpu struct. */
  percpu *high_level_percpu;

  /* This CPU's default TSS */
  tss_t default_tss __ALIGNED(16);

  /* Reserved space for special interrupt stacks */
  struct {
    uint8_t nmi[INTERRUPT_STACK_SIZE] __ALIGNED(16);
    uint8_t machine_check[INTERRUPT_STACK_SIZE] __ALIGNED(16);
    uint8_t double_fault[INTERRUPT_STACK_SIZE] __ALIGNED(16);
  } interrupt_stacks;
} __CPU_ALIGN;

static_assert(__offsetof(struct x86_percpu, direct) == PERCPU_DIRECT_OFFSET);
static_assert(__offsetof(struct x86_percpu, current_thread) == PERCPU_CURRENT_THREAD_OFFSET);
static_assert(__offsetof(struct x86_percpu, stack_guard) == ZX_TLS_STACK_GUARD_OFFSET);
static_assert(__offsetof(struct x86_percpu, kernel_unsafe_sp) == ZX_TLS_UNSAFE_SP_OFFSET);
static_assert(__offsetof(struct x86_percpu, saved_user_sp) == PERCPU_SAVED_USER_SP_OFFSET);
static_assert(__offsetof(struct x86_percpu, gpf_return_target) == PERCPU_GPF_RETURN_OFFSET);
static_assert(__offsetof(struct x86_percpu, cpu_num) == PERCPU_CPU_NUM_OFFSET);
static_assert(__offsetof(struct x86_percpu, high_level_percpu) == PERCPU_HIGH_LEVEL_PERCPU_OFFSET);
static_assert(__offsetof(struct x86_percpu, default_tss) == PERCPU_DEFAULT_TSS_OFFSET);
static_assert(__offsetof(struct x86_percpu, default_tss.rsp0) == PERCPU_KERNEL_SP_OFFSET);
static_assert(__offsetof(struct x86_percpu, interrupt_stacks.nmi) ==
              PERCPU_INTERRUPT_STACKS_NMI_OFFSET);

static_assert(sizeof(ktl::declval<x86_percpu>().interrupt_stacks.nmi) == INTERRUPT_STACK_SIZE);

extern struct x86_percpu bp_percpu;
extern struct x86_percpu *ap_percpus;

// This needs to be run very early in the boot process from start.S and as
// each CPU is brought up.
// Called from assembly.
extern "C" void x86_init_percpu(cpu_num_t cpu_num);

/* used to set the bootstrap processor's apic_id once the APIC is initialized */
void x86_set_local_apic_id(uint32_t apic_id);

int x86_apic_id_to_cpu_num(uint32_t apic_id);

// Allocate all of the necessary structures for all of the APs to run.
zx_status_t x86_allocate_ap_structures(uint32_t *apic_ids, uint8_t cpu_count);

inline struct x86_percpu *x86_get_percpu() {
  return (struct x86_percpu *)x86_read_gs_offset64(PERCPU_DIRECT_OFFSET);
}

// Return a pointer to the high-level percpu struct for the calling CPU.
inline struct percpu *arch_get_curr_percpu() {
  return ((struct percpu *)x86_read_gs_offset64(PERCPU_HIGH_LEVEL_PERCPU_OFFSET));
}

inline cpu_num_t arch_curr_cpu_num() { return x86_read_gs_offset32(PERCPU_CPU_NUM_OFFSET); }

extern uint8_t x86_num_cpus;
inline uint arch_max_num_cpus() { return x86_num_cpus; }

#define READ_PERCPU_FIELD32(field) x86_read_gs_offset32(offsetof(struct x86_percpu, field))

#define WRITE_PERCPU_FIELD32(field, value) \
  x86_write_gs_offset32(offsetof(struct x86_percpu, field), (value))

void x86_ipi_halt_handler(void *) __NO_RETURN;

// Called from assembly.
extern "C" void x86_secondary_entry(ktl::atomic<unsigned int> *aps_still_booting, Thread *thread);

void x86_force_halt_all_but_local_and_bsp();

// Setup the high-level percpu struct pointer for |cpu_num|.
void arch_setup_percpu(cpu_num_t cpu_num, struct percpu *percpu);

inline void arch_set_restricted_flag(bool set) {
  WRITE_PERCPU_FIELD32(in_restricted_mode, set ? 1 : 0);
}

#endif  // !__ASSEMBLER__

#endif  // ZIRCON_KERNEL_ARCH_X86_INCLUDE_ARCH_X86_MP_H_
