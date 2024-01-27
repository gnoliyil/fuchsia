// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_INCLUDE_KERNEL_LOCKDEP_H_
#define ZIRCON_KERNEL_INCLUDE_KERNEL_LOCKDEP_H_

#include <lockdep/guard.h>
#include <lockdep/guard_multiple.h>
#include <lockdep/lockdep.h>

// Bring some lockdep types into global namespace for the kernel.
// TODO(eieio): Is there a better namespace to put these in, or perhaps they
// should just be used from the lockdep namespace?
using lockdep::AdoptLock;
using lockdep::AliasedLock;
using lockdep::AssertHeld;
using lockdep::AssertOrderedLock;
using lockdep::Guard;
using lockdep::GuardMultiple;
using lockdep::Lock;
using lockdep::LockFlagsActiveListDisabled;
using lockdep::LockFlagsReportingDisabled;
using lockdep::LockFlagsTrackingDisabled;
using lockdep::NullGuard;

// Defines a singleton lock with the given name that wraps a raw global lock.
// The singleton instance may be retrieved using the static Get() method
// provided by the base class. The raw global lock is used as the underlying
// lock instead of an internally-defined lock. This is useful to instrument an
// existing global lock that may be shared with C code or for other reasons
// cannot be completely replaced with the above global lock type.
//
// Arguments:
//  name:        The name of the singleton to define.
//  global_lock: The global lock to wrap with this lock type. This must be an
//               lvalue reference expression to a lock with static storage
//               duration, either external or internal.
//  __VA_ARGS__: LockFlags expression specifying lock flags to honor in addition
//               to the flags specified for the lock type using the
//               LOCK_DEP_TRAITS macro below.
//
// Example usage:
//
//  extern SpinLock thread_lock;
//  LOCK_DEP_SINGLETON_LOCK_WRAPPER(ThreadLock, thread_lock [, LockFlags]);
//
//  void DoThreadStuff() {
//      Guard<SpinLock, IrqSave> guard{ThreadLock::Get()};
//      // ...
//  }
#define DECLARE_SINGLETON_LOCK_WRAPPER(name, global_lock, ...) \
  LOCK_DEP_SINGLETON_LOCK_WRAPPER(name, global_lock, ##__VA_ARGS__)

#endif  // ZIRCON_KERNEL_INCLUDE_KERNEL_LOCKDEP_H_
