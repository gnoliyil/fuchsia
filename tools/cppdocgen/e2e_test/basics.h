// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_CPPDOCGEN_E2E_TEST_BASICS_H_
#define TOOLS_CPPDOCGEN_E2E_TEST_BASICS_H_

/// # Custom file header
///
/// This is the docstring for this file. It should appear at the top of the generated documentation
///
/// ## Just the basics
///
/// This test file contains the basics of the document generator.

// Documentation for the API flag.
#define API_FLAG_1 1
#define API_FLAG_2 2

// Undocumented macro $nodoc
#define UNDOCUMENTED_MACRO 123

// Macro with no declaration emitted $nodecl
#define DOCUMENTED_BUT_NO_DECL 789

// This is a structure that defines some values. The values appear inside the structure.
//
// The first value has no docstring, the second one does.
struct SimpleTestStructure {
  int a;

  char b;  // Some end-of-line documentation.

  // Some documentation for the `b` member of the `SimpleTestStructure`.
  double c;
};

union StandaloneUnion {
  int i;
  double d;
};

// Here is a regular enum with everything implicit.
enum MySimpleEnum {
  kValue1,
  kValue2,
};

// # A very complex enum.
//
// This is a C++ enum class with an explicit type and explicit values. It also has an explicit
// title for the docstring.
enum class MyFancyEnum : char {
  kValue1 = 1,
  kValue2 = 1 + 1,
};

// This enum should be undocumented because of the $nodoc annotation.
enum UndocumentedEnum { kSomeValue = 34324 };

// This enum should have the declaration omitted because of the $nodecl annotation.
enum NoDeclEnum { kSomeOtherValue = 34324 };

// This is an extern global value.
extern int kGlobalValue;

typedef SimpleTestStructure SimpleTestStructureTypedef;
using SimpleTestStructureUsing = SimpleTestStructure;

// This tests the C-style thing of defining an unnamed struct and a typedef for it at the same time.
typedef struct {
  int a;
} UnnamedStructTypedef;

// This one has a name for the struct that's separate from the typedef, yet still defined in the
// same declaration.
//
// TODO https://bugs.fuchsia.dev/p/fuchsia/issues/detail?id=119281 the struct definition and the
// typedef should be grouped togeher.
typedef struct tagged_struct {
  int a;
} tagged_struct_t;

// Here the C non-typedef'ed struct and the typedefed version are separate declarations.
struct tagged_struct_separate {
  int a;
};
typedef struct tagged_struct_separate tagged_struct_separate_t;

#endif  // TOOLS_CPPDOCGEN_E2E_TEST_BASICS_H_
