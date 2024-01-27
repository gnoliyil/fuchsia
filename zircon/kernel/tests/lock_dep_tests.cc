// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <lib/fit/defer.h>
#include <lib/unittest/unittest.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <stdint.h>
#include <zircon/time.h>

#include <kernel/lock_validation_guard.h>
#include <kernel/mp.h>
#include <kernel/mutex.h>
#include <kernel/spinlock.h>
#include <ktl/atomic.h>
#include <ktl/bit.h>
#include <lockdep/guard_multiple.h>
#include <lockdep/lockdep.h>

#include "tests.h"

#include <ktl/enforce.h>

#if WITH_LOCK_DEP_TESTS

// Defined in kernel/lib/lockdep.
zx_status_t TriggerAndWaitForLoopDetection(zx_time_t deadline);

namespace test {

// Global flag that determines whether try lock operations succeed.
bool g_try_lock_succeeds = true;

// Define some proxy types to simulate different kinds of locks.
struct Spinlock : Mutex {
  using Mutex::Mutex;

  bool TryAcquire() TA_TRY_ACQ(true) {
    if (g_try_lock_succeeds)
      Acquire();
    return g_try_lock_succeeds;
  }
  bool TryAcquireIrqSave(uint64_t* flags) TA_TRY_ACQ(true) {
    (void)flags;
    if (g_try_lock_succeeds)
      Acquire();
    return g_try_lock_succeeds;
  }
};
LOCK_DEP_TRAITS(Spinlock, lockdep::LockFlagsIrqSafe);

// Fake C-style locking primitive.
struct TA_CAP("mutex") spinlock_t {
  void Acquire() TA_ACQ() {}
  void Release() TA_REL() {}
  bool TryAcquire() TA_TRY_ACQ(true) { return g_try_lock_succeeds; }
};
LOCK_DEP_TRAITS(spinlock_t, lockdep::LockFlagsIrqSafe);

void spinlock_lock(spinlock_t* lock) TA_ACQ(lock) { lock->Acquire(); }
void spinlock_unlock(spinlock_t* lock) TA_REL(lock) { lock->Release(); }
bool spinlock_try_lock(spinlock_t* lock) TA_TRY_ACQ(true, lock) { return lock->TryAcquire(); }

// Type tags to select Guard<> lock policies for Spinlock and spinlock_t.
struct IrqSave {};
struct NoIrqSave {};
struct TryIrqSave {};
struct TryNoIrqSave {};

struct SpinlockNoIrqSave {
  struct State {};

  // Protects the thread local lock list and validation.
  using ValidationGuard = LockValidationGuard;

  static void PreValidate(Spinlock* lock, State*) {}
  static bool Acquire(Spinlock* lock, State*) TA_ACQ(lock) {
    lock->Acquire();
    return true;
  }

  static void Release(Spinlock* lock, State*) TA_REL(lock) { lock->Release(); }
};
LOCK_DEP_POLICY_OPTION(Spinlock, NoIrqSave, SpinlockNoIrqSave);

struct SpinlockIrqSave {
  struct State {};

  // Protects the thread local lock list and validation.
  using ValidationGuard = LockValidationGuard;

  static void PreValidate(Spinlock* lock, State*) {}
  static bool Acquire(Spinlock* lock, State*) TA_ACQ(lock) {
    lock->Acquire();
    return true;
  }

  static void Release(Spinlock* lock, State*) TA_REL(lock) { lock->Release(); }
};
LOCK_DEP_POLICY_OPTION(Spinlock, IrqSave, SpinlockIrqSave);

struct SpinlockTryNoIrqSave {
  struct State {};

  // Protects the thread local lock list and validation.
  using ValidationGuard = LockValidationGuard;

  static void PreValidate(Spinlock* lock, State*) {}
  static bool Acquire(Spinlock* lock, State*) TA_TRY_ACQ(true, lock) { return lock->TryAcquire(); }
  static void Release(Spinlock* lock, State*) TA_REL(lock) { lock->Release(); }
};
LOCK_DEP_POLICY_OPTION(Spinlock, TryNoIrqSave, SpinlockTryNoIrqSave);

struct SpinlockTryIrqSave {
  struct State {};

  // Protects the thread local lock list and validation.
  using ValidationGuard = LockValidationGuard;

  static void PreValidate(Spinlock* lock, State*) {}
  static bool Acquire(Spinlock* lock, State*) TA_TRY_ACQ(true, lock) { return lock->TryAcquire(); }
  static void Release(Spinlock* lock, State*) TA_REL(lock) { lock->Release(); }
};
LOCK_DEP_POLICY_OPTION(Spinlock, TryIrqSave, SpinlockTryIrqSave);

struct spinlock_t_NoIrqSave {
  struct State {};

  // Protects the thread local lock list and validation.
  using ValidationGuard = LockValidationGuard;

  static void PreValidate(spinlock_t* lock, State*) {}
  static bool Acquire(spinlock_t* lock, State*) TA_ACQ(lock) {
    spinlock_lock(lock);
    return true;
  }
  static void Release(spinlock_t* lock, State*) TA_REL(lock) { spinlock_unlock(lock); }
};
LOCK_DEP_POLICY_OPTION(spinlock_t, NoIrqSave, spinlock_t_NoIrqSave);

struct spinlock_t_IrqSave {
  struct State {};

  // Protects the thread local lock list and validation.
  using ValidationGuard = LockValidationGuard;

  static void PreValidate(spinlock_t* lock, State*) {}
  static bool Acquire(spinlock_t* lock, State*) TA_ACQ(lock) {
    spinlock_lock(lock);
    return true;
  }

  static void Release(spinlock_t* lock, State*) TA_REL(lock) { spinlock_unlock(lock); }
};
LOCK_DEP_POLICY_OPTION(spinlock_t, IrqSave, spinlock_t_IrqSave);

struct spinlock_t_TryNoIrqSave {
  struct State {};

  // Protects the thread local lock list and validation.
  using ValidationGuard = LockValidationGuard;

  static void PreValidate(spinlock_t* lock, State*) {}
  static bool Acquire(spinlock_t* lock, State*) TA_TRY_ACQ(true, lock) {
    return spinlock_try_lock(lock);
  }
  static void Release(spinlock_t* lock, State*) TA_REL(lock) { spinlock_unlock(lock); }
};
LOCK_DEP_POLICY_OPTION(spinlock_t, TryNoIrqSave, spinlock_t_TryNoIrqSave);

struct spinlock_t_TryIrqSave {
  struct State {};

  // Protects the thread local lock list and validation.
  using ValidationGuard = LockValidationGuard;

  static void PreValidate(spinlock_t* lock, State*) {}
  static bool Acquire(spinlock_t* lock, State*) TA_TRY_ACQ(true, lock) {
    return spinlock_try_lock(lock);
  }
  static void Release(spinlock_t* lock, State*) TA_REL(lock) { spinlock_unlock(lock); }
};
LOCK_DEP_POLICY_OPTION(spinlock_t, TryIrqSave, spinlock_t_TryIrqSave);

struct Mutex : ::Mutex {
  using ::Mutex::Mutex;
};
// Uses the default traits: fbl::LockClassState::None.

struct Nestable : ::Mutex {
  using ::Mutex::Mutex;
};
LOCK_DEP_TRAITS(Nestable, lockdep::LockFlagsNestable);

struct TA_CAP("mutex") ReadWriteLock {
  bool AcquireWrite() TA_ACQ() { return true; }
  bool AcquireRead() TA_ACQ_SHARED() { return true; }
  void Release() TA_REL() {}

  struct Read {
    struct State {};
    struct Shared {};
    // Protects the thread local lock list and validation.
    using ValidationGuard = LockValidationGuard;
    static void PreValidate(ReadWriteLock* lock, State*) {}
    static bool Acquire(ReadWriteLock* lock, State*) TA_ACQ_SHARED(lock) {
      return lock->AcquireRead();
    }
    static void Release(ReadWriteLock* lock, State*) TA_REL(lock) { lock->Release(); }
  };

  struct Write {
    struct State {};
    // Protects the thread local lock list and validation.
    using ValidationGuard = LockValidationGuard;
    static void PreValidate(ReadWriteLock* lock, State*) {}
    static bool Acquire(ReadWriteLock* lock, State*) TA_ACQ(lock) { return lock->AcquireWrite(); }
    static void Release(ReadWriteLock* lock, State*) TA_REL(lock) { lock->Release(); }
  };
};
LOCK_DEP_POLICY_OPTION(ReadWriteLock, ReadWriteLock::Read, ReadWriteLock::Read);
LOCK_DEP_POLICY_OPTION(ReadWriteLock, ReadWriteLock::Write, ReadWriteLock::Write);

struct Foo {
  LOCK_DEP_INSTRUMENT(Foo, Mutex) lock;

  void TestRequire() TA_REQ(lock) {}
  void TestExclude() TA_EXCL(lock) {}
};

struct Bar {
  LOCK_DEP_INSTRUMENT(Bar, Mutex) lock;

  void TestRequire() TA_REQ(lock) {}
  void TestExclude() TA_EXCL(lock) {}
};

template <typename LockType, lockdep::LockFlags Flags = lockdep::LockFlagsNone>
struct Baz {
  LOCK_DEP_INSTRUMENT(Baz, LockType, Flags) lock;

  lockdep::Lock<LockType>* get_lock() TA_RET_CAP(lock) { return &lock; }

  void TestRequire() TA_REQ(lock) {}
  void TestExclude() TA_EXCL(lock) {}
  void TestShared() TA_REQ_SHARED(lock) {}
};

struct MultipleLocks {
  LOCK_DEP_INSTRUMENT(MultipleLocks, Mutex) lock_a;
  LOCK_DEP_INSTRUMENT(MultipleLocks, Mutex) lock_b;

  void TestRequireLockA() TA_REQ(lock_a) {}
  void TestExcludeLockA() TA_EXCL(lock_a) {}
  void TestRequireLockB() TA_REQ(lock_b) {}
  void TestExcludeLockB() TA_EXCL(lock_b) {}
};

template <size_t Index>
struct Number {
  LOCK_DEP_INSTRUMENT(Number, Mutex) lock;

  void TestRequire() TA_REQ(lock) {}
  void TestExclude() TA_EXCL(lock) {}
};

template <typename GuardType>
lockdep::LockResult GetLastResult(const GuardType&) {
#if WITH_LOCK_DEP
  lockdep::ThreadLockState* state = lockdep::ThreadLockState::Get(GuardType::kLockFlags);
  return state->last_result();
#else
  return lockdep::LockResult::Success;
#endif
}

void ResetTrackingState() {
#if WITH_LOCK_DEP
  for (auto& state : lockdep::LockClassState::Iter())
    state.Reset();
#endif
}

}  // namespace test

static bool lock_dep_dynamic_analysis_tests() {
  BEGIN_TEST;

  using lockdep::AssertNoLocksHeld;
  using lockdep::AssertOrderedLock;
  using lockdep::Guard;
  using lockdep::GuardMultiple;
  using lockdep::kInvalidLockClassId;
  using lockdep::LockClassState;
  using lockdep::LockFlagsLeaf;
  using lockdep::LockFlagsMultiAcquire;
  using lockdep::LockFlagsNestable;
  using lockdep::LockFlagsReAcquireFatal;
  using lockdep::LockResult;
  using lockdep::ThreadLockState;
  using test::Bar;
  using test::Baz;
  using test::Foo;
  using test::GetLastResult;
  using test::IrqSave;
  using test::MultipleLocks;
  using test::Mutex;
  using test::Nestable;
  using test::NoIrqSave;
  using test::Number;
  using test::ReadWriteLock;
  using test::Spinlock;
  using test::spinlock_t;
  using test::TryIrqSave;
  using test::TryNoIrqSave;

  // Reset the tracking state before each test run.
  test::ResetTrackingState();

  // Initially no locks should be held.
  AssertNoLocksHeld();

  // Single lock.
  {
    Foo a{};

    Guard<Mutex> guard_a{&a.lock};
    EXPECT_TRUE(guard_a);
    EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));
  }

  // Single lock.
  {
    Bar a{};

    Guard<Mutex> guard_a{&a.lock};
    EXPECT_TRUE(guard_a);
    EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));
  }

  // Test order invariant.
  {
    Foo a{};
    Foo b{};

    Guard<Mutex> guard_a{&a.lock};
    EXPECT_TRUE(guard_a);
    EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

    Guard<Mutex> guard_b{&b.lock};
    EXPECT_TRUE(guard_b);
    EXPECT_EQ(LockResult::AlreadyAcquired, test::GetLastResult(guard_b));
  }

  // Test order invariant with a different lock class.
  {
    Bar a{};
    Bar b{};

    Guard<Mutex> guard_a{&a.lock};
    EXPECT_TRUE(guard_a);
    EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

    Guard<Mutex> guard_b{&b.lock};
    EXPECT_TRUE(guard_b);
    EXPECT_EQ(LockResult::AlreadyAcquired, test::GetLastResult(guard_b));
  }

  // Test address order invariant.
  {
    Foo a{};
    Foo b{};

    {
      GuardMultiple<2, Mutex> guard_all{&a.lock, &b.lock};
      EXPECT_TRUE(guard_all);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_all));
    }

    {
      GuardMultiple<2, Mutex> guard_all{&b.lock, &a.lock};
      EXPECT_TRUE(guard_all);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_all));
    }
  }

  // Test address order invariant with a different lock class.
  {
    Bar a{};
    Bar b{};

    {
      GuardMultiple<2, Mutex> guard_all{&a.lock, &b.lock};
      EXPECT_TRUE(guard_all);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_all));
    }

    {
      GuardMultiple<2, Mutex> guard_all{&b.lock, &a.lock};
      EXPECT_TRUE(guard_all);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_all));
    }
  }

  // Test address order invariant with spinlocks.
  {
    Baz<Spinlock> a{};
    Baz<Spinlock> b{};

    {
      GuardMultiple<2, Spinlock, NoIrqSave> guard_all{&a.lock, &b.lock};
      EXPECT_TRUE(guard_all);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_all));
    }

    {
      GuardMultiple<2, Spinlock, NoIrqSave> guard_all{&b.lock, &a.lock};
      EXPECT_TRUE(guard_all);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_all));
    }

    {
      test::g_try_lock_succeeds = true;
      GuardMultiple<2, Spinlock, TryNoIrqSave> guard_all{&a.lock, &b.lock};
      EXPECT_TRUE(guard_all);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_all));
    }

    {
      test::g_try_lock_succeeds = true;
      GuardMultiple<2, Spinlock, TryNoIrqSave> guard_all{&b.lock, &a.lock};
      EXPECT_TRUE(guard_all);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_all));
    }

    {
      test::g_try_lock_succeeds = false;
      GuardMultiple<2, Spinlock, TryNoIrqSave> guard_all{&a.lock, &b.lock};
      EXPECT_FALSE(guard_all);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_all));
    }

    {
      test::g_try_lock_succeeds = false;
      GuardMultiple<2, Spinlock, TryNoIrqSave> guard_all{&b.lock, &a.lock};
      EXPECT_FALSE(guard_all);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_all));
    }
  }

  // Foo -> Bar -- establish order.
  {
    Foo a{};
    Bar b{};

    Guard<Mutex> guard_a{&a.lock};
    EXPECT_TRUE(guard_a);
    EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

    Guard<Mutex> guard_b{&b.lock};
    EXPECT_TRUE(guard_b);
    EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_b));
  }

  // Bar -> Foo -- check order invariant.
  {
    Foo a{};
    Bar b{};

    Guard<Mutex> guard_b{&b.lock};
    EXPECT_TRUE(guard_b);
    EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_b));

    Guard<Mutex> guard_a{&a.lock};
    EXPECT_TRUE(guard_a);
    EXPECT_EQ(LockResult::OutOfOrder, test::GetLastResult(guard_a));
  }

  // Test external order invariant.
  {
    Baz<Nestable> baz1;
    Baz<Nestable> baz2;

    {
      Guard<Nestable> auto_baz1{&baz1.lock, 0};
      EXPECT_TRUE(auto_baz1);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz1));

      Guard<Nestable> auto_baz2{&baz2.lock, 1};
      EXPECT_TRUE(auto_baz2);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz2));
    }

    {
      Guard<Nestable> auto_baz2{&baz2.lock, 0};
      EXPECT_TRUE(auto_baz2);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz2));

      Guard<Nestable> auto_baz1{&baz1.lock, 1};
      EXPECT_TRUE(auto_baz1);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz2));
    }

    {
      Guard<Nestable> auto_baz2{&baz2.lock, 1};
      EXPECT_TRUE(auto_baz2);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz2));

      Guard<Nestable> auto_baz1{&baz1.lock, 0};
      EXPECT_TRUE(auto_baz1);
      EXPECT_EQ(LockResult::InvalidNesting, test::GetLastResult(auto_baz1));
    }
  }

  // Test external order invariant with nestable flag supplied on the lock
  // member, rather than the lock type.
  {
    Baz<Mutex, LockFlagsNestable> baz1;
    Baz<Mutex, LockFlagsNestable> baz2;

    {
      Guard<Mutex> auto_baz1{&baz1.lock, 0};
      EXPECT_TRUE(auto_baz1);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz1));

      Guard<Mutex> auto_baz2{&baz2.lock, 1};
      EXPECT_TRUE(auto_baz2);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz2));
    }

    {
      Guard<Mutex> auto_baz2{&baz2.lock, 0};
      EXPECT_TRUE(auto_baz2);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz2));

      Guard<Mutex> auto_baz1{&baz1.lock, 1};
      EXPECT_TRUE(auto_baz1);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz1));
    }

    {
      Guard<Mutex> auto_baz2{&baz2.lock, 1};
      EXPECT_TRUE(auto_baz2);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz2));

      Guard<Mutex> auto_baz1{&baz1.lock, 0};
      EXPECT_TRUE(auto_baz1);
      EXPECT_EQ(LockResult::InvalidNesting, test::GetLastResult(auto_baz1));
    }
  }

  // Test external order invariant with nestable flag supplied on the lock
  // member, rather than the lock type, using the type-erased lock type.
  {
    Baz<Mutex, LockFlagsNestable> baz1;
    Baz<Mutex, LockFlagsNestable> baz2;

    {
      Guard<Mutex> auto_baz1{AssertOrderedLock, baz1.get_lock(), 0};
      EXPECT_TRUE(auto_baz1);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz1));

      Guard<Mutex> auto_baz2{AssertOrderedLock, baz2.get_lock(), 1};
      EXPECT_TRUE(auto_baz2);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz2));
    }

    {
      Guard<Mutex> auto_baz2{AssertOrderedLock, baz2.get_lock(), 0};
      EXPECT_TRUE(auto_baz2);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz2));

      Guard<Mutex> auto_baz1{AssertOrderedLock, baz1.get_lock(), 1};
      EXPECT_TRUE(auto_baz1);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz1));
    }

    {
      Guard<Mutex> auto_baz2{AssertOrderedLock, baz2.get_lock(), 1};
      EXPECT_TRUE(auto_baz2);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz2));

      Guard<Mutex> auto_baz1{AssertOrderedLock, baz1.get_lock(), 0};
      EXPECT_TRUE(auto_baz1);
      EXPECT_EQ(LockResult::InvalidNesting, test::GetLastResult(auto_baz1));
    }
  }

  // Test using a non-nestable lock with the AssertOrderedLock tag.
  // TODO(33187): Enable the userspace death test when lockdep has a userspace
  // runtime and validation can be tested in userspace.
  {
#if TEST_WILL_DEBUG_ASSERT || 0
    Baz<Mutex> baz3;
    Guard<Mutex> auto_baz3{AssertOrderedLock, baz3.get_lock(), 0};
#endif
  }

  // Test that asserting no locks held while holding a log will fail.
  // TODO(338187): Enable the userspace death test when lockdep has a userspace
  // runtime and validation can be tested in userspace.
  {
#if TEST_WILL_DEBUG_ASSERT || 0
    Baz<Mutex> baz;
    Guard<Mutex> auto_baz{&baz.lock};
    AssertNoLocksHeld();
#endif
  }

  // Test that acquiring a lock after a leaf lock will fail.
  // TODO(338187): Enable the userspace death test when lockdep has a userspace
  // runtime and validation can be tested in userspace.
  {
#if TEST_WILL_PANIC || 0
    Baz<Mutex, LockFlagsLeaf> baz_leaf_lock;
    Baz<Mutex> baz;
    Guard<Mutex> leaf_guard{&baz_leaf_lock.lock};
    Guard<Mutex> guard{&baz.lock};
#endif
  }

  // Test irq-safety invariant.
  {
    Baz<Mutex> baz1;
    Baz<Spinlock> baz2;

    {
      Guard<Mutex> auto_baz1{&baz1.lock};
      EXPECT_TRUE(auto_baz1);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz1));

      Guard<Spinlock, NoIrqSave> auto_baz2{&baz2.lock};
      EXPECT_TRUE(auto_baz2);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz2));
    }

    // Note that this ordering (spinlock followed by mutex) cannot currently be
    // detected by lockdep in the kernel.  See fxb/85289 and the discussion of
    // the solution for details.  The TL;DR is that spinlocks and mutexes end up
    // using different system provided contexts for their analysis, and rely on
    // asserts in the lock implementations themselves to provide protection
    // against either attempting to acquire a spinlock with IRQs still enabled,
    // or attempting to acquire a blocking mutex while holding a spinlock.
#if 0
    {
      Guard<Spinlock, NoIrqSave> auto_baz2{&baz2.lock};
      EXPECT_TRUE(auto_baz2);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(auto_baz2));

      Guard<Mutex> auto_baz1{&baz1.lock};
      EXPECT_TRUE(auto_baz1);
      EXPECT_EQ(LockResult::InvalidIrqSafety, test::GetLastResult(auto_baz1));
    }
#endif
  }

  // Test spinlock options compile and basic guard functions.
  // TODO(eieio): Add Guard<>::state() accessor and check state values.
  {
    Baz<Spinlock> baz1;
    Baz<spinlock_t> baz2;

    {
      Guard<Spinlock, NoIrqSave> guard{&baz1.lock};
      EXPECT_TRUE(guard);
      guard.Release();
      EXPECT_FALSE(guard);
    }

    {
      Guard<Spinlock, IrqSave> guard{&baz1.lock};
      EXPECT_TRUE(guard);
      guard.Release();
      EXPECT_FALSE(guard);
    }

    {
      Guard<spinlock_t, NoIrqSave> guard{&baz2.lock};
      EXPECT_TRUE(guard);
      guard.Release();
      EXPECT_FALSE(guard);
    }

    {
      Guard<spinlock_t, IrqSave> guard{&baz2.lock};
      EXPECT_TRUE(guard);
      guard.Release();
      EXPECT_FALSE(guard);
    }

    {
      test::g_try_lock_succeeds = true;
      Guard<Spinlock, TryNoIrqSave> guard{&baz1.lock};
      EXPECT_TRUE(guard);
      guard.Release();
      EXPECT_FALSE(guard);
    }

    {
      test::g_try_lock_succeeds = true;
      Guard<Spinlock, TryIrqSave> guard{&baz1.lock};
      EXPECT_TRUE(guard);
      guard.Release();
      EXPECT_FALSE(guard);
    }

    {
      test::g_try_lock_succeeds = false;
      Guard<spinlock_t, TryNoIrqSave> guard{&baz2.lock};
      EXPECT_FALSE(guard);
      guard.Release();
      EXPECT_FALSE(guard);
    }

    {
      test::g_try_lock_succeeds = false;
      Guard<spinlock_t, TryIrqSave> guard{&baz2.lock};
      EXPECT_FALSE(guard);
      guard.Release();
      EXPECT_FALSE(guard);
    }

    // Test that Guard<LockType, Option> fails to compile when Option is
    // required by the policy config but not specified.
    {
#if TEST_WILL_NOT_COMPILE || 0
      Guard<Spinlock> guard1{&baz1.lock};
      Guard<spinlock_t> guard2{&baz2.lock};
#endif
    }
  }

  // Test read/write lock compiles and basic guard options.
  {
    Baz<ReadWriteLock> a{};
    Baz<ReadWriteLock> b{};

    {
      Guard<ReadWriteLock, ReadWriteLock::Read> guard{&a.lock};
      EXPECT_TRUE(guard);
      guard.Release();
      EXPECT_FALSE(guard);
    }

    {
      Guard<ReadWriteLock, ReadWriteLock::Read> guard{&b.lock};
      EXPECT_TRUE(guard);
      guard.Release();
      EXPECT_FALSE(guard);
    }

    {
      Guard<ReadWriteLock, ReadWriteLock::Write> guard{&a.lock};
      EXPECT_TRUE(guard);
      guard.Release();
      EXPECT_FALSE(guard);
    }

    {
      Guard<ReadWriteLock, ReadWriteLock::Write> guard{&b.lock};
      EXPECT_TRUE(guard);
      guard.Release();
      EXPECT_FALSE(guard);
    }
  }

  // Test read/write lock order invariants.
  {
    Baz<ReadWriteLock> a{};
    Baz<ReadWriteLock> b{};

    {
      Guard<ReadWriteLock, ReadWriteLock::Read> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      Guard<ReadWriteLock, ReadWriteLock::Read> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::AlreadyAcquired, test::GetLastResult(guard_b));
    }

    {
      Guard<ReadWriteLock, ReadWriteLock::Read> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      [&]() TA_NO_THREAD_SAFETY_ANALYSIS {
        Guard<ReadWriteLock, ReadWriteLock::Read> guard_b{&a.lock};
        EXPECT_TRUE(guard_b);
        EXPECT_EQ(LockResult::Reentrance, test::GetLastResult(guard_b));
        return true;
      }();
    }

    {
      Guard<ReadWriteLock, ReadWriteLock::Read> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      Guard<ReadWriteLock, ReadWriteLock::Write> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::AlreadyAcquired, test::GetLastResult(guard_b));
    }

    {
      Guard<ReadWriteLock, ReadWriteLock::Read> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      [&]() TA_NO_THREAD_SAFETY_ANALYSIS {
        Guard<ReadWriteLock, ReadWriteLock::Write> guard_b{&a.lock};
        EXPECT_TRUE(guard_b);
        EXPECT_EQ(LockResult::Reentrance, test::GetLastResult(guard_b));
        return true;
      }();
    }

    {
      Guard<ReadWriteLock, ReadWriteLock::Write> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      Guard<ReadWriteLock, ReadWriteLock::Read> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::AlreadyAcquired, test::GetLastResult(guard_b));
    }

    {
      Guard<ReadWriteLock, ReadWriteLock::Write> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      [&]() TA_NO_THREAD_SAFETY_ANALYSIS {
        Guard<ReadWriteLock, ReadWriteLock::Read> guard_b{&a.lock};
        EXPECT_TRUE(guard_b);
        EXPECT_EQ(LockResult::Reentrance, test::GetLastResult(guard_b));
        return true;
      }();
    }

    {
      Guard<ReadWriteLock, ReadWriteLock::Write> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      Guard<ReadWriteLock, ReadWriteLock::Write> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::AlreadyAcquired, test::GetLastResult(guard_b));
    }

    {
      Guard<ReadWriteLock, ReadWriteLock::Write> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      [&]() TA_NO_THREAD_SAFETY_ANALYSIS {
        Guard<ReadWriteLock, ReadWriteLock::Write> guard_b{&a.lock};
        EXPECT_TRUE(guard_b);
        EXPECT_EQ(LockResult::Reentrance, test::GetLastResult(guard_b));
        return true;
      }();
    }

    {
      GuardMultiple<2, ReadWriteLock, ReadWriteLock::Read> guard{&a.lock, &b.lock};
      EXPECT_TRUE(guard);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard));
    }

    {
      GuardMultiple<2, ReadWriteLock, ReadWriteLock::Read> guard{&a.lock, &a.lock};
      EXPECT_TRUE(guard);
      EXPECT_EQ(LockResult::Reentrance, test::GetLastResult(guard));
    }

    {
      GuardMultiple<2, ReadWriteLock, ReadWriteLock::Write> guard{&a.lock, &b.lock};
      EXPECT_TRUE(guard);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard));
    }

    {
      GuardMultiple<2, ReadWriteLock, ReadWriteLock::Write> guard{&a.lock, &a.lock};
      EXPECT_TRUE(guard);
      EXPECT_EQ(LockResult::Reentrance, test::GetLastResult(guard));
    }
  }

  // Test that each lock in a structure behaves as an individual lock class.
  {
    MultipleLocks value{};

    {
      Guard<Mutex> guard_a{&value.lock_a};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      Guard<Mutex> guard_b{&value.lock_b};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_b));
    }

    {
      Guard<Mutex> guard_b{&value.lock_b};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_b));

      Guard<Mutex> guard_a{&value.lock_a};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::OutOfOrder, test::GetLastResult(guard_a));
    }
  }

  // Test multi-acquire rule. Re-acquiring the same lock class is allowed,
  // however, ordering with other locks is still enforced.
  {
    Baz<Mutex, LockFlagsMultiAcquire> a;
    Baz<Mutex, LockFlagsMultiAcquire> b;
#if 0 || TEST_WILL_NOT_COMPILE
    // Test mutually exclusive flags fail to compile.
    Baz<Mutex, LockFlagsMultiAcquire | LockFlagsNestable> c;
    Baz<Mutex, LockFlagsMultiAcquire | LockFlagsReAcquireFatal> d;
#endif

    // Use a unique lock class for each of these order tests.
    Foo before;
    Bar after;
    Baz<Mutex> between;

    // Test re-acquiring the same lock class.
    {
      Guard<Mutex> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      Guard<Mutex> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_b));
    }

    // Test ordering with another lock class before this one.
    {
      Guard<Mutex> guard_before{&before.lock};
      EXPECT_TRUE(guard_before);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_before));

      Guard<Mutex> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      Guard<Mutex> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_b));
    }
    {
      Guard<Mutex> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      Guard<Mutex> guard_before{&before.lock};
      EXPECT_TRUE(guard_before);
      EXPECT_EQ(LockResult::OutOfOrder, test::GetLastResult(guard_before));
    }
    {
      Guard<Mutex> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_b));

      // Subsequent violations are not reported.
      Guard<Mutex> guard_before{&before.lock};
      EXPECT_TRUE(guard_before);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_before));
    }

    // Test ordering with another lock class after this one.
    {
      Guard<Mutex> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      Guard<Mutex> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_b));

      Guard<Mutex> guard_after{&after.lock};
      EXPECT_TRUE(guard_after);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_after));
    }
    {
      Guard<Mutex> guard_after{&after.lock};
      EXPECT_TRUE(guard_after);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_after));

      Guard<Mutex> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::OutOfOrder, test::GetLastResult(guard_a));
    }
    {
      Guard<Mutex> guard_after{&after.lock};
      EXPECT_TRUE(guard_after);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_after));

      // Subsequent violations are not reported.
      Guard<Mutex> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_b));
    }

    // Test ordering with another lock class between this one.
    {
      Guard<Mutex> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      Guard<Mutex> guard_between{&between.lock};
      EXPECT_TRUE(guard_between);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_between));

      Guard<Mutex> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::OutOfOrder, test::GetLastResult(guard_b));
    }
    {
      Guard<Mutex> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_b));

      Guard<Mutex> guard_between{&between.lock};
      EXPECT_TRUE(guard_between);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_between));

      // Subsequent violations are not reported.
      Guard<Mutex> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));
    }
  }

  // Test circular dependency detection.
  {
    Number<1> a{};  // Node A.
    Number<2> b{};  // Node B.
    Number<3> c{};  // Node C.
    Number<4> d{};  // Node D.

    // A -> B
    {
      Guard<Mutex> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));

      Guard<Mutex> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_b));
    }

    // B -> C
    {
      Guard<Mutex> guard_b{&b.lock};
      EXPECT_TRUE(guard_b);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_b));

      Guard<Mutex> guard_c{&c.lock};
      EXPECT_TRUE(guard_c);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_c));
    }

    // C -> A -- cycle in (A, B, C)
    {
      Guard<Mutex> guard_c{&c.lock};
      EXPECT_TRUE(guard_c);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_c));

      Guard<Mutex> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));
    }

    // C -> D
    {
      Guard<Mutex> guard_c{&c.lock};
      EXPECT_TRUE(guard_c);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_c));

      Guard<Mutex> guard_d{&d.lock};
      EXPECT_TRUE(guard_d);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_d));
    }

    // D -> A -- cycle in (A, B, C, D)
    {
      Guard<Mutex> guard_d{&d.lock};
      EXPECT_TRUE(guard_d);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_d));

      Guard<Mutex> guard_a{&a.lock};
      EXPECT_TRUE(guard_a);
      EXPECT_EQ(LockResult::Success, test::GetLastResult(guard_a));
    }

    // Ensure that the loop detection pass completes before the test ends to
    // avoid triggering lockdep failures in CQ/CI. Use an infinite timeout and
    // let test infra kill the test due to timeout instead.
    zx_status_t status = TriggerAndWaitForLoopDetection(ZX_TIME_INFINITE);
    EXPECT_EQ(ZX_OK, status);
  }

  // Test CallUntracked operation.
  {
    Baz<Mutex> a, b;

    // A -> B
    {
      Guard<Mutex> guard_a{&a.lock};
      Guard<Mutex> guard_b{&b.lock};
    }
    // Now acquire B -> A using untracked to make it work.
    {
      Guard<Mutex> guard_b{&b.lock};
      guard_b.CallUntracked([&] {
        // B should still be held.
        ASSERT(guard_b);
        Guard<Mutex> guard_a{&a.lock};
      });
    }
    // Should be able to AssertNoLocks held as well.
    {
      Guard<Mutex> guard_a{&a.lock};
      guard_a.CallUntracked([&] {
        ASSERT(guard_a);
        AssertNoLocksHeld();
      });
    }
  }

  // With all locks released, regardless of what we did in the middle, should still detect no locks
  // held.
  AssertNoLocksHeld();

  // Reset the tracking state to ensure that circular dependencies are not
  // reported outside of the test.
  test::ResetTrackingState();

  END_TEST;
}

// Basic compile-time tests of lockdep clang lock annotations.
static bool lock_dep_static_analysis_tests() {
  BEGIN_TEST;

  using lockdep::Guard;
  using lockdep::GuardMultiple;
  using lockdep::LockResult;
  using lockdep::ThreadLockState;
  using test::Bar;
  using test::Baz;
  using test::Foo;
  using test::MultipleLocks;
  using test::Mutex;
  using test::Nestable;
  using test::Number;
  using test::ReadWriteLock;
  using test::Spinlock;
  using test::TryNoIrqSave;

  // Test require and exclude annotations.
  {
    Foo a{};

    Guard<Mutex> guard_a{&a.lock};
    a.TestRequire();
#if TEST_WILL_NOT_COMPILE || 0
    a.TestExclude();
#endif

    guard_a.Release();
#if TEST_WILL_NOT_COMPILE || 0
    a.TestRequire();
#endif
    a.TestExclude();
  }

  // Test multiple acquire.
  {
    Foo a{};

    Guard<Mutex> guard_a{&a.lock};
#if TEST_WILL_NOT_COMPILE || 0
    Guard<Mutex> guard_b{&a.lock};
#endif
  }

  // Test sequential acquire/release.
  {
    Foo a{};

    Guard<Mutex> guard_a{&a.lock};
    guard_a.Release();
    Guard<Mutex> guard_b{&a.lock};
  }

  // Test shared.
  {
    Baz<ReadWriteLock> a{};

    Guard<ReadWriteLock, ReadWriteLock::Read> guard_a{&a.lock};
    a.TestShared();
#if TEST_WILL_NOT_COMPILE || 0
    a.TestRequire();
#endif
  }

  {
    Baz<ReadWriteLock> a{};

    Guard<ReadWriteLock, ReadWriteLock::Write> guard_a{&a.lock};
    a.TestShared();
    a.TestRequire();
  }

  END_TEST;
}

UNITTEST_START_TESTCASE(lock_dep_tests)
UNITTEST("lock_dep_dynamic_analysis_tests", lock_dep_dynamic_analysis_tests)
UNITTEST("lock_dep_static_analysis_tests", lock_dep_static_analysis_tests)
UNITTEST_END_TESTCASE(lock_dep_tests, "lock_dep_tests", "lock_dep_tests")
#endif

#if WITH_LOCK_DEP
static bool bug_84827_regression_test() {
  BEGIN_TEST;

  cpu_mask_t online_cpus = mp_get_online_mask();
  ASSERT_GE(ktl::popcount(online_cpus), 2, "Must have at least 2 CPUs online");

  enum class TestState : uint32_t {
    WaitingForThreadStart,
    ThreadWaitingInLock,
    IPIHasBeenQueued,
  };

  struct TestArgs {
    DECLARE_SPINLOCK(TestArgs) lock;
    cpu_mask_t first_cpu_mask{0};
    cpu_mask_t second_cpu_mask{0};
    ktl::atomic<TestState> state{TestState::WaitingForThreadStart};
  } args;

  // Determine the cores that this test will use.
  for (cpu_num_t i = 0; !args.second_cpu_mask; ++i) {
    if ((online_cpus & cpu_num_to_mask(i)) != 0) {
      if (!args.first_cpu_mask) {
        args.first_cpu_mask = cpu_num_to_mask(i);
      } else {
        args.second_cpu_mask = cpu_num_to_mask(i);
      }
    }
  }

  // Migrate to the secondary CPU we have chosen.  No matter what happens after
  // this, don't forget to set our affinity mask back to what it was before.
  cpu_mask_t prev_affinity = Thread::Current::Get()->GetCpuAffinity();
  auto cleanup =
      fit::defer([prev_affinity]() { Thread::Current::Get()->SetCpuAffinity(prev_affinity); });
  Thread::Current::Get()->SetCpuAffinity(args.second_cpu_mask);

  // Start up our work thread.  It will migrate to our first chosen core and
  // then signal us via the test args state.
  Thread* work_thread = Thread::Create(
      "bug_84827_test_thread",
      [](void* ctx) -> int {
        TestArgs& args = *(reinterpret_cast<TestArgs*>(ctx));

        // Migrate to our chosen cpu.
        Thread::Current::Get()->SetCpuAffinity(args.first_cpu_mask);

        // Enter the lock and signal that we are in the waiting-in-lock state.  Then
        // wait until the main thread signals to us that it has queued the IPI.
        {
          Guard<SpinLock, IrqSave> guard(&args.lock);
          args.state.store(TestState::ThreadWaitingInLock);
          while (args.state.load() != TestState::IPIHasBeenQueued) {
            // empty body, just spin.
          }
        }
        return 0;
      },
      &args, DEFAULT_PRIORITY);
  ASSERT_NONNULL(work_thread);
  work_thread->Resume();

  // Make sure we clean up our thread when we are done.
  auto cleanup_thread = fit::defer([&work_thread]() {
    int retcode;
    work_thread->Thread::Join(&retcode, ZX_TIME_INFINITE);
    work_thread = nullptr;
  });

  // Wait until we know that our work_thread is in the waiting state.
  while (args.state.load() != TestState::ThreadWaitingInLock) {
    Thread::Current::SleepRelative(ZX_USEC(100));
  }

  // OK, our work thread is now inside of the test spinlock and spinning,
  // waiting for us to release it by setting the test state variable to
  // IPIHasBeenQueued.
  //
  // We now schedule a small lambda to run at IRQ time on both our core, as well
  // as the core the work_thread is on, using mp_sync_exec.
  //
  // What should happen here is that the task sent to the work_thread will become
  // pending, but not able to run because the thread we just started is spinning
  // in the spinlock.  _After_ this task has been queued and the IPI sent, the
  // local core will run its copy of the task.  Note that this is currently a
  // side effect of the behavior of mp_sync_exec, but one that we depend on.
  // The order is important here.
  //
  // The local core will spin for just a little bit (to make absolutely sure
  // that the IPI has been registered with the other core) and then release the
  // work thread by changing the state variable.
  //
  // When the work thread eventually drops the lock, the IPI should immediately
  // fire and cause the thread to re-obtain the lock again.  If the lockdep
  // bookkeeping has not been cleared at this point in time, it will appear to
  // the lockdep code that our thread is re-entering a lock it is already
  // holding (even though the lock has already been dropped) triggering an OOPS.
  mp_sync_exec(
      MP_IPI_TARGET_MASK, args.first_cpu_mask | args.second_cpu_mask,
      [](void* ctx) {
        TestArgs& args = *(reinterpret_cast<TestArgs*>(ctx));

        if (args.first_cpu_mask == Thread::Current::Get()->GetCpuAffinity()) {
          bool prev_state = arch_blocking_disallowed();
          arch_set_blocking_disallowed(false);
          { Guard<SpinLock, IrqSave> guard(&args.lock); }
          arch_set_blocking_disallowed(prev_state);
        } else {
          zx_time_t spin_deadline = current_time() + ZX_USEC(10);
          while (current_time() < spin_deadline) {
            // empty body, just spin.
          }
          args.state.store(TestState::IPIHasBeenQueued);
        }
      },
      &args);

  END_TEST;
}

UNITTEST_START_TESTCASE(lock_dep_regression_tests)
UNITTEST("Bug #84827 regression test", bug_84827_regression_test)
UNITTEST_END_TESTCASE(lock_dep_regression_tests, "lock_dep_regression_tests",
                      "lock_dep_regression_tests")
#endif
