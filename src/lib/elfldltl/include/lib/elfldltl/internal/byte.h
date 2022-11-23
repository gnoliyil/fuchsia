// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_ELFLDLTL_INCLUDE_LIB_ELFLDLTL_INTERNAL_BYTE_H_
#define SRC_LIB_ELFLDLTL_INCLUDE_LIB_ELFLDLTL_INTERNAL_BYTE_H_

#include <cstdint>
#include <string_view>

namespace elfldltl::internal {

// TODO(fxbug.dev/115994): This is what the libc++ default template for
// std::char_traits used to do, but it's nonstandard to expect that.  The
// note.h API should migrate away from using string_view<std::byte>, probably
// using span<const std::byte> instead; but that has different semantics for
// operator== and such, so it requires some careful refactoring.
struct ByteCharTraits : public std::char_traits<uint8_t> {
  using char_type = std::byte;

  static constexpr void assign(char_type& a, const char_type& b) { a = b; }

  static constexpr bool eq(char_type a, char_type b) { return a == b; }

  static constexpr bool lt(char_type a, char_type b) { return a < b; }

  static int compare(const char_type* a, const char_type* b, size_t n) {
    return std::char_traits<uint8_t>::compare(reinterpret_cast<const uint8_t*>(a),
                                              reinterpret_cast<const uint8_t*>(b), n);
  }

  static size_t length(const char_type* s) {
    return std::char_traits<uint8_t>::length(reinterpret_cast<const uint8_t*>(s));
  }

  static const char_type* find(const char_type* s, size_t n, const char_type& c) {
    return reinterpret_cast<const char_type*>(std::char_traits<uint8_t>::find(
        reinterpret_cast<const uint8_t*>(s), n, static_cast<uint8_t>(c)));
  }

  static char_type* move(char_type* a, const char_type* b, size_t n) {
    return reinterpret_cast<char_type*>(std::char_traits<uint8_t>::move(
        reinterpret_cast<uint8_t*>(a), reinterpret_cast<const uint8_t*>(b), n));
  }

  static char_type* copy(char_type* a, const char_type* b, size_t n) {
    return reinterpret_cast<char_type*>(std::char_traits<uint8_t>::copy(
        reinterpret_cast<uint8_t*>(a), reinterpret_cast<const uint8_t*>(b), n));
  }

  static char_type* assign(char_type* s, size_t n, const char_type& c) {
    return reinterpret_cast<char_type*>(std::char_traits<uint8_t>::assign(
        reinterpret_cast<uint8_t*>(s), n, static_cast<uint8_t>(c)));
  }

  static constexpr char_type to_char_type(int_type x) { return static_cast<char_type>(x); }

  static constexpr int_type to_int_type(char_type x) { return static_cast<int_type>(x); }
};

}  // namespace elfldltl::internal

#endif  // SRC_LIB_ELFLDLTL_INCLUDE_LIB_ELFLDLTL_INTERNAL_BYTE_H_
