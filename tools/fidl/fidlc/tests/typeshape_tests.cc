// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "tools/fidl/fidlc/include/fidl/flat_ast.h"
#include "tools/fidl/fidlc/include/fidl/type_shape.h"
#include "tools/fidl/fidlc/tests/test_library.h"

namespace {

const std::string kPrologWithHandleDefinition(R"FIDL(
library example;

type ObjType = enum : uint32 {
    NONE = 0;
    PROCESS = 1;
    THREAD = 2;
    VMO = 3;
    CHANNEL = 4;
    PORT = 6;
    TIMER = 22;
};

resource_definition handle : uint32 {
    properties {
        subtype ObjType;
    };
};
)FIDL");

struct Expected {
  uint32_t inline_size = 0;
  uint32_t alignment = 0;
  uint32_t max_out_of_line = 0;
  uint32_t max_handles = 0;
  uint32_t depth = 0;
  bool has_padding = false;
  bool has_flexible_envelope = false;
};

#define EXPECT_TYPE_SHAPE(object, expected)   \
  {                                           \
    SCOPED_TRACE("EXPECT_TYPE_SHAPE failed"); \
    ExpectTypeShape((object), (expected));    \
  }

void ExpectTypeShape(const fidl::flat::Object* object, Expected expected) {
  auto actual = fidl::TypeShape(object, fidl::WireFormat::kV2);
  EXPECT_EQ(expected.inline_size, actual.inline_size);
  EXPECT_EQ(expected.alignment, actual.alignment);
  EXPECT_EQ(expected.max_out_of_line, actual.max_out_of_line);
  EXPECT_EQ(expected.max_handles, actual.max_handles);
  EXPECT_EQ(expected.depth, actual.depth);
  EXPECT_EQ(expected.has_padding, actual.has_padding);
  EXPECT_EQ(expected.has_flexible_envelope, actual.has_flexible_envelope);
}

struct ExpectedField {
  uint32_t offset = 0;
  uint32_t padding = 0;
};

#define EXPECT_FIELD_SHAPE(field, expected)    \
  {                                            \
    SCOPED_TRACE("EXPECT_FIELD_SHAPE failed"); \
    ExpectFieldShape((field), (expected));     \
  }

template <typename T>
void ExpectFieldShape(const T& field, ExpectedField expected) {
  auto actual = fidl::FieldShape(field, fidl::WireFormat::kV2);
  EXPECT_EQ(expected.offset, actual.offset);
  EXPECT_EQ(expected.padding, actual.padding);
}

TEST(TypeshapeTests, GoodEmptyStruct) {
  TestLibrary test_library(R"FIDL(library example;

type Empty = struct {};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto empty = test_library.LookupStruct("Empty");
  ASSERT_NE(empty, nullptr);
  EXPECT_TYPE_SHAPE(empty, (Expected{
                               .inline_size = 1,
                               .alignment = 1,
                           }));
  ASSERT_EQ(empty->members.size(), 0u);
}

TEST(TypeshapeTests, GoodEmptyStructWithinAnotherStruct) {
  TestLibrary test_library(R"FIDL(library example;

type Empty = struct {};

// Size = 1 byte for |bool a|
//      + 1 byte for |Empty b|
//      + 2 bytes for |int16 c|
//      + 1 bytes for |Empty d|
//      + 3 bytes padding
//      + 4 bytes for |int32 e|
//      + 2 bytes for |int16 f|
//      + 1 byte for |Empty g|
//      + 1 byte for |Empty h|
//      = 16 bytes
//
// Alignment = 4 bytes stemming from largest member (int32).
//
type EmptyWithOtherThings = struct {
    a bool;
    // no padding
    b Empty;
    // no padding
    c int16;
    // no padding
    d Empty;
    // 3 bytes padding
    e int32;
    // no padding
    f int16;
    // no padding
    g Empty;
    // no padding
    h Empty;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto empty_with_other_things = test_library.LookupStruct("EmptyWithOtherThings");
  ASSERT_NE(empty_with_other_things, nullptr);
  EXPECT_TYPE_SHAPE(empty_with_other_things, (Expected{
                                                 .inline_size = 16,
                                                 .alignment = 4,
                                                 .has_padding = true,
                                             }));
  ASSERT_EQ(empty_with_other_things->members.size(), 8u);
  // bool a;
  EXPECT_FIELD_SHAPE(empty_with_other_things->members[0], (ExpectedField{}));
  // Empty b;
  EXPECT_FIELD_SHAPE(empty_with_other_things->members[1], (ExpectedField{
                                                              .offset = 1,
                                                          }));
  // int16 c;
  EXPECT_FIELD_SHAPE(empty_with_other_things->members[2], (ExpectedField{
                                                              .offset = 2,
                                                          }));
  // Empty d;
  EXPECT_FIELD_SHAPE(empty_with_other_things->members[3],
                     (ExpectedField{.offset = 4, .padding = 3}));
  // int32 e;
  EXPECT_FIELD_SHAPE(empty_with_other_things->members[4], (ExpectedField{
                                                              .offset = 8,
                                                          }));
  // int16 f;
  EXPECT_FIELD_SHAPE(empty_with_other_things->members[5], (ExpectedField{
                                                              .offset = 12,
                                                          }));
  // Empty g;
  EXPECT_FIELD_SHAPE(empty_with_other_things->members[6], (ExpectedField{
                                                              .offset = 14,
                                                          }));
  // Empty h;
  EXPECT_FIELD_SHAPE(empty_with_other_things->members[7], (ExpectedField{
                                                              .offset = 15,
                                                          }));
}

TEST(TypeshapeTests, GoodSimpleNewTypes) {
  TestLibrary test_library(R"FIDL(library example;

type BoolAndU32 = struct {
    b bool;
    u uint32;
};
type NewBoolAndU32 = BoolAndU32;

type BitsImplicit = strict bits {
    VALUE = 1;
};
type NewBitsImplicit = BitsImplicit;


type TableWithBoolAndU32 = table {
    1: b bool;
    2: u uint32;
};
type NewTableWithBoolAndU32 = TableWithBoolAndU32;

type BoolAndU64 = struct {
    b bool;
    u uint64;
};
type UnionOfThings = strict union {
    1: ob bool;
    2: bu BoolAndU64;
};
type NewUnionOfThings = UnionOfThings;
)FIDL");
  test_library.EnableFlag(fidl::ExperimentalFlags::Flag::kAllowNewTypes);
  ASSERT_COMPILED(test_library);

  auto new_bool_and_u32_struct = test_library.LookupNewType("NewBoolAndU32");
  ASSERT_NE(new_bool_and_u32_struct, nullptr);
  EXPECT_TYPE_SHAPE(new_bool_and_u32_struct, (Expected{
                                                 .inline_size = 8,
                                                 .alignment = 4,
                                                 .has_padding = true,
                                             }));

  auto new_bits_implicit = test_library.LookupNewType("NewBitsImplicit");
  ASSERT_NE(new_bits_implicit, nullptr);
  EXPECT_TYPE_SHAPE(new_bits_implicit, (Expected{
                                           .inline_size = 4,
                                           .alignment = 4,
                                       }));

  auto new_bool_and_u32_table = test_library.LookupNewType("NewTableWithBoolAndU32");
  ASSERT_NE(new_bool_and_u32_table, nullptr);
  EXPECT_TYPE_SHAPE(new_bool_and_u32_table, (Expected{
                                                .inline_size = 16,
                                                .alignment = 8,
                                                .max_out_of_line = 16,
                                                .depth = 2,
                                                .has_padding = true,
                                                .has_flexible_envelope = true,
                                            }));

  auto new_union = test_library.LookupNewType("NewUnionOfThings");
  ASSERT_NE(new_union, nullptr);
  EXPECT_TYPE_SHAPE(new_union, (Expected{
                                   .inline_size = 16,
                                   .alignment = 8,
                                   .max_out_of_line = 16,
                                   .depth = 1,
                                   .has_padding = true,
                               }));
}

TEST(TypeshapeTests, GoodSimpleStructs) {
  TestLibrary test_library(R"FIDL(library example;

type OneBool = struct {
    b bool;
};

type TwoBools = struct {
    a bool;
    b bool;
};

type BoolAndU32 = struct {
    b bool;
    u uint32;
};

type BoolAndU64 = struct {
    b bool;
    u uint64;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto one_bool = test_library.LookupStruct("OneBool");
  ASSERT_NE(one_bool, nullptr);
  EXPECT_TYPE_SHAPE(one_bool, (Expected{
                                  .inline_size = 1,
                                  .alignment = 1,
                              }));
  ASSERT_EQ(one_bool->members.size(), 1u);
  EXPECT_FIELD_SHAPE(one_bool->members[0], (ExpectedField{}));

  auto two_bools = test_library.LookupStruct("TwoBools");
  ASSERT_NE(two_bools, nullptr);
  EXPECT_TYPE_SHAPE(two_bools, (Expected{
                                   .inline_size = 2,
                                   .alignment = 1,
                               }));
  ASSERT_EQ(two_bools->members.size(), 2u);
  EXPECT_FIELD_SHAPE(two_bools->members[0], (ExpectedField{}));
  EXPECT_FIELD_SHAPE(two_bools->members[1], (ExpectedField{
                                                .offset = 1,
                                            }));

  auto bool_and_u32 = test_library.LookupStruct("BoolAndU32");
  ASSERT_NE(bool_and_u32, nullptr);
  EXPECT_TYPE_SHAPE(bool_and_u32, (Expected{
                                      .inline_size = 8,
                                      .alignment = 4,
                                      .has_padding = true,
                                  }));
  ASSERT_EQ(bool_and_u32->members.size(), 2u);
  EXPECT_FIELD_SHAPE(bool_and_u32->members[0], (ExpectedField{.padding = 3}));
  EXPECT_FIELD_SHAPE(bool_and_u32->members[1], (ExpectedField{
                                                   .offset = 4,
                                               }));

  auto bool_and_u64 = test_library.LookupStruct("BoolAndU64");
  ASSERT_NE(bool_and_u64, nullptr);
  EXPECT_TYPE_SHAPE(bool_and_u64, (Expected{
                                      .inline_size = 16,
                                      .alignment = 8,
                                      .has_padding = true,
                                  }));
  ASSERT_EQ(bool_and_u64->members.size(), 2u);
  EXPECT_FIELD_SHAPE(bool_and_u64->members[0], (ExpectedField{.padding = 7}));
  EXPECT_FIELD_SHAPE(bool_and_u64->members[1], (ExpectedField{
                                                   .offset = 8,
                                               }));
}

TEST(TypeshapeTests, GoodSimpleStructsWithHandles) {
  TestLibrary test_library(kPrologWithHandleDefinition + R"FIDL(
type OneHandle = resource struct {
  h handle;
};

type TwoHandles = resource struct {
  h1 handle:CHANNEL;
  h2 handle:PORT;
};

type ThreeHandlesOneOptional = resource struct {
  h1 handle:CHANNEL;
  h2 handle:PORT;
  opt_h3 handle:<TIMER, optional>;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto one_handle = test_library.LookupStruct("OneHandle");
  ASSERT_NE(one_handle, nullptr);
  EXPECT_TYPE_SHAPE(one_handle, (Expected{
                                    .inline_size = 4,
                                    .alignment = 4,
                                    .max_handles = 1,
                                }));
  ASSERT_EQ(one_handle->members.size(), 1u);
  EXPECT_FIELD_SHAPE(one_handle->members[0], (ExpectedField{}));

  auto two_handles = test_library.LookupStruct("TwoHandles");
  ASSERT_NE(two_handles, nullptr);
  EXPECT_TYPE_SHAPE(two_handles, (Expected{
                                     .inline_size = 8,
                                     .alignment = 4,
                                     .max_handles = 2,
                                 }));
  ASSERT_EQ(two_handles->members.size(), 2u);
  EXPECT_FIELD_SHAPE(two_handles->members[0], (ExpectedField{}));
  EXPECT_FIELD_SHAPE(two_handles->members[1], (ExpectedField{
                                                  .offset = 4,
                                              }));

  auto three_handles_one_optional = test_library.LookupStruct("ThreeHandlesOneOptional");
  ASSERT_NE(three_handles_one_optional, nullptr);
  EXPECT_TYPE_SHAPE(three_handles_one_optional, (Expected{
                                                    .inline_size = 12,
                                                    .alignment = 4,
                                                    .max_handles = 3,
                                                }));
  ASSERT_EQ(three_handles_one_optional->members.size(), 3u);
  EXPECT_FIELD_SHAPE(three_handles_one_optional->members[0], (ExpectedField{}));
  EXPECT_FIELD_SHAPE(three_handles_one_optional->members[1], (ExpectedField{
                                                                 .offset = 4,
                                                             }));
  EXPECT_FIELD_SHAPE(three_handles_one_optional->members[2], (ExpectedField{
                                                                 .offset = 8,
                                                             }));
}

TEST(TypeshapeTests, GoodBits) {
  TestLibrary test_library(R"FIDL(library example;

type Bits16 = strict bits : uint16 {
    VALUE = 1;
};

type BitsImplicit = strict bits {
    VALUE = 1;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto bits16 = test_library.LookupBits("Bits16");
  ASSERT_NE(bits16, nullptr);
  EXPECT_TYPE_SHAPE(bits16, (Expected{
                                .inline_size = 2,
                                .alignment = 2,
                            }));

  auto bits_implicit = test_library.LookupBits("BitsImplicit");
  ASSERT_NE(bits_implicit, nullptr);
  EXPECT_TYPE_SHAPE(bits_implicit, (Expected{
                                       .inline_size = 4,
                                       .alignment = 4,
                                   }));
}

TEST(TypeshapeTests, GoodSimpleTables) {
  TestLibrary test_library(R"FIDL(library example;

type TableWithNoMembers = table {};

type TableWithOneBool = table {
    1: b bool;
};

type TableWithTwoBools = table {
    1: a bool;
    2: b bool;
};

type TableWithBoolAndU32 = table {
    1: b bool;
    2: u uint32;
};

type TableWithBoolAndU64 = table {
    1: b bool;
    2: u uint64;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto no_members = test_library.LookupTable("TableWithNoMembers");
  ASSERT_NE(no_members, nullptr);
  EXPECT_TYPE_SHAPE(no_members, (Expected{
                                    .inline_size = 16,
                                    .alignment = 8,
                                    .depth = 1,
                                    .has_padding = false,
                                    .has_flexible_envelope = true,
                                }));

  auto one_bool = test_library.LookupTable("TableWithOneBool");
  ASSERT_NE(one_bool, nullptr);
  EXPECT_TYPE_SHAPE(one_bool, (Expected{
                                  .inline_size = 16,
                                  .alignment = 8,
                                  .max_out_of_line = 8,
                                  .depth = 2,
                                  .has_padding = true,
                                  .has_flexible_envelope = true,
                              }));

  auto two_bools = test_library.LookupTable("TableWithTwoBools");
  ASSERT_NE(two_bools, nullptr);
  EXPECT_TYPE_SHAPE(two_bools, (Expected{
                                   .inline_size = 16,
                                   .alignment = 8,
                                   .max_out_of_line = 16,
                                   .depth = 2,
                                   .has_padding = true,
                                   .has_flexible_envelope = true,
                               }));

  auto bool_and_u32 = test_library.LookupTable("TableWithBoolAndU32");
  ASSERT_NE(bool_and_u32, nullptr);
  EXPECT_TYPE_SHAPE(bool_and_u32, (Expected{
                                      .inline_size = 16,
                                      .alignment = 8,
                                      .max_out_of_line = 16,
                                      .depth = 2,
                                      .has_padding = true,
                                      .has_flexible_envelope = true,
                                  }));

  auto bool_and_u64 = test_library.LookupTable("TableWithBoolAndU64");
  ASSERT_NE(bool_and_u64, nullptr);
  EXPECT_TYPE_SHAPE(bool_and_u64, (Expected{
                                      .inline_size = 16,
                                      .alignment = 8,
                                      .max_out_of_line = 24,
                                      .depth = 2,
                                      .has_padding = true,
                                      .has_flexible_envelope = true,
                                  }));
}

TEST(TypeshapeTests, GoodTablesWithReservedFields) {
  TestLibrary test_library(R"FIDL(library example;

type SomeReserved = table {
    1: b bool;
    2: reserved;
    3: b2 bool;
    4: reserved;
};

type LastNonReserved = table {
    1: reserved;
    2: reserved;
    3: b bool;
};

type LastReserved = table {
    1: b bool;
    2: b2 bool;
    3: reserved;
    4: reserved;
};

type AllReserved = table {
    1: reserved;
    2: reserved;
    3: reserved;
};

type OneReserved = table {
    1: reserved;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto some_reserved = test_library.LookupTable("SomeReserved");
  ASSERT_NE(some_reserved, nullptr);
  EXPECT_TYPE_SHAPE(some_reserved, (Expected{
                                       .inline_size = 16,
                                       .alignment = 8,
                                       .max_out_of_line = 24,
                                       .depth = 2,
                                       .has_padding = true,
                                       .has_flexible_envelope = true,
                                   }));

  auto last_non_reserved = test_library.LookupTable("LastNonReserved");
  ASSERT_NE(last_non_reserved, nullptr);
  EXPECT_TYPE_SHAPE(last_non_reserved, (Expected{
                                           .inline_size = 16,
                                           .alignment = 8,
                                           .max_out_of_line = 24,
                                           .depth = 2,
                                           .has_padding = true,
                                           .has_flexible_envelope = true,
                                       }));

  auto last_reserved = test_library.LookupTable("LastReserved");
  ASSERT_NE(last_reserved, nullptr);
  EXPECT_TYPE_SHAPE(last_reserved, (Expected{
                                       .inline_size = 16,
                                       .alignment = 8,
                                       .max_out_of_line = 16,
                                       .depth = 2,
                                       .has_padding = true,
                                       .has_flexible_envelope = true,
                                   }));

  auto all_reserved = test_library.LookupTable("AllReserved");
  ASSERT_NE(all_reserved, nullptr);
  EXPECT_TYPE_SHAPE(all_reserved, (Expected{
                                      .inline_size = 16,
                                      .alignment = 8,
                                      .max_out_of_line = 0,
                                      .depth = 1,
                                      .has_padding = false,
                                      .has_flexible_envelope = true,
                                  }));

  auto one_reserved = test_library.LookupTable("OneReserved");
  ASSERT_NE(one_reserved, nullptr);
  EXPECT_TYPE_SHAPE(one_reserved, (Expected{
                                      .inline_size = 16,
                                      .alignment = 8,
                                      .max_out_of_line = 0,
                                      .depth = 1,
                                      .has_padding = false,
                                      .has_flexible_envelope = true,
                                  }));
}

TEST(TypeshapeTests, GoodSimpleTablesWithHandles) {
  TestLibrary test_library(R"FIDL(
library example;
using zx;

type TableWithOneHandle = resource table {
  1: h zx.Handle;
};

)FIDL");
  test_library.UseLibraryZx();
  ASSERT_COMPILED(test_library);

  auto one_handle = test_library.LookupTable("TableWithOneHandle");
  ASSERT_NE(one_handle, nullptr);
  EXPECT_TYPE_SHAPE(one_handle, (Expected{
                                    .inline_size = 16,
                                    .alignment = 8,
                                    .max_out_of_line = 8,
                                    .max_handles = 1,
                                    .depth = 2,
                                    .has_padding = true,
                                    .has_flexible_envelope = true,
                                }));
}

TEST(TypeshapeTests, GoodOptionalStructs) {
  TestLibrary test_library(R"FIDL(library example;

type OneBool = struct {
    b bool;
};

type OptionalOneBool = struct {
    s box<OneBool>;
};

type TwoBools = struct {
    a bool;
    b bool;
};

type OptionalTwoBools = struct {
    s box<TwoBools>;
};

type BoolAndU32 = struct {
    b bool;
    u uint32;
};

type OptionalBoolAndU32 = struct {
    s box<BoolAndU32>;
};

type BoolAndU64 = struct {
    b bool;
    u uint64;
};

type OptionalBoolAndU64 = struct {
    s box<BoolAndU64>;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto one_bool = test_library.LookupStruct("OptionalOneBool");
  ASSERT_NE(one_bool, nullptr);
  EXPECT_TYPE_SHAPE(one_bool, (Expected{
                                  .inline_size = 8,
                                  .alignment = 8,
                                  .max_out_of_line = 8,
                                  .depth = 1,
                                  .has_padding = true,
                              }));

  auto two_bools = test_library.LookupStruct("OptionalTwoBools");
  ASSERT_NE(two_bools, nullptr);
  EXPECT_TYPE_SHAPE(two_bools, (Expected{
                                   .inline_size = 8,
                                   .alignment = 8,
                                   .max_out_of_line = 8,
                                   .depth = 1,
                                   .has_padding = true,
                               }));

  auto bool_and_u32 = test_library.LookupStruct("OptionalBoolAndU32");
  ASSERT_NE(bool_and_u32, nullptr);
  EXPECT_TYPE_SHAPE(bool_and_u32, (Expected{
                                      .inline_size = 8,
                                      .alignment = 8,
                                      .max_out_of_line = 8,
                                      .depth = 1,
                                      .has_padding = true,  // because |BoolAndU32| has padding
                                  }));

  auto bool_and_u64 = test_library.LookupStruct("OptionalBoolAndU64");
  ASSERT_NE(bool_and_u64, nullptr);
  EXPECT_TYPE_SHAPE(bool_and_u64, (Expected{
                                      .inline_size = 8,
                                      .alignment = 8,
                                      .max_out_of_line = 16,
                                      .depth = 1,
                                      .has_padding = true,  // because |BoolAndU64| has padding
                                  }));
}

TEST(TypeshapeTests, GoodOptionalTables) {
  TestLibrary test_library(R"FIDL(library example;

type OneBool = struct {
    b bool;
};

type TableWithOptionalOneBool = table {
    1: s OneBool;
};

type TableWithOneBool = table {
    1: b bool;
};

type TableWithOptionalTableWithOneBool = table {
    1: s TableWithOneBool;
};

type TwoBools = struct {
    a bool;
    b bool;
};

type TableWithOptionalTwoBools = table {
    1: s TwoBools;
};

type TableWithTwoBools = table {
    1: a bool;
    2: b bool;
};

type TableWithOptionalTableWithTwoBools = table {
    1: s TableWithTwoBools;
};

type BoolAndU32 = struct {
    b bool;
    u uint32;
};

type TableWithOptionalBoolAndU32 = table {
    1: s BoolAndU32;
};

type TableWithBoolAndU32 = table {
    1: b bool;
    2: u uint32;
};

type TableWithOptionalTableWithBoolAndU32 = table {
    1: s TableWithBoolAndU32;
};

type BoolAndU64 = struct {
    b bool;
    u uint64;
};

type TableWithOptionalBoolAndU64 = table {
    1: s BoolAndU64;
};

type TableWithBoolAndU64 = table {
    1: b bool;
    2: u uint64;
};

type TableWithOptionalTableWithBoolAndU64 = table {
    1: s TableWithBoolAndU64;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto one_bool = test_library.LookupTable("TableWithOptionalOneBool");
  ASSERT_NE(one_bool, nullptr);
  EXPECT_TYPE_SHAPE(one_bool, (Expected{
                                  .inline_size = 16,
                                  .alignment = 8,
                                  .max_out_of_line = 8,
                                  .depth = 2,
                                  .has_padding = true,
                                  .has_flexible_envelope = true,
                              }));

  auto table_with_one_bool = test_library.LookupTable("TableWithOptionalTableWithOneBool");
  ASSERT_NE(table_with_one_bool, nullptr);
  EXPECT_TYPE_SHAPE(table_with_one_bool, (Expected{
                                             .inline_size = 16,
                                             .alignment = 8,
                                             .max_out_of_line = 32,
                                             .depth = 4,
                                             .has_padding = true,
                                             .has_flexible_envelope = true,
                                         }));

  auto two_bools = test_library.LookupTable("TableWithOptionalTwoBools");
  ASSERT_NE(two_bools, nullptr);
  EXPECT_TYPE_SHAPE(two_bools, (Expected{
                                   .inline_size = 16,
                                   .alignment = 8,
                                   .max_out_of_line = 8,
                                   .depth = 2,
                                   .has_padding = true,
                                   .has_flexible_envelope = true,
                               }));

  auto table_with_two_bools = test_library.LookupTable("TableWithOptionalTableWithTwoBools");
  ASSERT_NE(table_with_two_bools, nullptr);
  EXPECT_TYPE_SHAPE(table_with_two_bools, (Expected{
                                              .inline_size = 16,
                                              .alignment = 8,
                                              .max_out_of_line = 40,
                                              .depth = 4,
                                              .has_padding = true,
                                              .has_flexible_envelope = true,
                                          }));

  auto bool_and_u32 = test_library.LookupTable("TableWithOptionalBoolAndU32");
  ASSERT_NE(bool_and_u32, nullptr);
  EXPECT_TYPE_SHAPE(bool_and_u32, (Expected{
                                      .inline_size = 16,
                                      .alignment = 8,
                                      .max_out_of_line = 16,
                                      .depth = 2,
                                      .has_padding = true,
                                      .has_flexible_envelope = true,
                                  }));

  auto table_with_bool_and_u32 = test_library.LookupTable("TableWithOptionalTableWithBoolAndU32");
  ASSERT_NE(table_with_bool_and_u32, nullptr);
  EXPECT_TYPE_SHAPE(table_with_bool_and_u32, (Expected{
                                                 .inline_size = 16,
                                                 .alignment = 8,
                                                 .max_out_of_line = 40,
                                                 .depth = 4,
                                                 .has_padding = true,
                                                 .has_flexible_envelope = true,
                                             }));

  auto bool_and_u64 = test_library.LookupTable("TableWithOptionalBoolAndU64");
  ASSERT_NE(bool_and_u64, nullptr);
  EXPECT_TYPE_SHAPE(bool_and_u64, (Expected{
                                      .inline_size = 16,
                                      .alignment = 8,
                                      .max_out_of_line = 24,
                                      .depth = 2,
                                      .has_padding = true,
                                      .has_flexible_envelope = true,
                                  }));

  auto table_with_bool_and_u64 = test_library.LookupTable("TableWithOptionalTableWithBoolAndU64");
  ASSERT_NE(table_with_bool_and_u64, nullptr);
  EXPECT_TYPE_SHAPE(table_with_bool_and_u64, (Expected{
                                                 .inline_size = 16,
                                                 .alignment = 8,
                                                 .max_out_of_line = 48,
                                                 .depth = 4,
                                                 .has_padding = true,
                                                 .has_flexible_envelope = true,
                                             }));
}

TEST(TypeshapeTests, GoodUnions) {
  TestLibrary test_library(R"FIDL(library example;

type BoolAndU64 = struct {
    b bool;
    u uint64;
};

type UnionOfThings = strict union {
    1: ob bool;
    2: bu BoolAndU64;
};

type Bool = struct {
    b bool;
};

type OptBool = struct {
    opt_b box<Bool>;
};

type UnionWithOutOfLine = strict union {
    1: opt_bool OptBool;
};

type OptionalUnion = struct {
    u UnionOfThings:optional;
};

type TableWithOptionalUnion = table {
    1: u UnionOfThings;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto union_with_out_of_line = test_library.LookupUnion("UnionWithOutOfLine");
  EXPECT_TYPE_SHAPE(union_with_out_of_line, (Expected{
                                                .inline_size = 16,
                                                .alignment = 8,
                                                .max_out_of_line = 16,
                                                .depth = 2,
                                                .has_padding = true,
                                            }));

  auto a_union = test_library.LookupUnion("UnionOfThings");
  ASSERT_NE(a_union, nullptr);
  EXPECT_TYPE_SHAPE(a_union, (Expected{
                                 .inline_size = 16,
                                 .alignment = 8,
                                 .max_out_of_line = 16,
                                 .depth = 1,
                                 .has_padding = true,
                             }));
  ASSERT_EQ(a_union->members.size(), 2u);
  ASSERT_NE(a_union->members[0].maybe_used, nullptr);
  EXPECT_FIELD_SHAPE(*a_union->members[0].maybe_used, (ExpectedField{
                                                          .offset = 0,
                                                          .padding = 7,
                                                      }));
  ASSERT_NE(a_union->members[1].maybe_used, nullptr);
  EXPECT_FIELD_SHAPE(*a_union->members[1].maybe_used, (ExpectedField{}));

  auto optional_union = test_library.LookupStruct("OptionalUnion");
  ASSERT_NE(optional_union, nullptr);
  EXPECT_TYPE_SHAPE(optional_union, (Expected{
                                        // because |UnionOfThings| union header is inline
                                        .inline_size = 16,
                                        .alignment = 8,
                                        .max_out_of_line = 16,
                                        .depth = 1,
                                        .has_padding = true,
                                    }));

  auto table_with_optional_union = test_library.LookupTable("TableWithOptionalUnion");
  ASSERT_NE(table_with_optional_union, nullptr);
  EXPECT_TYPE_SHAPE(table_with_optional_union, (Expected{
                                                   .inline_size = 16,
                                                   .alignment = 8,
                                                   .max_out_of_line = 40,
                                                   .depth = 3,
                                                   .has_padding = true,
                                                   .has_flexible_envelope = true,
                                               }));
}

TEST(TypeshapeTests, GoodUnionsWithHandles) {
  TestLibrary test_library(R"FIDL(
library example;
using zx;

type OneHandleUnion = strict resource union {
  1: one_handle zx.Handle;
  2: one_bool bool;
  3: one_int uint32;
};

type ManyHandleUnion = strict resource union {
  1: one_handle zx.Handle;
  2: handle_array array<zx.Handle, 8>;
  3: handle_vector vector<zx.Handle>:8;
};

)FIDL");
  test_library.UseLibraryZx();
  ASSERT_COMPILED(test_library);

  auto one_handle_union = test_library.LookupUnion("OneHandleUnion");
  ASSERT_NE(one_handle_union, nullptr);
  EXPECT_TYPE_SHAPE(one_handle_union, (Expected{
                                          .inline_size = 16,
                                          .alignment = 8,
                                          .max_out_of_line = 0,
                                          .max_handles = 1,
                                          .depth = 1,
                                          .has_padding = true,
                                      }));
  ASSERT_EQ(one_handle_union->members.size(), 3u);
  ASSERT_NE(one_handle_union->members[0].maybe_used, nullptr);
  EXPECT_FIELD_SHAPE(*one_handle_union->members[0].maybe_used, (ExpectedField{
                                                                   .offset = 0,
                                                                   .padding = 4,
                                                               }));
  ASSERT_NE(one_handle_union->members[1].maybe_used, nullptr);
  EXPECT_FIELD_SHAPE(*one_handle_union->members[1].maybe_used, (ExpectedField{
                                                                   .offset = 0,
                                                                   .padding = 7,
                                                               }));
  ASSERT_NE(one_handle_union->members[2].maybe_used, nullptr);
  EXPECT_FIELD_SHAPE(*one_handle_union->members[2].maybe_used, (ExpectedField{
                                                                   .offset = 0,
                                                                   .padding = 4,
                                                               }));

  auto many_handle_union = test_library.LookupUnion("ManyHandleUnion");
  ASSERT_NE(many_handle_union, nullptr);
  EXPECT_TYPE_SHAPE(many_handle_union, (Expected{
                                           .inline_size = 16,
                                           .alignment = 8,
                                           .max_out_of_line = 48,
                                           .max_handles = 8,
                                           .depth = 2,
                                           .has_padding = true,
                                       }));
  ASSERT_EQ(many_handle_union->members.size(), 3u);
  ASSERT_NE(many_handle_union->members[1].maybe_used, nullptr);
  EXPECT_FIELD_SHAPE(*many_handle_union->members[0].maybe_used, (ExpectedField{
                                                                    .offset = 0,
                                                                    .padding = 4,
                                                                }));
  ASSERT_NE(many_handle_union->members[1].maybe_used, nullptr);
  EXPECT_FIELD_SHAPE(*many_handle_union->members[1].maybe_used, (ExpectedField{}));
  ASSERT_NE(many_handle_union->members[2].maybe_used, nullptr);
  EXPECT_FIELD_SHAPE(*many_handle_union->members[2].maybe_used, (ExpectedField{}));
}

TEST(TypeshapeTests, GoodOverlays) {
  TestLibrary test_library(R"FIDL(library example;

type BoolOrStringOrU64 = strict overlay {
    1: b bool;
    2: s string:255;
    3: u uint64;
};

type BoolOverlay = strict overlay {
    1: b bool;
};

type U64BoolStruct = struct {
    u uint64;
    b bool;
};

type BoolOverlayStruct = struct {
    bo BoolOverlay;
};

)FIDL");
  test_library.EnableFlag(fidl::ExperimentalFlags::Flag::kZxCTypes);

  ASSERT_COMPILED(test_library);
  auto bool_or_string_or_u64 = test_library.LookupOverlay("BoolOrStringOrU64");
  EXPECT_TYPE_SHAPE(bool_or_string_or_u64, (Expected{
                                               .inline_size = 24,
                                               .alignment = 8,
                                               .max_out_of_line = 256,
                                               .depth = 1,
                                               .has_padding = true,
                                           }));
  EXPECT_FIELD_SHAPE(*bool_or_string_or_u64->members[0].maybe_used,
                     (ExpectedField{.offset = 8, .padding = 15}));
  EXPECT_FIELD_SHAPE(*bool_or_string_or_u64->members[1].maybe_used,
                     (ExpectedField{.offset = 8, .padding = 0}));
  EXPECT_FIELD_SHAPE(*bool_or_string_or_u64->members[2].maybe_used,
                     (ExpectedField{.offset = 8, .padding = 8}));

  // BoolOverlay and U64BoolStruct should have basically the same typeshape.
  auto bool_overlay = test_library.LookupOverlay("BoolOverlay");
  auto u64_bool_struct = test_library.LookupStruct("U64BoolStruct");
  EXPECT_TYPE_SHAPE(bool_overlay, (Expected{
                                      .inline_size = 16,
                                      .alignment = 8,
                                      .max_out_of_line = 0,
                                      .depth = 0,
                                      .has_padding = true,
                                  }));
  EXPECT_FIELD_SHAPE(*bool_overlay->members[0].maybe_used,
                     (ExpectedField{.offset = 8, .padding = 7}));
  EXPECT_TYPE_SHAPE(u64_bool_struct, (Expected{
                                         .inline_size = 16,
                                         .alignment = 8,
                                         .max_out_of_line = 0,
                                         .depth = 0,
                                         .has_padding = true,
                                     }));

  auto bool_overlay_struct = test_library.LookupStruct("BoolOverlayStruct");
  EXPECT_FIELD_SHAPE(bool_overlay_struct->members[0], (ExpectedField{.offset = 0, .padding = 7}));
}

TEST(TypeshapeTests, GoodVectors) {
  TestLibrary test_library(R"FIDL(library example;

type PaddedVector = struct {
    pv vector<int32>:3;
};

type NoPaddingVector = struct {
    npv vector<uint64>:3;
};

type UnboundedVector = struct {
    uv vector<int32>;
};

type UnboundedVectors = struct {
    uv1 vector<int32>;
    uv2 vector<int32>;
};

type TableWithPaddedVector = table {
    1: pv vector<int32>:3;
};

type TableWithUnboundedVector = table {
    1: uv vector<int32>;
};

type TableWithUnboundedVectors = table {
    1: uv1 vector<int32>;
    2: uv2 vector<int32>;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto padded_vector = test_library.LookupStruct("PaddedVector");
  ASSERT_NE(padded_vector, nullptr);
  EXPECT_TYPE_SHAPE(padded_vector, (Expected{
                                       .inline_size = 16,
                                       .alignment = 8,
                                       .max_out_of_line = 16,
                                       .depth = 1,
                                       .has_padding = true,
                                   }));

  auto no_padding_vector = test_library.LookupStruct("NoPaddingVector");
  ASSERT_NE(no_padding_vector, nullptr);
  EXPECT_TYPE_SHAPE(no_padding_vector, (Expected{
                                           .inline_size = 16,
                                           .alignment = 8,
                                           .max_out_of_line = 24,
                                           .depth = 1,
                                           .has_padding = false,
                                       }));

  auto unbounded_vector = test_library.LookupStruct("UnboundedVector");
  ASSERT_NE(unbounded_vector, nullptr);
  EXPECT_TYPE_SHAPE(unbounded_vector, (Expected{
                                          .inline_size = 16,
                                          .alignment = 8,
                                          .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                                          .depth = 1,
                                          .has_padding = true,
                                      }));

  auto unbounded_vectors = test_library.LookupStruct("UnboundedVectors");
  ASSERT_NE(unbounded_vectors, nullptr);
  EXPECT_TYPE_SHAPE(unbounded_vectors, (Expected{
                                           .inline_size = 32,
                                           .alignment = 8,
                                           .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                                           .depth = 1,
                                           .has_padding = true,
                                       }));

  auto table_with_padded_vector = test_library.LookupTable("TableWithPaddedVector");
  ASSERT_NE(table_with_padded_vector, nullptr);
  EXPECT_TYPE_SHAPE(table_with_padded_vector, (Expected{
                                                  .inline_size = 16,
                                                  .alignment = 8,
                                                  .max_out_of_line = 40,
                                                  .depth = 3,
                                                  .has_padding = true,
                                                  .has_flexible_envelope = true,
                                              }));

  auto table_with_unbounded_vector = test_library.LookupTable("TableWithUnboundedVector");
  ASSERT_NE(table_with_unbounded_vector, nullptr);
  EXPECT_TYPE_SHAPE(table_with_unbounded_vector,
                    (Expected{
                        .inline_size = 16,
                        .alignment = 8,
                        .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                        .depth = 3,
                        .has_padding = true,
                        .has_flexible_envelope = true,
                    }));

  auto table_with_unbounded_vectors = test_library.LookupTable("TableWithUnboundedVectors");
  ASSERT_NE(table_with_unbounded_vectors, nullptr);
  EXPECT_TYPE_SHAPE(table_with_unbounded_vectors,
                    (Expected{
                        .inline_size = 16,
                        .alignment = 8,
                        .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                        .depth = 3,
                        .has_padding = true,
                        .has_flexible_envelope = true,
                    }));
}

TEST(TypeshapeTests, GoodVectorsWithHandles) {
  TestLibrary test_library(R"FIDL(
library example;
using zx;

type HandleVector = resource struct {
  hv vector<zx.Handle>:8;
};

type HandleNullableVector = resource struct {
  hv vector<zx.Handle>:<8, optional>;
};

type TableWithHandleVector = resource table {
  1: hv vector<zx.Handle>:8;
};

type UnboundedHandleVector = resource struct {
  hv vector<zx.Handle>;
};

type TableWithUnboundedHandleVector = resource table {
  1: hv vector<zx.Handle>;
};

type OneHandle = resource struct {
  h zx.Handle;
};

type HandleStructVector = resource struct {
  sv vector<OneHandle>:8;
};

type TableWithOneHandle = resource table {
  1: h zx.Handle;
};

type HandleTableVector = resource struct {
  sv vector<TableWithOneHandle>:8;
};

type TableWithHandleStructVector = resource table {
  1: sv vector<OneHandle>:8;
};

)FIDL");
  test_library.UseLibraryZx();
  ASSERT_COMPILED(test_library);

  auto handle_vector = test_library.LookupStruct("HandleVector");
  ASSERT_NE(handle_vector, nullptr);
  EXPECT_TYPE_SHAPE(handle_vector, (Expected{
                                       .inline_size = 16,
                                       .alignment = 8,
                                       .max_out_of_line = 32,
                                       .max_handles = 8,
                                       .depth = 1,
                                       .has_padding = true,
                                   }));

  auto handle_nullable_vector = test_library.LookupStruct("HandleNullableVector");
  ASSERT_NE(handle_nullable_vector, nullptr);
  EXPECT_TYPE_SHAPE(handle_nullable_vector, (Expected{
                                                .inline_size = 16,
                                                .alignment = 8,
                                                .max_out_of_line = 32,
                                                .max_handles = 8,
                                                .depth = 1,
                                                .has_padding = true,
                                            }));

  auto unbounded_handle_vector = test_library.LookupStruct("UnboundedHandleVector");
  ASSERT_NE(unbounded_handle_vector, nullptr);
  EXPECT_TYPE_SHAPE(unbounded_handle_vector,
                    (Expected{
                        .inline_size = 16,
                        .alignment = 8,
                        .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                        .max_handles = std::numeric_limits<uint32_t>::max(),
                        .depth = 1,
                        .has_padding = true,
                    }));

  auto table_with_unbounded_handle_vector =
      test_library.LookupTable("TableWithUnboundedHandleVector");
  ASSERT_NE(table_with_unbounded_handle_vector, nullptr);
  EXPECT_TYPE_SHAPE(table_with_unbounded_handle_vector,
                    (Expected{
                        .inline_size = 16,
                        .alignment = 8,
                        .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                        .max_handles = std::numeric_limits<uint32_t>::max(),
                        .depth = 3,
                        .has_padding = true,
                        .has_flexible_envelope = true,
                    }));

  auto handle_struct_vector = test_library.LookupStruct("HandleStructVector");
  ASSERT_NE(handle_struct_vector, nullptr);
  EXPECT_TYPE_SHAPE(handle_struct_vector, (Expected{
                                              .inline_size = 16,
                                              .alignment = 8,
                                              .max_out_of_line = 32,
                                              .max_handles = 8,
                                              .depth = 1,
                                              .has_padding = true,
                                          }));

  auto handle_table_vector = test_library.LookupStruct("HandleTableVector");
  ASSERT_NE(handle_table_vector, nullptr);
  EXPECT_TYPE_SHAPE(handle_table_vector, (Expected{
                                             .inline_size = 16,
                                             .alignment = 8,
                                             .max_out_of_line = 192,
                                             .max_handles = 8,
                                             .depth = 3,
                                             .has_padding = true,
                                             .has_flexible_envelope = true,
                                         }));

  auto table_with_handle_struct_vector = test_library.LookupTable("TableWithHandleStructVector");
  ASSERT_NE(table_with_handle_struct_vector, nullptr);
  EXPECT_TYPE_SHAPE(table_with_handle_struct_vector, (Expected{
                                                         .inline_size = 16,
                                                         .alignment = 8,
                                                         .max_out_of_line = 56,
                                                         .max_handles = 8,
                                                         .depth = 3,
                                                         .has_padding = true,
                                                         .has_flexible_envelope = true,
                                                     }));
}

TEST(TypeshapeTests, GoodStrings) {
  TestLibrary test_library(R"FIDL(library example;

type ShortString = struct {
    s string:5;
};

type UnboundedString = struct {
    s string;
};

type TableWithShortString = table {
    1: s string:5;
};

type TableWithUnboundedString = table {
    1: s string;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto short_string = test_library.LookupStruct("ShortString");
  ASSERT_NE(short_string, nullptr);
  EXPECT_TYPE_SHAPE(short_string, (Expected{
                                      .inline_size = 16,
                                      .alignment = 8,
                                      .max_out_of_line = 8,
                                      .depth = 1,
                                      .has_padding = true,
                                  }));

  auto unbounded_string = test_library.LookupStruct("UnboundedString");
  ASSERT_NE(unbounded_string, nullptr);
  EXPECT_TYPE_SHAPE(unbounded_string, (Expected{
                                          .inline_size = 16,
                                          .alignment = 8,
                                          .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                                          .depth = 1,
                                          .has_padding = true,
                                      }));

  auto table_with_short_string = test_library.LookupTable("TableWithShortString");
  ASSERT_NE(table_with_short_string, nullptr);
  EXPECT_TYPE_SHAPE(table_with_short_string, (Expected{
                                                 .inline_size = 16,
                                                 .alignment = 8,
                                                 .max_out_of_line = 32,
                                                 .depth = 3,
                                                 .has_padding = true,
                                                 .has_flexible_envelope = true,
                                             }));

  auto table_with_unbounded_string = test_library.LookupTable("TableWithUnboundedString");
  ASSERT_NE(table_with_unbounded_string, nullptr);
  EXPECT_TYPE_SHAPE(table_with_unbounded_string,
                    (Expected{
                        .inline_size = 16,
                        .alignment = 8,
                        .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                        .depth = 3,
                        .has_padding = true,
                        .has_flexible_envelope = true,
                    }));
}

TEST(TypeshapeTests, GoodStringArrays) {
  TestLibrary test_library(R"FIDL(library example;

type StringArray = struct {
    s string_array<5>;
};

)FIDL");
  test_library.EnableFlag(fidl::ExperimentalFlags::Flag::kZxCTypes);

  ASSERT_COMPILED(test_library);

  auto string_array = test_library.LookupStruct("StringArray");
  ASSERT_NE(string_array, nullptr);
  EXPECT_TYPE_SHAPE(string_array, (Expected{
                                      .inline_size = 5,
                                      .alignment = 1,
                                      .max_out_of_line = 0,
                                      .depth = 0,
                                      .has_padding = false,
                                  }));
}

TEST(TypeshapeTests, GoodArrays) {
  TestLibrary test_library(R"FIDL(library example;

type AnArray = struct {
    a array<int64, 5>;
};

type TableWithAnArray = table {
    1: a array<int64, 5>;
};

type TableWithAnInt32ArrayWithPadding = table {
    1: a array<int32, 3>;
};

type TableWithAnInt32ArrayNoPadding = table {
    1: a array<int32, 4>;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto an_array = test_library.LookupStruct("AnArray");
  ASSERT_NE(an_array, nullptr);
  EXPECT_TYPE_SHAPE(an_array, (Expected{
                                  .inline_size = 40,
                                  .alignment = 8,
                              }));

  auto table_with_an_array = test_library.LookupTable("TableWithAnArray");
  ASSERT_NE(table_with_an_array, nullptr);
  EXPECT_TYPE_SHAPE(table_with_an_array, (Expected{
                                             .inline_size = 16,
                                             .alignment = 8,
                                             .max_out_of_line = 48,
                                             .depth = 2,
                                             .has_padding = false,
                                             .has_flexible_envelope = true,
                                         }));

  auto table_with_an_int32_array_with_padding =
      test_library.LookupTable("TableWithAnInt32ArrayWithPadding");
  ASSERT_NE(table_with_an_int32_array_with_padding, nullptr);
  EXPECT_TYPE_SHAPE(table_with_an_int32_array_with_padding, (Expected{
                                                                .inline_size = 16,
                                                                .alignment = 8,
                                                                .max_out_of_line = 24,
                                                                .depth = 2,
                                                                .has_padding = true,
                                                                .has_flexible_envelope = true,
                                                            }));

  auto table_with_an_int32_array_no_padding =
      test_library.LookupTable("TableWithAnInt32ArrayNoPadding");
  ASSERT_NE(table_with_an_int32_array_no_padding, nullptr);
  EXPECT_TYPE_SHAPE(table_with_an_int32_array_no_padding, (Expected{
                                                              .inline_size = 16,
                                                              .alignment = 8,
                                                              .max_out_of_line = 24,
                                                              .depth = 2,
                                                              .has_padding = false,
                                                              .has_flexible_envelope = true,
                                                          }));
}

TEST(TypeshapeTests, GoodArraysWithHandles) {
  TestLibrary test_library(R"FIDL(
library example;
using zx;

type HandleArray = resource struct {
  h1 array<zx.Handle, 8>;
};

type TableWithHandleArray = resource table {
  1: ha array<zx.Handle, 8>;
};

type NullableHandleArray = resource struct {
  ha array<zx.Handle:optional, 8>;
};

type TableWithNullableHandleArray = resource table {
  1: ha array<zx.Handle:optional, 8>;
};

)FIDL");
  test_library.UseLibraryZx();
  ASSERT_COMPILED(test_library);

  auto handle_array = test_library.LookupStruct("HandleArray");
  ASSERT_NE(handle_array, nullptr);
  EXPECT_TYPE_SHAPE(handle_array, (Expected{
                                      .inline_size = 32,
                                      .alignment = 4,
                                      .max_handles = 8,
                                  }));

  auto table_with_handle_array = test_library.LookupTable("TableWithHandleArray");
  ASSERT_NE(table_with_handle_array, nullptr);
  EXPECT_TYPE_SHAPE(table_with_handle_array, (Expected{
                                                 .inline_size = 16,
                                                 .alignment = 8,
                                                 .max_out_of_line = 40,
                                                 .max_handles = 8,
                                                 .depth = 2,
                                                 .has_padding = false,
                                                 .has_flexible_envelope = true,
                                             }));

  auto nullable_handle_array = test_library.LookupStruct("NullableHandleArray");
  ASSERT_NE(nullable_handle_array, nullptr);
  EXPECT_TYPE_SHAPE(nullable_handle_array, (Expected{
                                               .inline_size = 32,
                                               .alignment = 4,
                                               .max_handles = 8,
                                           }));

  auto table_with_nullable_handle_array = test_library.LookupTable("TableWithNullableHandleArray");
  ASSERT_NE(table_with_nullable_handle_array, nullptr);
  EXPECT_TYPE_SHAPE(table_with_nullable_handle_array, (Expected{
                                                          .inline_size = 16,
                                                          .alignment = 8,
                                                          .max_out_of_line = 40,
                                                          .max_handles = 8,
                                                          .depth = 2,
                                                          .has_padding = false,
                                                          .has_flexible_envelope = true,
                                                      }));
}

// TODO(fxbug.dev/118282): write a "unions_with_handles" test case.

TEST(TypeshapeTests, GoodFlexibleUnions) {
  TestLibrary test_library(R"FIDL(library example;

type UnionWithOneBool = flexible union {
    1: b bool;
};

type StructWithOptionalUnionWithOneBool = struct {
    opt_union_with_bool UnionWithOneBool:optional;
};

type UnionWithBoundedOutOfLineObject = flexible union {
    // smaller than |v| below, so will not be selected for max-out-of-line
    // calculation.
    1: b bool;

    // 1. vector<int32>:5 = 8 bytes for vector element count
    //                    + 8 bytes for data pointer
    //                    + 24 bytes out-of-line (20 bytes contents +
    //                                            4 bytes for 8-byte alignment)
    //                    = 40 bytes total
    // 1. vector<vector<int32>:5>:6 = vector of up to six of vector<int32>:5
    //                              = 8 bytes for vector element count
    //                              + 8 bytes for data pointer
    //                              + 240 bytes out-of-line (40 bytes contents * 6)
    //                              = 256 bytes total
    2: v vector<vector<int32>:5>:6;
};

type UnionWithUnboundedOutOfLineObject = flexible union {
    1: s string;
};

type UnionWithoutPayloadPadding = flexible union {
    1: a array<uint64, 7>;
};

type PaddingCheck = flexible union {
    1: three array<uint8, 3>;
    2: five array<uint8, 5>;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto one_bool = test_library.LookupUnion("UnionWithOneBool");
  ASSERT_NE(one_bool, nullptr);
  EXPECT_TYPE_SHAPE(one_bool, (Expected{
                                  .inline_size = 16,
                                  .alignment = 8,
                                  .max_out_of_line = 0,
                                  .depth = 1,
                                  .has_padding = true,
                                  .has_flexible_envelope = true,
                              }));
  ASSERT_EQ(one_bool->members.size(), 1u);
  ASSERT_NE(one_bool->members[0].maybe_used, nullptr);
  EXPECT_FIELD_SHAPE(*one_bool->members[0].maybe_used, (ExpectedField{.padding = 7}));

  auto opt_one_bool = test_library.LookupStruct("StructWithOptionalUnionWithOneBool");
  ASSERT_NE(opt_one_bool, nullptr);
  EXPECT_TYPE_SHAPE(opt_one_bool, (Expected{
                                      .inline_size = 16,
                                      .alignment = 8,
                                      .max_out_of_line = 0,
                                      .depth = 1,
                                      .has_padding = true,
                                      .has_flexible_envelope = true,
                                  }));

  auto xu = test_library.LookupUnion("UnionWithBoundedOutOfLineObject");
  ASSERT_NE(xu, nullptr);
  EXPECT_TYPE_SHAPE(xu, (Expected{
                            .inline_size = 16,
                            .alignment = 8,
                            .max_out_of_line = 256,
                            .depth = 3,
                            .has_padding = true,
                            .has_flexible_envelope = true,
                        }));

  auto unbounded = test_library.LookupUnion("UnionWithUnboundedOutOfLineObject");
  ASSERT_NE(unbounded, nullptr);
  EXPECT_TYPE_SHAPE(unbounded, (Expected{
                                   .inline_size = 16,
                                   .alignment = 8,
                                   .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                                   .depth = 2,
                                   .has_padding = true,
                                   .has_flexible_envelope = true,
                               }));

  auto xu_no_payload_padding = test_library.LookupUnion("UnionWithoutPayloadPadding");
  ASSERT_NE(xu_no_payload_padding, nullptr);
  EXPECT_TYPE_SHAPE(xu_no_payload_padding,
                    (Expected{
                        .inline_size = 16,
                        .alignment = 8,
                        .max_out_of_line = 56,
                        .depth = 1,
                        // TODO(fxbug.dev/36332): Unions currently return true for
                        // has_padding in all cases, which should be fixed.
                        .has_padding = true,
                        .has_flexible_envelope = true,
                    }));

  auto padding_check = test_library.LookupUnion("PaddingCheck");
  ASSERT_NE(padding_check, nullptr);
  EXPECT_TYPE_SHAPE(padding_check, (Expected{
                                       .inline_size = 16,
                                       .alignment = 8,
                                       .max_out_of_line = 8,
                                       .depth = 1,
                                       .has_padding = true,
                                       .has_flexible_envelope = true,
                                   }));
  ASSERT_EQ(padding_check->members.size(), 2u);
  ASSERT_NE(padding_check->members[0].maybe_used, nullptr);
  EXPECT_FIELD_SHAPE(*padding_check->members[0].maybe_used, (ExpectedField{.padding = 5}));
  EXPECT_FIELD_SHAPE(*padding_check->members[1].maybe_used, (ExpectedField{.padding = 3}));
}

TEST(TypeshapeTests, GoodEnvelopeStrictness) {
  TestLibrary test_library(R"FIDL(library example;

type StrictLeafUnion = strict union {
    1: a int64;
};

type FlexibleLeafUnion = flexible union {
    1: a int64;
};

type FlexibleUnionOfStrictUnion = flexible union {
    1: xu StrictLeafUnion;
};

type FlexibleUnionOfFlexibleUnion = flexible union {
    1: xu FlexibleLeafUnion;
};

type StrictUnionOfStrictUnion = strict union {
    1: xu StrictLeafUnion;
};

type StrictUnionOfFlexibleUnion = strict union {
    1: xu FlexibleLeafUnion;
};

type FlexibleLeafTable = table {};

type StrictUnionOfFlexibleTable = strict union {
    1: ft FlexibleLeafTable;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto strict_union = test_library.LookupUnion("StrictLeafUnion");
  ASSERT_NE(strict_union, nullptr);
  EXPECT_TYPE_SHAPE(strict_union, (Expected{
                                      .inline_size = 16,
                                      .alignment = 8,
                                      .max_out_of_line = 8,
                                      .depth = 1,
                                      .has_padding = true,
                                  }));

  auto flexible_union = test_library.LookupUnion("FlexibleLeafUnion");
  ASSERT_NE(flexible_union, nullptr);
  EXPECT_TYPE_SHAPE(flexible_union, (Expected{
                                        .inline_size = 16,
                                        .alignment = 8,
                                        .max_out_of_line = 8,
                                        .depth = 1,
                                        .has_padding = true,
                                        .has_flexible_envelope = true,
                                    }));

  auto flexible_of_strict = test_library.LookupUnion("FlexibleUnionOfStrictUnion");
  ASSERT_NE(flexible_of_strict, nullptr);
  EXPECT_TYPE_SHAPE(flexible_of_strict, (Expected{
                                            .inline_size = 16,
                                            .alignment = 8,
                                            .max_out_of_line = 24,
                                            .depth = 2,
                                            .has_padding = true,
                                            .has_flexible_envelope = true,
                                        }));

  auto flexible_of_flexible = test_library.LookupUnion("FlexibleUnionOfFlexibleUnion");
  ASSERT_NE(flexible_of_flexible, nullptr);
  EXPECT_TYPE_SHAPE(flexible_of_flexible, (Expected{
                                              .inline_size = 16,
                                              .alignment = 8,
                                              .max_out_of_line = 24,
                                              .depth = 2,
                                              .has_padding = true,
                                              .has_flexible_envelope = true,
                                          }));

  auto strict_of_strict = test_library.LookupUnion("StrictUnionOfStrictUnion");
  ASSERT_NE(strict_of_strict, nullptr);
  EXPECT_TYPE_SHAPE(strict_of_strict, (Expected{
                                          .inline_size = 16,
                                          .alignment = 8,
                                          .max_out_of_line = 24,
                                          .depth = 2,
                                          .has_padding = true,
                                      }));

  auto strict_of_flexible = test_library.LookupUnion("StrictUnionOfFlexibleUnion");
  ASSERT_NE(strict_of_flexible, nullptr);
  EXPECT_TYPE_SHAPE(strict_of_flexible, (Expected{
                                            .inline_size = 16,
                                            .alignment = 8,
                                            .max_out_of_line = 24,
                                            .depth = 2,
                                            .has_padding = true,
                                            .has_flexible_envelope = true,
                                        }));

  auto flexible_table = test_library.LookupTable("FlexibleLeafTable");
  ASSERT_NE(flexible_table, nullptr);
  EXPECT_TYPE_SHAPE(flexible_table, (Expected{
                                        .inline_size = 16,
                                        .alignment = 8,
                                        .max_out_of_line = 0,
                                        .depth = 1,
                                        .has_padding = false,
                                        .has_flexible_envelope = true,
                                    }));

  auto strict_union_of_flexible_table = test_library.LookupUnion("StrictUnionOfFlexibleTable");
  ASSERT_NE(strict_union_of_flexible_table, nullptr);
  EXPECT_TYPE_SHAPE(strict_union_of_flexible_table, (Expected{
                                                        .inline_size = 16,
                                                        .alignment = 8,
                                                        .max_out_of_line = 16,
                                                        .depth = 2,
                                                        .has_padding = true,
                                                        .has_flexible_envelope = true,
                                                    }));
}

TEST(TypeshapeTests, GoodProtocolsAndRequestOfProtocols) {
  TestLibrary test_library(R"FIDL(library example;

protocol SomeProtocol {};

type UsingSomeProtocol = resource struct {
    value client_end:SomeProtocol;
};

type UsingOptSomeProtocol = resource struct {
    value client_end:<SomeProtocol, optional>;
};

type UsingRequestSomeProtocol = resource struct {
    value server_end:SomeProtocol;
};

type UsingOptRequestSomeProtocol = resource struct {
    value server_end:<SomeProtocol, optional>;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto using_some_protocol = test_library.LookupStruct("UsingSomeProtocol");
  ASSERT_NE(using_some_protocol, nullptr);
  EXPECT_TYPE_SHAPE(using_some_protocol, (Expected{
                                             .inline_size = 4,
                                             .alignment = 4,
                                             .max_handles = 1,
                                         }));

  auto using_opt_some_protocol = test_library.LookupStruct("UsingOptSomeProtocol");
  ASSERT_NE(using_opt_some_protocol, nullptr);
  EXPECT_TYPE_SHAPE(using_opt_some_protocol, (Expected{
                                                 .inline_size = 4,
                                                 .alignment = 4,
                                                 .max_handles = 1,
                                             }));

  auto using_request_some_protocol = test_library.LookupStruct("UsingRequestSomeProtocol");
  ASSERT_NE(using_request_some_protocol, nullptr);
  EXPECT_TYPE_SHAPE(using_request_some_protocol, (Expected{
                                                     .inline_size = 4,
                                                     .alignment = 4,
                                                     .max_handles = 1,
                                                 }));

  auto using_opt_request_some_protocol = test_library.LookupStruct("UsingOptRequestSomeProtocol");
  ASSERT_NE(using_opt_request_some_protocol, nullptr);
  EXPECT_TYPE_SHAPE(using_opt_request_some_protocol, (Expected{
                                                         .inline_size = 4,
                                                         .alignment = 4,
                                                         .max_handles = 1,
                                                     }));
}

TEST(TypeshapeTests, GoodExternalDefinitions) {
  TestLibrary test_library;
  test_library.UseLibraryZx();
  test_library.AddSource("example.fidl", R"FIDL(
library example;

using zx;

type ExternalArrayStruct = struct {
    a array<ExternalSimpleStruct, EXTERNAL_SIZE_DEF>;
};

type ExternalStringSizeStruct = struct {
    a string:EXTERNAL_SIZE_DEF;
};

type ExternalVectorSizeStruct = resource struct {
    a vector<zx.Handle>:EXTERNAL_SIZE_DEF;
};

)FIDL");
  test_library.AddSource("extern_defs.fidl", R"FIDL(
library example;

const EXTERNAL_SIZE_DEF uint32 = ANOTHER_INDIRECTION;
const ANOTHER_INDIRECTION uint32 = 32;

type ExternalSimpleStruct = struct {
    a uint32;
};
)FIDL");
  ASSERT_COMPILED(test_library);

  auto ext_struct = test_library.LookupStruct("ExternalSimpleStruct");
  ASSERT_NE(ext_struct, nullptr);
  EXPECT_TYPE_SHAPE(ext_struct, (Expected{
                                    .inline_size = 4,
                                    .alignment = 4,
                                }));

  auto ext_arr_struct = test_library.LookupStruct("ExternalArrayStruct");
  ASSERT_NE(ext_arr_struct, nullptr);
  EXPECT_TYPE_SHAPE(ext_arr_struct, (Expected{
                                        .inline_size = 4 * 32,
                                        .alignment = 4,
                                    }));

  auto ext_str_struct = test_library.LookupStruct("ExternalStringSizeStruct");
  ASSERT_NE(ext_str_struct, nullptr);
  EXPECT_TYPE_SHAPE(ext_str_struct, (Expected{
                                        .inline_size = 16,
                                        .alignment = 8,
                                        .max_out_of_line = 32,
                                        .depth = 1,
                                        .has_padding = true,
                                    }));

  auto ext_vec_struct = test_library.LookupStruct("ExternalVectorSizeStruct");
  ASSERT_NE(ext_vec_struct, nullptr);
  EXPECT_TYPE_SHAPE(ext_vec_struct, (Expected{
                                        .inline_size = 16,
                                        .alignment = 8,
                                        .max_out_of_line = 32 * 4,
                                        .max_handles = 32,
                                        .depth = 1,
                                        .has_padding = true,
                                    }));
}

TEST(TypeshapeTests, GoodSimpleRequest) {
  TestLibrary library(R"FIDL(library example;

protocol Test {
    Method(struct { a int16; b int16; });
};
)FIDL");
  ASSERT_COMPILED(library);

  auto protocol = library.LookupProtocol("Test");
  ASSERT_NE(protocol, nullptr);
  ASSERT_EQ(protocol->methods.size(), 1u);
  auto& method = protocol->methods[0];
  auto method_request = method.maybe_request.get();
  EXPECT_EQ(method.has_request, true);
  ASSERT_NE(method_request, nullptr);

  auto id = static_cast<const fidl::flat::IdentifierType*>(method_request->type);
  auto as_struct = static_cast<const fidl::flat::Struct*>(id->type_decl);
  ASSERT_NE(as_struct, nullptr);

  EXPECT_TYPE_SHAPE(as_struct, (Expected{
                                   .inline_size = 4,
                                   .alignment = 2,
                                   .max_handles = 0,
                                   .has_padding = false,
                               }));

  ASSERT_EQ(as_struct->members.size(), 2u);
  EXPECT_FIELD_SHAPE(as_struct->members[0], (ExpectedField{.offset = 0, .padding = 0}));
  EXPECT_FIELD_SHAPE(as_struct->members[1], (ExpectedField{.offset = 2, .padding = 0}));
}

TEST(TypeshapeTests, GoodSimpleResponse) {
  TestLibrary library(R"FIDL(library example;

protocol Test {
    strict Method() -> (struct { a int16; b int16; });
};
)FIDL");
  ASSERT_COMPILED(library);

  auto protocol = library.LookupProtocol("Test");
  ASSERT_NE(protocol, nullptr);
  ASSERT_EQ(protocol->methods.size(), 1u);
  auto& method = protocol->methods[0];
  auto method_response = method.maybe_response.get();
  EXPECT_EQ(method.has_response, true);
  ASSERT_NE(method_response, nullptr);

  auto id = static_cast<const fidl::flat::IdentifierType*>(method_response->type);
  auto as_struct = static_cast<const fidl::flat::Struct*>(id->type_decl);
  ASSERT_NE(as_struct, nullptr);

  EXPECT_TYPE_SHAPE(as_struct, (Expected{
                                   .inline_size = 4,
                                   .alignment = 2,
                                   .max_handles = 0,
                                   .has_padding = false,
                               }));

  ASSERT_EQ(as_struct->members.size(), 2u);
  EXPECT_FIELD_SHAPE(as_struct->members[0], (ExpectedField{.offset = 0, .padding = 0}));
  EXPECT_FIELD_SHAPE(as_struct->members[1], (ExpectedField{.offset = 2, .padding = 0}));
}

TEST(TypeshapeTests, GoodRecursiveRequest) {
  TestLibrary library(R"FIDL(library example;

type WebMessage = resource struct {
    message_port_req server_end:MessagePort;
};

protocol MessagePort {
    PostMessage(resource struct {
        message WebMessage;
    }) -> (struct {
        success bool;
    });
};
)FIDL");
  ASSERT_COMPILED(library);

  auto web_message = library.LookupStruct("WebMessage");
  ASSERT_NE(web_message, nullptr);
  EXPECT_TYPE_SHAPE(web_message, (Expected{
                                     .inline_size = 4,
                                     .alignment = 4,
                                     .max_handles = 1,
                                 }));
  ASSERT_EQ(web_message->members.size(), 1u);
  EXPECT_FIELD_SHAPE(web_message->members[0], (ExpectedField{}));

  auto message_port = library.LookupProtocol("MessagePort");
  ASSERT_NE(message_port, nullptr);
  ASSERT_EQ(message_port->methods.size(), 1u);
  auto& post_message = message_port->methods[0];
  auto post_message_request = post_message.maybe_request.get();
  EXPECT_EQ(post_message.has_request, true);
  ASSERT_NE(post_message_request, nullptr);

  auto id = static_cast<const fidl::flat::IdentifierType*>(post_message_request->type);
  auto as_struct = static_cast<const fidl::flat::Struct*>(id->type_decl);
  ASSERT_NE(as_struct, nullptr);

  EXPECT_TYPE_SHAPE(as_struct, (Expected{
                                   .inline_size = 4,
                                   .alignment = 4,
                                   .max_handles = 1,
                                   .has_padding = false,
                               }));
  ASSERT_EQ(as_struct->members.size(), 1u);
  EXPECT_FIELD_SHAPE(as_struct->members[0], (ExpectedField{.offset = 0, .padding = 0}));
}

TEST(TypeshapeTests, GoodRecursiveOptRequest) {
  TestLibrary library(R"FIDL(library example;

type WebMessage = resource struct {
    opt_message_port_req server_end:<MessagePort, optional>;
};

protocol MessagePort {
    PostMessage(resource struct {
        message WebMessage;
    }) -> (struct {
        success bool;
    });
};
)FIDL");
  ASSERT_COMPILED(library);

  auto web_message = library.LookupStruct("WebMessage");
  ASSERT_NE(web_message, nullptr);
  EXPECT_TYPE_SHAPE(web_message, (Expected{
                                     .inline_size = 4,
                                     .alignment = 4,
                                     .max_handles = 1,
                                 }));

  auto message_port = library.LookupProtocol("MessagePort");
  ASSERT_NE(message_port, nullptr);
  ASSERT_EQ(message_port->methods.size(), 1u);
  auto& post_message = message_port->methods[0];
  auto post_message_request = post_message.maybe_request.get();
  EXPECT_EQ(post_message.has_request, true);

  auto id = static_cast<const fidl::flat::IdentifierType*>(post_message_request->type);
  auto as_struct = static_cast<const fidl::flat::Struct*>(id->type_decl);
  ASSERT_NE(as_struct, nullptr);

  EXPECT_TYPE_SHAPE(as_struct, (Expected{
                                   .inline_size = 4,
                                   .alignment = 4,
                                   .max_handles = 1,
                                   .has_padding = false,
                               }));
  ASSERT_EQ(as_struct->members.size(), 1u);
  EXPECT_FIELD_SHAPE(as_struct->members[0], (ExpectedField{.offset = 0, .padding = 0}));
}

TEST(TypeshapeTests, GoodRecursiveProtocol) {
  TestLibrary library(R"FIDL(library example;

type WebMessage = resource struct {
    message_port client_end:MessagePort;
};

protocol MessagePort {
    PostMessage(resource struct {
        message WebMessage;
    }) -> (struct {
        success bool;
    });
};
)FIDL");
  ASSERT_COMPILED(library);

  auto web_message = library.LookupStruct("WebMessage");
  ASSERT_NE(web_message, nullptr);
  EXPECT_TYPE_SHAPE(web_message, (Expected{
                                     .inline_size = 4,
                                     .alignment = 4,
                                     .max_handles = 1,
                                 }));

  auto message_port = library.LookupProtocol("MessagePort");
  ASSERT_NE(message_port, nullptr);
  ASSERT_EQ(message_port->methods.size(), 1u);
  auto& post_message = message_port->methods[0];
  auto post_message_request = post_message.maybe_request.get();
  EXPECT_EQ(post_message.has_request, true);

  auto id = static_cast<const fidl::flat::IdentifierType*>(post_message_request->type);
  auto as_struct = static_cast<const fidl::flat::Struct*>(id->type_decl);
  ASSERT_NE(as_struct, nullptr);

  EXPECT_TYPE_SHAPE(as_struct, (Expected{
                                   .inline_size = 4,
                                   .alignment = 4,
                                   .max_handles = 1,
                                   .has_padding = false,
                               }));
  ASSERT_EQ(as_struct->members.size(), 1u);
  EXPECT_FIELD_SHAPE(as_struct->members[0], (ExpectedField{.offset = 0, .padding = 0}));
}

TEST(TypeshapeTests, GoodRecursiveOptProtocol) {
  TestLibrary library(R"FIDL(library example;

type WebMessage = resource struct {
    opt_message_port client_end:<MessagePort, optional>;
};

protocol MessagePort {
    PostMessage(resource struct {
        message WebMessage;
    }) -> (struct {
        success bool;
    });
};
)FIDL");
  ASSERT_COMPILED(library);

  auto web_message = library.LookupStruct("WebMessage");
  ASSERT_NE(web_message, nullptr);
  EXPECT_TYPE_SHAPE(web_message, (Expected{
                                     .inline_size = 4,
                                     .alignment = 4,
                                     .max_handles = 1,
                                 }));

  auto message_port = library.LookupProtocol("MessagePort");
  ASSERT_NE(message_port, nullptr);
  ASSERT_EQ(message_port->methods.size(), 1u);
  auto& post_message = message_port->methods[0];
  auto post_message_request = post_message.maybe_request.get();
  EXPECT_EQ(post_message.has_request, true);

  auto id = static_cast<const fidl::flat::IdentifierType*>(post_message_request->type);
  auto as_struct = static_cast<const fidl::flat::Struct*>(id->type_decl);
  ASSERT_NE(as_struct, nullptr);

  EXPECT_TYPE_SHAPE(as_struct, (Expected{
                                   .inline_size = 4,
                                   .alignment = 4,
                                   .max_handles = 1,
                                   .has_padding = false,
                               }));
  ASSERT_EQ(as_struct->members.size(), 1u);
  EXPECT_FIELD_SHAPE(as_struct->members[0], (ExpectedField{.offset = 0, .padding = 0}));
}

TEST(TypeshapeTests, GoodRecursiveStruct) {
  TestLibrary library(R"FIDL(library example;

type TheStruct = struct {
    opt_one_more box<TheStruct>;
};
)FIDL");
  ASSERT_COMPILED(library);

  auto the_struct = library.LookupStruct("TheStruct");
  ASSERT_NE(the_struct, nullptr);
  EXPECT_TYPE_SHAPE(the_struct, (Expected{
                                    .inline_size = 8,
                                    .alignment = 8,
                                    .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                                    .max_handles = 0,
                                    .depth = std::numeric_limits<uint32_t>::max(),
                                }));
  ASSERT_EQ(the_struct->members.size(), 1u);
  EXPECT_FIELD_SHAPE(the_struct->members[0], (ExpectedField{}));
}

TEST(TypeshapeTests, GoodRecursiveStructWithHandles) {
  TestLibrary library(kPrologWithHandleDefinition + R"FIDL(
type TheStruct = resource struct {
  some_handle handle:VMO;
  opt_one_more box<TheStruct>;
};
)FIDL");
  ASSERT_COMPILED(library);

  auto the_struct = library.LookupStruct("TheStruct");
  ASSERT_NE(the_struct, nullptr);
  EXPECT_TYPE_SHAPE(the_struct, (Expected{
                                    .inline_size = 16,
                                    .alignment = 8,
                                    .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                                    .max_handles = std::numeric_limits<uint32_t>::max(),
                                    .depth = std::numeric_limits<uint32_t>::max(),
                                    .has_padding = true,
                                }));
  ASSERT_EQ(the_struct->members.size(), 2u);
  EXPECT_FIELD_SHAPE(the_struct->members[0], (ExpectedField{
                                                 .padding = 4,
                                             }));
  EXPECT_FIELD_SHAPE(the_struct->members[1], (ExpectedField{
                                                 .offset = 8,
                                             }));
}

TEST(TypeshapeTests, GoodCoRecursiveStruct) {
  TestLibrary library(R"FIDL(library example;

type A = struct {
    foo box<B>;
};

type B = struct {
    bar box<A>;
};
)FIDL");
  ASSERT_COMPILED(library);

  auto struct_a = library.LookupStruct("A");
  ASSERT_NE(struct_a, nullptr);
  EXPECT_TYPE_SHAPE(struct_a, (Expected{
                                  .inline_size = 8,
                                  .alignment = 8,
                                  .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                                  .max_handles = 0,
                                  .depth = std::numeric_limits<uint32_t>::max(),
                              }));

  auto struct_b = library.LookupStruct("B");
  ASSERT_NE(struct_b, nullptr);
  EXPECT_TYPE_SHAPE(struct_b, (Expected{
                                  .inline_size = 8,
                                  .alignment = 8,
                                  .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                                  .max_handles = 0,
                                  .depth = std::numeric_limits<uint32_t>::max(),
                              }));
}

TEST(TypeshapeTests, GoodCoRecursiveStructWithHandles) {
  TestLibrary library(R"FIDL(
library example;
using zx;

type A = resource struct {
    a zx.Handle;
    foo box<B>;
};

type B = resource struct {
    b zx.Handle;
    bar box<A>;
};
)FIDL");
  library.UseLibraryZx();
  ASSERT_COMPILED(library);

  auto struct_a = library.LookupStruct("A");
  ASSERT_NE(struct_a, nullptr);
  EXPECT_TYPE_SHAPE(struct_a, (Expected{
                                  .inline_size = 16,
                                  .alignment = 8,
                                  .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                                  .max_handles = std::numeric_limits<uint32_t>::max(),
                                  .depth = std::numeric_limits<uint32_t>::max(),
                                  .has_padding = true,
                              }));

  auto struct_b = library.LookupStruct("B");
  ASSERT_NE(struct_b, nullptr);
  EXPECT_TYPE_SHAPE(struct_b, (Expected{
                                  .inline_size = 16,
                                  .alignment = 8,
                                  .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                                  .max_handles = std::numeric_limits<uint32_t>::max(),
                                  .depth = std::numeric_limits<uint32_t>::max(),
                                  .has_padding = true,
                              }));
}

TEST(TypeshapeTests, GoodCoRecursiveStruct2) {
  TestLibrary library(R"FIDL(library example;

type Foo = struct {
    b Bar;
};

type Bar = struct {
    f box<Foo>;
};
)FIDL");
  ASSERT_COMPILED(library);

  auto struct_foo = library.LookupStruct("Foo");
  ASSERT_NE(struct_foo, nullptr);
  EXPECT_TYPE_SHAPE(struct_foo, (Expected{
                                    .inline_size = 8,
                                    .alignment = 8,
                                    .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                                    .max_handles = 0,
                                    .depth = std::numeric_limits<uint32_t>::max(),
                                }));

  auto struct_bar = library.LookupStruct("Bar");
  ASSERT_NE(struct_bar, nullptr);
  EXPECT_TYPE_SHAPE(struct_bar, (Expected{
                                    .inline_size = 8,
                                    .alignment = 8,
                                    .max_out_of_line = std::numeric_limits<uint32_t>::max(),
                                    .max_handles = 0,
                                    .depth = std::numeric_limits<uint32_t>::max(),
                                }));
}

TEST(TypeshapeTests, GoodStructTwoDeep) {
  TestLibrary library(kPrologWithHandleDefinition + R"FIDL(
type DiffEntry = resource struct {
    key vector<uint8>:256;

    base box<Value>;
    left box<Value>;
    right box<Value>;
};

type Value = resource struct {
    value box<Buffer>;
    priority Priority;
};

type Buffer = resource struct {
    vmo handle:VMO;
    size uint64;
};

type Priority = enum {
    EAGER = 0;
    LAZY = 1;
};
)FIDL");
  ASSERT_COMPILED(library);

  auto buffer = library.LookupStruct("Buffer");
  ASSERT_NE(buffer, nullptr);
  EXPECT_TYPE_SHAPE(buffer, (Expected{
                                .inline_size = 16,
                                .alignment = 8,
                                .max_handles = 1,
                                .has_padding = true,
                            }));

  auto value = library.LookupStruct("Value");
  ASSERT_NE(value, nullptr);
  EXPECT_TYPE_SHAPE(value,
                    (Expected{
                        .inline_size = 16,
                        .alignment = 8,
                        .max_out_of_line = 16,
                        .max_handles = 1,
                        .depth = 1,
                        .has_padding = true,  // because the size of |Priority| defaults to uint32
                    }));

  auto diff_entry = library.LookupStruct("DiffEntry");
  ASSERT_NE(diff_entry, nullptr);
  EXPECT_TYPE_SHAPE(diff_entry, (Expected{
                                    .inline_size = 40,
                                    .alignment = 8,
                                    .max_out_of_line = 352,
                                    .max_handles = 3,
                                    .depth = 2,
                                    .has_padding = true,  // because |Value| has padding
                                }));
}

TEST(TypeshapeTests, GoodProtocolChildAndParent) {
  SharedAmongstLibraries shared;
  TestLibrary parent_library(&shared, "parent.fidl", R"FIDL(library parent;

protocol Parent {
    Sync() -> ();
};
)FIDL");
  ASSERT_COMPILED(parent_library);

  TestLibrary child_library(&shared, "child.fidl", R"FIDL(
library child;

using parent;

protocol Child {
  compose parent.Parent;
};
)FIDL");
  ASSERT_COMPILED(child_library);

  auto child = child_library.LookupProtocol("Child");
  ASSERT_NE(child, nullptr);
  ASSERT_EQ(child->all_methods.size(), 1u);
  auto& sync_with_info = child->all_methods[0];
  auto sync_request = sync_with_info.method->maybe_request.get();
  EXPECT_EQ(sync_with_info.method->has_request, true);
  ASSERT_EQ(sync_request, nullptr);
}

TEST(TypeshapeTests, GoodUnionSize8Alignment4Sandwich) {
  TestLibrary library(R"FIDL(library example;

type UnionSize8Alignment4 = strict union {
    1: variant uint32;
};

type Sandwich = struct {
    before uint32;
    union UnionSize8Alignment4;
    after uint32;
};
)FIDL");
  ASSERT_COMPILED(library);

  auto sandwich = library.LookupStruct("Sandwich");
  ASSERT_NE(sandwich, nullptr);
  EXPECT_TYPE_SHAPE(sandwich, (Expected{
                                  .inline_size = 32,
                                  .alignment = 8,
                                  .max_out_of_line = 0,
                                  .max_handles = 0,
                                  .depth = 1,
                                  .has_padding = true,
                              }));
  ASSERT_EQ(sandwich->members.size(), 3u);
  EXPECT_FIELD_SHAPE(sandwich->members[0],  // before
                     (ExpectedField{
                         .offset = 0,
                         .padding = 4,
                     }));
  EXPECT_FIELD_SHAPE(sandwich->members[1],  // union
                     (ExpectedField{
                         .offset = 8,
                         .padding = 0,
                     }));
  EXPECT_FIELD_SHAPE(sandwich->members[2],  // after
                     (ExpectedField{
                         .offset = 24,
                         .padding = 4,
                     }));
}

TEST(TypeshapeTests, GoodUnionSize12Alignment4Sandwich) {
  TestLibrary library(R"FIDL(library example;

type UnionSize12Alignment4 = strict union {
    1: variant array<uint8, 6>;
};

type Sandwich = struct {
    before uint32;
    union UnionSize12Alignment4;
    after int32;
};
)FIDL");
  ASSERT_COMPILED(library);

  auto sandwich = library.LookupStruct("Sandwich");
  ASSERT_NE(sandwich, nullptr);
  EXPECT_TYPE_SHAPE(sandwich, (Expected{
                                  .inline_size = 32,
                                  .alignment = 8,
                                  .max_out_of_line = 8,
                                  .max_handles = 0,
                                  .depth = 1,
                                  .has_padding = true,
                              }));
  ASSERT_EQ(sandwich->members.size(), 3u);
  EXPECT_FIELD_SHAPE(sandwich->members[0],  // before
                     (ExpectedField{
                         .offset = 0,
                         .padding = 4,
                     }));
  EXPECT_FIELD_SHAPE(sandwich->members[1],  // union
                     (ExpectedField{
                         .offset = 8,
                         .padding = 0,
                     }));
  EXPECT_FIELD_SHAPE(sandwich->members[2],  // after
                     (ExpectedField{
                         .offset = 24,
                         .padding = 4,
                     }));
}

TEST(TypeshapeTests, GoodUnionSize24Alignment8Sandwich) {
  TestLibrary library(R"FIDL(library example;

type StructSize16Alignment8 = struct {
    f1 uint64;
    f2 uint64;
};

type UnionSize24Alignment8 = strict union {
    1: variant StructSize16Alignment8;
};

type Sandwich = struct {
    before uint32;
    union UnionSize24Alignment8;
    after uint32;
};
)FIDL");
  ASSERT_COMPILED(library);

  auto sandwich = library.LookupStruct("Sandwich");
  ASSERT_NE(sandwich, nullptr);
  EXPECT_TYPE_SHAPE(sandwich, (Expected{
                                  .inline_size = 32,
                                  .alignment = 8,
                                  .max_out_of_line = 16,
                                  .max_handles = 0,
                                  .depth = 1,
                                  .has_padding = true,
                              }));
  ASSERT_EQ(sandwich->members.size(), 3u);
  EXPECT_FIELD_SHAPE(sandwich->members[0],  // before
                     (ExpectedField{
                         .offset = 0,
                         .padding = 4,
                     }));
  EXPECT_FIELD_SHAPE(sandwich->members[1],  // union
                     (ExpectedField{
                         .offset = 8,
                         .padding = 0,
                     }));
  EXPECT_FIELD_SHAPE(sandwich->members[2],  // after
                     (ExpectedField{
                         .offset = 24,
                         .padding = 4,
                     }));
}

TEST(TypeshapeTests, GoodUnionSize36Alignment4Sandwich) {
  TestLibrary library(R"FIDL(library example;

type UnionSize36Alignment4 = strict union {
    1: variant array<uint8, 32>;
};

type Sandwich = struct {
    before uint32;
    union UnionSize36Alignment4;
    after uint32;
};
)FIDL");
  ASSERT_COMPILED(library);

  auto sandwich = library.LookupStruct("Sandwich");
  ASSERT_NE(sandwich, nullptr);
  EXPECT_TYPE_SHAPE(sandwich, (Expected{
                                  .inline_size = 32,
                                  .alignment = 8,
                                  .max_out_of_line = 32,
                                  .max_handles = 0,
                                  .depth = 1,
                                  .has_padding = true,
                              }));
  ASSERT_EQ(sandwich->members.size(), 3u);
  EXPECT_FIELD_SHAPE(sandwich->members[0],  // before
                     (ExpectedField{
                         .offset = 0,
                         .padding = 4,
                     }));
  EXPECT_FIELD_SHAPE(sandwich->members[1],  // union
                     (ExpectedField{
                         .offset = 8,
                         .padding = 0,
                     }));
  EXPECT_FIELD_SHAPE(sandwich->members[2],  // after
                     (ExpectedField{
                         .offset = 24,
                         .padding = 4,
                     }));
}

TEST(TypeshapeTests, GoodZeroSizeVector) {
  TestLibrary library(R"FIDL(
library example;
using zx;

type A = resource struct {
    zero_size vector<zx.Handle>:0;
};

)FIDL");
  library.UseLibraryZx();
  ASSERT_COMPILED(library);

  auto struct_a = library.LookupStruct("A");
  ASSERT_NE(struct_a, nullptr);
  EXPECT_TYPE_SHAPE(struct_a, (Expected{
                                  .inline_size = 16,
                                  .alignment = 8,
                                  .max_out_of_line = 0,
                                  .max_handles = 0,
                                  .depth = 1,
                                  .has_padding = true,
                              }));
}

}  // namespace
