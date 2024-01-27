// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <lib/elfldltl/dynamic.h>
#include <lib/elfldltl/layout.h>
#include <lib/elfldltl/load.h>
#include <lib/elfldltl/mapped-fd-file.h>
#include <lib/elfldltl/resolve.h>

#include <string>
#include <string_view>

#include "symbol-tests.h"

namespace {

template <class ElfLayout>
class ElfldltlResolveTests : public testing::Test {
 public:
  using Elf = ElfLayout;

  class TestModule {
   public:
    using size_type = typename Elf::size_type;

    TestModule(std::string_view prefix) { create(prefix); }

    constexpr const auto& symbol_info() const { return symbol_info_; }
    constexpr auto& symbol_info() { return symbol_info_; }

    constexpr size_type load_bias() const { return 0; }

    constexpr auto& file() { return file_; }

   private:
    // This is in it's own function that returns void so that ASSERT_* macros
    // can be used. Evidently they don't work in constructors. This method
    // should only be used by the constructor.
    void create(std::string_view prefix) {
      using Phdr = typename Elf::Phdr;
      using Dyn = typename Elf::Dyn;

      auto diag = ExpectOkDiagnostics();

      std::filesystem::path path = GetTestDataPath(GetFileName(prefix));
      fbl::unique_fd fd(open(path.c_str(), O_RDONLY));
      ASSERT_TRUE(fd) << path;

      ASSERT_TRUE(file_.Init(fd.get()).is_ok());

      auto headers =
          elfldltl::LoadHeadersFromFile<Elf>(diag, file_, elfldltl::NoArrayFromFile<Phdr>(), {});
      ASSERT_TRUE(headers);

      std::optional<Phdr> ph;
      elfldltl::DecodePhdrs(diag, headers->second, elfldltl::PhdrDynamicObserver<Elf>(ph));
      ASSERT_TRUE(ph);

      // Note: We read in from offset here and not vaddr so we get the file
      // contents because we are not loading this file. For this reason, the
      // checked in files must have a flat dynamic segment entirely in RODATA.
      auto dyn = file_.template ReadArray<Dyn>(ph->offset(), ph->filesz() / sizeof(Dyn));
      ASSERT_TRUE(dyn);

      ASSERT_TRUE(elfldltl::DecodeDynamic(diag, file_, *dyn,
                                          elfldltl::DynamicSymbolInfoObserver(symbol_info_)));
    }

    elfldltl::MappedFdFile file_;
    elfldltl::SymbolInfo<Elf> symbol_info_;
  };

 private:
  static std::string GetFileName(std::string_view prefix) {
    using namespace std::string_literals;

    constexpr std::string_view class_name = [] {
      switch (Elf::kClass) {
        case elfldltl::ElfClass::k32:
          return "32"sv;
        case elfldltl::ElfClass::k64:
          return "64"sv;
      }
    }();

    constexpr std::string_view data_name = [] {
      switch (Elf::kData) {
        case elfldltl::ElfData::k2Lsb:
          return "le"sv;
        case elfldltl::ElfData::k2Msb:
          return "be"sv;
      }
    }();

    return std::string{prefix} + "-" + std::string{class_name} + std::string{data_name} + ".so";
  }
};

TYPED_TEST_SUITE(ElfldltlResolveTests, AllFormatsTypedTest);

constexpr elfldltl::SymbolName kASymbol("a"sv);
constexpr elfldltl::SymbolName kBSymbol("b"sv);

TYPED_TEST(ElfldltlResolveTests, SingleModule) {
  using Elf = typename TestFixture::Elf;
  using Sym = typename Elf::Sym;
  using TestModule = typename ElfldltlResolveTests<Elf>::TestModule;

  auto diag = ExpectOkDiagnostics();

  TestModule module("first");
  if (this->HasFatalFailure()) {
    return;
  }

  auto& si = module.symbol_info();

  const Sym* a = kASymbol.Lookup(si);
  ASSERT_NE(a, nullptr);

  cpp20::span modules{&module, 1};
  auto resolve = elfldltl::MakeSymbolResolver(si, modules, diag);
  auto found = resolve(*a, elfldltl::RelocateTls::kNone);
  ASSERT_TRUE(found);
  ASSERT_FALSE(found->undefined_weak());
  EXPECT_EQ(&found->symbol(), a);
  EXPECT_EQ(found->symbol().value, 1ul);
}

TYPED_TEST(ElfldltlResolveTests, DefBothFoundFirst) {
  using Elf = typename TestFixture::Elf;
  using Sym = typename Elf::Sym;
  using TestModule = typename ElfldltlResolveTests<Elf>::TestModule;

  auto diag = ExpectOkDiagnostics();

  std::array modules{TestModule("first"), TestModule("second")};
  if (this->HasFatalFailure()) {
    return;
  }

  auto& si = modules[0].symbol_info();
  auto& si2 = modules[1].symbol_info();

  const Sym* a = kASymbol.Lookup(si);
  ASSERT_NE(a, nullptr);
  const Sym* a2 = kASymbol.Lookup(si2);
  ASSERT_NE(a2, nullptr);

  auto resolve = elfldltl::MakeSymbolResolver(si2, modules, diag);
  auto found = resolve(*a2, elfldltl::RelocateTls::kNone);
  ASSERT_TRUE(found);
  ASSERT_FALSE(found->undefined_weak());
  EXPECT_EQ(&found->symbol(), a);
  EXPECT_EQ(found->symbol().value, 1ul);
}

TYPED_TEST(ElfldltlResolveTests, UndefFirstFoundSecond) {
  using Elf = typename TestFixture::Elf;
  using Sym = typename Elf::Sym;
  using TestModule = typename ElfldltlResolveTests<Elf>::TestModule;

  auto diag = ExpectOkDiagnostics();

  std::array modules{TestModule("first"), TestModule("second")};
  if (this->HasFatalFailure()) {
    return;
  }

  auto& si2 = modules[1].symbol_info();

  elfldltl::SymbolInfoForSingleLookup<Elf> si{"b"};

  const Sym* b_def = kBSymbol.Lookup(si2);
  ASSERT_NE(b_def, nullptr);

  auto resolve = elfldltl::MakeSymbolResolver(si, modules, diag);
  auto found = resolve(si.symbol(), elfldltl::RelocateTls::kNone);
  ASSERT_TRUE(found);
  ASSERT_FALSE(found->undefined_weak());
  EXPECT_EQ(&found->symbol(), b_def);
  EXPECT_EQ(found->symbol().value, 2ul);
}

TYPED_TEST(ElfldltlResolveTests, UndefinedWeak) {
  using Elf = typename TestFixture::Elf;
  using TestModule = typename ElfldltlResolveTests<Elf>::TestModule;

  auto diag = ExpectOkDiagnostics();

  std::array modules{TestModule("first"), TestModule("second")};
  if (this->HasFatalFailure()) {
    return;
  }

  elfldltl::SymbolInfoForSingleLookup<Elf> si{"noexist", elfldltl::ElfSymType::kNoType,
                                              elfldltl::ElfSymBind::kWeak};
  auto resolve = elfldltl::MakeSymbolResolver(si, modules, diag);
  auto found = resolve(si.symbol(), elfldltl::RelocateTls::kNone);
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->undefined_weak());
}

}  // namespace
