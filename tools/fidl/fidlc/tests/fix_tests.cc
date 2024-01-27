// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zircon/types.h>

#include <zxtest/zxtest.h>

#include "tools/fidl/fidlc/include/fidl/diagnostics.h"
#include "tools/fidl/fidlc/include/fidl/experimental_flags.h"
#include "tools/fidl/fidlc/include/fidl/fixes.h"
#include "tools/fidl/fidlc/include/fidl/reporter.h"
#include "tools/fidl/fidlc/include/fidl/source_file.h"
#include "tools/fidl/fidlc/include/fidl/source_manager.h"
#include "tools/fidl/fidlc/tests/error_test.h"
#include "tools/fidl/fidlc/tests/test_library.h"

namespace {

using namespace fidl;
using namespace fidl::fix;

template <typename T>
void GoodParsedFix(const ExperimentalFlags experimental_flags, const std::string& before,
                   const std::string& after) {
  static_assert(std::is_base_of<ParsedFix, T>::value,
                "Only classes derived from |ParsedFix| may be used by |GoodTransform()}");
  static const std::string kGoodFileName = "good.fidl";

  Reporter reporter;
  auto manager = std::make_unique<SourceManager>();
  manager->AddSourceFile(std::make_unique<SourceFile>(kGoodFileName, before));
  auto fix = T(manager, experimental_flags);
  EXPECT_EQ(fix.ValidateFlags(), Status::kOk);

  auto result = fix.Transform(&reporter);
  ASSERT_TRUE(result.is_ok());

  OutputMap outputs = result.value();
  EXPECT_EQ(outputs.size(), 1);

  auto transformed = outputs[manager->sources()[0].get()];
  EXPECT_STREQ(transformed, after);
}

// Immediately fail - used to test transform failure reporting.
class FailingTransformer final : public ParsedTransformer {
 public:
  FailingTransformer(const std::vector<const SourceFile*>& source_files,
                     const ExperimentalFlags& experimental_flags, Reporter* reporter)
      : ParsedTransformer(source_files, experimental_flags, reporter) {}

  void WhenFile(raw::File* el, TokenSlice& token_slice) override {
    // Immediately fail, and do nothing else.
    AddError("failure!");
  }
};

// A fix that fails during transformation.
class FailingParsedFix final : public ParsedFix {
 public:
  FailingParsedFix(const std::unique_ptr<SourceManager>& library,
                   const ExperimentalFlags experimental_flags)
      : ParsedFix(Fixable::Get(Fixable::Kind::kNoop), library, experimental_flags) {}

  std::unique_ptr<ParsedTransformer> GetParsedTransformer(
      const std::vector<const SourceFile*> source_files,
      const fidl::ExperimentalFlags& experimental_flags, Reporter* reporter) override {
    return std::make_unique<FailingTransformer>(source_files, experimental_flags, reporter);
  }
};

TEST(FixTests, BadInvalidFlags) {
  ExperimentalFlags experimental_flags;
  auto manager = std::make_unique<SourceManager>();
  manager->AddSourceFile(std::make_unique<SourceFile>("bad.fidl", "library;"));

  auto fix = FailingParsedFix(manager, experimental_flags);
  EXPECT_EQ(fix.ValidateFlags(), Status::kErrorOther);
}

TEST(FixTests, BadParsingFailure) {
  Reporter reporter;
  ExperimentalFlags experimental_flags;
  experimental_flags.EnableFlag(ExperimentalFlags::Flag::kNoop);
  auto manager = std::make_unique<SourceManager>();
  manager->AddSourceFile(std::make_unique<SourceFile>("bad.fidl", "library;"));

  auto fix = NoopParsedFix(manager, experimental_flags);
  EXPECT_EQ(fix.ValidateFlags(), Status::kOk);

  auto result = fix.Transform(&reporter);
  ASSERT_FALSE(result.is_ok());

  auto failure = result.error_value();
  EXPECT_EQ(failure.status, Status::kErrorPreFix);
  ASSERT_EQ(failure.errors.size(), 1);
  ASSERT_EQ(reporter.errors().size(), 1);
  EXPECT_EQ(failure.errors[0].step, fidl::fix::Step::kPreparing);
  EXPECT_ERR(reporter.errors()[0], ErrUnexpectedTokenOfKind);
  EXPECT_SUBSTR(failure.errors[0].msg, "bad.fidl");
}

TEST(FixTests, BadTransformFailure) {
  Reporter reporter;
  ExperimentalFlags experimental_flags;
  experimental_flags.EnableFlag(ExperimentalFlags::Flag::kNoop);
  auto manager = std::make_unique<SourceManager>();
  manager->AddSourceFile(std::make_unique<SourceFile>("valid.fidl", "library example;"));

  auto fix = FailingParsedFix(manager, experimental_flags);
  EXPECT_EQ(fix.ValidateFlags(), Status::kOk);

  auto result = fix.Transform(&reporter);
  ASSERT_FALSE(result.is_ok());

  auto failure = result.error_value();
  EXPECT_EQ(failure.status, Status::kErrorDuringFix);
  ASSERT_EQ(failure.errors.size(), 2);
  ASSERT_TRUE(reporter.errors().empty());
  EXPECT_EQ(failure.errors[0].step, fidl::fix::Step::kTransforming);
  EXPECT_SUBSTR(failure.errors[0].msg, "failure!");
  EXPECT_EQ(failure.errors[1].step, fidl::fix::Step::kTransforming);
  EXPECT_SUBSTR(failure.errors[1].msg, "valid.fidl");
}

TEST(FixTests, BadProtocolModifierFixMissingExperiment) {
  ExperimentalFlags experimental_flags;
  auto manager = std::make_unique<SourceManager>();
  manager->AddSourceFile(std::make_unique<SourceFile>("valid.fidl", "library example;"));

  auto fix = ProtocolModifierFix(manager, experimental_flags);
  EXPECT_EQ(fix.ValidateFlags(), Status::kErrorOther);
}

void GoodProtocolModifierFix(const std::string& before, const std::string& after) {
  GoodParsedFix<ProtocolModifierFix>(
      ExperimentalFlags(ExperimentalFlags::Flag::kUnknownInteractions), before, after);
}

TEST(FixTests, GoodProtocolModifierFixEmptyProtocolAlreadyClosed) {
  std::string noop = R"FIDL(library example;
closed protocol MyProtocol {};
)FIDL";
  GoodProtocolModifierFix(noop, noop);
}

TEST(FixTests, GoodProtocolModifierFixEmptyProtocolAlreadyAjar) {
  std::string noop = R"FIDL(library example;
ajar protocol MyProtocol {};
)FIDL";
  GoodProtocolModifierFix(noop, noop);
}

TEST(FixTests, GoodProtocolModifierFixEmptyProtocolAlreadyOpen) {
  std::string noop = R"FIDL(library example;
open protocol MyProtocol {};
)FIDL";
  GoodProtocolModifierFix(noop, noop);
}

TEST(FixTests, GoodProtocolModifierFixEmptyProtocolUnmodified) {
  GoodProtocolModifierFix(R"FIDL(library example;
protocol MyProtocol {};
)FIDL",
                          R"FIDL(library example;
closed protocol MyProtocol {};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixEmptyProtocolWithComment) {
  GoodProtocolModifierFix(R"FIDL(library example;
// comment
protocol MyProtocol {};
)FIDL",
                          R"FIDL(library example;
// comment
closed protocol MyProtocol {};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixEmptyProtocolWithDocComment) {
  GoodProtocolModifierFix(R"FIDL(library example;
/// doc comment
protocol MyProtocol {};
)FIDL",
                          R"FIDL(library example;
/// doc comment
closed protocol MyProtocol {};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixEmptyProtocolWithAttribute) {
  GoodProtocolModifierFix(R"FIDL(library example;
@attr
protocol MyProtocol {};
)FIDL",
                          R"FIDL(library example;
@attr
closed protocol MyProtocol {};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixEmptyProtocolWithAllPrefixes) {
  GoodProtocolModifierFix(R"FIDL(library example;
// comment
/// doc comment
@attr
protocol MyProtocol {};
)FIDL",
                          R"FIDL(library example;
// comment
/// doc comment
@attr
closed protocol MyProtocol {};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixMultipleEmptyProtocols) {
  GoodProtocolModifierFix(R"FIDL(library example;
protocol MyProtocol {};
protocol MyOtherProtocol {};
)FIDL",
                          R"FIDL(library example;
closed protocol MyProtocol {};
closed protocol MyOtherProtocol {};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixMethodsAlreadyStrict) {
  std::string noop = R"FIDL(library example;
closed protocol MyProtocol {
    strict OneWay();
    strict TwoWay() -> ();
    strict WithErr() -> () error uint32;
    strict -> OnEvent();
};
)FIDL";
  GoodProtocolModifierFix(noop, noop);
}

TEST(FixTests, GoodProtocolModifierFixMethodsAlreadyFlexible) {
  std::string noop = R"FIDL(library example;
open protocol MyProtocol {
    flexible OneWay();
    flexible TwoWay() -> ();
    flexible WithErr() -> () error uint32;
    flexible -> OnEvent();
};
)FIDL";
  GoodProtocolModifierFix(noop, noop);
}

TEST(FixTests, GoodProtocolModifierFixMethodsUnmodified) {
  GoodProtocolModifierFix(R"FIDL(library example;
protocol MyProtocol {
    OneWay();
    TwoWay() -> ();
    WithErr() -> () error uint32;
    -> OnEvent();
};
)FIDL",
                          R"FIDL(library example;
closed protocol MyProtocol {
    strict OneWay();
    strict TwoWay() -> ();
    strict WithErr() -> () error uint32;
    strict -> OnEvent();
};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixMethodsWithComments) {
  GoodProtocolModifierFix(R"FIDL(library example;
protocol MyProtocol {
    // comment
    OneWay();
    // comment
    TwoWay() -> ();
    // comment
    WithErr() -> () error uint32;
    // comment
    -> OnEvent();
};
)FIDL",
                          R"FIDL(library example;
closed protocol MyProtocol {
    // comment
    strict OneWay();
    // comment
    strict TwoWay() -> ();
    // comment
    strict WithErr() -> () error uint32;
    // comment
    strict -> OnEvent();
};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixMethodsWithDocComments) {
  GoodProtocolModifierFix(R"FIDL(library example;
protocol MyProtocol {
    /// doc comment
    OneWay();
    /// doc comment
    TwoWay() -> ();
    /// doc comment
    WithErr() -> () error uint32;
    /// doc comment
    -> OnEvent();
};
)FIDL",
                          R"FIDL(library example;
closed protocol MyProtocol {
    /// doc comment
    strict OneWay();
    /// doc comment
    strict TwoWay() -> ();
    /// doc comment
    strict WithErr() -> () error uint32;
    /// doc comment
    strict -> OnEvent();
};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixMethodsWithAttributes) {
  GoodProtocolModifierFix(R"FIDL(library example;
protocol MyProtocol {
    @attr
    OneWay();
    @attr
    TwoWay() -> ();
    @attr
    WithErr() -> () error uint32;
    @attr
    -> OnEvent();
};
)FIDL",
                          R"FIDL(library example;
closed protocol MyProtocol {
    @attr
    strict OneWay();
    @attr
    strict TwoWay() -> ();
    @attr
    strict WithErr() -> () error uint32;
    @attr
    strict -> OnEvent();
};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixMethodsWithAllPrefixes) {
  GoodProtocolModifierFix(R"FIDL(library example;
protocol MyProtocol {
    // comment
    /// doc comment
    @attr
    OneWay();
    // comment
    /// doc comment
    @attr
    TwoWay() -> ();
    // comment
    /// doc comment
    @attr
    WithErr() -> () error uint32;
    // comment
    /// doc comment
    @attr
    -> OnEvent();
};
)FIDL",
                          R"FIDL(library example;
closed protocol MyProtocol {
    // comment
    /// doc comment
    @attr
    strict OneWay();
    // comment
    /// doc comment
    @attr
    strict TwoWay() -> ();
    // comment
    /// doc comment
    @attr
    strict WithErr() -> () error uint32;
    // comment
    /// doc comment
    @attr
    strict -> OnEvent();
};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixMultiplePopulatedProtocols) {
  GoodProtocolModifierFix(R"FIDL(library example;
protocol MyProtocol {
    OneWay();
    TwoWay() -> ();
};
protocol MyOtherProtocol {
    WithErr() -> () error uint32;
    -> OnEvent();
};
)FIDL",
                          R"FIDL(library example;
closed protocol MyProtocol {
    strict OneWay();
    strict TwoWay() -> ();
};
closed protocol MyOtherProtocol {
    strict WithErr() -> () error uint32;
    strict -> OnEvent();
};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixProtocolModifierOnly) {
  GoodProtocolModifierFix(R"FIDL(library example;
protocol MyProtocol {
    strict OneWay();
    strict TwoWay() -> ();
    strict WithErr() -> () error uint32;
    strict -> OnEvent();
};
)FIDL",
                          R"FIDL(library example;
closed protocol MyProtocol {
    strict OneWay();
    strict TwoWay() -> ();
    strict WithErr() -> () error uint32;
    strict -> OnEvent();
};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixMethodModifiersOnly) {
  GoodProtocolModifierFix(R"FIDL(library example;
closed protocol MyProtocol {
    OneWay();
    TwoWay() -> ();
    WithErr() -> () error uint32;
    -> OnEvent();
};
)FIDL",
                          R"FIDL(library example;
closed protocol MyProtocol {
    strict OneWay();
    strict TwoWay() -> ();
    strict WithErr() -> () error uint32;
    strict -> OnEvent();
};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixSomeExistingModifiers) {
  GoodProtocolModifierFix(R"FIDL(library example;
protocol MyProtocol {
    strict OneWay();
    strict TwoWay() -> ();
    WithErr() -> () error uint32;
    -> OnEvent();
};
open protocol MyOtherProtocol {
    OneWay();
    TwoWay() -> ();
    strict WithErr() -> () error uint32;
    flexible -> OnEvent();
};
)FIDL",
                          R"FIDL(library example;
closed protocol MyProtocol {
    strict OneWay();
    strict TwoWay() -> ();
    strict WithErr() -> () error uint32;
    strict -> OnEvent();
};
open protocol MyOtherProtocol {
    strict OneWay();
    strict TwoWay() -> ();
    strict WithErr() -> () error uint32;
    flexible -> OnEvent();
};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixOtherProtocolIdentifiers) {
  GoodProtocolModifierFix(R"FIDL(library example;

const protocol bool = true;

@attr(protocol)
protocol MyProtocol {};

@protocol(protocol=protocol)
protocol MyOtherProtocol {};
)FIDL",
                          R"FIDL(library example;

const protocol bool = true;

@attr(protocol)
closed protocol MyProtocol {};

@protocol(protocol=protocol)
closed protocol MyOtherProtocol {};
)FIDL");
}

TEST(FixTests, GoodProtocolModifierFixProtocolNamedProtocol) {
  GoodProtocolModifierFix(R"FIDL(library example;
protocol protocol {};
)FIDL",
                          R"FIDL(library example;
closed protocol protocol {};
)FIDL");
}

}  // namespace
