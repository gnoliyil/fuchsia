// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_CPP_WIRE_INTERNAL_INTRUSIVE_CONTAINER_HELPER_MACROS_H_
#define LIB_FIDL_CPP_WIRE_INTERNAL_INTRUSIVE_CONTAINER_HELPER_MACROS_H_

#include <type_traits>

// Macro for defining a trait that checks if a type T has a method with the
// given name. This is not as strong as using is_same to check function
// signatures, but checking this trait first gives a better static_assert
// message than the compiler warnings from is_same if the function doesn't
// exist.
// Note that the resulting trait_name will be in the namespace where the macro
// is used.
//
// Example:
//
// DECLARE_HAS_MEMBER_FN(has_bar, Bar);
// template <typename T>
// class Foo {
//   static_assert(has_bar_v<T>, "Foo classes must implement Bar()!");
//   // TODO: use 'if constexpr' to avoid this next static_assert once c++17
//   lands.
//   static_assert(is_same_v<decltype(&T::Bar), void (T::*)(int)>,
//                 "Bar must be a non-static member function with signature "
//                 "'void Bar(int)', and must be visible to Foo (either "
//                 "because it is public, or due to friendship).");
//  };
#define DECLARE_HAS_MEMBER_FN(trait_name, fn_name)                   \
  template <typename T>                                              \
  struct trait_name {                                                \
   private:                                                          \
    template <typename C>                                            \
    static std::true_type test(decltype(&C::fn_name));               \
    template <typename C>                                            \
    static std::false_type test(...);                                \
                                                                     \
   public:                                                           \
    static constexpr bool value = decltype(test<T>(nullptr))::value; \
  };                                                                 \
  template <typename T>                                              \
  static inline constexpr bool trait_name##_v = trait_name<T>::value

// Similar to DECLARE_HAS_MEMBER_FN but also checks the function signature.
// This is especially useful when the desired function may be overloaded.
// The signature must take the form "ResultType (C::*)(ArgType1, ArgType2)".
//
// Example:
//
// FIDL_INTERNAL_DECLARE_HAS_MEMBER_FN_WITH_SIGNATURE(has_c_str, c_str, const char* (C::*)() const);
#define FIDL_INTERNAL_DECLARE_HAS_MEMBER_FN_WITH_SIGNATURE(trait_name, fn_name, sig) \
  template <typename T>                                                              \
  struct trait_name {                                                                \
   private:                                                                          \
    template <typename C>                                                            \
    static std::true_type test(decltype(static_cast<sig>(&C::fn_name)));             \
    template <typename C>                                                            \
    static std::false_type test(...);                                                \
                                                                                     \
   public:                                                                           \
    static constexpr bool value = decltype(test<T>(nullptr))::value;                 \
  };                                                                                 \
  template <typename T>                                                              \
  static inline constexpr bool trait_name##_v = trait_name<T>::value

// Similar to DECLARE_HAS_MEMBER_FN but for member types.
//
// Example:
//
// FIDL_INTERNAL_DECLARE_HAS_MEMBER_TYPE(has_value_type, ValueType);
#define FIDL_INTERNAL_DECLARE_HAS_MEMBER_TYPE(trait_name, type_name) \
  template <typename T>                                              \
  struct trait_name {                                                \
   private:                                                          \
    template <typename C>                                            \
    static std::true_type test(typename C::type_name*);              \
    template <typename C>                                            \
    static std::false_type test(...);                                \
                                                                     \
   public:                                                           \
    static constexpr bool value = decltype(test<T>(nullptr))::value; \
  };                                                                 \
  template <typename T>                                              \
  static inline constexpr bool trait_name##_v = trait_name<T>::value

#endif  // LIB_FIDL_CPP_WIRE_INTERNAL_INTRUSIVE_CONTAINER_HELPER_MACROS_H_
