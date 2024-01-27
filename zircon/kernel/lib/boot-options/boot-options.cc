// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <ctype.h>
#include <inttypes.h>
#include <lib/boot-options/boot-options.h>
#include <lib/boot-options/types.h>
#include <lib/boot-options/word-view.h>
#include <lib/stdcompat/algorithm.h>
#include <zircon/compiler.h>

namespace {

template <auto MemberPtr>
constexpr auto kDefaultValue = MemberPtr;

// Provides constants for the default value of each member.
#define DEFINE_OPTION(name, type, member, init, doc) \
  template <>                                        \
  constexpr type kDefaultValue<&BootOptions::member> init;
#include <lib/boot-options/options.inc>
#undef DEFINE_OPTION

#include "enum.h"

}  // namespace

const BootOptions* gBootOptions = nullptr;

using namespace std::string_view_literals;

// This avoids libc++ functions the kernel can't use, and avoids strtoul so as
// not to require NUL termination.
//
// TODO(fxbug.dev/62052): Reconsider the overflow policy below.
std::optional<int64_t> BootOptions::ParseInt(std::string_view value, std::string_view* rest) {
  int64_t neg = 1;
  if (value.substr(0, 1) == "-") {
    neg = -1;
    value.remove_prefix(1);
  } else if (value.substr(0, 1) == "+") {
    value.remove_prefix(1);
  }

  std::optional<int64_t> result;
  auto from_chars = [neg, rest, &result](std::string_view prefix, int base, std::string_view value,
                                         bool trim = false) {
    if (value.substr(0, prefix.size()) == prefix) {
      if (trim) {
        value.remove_prefix(prefix.size());
      }
      if (value.empty()) {
        return false;
      }
      int64_t result_value = 0;
      for (char c : value) {
        std::optional<unsigned int> digit;
        switch (c) {
          case '0' ... '9':
            if (c - '0' < base) {
              digit = c - '0';
            }
            break;
          case 'a' ... 'f':
            if (base == 16) {
              digit = c - 'a' + 10;
            }
            break;
        }
        if (digit) {
          mul_overflow(result_value, base, &result_value);
          add_overflow(result_value, *digit, &result_value);
          value.remove_prefix(1);
        } else if (rest) {
          break;
        } else {
          return false;
        }
      }
      if (rest) {
        *rest = value;
      }
      mul_overflow(result_value, neg, &result_value);
      result = result_value;  // Finally set the result.
      return true;
    }
    return false;
  };

  from_chars("0x", 16, value, true) || from_chars("0", 8, value) || from_chars("", 10, value);
  return result;
}

namespace {

constexpr char kComplainPrefix[] = "kernel";

// This names an arbitrary index for each member.  This explicit indirection
// avoids having a constexpr table of string_view constants, which doesn't fly
// for pure PIC.
enum class Index {
#define DEFINE_OPTION(name, type, member, init, doc) member,
#include <lib/boot-options/options.inc>
#undef DEFINE_OPTION
};

// Map Index::member to the name for BootOptions::member.  The compiler may
// optimize this back into a table lookup with an array of string_view
// constants, but that optimization is disabled when pure PIC is required.
constexpr std::string_view OptionName(Index idx) {
  switch (idx) {
#define DEFINE_OPTION(name, type, member, init, doc) \
  case Index::member:                                \
    return name##sv;
#include <lib/boot-options/options.inc>
#undef DEFINE_OPTION
  }
  return {};
}

// This overload lets the generic lambda below work for both cases.
constexpr std::string_view OptionName(std::string_view name) { return name; }

// Compare option names, using either string_view or Index.
constexpr auto OptionLessThan = [](auto&& a, auto&& b) { return OptionName(a) < OptionName(b); };

constexpr auto CheckSortedNames = [](const auto& names) {
  return cpp20::is_sorted(names.begin(), names.end(), OptionLessThan);
};

// kSortedNames lists Index values in ascending lexicographic order of name.
constexpr auto kSortedNames = []() {
  std::array names{
#define DEFINE_OPTION(name, type, member, init, doc) Index::member,
#include <lib/boot-options/options.inc>
#undef DEFINE_OPTION
  };
  cpp20::sort(names.begin(), names.end(), OptionLessThan);
  return names;
}();

static_assert(CheckSortedNames(kSortedNames));

// Map option name to Index using binary search.
std::optional<Index> FindOption(std::string_view name) {
  if (auto it = std::lower_bound(kSortedNames.begin(), kSortedNames.end(), name, OptionLessThan);
      it != kSortedNames.end() && name == OptionName(*it)) {
    return *it;
  }
  return std::nullopt;
}

// The length of the longest option name.
constexpr size_t kMaxNameLen =
    OptionName(*std::max_element(kSortedNames.begin(), kSortedNames.end(), [](Index a, Index b) {
      return OptionName(a).size() < OptionName(b).size();
    })).size();

template <Index member, typename T>
void ShowOption(const T& value, bool defaults, FILE* out);

#define DEFINE_OPTION(name, type, member, init, doc)                                  \
  template <>                                                                         \
  void ShowOption<Index::member, type>(const type& value, bool defaults, FILE* out) { \
    const type default_value init;                                                    \
    BootOptions::Print(OptionName(Index::member), value, out);                        \
    if (defaults) {                                                                   \
      fprintf(out, " (default ");                                                     \
      BootOptions::Print(OptionName(Index::member), default_value, out);              \
      fprintf(out, ")\n");                                                            \
    } else {                                                                          \
      fprintf(out, "\n");                                                             \
    }                                                                                 \
  }
#include <lib/boot-options/options.inc>
#undef DEFINE_OPTION

}  // namespace

BootOptions::WordResult BootOptions::ParseWord(std::string_view word) {
  std::string_view key, value;
  if (auto eq = word.find('='); eq == std::string_view::npos) {
    // No '=' means the whole word is the key, with an empty value.
    key = word;
  } else {
    key = word.substr(0, eq);
    value = word.substr(eq + 1);
  }

  // Match the key against the known option names.
  // Note this leaves the member with its current/default value but still
  // returns true when the key was known but the value was unparsable.
  if (auto option = FindOption(key)) {
    switch (*option) {
#define DEFINE_OPTION(name, type, member, init, doc)      \
  case Index::member:                                     \
    if (!Parse(value, &BootOptions::member)) {            \
      this->member = kDefaultValue<&BootOptions::member>; \
    }                                                     \
    break;
#include <lib/boot-options/options.inc>
#undef DEFINE_OPTION
    }
    return {key, true};
  }

  return {key, false};
}

void BootOptions::SetMany(std::string_view cmdline, FILE* complain) {
  bool verbose = complain != nullptr;
  if (!complain) {
    complain = stdout;
  }
  for (auto word : WordView(cmdline)) {
    if (auto result = ParseWord(word);
        !result.known &&
        (verbose ||
         result.key.substr(0, std::string_view(kComplainPrefix).size()) == kComplainPrefix)) {
      if (result.key.size() > kMaxNameLen) {
        fprintf(complain, "NOTE: Unrecognized kernel option %zu characters long (max %zu)\n",
                result.key.size(), kMaxNameLen);
      } else {
        char name[kMaxNameLen + 1];
        name[SanitizeString(name, kMaxNameLen, result.key)] = '\0';
        fprintf(complain, "WARN: Kernel ignored unrecognized option '%s'\n", name);
      }
    }
  }

// Set smp_max_cpus to the arch specific value.
#if defined(__x86_64__)
  smp_max_cpus = x86_smp_max_cpus;
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
  smp_max_cpus = arm64_smp_max_cpus;
#endif
}

int BootOptions::Show(std::string_view key, bool defaults, FILE* out) const {
  if (auto option = FindOption(key)) {
    switch (*option) {
#define DEFINE_OPTION(name, type, member, init, doc)        \
  case Index::member:                                       \
    ShowOption<Index::member, type>(member, defaults, out); \
    break;
#include <lib/boot-options/options.inc>
#undef DEFINE_OPTION
    }
    return 0;
  }
  return -1;
}

void BootOptions::Show(bool defaults, FILE* out) const {
#define DEFINE_OPTION(name, type, member, init, doc) \
  ShowOption<Index::member, type>(member, defaults, out);
#include <lib/boot-options/options.inc>
#undef DEFINE_OPTION
}

// Helpers for BootOptions::Parse overloads below.

namespace {

template <typename T>
bool ParseIntValue(std::string_view value, T& result) {
  if (auto parsed = BootOptions::ParseInt(value)) {
    result = static_cast<T>(*parsed);
    return true;
  }
  return false;
}

template <typename... Driver>
struct UartParser {
  uart::all::Driver& result_;

  template <typename OneDriver>
  bool MaybeCreate(std::string_view value) {
    if (auto driver = OneDriver::MaybeCreate(value)) {
      result_ = std::move(*driver);
      return true;
    }
    return false;
  }

  bool Parse(std::string_view value) { return (MaybeCreate<Driver>(value) || ... || false); }
};

}  // namespace

// Overloads for various types.

bool BootOptions::Parse(std::string_view value, bool BootOptions::*member) {
  // Any other value, even an empty value, means true.
  this->*member = value != "false" && value != "0" && value != "off";
  return true;
}

void BootOptions::PrintValue(const bool& value, FILE* out) {
  fprintf(out, "%s", value ? "true" : "false");
}

bool BootOptions::Parse(std::string_view value, uint64_t BootOptions::*member) {
  return ParseIntValue(value, this->*member);
}

void BootOptions::PrintValue(const uint64_t& value, FILE* out) { fprintf(out, "%#" PRIx64, value); }

bool BootOptions::Parse(std::string_view value, uint32_t BootOptions::*member) {
  return ParseIntValue(value, this->*member);
}

void BootOptions::PrintValue(const uint32_t& value, FILE* out) { fprintf(out, "%#" PRIx32, value); }

bool BootOptions::Parse(std::string_view value, uint8_t BootOptions::*member) {
  return ParseIntValue(value, this->*member);
}

void BootOptions::PrintValue(const uint8_t& value, FILE* out) { fprintf(out, "%#" PRIx8, value); }

bool BootOptions::Parse(std::string_view value, SmallString BootOptions::*member) {
  SmallString& result = this->*member;
  size_t wrote = value.copy(result.data(), result.size());
  // In the event of a value of size greater or equal to SmallString's capacity,
  // truncate to keep invariant that the string is NUL-terminated.
  result[std::min(wrote, result.size() - 1)] = '\0';
  return true;
}

void BootOptions::PrintValue(const SmallString& value, FILE* out) {
  ZX_ASSERT(value.back() == '\0');
  fprintf(out, "%s", value.data());
}

bool BootOptions::Parse(std::string_view value, RedactedHex BootOptions::*member) {
  RedactedHex& result = this->*member;
  if (std::all_of(value.begin(), value.end(), isxdigit)) {
    result.len = value.copy(result.hex.data(), result.hex.size());
    Redact(value);
  };
  return true;
}

void BootOptions::PrintValue(const RedactedHex& value, FILE* out) {
  if (value.len > 0) {
    fprintf(out, "<redacted.%zu.hex.chars>", value.len);
  }
}

bool BootOptions::Parse(std::string_view value, uart::all::Driver BootOptions::*member) {
  if (!uart::all::WithAllDrivers<UartParser>{this->*member}.Parse(value)) {
    // Probably has nowhere to go, but anyway.
    printf("WARN: Unrecognized serial console setting '%.*s' ignored\n",
           static_cast<int>(value.size()), value.data());
    return false;
  }
  return true;
}

void BootOptions::PrintValue(const uart::all::Driver& value, FILE* out) {
  std::visit([out](const auto& uart) { uart.Unparse(out); }, value);
}

bool BootOptions::Parse(std::string_view value, OomBehavior BootOptions::*member) {
  return Enum<OomBehavior>(EnumParser{value, &(this->*member)}).Check();
}

void BootOptions::PrintValue(const OomBehavior& value, FILE* out) {
  Enum<OomBehavior>(EnumPrinter{value, out});
}

bool BootOptions::Parse(std::string_view value, EntropyTestSource BootOptions::*member) {
  return Enum<EntropyTestSource>(EnumParser{value, &(this->*member)}).Check();
}

void BootOptions::PrintValue(const EntropyTestSource& value, FILE* out) {
  Enum<EntropyTestSource>(EnumPrinter{value, out});
}

bool BootOptions::Parse(std::string_view value, PageTableEvictionPolicy BootOptions::*member) {
  return Enum<PageTableEvictionPolicy>(EnumParser{value, &(this->*member)}).Check();
}

void BootOptions::PrintValue(const PageTableEvictionPolicy& value, FILE* out) {
  Enum<PageTableEvictionPolicy>(EnumPrinter{value, out});
}

bool BootOptions::Parse(std::string_view value, GfxConsoleFont BootOptions::*member) {
  return Enum<GfxConsoleFont>(EnumParser{value, &(this->*member)}).Check();
}

void BootOptions::PrintValue(const GfxConsoleFont& value, FILE* out) {
  Enum<GfxConsoleFont>(EnumPrinter{value, out});
}

bool BootOptions::Parse(std::string_view value, SerialDebugSyscalls BootOptions::*member) {
  return Enum<SerialDebugSyscalls>(EnumParser{value, &(this->*member)}).Check();
}

void BootOptions::PrintValue(const SerialDebugSyscalls& value, FILE* out) {
  Enum<SerialDebugSyscalls>(EnumPrinter{value, out});
}

bool BootOptions::Parse(std::string_view value, RootJobBehavior BootOptions::*member) {
  return Enum<RootJobBehavior>(EnumParser{value, &(this->*member)}).Check();
}

void BootOptions::PrintValue(const RootJobBehavior& value, FILE* out) {
  Enum<RootJobBehavior>(EnumPrinter{value, out});
}

bool BootOptions::Parse(std::string_view value, WallclockType BootOptions::*member) {
  return Enum<WallclockType>(EnumParser{value, &(this->*member)}).Check();
}

void BootOptions::PrintValue(const WallclockType& value, FILE* out) {
  Enum<WallclockType>(EnumPrinter{value, out});
}

bool BootOptions::Parse(std::string_view value, CompressionStrategy BootOptions::*member) {
  return Enum<CompressionStrategy>(EnumParser{value, &(this->*member)}).Check();
}

void BootOptions::PrintValue(const CompressionStrategy& value, FILE* out) {
  Enum<CompressionStrategy>(EnumPrinter{value, out});
}

bool BootOptions::Parse(std::string_view value, CompressionStorageStrategy BootOptions::*member) {
  return Enum<CompressionStorageStrategy>(EnumParser{value, &(this->*member)}).Check();
}

void BootOptions::PrintValue(const CompressionStorageStrategy& value, FILE* out) {
  Enum<CompressionStorageStrategy>(EnumPrinter{value, out});
}

bool BootOptions::Parse(std::string_view value,
                        std::optional<RamReservation> BootOptions::*member) {
  if (value.empty()) {
    this->*member = std::nullopt;
  } else if (auto size = ParseInt(value, &value); !size) {
    return false;
  } else {
    RamReservation ram = {.size = static_cast<uint64_t>(*size)};
    if (!value.empty()) {
      if (value[0] != ',') {
        return false;
      }
      auto paddr = ParseInt(value.substr(1));
      if (!paddr) {
        return false;
      }
      ram.paddr = static_cast<uint64_t>(*paddr);
    }
    this->*member = ram;
  }
  return true;
}

void BootOptions::PrintValue(const std::optional<RamReservation>& value, FILE* out) {
  if (value) {
    fprintf(out, "%#" PRIx64, value->size);
    if (value->paddr) {
      fprintf(out, ",%#" PRIx64, *value->paddr);
    }
  }
}

#if BOOT_OPTIONS_TESTONLY_OPTIONS

bool BootOptions::Parse(std::string_view value, TestEnum BootOptions::*member) {
  return Enum<TestEnum>(EnumParser{value, &(this->*member)}).Check();
}

void BootOptions::PrintValue(const TestEnum& value, FILE* out) {
  Enum<TestEnum>(EnumPrinter{value, out});
}

bool BootOptions::Parse(std::string_view value, TestStruct BootOptions::*member) {
  if (value != "test") {
    printf("WARN: Ignored unknown value '%.*s' for test option\n", static_cast<int>(value.size()),
           value.data());
    return false;
  }

  (this->*member).present = true;
  return true;
}

void BootOptions::PrintValue(const TestStruct& value, FILE* out) { fprintf(out, "test"); }

#endif  // BOOT_OPTIONS_TESTONLY_OPTIONS

#if BOOT_OPTIONS_GENERATOR || defined(__aarch64__)

bool BootOptions::Parse(std::string_view value, Arm64PhysPsciReset BootOptions::*member) {
  return Enum<Arm64PhysPsciReset>(EnumParser{value, &(this->*member)}).Check();
}

void BootOptions::PrintValue(const Arm64PhysPsciReset& value, FILE* out) {
  Enum<Arm64PhysPsciReset>(EnumPrinter{value, out});
}

#endif  // BOOT_OPTIONS_GENERATOR || defined(__aarch64__)

#if BOOT_OPTIONS_GENERATOR || defined(__x86_64__)

bool BootOptions::Parse(std::string_view value, IntelHwpPolicy BootOptions::*member) {
  return Enum<IntelHwpPolicy>(EnumParser{value, &(this->*member)}).Check();
}

void BootOptions::PrintValue(const IntelHwpPolicy& value, FILE* out) {
  Enum<IntelHwpPolicy>(EnumPrinter{value, out});
}

#endif  // BOOT_OPTIONS_GENERATOR || defined(__x86_64__)
