// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/elfldltl/diagnostics.h>
#include <lib/elfldltl/posix.h>

#ifdef __Fuchsia__
#include <lib/elfldltl/zircon.h>
#endif

#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tests.h"

namespace {

TEST(ElfldltlDiagnosticsTests, PrintfDiagnosticsReport) {
  std::array<char, 200> buffer{};
  auto printer = [&buffer](const char* format, auto&&... args) {
    snprintf(buffer.data(), buffer.size(), format, std::forward<decltype(args)>(args)...);
  };

  constexpr uint32_t kPrefixValue = 42;
  constexpr std::string_view kPrefixStringView = ": ";
  auto report =
      elfldltl::PrintfDiagnosticsReport(printer, "prefix", kPrefixValue, kPrefixStringView);

  constexpr std::string_view kStringViewArg = "foo";
  constexpr uint32_t kValue32 = 123;
  constexpr uint64_t kValue64 = 456;
  constexpr uint32_t kOffset32 = 0x123;
  constexpr uint64_t kOffset64 = 0x456;
  constexpr uint32_t kAddress32 = 0x1234;
  constexpr uint64_t kAddress64 = 0x4567;
  decltype(auto) retval =
      report(kStringViewArg, kValue32, "bar", kValue64, elfldltl::FileOffset{kOffset32},
             elfldltl::FileOffset{kOffset64}, elfldltl::FileAddress{kAddress32},
             elfldltl::FileAddress{kAddress64});

  static_assert(std::is_same_v<decltype(retval), bool>);
  EXPECT_TRUE(retval);

  ASSERT_EQ(buffer.back(), '\0');
  EXPECT_STREQ(
      "prefix 42: foo 123bar 456"
      " at file offset 0x123 at file offset 0x456"
      " at relative address 0x1234 at relative address 0x4567",
      buffer.data());
}

// clang-format off
// clang-format is all confused for some reason
TEST(ElfldltlDiagnosticsTests, PrintfDiagnosticsReportSystemErrors) {
  {
    std::array<char, 50> buffer{};
    auto printer = [&buffer](const char* format, auto&&... args) {
      snprintf(buffer.data(), buffer.size(), format, std::forward<decltype(args)>(args)...);
    };

    auto report = elfldltl::PrintfDiagnosticsReport(printer);
    ASSERT_TRUE(report(elfldltl::PosixError{EPERM}));

    ASSERT_EQ(buffer.back(), '\0');
    EXPECT_STREQ(strerror(EPERM), buffer.data());
  }
#ifdef __Fuchsia__
  {
    std::array<char, 50> buffer{};
    auto printer = [&buffer](const char* format, auto&&... args) {
      snprintf(buffer.data(), buffer.size(), format, std::forward<decltype(args)>(args)...);
    };

    auto report = elfldltl::PrintfDiagnosticsReport(printer);
    ASSERT_TRUE(report(elfldltl::ZirconError{ZX_ERR_NOT_SUPPORTED}));

    ASSERT_EQ(buffer.back(), '\0');
    EXPECT_STREQ(zx_status_get_string(ZX_ERR_NOT_SUPPORTED), buffer.data());
  }
#endif
}
// clang-format on

TEST(ElfldltlDiagnosticsTests, Trap) {
  auto diag = elfldltl::TrapDiagnostics();

  EXPECT_EQ(1u, diag.errors());
  ASSERT_DEATH(diag.FormatError("errors are fatal"), "");

  EXPECT_EQ(1u, diag.warnings());
  ASSERT_DEATH(diag.FormatWarning("warnings are fatal"), "");
}

TEST(ElfldltlDiagnosticsTests, Panic) {
  auto diag = elfldltl::PanicDiagnostics();

  EXPECT_EQ(1u, diag.errors());
  ASSERT_DEATH(diag.FormatError("errors are fatal"), "");

  EXPECT_EQ(1u, diag.warnings());
  ASSERT_DEATH(diag.FormatWarning("warnings are fatal"), "");
}

TEST(ElfldltlDiagnosticsTests, OneString) {
  std::string error = "no error";
  auto diag = elfldltl::OneStringDiagnostics(error);

  EXPECT_FALSE(diag.FormatError("first error"));
  EXPECT_EQ(error, "first error");
  EXPECT_EQ(1u, diag.errors());

  EXPECT_FALSE(diag.FormatError("second error"));
  EXPECT_EQ(error, "second error");
  EXPECT_EQ(2u, diag.errors());

  EXPECT_FALSE(diag.FormatWarning("warning"));
  EXPECT_EQ(error, "warning");
  EXPECT_EQ(1u, diag.warnings());
  EXPECT_EQ(2u, diag.errors());
}

TEST(ElfldltlDiagnosticsTests, CollectStrings) {
  std::vector<std::string> errors;
  const elfldltl::DiagnosticsFlags flags = {.multiple_errors = true};
  auto diag = elfldltl::CollectStringsDiagnostics(errors, flags);

  EXPECT_EQ(0u, diag.errors());
  EXPECT_EQ(0u, diag.warnings());

  EXPECT_TRUE(diag.FormatError("first error"));
  EXPECT_EQ(1u, errors.size());
  EXPECT_EQ(0u, diag.warnings());
  EXPECT_EQ(1u, diag.errors());

  EXPECT_TRUE(diag.FormatError("second error"));
  EXPECT_EQ(2u, errors.size());
  EXPECT_EQ(0u, diag.warnings());
  EXPECT_EQ(2u, diag.errors());

  EXPECT_TRUE(diag.FormatWarning("warning"));
  EXPECT_EQ(3u, errors.size());
  EXPECT_EQ(1u, diag.warnings());
  EXPECT_EQ(2u, diag.errors());

  ASSERT_GE(errors.size(), 3u);
  EXPECT_EQ(errors[0], "first error");
  EXPECT_EQ(errors[1], "second error");
  EXPECT_EQ(errors[2], "warning");
}

TEST(ElfldltlDiagnosticsTests, Ostream) {
  std::stringstream sstr;
  const elfldltl::DiagnosticsFlags flags = {.multiple_errors = true};
  auto diag = elfldltl::OstreamDiagnostics(sstr, flags, 'a', 1, ":");

  EXPECT_EQ(0u, diag.errors());
  EXPECT_EQ(0u, diag.warnings());

  EXPECT_TRUE(diag.FormatError("first error"));
  EXPECT_EQ(1u, diag.errors());

  EXPECT_TRUE(diag.FormatError("second error"));
  EXPECT_EQ(2u, diag.errors());

  EXPECT_TRUE(diag.FormatWarning("warning"));
  EXPECT_EQ(1u, diag.warnings());
  EXPECT_EQ(2u, diag.errors());

  EXPECT_EQ(sstr.str(),
            "a1:first error\n"
            "a1:second error\n"
            "a1:warning\n");
}

template <size_t... Args>
auto CreateExpect(std::index_sequence<Args...>) {
  return ExpectedSingleError{"error ", Args...};
}

template <typename Diag, size_t... Args>
auto CreateError(Diag& diag, std::index_sequence<Args...>) {
  diag.FormatError("error ", Args...);
}

TEST(ElfldltlDiagnosticsTests, FormatErrorVariadic) {
  {
    ExpectedSingleError expected("abc ", 123ull, " --- ", 45678910);
    expected.diag().FormatError("abc ", 123ull, " --- ", 45678910);
  }
  {
    auto expected = CreateExpect(std::make_index_sequence<20>{});
    CreateError(expected.diag(), std::make_index_sequence<20>{});
  }
}

TEST(ElfldltlDiagnosticsTests, ResourceLimit) {
  {
    ExpectedSingleError expected("error", ": maximum 501 < requested ", 723);
    expected.diag().ResourceLimit<501>("error", 723);
  }
}

TEST(ElfldltlDiagnosticsTests, SystemError) {
  {
    ExpectedSingleError expected("error", elfldltl::PosixError{EPERM});
    expected.diag().SystemError("error", elfldltl::PosixError{EPERM});
  }
#ifdef __Fuchsia__
  {
    ExpectedSingleError expected("error", elfldltl::ZirconError{ZX_ERR_NOT_SUPPORTED});
    expected.diag().SystemError("error", elfldltl::ZirconError{ZX_ERR_NOT_SUPPORTED});
  }
#endif
}

}  // namespace
