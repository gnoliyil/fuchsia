// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "kernel/brwlock.h"

#include <lib/affine/ratio.h>
#include <lib/affine/utils.h>
#include <lib/arch/intrin.h>
#include <lib/zircon-internal/macros.h>

#include <kernel/auto_preempt_disabler.h>
#include <kernel/lock_trace.h>
#include <kernel/task_runtime_timers.h>
#include <kernel/thread_lock.h>
#include <ktl/limits.h>

#include <ktl/enforce.h>

namespace internal {

template <BrwLockEnablePi PI>
BrwLock<PI>::~BrwLock() {
  DEBUG_ASSERT(state_.state_.load(ktl::memory_order_relaxed) == 0);
}

template <BrwLockEnablePi PI>
void BrwLock<PI>::Block(bool write) {
  zx_status_t ret;

  auto reason = write ? ResourceOwnership::Normal : ResourceOwnership::Reader;

  Thread* current_thread = Thread::Current::Get();
  const uint64_t flow_id = current_thread->TakeNextLockFlowId();
  LOCK_TRACE_FLOW_BEGIN("contend_rwlock", flow_id);

  if constexpr (PI == BrwLockEnablePi::Yes) {
    // Changes in ownership of node in a PI graph have the potential to affect
    // the running/runnable status of multiple threads at the same time.
    // Because of this, the OwnedWaitQueue class requires that we disable eager
    // rescheduling in order to optimize a situation where we might otherwise
    // send multiple (redundant) IPIs to the same CPUs during one PI graph
    // mutation.
    //
    // Note that this is an optimization, not a requirement.  What _is_ a
    // requirement is that we keep preemption disabled during the PI
    // propagation.  Currently, all of the invariants of OwnedWaitQueues and PI
    // graphs are protected by a single global "thread lock" which must be held
    // when a thread calls into the scheduler.  If a change to a PI graph would
    // cause the current scheduler to choose a different thread to run on that
    // CPU, however, the current thread will be preempted, and the ownership of
    // the thread lock will be transferred to the newly selected thread.  As the
    // new thread unwinds, it is going to drop the thread lock and return to
    // executing, leaving the first thread in the middle of what was supposed to
    // be an atomic operation.
    //
    // Because if this, it is critically important that local preemption be
    // disabled (at a minimum) when mutating a PI graph.  In the case that a
    // thread eventually blocks, the OWQ code will make sure that all invariants
    // will be restored before the thread finally blocks (and eventually wakes
    // and unwinds).
    AutoEagerReschedDisabler eager_resched_disabler;
    ret = wait_.BlockAndAssignOwner(Deadline::infinite(),
                                    state_.writer_.load(ktl::memory_order_relaxed), reason,
                                    Interruptible::No);
  } else {
    ret = wait_.BlockEtc(Deadline::infinite(), 0, reason, Interruptible::No);
  }

  LOCK_TRACE_FLOW_END("contend_rwlock", flow_id);

  if (unlikely(ret < ZX_OK)) {
    panic(
        "BrwLock<%d>::Block: Block returned with error %d lock %p, thr %p, "
        "sp %p\n",
        static_cast<bool>(PI), ret, this, Thread::Current::Get(), __GET_FRAME());
  }
}

template <BrwLockEnablePi PI>
ResourceOwnership BrwLock<PI>::Wake() {
  if constexpr (PI == BrwLockEnablePi::Yes) {
    using Action = OwnedWaitQueue::Hook::Action;
    struct Context {
      ResourceOwnership ownership;
      BrwLockState<PI>& state;
    };
    Context context = {ResourceOwnership::Normal, state_};
    auto cbk = [](Thread* woken, void* ctx) -> Action {
      Context* context = reinterpret_cast<Context*>(ctx);

      LOCK_TRACE_FLOW_STEP("contend_rwlock", woken->lock_flow_id());

      if (context->ownership == ResourceOwnership::Normal) {
        // Check if target is blocked for writing and not reading
        if (woken->state() == THREAD_BLOCKED) {
          context->state.writer_.store(woken, ktl::memory_order_relaxed);
          context->state.state_.fetch_add(-kBrwLockWaiter + kBrwLockWriter,
                                          ktl::memory_order_acq_rel);
          return Action::SelectAndAssignOwner;
        }
        // If not writing then we must be blocked for reading
        DEBUG_ASSERT(woken->state() == THREAD_BLOCKED_READ_LOCK);
        context->ownership = ResourceOwnership::Reader;
      }
      // Our current ownership is ResourceOwnership::Reader otherwise we would
      // have returned early
      DEBUG_ASSERT(context->ownership == ResourceOwnership::Reader);
      if (woken->state() == THREAD_BLOCKED_READ_LOCK) {
        // We are waking readers and we found a reader, so we can wake them up and
        // search for me.
        context->state.state_.fetch_add(-kBrwLockWaiter + kBrwLockReader,
                                        ktl::memory_order_acq_rel);
        return Action::SelectAndKeepGoing;
      } else {
        // We are waking readers but we have found a writer. To preserve fairness we
        // immediately stop and do not wake this thread or any others.
        return Action::Stop;
      }
    };

    wait_.WakeThreads(ktl::numeric_limits<uint32_t>::max(), {cbk, &context});
    return context.ownership;
  } else {
    zx_time_t now = current_time();
    Thread* next = wait_.Peek(now);
    DEBUG_ASSERT(next != NULL);
    if (next->state() == THREAD_BLOCKED_READ_LOCK) {
      while (!wait_.IsEmpty()) {
        next = wait_.Peek(now);
        if (next->state() != THREAD_BLOCKED_READ_LOCK) {
          break;
        }
        state_.state_.fetch_add(-kBrwLockWaiter + kBrwLockReader, ktl::memory_order_acq_rel);
        wait_.UnblockThread(next, ZX_OK);
      }
      return ResourceOwnership::Reader;
    } else {
      state_.state_.fetch_add(-kBrwLockWaiter + kBrwLockWriter, ktl::memory_order_acq_rel);
      wait_.UnblockThread(next, ZX_OK);
      return ResourceOwnership::Normal;
    }
  }
}

template <BrwLockEnablePi PI>
void BrwLock<PI>::ContendedReadAcquire() {
  LOCK_TRACE_DURATION("ContendedReadAcquire");

  // Remember the last call to current_ticks.
  zx_ticks_t now_ticks = current_ticks();
  Thread* current_thread = Thread::Current::Get();
  ContentionTimer timer(current_thread, now_ticks);

  const zx_duration_t spin_max_duration = Mutex::SPIN_MAX_DURATION;
  const affine::Ratio time_to_ticks = platform_get_ticks_to_time_ratio().Inverse();
  const zx_ticks_t spin_until_ticks =
      affine::utils::ClampAdd(now_ticks, time_to_ticks.Scale(spin_max_duration));

  do {
    const uint64_t state = state_.state_.load(ktl::memory_order_acquire);

    // If there are any waiters, implying another thread exhausted its spin phase on the same lock,
    // break out of the spin phase early.
    if (StateHasWaiters(state)) {
      break;
    }

    // If there are only readers now, return holding the lock for read, leaving the optimistic
    // reader count in place.
    if (!StateHasWriter(state)) {
      return;
    }

    // Give the arch a chance to relax the CPU.
    arch::Yield();
    now_ticks = current_ticks();
  } while (now_ticks < spin_until_ticks);

  // In the case where we wake other threads up we need them to not run until we're finished
  // holding the thread_lock, so disable local rescheduling.
  AnnotatedAutoPreemptDisabler preempt_disable;
  Guard<MonitoredSpinLock, IrqSave> guard{ThreadLock::Get(), SOURCE_TAG};

  // Remove our optimistic reader from the count, and put a waiter on there instead.
  uint64_t prev =
      state_.state_.fetch_add(-kBrwLockReader + kBrwLockWaiter, ktl::memory_order_relaxed);
  // If there is a writer then we just block, they will wake us up
  if (StateHasWriter(prev)) {
    Block(false);
    return;
  }
  // If we raced and there is in fact no one waiting then we can switch to
  // having the lock
  if (!StateHasWaiters(prev)) {
    state_.state_.fetch_add(-kBrwLockWaiter + kBrwLockReader, ktl::memory_order_acquire);
    return;
  }
  // If there are no current readers then we need to wake somebody up
  if (StateReaderCount(prev) == 1) {
    // See the comment in BrwLock<PI>::Block for why this eager reschedule
    // disabled is important.
    AutoEagerReschedDisabler eager_resched_disabler;
    if (Wake() == ResourceOwnership::Reader) {
      // Join the reader pool.
      state_.state_.fetch_add(-kBrwLockWaiter + kBrwLockReader, ktl::memory_order_acquire);
      return;
    }
  }

  Block(false);
}

template <BrwLockEnablePi PI>
void BrwLock<PI>::ContendedWriteAcquire() {
  LOCK_TRACE_DURATION("ContendedWriteAcquire");

  // Remember the last call to current_ticks.
  zx_ticks_t now_ticks = current_ticks();
  Thread* current_thread = Thread::Current::Get();
  ContentionTimer timer(current_thread, now_ticks);

  const zx_duration_t spin_max_duration = Mutex::SPIN_MAX_DURATION;
  const affine::Ratio time_to_ticks = platform_get_ticks_to_time_ratio().Inverse();
  const zx_ticks_t spin_until_ticks =
      affine::utils::ClampAdd(now_ticks, time_to_ticks.Scale(spin_max_duration));

  do {
    AcquireResult result = AtomicWriteAcquire(kBrwLockUnlocked, current_thread);

    // Acquire succeeded, return holding the lock.
    if (result) {
      return;
    }

    // If there are any waiters, implying another thread exhausted its spin phase on the same lock,
    // break out of the spin phase early.
    if (StateHasWaiters(result.state)) {
      break;
    }

    // Give the arch a chance to relax the CPU.
    arch::Yield();
    now_ticks = current_ticks();
  } while (now_ticks < spin_until_ticks);

  // In the case where we wake other threads up we need them to not run until we're finished
  // holding the thread_lock, so disable local rescheduling.
  AnnotatedAutoPreemptDisabler preempt_disable;
  Guard<MonitoredSpinLock, IrqSave> guard{ThreadLock::Get(), SOURCE_TAG};

  // Mark ourselves as waiting
  uint64_t prev = state_.state_.fetch_add(kBrwLockWaiter, ktl::memory_order_relaxed);
  // If there is a writer then we just block, they will wake us up
  if (StateHasWriter(prev)) {
    Block(true);
    return;
  }
  if (!StateHasReaders(prev)) {
    if (!StateHasWaiters(prev)) {
      if constexpr (PI == BrwLockEnablePi::Yes) {
        state_.writer_.store(Thread::Current::Get(), ktl::memory_order_relaxed);
      }
      // Must have raced previously as turns out there's no readers or
      // waiters, so we can convert to having the lock
      state_.state_.fetch_add(-kBrwLockWaiter + kBrwLockWriter, ktl::memory_order_acquire);
      return;
    } else {
      // There's no readers, but someone already waiting, wake up someone
      // before we ourselves block
      //
      // See the comment in BrwLock<PI>::Block for why this eager reschedule
      // disabled is important.
      AutoEagerReschedDisabler eager_resched_disabler;
      Wake();
    }
  }
  Block(true);
}

template <BrwLockEnablePi PI>
void BrwLock<PI>::WriteRelease() {
  canary_.Assert();

#if LK_DEBUGLEVEL > 0
  if constexpr (PI == BrwLockEnablePi::Yes) {
    Thread* holder = state_.writer_.load(ktl::memory_order_relaxed);
    Thread* ct = Thread::Current::Get();
    if (unlikely(ct != holder)) {
      panic(
          "BrwLock<PI>::WriteRelease: thread %p (%s) tried to release brwlock %p it "
          "doesn't "
          "own. Ownedby %p (%s)\n",
          ct, ct->name(), this, holder, holder ? holder->name() : "none");
    }
  }
#endif

  // For correct PI handling we need to ensure that up until a higher priority
  // thread can acquire the lock we will correctly be considered the owner.
  // Other threads are able to acquire the lock *after* we call ReleaseWakeup,
  // prior to that we could be racing with a higher priority acquirer and it
  // could be our responsibility to wake them up, and so up until ReleaseWakeup
  // is called they must be able to observe us as the owner.
  //
  // If we hold off on changing writer_ till after ReleaseWakeup we will then be
  // racing with others who may be acquiring, or be granted the write lock in
  // ReleaseWakeup, and so we would have to CAS writer_ to not clobber the new
  // holder. CAS is much more expensive than just a 'store', so to avoid that
  // we instead disable preemption. Disabling preemption effectively gives us the
  // highest priority, and so it is fine if acquirers observe writer_ to be null
  // and 'fail' to treat us as the owner.
  if constexpr (PI == BrwLockEnablePi::Yes) {
    Thread::Current::preemption_state().PreemptDisable();

    state_.writer_.store(nullptr, ktl::memory_order_relaxed);
  }
  uint64_t prev = state_.state_.fetch_sub(kBrwLockWriter, ktl::memory_order_release);

  if (unlikely(StateHasWaiters(prev))) {
    LOCK_TRACE_DURATION("ContendedWriteRelease");
    // There are waiters, we need to wake them up
    ReleaseWakeup();
  }

  if constexpr (PI == BrwLockEnablePi::Yes) {
    Thread::Current::preemption_state().PreemptReenable();
  }
}

template <BrwLockEnablePi PI>
void BrwLock<PI>::ReleaseWakeup() {
  // Don't reschedule whilst we're waking up all the threads as if there are
  // several readers available then we'd like to get them all out of the wait
  // queue.
  //
  // See the comment in BrwLock<PI>::Block for why this eager reschedule
  // disabled is important.
  AnnotatedAutoEagerReschedDisabler eager_resched_disabler;
  Guard<MonitoredSpinLock, IrqSave> guard{ThreadLock::Get(), SOURCE_TAG};

  uint64_t count = state_.state_.load(ktl::memory_order_relaxed);
  if (StateHasWaiters(count) && !StateHasWriter(count) && !StateHasReaders(count)) {
    Wake();
  }
}

template <BrwLockEnablePi PI>
void BrwLock<PI>::ContendedReadUpgrade() {
  LOCK_TRACE_DURATION("ContendedReadUpgrade");
  ContentionTimer timer(Thread::Current::Get(), current_ticks());

  AnnotatedAutoPreemptDisabler preempt_disable;
  Guard<MonitoredSpinLock, IrqSave> guard{ThreadLock::Get(), SOURCE_TAG};

  // Convert our reading into waiting
  uint64_t prev =
      state_.state_.fetch_add(-kBrwLockReader + kBrwLockWaiter, ktl::memory_order_relaxed);
  if (StateHasExclusiveReader(prev)) {
    if constexpr (PI == BrwLockEnablePi::Yes) {
      state_.writer_.store(Thread::Current::Get(), ktl::memory_order_relaxed);
    }
    // There are no writers or readers. There might be waiters, but as we
    // already have some form of lock we still have fairness even if we
    // bypass the queue, so we convert our waiting into writing
    state_.state_.fetch_add(-kBrwLockWaiter + kBrwLockWriter, ktl::memory_order_acquire);
  } else {
    Block(true);
  }
}

template class BrwLock<BrwLockEnablePi::Yes>;
template class BrwLock<BrwLockEnablePi::No>;

}  // namespace internal
