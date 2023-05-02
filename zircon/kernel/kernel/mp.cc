// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2014 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "kernel/mp.h"

#include <assert.h>
#include <debug.h>
#include <inttypes.h>
#include <lib/arch/intrin.h>
#include <lib/console.h>
#include <lib/fit/defer.h>
#include <lib/lockup_detector.h>
#include <lib/system-topology.h>
#include <lib/zircon-internal/macros.h>
#include <platform.h>
#include <stdlib.h>
#include <trace.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <arch/mp.h>
#include <arch/ops.h>
#include <dev/interrupt.h>
#include <fbl/algorithm.h>
#include <kernel/align.h>
#include <kernel/cpu.h>
#include <kernel/deadline.h>
#include <kernel/dpc.h>
#include <kernel/event.h>
#include <kernel/mp.h>
#include <kernel/mutex.h>
#include <kernel/scheduler.h>
#include <kernel/spinlock.h>
#include <kernel/stats.h>
#include <kernel/thread_lock.h>
#include <kernel/timer.h>
#include <ktl/bit.h>
#include <lk/init.h>
#include <platform/timer.h>

#define LOCAL_TRACE 0

// a global state structure, aligned on cpu cache line to minimize aliasing
struct mp_state mp __CPU_ALIGN_EXCLUSIVE;

// Helpers used for implementing mp_sync
struct mp_sync_context;
static void mp_sync_task(void* context);

void mp_init() {}

void mp_prepare_current_cpu_idle_state(bool idle) { arch_prepare_current_cpu_idle_state(idle); }

void mp_reschedule(cpu_mask_t mask, uint flags) {
  DEBUG_ASSERT(arch_ints_disabled());

  const cpu_num_t local_cpu = arch_curr_cpu_num();

  LTRACEF("local %u, mask %#x\n", local_cpu, mask);

  // mask out cpus that are not active and the local cpu
  mask &= mp.active_cpus.load();
  mask &= ~cpu_num_to_mask(local_cpu);

  LTRACEF("local %u, post mask target now 0x%x\n", local_cpu, mask);

  // if we have no work to do, return
  if (mask == 0) {
    return;
  }

  arch_mp_reschedule(mask);
}

void mp_interrupt(mp_ipi_target_t target, cpu_mask_t mask) {
  arch_mp_send_ipi(target, mask, MP_IPI_INTERRUPT);
}

struct mp_sync_context {
  mp_sync_task_t task;
  void* task_context;
  // Mask of which CPUs need to finish the task
  ktl::atomic<cpu_mask_t> outstanding_cpus;
};

static void mp_sync_task(void* raw_context) {
  auto context = reinterpret_cast<mp_sync_context*>(raw_context);
  context->task(context->task_context);
  // use seq-cst atomic to ensure this update is not seen before the
  // side-effects of context->task
  context->outstanding_cpus.fetch_and(~cpu_num_to_mask(arch_curr_cpu_num()));
}

/* @brief Execute a task on the specified CPUs, and block on the calling
 *        CPU until all CPUs have finished the task.
 *
 *  If MP_IPI_TARGET_ALL or MP_IPI_TARGET_ALL_BUT_LOCAL is the target, the online CPU
 *  mask will be used to determine actual targets.
 *
 * Interrupts must be disabled if calling with MP_IPI_TARGET_ALL_BUT_LOCAL as target
 *
 * The callback in |task| will always be called with |arch_blocking_disallowed()|
 * set to true.
 */
void mp_sync_exec(mp_ipi_target_t target, cpu_mask_t mask, mp_sync_task_t task, void* context) {
  uint num_cpus = arch_max_num_cpus();

  if (target == MP_IPI_TARGET_ALL) {
    mask = mp_get_online_mask();
  } else if (target == MP_IPI_TARGET_ALL_BUT_LOCAL) {
    // targeting all other CPUs but the current one is hazardous
    // if the local CPU may be changed underneath us
    DEBUG_ASSERT(arch_ints_disabled());
    mask = mp_get_online_mask() & ~cpu_num_to_mask(arch_curr_cpu_num());
  } else {
    // Mask any offline CPUs from target list
    mask &= mp_get_online_mask();
  }

  // disable interrupts so our current CPU doesn't change
  interrupt_saved_state_t irqstate = arch_interrupt_save();
  arch::ThreadMemoryBarrier();

  const cpu_num_t local_cpu = arch_curr_cpu_num();

  // remove self from target lists, since no need to IPI ourselves
  bool targetting_self = !!(mask & cpu_num_to_mask(local_cpu));
  mask &= ~cpu_num_to_mask(local_cpu);

  // create tasks to enqueue (we need one per target due to each containing
  // a linked list node
  struct mp_sync_context sync_context = {
      .task = task,
      .task_context = context,
      .outstanding_cpus = mask,
  };

  struct mp_ipi_task sync_tasks[SMP_MAX_CPUS] = {};
  for (cpu_num_t i = 0; i < num_cpus; ++i) {
    sync_tasks[i].func = mp_sync_task;
    sync_tasks[i].context = &sync_context;
  }

  // enqueue tasks
  mp.ipi_task_lock.Acquire();
  cpu_mask_t remaining = mask;
  cpu_num_t cpu_id = 0;
  while (remaining && cpu_id < num_cpus) {
    if (remaining & 1) {
      mp.ipi_task_list[cpu_id].push_back(&sync_tasks[cpu_id]);
    }
    remaining >>= 1;
    cpu_id++;
  }
  mp.ipi_task_lock.Release();

  // let CPUs know to begin executing
  arch_mp_send_ipi(MP_IPI_TARGET_MASK, mask, MP_IPI_GENERIC);

  if (targetting_self) {
    bool previous_blocking_disallowed = arch_blocking_disallowed();
    arch_set_blocking_disallowed(true);
    mp_sync_task(&sync_context);
    arch_set_blocking_disallowed(previous_blocking_disallowed);
  }
  arch::ThreadMemoryBarrier();

  // we can take interrupts again once we've executed our task
  arch_interrupt_restore(irqstate);

  bool ints_disabled = arch_ints_disabled();
  // wait for all other CPUs to be done with the context
  while (1) {
    // See comment in mp_unplug_trampoline about related CPU hotplug
    // guarantees.
    cpu_mask_t outstanding = sync_context.outstanding_cpus.load(ktl::memory_order_relaxed);
    cpu_mask_t online = mp_get_online_mask();
    if ((outstanding & online) == 0) {
      break;
    }

    // If interrupts are still disabled, we need to attempt to process any
    // tasks queued for us in order to prevent deadlock.
    if (ints_disabled) {
      // Optimistically check if our task list has work without the lock.
      // mp_mbx_generic_irq will take the lock and check again.
      bool empty = [local_cpu]() TA_NO_THREAD_SAFETY_ANALYSIS {
        return mp.ipi_task_list[local_cpu].is_empty();
      }();
      if (!empty) {
        bool previous_blocking_disallowed = arch_blocking_disallowed();
        arch_set_blocking_disallowed(true);
        mp_mbx_generic_irq(nullptr);
        arch_set_blocking_disallowed(previous_blocking_disallowed);
        continue;
      }
    }

    arch::Yield();
  }
  arch::ThreadMemoryBarrier();

  // make sure the sync_tasks aren't in lists anymore, since they're
  // stack allocated
  mp.ipi_task_lock.AcquireIrqSave(irqstate);
  for (cpu_num_t i = 0; i < num_cpus; ++i) {
    // If a task is still around, it's because the CPU went offline.
    if (sync_tasks[i].InContainer()) {
      sync_tasks[i].RemoveFromContainer();
    }
  }
  mp.ipi_task_lock.ReleaseIrqRestore(irqstate);
}

static void mp_unplug_trampoline() TA_REQ(thread_lock) __NO_RETURN;
static void mp_unplug_trampoline() {
  Thread* ct = Thread::Current::Get();
  auto unplug_done = reinterpret_cast<Event*>(ct->task_state().arg());

  lockup_secondary_shutdown();

  // We're still holding the incoming lock from the reschedule that took us here.
  DEBUG_ASSERT(ct->scheduler_state().previous_thread() != nullptr);
  ct->scheduler_state().previous_thread()->get_lock().AssertHeld();

  Scheduler::MigrateUnpinnedThreads();
  DEBUG_ASSERT(!mp_is_cpu_active(arch_curr_cpu_num()));

  // Now that this CPU is no longer active, it is critical that this thread
  // never block.  If this thread blocks, the scheduler may attempt to select
  // this CPU's idle thread to run.  Doing so would violate an invariant: tasks
  // may only be scheduled on active CPUs.
  DEBUG_ASSERT(arch_blocking_disallowed());

  // Note that before this invocation, but after we stopped accepting
  // interrupts, we may have received a synchronous task to perform.
  // Clearing this flag will cause the mp_sync_exec caller to consider
  // this CPU done.  If this CPU comes back online before other all
  // of the other CPUs finish their work (very unlikely, since tasks
  // should be quick), then this CPU may execute the task.
  mp_set_curr_cpu_online(false);

  // We had better not be holding any OwnedWaitQueues at this point in time
  // (it is unclear how we would have ever obtained any in the first place
  // since everything this thread ever does is in this function).
  ct->wait_queue_state().AssertNoOwnedWaitQueues();

  // Release the incoming lock but do *not* enable interrupts, we want this CPU
  // to never receive another interrupt.
  Scheduler::LockHandoff();

  // Stop and then shutdown this CPU's platform timer.
  platform_stop_timer();
  platform_shutdown_timer();

  // Shutdown the interrupt controller for this CPU.  On some platforms (arm64 with GIC) receiving
  // an interrupt at a powered off CPU can result in implementation defined behavior (including
  // resetting the whole system).
  shutdown_interrupts_curr_cpu();

  // flush all of our caches
  arch_flush_state_and_halt(unplug_done);
}

// Hotplug the given cpus.  Blocks until the CPUs are up, or a failure is
// detected.
//
// This should be called in a thread context
zx_status_t mp_hotplug_cpu_mask(cpu_mask_t cpu_mask) {
  DEBUG_ASSERT(!arch_ints_disabled());
  Guard<Mutex> lock(&mp.hotplug_lock);

  // Make sure all of the requested CPUs are offline
  if (cpu_mask & mp_get_online_mask()) {
    return ZX_ERR_BAD_STATE;
  }

  while (cpu_mask != 0) {
    cpu_num_t cpu_id = highest_cpu_set(cpu_mask);
    cpu_mask &= ~cpu_num_to_mask(cpu_id);

    zx_status_t status = platform_mp_cpu_hotplug(cpu_id);
    if (status != ZX_OK) {
      return status;
    }
  }

  return ZX_OK;
}

// Unplug a single CPU.  Must be called while holding the hotplug lock
static zx_status_t mp_unplug_cpu_mask_single_locked(cpu_num_t cpu_id, zx_time_t deadline,
                                                    Thread** leaked_thread) {
  percpu& percpu_to_unplug = percpu::Get(cpu_id);

  Thread* thread = nullptr;
  auto cleanup_thread = fit::defer([&thread, &leaked_thread, cpu_id]() {
    // TODO(fxbug.dev/34447): Work around a race in thread cleanup by leaking the thread and stack
    // structure. Since we're only using this while turning off the system currently, it's not a big
    // problem leaking the thread structure and stack.
    if (leaked_thread) {
      *leaked_thread = thread;
    } else if (thread != nullptr) {
      TRACEF("WARNING: leaking thread for cpu %u\n", cpu_id);
    }
  });

  // Wait for |percpu_to_unplug| to complete any in-progress DPCs and terminate its DPC thread.
  // Later, once nothing is running on it, we'll migrate its queued DPCs to another CPU.
  zx_status_t status = percpu_to_unplug.dpc_queue.Shutdown(deadline);
  if (status != ZX_OK) {
    return status;
  }

  // TODO(maniscalco): |cpu_to_unplug| is about to shutdown.  We should ensure it has no pinned
  // threads (except maybe the idle thread).  Once we're confident we've terminated/migrated them
  // all, this would be a good place to DEBUG_ASSERT.

  // Create a thread for the unplug.  We will cause the target CPU to
  // context switch to this thread.  After this happens, it should no
  // longer be accessing system state and can be safely shut down.
  //
  // This thread is pinned to the target CPU and set to run with the
  // highest priority.  This should cause it to pick up the thread
  // immediately (or very soon, if for some reason there is another
  // HIGHEST_PRIORITY task scheduled in between when we resume the
  // thread and when the CPU is woken up).
  Event unplug_done;
  thread = Thread::CreateEtc(nullptr, "unplug_thread", nullptr, &unplug_done,
                             SchedulerState::BaseProfile{HIGHEST_PRIORITY}, mp_unplug_trampoline);
  if (thread == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }

  status = platform_mp_prep_cpu_unplug(cpu_id);
  if (status != ZX_OK) {
    return status;
  }

  // Pin to the target CPU
  thread->SetCpuAffinity(cpu_num_to_mask(cpu_id));

  thread->SetBaseProfile(SchedulerState::BaseProfile{
      SchedDeadlineParams{SchedDuration{ZX_MSEC(9)}, SchedDuration{ZX_MSEC(10)}}});

  status = thread->DetachAndResume();
  if (status != ZX_OK) {
    return status;
  }

  // Wait for the unplug thread to get scheduled on the target.
  status = unplug_done.WaitDeadline(deadline, Interruptible::No);
  if (status != ZX_OK) {
    return status;
  }

  // Now that the cpu is no longer processing tasks, migrate
  // |percpu_to_unplug|'s TimerQueue and DpcQueue to this cpu.
  percpu& current_percpu = percpu::GetCurrent();
  current_percpu.timer_queue.TransitionOffCpu(percpu_to_unplug.timer_queue);
  current_percpu.dpc_queue.TransitionOffCpu(percpu_to_unplug.dpc_queue);

  return platform_mp_cpu_unplug(cpu_id);
}

// Unplug the given cpus.  Blocks until the CPUs are removed or |deadline| has been reached.
//
// Partial failure may occur (in which some CPUs are removed but not others).
//
// This should be called in a thread context.
//
// |leaked_threads| is an optional array of pointers to threads with length
// |SMP_MAX_CPUS|. If null, the threads used to "cleanup" each CPU will be
// leaked. If non-null, they will be returned to the caller so that the caller
// can |thread_forget| them.
zx_status_t mp_unplug_cpu_mask(cpu_mask_t cpu_mask, zx_time_t deadline, Thread** leaked_threads) {
  DEBUG_ASSERT(!arch_ints_disabled());
  Guard<Mutex> lock(&mp.hotplug_lock);

  // Make sure all of the requested CPUs are online
  if (cpu_mask & ~mp_get_online_mask()) {
    return ZX_ERR_BAD_STATE;
  }

  while (cpu_mask != 0) {
    cpu_num_t cpu_id = highest_cpu_set(cpu_mask);
    cpu_mask &= ~cpu_num_to_mask(cpu_id);

    zx_status_t status = mp_unplug_cpu_mask_single_locked(
        cpu_id, deadline, leaked_threads ? &leaked_threads[cpu_id] : nullptr);
    if (status != ZX_OK) {
      return status;
    }
  }

  return ZX_OK;
}

void mp_mbx_generic_irq(void*) {
  DEBUG_ASSERT(arch_ints_disabled());
  const cpu_num_t local_cpu = arch_curr_cpu_num();

  CPU_STATS_INC(generic_ipis);

  while (1) {
    struct mp_ipi_task* task;
    mp.ipi_task_lock.Acquire();
    task = mp.ipi_task_list[local_cpu].pop_front();
    mp.ipi_task_lock.Release();
    if (task == NULL) {
      break;
    }

    task->func(task->context);
  }
}

void mp_mbx_reschedule_irq(void*) {
  const cpu_num_t cpu = arch_curr_cpu_num();

  LTRACEF("cpu %u\n", cpu);

  CPU_STATS_INC(reschedule_ipis);

  if (mp.active_cpus.load() & cpu_num_to_mask(cpu)) {
    Thread::Current::preemption_state().PreemptSetPending(cpu_num_to_mask(cpu));
  }
}

void mp_mbx_interrupt_irq(void*) {
  const cpu_num_t cpu = arch_curr_cpu_num();

  LTRACEF("cpu %u\n", cpu);

  // do nothing, the entire point of this interrupt is to simply have one
  // delivered to the cpu.
}

zx_status_t platform_mp_cpu_hotplug(cpu_num_t cpu_id) { return arch_mp_cpu_hotplug(cpu_id); }

namespace {

// Tracks the CPUs that are "ready".
ktl::atomic<cpu_mask_t> ready_cpu_mask{0};

// Signals when all CPUs are ready.
Event ready_cpu_event;

}  // namespace

void mp_signal_curr_cpu_ready() {
  cpu_num_t num = arch_curr_cpu_num();
  DEBUG_ASSERT_MSG(mp_is_cpu_active(num), "CPU %u cannot be ready if it is not yet active", num);
  cpu_mask_t mask = cpu_num_to_mask(num);
  cpu_mask_t ready = ready_cpu_mask.fetch_or(mask) | mask;
  int ready_count = ktl::popcount(ready);
  int max_count = static_cast<int>(arch_max_num_cpus());
  DEBUG_ASSERT(ready_count <= max_count);
  if (ready_count == max_count) {
    ready_cpu_event.Signal();
  }
}

zx_status_t mp_wait_for_all_cpus_ready(Deadline deadline) { return ready_cpu_event.Wait(deadline); }

static void mp_all_cpu_startup_sync_hook(unsigned int rl) {
  // Before proceeding any further, wait for a _really_ long time to make sure
  // that all of the CPUs are ready.  We really don't want to start user-mode
  // until we have seen all of our CPUs start up.  In addition, there are
  // decisions to be made while setting up the VDSO which can only be made once
  // we have seen all CPUs start up and check-in.  Specifically, on ARM, we may
  // need to install a version of `zx_get_ticks` which is slower, but may be
  // needed to work around certain errata presented in only some revisions of
  // the CPU silicon (something which can only be determined by the core itself
  // as it comes up).
  //
  // If something goes wrong, do not panic.  Instead, just log a kernel OOPS and
  // keep going.  Things are bad, and we want to take notice in CI/CQ
  // environments, but not bad enough to panic the system and send it into a
  // boot-loop.
  constexpr zx_duration_t kCpuStartupTimeout = ZX_SEC(30);
  zx_status_t status = mp_wait_for_all_cpus_ready(Deadline::after(kCpuStartupTimeout));
  if (status != ZX_OK) {
    const auto online_mask = mp_get_online_mask();
    KERNEL_OOPS(
        "At least one CPU has not declared itself to be started after %ld ms.  "
        "(online mask = %08x)\n",
        kCpuStartupTimeout / ZX_MSEC(1), online_mask);

    // Also report a separate OOPS for any processor which is not yet in the
    // online mask.  Depending on where the core got wedged, it may be "online"
    // but has not managed to declare itself as started, or it might not have
    // even made it to the point where it declared itself to be online.
    for (auto* node : system_topology::GetSystemTopology().processors()) {
      const auto& processor = node->entity.processor;
      for (int i = 0; i < processor.logical_id_count; i++) {
        const cpu_num_t logical_id = node->entity.processor.logical_ids[i];
        if ((cpu_num_to_mask(logical_id) & online_mask) == 0) {
          KERNEL_OOPS("CPU %u is not online\n", logical_id);
        }
      }
    }
  }
}

// Before allowing the system to proceed to the USER init level, wait to be sure
// that all of the CPUs have started and made it to the check-in point (see
// above).
LK_INIT_HOOK(mp_all_cpu_startup_sync, mp_all_cpu_startup_sync_hook, LK_INIT_LEVEL_USER - 1)

static int cmd_mp(int argc, const cmd_args* argv, uint32_t flags) {
  if (argc < 2) {
    printf("not enough arguments\n");
  usage:
    printf("usage:\n");
    printf("%s unplug <cpu_id>\n", argv[0].str);
    printf("%s hotplug <cpu_id>\n", argv[0].str);
    printf("%s reschedule <cpu_id>        : send a reschedule ipi to <cpu_id>\n", argv[0].str);
    return ZX_ERR_INTERNAL;
  }

  if (!strcmp(argv[1].str, "unplug")) {
    if (argc < 3) {
      printf("specify a cpu_id\n");
      goto usage;
    }
    zx_status_t status = mp_unplug_cpu((cpu_num_t)argv[2].u);
    printf("CPU %lu unplug %s %d\n", argv[2].u, (status == ZX_OK ? "succeeded" : "failed"), status);
  } else if (!strcmp(argv[1].str, "hotplug")) {
    if (argc < 3) {
      printf("specify a cpu_id\n");
      goto usage;
    }
    zx_status_t status = mp_hotplug_cpu((cpu_num_t)argv[2].u);
    printf("CPU %lu hotplug %s %d\n", argv[2].u, (status == ZX_OK ? "succeeded" : "failed"),
           status);
  } else if (!strcmp(argv[1].str, "reschedule")) {
    if (argc < 3) {
      printf("specify a cpu_id\n");
      goto usage;
    }

    auto target_cpu = static_cast<cpu_num_t>(argv[2].u);
    if (!mp_is_cpu_active(target_cpu)) {
      printf("target cpu %u is not active\n", target_cpu);
      return ZX_OK;
    }

    cpu_mask_t mask = cpu_num_to_mask(target_cpu);
    cpu_num_t sending_cpu;
    {
      Guard<MonitoredSpinLock, IrqSave> thread_lock_guard{ThreadLock::Get(), SOURCE_TAG};
      sending_cpu = arch_curr_cpu_num();
      mp_reschedule(mask, 0);
    }

    if (sending_cpu == target_cpu) {
      printf("sending cpu is same as target cpu, no ipi sent\n");
    } else {
      printf("sent reschedule ipi to cpu %u\n", target_cpu);
    }
  } else {
    printf("unknown command\n");
    goto usage;
  }

  return ZX_OK;
}

STATIC_COMMAND_START
STATIC_COMMAND("mp", "mp test commands", &cmd_mp)
STATIC_COMMAND_END(mp)
