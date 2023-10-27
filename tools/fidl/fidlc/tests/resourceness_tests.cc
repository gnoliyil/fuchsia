// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zxtest/zxtest.h>

#include "tools/fidl/fidlc/include/fidl/diagnostics.h"
#include "tools/fidl/fidlc/include/fidl/flat_ast.h"
#include "tools/fidl/fidlc/include/fidl/source_file.h"
#include "tools/fidl/fidlc/tests/error_test.h"
#include "tools/fidl/fidlc/tests/test_library.h"

namespace {

TEST(ResourcenessTests, BadBitsResourceness) {
  TestLibrary library(R"FIDL(
library example;
type Foo = resource bits {
    BAR = 1;
};
)FIDL");
  library.ExpectFail(fidl::ErrCannotSpecifyModifier,
                     fidl::Token::KindAndSubkind(fidl::Token::Subkind::kResource),
                     fidl::Token::KindAndSubkind(fidl::Token::Subkind::kBits));
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(ResourcenessTests, BadEnumResourceness) {
  TestLibrary library(R"FIDL(
library example;
type Foo = resource enum {
    BAR = 1;
};
)FIDL");
  library.ExpectFail(fidl::ErrCannotSpecifyModifier,
                     fidl::Token::KindAndSubkind(fidl::Token::Subkind::kResource),
                     fidl::Token::KindAndSubkind(fidl::Token::Subkind::kEnum));
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(ResourcenessTests, BadConstResourceness) {
  TestLibrary library(R"FIDL(
library example;

resource const BAR uint32 = 1;
)FIDL");
  library.ExpectFail(fidl::ErrExpectedDeclaration, "resource");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(ResourcenessTests, BadProtocolResourceness) {
  TestLibrary library(R"FIDL(
library example;

resource protocol Foo {};
)FIDL");
  library.ExpectFail(fidl::ErrExpectedDeclaration, "resource");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(ResourcenessTests, BadAliasResourceness) {
  TestLibrary library(R"FIDL(
library example;

resource alias B = bool;
)FIDL");
  library.ExpectFail(fidl::ErrExpectedDeclaration, "resource");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(ResourcenessTests, BadDuplicateModifier) {
  TestLibrary library(R"FIDL(
library example;

type One = resource struct {};
type Two = resource resource struct {};            // line 5
type Three = resource resource resource struct {}; // line 6
)FIDL");
  ASSERT_FALSE(library.Compile());

  const auto& errors = library.errors();
  ASSERT_EQ(errors.size(), 3);
  ASSERT_ERR(errors[0], fidl::ErrDuplicateModifier);
  EXPECT_EQ(errors[0]->span.position().line, 5);
  ASSERT_SUBSTR(errors[0]->msg.c_str(), "resource");
  ASSERT_ERR(errors[1], fidl::ErrDuplicateModifier);
  EXPECT_EQ(errors[1]->span.position().line, 6);
  ASSERT_SUBSTR(errors[1]->msg.c_str(), "resource");
  ASSERT_ERR(errors[2], fidl::ErrDuplicateModifier);
  EXPECT_EQ(errors[2]->span.position().line, 6);
  ASSERT_SUBSTR(errors[2]->msg.c_str(), "resource");
}

TEST(ResourcenessTests, GoodResourceSimple) {
  TestLibrary library;
  library.UseLibraryZx();
  library.AddFile("good/fi-0110-a.test.fidl");

  ASSERT_COMPILED(library);
}

TEST(ResourcenessTests, BadResourceModifierMissing) {
  TestLibrary library;
  library.UseLibraryZx();
  library.AddFile("bad/fi-0110.test.fidl");

  library.ExpectFail(fidl::ErrTypeMustBeResource, "Foo", "handle", "struct");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(ResourcenessTests, GoodResourceStruct) {
  for (const std::string& definition : {
           "type Foo =  resource struct {};",
           "type Foo = resource struct { b bool; };",
           "using zx;\ntype Foo = resource struct{ h zx.Handle; };",
           "using zx;\ntype Foo = resource struct{ a array<zx.Handle, 1>; };",
           "using zx;\ntype Foo = resource struct{ v vector<zx.Handle>; };",
       }) {
    std::string fidl_library = "library example;\n\n" + definition + "\n";
    TestLibrary library(fidl_library);
    library.UseLibraryZx();
    ASSERT_COMPILED(library);
    EXPECT_EQ(library.LookupStruct("Foo")->resourceness, fidl::types::Resourceness::kResource, "%s",
              fidl_library.c_str());
  }
}

TEST(ResourcenessTests, GoodResourceTable) {
  for (const std::string& definition : {
           "type Foo = resource table {};",
           "type Foo = resource table { 1: b bool; };",
           "using zx;\ntype Foo = resource table { 1: h zx.Handle; };",
           "using zx;\ntype Foo = resource table { 1: a array<zx.Handle, 1>; };",
           "using zx;\ntype Foo = resource table { 1: v vector<zx.Handle>; };",
       }) {
    std::string fidl_library = "library example;\n\n" + definition + "\n";
    TestLibrary library(fidl_library);
    library.UseLibraryZx();
    ASSERT_COMPILED(library);
    EXPECT_EQ(library.LookupTable("Foo")->resourceness, fidl::types::Resourceness::kResource, "%s",
              fidl_library.c_str());
  }
}

TEST(ResourcenessTests, GoodResourceUnion) {
  for (const std::string& definition : {
           "type Foo = resource union { 1: b bool; };",
           "using zx;\ntype Foo = resource union { 1: h zx.Handle; };",
           "using zx;\ntype Foo = resource union { 1: a array<zx.Handle, 1>; };",
           "using zx;\ntype Foo = resource union { 1: v vector<zx.Handle>; };",
       }) {
    std::string fidl_library = "library example;\n\n" + definition + "\n";
    TestLibrary library(fidl_library);
    library.UseLibraryZx();
    ASSERT_COMPILED(library);
    EXPECT_EQ(library.LookupUnion("Foo")->resourceness, fidl::types::Resourceness::kResource, "%s",
              fidl_library.c_str());
  }
}

TEST(ResourcenessTests, BadHandlesInValueStruct) {
  for (const std::string& definition : {
           "type Foo = struct { bad_member zx.Handle; };",
           "type Foo = struct { bad_member zx.Handle:optional; };",
           "type Foo = struct { bad_member array<zx.Handle, 1>; };",
           "type Foo = struct { bad_member vector<zx.Handle>; };",
           "type Foo = struct { bad_member vector<zx.Handle>:0; };",
       }) {
    std::string fidl_library = "library example;\nusing zx;\n\n" + definition + "\n";
    TestLibrary library(fidl_library);
    library.UseLibraryZx();
    library.ExpectFail(fidl::ErrTypeMustBeResource, "Foo", "bad_member", "struct");
    ASSERT_COMPILER_DIAGNOSTICS(library);
  }
}

TEST(ResourcenessTests, BadHandlesInValueTable) {
  for (const std::string& definition : {
           "type Foo = table { 1: bad_member zx.Handle; };",
           "type Foo = table { 1: bad_member array<zx.Handle, 1>; };",
           "type Foo = table { 1: bad_member vector<zx.Handle>; };",
           "type Foo = table { 1: bad_member vector<zx.Handle>:0; };",
       }) {
    std::string fidl_library = "library example;\nusing zx;\n\n" + definition + "\n";
    TestLibrary library(fidl_library);
    library.UseLibraryZx();
    library.ExpectFail(fidl::ErrTypeMustBeResource, "Foo", "bad_member", "table");
    ASSERT_COMPILER_DIAGNOSTICS(library);
  }
}

TEST(ResourcenessTests, BadHandlesInValueUnion) {
  for (const std::string& definition : {
           "type Foo = union { 1: bad_member zx.Handle; };",
           "type Foo = union { 1: bad_member array<zx.Handle, 1>; };",
           "type Foo = union { 1: bad_member vector<zx.Handle>; };",
           "type Foo = union { 1: bad_member vector<zx.Handle>:0; };",
       }) {
    std::string fidl_library = "library example;\nusing zx;\n\n" + definition + "\n";
    TestLibrary library(fidl_library);
    library.UseLibraryZx();
    library.ExpectFail(fidl::ErrTypeMustBeResource, "Foo", "bad_member", "union");
    ASSERT_COMPILER_DIAGNOSTICS(library);
  }
}

TEST(ResourcenessTests, BadProtocolsInValueType) {
  for (const std::string& definition : {
           "type Foo = struct { bad_member client_end:Protocol; };",
           "type Foo = struct { bad_member client_end:<Protocol, optional>; };",
           "type Foo = struct { bad_member server_end:Protocol; };",
           "type Foo = struct { bad_member server_end:<Protocol, optional>; };",
       }) {
    std::string fidl_library = R"FIDL(
library example;
using zx;

protocol Protocol {};

)FIDL" + definition + "\n";
    TestLibrary library(fidl_library);
    library.UseLibraryZx();
    library.ExpectFail(fidl::ErrTypeMustBeResource, "Foo", "bad_member", "struct");
    ASSERT_COMPILER_DIAGNOSTICS(library);
  }
}
TEST(ResourcenessTests, BadResourceTypesInValueType) {
  for (const std::string& definition : {
           "type Foo = struct { bad_member ResourceStruct; };",
           "type Foo = struct { bad_member box<ResourceStruct>; };",
           "type Foo = struct { bad_member ResourceTable; };",
           "type Foo = struct { bad_member ResourceUnion; };",
           "type Foo = struct { bad_member ResourceUnion:optional; };",
       }) {
    std::string fidl_library = R"FIDL(
library example;

type ResourceStruct = resource struct {};
type ResourceTable = resource table {};
type ResourceUnion = resource union { 1: b bool; };

)FIDL" + definition + "\n";
    TestLibrary library(fidl_library);
    library.UseLibraryZx();
    library.ExpectFail(fidl::ErrTypeMustBeResource, "Foo", "bad_member", "struct");
    ASSERT_COMPILER_DIAGNOSTICS(library);
  }
}

TEST(ResourcenessTests, BadResourceAliasesInValueType) {
  for (const std::string& definition : {
           "type Foo = struct { bad_member HandleAlias; };",
           "type Foo = struct { bad_member ProtocolAlias; };",
           "type Foo = struct { bad_member ResourceStructAlias; };",
           "type Foo = struct { bad_member ResourceTableAlias; };",
           "type Foo = struct { bad_member ResourceUnionAlias; };",
       }) {
    std::string fidl_library = R"FIDL(
library example;
using zx;

alias HandleAlias = zx.Handle;
alias ProtocolAlias = client_end:Protocol;
alias ResourceStructAlias = ResourceStruct;
alias ResourceTableAlias = ResourceStruct;
alias ResourceUnionAlias = ResourceStruct;

protocol Protocol {};
type ResourceStruct = resource struct {};
type ResourceTable = resource table {};
type ResourceUnion = resource union { 1: b bool; };

)FIDL" + definition + "\n";
    TestLibrary library(fidl_library);
    library.UseLibraryZx();
    library.ExpectFail(fidl::ErrTypeMustBeResource, "Foo", "bad_member", "struct");
    ASSERT_COMPILER_DIAGNOSTICS(library);
  }
}

TEST(ResourcenessTests, BadResourcesInNestedContainers) {
  for (
      const std::string& definition : {
          "type Foo = struct { bad_member vector<vector<zx.Handle>>; };",
          "type Foo = struct { bad_member vector<vector<zx.Handle:optional>>; };",
          "type Foo = struct { bad_member vector<vector<client_end:Protocol>>; };",
          "type Foo = struct { bad_member vector<vector<ResourceStruct>>; };",
          "type Foo = struct { bad_member vector<vector<ResourceTable>>; };",
          "type Foo = struct { bad_member vector<vector<ResourceUnion>>; };",
          "type Foo = struct { bad_member vector<array<vector<ResourceStruct>:optional,2>>:optional; };",
      }) {
    std::string fidl_library = R"FIDL(
library example;
using zx;

protocol Protocol {};
type ResourceStruct = resource struct {};
type ResourceTable = resource table {};
type ResourceUnion = resource union { 1: b bool; };

)FIDL" + definition + "\n";
    TestLibrary library(fidl_library);
    library.UseLibraryZx();
    library.ExpectFail(fidl::ErrTypeMustBeResource, "Foo", "bad_member", "struct");
    ASSERT_COMPILER_DIAGNOSTICS(library);
  }
}

TEST(ResourcenessTests, BadMultipleResourceTypesInValueType) {
  TestLibrary library(R"FIDL(
library example;
using zx;

type Foo = struct {
  first zx.Handle;
  second zx.Handle:optional;
  third ResourceStruct;
};

type ResourceStruct = resource struct {};
)FIDL");
  library.UseLibraryZx();
  library.ExpectFail(fidl::ErrTypeMustBeResource, "Foo", "first", "struct");
  library.ExpectFail(fidl::ErrTypeMustBeResource, "Foo", "second", "struct");
  library.ExpectFail(fidl::ErrTypeMustBeResource, "Foo", "third", "struct");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(ResourcenessTests, GoodTransitiveResourceMember) {
  std::string fidl_library = R"FIDL(library example;

type Top = resource struct {
    middle Middle;
};
type Middle = resource struct {
    bottom Bottom;
};
type Bottom = resource struct {};
)FIDL";

  TestLibrary library(fidl_library);
  ASSERT_COMPILED(library);
  EXPECT_EQ(library.LookupStruct("Top")->resourceness, fidl::types::Resourceness::kResource);
}

TEST(ResourcenessTests, BadTransitiveResourceMember) {
  TestLibrary library(R"FIDL(
library example;

type Top = struct {
  middle Middle;
};
type Middle = struct {
  bottom Bottom;
};
type Bottom = resource struct {};
)FIDL");
  // `Top` must be a resource because it includes `middle`, an *effective* resource.
  library.ExpectFail(fidl::ErrTypeMustBeResource, "Top", "middle", "struct");
  // `Middle` must be a resource because it includes `bottom`, a *nominal* resource.
  library.ExpectFail(fidl::ErrTypeMustBeResource, "Middle", "bottom", "struct");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(ResourcenessTests, GoodRecursiveValueTypes) {
  std::string fidl_library = R"FIDL(library example;

type Ouro = struct {
    b box<Boros>;
};

type Boros = struct {
    o box<Ouro>;
};
)FIDL";

  TestLibrary library(fidl_library);
  ASSERT_COMPILED(library);
}

TEST(ResourcenessTests, GoodRecursiveResourceTypes) {
  std::string fidl_library = R"FIDL(library example;

type Ouro = resource struct {
    b box<Boros>;
};

type Boros = resource struct {
    o box<Ouro>;
};
)FIDL";

  TestLibrary library(fidl_library);
  ASSERT_COMPILED(library);
}

TEST(ResourcenessTests, BadRecursiveResourceTypes) {
  TestLibrary library(R"FIDL(
library example;

type Ouro = resource struct {
  b box<Boros>;
};

type Boros = struct {
  bad_member box<Ouro>;
};
)FIDL");
  library.ExpectFail(fidl::ErrTypeMustBeResource, "Boros", "bad_member", "struct");
  ASSERT_COMPILER_DIAGNOSTICS(library);
}

TEST(ResourcenessTests, GoodStrictResourceOrderIndependent) {
  TestLibrary library(R"FIDL(library example;

type SR = strict resource union {
    1: b bool;
};
type RS = strict resource union {
    1: b bool;
};
)FIDL");
  ASSERT_COMPILED(library);

  const auto strict_resource = library.LookupUnion("SR");
  EXPECT_EQ(strict_resource->strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(strict_resource->resourceness, fidl::types::Resourceness::kResource);

  const auto resource_strict = library.LookupUnion("RS");
  EXPECT_EQ(resource_strict->strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(resource_strict->resourceness, fidl::types::Resourceness::kResource);
}

}  // namespace
