// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <zircon/compiler.h>

#include <type_traits>

#include <lockdep/global_reference.h>

namespace lockdep {

// Tags the given lock type with an option name and a policy type. The policy
// type describes how to acquire and release the lock type and whether or not
// extra state must be stored to correctly operate the lock (i.e. IRQ state in
// spinlocks). The option name permits selecting different lock policies to
// apply when acquiring and releasing the lock (i.e. whether or not to save IRQ
// state when taking a spinlock).
//
// Arguments:
//  lock_type  : The type of the lock to specify the policy for. This type is
//               passed to the LockType argument of Guard<LockType, Option>
//               when acquiring this lock type.
//  option_name: A type tag to associate with this lock type and policy. This
//               type is passed to the Option argument of
//               Guard<LockType, Option> to select this policy instead of
//               another policy for the same lock type.
//  lock_policy: The policy to use when Guard<LockType, Option> specifies the
//               lock_type and option_name arguments given with this policy.
//
// This macro essentially creates a map from the tuple (lock_type, option_name)
// to lock_policy when instantiating Guard<lock_type, option_name>.
//
// Every lock policy must specify a public nested type named |State| that stores
// any state required by the lock acquisition. If state is not required then the
// type may be an empty struct. Every lock policy must also define three static
// methods to handle the acquire and release operations, as well as to handle
// any special actions which must be taken immediately before validation while
// acquiring the lock.
//
// For most locks, the pre-validation acquire step will be a no-op,
// however, there are situations where the pre-validation hook is important.
//
// For example, consider a spinlock which is used in the kernel both during
// normal operation, and while servicing an interrupt.  Successful validation of
// an Acquire operation involves lockdep recording ownership of the lock being
// acquired in currently active context.  If this information is recorded, and
// then an interrupt is taken before interrupts are disabled and the lock is
// acquired, then it is possible for the system to attempt to acquire the lock
// again during the ISR, triggering a false positive lockdep violation.  The ISR
// thinks that the lock is already acquired, even though it has not been just
// yet.  In practice, this can never actually happen as code operating in
// non-ISR context must always have disabled interrupts before actually
// acquiring the lock.
//
// So, for an IrqSave style spinlock, the lock policy disables interrupts during
// the pre-validation phase.  Then validation of the lock is performed and
// ownership recorded, and finally the lock is acquired during the policy's
// acquire phase, preventing the false positive described above.
//
// Note that in the case of a try-lock operation, if the lock acquisition fails
// during Acquire, it is the policy implementer's responsibility to undo any
// stateful actions take by the PreValidate implementation.  In the case of the
// IrqSave spinlock example described above, this would involve restoring the
// IRQ-enabled state to what it was before PreValidate had been called.
//
// For example:
//
//  struct LockPolicy {
//      struct State {
//          // State members, constructors, and destructors as needed.
//      };
//
//      static void PreValidate(LockType* lock, State* state) {
//          // Take any actions needed immediately before validation.
//      }
//
//      static bool Acquire(LockType* lock, State* state) __TA_ACQUIRE(lock) {
//          // Saves any state required by this lock type.
//          // Acquires the lock for this lock type.
//          // Returns whether the acquisition was successful.
//      }
//
//      static void Release(LockType* lock, State* state) {
//          // Releases the lock for this lock type.
//          // Restores any state required for this lock type.
//      }
//
//      static void AssertHeld(const LockType& lock) __TA_ASSERT(lock) {
//          // Assert that "lock" is held by the current thread,
//          // aborting execution if not.
//      }
//  };
//
#define LOCK_DEP_POLICY_OPTION(lock_type, option_name, lock_policy)         \
  ::lockdep::AmbiguousOption LOCK_DEP_GetLockPolicyType(lock_type*, void*); \
  lock_policy LOCK_DEP_GetLockPolicyType(lock_type*, option_name*)

// Tags the given lock type with the given policy type. Like the macro above
// the policy type describes how to acquire and release the lock and whether or
// not extra state must be stored to correctly release the lock. This variation
// is appropriate for lock types that do not have different options. This macro
// and the macro above are mutually exclusive and may not be used on the same
// lock type.
//
// Arguments:
//  lock_type : The type of the lock to specify the policy for. This type is
//             passed to the LockType argument of Guard<LockType> to select this
//             lock type to guard. The Optional argument of Guard may not be
//             specified.
//  lock_policy: The policy to use when Guard<LockType> specifies the lock_type
//               given with this policy.
//
// The policy type follows the same structure as the policy type described for
// the macro above.
//
#define LOCK_DEP_POLICY(lock_type, lock_policy) \
  lock_policy LOCK_DEP_GetLockPolicyType(lock_type*, void*)

// Looks up the lock policy for the given lock type and optional option type, as
// specified by the macros above. This utility resolves the tagged types across
// namespaces using ADL.
template <typename Lock, typename Option = void>
using LookupLockPolicy = decltype(LOCK_DEP_GetLockPolicyType(
    static_cast<RemoveGlobalReference<Lock>*>(nullptr), static_cast<Option*>(nullptr)));

namespace internal {

// Detect whether T has a member function `AssertHeld()`.
template <typename T, typename = void>
struct HasAssertHeld : std::false_type {};
template <typename T>
struct HasAssertHeld<T, std::void_t<decltype(std::declval<const T>().AssertHeld())>>
    : std::true_type {};

template <typename T>
using EnableIfHasAssertHeld = std::enable_if_t<HasAssertHeld<T>::value>;

// Detects whether T has a subtype `T::ValidationGuard`.
template <typename T, typename = void>
struct HasValidationGuard : std::false_type {};
template <typename T>
struct HasValidationGuard<T, std::void_t<typename T::ValidationGuard>> : std::true_type {};

}  // namespace internal

// Null validation guard for use cases that do not need additional protection of
// the thread local lock list.
struct NullValidationGuard {
  // Constructor must be non-trivial to avoid unused variable warnings.
  NullValidationGuard() {}
  ~NullValidationGuard() = default;
};

// Default lock policy type that describes how to acquire and release a basic
// mutex with no additional state or flags.
struct DefaultLockPolicy {
  // This policy does not specify any additional state for a lock acquisition.
  struct State {};

  // This policy does not specify additional actions to guard lock validation.
  using ValidationGuard = NullValidationGuard;

  // Default lock policy has nothing special to do just before validation.
  template <typename Lock>
  static void PreValidate(Lock*, State*) {}

  // Acquires the lock by calling its Acquire method. The extra state argument
  // is unused.
  template <typename Lock>
  static bool Acquire(Lock* lock, State*) __TA_ACQUIRE(lock) {
    lock->Acquire();
    return true;
  }

  // Releases the lock by calling its Release method. The extra state argument
  // is unused.
  template <typename Lock>
  static void Release(Lock* lock, State*) __TA_RELEASE(lock) {
    lock->Release();
  }

  // Assert that the given lock is exclusively held by the current thread.
  //
  // Can be used both for runtime debugging checks, and also to help when
  // thread safety analysis can't prove you are holding a lock. The underlying
  // lock implementation may optimize away asserts in release builds.
  //
  // This will typically be invoked by users through the function
  // "AssertHeld()" declared in "guard.h".
  template <typename Lock, typename = internal::EnableIfHasAssertHeld<Lock>>
  static void AssertHeld(const Lock& lock) __TA_ASSERT(lock) {
    lock.AssertHeld();
  }
};

// Sentinel type used to prevent mixing LOCK_DEP_POLICY_OPTION and
// LOCK_DEP_POLICY on same lock type. Mixing these macros on the same lock type
// causes a static assert in Guard for that lock type.
struct AmbiguousOption {};

// Base lock policy type that simply returns the DefaultLockPolicy. This is the
// default policy applied to any lock that is not tagged with the macros above
// when the default policy is enabled. The default policy is disabled by default
// to avoid mistakes in environments that require validation guards for correct
// operation, such as the kernel.
template <typename Lock, typename Option = void, typename Enabled = void>
struct LockPolicyType {
#if LOCK_DEP_ENABLE_DEFAULT_LOCK_POLICY
  using Type = DefaultLockPolicy;
#else
  static_assert(
      !std::is_same<Lock, Lock>::value,
      "Default lock policy is disabled. Please define a custom policy or enable the default policy.");
#endif
};

// Specialization that returns the lock policy type for the combination of
// |Lock| and |Option| tagged by the macros above.
template <typename Lock, typename Option>
struct LockPolicyType<Lock, Option, std::void_t<LookupLockPolicy<Lock, Option>>> {
  using Type = LookupLockPolicy<Lock, Option>;
  static_assert(internal::HasValidationGuard<Type>::value,
                "Custom policy missing ValidationGuard subtype!");
};

// Alias that selects the lock policy for the given |Lock| and optional
// |Option| type. This alias simplifies lock policy type expressions used
// elsewhere.
template <typename Lock, typename Option = void>
using LockPolicy = typename LockPolicyType<Lock, Option>::Type;

}  // namespace lockdep
