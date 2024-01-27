// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HWREG_INTERNAL_H_
#define HWREG_INTERNAL_H_

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>

#include <array>
#include <type_traits>
#include <variant>

// Internal macros for generating a unique name for the internal bookkeeping
// member associated with a given field. Beyond its definition, the member is
// never referenced again, within or outside of the macro: accordingly, we use
// __LINE__ as a simple way to satisfy the desired uniqueness. We do not make
// the name a function of the bit range as that would otherwise preclude the
// use of constexpr arithmetic expressions for bits values; moreover, we cannot
// rely on field name alone, as their an a priori unique one for each reserved
// field, and so would still need an alternate source of uniqueness to cover
// that case.
#define HWREG_INTERNAL_PASTE_IMPL(a, b) a##b
#define HWREG_INTERNAL_PASTE(a, b) HWREG_INTERNAL_PASTE_IMPL(a, b)
#define HWREG_INTERNAL_MEMBER_NAME(NAME) \
  HWREG_INTERNAL_PASTE(field_, HWREG_INTERNAL_PASTE(NAME##_, __LINE__))

// Internal macro for checking the type and range of a subfield definition.
#define HWREG_INTERNAL_CHECK_SUBFIELD(FIELD, BIT_HIGH, BIT_LOW)                    \
  static_assert(hwreg::internal::IsSupportedInt<                                   \
                    typename std::remove_reference<decltype(FIELD)>::type>::value, \
                #FIELD " has unsupported type");                                   \
  static_assert((BIT_HIGH) >= (BIT_LOW), "Upper bit goes before lower bit");       \
  static_assert((BIT_HIGH) < sizeof(decltype(FIELD)) * CHAR_BIT, "Upper bit is out of range");

namespace hwreg {

struct EnablePrinter;

namespace internal {

template <typename T>
struct IsSupportedInt : std::false_type {};
template <>
struct IsSupportedInt<uint8_t> : std::true_type {};
template <>
struct IsSupportedInt<uint16_t> : std::true_type {};
template <>
struct IsSupportedInt<uint32_t> : std::true_type {};
template <>
struct IsSupportedInt<uint64_t> : std::true_type {};

template <class IntType>
constexpr IntType ComputeMask(uint32_t num_bits) {
  if (num_bits == sizeof(IntType) * CHAR_BIT) {
    return static_cast<IntType>(~0ull);
  }
  return static_cast<IntType>((static_cast<IntType>(1) << num_bits) - 1);
}

class FieldPrinter {
 public:
  constexpr FieldPrinter() : name_(nullptr), bit_high_incl_(0), bit_low_(0) {}
  constexpr FieldPrinter(const char* name, uint32_t bit_high_incl, uint32_t bit_low)
      : name_(name), bit_high_incl_(bit_high_incl), bit_low_(bit_low) {}

  // Prints the field name, and the result of extracting the field from |value| in
  // hex (with a left-padding of zeroes to a length matching the maximum number of
  // nibbles needed to represent any value the field could take).
  void Print(uint64_t value, char* buf, size_t len) const;

  constexpr auto name() const { return name_; }
  constexpr auto bit_high_incl() const { return bit_high_incl_; }
  constexpr auto bit_low() const { return bit_low_; }

 private:
  const char* name_;
  uint32_t bit_high_incl_;
  uint32_t bit_low_;
};

// Structure used to reduce the storage cost of the pretty-printing features if
// they are not enabled.
template <bool Enabled, typename IntType>
struct FieldPrinterList {
  void AppendField(const char* name, uint32_t bit_high_incl, uint32_t bit_low) {}
};

template <typename IntType>
struct FieldPrinterList<true, IntType> {
  // These two members are used for implementing the Print() function above.
  // They will typically be optimized away if Print() is not used.
  std::array<FieldPrinter, sizeof(IntType) * CHAR_BIT> fields;
  unsigned num_fields = 0;

  void AppendField(const char* name, uint32_t bit_high_incl, uint32_t bit_low) {
    ZX_DEBUG_ASSERT(num_fields < fields.size());
    fields[num_fields++] = FieldPrinter(name, bit_high_incl, bit_low);
  }
};

template <bool PrinterEnabled, typename ValueType>
struct FieldParameters {
  ValueType rsvdz_mask = 0;
  ValueType fields_mask = 0;

  __NO_UNIQUE_ADDRESS FieldPrinterList<PrinterEnabled, ValueType> printer;
};

// Used to record information about a field at construction time.  This enables
// checking for overlapping fields and pretty-printing.
// The UnusedMarker argument is to give each Field member its own type.  This,
// combined with __NO_UNIQUE_ADDRESS, allows the compiler to use no storage
// for the Field members.
template <class RegType, typename UnusedMarker>
class Field {
 private:
  using IntType = typename RegType::ValueType;

 public:
  Field(FieldParameters<RegType::PrinterEnabled::value, IntType>* reg, const char* name,
        uint32_t bit_high_incl, uint32_t bit_low) {
    IntType mask = static_cast<IntType>(internal::ComputeMask<IntType>(bit_high_incl - bit_low + 1)
                                        << bit_low);
    // Check for overlapping bit ranges
    ZX_DEBUG_ASSERT((reg->fields_mask & mask) == 0ull);
    reg->fields_mask = static_cast<IntType>(reg->fields_mask | mask);

    reg->printer.AppendField(name, bit_high_incl, bit_low);
  }
};

// Used to record information about reserved-zero fields at construction time.
// This enables auto-zeroing of reserved-zero fields on register write.
// Represents a field that must be zeroed on write.
// The UnusedMarker argument is to give each RsvdZField member its own type.
// This, combined with __NO_UNIQUE_ADDRESS, allows the compiler to use no
// storage for the RsvdZField members.
template <class RegType, typename UnusedMarker>
class RsvdZField : public Field<RegType, UnusedMarker> {
 private:
  using IntType = typename RegType::ValueType;

 public:
  RsvdZField(FieldParameters<RegType::PrinterEnabled::value, IntType>* reg, uint32_t bit_high_incl,
             uint32_t bit_low)
      : Field<RegType, UnusedMarker>(reg, "RsvdZ", bit_high_incl, bit_low) {
    IntType mask = static_cast<IntType>(internal::ComputeMask<IntType>(bit_high_incl - bit_low + 1)
                                        << bit_low);
    reg->rsvdz_mask = static_cast<IntType>(reg->rsvdz_mask | mask);
  }
};

// Implementation for RegisterBase::Print, see the documentation there.
// |reg_value| is the current value of the register.
// |fields_mask| is a bitmask with a bit set for each bit that has been defined
// in the register.
template <typename F>
void PrintRegister(F print_fn, FieldPrinter fields[], size_t num_fields, uint64_t reg_value,
                   uint64_t fields_mask, int register_width_bytes) {
  char buf[128];
  for (unsigned i = 0; i < num_fields; ++i) {
    fields[i].Print(reg_value, buf, sizeof(buf));
    print_fn(buf);
  }

  // Check if any unknown bits are set, and if so let the caller know
  uint64_t val = reg_value & ~fields_mask;
  if (val != 0) {
    int pad_len = (register_width_bytes * CHAR_BIT) / 4;
    snprintf(buf, sizeof(buf), "unknown set bits: 0x%0*" PRIx64, pad_len, val);
    buf[sizeof(buf) - 1] = 0;
    print_fn(buf);
  }
}

// Utility for the common print function of [](const char* arg) { printf("%s\n", arg); }
void PrintRegisterPrintf(FieldPrinter fields[], size_t num_fields, uint64_t reg_value,
                         uint64_t fields_mask, int register_width_bytes);

// The std::visit implementation is quite complicated and works in a way that
// relies on constexpr arrays of function pointers.  This is not reliably
// compiled to pure PIC as is required in some contexts like phys executables.
//
// This Visit provides a limited implementation that is much simpler.  It's
// less general than std::visit in that it doesn't handle return values or
// multiple arguments of variant types.  The first argument to the function
// must be the variant type, and any other arguments are forwarded perfectly.
//
// Visit also has two additional features over std::visit.  It works in the
// degenerate case where the first argument is not a std::variant type.  It
// also works "recursively", i.e. if the selected variant is itself another
// std::variant type, then Visit acts on the selected inner variant.

template <typename T>
constexpr bool IsVariant = false;

template <typename... Variants>
constexpr bool IsVariant<std::variant<Variants...>> = true;

// Forward declaration.
template <typename F, typename V, typename... A, size_t... I>
void VisitEach(F&& f, V&& v, A&&... args, std::index_sequence<I...>);

template <typename F, typename V, typename... A>
void Visit(F&& f, V&& v, A&&... args) {
  if constexpr (IsVariant<std::decay_t<V>>) {
    constexpr auto n = std::variant_size_v<std::decay_t<V>>;
    VisitEach(std::forward<F>(f), std::forward<V>(v), std::forward<A>(args)...,
              std::make_index_sequence<n>());
  } else {
    f(std::forward<V>(v), std::forward<A>(args)...);
  }
}

// Helper template for Visit, needed so it can use a fold expression across
// the variant indices.  Forward the remaining arguments to a Visit call using
// the selected variant.
template <typename F, typename V, typename... A, size_t... I>
void VisitEach(F&& f, V&& v, A&&... args, std::index_sequence<I...>) {
  static_assert(sizeof...(I) == std::variant_size_v<std::decay_t<V>>);
  [[maybe_unused]] bool visited_one =  // Statically always true.
      ((v.index() == I
            // Exactly one of these Visit calls will be evaluated.
            ? (Visit(std::forward<F>(f), std::get<I>(std::forward<V>(v)), std::forward<A>(args)...),
               true)
            : false) ||
       ...);
  ZX_DEBUG_ASSERT(visited_one);
}

}  // namespace internal

}  // namespace hwreg

#endif  // HWREG_INTERNAL_H_
