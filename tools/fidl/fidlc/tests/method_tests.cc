// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zxtest/zxtest.h>

#include "tools/fidl/fidlc/include/fidl/diagnostics.h"
#include "tools/fidl/fidlc/include/fidl/flat/types.h"
#include "tools/fidl/fidlc/include/fidl/flat_ast.h"
#include "tools/fidl/fidlc/tests/error_test.h"
#include "tools/fidl/fidlc/tests/test_library.h"

namespace {

// TODO(fxbug.dev/88366): remove once fully migrated.
TEST(MethodTests, GoodValidMethodsAndEventsMigrationMode) {
  TestLibrary library(R"FIDL(library example;

open protocol OpenExample1 {
    -> OnMyEvent();
    MyOneWayMethod();
    MyTwoWayMethod() -> ();
};

open protocol OpenExample2 {
    strict -> OnMyEvent();
    strict MyOneWayMethod();
    strict MyTwoWayMethod() -> ();
};

open protocol OpenExample3 {
    flexible -> OnMyEvent();
    flexible MyOneWayMethod();
    flexible MyTwoWayMethod() -> ();
};

ajar protocol AjarExample1 {
    -> OnMyEvent();
    MyOneWayMethod();
    MyTwoWayMethod() -> ();
};

ajar protocol AjarExample2 {
    strict -> OnMyEvent();
    strict MyOneWayMethod();
    strict MyTwoWayMethod() -> ();
};

ajar protocol AjarExample3 {
    flexible -> OnMyEvent();
    flexible MyOneWayMethod();
};

closed protocol ClosedExample1 {
    -> OnMyEvent();
    MyOneWayMethod();
    MyTwoWayMethod() -> ();
};

closed protocol ClosedExample2 {
    strict -> OnMyEvent();
    strict MyOneWayMethod();
    strict MyTwoWayMethod() -> ();
};

protocol ImplicitClosedExample1 {
    -> OnMyEvent();
    MyOneWayMethod();
    MyTwoWayMethod() -> ();
};

protocol ImplicitClosedExample2 {
    strict -> OnMyEvent();
    strict MyOneWayMethod();
    strict MyTwoWayMethod() -> ();
};

)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  ASSERT_COMPILED(library);

  auto open_protocol1 = library.LookupProtocol("OpenExample1");
  ASSERT_NOT_NULL(open_protocol1);
  ASSERT_EQ(open_protocol1->methods.size(), 3);
  EXPECT_EQ(open_protocol1->all_methods.size(), 3);
  for (auto& method : open_protocol1->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kStrict);
  }

  auto open_protocol2 = library.LookupProtocol("OpenExample2");
  ASSERT_NOT_NULL(open_protocol2);
  ASSERT_EQ(open_protocol2->methods.size(), 3);
  EXPECT_EQ(open_protocol2->all_methods.size(), 3);
  for (auto& method : open_protocol2->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kStrict);
  }

  auto open_protocol3 = library.LookupProtocol("OpenExample3");
  ASSERT_NOT_NULL(open_protocol3);
  ASSERT_EQ(open_protocol3->methods.size(), 3);
  EXPECT_EQ(open_protocol3->all_methods.size(), 3);
  for (auto& method : open_protocol3->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kFlexible);
  }

  auto ajar_protocol1 = library.LookupProtocol("AjarExample1");
  ASSERT_NOT_NULL(ajar_protocol1);
  ASSERT_EQ(ajar_protocol1->methods.size(), 3);
  EXPECT_EQ(ajar_protocol1->all_methods.size(), 3);
  for (auto& method : ajar_protocol1->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kStrict);
  }

  auto ajar_protocol2 = library.LookupProtocol("AjarExample2");
  ASSERT_NOT_NULL(ajar_protocol2);
  ASSERT_EQ(ajar_protocol2->methods.size(), 3);
  EXPECT_EQ(ajar_protocol2->all_methods.size(), 3);
  for (auto& method : ajar_protocol2->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kStrict);
  }

  auto ajar_protocol3 = library.LookupProtocol("AjarExample3");
  ASSERT_NOT_NULL(ajar_protocol3);
  ASSERT_EQ(ajar_protocol3->methods.size(), 2);
  EXPECT_EQ(ajar_protocol3->all_methods.size(), 2);
  for (auto& method : ajar_protocol3->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kFlexible);
  }

  auto closed_protocol1 = library.LookupProtocol("ClosedExample1");
  ASSERT_NOT_NULL(closed_protocol1);
  ASSERT_EQ(closed_protocol1->methods.size(), 3);
  EXPECT_EQ(closed_protocol1->all_methods.size(), 3);
  for (auto& method : closed_protocol1->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kStrict);
  }

  auto closed_protocol2 = library.LookupProtocol("ClosedExample2");
  ASSERT_NOT_NULL(closed_protocol2);
  ASSERT_EQ(closed_protocol2->methods.size(), 3);
  EXPECT_EQ(closed_protocol2->all_methods.size(), 3);
  for (auto& method : closed_protocol2->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kStrict);
  }

  auto implicit_closed_protocol1 = library.LookupProtocol("ImplicitClosedExample1");
  ASSERT_NOT_NULL(implicit_closed_protocol1);
  ASSERT_EQ(implicit_closed_protocol1->methods.size(), 3);
  EXPECT_EQ(implicit_closed_protocol1->all_methods.size(), 3);
  for (auto& method : implicit_closed_protocol1->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kStrict);
  }

  auto implicit_closed_protocol2 = library.LookupProtocol("ImplicitClosedExample2");
  ASSERT_NOT_NULL(implicit_closed_protocol2);
  ASSERT_EQ(implicit_closed_protocol2->methods.size(), 3);
  EXPECT_EQ(implicit_closed_protocol2->all_methods.size(), 3);
  for (auto& method : implicit_closed_protocol2->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kStrict);
  }
}

// TODO(fxbug.dev/88366): remove once fully migrated.
TEST(MethodTests, BadInvalidMethodsAndEventsMigrationMode) {
  TestLibrary library1(R"FIDL(library example;
ajar protocol Example {
    flexible TwoWay() -> ();
};
)FIDL");
  library1.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  ASSERT_ERRORED_DURING_COMPILE(library1, fidl::ErrFlexibleTwoWayMethodRequiresOpenProtocol)

  TestLibrary library2(R"FIDL(library example;
closed protocol Example {
    flexible TwoWay() -> ();
};
)FIDL");
  library2.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  ASSERT_ERRORED_DURING_COMPILE(library2, fidl::ErrFlexibleTwoWayMethodRequiresOpenProtocol)

  TestLibrary library3(R"FIDL(library example;
protocol Example {
    flexible TwoWay() -> ();
};
)FIDL");
  library3.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  ASSERT_ERRORED_DURING_COMPILE(library3, fidl::ErrFlexibleTwoWayMethodRequiresOpenProtocol)

  TestLibrary library4(R"FIDL(library example;
closed protocol Example {
    flexible OneWay();
};
)FIDL");
  library4.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  ASSERT_ERRORED_DURING_COMPILE(library4, fidl::ErrFlexibleOneWayMethodInClosedProtocol)

  TestLibrary library5(R"FIDL(library example;
closed protocol Example {
    flexible -> OnEvent();
};
)FIDL");
  library5.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  ASSERT_ERRORED_DURING_COMPILE(library5, fidl::ErrFlexibleOneWayMethodInClosedProtocol)

  TestLibrary library6(R"FIDL(library example;
protocol Example {
    flexible OneWay();
};
)FIDL");
  library6.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  ASSERT_ERRORED_DURING_COMPILE(library6, fidl::ErrFlexibleOneWayMethodInClosedProtocol)

  TestLibrary library7(R"FIDL(library example;
protocol Example {
    flexible -> OnEvent();
};
)FIDL");
  library7.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  ASSERT_ERRORED_DURING_COMPILE(library7, fidl::ErrFlexibleOneWayMethodInClosedProtocol)
}

// TODO(fxbug.dev/88366): remove once fully migrated.
TEST(MethodTests, GoodValidMethodsAndEventsMandateMode) {
  TestLibrary library(R"FIDL(library example;

open protocol OpenExample1 {
    strict -> OnMyEvent();
    strict MyOneWayMethod();
    strict MyTwoWayMethod() -> ();
};

open protocol OpenExample2 {
    flexible -> OnMyEvent();
    flexible MyOneWayMethod();
    flexible MyTwoWayMethod() -> ();
};

ajar protocol AjarExample1 {
    strict -> OnMyEvent();
    strict MyOneWayMethod();
    strict MyTwoWayMethod() -> ();
};

ajar protocol AjarExample2 {
    flexible -> OnMyEvent();
    flexible MyOneWayMethod();
};

closed protocol ClosedExample1 {
    strict -> OnMyEvent();
    strict MyOneWayMethod();
    strict MyTwoWayMethod() -> ();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsMandate);
  ASSERT_COMPILED(library);

  auto open_protocol1 = library.LookupProtocol("OpenExample1");
  ASSERT_NOT_NULL(open_protocol1);
  ASSERT_EQ(open_protocol1->methods.size(), 3);
  EXPECT_EQ(open_protocol1->all_methods.size(), 3);
  for (auto& method : open_protocol1->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kStrict);
  }

  auto open_protocol2 = library.LookupProtocol("OpenExample2");
  ASSERT_NOT_NULL(open_protocol2);
  ASSERT_EQ(open_protocol2->methods.size(), 3);
  EXPECT_EQ(open_protocol2->all_methods.size(), 3);
  for (auto& method : open_protocol2->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kFlexible);
  }

  auto ajar_protocol1 = library.LookupProtocol("AjarExample1");
  ASSERT_NOT_NULL(ajar_protocol1);
  ASSERT_EQ(ajar_protocol1->methods.size(), 3);
  EXPECT_EQ(ajar_protocol1->all_methods.size(), 3);
  for (auto& method : ajar_protocol1->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kStrict);
  }

  auto ajar_protocol2 = library.LookupProtocol("AjarExample2");
  ASSERT_NOT_NULL(ajar_protocol2);
  ASSERT_EQ(ajar_protocol2->methods.size(), 2);
  EXPECT_EQ(ajar_protocol2->all_methods.size(), 2);
  for (auto& method : ajar_protocol2->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kFlexible);
  }

  auto closed_protocol1 = library.LookupProtocol("ClosedExample1");
  ASSERT_NOT_NULL(closed_protocol1);
  ASSERT_EQ(closed_protocol1->methods.size(), 3);
  EXPECT_EQ(closed_protocol1->all_methods.size(), 3);
  for (auto& method : closed_protocol1->methods) {
    EXPECT_EQ(method.strictness, fidl::types::Strictness::kStrict);
  }
}

// TODO(fxbug.dev/88366): remove once fully migrated.
TEST(MethodTests, BadInvalidMethodsAndEventsMandateMode) {
  TestLibrary library1;
  library1.AddFile("bad/fi-0191.test.fidl");
  library1.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library1.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsMandate);
  ASSERT_ERRORED_DURING_COMPILE(library1, fidl::ErrMethodMustDefineStrictness)

  TestLibrary library2(R"FIDL(library example;
open protocol Example {
    TwoWay() -> ();
};
)FIDL");
  library2.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library2.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsMandate);
  ASSERT_ERRORED_DURING_COMPILE(library2, fidl::ErrMethodMustDefineStrictness)

  TestLibrary library3(R"FIDL(library example;
open protocol Example {
    -> Event();
};
)FIDL");
  library3.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library3.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsMandate);
  ASSERT_ERRORED_DURING_COMPILE(library3, fidl::ErrMethodMustDefineStrictness)

  TestLibrary library4(R"FIDL(library example;
ajar protocol Example {
    OneWay();
};
)FIDL");
  library4.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library4.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsMandate);
  ASSERT_ERRORED_DURING_COMPILE(library4, fidl::ErrMethodMustDefineStrictness)

  TestLibrary library5(R"FIDL(library example;
ajar protocol Example {
    TwoWay() -> ();
};
)FIDL");
  library5.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library5.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsMandate);
  ASSERT_ERRORED_DURING_COMPILE(library5, fidl::ErrMethodMustDefineStrictness)

  TestLibrary library6(R"FIDL(library example;
ajar protocol Example {
    -> Event();
};
)FIDL");
  library6.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library6.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsMandate);
  ASSERT_ERRORED_DURING_COMPILE(library6, fidl::ErrMethodMustDefineStrictness)

  TestLibrary library7(R"FIDL(library example;
closed protocol Example {
    OneWay();
};
)FIDL");
  library7.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library7.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsMandate);
  ASSERT_ERRORED_DURING_COMPILE(library7, fidl::ErrMethodMustDefineStrictness)

  TestLibrary library8(R"FIDL(library example;
closed protocol Example {
    TwoWay() -> ();
};
)FIDL");
  library8.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library8.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsMandate);
  ASSERT_ERRORED_DURING_COMPILE(library8, fidl::ErrMethodMustDefineStrictness)

  TestLibrary library9(R"FIDL(library example;
closed protocol Example {
    -> Event();
};
)FIDL");
  library9.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library9.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsMandate);
  ASSERT_ERRORED_DURING_COMPILE(library9, fidl::ErrMethodMustDefineStrictness)
}

TEST(MethodTests, GoodValidComposeMethod) {
  TestLibrary library(R"FIDL(library example;

open protocol HasComposeMethod1 {
    compose();
};

open protocol HasComposeMethod2 {
    compose() -> ();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto protocol1 = library.LookupProtocol("HasComposeMethod1");
  ASSERT_NOT_NULL(protocol1);
  ASSERT_EQ(protocol1->methods.size(), 1);
  EXPECT_EQ(protocol1->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol1->all_methods.size(), 1);

  auto protocol2 = library.LookupProtocol("HasComposeMethod2");
  ASSERT_NOT_NULL(protocol2);
  ASSERT_EQ(protocol2->methods.size(), 1);
  EXPECT_EQ(protocol2->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol2->all_methods.size(), 1);
}

TEST(MethodTests, GoodValidStrictComposeMethod) {
  TestLibrary library(R"FIDL(library example;

open protocol HasComposeMethod1 {
    strict compose();
};

open protocol HasComposeMethod2 {
    strict compose() -> ();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto protocol1 = library.LookupProtocol("HasComposeMethod1");
  ASSERT_NOT_NULL(protocol1);
  ASSERT_EQ(protocol1->methods.size(), 1);
  EXPECT_EQ(protocol1->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol1->all_methods.size(), 1);

  auto protocol2 = library.LookupProtocol("HasComposeMethod2");
  ASSERT_NOT_NULL(protocol2);
  ASSERT_EQ(protocol2->methods.size(), 1);
  EXPECT_EQ(protocol2->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol2->all_methods.size(), 1);
}

TEST(MethodTests, GoodValidFlexibleComposeMethod) {
  TestLibrary library(R"FIDL(library example;

open protocol HasComposeMethod1 {
    flexible compose();
};

open protocol HasComposeMethod2 {
    flexible compose() -> ();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto protocol1 = library.LookupProtocol("HasComposeMethod1");
  ASSERT_NOT_NULL(protocol1);
  ASSERT_EQ(protocol1->methods.size(), 1);
  EXPECT_EQ(protocol1->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol1->all_methods.size(), 1);

  auto protocol2 = library.LookupProtocol("HasComposeMethod2");
  ASSERT_NOT_NULL(protocol2);
  ASSERT_EQ(protocol2->methods.size(), 1);
  EXPECT_EQ(protocol2->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol2->all_methods.size(), 1);
}

TEST(MethodTests, GoodValidStrictMethod) {
  TestLibrary library(R"FIDL(library example;

open protocol HasStrictMethod1 {
    strict();
};

open protocol HasStrictMethod2 {
    strict() -> ();
};

open protocol HasStrictMethod3 {
    strict strict();
};

open protocol HasStrictMethod4 {
    strict strict() -> ();
};

open protocol HasStrictMethod5 {
    flexible strict();
};

open protocol HasStrictMethod6 {
    flexible strict() -> ();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto protocol1 = library.LookupProtocol("HasStrictMethod1");
  ASSERT_NOT_NULL(protocol1);
  ASSERT_EQ(protocol1->methods.size(), 1);
  EXPECT_EQ(protocol1->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol1->all_methods.size(), 1);

  auto protocol2 = library.LookupProtocol("HasStrictMethod2");
  ASSERT_NOT_NULL(protocol2);
  ASSERT_EQ(protocol2->methods.size(), 1);
  EXPECT_EQ(protocol2->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol2->all_methods.size(), 1);

  auto protocol3 = library.LookupProtocol("HasStrictMethod3");
  ASSERT_NOT_NULL(protocol3);
  ASSERT_EQ(protocol3->methods.size(), 1);
  EXPECT_EQ(protocol3->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol3->all_methods.size(), 1);

  auto protocol4 = library.LookupProtocol("HasStrictMethod4");
  ASSERT_NOT_NULL(protocol4);
  ASSERT_EQ(protocol4->methods.size(), 1);
  EXPECT_EQ(protocol4->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol4->all_methods.size(), 1);

  auto protocol5 = library.LookupProtocol("HasStrictMethod5");
  ASSERT_NOT_NULL(protocol5);
  ASSERT_EQ(protocol5->methods.size(), 1);
  EXPECT_EQ(protocol5->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol5->all_methods.size(), 1);

  auto protocol6 = library.LookupProtocol("HasStrictMethod6");
  ASSERT_NOT_NULL(protocol6);
  ASSERT_EQ(protocol6->methods.size(), 1);
  EXPECT_EQ(protocol6->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol6->all_methods.size(), 1);
}

TEST(MethodTests, GoodValidFlexibleTwoWayMethod) {
  TestLibrary library(R"FIDL(library example;

open protocol HasFlexibleTwoWayMethod1 {
    flexible();
};

open protocol HasFlexibleTwoWayMethod2 {
    flexible() -> ();
};

open protocol HasFlexibleTwoWayMethod3 {
    strict flexible();
};

open protocol HasFlexibleTwoWayMethod4 {
    strict flexible() -> ();
};

open protocol HasFlexibleTwoWayMethod5 {
    flexible flexible();
};

open protocol HasFlexibleTwoWayMethod6 {
    flexible flexible() -> ();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto protocol1 = library.LookupProtocol("HasFlexibleTwoWayMethod1");
  ASSERT_NOT_NULL(protocol1);
  ASSERT_EQ(protocol1->methods.size(), 1);
  EXPECT_EQ(protocol1->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol1->all_methods.size(), 1);

  auto protocol2 = library.LookupProtocol("HasFlexibleTwoWayMethod2");
  ASSERT_NOT_NULL(protocol2);
  ASSERT_EQ(protocol2->methods.size(), 1);
  EXPECT_EQ(protocol2->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol2->all_methods.size(), 1);

  auto protocol3 = library.LookupProtocol("HasFlexibleTwoWayMethod3");
  ASSERT_NOT_NULL(protocol3);
  ASSERT_EQ(protocol3->methods.size(), 1);
  EXPECT_EQ(protocol3->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol3->all_methods.size(), 1);

  auto protocol4 = library.LookupProtocol("HasFlexibleTwoWayMethod4");
  ASSERT_NOT_NULL(protocol4);
  ASSERT_EQ(protocol4->methods.size(), 1);
  EXPECT_EQ(protocol4->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol4->all_methods.size(), 1);

  auto protocol5 = library.LookupProtocol("HasFlexibleTwoWayMethod5");
  ASSERT_NOT_NULL(protocol5);
  ASSERT_EQ(protocol5->methods.size(), 1);
  EXPECT_EQ(protocol5->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol5->all_methods.size(), 1);

  auto protocol6 = library.LookupProtocol("HasFlexibleTwoWayMethod6");
  ASSERT_NOT_NULL(protocol6);
  ASSERT_EQ(protocol6->methods.size(), 1);
  EXPECT_EQ(protocol6->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol6->all_methods.size(), 1);
}

TEST(MethodTests, GoodValidNormalMethod) {
  TestLibrary library(R"FIDL(library example;

open protocol HasNormalMethod1 {
    MyMethod();
};

open protocol HasNormalMethod2 {
    MyMethod() -> ();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto protocol1 = library.LookupProtocol("HasNormalMethod1");
  ASSERT_NOT_NULL(protocol1);
  ASSERT_EQ(protocol1->methods.size(), 1);
  EXPECT_EQ(protocol1->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol1->all_methods.size(), 1);

  auto protocol2 = library.LookupProtocol("HasNormalMethod2");
  ASSERT_NOT_NULL(protocol2);
  ASSERT_EQ(protocol2->methods.size(), 1);
  EXPECT_EQ(protocol2->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol2->all_methods.size(), 1);
}

TEST(MethodTests, GoodValidStrictNormalMethod) {
  TestLibrary library(R"FIDL(library example;

open protocol HasNormalMethod1 {
    strict MyMethod();
};

open protocol HasNormalMethod2 {
    strict MyMethod() -> ();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto protocol1 = library.LookupProtocol("HasNormalMethod1");
  ASSERT_NOT_NULL(protocol1);
  ASSERT_EQ(protocol1->methods.size(), 1);
  EXPECT_EQ(protocol1->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol1->all_methods.size(), 1);

  auto protocol2 = library.LookupProtocol("HasNormalMethod2");
  ASSERT_NOT_NULL(protocol2);
  ASSERT_EQ(protocol2->methods.size(), 1);
  EXPECT_EQ(protocol2->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol2->all_methods.size(), 1);
}

TEST(MethodTests, GoodValidFlexibleNormalMethod) {
  TestLibrary library(R"FIDL(library example;

open protocol HasNormalMethod1 {
    flexible MyMethod();
};

open protocol HasNormalMethod2 {
    flexible MyMethod() -> ();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto protocol1 = library.LookupProtocol("HasNormalMethod1");
  ASSERT_NOT_NULL(protocol1);
  ASSERT_EQ(protocol1->methods.size(), 1);
  EXPECT_EQ(protocol1->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol1->all_methods.size(), 1);

  auto protocol2 = library.LookupProtocol("HasNormalMethod2");
  ASSERT_NOT_NULL(protocol2);
  ASSERT_EQ(protocol2->methods.size(), 1);
  EXPECT_EQ(protocol2->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol2->all_methods.size(), 1);
}

TEST(MethodTests, GoodValidEvent) {
  TestLibrary library(R"FIDL(library example;

protocol HasEvent {
    -> MyEvent();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto protocol = library.LookupProtocol("HasEvent");
  ASSERT_NOT_NULL(protocol);
  ASSERT_EQ(protocol->methods.size(), 1);
  EXPECT_EQ(protocol->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol->all_methods.size(), 1);
}

TEST(MethodTests, GoodValidStrictEvent) {
  TestLibrary library(R"FIDL(library example;

protocol HasEvent {
    strict -> MyMethod();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto protocol = library.LookupProtocol("HasEvent");
  ASSERT_NOT_NULL(protocol);
  ASSERT_EQ(protocol->methods.size(), 1);
  EXPECT_EQ(protocol->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol->all_methods.size(), 1);
}

TEST(MethodTests, GoodValidFlexibleEvent) {
  TestLibrary library(R"FIDL(library example;

protocol HasEvent {
    flexible -> MyMethod();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);

  ASSERT_COMPILED(library);

  auto protocol = library.LookupProtocol("HasEvent");
  ASSERT_NOT_NULL(protocol);
  ASSERT_EQ(protocol->methods.size(), 1);
  EXPECT_EQ(protocol->methods[0].strictness, fidl::types::Strictness::kFlexible);
  EXPECT_EQ(protocol->all_methods.size(), 1);
}

TEST(MethodTests, GoodValidStrictnessModifiers) {
  TestLibrary library(R"FIDL(library example;

closed protocol Closed {
  strict StrictOneWay();
  strict StrictTwoWay() -> ();
  strict -> StrictEvent();
};

ajar protocol Ajar {
  strict StrictOneWay();
  flexible FlexibleOneWay();

  strict StrictTwoWay() -> ();

  strict -> StrictEvent();
  flexible -> FlexibleEvent();
};

open protocol Open {
  strict StrictOneWay();
  flexible FlexibleOneWay();

  strict StrictTwoWay() -> ();
  flexible FlexibleTwoWay() -> ();

  strict -> StrictEvent();
  flexible -> FlexibleEvent();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto closed = library.LookupProtocol("Closed");
  ASSERT_NOT_NULL(closed);
  ASSERT_EQ(closed->methods.size(), 3);

  auto ajar = library.LookupProtocol("Ajar");
  ASSERT_NOT_NULL(ajar);
  ASSERT_EQ(ajar->methods.size(), 5);

  auto open = library.LookupProtocol("Open");
  ASSERT_NOT_NULL(open);
  ASSERT_EQ(open->methods.size(), 6);
}

TEST(MethodTests, BadInvalidStrictnessFlexibleEventInClosed) {
  TestLibrary library(R"FIDL(library example;

closed protocol Closed {
  flexible -> Event();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrFlexibleOneWayMethodInClosedProtocol);
}

TEST(MethodTests, BadInvalidStrictnessFlexibleOneWayMethodInClosed) {
  TestLibrary library;
  library.AddFile("bad/fi-0116.test.fidl");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrFlexibleOneWayMethodInClosedProtocol);
}

TEST(MethodTests, BadInvalidStrictnessFlexibleTwoWayMethodInClosed) {
  TestLibrary library(R"FIDL(library example;

closed protocol Closed {
  flexible Method() -> ();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrFlexibleTwoWayMethodRequiresOpenProtocol);
}

TEST(MethodTests, BadInvalidStrictnessFlexibleTwoWayMethodInAjar) {
  TestLibrary library;
  library.AddFile("bad/fi-0115.test.fidl");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrFlexibleTwoWayMethodRequiresOpenProtocol);
}

TEST(MethodTests, BadInvalidOpennessModifierOnMethod) {
  TestLibrary library(R"FIDL(
library example;

protocol BadMethod {
    open Method();
};

)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidProtocolMember);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, GoodValidComposeMethodWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasMethod {
    compose();
};
)FIDL");
  ASSERT_COMPILED(library);

  auto protocol = library.LookupProtocol("HasMethod");
  ASSERT_NOT_NULL(protocol);
  ASSERT_EQ(protocol->methods.size(), 1);
  EXPECT_EQ(protocol->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol->all_methods.size(), 1);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, BadStrictComposeMethodWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasMethod {
    strict compose();
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidProtocolMember);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, BadFlexibleComposeMethodWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasMethod {
    flexible compose();
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidProtocolMember);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, GoodValidStrictMethodWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasMethod {
    strict();
};
)FIDL");
  ASSERT_COMPILED(library);

  auto protocol = library.LookupProtocol("HasMethod");
  ASSERT_NOT_NULL(protocol);
  ASSERT_EQ(protocol->methods.size(), 1);
  EXPECT_EQ(protocol->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol->all_methods.size(), 1);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, BadStrictStrictMethodWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasMethod {
    strict strict();
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidProtocolMember);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, BadFlexibleStrictMethodWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasMethod {
    flexible strict();
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidProtocolMember);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, GoodValidFlexibleTwoWayMethodWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasMethod {
    flexible();
};
)FIDL");
  ASSERT_COMPILED(library);

  auto protocol = library.LookupProtocol("HasMethod");
  ASSERT_NOT_NULL(protocol);
  ASSERT_EQ(protocol->methods.size(), 1);
  EXPECT_EQ(protocol->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol->all_methods.size(), 1);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, BadStrictFlexibleTwoWayMethodWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasMethod {
    strict flexible();
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidProtocolMember);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, BadFlexibleFlexibleTwoWayMethodWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasMethod {
    flexible flexible();
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidProtocolMember);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, GoodValidNormalMethodWithoutUnknownInteractions) {
  TestLibrary library;
  library.AddFile("good/fi-0024.test.fidl");
  ASSERT_COMPILED(library);

  auto protocol = library.LookupProtocol("Example");
  ASSERT_NOT_NULL(protocol);
  ASSERT_EQ(protocol->methods.size(), 1);
  EXPECT_EQ(protocol->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol->all_methods.size(), 1);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, BadStrictNormalMethodWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasMethod {
    strict MyMethod();
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidProtocolMember);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, BadFlexibleNormalMethodWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasMethod {
    flexible MyMethod();
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidProtocolMember);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, GoodValidEventWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasEvent {
    -> OnSomething();
};
)FIDL");
  ASSERT_COMPILED(library);

  auto protocol = library.LookupProtocol("HasEvent");
  ASSERT_NOT_NULL(protocol);
  ASSERT_EQ(protocol->methods.size(), 1);
  EXPECT_EQ(protocol->methods[0].strictness, fidl::types::Strictness::kStrict);
  EXPECT_EQ(protocol->all_methods.size(), 1);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, BadStrictEventWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasEvent {
    strict -> OnSomething();
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidProtocolMember);
}

// TODO(fxb/88366): remove checks for behavior with unknown interactions turned
// off when unknown interactions are always-on.
TEST(MethodTests, BadFlexibleEventWithoutUnknownInteractions) {
  TestLibrary library(R"FIDL(library example;
protocol HasEvent {
    flexible -> OnSomething();
};
)FIDL");
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrInvalidProtocolMember);
}

TEST(MethodTests, GoodValidEmptyPayloads) {
  TestLibrary library(R"FIDL(library example;

open protocol Test {
  strict MethodA() -> ();
  flexible MethodB() -> ();
  strict MethodC() -> () error int32;
  flexible MethodD() -> () error int32;
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto closed = library.LookupProtocol("Test");
  ASSERT_NOT_NULL(closed);
  ASSERT_EQ(closed->methods.size(), 4);
}

TEST(MethodTests, BadInvalidEmptyStructPayloadStrictNoError) {
  TestLibrary library(R"FIDL(library example;

open protocol Test {
  strict Method() -> (struct {});
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrEmptyPayloadStructs);
}

// TODO(fxbug.dev/112767): This is temporarily still allowed. Remove once the
// soft transition of `--experimental simple_empty_response_syntax` is done.
TEST(MethodTests, GoodEmptyStructPayloadFlexibleNoError) {
  TestLibrary library(R"FIDL(library example;

open protocol Test {
  flexible Method() -> (struct {});
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

// TODO(fxbug.dev/112767): This is temporarily still allowed. Remove once the
// soft transition of `--experimental simple_empty_response_syntax` is done.
TEST(MethodTests, GoodEmptyStructPayloadStrictError) {
  TestLibrary library(R"FIDL(library example;

open protocol Test {
  strict Method() -> (struct {}) error int32;
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

// TODO(fxbug.dev/112767): This is temporarily still allowed. Remove once the
// soft transition of `--experimental simple_empty_response_syntax` is done.
TEST(MethodTests, GoodEmptyStructPayloadFlexibleError) {
  TestLibrary library(R"FIDL(library example;

open protocol Test {
  flexible Method() -> (struct {}) error int32;
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

TEST(MethodTests, GoodAbsentPayloadFlexibleNoError) {
  TestLibrary library(R"FIDL(library example;

open protocol Test {
  flexible Method() -> ();
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

TEST(MethodTests, GoodAbsentPayloadStrictError) {
  TestLibrary library(R"FIDL(library example;

open protocol Test {
  strict Method() -> () error int32;
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

TEST(MethodTests, GoodAbsentPayloadFlexibleError) {
  TestLibrary library(R"FIDL(library example;

open protocol Test {
  flexible Method() -> () error int32;
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);
}

TEST(MethodTests, BadEmptyStructPayloadFlexibleNoError) {
  TestLibrary library(R"FIDL(library example;

open protocol Test {
  flexible Method() -> (struct {});
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kSimpleEmptyResponseSyntax);
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrEmptyPayloadStructs);
}

TEST(MethodTests, BadEmptyStructPayloadStrictError) {
  TestLibrary library(R"FIDL(library example;

open protocol Test {
  strict Method() -> (struct {}) error int32;
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kSimpleEmptyResponseSyntax);
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrEmptyPayloadStructs);
}

TEST(MethodTests, BadEmptyStructPayloadFlexibleError) {
  TestLibrary library(R"FIDL(library example;

open protocol Test {
  flexible Method() -> (struct {}) error int32;
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kSimpleEmptyResponseSyntax);
  ASSERT_ERRORED_DURING_COMPILE(library, fidl::ErrEmptyPayloadStructs);
}

TEST(MethodTests, GoodFlexibleNoErrorResponseUnion) {
  TestLibrary library(R"FIDL(library example;

open protocol Example {
    flexible Method() -> (struct {
        foo string;
    });
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto methods = &library.LookupProtocol("Example")->methods;
  ASSERT_EQ(methods->size(), 1);
  auto method = &methods->at(0);
  auto response = method->maybe_response.get();
  ASSERT_NOT_NULL(response);

  ASSERT_EQ(response->type->kind, fidl::flat::Type::Kind::kIdentifier);
  auto id = static_cast<const fidl::flat::IdentifierType*>(response->type);
  ASSERT_EQ(id->type_decl->kind, fidl::flat::Decl::Kind::kStruct);
  auto as_struct = static_cast<const fidl::flat::Struct*>(id->type_decl);
  ASSERT_EQ(as_struct->members.size(), 1);

  auto response_member = &as_struct->members.at(0);
  ASSERT_EQ(response_member->type_ctor->type->kind, fidl::flat::Type::Kind::kIdentifier);
  auto result_identifier =
      static_cast<const fidl::flat::IdentifierType*>(response_member->type_ctor->type);
  const fidl::flat::Union* result_union =
      library.LookupUnion(std::string(result_identifier->name.decl_name()));
  ASSERT_NOT_NULL(result_union);
  ASSERT_NOT_NULL(result_union->attributes);
  ASSERT_NOT_NULL(result_union->attributes->Get("result"));
  ASSERT_EQ(result_union->members.size(), 3);

  const auto& success = result_union->members.at(0);
  ASSERT_NOT_NULL(success.maybe_used);
  ASSERT_STREQ("response", std::string(success.maybe_used->name.data()).c_str());

  const fidl::flat::Union::Member& error = result_union->members.at(1);
  ASSERT_NULL(error.maybe_used);
  ASSERT_STREQ("err", std::string(error.span->data()).c_str());

  const fidl::flat::Union::Member& transport_error = result_union->members.at(2);
  ASSERT_NOT_NULL(transport_error.maybe_used);
  ASSERT_STREQ("transport_err", std::string(transport_error.maybe_used->name.data()).c_str());

  ASSERT_NOT_NULL(transport_error.maybe_used->type_ctor->type);
  ASSERT_EQ(transport_error.maybe_used->type_ctor->type->kind, fidl::flat::Type::Kind::kInternal);
  auto transport_err_internal_type =
      static_cast<const fidl::flat::InternalType*>(transport_error.maybe_used->type_ctor->type);
  ASSERT_EQ(transport_err_internal_type->subtype, fidl::types::InternalSubtype::kTransportErr);
}

TEST(MethodTests, GoodFlexibleErrorResponseUnion) {
  TestLibrary library(R"FIDL(library example;

open protocol Example {
    flexible Method() -> (struct {
        foo string;
    }) error uint32;
};
)FIDL");
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractions);
  library.EnableFlag(fidl::ExperimentalFlags::Flag::kUnknownInteractionsNewDefaults);
  ASSERT_COMPILED(library);

  auto methods = &library.LookupProtocol("Example")->methods;
  ASSERT_EQ(methods->size(), 1);
  auto method = &methods->at(0);
  auto response = method->maybe_response.get();
  ASSERT_NOT_NULL(response);

  ASSERT_EQ(response->type->kind, fidl::flat::Type::Kind::kIdentifier);
  auto id = static_cast<const fidl::flat::IdentifierType*>(response->type);
  ASSERT_EQ(id->type_decl->kind, fidl::flat::Decl::Kind::kStruct);
  auto as_struct = static_cast<const fidl::flat::Struct*>(id->type_decl);
  ASSERT_EQ(as_struct->members.size(), 1);

  auto response_member = &as_struct->members.at(0);
  ASSERT_EQ(response_member->type_ctor->type->kind, fidl::flat::Type::Kind::kIdentifier);
  auto result_identifier =
      static_cast<const fidl::flat::IdentifierType*>(response_member->type_ctor->type);
  const fidl::flat::Union* result_union =
      library.LookupUnion(std::string(result_identifier->name.decl_name()));
  ASSERT_NOT_NULL(result_union);
  ASSERT_NOT_NULL(result_union->attributes);
  ASSERT_NOT_NULL(result_union->attributes->Get("result"));
  ASSERT_EQ(result_union->members.size(), 3);

  const auto& success = result_union->members.at(0);
  ASSERT_NOT_NULL(success.maybe_used);
  ASSERT_STREQ("response", std::string(success.maybe_used->name.data()).c_str());

  const fidl::flat::Union::Member& error = result_union->members.at(1);
  ASSERT_NOT_NULL(error.maybe_used);
  ASSERT_STREQ("err", std::string(error.maybe_used->name.data()).c_str());

  ASSERT_NOT_NULL(error.maybe_used->type_ctor->type);
  ASSERT_EQ(error.maybe_used->type_ctor->type->kind, fidl::flat::Type::Kind::kPrimitive);
  auto err_primitive_type =
      static_cast<const fidl::flat::PrimitiveType*>(error.maybe_used->type_ctor->type);
  ASSERT_EQ(err_primitive_type->subtype, fidl::types::PrimitiveSubtype::kUint32);

  const fidl::flat::Union::Member& transport_error = result_union->members.at(2);
  ASSERT_NOT_NULL(transport_error.maybe_used);
  ASSERT_STREQ("transport_err", std::string(transport_error.maybe_used->name.data()).c_str());

  ASSERT_NOT_NULL(transport_error.maybe_used->type_ctor->type);
  ASSERT_EQ(transport_error.maybe_used->type_ctor->type->kind, fidl::flat::Type::Kind::kInternal);
  auto transport_err_internal_type =
      static_cast<const fidl::flat::InternalType*>(transport_error.maybe_used->type_ctor->type);
  ASSERT_EQ(transport_err_internal_type->subtype, fidl::types::InternalSubtype::kTransportErr);
}
}  // namespace
