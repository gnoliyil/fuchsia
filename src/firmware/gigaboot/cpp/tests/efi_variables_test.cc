// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "efi_variables.h"

#include <lib/efi/testing/stub_boot_services.h>
#include <lib/efi/testing/stub_runtime_services.h>

#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <fbl/vector.h>
#include <gtest/gtest.h>
#include <phys/efi/main.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xefi.h"

using ::efi::MatchGuid;
using ::efi::MockBootServices;
using ::efi::MockRuntimeServices;
using ::efi::StubRuntimeServices;
using ::testing::_;
using ::testing::ContainerEq;
using ::testing::ElementsAreArray;
using ::testing::EndsWith;
using ::testing::Eq;
using ::testing::Ge;
using ::testing::InSequence;
using ::testing::IsEmpty;
using ::testing::MatchesRegex;
using ::testing::NiceMock;
using ::testing::NotNull;
using ::testing::Pointee;
using ::testing::Return;
using ::testing::StrEq;
using ::testing::Test;

const efi_handle kImageHandle = reinterpret_cast<efi_handle>(0x10);

namespace fbl {
template <typename T>
inline bool operator==(const fbl::Vector<T>& a, const fbl::Vector<T>& b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end());
}

template <typename T>
inline bool operator!=(const fbl::Vector<T>& a, const fbl::Vector<T>& b) {
  return !(a == b);
}
}  // namespace fbl

namespace gigaboot {

fbl::Vector<char> EfiCString(const std::string_view sv);
fbl::Vector<char16_t> EfiCString(const std::u16string_view sv);
fbl::Vector<char16_t> EfiCString(const fbl::Vector<char16_t>& v);

TEST(EfiVariableId, DefaultInvalid) { EXPECT_FALSE(EfiVariables::EfiVariableId().IsValid()); }

TEST(EfiVariableId, InvalidateResult) {
  EfiVariables::EfiVariableId id;
  id.Invalidate();
  EXPECT_FALSE(id.IsValid());
}

class EfiVariableIdFixture
    : public ::testing::TestWithParam<std::list<EfiVariables::EfiVariableId>> {};

TEST_P(EfiVariableIdFixture, Eq) {
  std::list<EfiVariables::EfiVariableId> input = GetParam();
  for (const auto& a : input) {
    EXPECT_EQ(a, a);
  }
}

TEST_P(EfiVariableIdFixture, Ne) {
  std::list<EfiVariables::EfiVariableId> input = GetParam();
  for (auto a = input.begin(); a != input.end(); a++) {
    for (auto b = std::next(a); b != input.end(); b++) {
      EXPECT_NE(*a, *b);
    }
  }
}

constexpr efi_guid kEfiGuid0 = {0x0, 0x0, 0x0, {0x0}};
INSTANTIATE_TEST_SUITE_P(EfiVariableIdTest, EfiVariableIdFixture,
                         ::testing::Values(std::list<EfiVariables::EfiVariableId>{
                             {EfiCString(u""), kEfiGuid0},
                             {EfiCString(u""), {0x1, 0x0, 0x0, {0x0}}},
                             {EfiCString(u"var0"), kEfiGuid0},
                             {EfiCString(u"var0"), {0x1, 0x0, 0x0, {0x0}}},
                             {EfiCString(u"var0"), {0x0, 0x1, 0x0, {0x0}}},
                             {EfiCString(u"var0"), {0x0, 0x0, 0x1, {0x0}}},
                             {EfiCString(u"var0"), {0x0, 0x0, 0x1, {0x1}}},
                             {EfiCString(u"var1"), kEfiGuid0},
                             {EfiCString(u"abcDEF123!@#"), kEfiGuid0},
                             {EfiCString(u"a\tb\nc\x0001\x0002"), kEfiGuid0}}));

TEST(EfiVariablesUcs2ToStr, EmptyStringView) {
  auto res = EfiVariables::Ucs2ToStr(u"");
  ASSERT_TRUE(res.is_error());
}

TEST(EfiVariablesUcs2ToStr, NotCString) {
  const std::u16string input(u"abc");

  auto res = EfiVariables::Ucs2ToStr(input);

  ASSERT_TRUE(res.is_error());
}

TEST(EfiVariablesUcs2ToStr, LongString) {
  constexpr size_t kCharNumber = 4096;
  std::u16string input(kCharNumber, u'a');
  input.push_back('\0');
  const std::string expected(kCharNumber, 'a');

  auto res = EfiVariables::Ucs2ToStr(input);

  ASSERT_TRUE(res.is_ok());
  EXPECT_EQ(res.value().data(), expected);
}

template <typename T>
fbl::Vector<T> copy(const fbl::Vector<T>& src) {
  fbl::Vector<T> res;
  res.resize(src.size());
  std::copy(src.begin(), src.end(), res.begin());
  return res;
}

struct Ucs2Str {
  fbl::Vector<char16_t> ucs2;
  fbl::Vector<char> str;

  Ucs2Str(const fbl::Vector<char16_t>& ucs2_in, const fbl::Vector<char>& str_in)
      : ucs2(copy(ucs2_in)), str(copy(str_in)) {}
  Ucs2Str(const std::u16string_view ucs2_in, const std::string_view str_in)
      : ucs2(EfiCString(ucs2_in)), str(EfiCString(str_in)) {}
  Ucs2Str(const Ucs2Str& src) : ucs2(copy(src.ucs2)), str(copy(src.str)) {}
  Ucs2Str& operator=(const Ucs2Str& src) {
    ucs2 = copy(src.ucs2);
    str = copy(src.str);
    return *this;
  }
};

class Ucs2StrFixture : public ::testing::TestWithParam<Ucs2Str> {};

TEST_P(Ucs2StrFixture, Ucs2StrConvertSuccess) {
  Ucs2Str ucs2_str = GetParam();

  auto res = EfiVariables::Ucs2ToStr(ucs2_str.ucs2);
  ASSERT_TRUE(res.is_ok());
  EXPECT_EQ(res.value(), ucs2_str.str);
}

TEST_P(Ucs2StrFixture, Str2UcsConvertSuccess) {
  Ucs2Str ucs2_str = GetParam();

  // Convert input to correct c-string
  auto ucs2 = copy(ucs2_str.ucs2);
  ucs2.push_back('\0');

  auto res = EfiVariables::StrToUcs2(ucs2_str.str);

  ASSERT_TRUE(res.is_ok());
  EXPECT_EQ(res.value(), ucs2);
}

INSTANTIATE_TEST_SUITE_P(
    Ucs2StrTests, Ucs2StrFixture,
    ::testing::Values(Ucs2Str{u"", ""},                                   // empty c-string
                      Ucs2Str{u"abcDEF123!@#", "abcDEF123!@#"},           // visible chars
                      Ucs2Str{u"\t\n\x0001\x0002", "\t\n\x01\x02"},       // not printable codes
                      Ucs2Str{u"a\tb\nc\x0001\x0002", "a\tb\nc\x01\x02"}  // (not)printable mix
                      // multi-byte chars are not converted properly with `utf8_to_utf16()`
                      // Ucs2Str{u"abcDEF123!@#Δ☹☼", "abcDEF123!@#Δ☹☼"},
                      ));

TEST(EfiVariablesStrToUcs2, LongString) {
  constexpr size_t kCharNumber = 4096;
  const std::string input(kCharNumber, 'a');
  std::u16string expected(kCharNumber, u'a');
  expected.push_back('\0');

  auto res = EfiVariables::StrToUcs2(input);

  ASSERT_TRUE(res.is_ok());
  EXPECT_EQ(res.value(), ToVector(expected));
}

class EfiVariablesTest : public Test {
 public:
  void SetUp() override {
    // Configure the necessary mocks for memory operation.
    system_table_ = efi_system_table{
        .RuntimeServices = mock_runtime_services_.services(),
        .BootServices = mock_boot_services_.services(),
    };

    // EfiMain(kImageHandle, &system_table_);
    gEfiImageHandle = kImageHandle;
    gEfiSystemTable = &system_table_;

    ON_CALL(mock_runtime_services_, QueryVariableInfo).WillByDefault(Return(EFI_NOT_FOUND));
    ON_CALL(mock_runtime_services_, GetNextVariableName).WillByDefault(Return(EFI_NOT_FOUND));
    ON_CALL(mock_runtime_services_, GetVariable).WillByDefault(Return(EFI_NOT_FOUND));
  }

  void TearDown() override {
    gEfiImageHandle = 0;
    gEfiSystemTable = nullptr;
  }

 protected:
  NiceMock<MockRuntimeServices> mock_runtime_services_;
  NiceMock<MockBootServices> mock_boot_services_;
  efi_system_table system_table_ = {};
  EfiVariables efi_variables_;
};

class EfiVariablesStubTest : public Test {
 public:
  void SetUp() override {
    // Configure the necessary mocks for memory operation.
    system_table_ = efi_system_table{
        .RuntimeServices = stub_runtime_services_.services(),
        .BootServices = mock_boot_services_.services(),
    };

    // EfiMain(kImageHandle, &system_table_);
    gEfiImageHandle = kImageHandle;
    gEfiSystemTable = &system_table_;
  }

  void TearDown() override {
    gEfiImageHandle = 0;
    gEfiSystemTable = nullptr;
  }

  void SetVariables(const std::list<std::pair<const EfiVariables::EfiVariableId,
                                              StubRuntimeServices::VariableValue>>& vars_in) {
    std::list<std::pair<StubRuntimeServices::VariableName, StubRuntimeServices::VariableValue>>
        vars;

    std::transform(vars_in.begin(), vars_in.end(), std::back_inserter(vars),
                   [](const auto& v) { return std::make_pair(ToVariableName(v.first), v.second); });
    stub_runtime_services_.SetVariables(vars);
  }

 protected:
  StubRuntimeServices stub_runtime_services_;
  NiceMock<MockBootServices> mock_boot_services_;
  efi_system_table system_table_ = {};
  EfiVariables efi_variables_;

  static inline StubRuntimeServices::VariableName ToVariableName(
      const EfiVariables::EfiVariableId& v) {
    // make sure we remove trailing null-character
    auto tmp = copy(v.name);
    if (!tmp.is_empty() && tmp[tmp.size() - 1] == u'\0')
      tmp.pop_back();
    return StubRuntimeServices::VariableName(tmp, v.vendor_guid);
  }
};

class EfiVariableInfoFixture
    : public ::testing::TestWithParam<std::list<EfiVariables::EfiVariableInfo>> {};

TEST_P(EfiVariableInfoFixture, Eq) {
  std::list<EfiVariables::EfiVariableInfo> input = GetParam();
  for (const auto& a : input) {
    EXPECT_EQ(a, a);
  }
}

TEST_P(EfiVariableInfoFixture, Ne) {
  std::list<EfiVariables::EfiVariableInfo> input = GetParam();
  for (auto a = input.begin(); a != input.end(); a++) {
    for (auto b = std::next(a); b != input.end(); b++) {
      EXPECT_NE(*a, *b);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(EfiVariableInfoTest, EfiVariableInfoFixture,
                         ::testing::Values(std::list<EfiVariables::EfiVariableInfo>{
                             {0, 0, 0},
                             {1, 2, 3},
                             {3, 2, 1},
                             {std::numeric_limits<uint64_t>::max(), 2, 1},
                             {3, std::numeric_limits<uint64_t>::max(), 1},
                             {3, 2, std::numeric_limits<uint64_t>::max()},
                             {std::numeric_limits<uint64_t>::max(),
                              std::numeric_limits<uint64_t>::max(),
                              std::numeric_limits<uint64_t>::max()},
                         }));

TEST_F(EfiVariablesTest, EfiQueryVariableInfoError) {
  auto res = efi_variables_.EfiQueryVariableInfo();
  ASSERT_TRUE(res.is_error());
}

TEST_F(EfiVariablesTest, DumpVarInfoSuccess) {
  constexpr EfiVariables::EfiVariableInfo expected = {
      .max_var_storage_size = 1,
      .remaining_var_storage_size = 2,
      .max_var_size = 3,
  };
  EXPECT_CALL(mock_runtime_services_, QueryVariableInfo)
      .WillOnce([](uint32_t attributes, uint64_t* max_var_storage_size,
                   uint64_t* remaining_var_storage_size, uint64_t* max_var_size) {
        *max_var_storage_size = expected.max_var_storage_size;
        *remaining_var_storage_size = expected.remaining_var_storage_size;
        *max_var_size = expected.max_var_size;
        return EFI_SUCCESS;
      });

  auto res = efi_variables_.EfiQueryVariableInfo();

  ASSERT_TRUE(res.is_ok());
  EXPECT_EQ(res.value(), expected);
}

TEST_F(EfiVariablesTest, DumpVarInfoMaxValues) {
  constexpr EfiVariables::EfiVariableInfo expected = {
      .max_var_storage_size = std::numeric_limits<uint64_t>::max(),
      .remaining_var_storage_size = std::numeric_limits<uint64_t>::max(),
      .max_var_size = std::numeric_limits<uint64_t>::max(),
  };
  EXPECT_CALL(mock_runtime_services_, QueryVariableInfo)
      .WillOnce([](uint32_t attributes, uint64_t* max_var_storage_size,
                   uint64_t* remaining_var_storage_size, uint64_t* max_var_size) {
        *max_var_storage_size = expected.max_var_storage_size;
        *remaining_var_storage_size = expected.remaining_var_storage_size;
        *max_var_size = expected.max_var_size;
        return EFI_SUCCESS;
      });

  auto res = efi_variables_.EfiQueryVariableInfo();

  ASSERT_TRUE(res.is_ok());
  EXPECT_EQ(res.value(), expected);
}

constexpr efi_guid kGuid[] = {
    {0x0, 0x0, 0x0, {0x0}},
    {0x1, 0x1, 0x1, {0x1}},
    {0x2, 0x2, 0x2, {0x2}},
    {0x3, 0x3, 0x3, {0x3}},
};
static const EfiVariables::EfiVariableId kVariableId[] = {
    {EfiCString(u"var0"), kGuid[0]},
    {EfiCString(u"var1"), kGuid[1]},
    {EfiCString(u"var2"), kGuid[2]},
    {EfiCString(u"var3"), kGuid[3]},
};
static const std::vector<uint8_t> kValue[] = {
    {0x00},
    {0x01, 0x02},
    {0x01, 0x02, 0x03},
    {0x01, 0x02, 0x03, 0x04},
};

TEST_F(EfiVariablesStubTest, GetVariableNotThere) {
  auto res = efi_variables_.EfiGetVariable(kVariableId[0]);
  ASSERT_TRUE(res.is_error());
}

TEST_F(EfiVariablesStubTest, GetVariableGood) {
  SetVariables({{kVariableId[0], kValue[0]}});

  auto res = efi_variables_.EfiGetVariable(kVariableId[0]);
  ASSERT_TRUE(res.is_ok());
  EXPECT_THAT(kValue[0], ElementsAreArray(res.value()));
}

TEST_F(EfiVariablesStubTest, GetVariableMultipleGood) {
  SetVariables({
      {kVariableId[0], kValue[0]},
      {kVariableId[1], kValue[1]},
      {kVariableId[2], kValue[2]},
      {kVariableId[3], kValue[3]},
  });

  for (size_t i = 0; i < std::size(kVariableId); i++) {
    auto res = efi_variables_.EfiGetVariable(kVariableId[i]);
    ASSERT_TRUE(res.is_ok());
    EXPECT_THAT(kValue[i], ElementsAreArray(res.value()));
  }
}

TEST_F(EfiVariablesStubTest, GetVariableBadGuid) {
  SetVariables({{{kVariableId[0].name, kGuid[1]}, kValue[0]}});

  auto res = efi_variables_.EfiGetVariable(kVariableId[0]);
  ASSERT_TRUE(res.is_error());
}

TEST_F(EfiVariablesStubTest, GetVariableBadName) {
  SetVariables({{{kVariableId[1].name, kGuid[0]}, kValue[0]}});

  auto res = efi_variables_.EfiGetVariable(kVariableId[0]);
  ASSERT_TRUE(res.is_error());
}

TEST_F(EfiVariablesStubTest, GetVariableGuidDiffers) {
  SetVariables({
      {{kVariableId[1].name, kGuid[0]}, kValue[1]},
      {kVariableId[0], kValue[0]},
      {{kVariableId[2].name, kGuid[0]}, kValue[2]},
  });

  auto res = efi_variables_.EfiGetVariable(kVariableId[0]);
  ASSERT_TRUE(res.is_ok());
  EXPECT_THAT(kValue[0], ElementsAreArray(res.value()));
}

TEST_F(EfiVariablesTest, GetVariableNotThere) {
  auto res = efi_variables_.EfiGetVariable(kVariableId[0]);
  ASSERT_TRUE(res.is_error());
}

TEST_F(EfiVariablesStubTest, GetGuidFromEmptyList) {
  SetVariables({});
  auto res = efi_variables_.GetGuid(EfiCString(kVariableId[0].name));
  ASSERT_TRUE(res.is_error());
}

TEST_F(EfiVariablesStubTest, GetGuidOneExist) {
  SetVariables({
      {kVariableId[0], kValue[0]},
      {kVariableId[1], kValue[1]},
  });

  auto res = efi_variables_.GetGuid(EfiCString(kVariableId[0].name));

  ASSERT_TRUE(res.is_ok());
  EXPECT_EQ(res.value(), kVariableId[0].vendor_guid);
}

TEST_F(EfiVariablesStubTest, GetGuidMultipleExist) {
  SetVariables({
      {kVariableId[0], kValue[0]},
      {kVariableId[0], kValue[1]},
  });

  auto res = efi_variables_.GetGuid(EfiCString(kVariableId[0].name));

  ASSERT_TRUE(res.is_error());
}

TEST_F(EfiVariablesStubTest, GetGuidMissing) {
  SetVariables({
      {kVariableId[0], kValue[0]},
      {kVariableId[1], kValue[1]},
  });

  auto res = efi_variables_.GetGuid(EfiCString(kVariableId[2].name));

  ASSERT_TRUE(res.is_error());
}

TEST_F(EfiVariablesStubTest, GetGuidBadInput) {
  SetVariables({
      {kVariableId[0], kValue[0]},
      {kVariableId[1], kValue[1]},
  });

  auto res = efi_variables_.GetGuid(u"");

  ASSERT_TRUE(res.is_error());
}

TEST_F(EfiVariablesStubTest, IterateOverEmptyList) {
  SetVariables({});

  EfiVariables efi_variables;
  EXPECT_EQ(std::distance(efi_variables.begin(), efi_variables.end()), 0UL);
  EXPECT_EQ(efi_variables.begin(), efi_variables.end());
}

TEST_F(EfiVariablesStubTest, IterateOverList) {
  SetVariables({
      {kVariableId[0], kValue[0]},
      {kVariableId[1], kValue[1]},
      {kVariableId[2], kValue[2]},
      {kVariableId[3], kValue[3]},
  });

  EfiVariables efi_variables;

  size_t i = 0;
  for (const auto& v_id : efi_variables) {
    ASSERT_TRUE(v_id.IsValid());
    EXPECT_EQ(v_id.name, EfiCString(kVariableId[i].name));
    EXPECT_EQ(v_id.vendor_guid, kVariableId[i].vendor_guid);
    ++i;
  }
}

fbl::Vector<char> EfiCString(const std::string_view sv) {
  fbl::Vector<char> res = ToVector(sv);
  if (res.is_empty() || res[res.size() - 1] != '\0') {
    res.push_back('\0');
  }
  return res;
}
fbl::Vector<char16_t> EfiCString(const std::u16string_view sv) {
  fbl::Vector<char16_t> res = ToVector(sv);
  if (res.is_empty() || res[res.size() - 1] != '\0') {
    res.push_back('\0');
  }
  return res;
}
fbl::Vector<char16_t> EfiCString(const fbl::Vector<char16_t>& v) {
  std::u16string_view sv(v.data(), v.size());
  return EfiCString(sv);
}

}  // namespace gigaboot
