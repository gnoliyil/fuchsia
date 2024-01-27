// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fit/defer.h>

#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <hwreg/asm.h>

namespace hwreg {

int AsmHeader::Main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s OUTPUT_FILE INCLUDE_NAME\n", argv[0]);
    return 1;
  }
  const char* filename = argv[1];
  const char* include_name = argv[2];
  int error = Output(filename, include_name);
  if (error) {
    fprintf(stderr, "%s: %s\n", filename, strerror(error));
    return 1;
  }
  return 0;
}

std::string AsmHeader::Output(std::string_view include_name) {
  std::string guard("_");
  guard += include_name;
  for (auto& c : guard) {
    c = std::isalnum(c) ? static_cast<char>(std::toupper(c)) : '_';
  }
  guard += '_';

  std::string contents("// This file is generated.  DO NOT EDIT!\n\n");
  contents += "#ifndef " + guard + '\n';
  contents += "#define " + guard + " 1\n";
  contents += '\n';
  contents += body_;
  contents += '\n';
  contents += "#endif  // " + guard + '\n';

  return contents;
}

int AsmHeader::Output(const char* filename, std::string_view include_name) {
  std::string contents = Output(include_name);

  // Return early if the file exists and matches the contents.
  if (FILE* f = fopen(filename, "r")) {
    auto cleanup = fit::defer([f]() { fclose(f); });
    if (fseek(f, 0, SEEK_END) == 0) {
      long int pos = ftell(f);
      if (pos > 0) {
        std::string old;
        old.resize(pos);
        rewind(f);
        fread(old.data(), 1, pos, f);
        if (!ferror(f) && old == contents) {
          return 0;
        }
      }
    }
  }

  // Write the file out.
  if (FILE* outf = fopen(filename, "w")) {
    fwrite(contents.data(), 1, contents.size(), outf);
    if (!ferror(outf) && fclose(outf) == 0) {
      return 0;
    }
  }
  return errno;
}

AsmHeader& AsmHeader::Line(std::initializer_list<std::string_view> strings) {
  for (std::string_view str : strings) {
    body_ += str;
  }
  body_ += '\n';
  return *this;
}

AsmHeader& AsmHeader::Macro(std::string_view name, std::string_view value) {
  body_ += "#define ";
  body_ += name;
  body_ += ' ';
  body_ += value;
  body_ += '\n';
  return *this;
}

AsmHeader& AsmHeader::Macro(std::string_view name, uint64_t value) {
  char buf[2 + std::numeric_limits<uint64_t>::digits10 + 1];
  snprintf(buf, sizeof(buf), "%#" PRIx64, value);
  return Macro(name, buf);
}

void AsmHeader::FieldMacro(std::string_view prefix, const char* field_name, uint32_t bit_high_incl,
                           uint32_t bit_low) {
  // Compose the name with the prefix and upcase.
  std::string name(prefix);
  name += field_name;
  for (auto& c : name) {
    c = static_cast<char>(std::toupper(c));
  }

  // The macro under the field's unadorned name is its unshifted mask.
  Macro(name, internal::ComputeMask<uint64_t>(bit_high_incl - bit_low + 1) << bit_low);

  if (bit_high_incl == bit_low) {
    // Single bits also get a bit-number macro (rendered in decimal).
    Macro(name + "_BIT", std::to_string(bit_low));
  } else {
    // Larger fields get a pair of _SHIFT and _MASK macros too.
    Macro(name + "_SHIFT", std::to_string(bit_low));
    Macro(name + "_MASK", internal::ComputeMask<uint64_t>(bit_high_incl - bit_low + 1));

    // A convenience function-like macro uses the shift and mask macros.
    Macro(name + "_FIELD(x)", "(((x) & " + name + "_MASK) << " + name + "_SHIFT)");
  }
}

void AsmHeader::RegisterMacros(std::string_view prefix, uint64_t rsvdz, uint64_t known,
                               uint64_t unknown) {
  std::string name(prefix);
  for (auto& c : name) {
    c = static_cast<char>(std::toupper(c));
  }
  if (rsvdz != 0) {
    Macro(name + "RSVDZ", rsvdz);
  }
  if (known != 0) {
    Macro(name + "MASK", known);
  }
  if (unknown != 0) {
    Macro(name + "UNKNOWN", unknown);
  }
}

}  // namespace hwreg
