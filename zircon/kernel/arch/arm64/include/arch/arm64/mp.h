// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_ARM64_INCLUDE_ARCH_ARM64_MP_H_
#define ZIRCON_KERNEL_ARCH_ARM64_INCLUDE_ARCH_ARM64_MP_H_

#define PERCPU_IN_RESTRICTED_MODE 32

#ifndef __ASSEMBLER__

#include <zircon/compiler.h>

#include <arch/arm64.h>
#include <arch/arm64/feature.h>
#include <kernel/align.h>
#include <kernel/cpu.h>

// bits for mpidr register
#define MPIDR_AFF0_MASK 0xFFULL
#define MPIDR_AFF0_SHIFT 0
#define MPIDR_AFF1_MASK (0xFFULL << 8)
#define MPIDR_AFF1_SHIFT 8
#define MPIDR_AFF2_MASK (0xFFULL << 16)
#define MPIDR_AFF2_SHIFT 16
#define MPIDR_AFF3_MASK (0xFFULL << 32)
#define MPIDR_AFF3_SHIFT 32

// construct a ARM MPID from cluster (AFF1) and cpu number (AFF0)
#define ARM64_MPID(cluster, cpu)                       \
  (((cluster << MPIDR_AFF1_SHIFT) & MPIDR_AFF1_MASK) | \
   ((cpu << MPIDR_AFF0_SHIFT) & MPIDR_AFF0_MASK))

#define ARM64_MPIDR_MASK (MPIDR_AFF3_MASK | MPIDR_AFF2_MASK | MPIDR_AFF1_MASK | MPIDR_AFF0_MASK)

// TODO: add support for AFF2 and AFF3

struct percpu;

// Per cpu structure, pointed to by a fixed register while in kernel mode.
// Aligned on the maximum architectural cache line to avoid cache
// line sharing between cpus.
struct arm64_percpu {
  // cpu number
  cpu_num_t cpu_num;

  // Whether blocking is disallowed.  See arch_blocking_disallowed().
  uint32_t blocking_disallowed;

  // Number of spinlocks currently held.
  uint32_t num_spinlocks;

  // Microarchitecture of this cpu (ex: Cortex-A53)
  arm64_microarch microarch;

  // True if the branch predictor should be invalidated during context switch or on suspicious
  // entries to EL2 to mitigate Spectre V2 attacks.
  bool should_invalidate_bp_on_context_switch;

  // A pointer providing fast access to the high-level arch-agnostic per-cpu struct.
  percpu* high_level_percpu;

  // Flag to track that we're in restricted mode.
  uint32_t in_restricted_mode;
} __CPU_ALIGN;
static_assert(offsetof(struct arm64_percpu, in_restricted_mode) == PERCPU_IN_RESTRICTED_MODE,
              "in_restricted mode is at the wrong offset");

void arch_init_cpu_map(uint cluster_count, const uint* cluster_cpus);
void arch_register_mpid(uint cpu_id, uint64_t mpid);
void arm64_init_percpu_early();

extern uint arm_num_cpus;
extern uint arm64_cpu_cluster_ids[SMP_MAX_CPUS];
extern uint arm64_cpu_cpu_ids[SMP_MAX_CPUS];

// Use the x20 register to always point at the local cpu structure for fast access.
// x20 is the first available callee-saved register that clang will allow to be marked
// as fixed (via -ffixed-x20 command line). Since it's callee saved when making firmware
// calls to PSCI or SMCC the register will be naturally saved and restored.
inline void arm64_write_percpu_ptr(struct arm64_percpu* percpu) {
  __asm__ volatile("mov x20, %0" ::"r"(percpu));
}

inline struct arm64_percpu* arm64_read_percpu_ptr() {
  struct arm64_percpu* p;
  __asm__ volatile("mov %0, x20" : "=r"(p));
  return p;
}

// Mark as volatile to force a read of the field to make sure the compiler
// always emits a read when asked and does not cache a copy between.  For the
// same reason, this can't by done via the arm64_percpu_ptr variable, since
// the compiler could copy x20 into another register and access it after a
// reschedule.
template <typename T, size_t Offset>
[[gnu::always_inline]] inline T arm64_read_percpu_field() {
  static_assert((Offset & (alignof(T) - 1)) == 0, "Bad offset alignment");
  if constexpr (sizeof(T) == sizeof(uint32_t)) {
    T value;
    __asm__ volatile("ldr %w[val], [x20, %[offset]]" : [val] "=r"(value) : [offset] "Ir"(Offset));
    return value;
  } else {
    static_assert(sizeof(T) == sizeof(uint64_t));
    T value;
    __asm__ volatile("ldr %[val], [x20, %[offset]]" : [val] "=r"(value) : [offset] "Ir"(Offset));
    return value;
  }
}
#define READ_PERCPU_FIELD(field) \
  (arm64_read_percpu_field<decltype(arm64_percpu::field), offsetof(arm64_percpu, field)>())

template <typename T, size_t Offset>
[[gnu::always_inline]] inline void arm64_write_percpu_field(T value) {
  static_assert((Offset & (alignof(T) - 1)) == 0, "Bad offset alignment");
  if constexpr (sizeof(T) == sizeof(uint32_t)) {
    __asm__ volatile("str %w[val], [x20, %[offset]]" ::[val] "r"(value), [offset] "Ir"(Offset)
                     : "memory");
  } else {
    static_assert(sizeof(T) == sizeof(uint64_t));
    __asm__ volatile("str %[val], [x20, %[offset]]" ::[val] "r"(value), [offset] "Ir"(Offset)
                     : "memory");
  }
}
#define WRITE_PERCPU_FIELD(field, value) \
  (arm64_write_percpu_field<decltype(arm64_percpu::field), offsetof(arm64_percpu, field)>(value))

// Return a pointer to the high-level percpu struct for the calling CPU.
inline struct percpu* arch_get_curr_percpu() { return READ_PERCPU_FIELD(high_level_percpu); }

inline cpu_num_t arch_curr_cpu_num() { return READ_PERCPU_FIELD(cpu_num); }

// TODO(https://fxbug.dev/32903) get num_cpus from topology.
// This needs to be set very early (before arch_init).
inline void arch_set_num_cpus(uint cpu_count) { arm_num_cpus = cpu_count; }

inline uint arch_max_num_cpus() { return arm_num_cpus; }

// translate a cpu number back to the cluster ID (AFF1)
inline uint arch_cpu_num_to_cluster_id(cpu_num_t cpu) {
  DEBUG_ASSERT(cpu < SMP_MAX_CPUS);

  return arm64_cpu_cluster_ids[cpu];
}

// translate a cpu number back to the MP cpu number within a cluster (AFF0)
inline uint arch_cpu_num_to_cpu_id(cpu_num_t cpu) {
  DEBUG_ASSERT(cpu < SMP_MAX_CPUS);

  return arm64_cpu_cpu_ids[cpu];
}

// Translate a CPU number back to the MPIDR of the CPU.
uint64_t arch_cpu_num_to_mpidr(cpu_num_t cpu_num);

// translate mpidr to cpu number
cpu_num_t arm64_mpidr_to_cpu_num(uint64_t mpidr);

// Setup the high-level percpu struct pointer for |cpu_num|.
void arch_setup_percpu(cpu_num_t cpu_num, struct percpu* percpu);

inline void arch_set_restricted_flag(bool restricted) {
  WRITE_PERCPU_FIELD(in_restricted_mode, restricted ? 1 : 0);
}

inline bool arch_get_restricted_flag() { return READ_PERCPU_FIELD(in_restricted_mode); }

#endif  // !__ASSEMBLER__

#endif  // ZIRCON_KERNEL_ARCH_ARM64_INCLUDE_ARCH_ARM64_MP_H_
