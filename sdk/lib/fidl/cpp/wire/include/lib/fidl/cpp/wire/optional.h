// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FIDL_CPP_WIRE_OPTIONAL_H_
#define LIB_FIDL_CPP_WIRE_OPTIONAL_H_

#include <lib/fidl/cpp/wire/traits.h>
#include <zircon/assert.h>

namespace fidl {

namespace internal {
template <typename T, typename Constraint, bool IsRecursive, class Enable>
struct WireCodingTraits;
}

// |fidl::WireOptional<T>| wraps a wire union type |T| and represents the optional
// version of that union. Conceptually it is similar to an |std::optional|, but
// it is optimized to have the same memory layout as |T|, using the fact that
// FIDL unions are naturally optional (an absent union consists of all zeros).
//
// TODO(fxbug.dev/109737): Consider using |fidl::WireOptional| to represent optional
// vectors and optional strings.
template <typename T>
struct WireOptional final {
 public:
  // Constructs an absent optional union.
  WireOptional() : t_() { static_assert(IsUnion<T>::value, "|T| must be a wire FIDL union."); }

  WireOptional(const WireOptional& other) = default;
  WireOptional(WireOptional&& other) noexcept = default;
  WireOptional& operator=(const WireOptional& other) = default;
  WireOptional& operator=(WireOptional&& other) noexcept = default;
  ~WireOptional() = default;

  // Intentional implicit constructor to go from |T| to an |WireOptional<T>|
  // NOLINTNEXTLINE(google-explicit-constructor)
  WireOptional(const T& t) : t_(t) {}

  // Intentional implicit constructor to go from |T| to an |WireOptional<T>|
  // NOLINTNEXTLINE(google-explicit-constructor)
  WireOptional(T&& t) noexcept : t_(std::move(t)) {}

  WireOptional& operator=(const T& other) {
    t_ = other;
    return *this;
  }
  WireOptional& operator=(T&& other) noexcept {
    t_ = std::move(other);
    return *this;
  }

  // Returns whether the union is present.
  bool has_value() const { return !t_.has_invalid_tag(); }

  // Accesses the union.
  T& value() {
    ZX_ASSERT(has_value());
    return t_;
  }

  // Accesses the union.
  const T& value() const {
    ZX_ASSERT(has_value());
    return t_;
  }

  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

  T& operator*() { return value(); }
  const T& operator*() const { return value(); }

 private:
  template <typename U, typename Constraint, bool IsRecursive, class Enable>
  friend struct ::fidl::internal::WireCodingTraits;

  T t_;
};

template <typename T>
struct IsResource<WireOptional<T>> : public IsResource<T> {};

template <typename T>
struct TypeTraits<WireOptional<T>> : public TypeTraits<T> {};

template <typename T>
struct IsFidlType<WireOptional<T>> : public IsFidlType<T> {};

template <typename T>
struct IsUnion<WireOptional<T>> : public IsUnion<T> {};

}  // namespace fidl

#endif  // LIB_FIDL_CPP_WIRE_OPTIONAL_H_
