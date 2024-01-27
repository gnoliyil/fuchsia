// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "kernel/dpc.h"

#include <assert.h>
#include <trace.h>
#include <zircon/errors.h>
#include <zircon/listnode.h>
#include <zircon/types.h>

#include <kernel/auto_preempt_disabler.h>
#include <kernel/event.h>
#include <kernel/lockdep.h>
#include <kernel/percpu.h>
#include <kernel/spinlock.h>
#include <lk/init.h>

#define DPC_THREAD_PRIORITY HIGH_PRIORITY

DECLARE_SINGLETON_SPINLOCK(dpc_lock);

zx_status_t Dpc::Queue() {
  DEBUG_ASSERT(func_);

  DpcQueue* dpc_queue = nullptr;
  {
    Guard<SpinLock, IrqSave> guard{dpc_lock::Get()};

    if (InContainer()) {
      return ZX_ERR_ALREADY_EXISTS;
    }

    dpc_queue = &percpu::GetCurrent().dpc_queue;

    // Put this Dpc at the tail of the list. Signal the worker outside the lock.
    dpc_queue->Enqueue(this);
  }

  // Signal outside of the lock to avoid lock order inversion with the thread
  // lock.
  dpc_queue->Signal();
  return ZX_OK;
}

zx_status_t Dpc::QueueThreadLocked() {
  DEBUG_ASSERT(func_);
  DEBUG_ASSERT(arch_ints_disabled());

  DpcQueue& dpc_queue = percpu::GetCurrent().dpc_queue;
  {
    Guard<SpinLock, NoIrqSave> guard{dpc_lock::Get()};

    if (InContainer()) {
      return ZX_ERR_ALREADY_EXISTS;
    }

    // Put this Dpc at the tail of the list and signal the worker.
    dpc_queue.Enqueue(this);
  }

  dpc_queue.SignalLocked();
  return ZX_OK;
}

void Dpc::Invoke() {
  if (func_)
    func_(this);
}

void DpcQueue::Enqueue(Dpc* dpc) { list_.push_back(dpc); }

void DpcQueue::Signal() { event_.Signal(); }
void DpcQueue::SignalLocked() { event_.SignalLocked(); }

zx_status_t DpcQueue::Shutdown(zx_time_t deadline) {
  Thread* t;
  Event* event;
  {
    Guard<SpinLock, IrqSave> guard{dpc_lock::Get()};

    // Ask the Dpc's thread to terminate.
    DEBUG_ASSERT(!stop_);
    stop_ = true;

    // Remember this Event so we can signal it outside the spinlock.
    event = &event_;

    // Remember the thread so we can join outside the spinlock.
    t = thread_;
    thread_ = nullptr;
  }

  // Wake the thread and wait for it to terminate.
  AutoPreemptDisabler preempt_disabled;
  event->Signal();
  return t->Join(nullptr, deadline);
}

void DpcQueue::TransitionOffCpu(DpcQueue& source) {
  Guard<SpinLock, IrqSave> guard{dpc_lock::Get()};

  // |source|'s cpu is shutting down. Assert that we are migrating to the current cpu.
  DEBUG_ASSERT(cpu_ == arch_curr_cpu_num());
  DEBUG_ASSERT(cpu_ != source.cpu_);

  // The Dpc's thread must already have been stopped by a call to |Shutdown|.
  DEBUG_ASSERT(source.stop_);
  DEBUG_ASSERT(source.thread_ == nullptr);

  // Move the contents of |source.list_| to the back of our |list_|.
  auto back = list_.end();
  list_.splice(back, source.list_);

  // Reset |source|'s state so we can restart Dpc processing if its cpu comes back online.
  source.event_.Unsignal();
  DEBUG_ASSERT(source.list_.is_empty());
  source.stop_ = false;
  source.initialized_ = false;
  source.cpu_ = INVALID_CPU;
}

int DpcQueue::WorkerThread(void* unused) { return percpu::GetCurrent().dpc_queue.Work(); }

int DpcQueue::Work() {
  for (;;) {
    // Wait for a Dpc to fire.
    [[maybe_unused]] zx_status_t err = event_.Wait();
    DEBUG_ASSERT(err == ZX_OK);

    Dpc dpc_local;
    {
      Guard<SpinLock, IrqSave> guard{dpc_lock::Get()};

      if (stop_) {
        return 0;
      }

      // Pop a Dpc off our list, and make a local copy.
      Dpc* dpc = list_.pop_front();

      // If our list is now empty, unsignal the event so we block until it is.
      if (!dpc) {
        event_.Unsignal();
        continue;
      }

      // Copy the Dpc to the stack.
      dpc_local = *dpc;
    }

    // Call the Dpc.
    dpc_local.Invoke();
  }

  return 0;
}

void DpcQueue::InitForCurrentCpu() {
  // This cpu's DpcQueue was initialized on a previous hotplug event.
  if (initialized_) {
    return;
  }

  DEBUG_ASSERT(cpu_ == INVALID_CPU);
  DEBUG_ASSERT(!stop_);
  DEBUG_ASSERT(thread_ == nullptr);

  cpu_ = arch_curr_cpu_num();

  initialized_ = true;
  stop_ = false;

  char name[10];
  snprintf(name, sizeof(name), "dpc-%u", cpu_);
  thread_ = Thread::Create(name, &DpcQueue::WorkerThread, nullptr, DPC_THREAD_PRIORITY);
  DEBUG_ASSERT(thread_ != nullptr);
  thread_->SetCpuAffinity(cpu_num_to_mask(cpu_));

  // The Dpc thread may use up to 150us out of every 300us (i.e. 50% of the CPU)
  // in the worst case. DPCs usually take only a small fraction of this and have
  // a much lower frequency than 3.333KHz.
  // TODO(fxbug.dev/38571): Make this runtime tunable. It may be necessary to change the
  // Dpc deadline params later in boot, after configuration is loaded somehow.
  thread_->SetBaseProfile(SchedulerState::BaseProfile{
      SchedDeadlineParams{SchedDuration{ZX_USEC(150)}, SchedDuration{ZX_USEC(300)}}});

  thread_->Resume();
}

static void dpc_init(unsigned int level) {
  // Initialize the DpcQueue for the main cpu.
  percpu::GetCurrent().dpc_queue.InitForCurrentCpu();
}

LK_INIT_HOOK(dpc, dpc_init, LK_INIT_LEVEL_THREADING)
