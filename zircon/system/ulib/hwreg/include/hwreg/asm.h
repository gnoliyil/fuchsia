// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HWREG_ASM_H_
#define HWREG_ASM_H_

#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>

#include <hwreg/bitfields.h>

namespace hwreg {

// A `RegisterBase` "PrinterState" parameter that permits the generation of
// assembly constants from the fields of derived, register classes (see below).
//
// We define an alias for clarity in contexts in which enabling "print"ing
// would not seem relevant to these ends.
//
// TODO(fxbug.dev/68404): Use at your own risk.
using EnableAsmGeneration = EnablePrinter;

// This can be used in fluid style to define macros directly for constants,
// or to define macros derived from an hwreg::RegisterBase subclass, e.g.:
// ```
// int main(int argc, char**) {
//   return hwreg::AsmHeader()
//       .Register<FooReg>("FOO_")
//       .Macro("FOO_BAR_VAL", FooReg::kBarVal)
//       .Main(argc, argv);
// }
// ```
// See //zircon/system/ulib/hwreg/hwreg_asm_header.gni to easily plug such a
// header generator program into the build.

class AsmHeader {
 public:
  // Emit a raw line to the header by concatenating the strings.
  AsmHeader& Line(std::initializer_list<std::string_view> strings);

  // Emit a fixed macro definition.
  AsmHeader& Macro(std::string_view name, std::string_view value);

  // Same but for integer values.
  AsmHeader& Macro(std::string_view name, uint64_t value);

  // Emit a macro for each field in the register, plus a macro for the mask
  // of reserved-zero bits and a macro for the mask of unknown bits.
  // T is required to inherit from a `RegisterBase` with `EnableAsmGeneration`.
  template <typename T>
  AsmHeader& Register(std::string_view prefix) {
    T{}.ForEachField(
        [this, prefix](const char* name, auto value, auto bit_high_incl, auto bit_low) {
          if (name) {
            FieldMacro(prefix, name, bit_high_incl, bit_low);
          }
        });
    RegisterMacros(prefix, T{}.rsvdz_mask(), T{}.fields_mask(), ~T{}.fields_mask());
    return *this;
  }

  // Format the contents of a header file with the accumulated definitions.
  // The header should be the #include "name" for the header, which is
  // translated into its guard symbol.
  std::string Output(std::string_view include_name);

  // Write out the accumulated definitions to the file, not touching it if
  // it hasn't changed.  Returns 0 on success or an errno code for failure
  // to write the file.
  int Output(const char* filename, std::string_view include_name);

  // Parse two command line arguments for filename and include_name and
  // write the file.  Returns exit status and prints errors to stderr.
  int Main(int argc, char** argv);

 private:
  std::string body_;

  void FieldMacro(std::string_view prefix, const char* name, uint32_t bit_high_incl,
                  uint32_t bit_low);
  void RegisterMacros(std::string_view prefix, uint64_t rsvdz, uint64_t known, uint64_t unknown);
};

}  // namespace hwreg

#endif  // HWREG_ASM_H_
