// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2008-2014 Travis Geiselbrecht
// Copyright (c) 2012 Shantanu Gupta
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_INCLUDE_KERNEL_MUTEX_H_
#define ZIRCON_KERNEL_INCLUDE_KERNEL_MUTEX_H_

#include <assert.h>
#include <debug.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <stdint.h>
#include <zircon/compiler.h>
#include <zircon/time.h>

#include <fbl/canary.h>
#include <fbl/macros.h>
#include <kernel/lock_validation_guard.h>
#include <kernel/lockdep.h>
#include <kernel/owned_wait_queue.h>
#include <kernel/thread.h>
#include <ktl/atomic.h>

// Kernel mutex support.
//
class TA_CAP("mutex") Mutex {
 public:
  constexpr Mutex() = default;
  ~Mutex();

  // No moving or copying allowed.
  DISALLOW_COPY_ASSIGN_AND_MOVE(Mutex);

  // The maximum duration to spin before falling back to blocking.
  // TODO(fxbug.dev/34646): Decide how to make this configurable per device/platform
  // and describe how to optimize this value.
  static constexpr zx_duration_t SPIN_MAX_DURATION = ZX_USEC(150);

  // A constant for users of AutoExpiringPreemptDisabler to select a value
  // consistent with the defaults for Mutex/CriticalMutex. Using this constant
  // also improves readablity, providing a link to the motivating use case for
  // timeslice extension.
  static constexpr zx_duration_t DEFAULT_TIMESLICE_EXTENSION = SPIN_MAX_DURATION;

  // Acquire the mutex.
  inline void Acquire(zx_duration_t spin_max_duration = SPIN_MAX_DURATION) TA_ACQ()
      TA_EXCL(thread_lock);

  // Release the mutex. Must be held by the current thread.
  void Release() TA_REL() TA_EXCL(thread_lock);

  // Special version of Release which operates with the thread lock held. Must
  // be called with preemption disabled.
  void ReleaseThreadLocked() TA_REL() TA_REQ(thread_lock);

  // does the current thread hold the mutex?
  bool IsHeld() const { return (holder() == Thread::Current::Get()); }

  // Panic unless the given lock is held.
  //
  // Can be used when thread safety analysis can't prove you are holding
  // a lock. The asserts may be optimized away in release builds.
  void AssertHeld() const TA_ASSERT() { DEBUG_ASSERT(IsHeld()); }

 protected:
  // TimesliceExtension is used to control whether a timeslice extension will be
  // set and if so, what value will be used.
  template <bool Enabled>
  struct TimesliceExtension;

  // Used by both Mutex and CriticalMutex.
  //
  // If TimesliceExtensionEnabled is true, attempt to set the timeslice
  // extension after acquiring the mutex and return true if the extension was
  // set.  Otherwise, return false.
  template <bool TimesliceExtensionEnabled>
  bool AcquireCommon(zx_duration_t spin_max_duration,
                     TimesliceExtension<TimesliceExtensionEnabled> timeslice_extension) TA_ACQ()
      TA_EXCL(thread_lock);

 private:
  // Attempts to release the mutex. Returns STATE_FREE if the mutex was
  // uncontested and released, otherwise returns the contested state of the
  // mutex.
  inline uintptr_t TryRelease(Thread* current_thread);

  // Acquire a lock held by another thread.
  //
  // This is a slowpath taken by |Acquire| if the mutex is found to already be held
  // by another thread.
  //
  // If TimesliceExtensionEnabled is true, attempt to set the timeslice
  // extension after acquiring the mutex and return true if the extension was
  // set.  Otherwise, return false.
  //
  // This function is deliberately moved out of line from |Acquire| to keep the stack
  // set up, tear down in the |Acquire| fastpath small.
  template <bool TimesliceExtensionEnabled>
  bool AcquireContendedMutex(zx_duration_t spin_max_duration, Thread* current_thread,
                             TimesliceExtension<TimesliceExtensionEnabled> timeslic_extension)
      TA_ACQ() TA_EXCL(thread_lock);

  // Release a lock contended by another thread.
  //
  // This is the slowpath taken by |Release| when releasing a lock that is being
  // waited for by another thread.
  //
  // This function is deliberately moved out of line from |Release| to keep the
  // stack set up, tear down in the |Release| fastpath small.
  void ReleaseContendedMutex(Thread* current_thread, uintptr_t old_mutex_state)
      TA_REQ(thread_lock, preempt_disabled_token);

  void RecordInitialAssignedCpu() {
    maybe_acquired_on_cpu_.store(arch_curr_cpu_num(), ktl::memory_order_relaxed);
  }

  void ClearInitialAssignedCpu() {
    maybe_acquired_on_cpu_.store(INVALID_CPU, ktl::memory_order_relaxed);
  }

  static constexpr uint32_t MAGIC = 0x6D757478;  // 'mutx'
  static constexpr uintptr_t STATE_FREE = 0u;
  static constexpr uintptr_t STATE_FLAG_CONTESTED = 1u;

  // Accessors to extract the holder pointer from the val member
  uintptr_t val() const { return val_.load(ktl::memory_order_relaxed); }

  static Thread* holder_from_val(uintptr_t value) {
    return reinterpret_cast<Thread*>(value & ~STATE_FLAG_CONTESTED);
  }

  Thread* holder() const { return holder_from_val(val()); }

  fbl::Canary<MAGIC> magic_;

  // See the comment near the start of AcquireContendedMutex for details about
  // this variable, and how it affects the spin phase behavior of
  // Mutex::AcquireContendedMutex
  ktl::atomic<cpu_num_t> maybe_acquired_on_cpu_{INVALID_CPU};
  ktl::atomic<uintptr_t> val_{STATE_FREE};
  OwnedWaitQueue wait_;
};

// TimeslicExtension specializations for Mutex::Acquire and
// Mutex::AcquireContendedMutex.
//
// No timeslice extension.
template <>
struct Mutex::TimesliceExtension<false> {};
// A timeslice extension of |value| nanoseconds.
template <>
struct Mutex::TimesliceExtension<true> {
  zx_duration_t value;
};

inline void Mutex::Acquire(zx_duration_t spin_max_duration) TA_ACQ() TA_EXCL(thread_lock) {
  AcquireCommon(spin_max_duration, TimesliceExtension<false>{});
}

// CriticalMutex is a mutex variant that uses a thread timeslice extension to
// disable preemption for a limited time during the critical section.
//
// Acquiring a CriticalMutex sets a timeslice extension that's cleared when the
// mutex is released or the extension expires, whichever comes first.
//
// This variant is useful to avoid a form of thrash where a thread holding a
// mutex is preempted by another thread that subsequently blocks while trying to
// acquire the already held mutex.  CriticalMutex is designed to mitigate this
// type of thrash by preventing the holder from being preempted, up to some
// maximum amount of time.  By limiting maximum amount of time the holder is
// non-preemptible, we can limit the degree to which other, logically higher
// priority tasks are delayed from executing.
//
// Good candidates for CriticalMutex are global or widely shared locks that
// typically, but not necessarily always, have very short critical sections
// (tens of microseconds or less) and high contention under load.
//
// CriticalMutex differs from SpinLock in the following ways:
// * Threads contending a CriticalMutex will block after the spin interval is
//   exceeded, avoiding extended monopolization of multiple CPUs.
// * Threads may block while holding a CriticalMutex, simplifying maintaining
//   invariants in slow paths.
// * Interrupts may remain enabled while holding a CriticalMutex, avoiding
//   undesirable IRQ latency.
//
class TA_CAP("mutex") CriticalMutex : private Mutex {
 public:
  CriticalMutex() = default;
  ~CriticalMutex() = default;

  CriticalMutex(const CriticalMutex&) = delete;
  CriticalMutex& operator=(const CriticalMutex&) = delete;
  CriticalMutex(CriticalMutex&&) = delete;
  CriticalMutex& operator=(CriticalMutex&&) = delete;

  // Acquire the mutex.
  void Acquire(zx_duration_t spin_max_duration = Mutex::SPIN_MAX_DURATION) TA_ACQ()
      TA_EXCL(thread_lock) {
    // TODO(maniscalco): What's the right duration here?  Is it a function of
    // spin_max_duration?
    const TimesliceExtension<true> timeslice_extension{spin_max_duration};
    should_clear_ = Mutex::AcquireCommon(spin_max_duration, timeslice_extension);
  }

  // Release the mutex. Must be held by the current thread.
  void Release() TA_REL() TA_EXCL(thread_lock) {
    // Make a copy because once we have released the mutex we can no longer
    // access should_clear_.
    const bool should_clear_copy = should_clear_;
    should_clear_ = false;

    Mutex::Release();

    if (should_clear_copy) {
      Thread::Current::preemption_state().ClearTimesliceExtension();
    }
  }

  // See |Mutex::ReleaseThreadLocked|.
  void ReleaseThreadLocked() TA_REL() TA_REQ(thread_lock) {
    // Make a copy because once we have released the mutex we can no longer
    // access should_clear_.
    const bool should_clear_copy = should_clear_;
    should_clear_ = false;

    Mutex::ReleaseThreadLocked();

    if (should_clear_copy) {
      Thread::Current::preemption_state().ClearTimesliceExtension();
    }
  }

  // See |Mutex::IsHeld|.
  bool IsHeld() const { return Mutex::IsHeld(); }

  // See |Mutex::AssertHeld|.
  void AssertHeld() const TA_ASSERT() { return Mutex::AssertHeld(); }

 private:
  // This field must not be accessed concurrently.  Be sure to only access it
  // after |Mutex::Acquire| has returned and before |Mutex::Release| is called.
  bool should_clear_{false};
};

// Lock policy for kernel mutexes
//
struct MutexPolicy {
  struct State {
    const zx_duration_t spin_max_duration{Mutex::SPIN_MAX_DURATION};
  };

  // Protects the thread local lock list and validation.
  using ValidationGuard = LockValidationGuard;

  // No special actions are needed during pre-validation.
  template <typename LockType>
  static void PreValidate(LockType*, State*) {}

  // Basic acquire and release operations.
  template <typename LockType>
  static bool Acquire(LockType* lock, State* state) TA_ACQ(lock) TA_EXCL(thread_lock) {
    lock->Acquire(state->spin_max_duration);
    return true;
  }

  template <typename LockType>
  static void Release(LockType* lock, State*) TA_REL(lock) TA_EXCL(thread_lock) {
    lock->Release();
  }

  // Runtime lock assertions.
  template <typename LockType>
  static void AssertHeld(const LockType& lock) TA_ASSERT(lock) {
    lock.AssertHeld();
  }

  // A enum tag that can be passed to Guard<Mutex>::Release(...) to
  // select the special-case release method below.
  enum SelectThreadLockHeld { ThreadLockHeld };

  // Releases the lock using the special mutex release operation. This
  // is selected by calling:
  //
  //  Guard<TrivialMutex|Mutex|Mutex>::Release(ThreadLockHeld)
  //
  template <typename LockType>
  static void Release(LockType* lock, State*, SelectThreadLockHeld) TA_REL(lock)
      TA_REQ(thread_lock) {
    lock->ReleaseThreadLocked();
  }
};

// Configure the lockdep::Guard for kernel mutexes to use MutexPolicy.
LOCK_DEP_POLICY(Mutex, MutexPolicy);
LOCK_DEP_POLICY(CriticalMutex, MutexPolicy);

// Declares a Mutex member of the struct or class |containing_type|.
//
// Example usage:
//
// struct MyType {
//     DECLARE_MUTEX(MyType [, LockFlags]) lock;
// };
//
#define DECLARE_MUTEX(containing_type, ...) \
  LOCK_DEP_INSTRUMENT(containing_type, ::Mutex, ##__VA_ARGS__)

#define DECLARE_CRITICAL_MUTEX(containing_type, ...) \
  LOCK_DEP_INSTRUMENT(containing_type, ::CriticalMutex, ##__VA_ARGS__)

// Declares a |lock_type| member of the struct or class |containing_type|.
//
// Example usage:
//
// struct MyType {
//     DECLARE_LOCK(MyType, LockType [, LockFlags]) lock;
// };
//
#define DECLARE_LOCK(containing_type, lock_type, ...) \
  LOCK_DEP_INSTRUMENT(containing_type, lock_type, ##__VA_ARGS__)

// By default, singleton mutexes in the kernel use Mutex in order to avoid
// a useless global dtor
//
// Example usage:
//
//  DECLARE_SINGLETON_MUTEX(MyGlobalLock [, LockFlags]);
//
#define DECLARE_SINGLETON_MUTEX(name, ...) LOCK_DEP_SINGLETON_LOCK(name, ::Mutex, ##__VA_ARGS__)

#define DECLARE_SINGLETON_CRITICAL_MUTEX(name, ...) \
  LOCK_DEP_SINGLETON_LOCK(name, ::CriticalMutex, ##__VA_ARGS__)

// Declares a singleton |lock_type| with the name |name|.
//
// Example usage:
//
//  DECLARE_SINGLETON_LOCK(MyGlobalLock, LockType, [, LockFlags]);
//
#define DECLARE_SINGLETON_LOCK(name, lock_type, ...) \
  LOCK_DEP_SINGLETON_LOCK(name, lock_type, ##__VA_ARGS__)

#endif  // ZIRCON_KERNEL_INCLUDE_KERNEL_MUTEX_H_
