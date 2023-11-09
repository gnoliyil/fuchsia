// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zxtest/zxtest.h>

#include "tools/fidl/fidlc/include/fidl/diagnostics.h"
#include "tools/fidl/fidlc/tests/error_test.h"
#include "tools/fidl/fidlc/tests/test_library.h"

namespace {

TEST(StructsTests, GoodSimpleStruct) {
  TestLibrary library;
  library.AddFile("good/fi-0001.test.fidl");

  ASSERT_COMPILED(library);
}

TEST(StructsTests, GoodPrimitiveDefaultValueLiteral) {
  TestLibrary library(R"FIDL(library example;

type MyStruct = struct {
    @allow_deprecated_struct_defaults
    field int64 = 20;
};
)FIDL");
  ASSERT_COMPILED(library);
  auto type_decl = library.LookupStruct("MyStruct");
  ASSERT_NOT_NULL(type_decl);
  EXPECT_EQ(type_decl->members.size(), 1);
}

TEST(StructsTests, BadPrimitiveDefaultValueNoAnnotation) {
  TestLibrary library;
  library.AddFile("bad/fi-0050.test.fidl");
  library.ExpectFail(fidl::ErrDeprecatedStructDefaults);
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, GoodPrimitiveDefaultValueConstReference) {
  TestLibrary library(R"FIDL(library example;

const A int32 = 20;

type MyStruct = struct {
    @allow_deprecated_struct_defaults
    field int64 = A;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StructsTests, BadMissingDefaultValueReferenceTarget) {
  TestLibrary library(R"FIDL(
library example;

type MyStruct = struct {
    field int64 = A;
};
)FIDL");
  ASSERT_FALSE(library.Compile());
}

TEST(StructsTests, GoodEnumDefaultValueEnumMemberReference) {
  TestLibrary library(R"FIDL(library example;

type MyEnum = strict enum : int32 {
    A = 5;
};

type MyStruct = struct {
    @allow_deprecated_struct_defaults
    field MyEnum = MyEnum.A;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StructsTests, GoodPrimitiveDefaultValueEnumMemberReference) {
  TestLibrary library(R"FIDL(library example;

type MyEnum = strict enum : int32 {
    A = 5;
};

type MyStruct = struct {
    @allow_deprecated_struct_defaults
    field int64 = MyEnum.A;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StructsTests, BadDefaultValueEnumType) {
  TestLibrary library(R"FIDL(
library example;

type MyEnum = enum : int32 { A = 1; };
type OtherEnum = enum : int32 { A = 1; };

type MyStruct = struct {
    @allow_deprecated_struct_defaults
    field MyEnum = OtherEnum.A;
};
)FIDL");
  library.ExpectFail(fidl::ErrCouldNotResolveMemberDefault, "field");
  library.ExpectFail(fidl::ErrMismatchedNameTypeAssignment, "MyEnum", "OtherEnum");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, BadDefaultValuePrimitiveInEnum) {
  TestLibrary library;
  library.AddFile("bad/fi-0103.test.fidl");
  library.ExpectFail(fidl::ErrCouldNotResolveMemberDefault, "field");
  library.ExpectFail(fidl::ErrTypeCannotBeConvertedToType, "1", "untyped numeric",
                     "test.bad.fi0103/MyEnum");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, GoodEnumDefaultValueBitsMemberReference) {
  TestLibrary library(R"FIDL(library example;

type MyBits = strict bits : uint32 {
    A = 0x00000001;
};

type MyStruct = struct {
    @allow_deprecated_struct_defaults
    field MyBits = MyBits.A;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StructsTests, GoodPrimitiveDefaultValueBitsMemberReference) {
  TestLibrary library(R"FIDL(library example;

type MyBits = strict bits : uint32 {
    A = 0x00000001;
};

type MyStruct = struct {
    @allow_deprecated_struct_defaults
    field int64 = MyBits.A;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StructsTests, BadDefaultValueBitsType) {
  TestLibrary library(R"FIDL(
library example;

type MyBits = bits : uint32 { A = 0x00000001; };
type OtherBits = bits : uint32 { A = 0x00000001; };

type MyStruct = struct {
    @allow_deprecated_struct_defaults
    field MyBits = OtherBits.A;
};
)FIDL");
  library.ExpectFail(fidl::ErrCouldNotResolveMemberDefault, "field");
  library.ExpectFail(fidl::ErrMismatchedNameTypeAssignment, "MyBits", "OtherBits");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, BadDefaultValuePrimitiveInBits) {
  TestLibrary library(R"FIDL(
library example;

type MyBits = enum : int32 { A = 0x00000001; };

type MyStruct = struct {
    @allow_deprecated_struct_defaults
    field MyBits = 1;
};
)FIDL");
  library.ExpectFail(fidl::ErrCouldNotResolveMemberDefault, "field");
  library.ExpectFail(fidl::ErrTypeCannotBeConvertedToType, "1", "untyped numeric",
                     "example/MyBits");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

// The old-style of enum-referencing should no longer work.
TEST(StructsTests, BadLegacyEnumMemberReference) {
  TestLibrary library(R"FIDL(
library example;

type MyEnum = enum : int32 { A = 5; };

type MyStruct = struct {
    field MyEnum = A;
};
)FIDL");
  ASSERT_FALSE(library.Compile());
}

TEST(StructsTests, BadDefaultValueNullableString) {
  TestLibrary library;
  library.AddFile("bad/fi-0091.test.fidl");
  library.ExpectFail(fidl::ErrInvalidStructMemberType, "name", "string?");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, BadDuplicateMemberName) {
  TestLibrary library(R"FIDL(
library example;

type MyStruct = struct {
    my_struct_member string;
    my_struct_member uint8;
};
)FIDL");
  library.ExpectFail(fidl::ErrDuplicateElementName, fidl::flat::Element::Kind::kStructMember,
                     "my_struct_member", "example.fidl:5:5");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, GoodMaxInlineSize) {
  TestLibrary library(R"FIDL(library example;

type MyStruct = struct {
    arr array<uint8, 65535>;
};
)FIDL");
  ASSERT_COMPILED(library);
}

TEST(StructsTests, BadInlineSizeExceeds64k) {
  TestLibrary library;
  library.AddFile("bad/fi-0111.test.fidl");
  library.ExpectFail(fidl::ErrInlineSizeExceedsLimit, "MyStruct", 65536, 65535);
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, BadMutuallyRecursive) {
  TestLibrary library;
  library.AddFile("bad/fi-0057-a.test.fidl");

  library.ExpectFail(fidl::ErrIncludeCycle, "struct 'Yang' -> struct 'Yin' -> struct 'Yang'");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, BadSelfRecursive) {
  TestLibrary library;
  library.AddFile("bad/fi-0057-c.test.fidl");

  library.ExpectFail(fidl::ErrIncludeCycle, "struct 'MySelf' -> struct 'MySelf'");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, GoodOptionalityAllowsRecursion) {
  TestLibrary library;
  library.AddFile("good/fi-0057.test.fidl");

  ASSERT_COMPILED(library);
}

TEST(StructsTests, BadMutuallyRecursiveWithIncomingLeaf) {
  TestLibrary library(R"FIDL(
library example;

type Yin = struct {
  yang Yang;
};

type Yang = struct {
  yin Yin;
};

type Leaf = struct {
  yin Yin;
};
)FIDL");
  // Leaf sorts before either Yin or Yang, so the cycle finder in sort_step.cc
  // starts there, which leads it to yin before yang.
  library.ExpectFail(fidl::ErrIncludeCycle, "struct 'Yin' -> struct 'Yang' -> struct 'Yin'");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, BadMutuallyRecursiveWithOutogingLeaf) {
  TestLibrary library(R"FIDL(
library example;

type Yin = struct {
  yang Yang;
};

type Yang = struct {
  yin Yin;
  leaf Leaf;
};

type Leaf = struct {
  x int32;
};
)FIDL");
  library.ExpectFail(fidl::ErrIncludeCycle, "struct 'Yang' -> struct 'Yin' -> struct 'Yang'");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, BadMutuallyRecursiveIntersectingLoops) {
  TestLibrary library(R"FIDL(
library example;

type Yin = struct {
  intersection Intersection;
};

type Yang = struct {
  intersection Intersection;
};

type Intersection = struct {
  yin Yin;
  yang Yang;
};
)FIDL");
  library.ExpectFail(fidl::ErrIncludeCycle,
                     "struct 'Intersection' -> struct 'Yang' -> struct 'Intersection'");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, BadBoxCannotBeOptional) {
  TestLibrary library;
  library.AddFile("bad/fi-0169.test.fidl");
  library.ExpectFail(fidl::ErrBoxCannotBeOptional);
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, BadStructCannotBeOptional) {
  TestLibrary library;
  library.AddFile("bad/fi-0159.test.fidl");
  library.ExpectFail(fidl::ErrStructCannotBeOptional, "Date");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, BadHandleCannotBeBoxedShouldBeOptional) {
  TestLibrary library;
  library.AddFile("bad/fi-0171.test.fidl");
  library.UseLibraryZx();
  library.ExpectFail(fidl::ErrCannotBeBoxedShouldBeOptional, "Handle");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, BadTypeCannotBeBoxedShouldBeOptional) {
  for (const std::string& definition : {
           "type Foo = struct { union_member box<union { 1: data uint8; }>; };",
           "type Foo = struct { vector_member box<vector<uint8>>; };",
           "type Foo = struct { string_member box<string>; };",
           "type Foo = resource struct { handle_member box<zx.Handle>; };",
           "protocol Bar {}; type Foo = resource struct { client_member box<client_end:Bar>; };",
           "protocol Bar {}; type Foo = resource struct { server_member box<server_end:Bar>; };",
       }) {
    std::string fidl_library = "library example;\nusing zx;\n\n" + definition + "\n";
    TestLibrary library(fidl_library);
    library.UseLibraryZx();
    ASSERT_FALSE(library.Compile());
    ASSERT_EQ(library.errors().size(), 1);
    ASSERT_ERR(library.errors()[0], fidl::ErrCannotBeBoxedShouldBeOptional);
  }
}

TEST(StructsTests, BadCannotBoxPrimitive) {
  TestLibrary library;
  library.AddFile("bad/fi-0193.test.fidl");
  library.ExpectFail(fidl::ErrCannotBeBoxedNorOptional, "bool");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, BadTypeCannotBeBoxedNorOptional) {
  for (const std::string& definition : {
           "type Foo = struct { table_member box<table { 1: data uint8; }>; };",
           "type Foo = struct { box_member box<box<struct {}>>; };",
           "type Foo = struct { enum_member box<enum { DATA = 1; }>; };",
           "type Foo = struct { bits_member box<bits { DATA = 1; }>; };",
           "type Foo = struct { array_member box<array<uint8, 1>>; };",
           "type Foo = struct { bool_member box<bool>; };",
           "type Foo = struct { int8_member box<int8>; };",
           "type Foo = struct { int16_member box<int16>; };",
           "type Foo = struct { int32_member box<int32>; };",
           "type Foo = struct { int64_member box<int64>; };",
           "type Foo = struct { uint8_member box<uint8>; };",
           "type Foo = struct { uint16_member box<uint16>; };",
           "type Foo = struct { uint32_member box<uint32>; };",
           "type Foo = struct { uint64_member box<uint64>; };",
           "type Foo = struct { float32_member box<float32>; };",
           "type Foo = struct { float64_member box<float64>; };",
       }) {
    std::string fidl_library = "library example;\nusing zx;\n\n" + definition + "\n";
    TestLibrary library(fidl_library);
    library.UseLibraryZx();
    ASSERT_FALSE(library.Compile());
    ASSERT_EQ(library.errors().size(), 1);
    ASSERT_ERR(library.errors()[0], fidl::ErrCannotBeBoxedNorOptional);
  }
}

TEST(StructsTests, BadDefaultValueReferencesInvalidConst) {
  TestLibrary library(R"FIDL(
library example;

type Foo = struct {
    @allow_deprecated_struct_defaults
    flag bool = BAR;
};

const BAR bool = "not a bool";
)FIDL");
  ASSERT_FALSE(library.Compile());
  ASSERT_EQ(library.errors().size(), 3);
  EXPECT_ERR(library.errors()[0], fidl::ErrTypeCannotBeConvertedToType);
  EXPECT_ERR(library.errors()[1], fidl::ErrCannotResolveConstantValue);
  EXPECT_ERR(library.errors()[2], fidl::ErrCouldNotResolveMemberDefault);
}

TEST(StructsTests, CannotReferToIntMember) {
  TestLibrary library;
  library.AddFile("bad/fi-0053-a.test.fidl");

  library.ExpectFail(fidl::ErrCannotReferToMember, "struct 'Person'");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(StructsTests, CannotReferToStructMember) {
  TestLibrary library;
  library.AddFile("bad/fi-0053-b.test.fidl");

  library.ExpectFail(fidl::ErrCannotReferToMember, "struct 'Person'");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

}  // namespace
