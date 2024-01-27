// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_STDCOMPAT_INTERNAL_ATOMIC_H_
#define LIB_STDCOMPAT_INTERNAL_ATOMIC_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <type_traits>

#include "../memory.h"
#include "../type_traits.h"
#include "linkage.h"

namespace cpp20 {
namespace atomic_internal {

// Maps |std::memory_order| to builtin |__ATOMIC_XXXX| values.
LIB_STDCOMPAT_INLINE_LINKAGE constexpr int to_builtin_memory_order(std::memory_order order) {
  switch (order) {
    case std::memory_order_relaxed:
      return __ATOMIC_RELAXED;
    case std::memory_order_consume:
      return __ATOMIC_CONSUME;
    case std::memory_order_acquire:
      return __ATOMIC_ACQUIRE;
    case std::memory_order_release:
      return __ATOMIC_RELEASE;
    case std::memory_order_acq_rel:
      return __ATOMIC_ACQ_REL;
    case std::memory_order_seq_cst:
      return __ATOMIC_SEQ_CST;
    default:
      __builtin_abort();
  }
}

// Applies corrections to |order| on |compare_exchange|'s load operations.
LIB_STDCOMPAT_INLINE_LINKAGE constexpr std::memory_order compare_exchange_load_memory_order(
    std::memory_order order) {
  if (order == std::memory_order_acq_rel) {
    return std::memory_order_acquire;
  }

  if (order == std::memory_order_release) {
    return std::memory_order_relaxed;
  }

  return order;
}

// Unspecialized types alignment, who have a size that matches a known integer(power of 2 bytes)
// should be aligned to its size at least for better performance. Otherwise, we default to its
// default alignment.
template <typename T, typename Enabled = void>
struct alignment {
  static constexpr size_t required_alignment =
      std::max((sizeof(T) & (sizeof(T) - 1) || sizeof(T) > 16) ? 0 : sizeof(T), alignof(T));
};

template <typename T>
struct alignment<T, std::enable_if_t<cpp17::is_integral_v<T>>> {
  static constexpr size_t required_alignment = sizeof(T) > alignof(T) ? sizeof(T) : alignof(T);
};

template <typename T>
struct alignment<T, std::enable_if_t<cpp17::is_pointer_v<T> || cpp17::is_floating_point_v<T>>> {
  static constexpr size_t required_alignment = alignof(T);
};

template <typename T>
static constexpr bool unqualified = cpp17::is_same_v<T, std::remove_cv_t<T>>;

template <typename T>
LIB_STDCOMPAT_INLINE_LINKAGE inline bool compare_exchange(T* ptr,
                                                          std::remove_volatile_t<T>& expected,
                                                          std::remove_volatile_t<T> desired,
                                                          bool is_weak, std::memory_order success,
                                                          std::memory_order failure) {
  return __atomic_compare_exchange(ptr, cpp17::addressof(expected), cpp17::addressof(desired),
                                   is_weak, to_builtin_memory_order(success),
                                   to_builtin_memory_order(failure));
}

// Provide atomic operations based on compiler builtins.
template <typename Derived, typename T>
class atomic_ops {
 private:
  // Removes |volatile| and deprecation messages from static analizers.
  using value_t = std::remove_cv_t<T>;

  // Storage.
  using storage_t = std::aligned_storage_t<sizeof(T), alignof(T)>;

 public:
  LIB_STDCOMPAT_INLINE_LINKAGE void store(
      value_t desired, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    __atomic_store(ptr(), cpp17::addressof(desired), to_builtin_memory_order(order));
  }

  LIB_STDCOMPAT_INLINE_LINKAGE value_t
  load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
    storage_t store;
    value_t* ret = reinterpret_cast<value_t*>(&store);
    __atomic_load(ptr(), ret, to_builtin_memory_order(order));
    return *ret;
  }

  LIB_STDCOMPAT_INLINE_LINKAGE operator value_t() const noexcept { return this->load(); }

  LIB_STDCOMPAT_INLINE_LINKAGE value_t
  exchange(value_t desired, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    storage_t store;
    value_t* ret = reinterpret_cast<value_t*>(&store);
    value_t noncv_desired = desired;
    __atomic_exchange(ptr(), cpp17::addressof(noncv_desired), ret, to_builtin_memory_order(order));
    return *ret;
  }

  LIB_STDCOMPAT_INLINE_LINKAGE bool compare_exchange_weak(
      T& expected, value_t desired,
      std::memory_order success = std::memory_order_seq_cst) const noexcept {
    return compare_exchange_weak(expected, desired, success,
                                 compare_exchange_load_memory_order(success));
  }

  LIB_STDCOMPAT_INLINE_LINKAGE bool compare_exchange_weak(
      T& expected, value_t desired, std::memory_order success,
      std::memory_order failure) const noexcept {
    check_failure_memory_order(failure);
    return compare_exchange(ptr(), expected, desired,
                            /*is_weak=*/true, success, failure);
  }

  LIB_STDCOMPAT_INLINE_LINKAGE bool compare_exchange_strong(
      T& expected, value_t desired,
      std::memory_order success = std::memory_order_seq_cst) const noexcept {
    return compare_exchange_strong(expected, desired, success,
                                   compare_exchange_load_memory_order(success));
  }

  LIB_STDCOMPAT_INLINE_LINKAGE bool compare_exchange_strong(
      T& expected, value_t desired, std::memory_order success,
      std::memory_order failure) const noexcept {
    check_failure_memory_order(failure);
    return compare_exchange(ptr(), expected, desired,
                            /*is_weak=*/false, success, failure);
  }

 private:
  // |failure| memory order may not be |std::memory_order_release| or
  // |std::memory_order_acq_release|.
  LIB_STDCOMPAT_INLINE_LINKAGE constexpr void check_failure_memory_order(
      std::memory_order failure) const {
    if (failure == std::memory_order_acq_rel || failure == std::memory_order_release) {
      __builtin_abort();
    }
  }
  constexpr T* ptr() const { return static_cast<const Derived*>(this)->ptr_; }
};

// Delegate to helper templates the arguments which differ between pointer and integral types.
template <typename T>
struct arithmetic_ops_helper {
  // Return type of |ptr| method.
  using ptr_type = T*;

  // Return type of atomic builtins.
  using return_type = std::remove_volatile_t<T>;

  // Type of operands used.
  using operand_type = std::remove_volatile_t<T>;

  // Arithmetic operands are amplified by this scalar.
  static constexpr size_t modifier = 1;
};

template <typename T>
struct arithmetic_ops_helper<T*> {
  // Return type of |ptr| method.
  using ptr_type = T**;

  // Return type of atomic builtins.
  using return_type = T*;

  // Type of operands used.
  using operand_type = ptrdiff_t;

  // Arithmetic operands are amplified by this scalar.
  static constexpr size_t modifier = sizeof(T);
};

template <typename T>
using difference_t = typename arithmetic_ops_helper<T>::operand_type;

// Arithmetic operations.
//
// Enables :
//  - fetch_add
//  - fetch_sub
//  - operator++
//  - operator--
//  - operator+=
//  - operator-=
template <typename Derived, typename T, typename Enabled = void>
class arithmetic_ops {};

// Non volatile pointers Pointer and Integral operations.
template <typename Derived, typename T>
class arithmetic_ops<Derived, T,
                     std::enable_if_t<cpp17::is_integral_v<T> ||
                                      (cpp17::is_pointer_v<T> && !cpp17::is_volatile_v<T>)>> {
  using return_t = typename arithmetic_ops_helper<T>::return_type;
  using operand_t = typename arithmetic_ops_helper<T>::operand_type;
  using ptr_t = typename arithmetic_ops_helper<T>::ptr_type;
  static constexpr auto modifier = arithmetic_ops_helper<T>::modifier;

 public:
  LIB_STDCOMPAT_INLINE_LINKAGE return_t
  fetch_add(operand_t operand, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return __atomic_fetch_add(ptr(), operand * modifier, to_builtin_memory_order(order));
  }

  LIB_STDCOMPAT_INLINE_LINKAGE return_t
  fetch_sub(operand_t operand, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return __atomic_fetch_sub(ptr(), operand * modifier, to_builtin_memory_order(order));
  }

  LIB_STDCOMPAT_INLINE_LINKAGE return_t operator++(int) const noexcept { return fetch_add(1); }
  LIB_STDCOMPAT_INLINE_LINKAGE return_t operator--(int) const noexcept { return fetch_sub(1); }
  LIB_STDCOMPAT_INLINE_LINKAGE return_t operator++() const noexcept { return fetch_add(1) + 1; }
  LIB_STDCOMPAT_INLINE_LINKAGE return_t operator--() const noexcept { return fetch_sub(1) - 1; }
  LIB_STDCOMPAT_INLINE_LINKAGE return_t operator+=(operand_t operand) const noexcept {
    return fetch_add(operand) + operand;
  }
  LIB_STDCOMPAT_INLINE_LINKAGE return_t operator-=(operand_t operand) const noexcept {
    return fetch_sub(operand) - operand;
  }

 private:
  LIB_STDCOMPAT_INLINE_LINKAGE constexpr ptr_t ptr() const {
    return static_cast<const Derived*>(this)->ptr_;
  }
};

// Floating point arithmetic operations.
// Based on CAS cycles to perform atomic add and sub.
template <typename Derived, typename T>
class arithmetic_ops<Derived, T, std::enable_if_t<cpp17::is_floating_point_v<T>>> {
 private:
  using value_t = std::remove_volatile_t<T>;

 public:
  LIB_STDCOMPAT_INLINE_LINKAGE value_t
  fetch_add(value_t operand, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    value_t old_value = derived()->load(std::memory_order_relaxed);
    value_t new_value = old_value + operand;
    while (
        !compare_exchange(ptr(), old_value, new_value, false, order, std::memory_order_relaxed)) {
      new_value = old_value + operand;
    }
    return old_value;
  }

  LIB_STDCOMPAT_INLINE_LINKAGE value_t
  fetch_sub(value_t operand, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    value_t old_value = derived()->load(std::memory_order_relaxed);
    value_t new_value = old_value - operand;
    while (
        !compare_exchange(ptr(), old_value, new_value, false, order, std::memory_order_relaxed)) {
      new_value = old_value - operand;
    }
    return old_value;
  }

  LIB_STDCOMPAT_INLINE_LINKAGE value_t operator+=(value_t operand) const noexcept {
    return fetch_add(operand) + operand;
  }
  LIB_STDCOMPAT_INLINE_LINKAGE value_t operator-=(value_t operand) const noexcept {
    return fetch_sub(operand) - operand;
  }

 private:
  LIB_STDCOMPAT_INLINE_LINKAGE constexpr T* ptr() const {
    return static_cast<const Derived*>(this)->ptr_;
  }
  LIB_STDCOMPAT_INLINE_LINKAGE constexpr const Derived* derived() const {
    return static_cast<const Derived*>(this);
  }
};

// Bitwise operations.
//
// Enables :
//  - fetch_and
//  - fetch_or
//  - fetch_xor
//  - operator&=
//  - operator|=
//  - operator^=
template <typename Derived, typename T, typename Enabled = void>
class bitwise_ops {};

template <typename Derived, typename T>
class bitwise_ops<Derived, T, std::enable_if_t<cpp17::is_integral_v<T>>> {
 private:
  // Removes |volatile| and deprecation messages from static analizers.
  using value_t = std::remove_cv_t<T>;

 public:
  LIB_STDCOMPAT_INLINE_LINKAGE value_t
  fetch_and(value_t operand, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return __atomic_fetch_and(ptr(), operand, to_builtin_memory_order(order));
  }

  LIB_STDCOMPAT_INLINE_LINKAGE value_t
  fetch_or(value_t operand, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return __atomic_fetch_or(ptr(), operand, to_builtin_memory_order(order));
  }

  LIB_STDCOMPAT_INLINE_LINKAGE value_t
  fetch_xor(value_t operand, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return __atomic_fetch_xor(ptr(), operand, to_builtin_memory_order(order));
  }

  LIB_STDCOMPAT_INLINE_LINKAGE value_t operator&=(value_t operand) const noexcept {
    return fetch_and(operand) & operand;
  }
  LIB_STDCOMPAT_INLINE_LINKAGE value_t operator|=(value_t operand) const noexcept {
    return fetch_or(operand) | operand;
  }
  LIB_STDCOMPAT_INLINE_LINKAGE value_t operator^=(value_t operand) const noexcept {
    return fetch_xor(operand) ^ operand;
  }

 private:
  LIB_STDCOMPAT_INLINE_LINKAGE constexpr T* ptr() const {
    return static_cast<const Derived*>(this)->ptr_;
  }
};

}  // namespace atomic_internal
}  // namespace cpp20

#endif  // LIB_STDCOMPAT_INTERNAL_ATOMIC_H_
