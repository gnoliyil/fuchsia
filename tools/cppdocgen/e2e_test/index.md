# cppdocgen e2e test

This directory contains headers that are run through the document generator, and
golden versions of the generated documentation.

## How this file affects the output

The contents of this README file will comprise the top of the generated index.md.

## Header files

  - [e2e_test/basics.h](basics.h.md)
  - [e2e_test/classes.h](classes.h.md)
  - [e2e_test/functions.h](functions.h.md)
  - [e2e_test/grouping.h](grouping.h.md)
  - [e2e_test/namespace.h](namespace.h.md)
  - [e2e_test/templates.h](templates.h.md)

## Classes and structures

  - [BaseClass1](classes.h.md#BaseClass1) class
  - [BaseClass2](classes.h.md#BaseClass2) class
  - [BaseTemplate](templates.h.md#BaseTemplate) class
  - [BaseTemplate](templates.h.md#BaseTemplate) class
  - [BaseTemplate](templates.h.md#BaseTemplate) class
  - [myns::ClassInsideNamespace](namespace.h.md#myns::ClassInsideNamespace) class
  - [DerivedClass](classes.h.md#DerivedClass) class
  - [DerivesFromTemplate](templates.h.md#DerivesFromTemplate) class
  - [MyClass](grouping.h.md#MyClass) class
  - [NoDeclarationClass](classes.h.md#NoDeclarationClass) class
  - [SimpleTestClass](classes.h.md#SimpleTestClass) class
  - [SimpleTestStructure](basics.h.md#SimpleTestStructure) struct
  - [StandaloneUnion](basics.h.md#StandaloneUnion) union
  - [myns::StructInsideNamespace](namespace.h.md#myns::StructInsideNamespace) struct
  - [UnnamedStructTypedef](basics.h.md#UnnamedStructTypedef) struct

## Functions

  - [CustomTitleFunction(…)](functions.h.md#CustomTitleFunction)
  - [GetStringFromVectors(…)](functions.h.md#GetStringFromVectors)
  - [GroupedExplicitlyOne(…)](grouping.h.md#GroupedExplicitlyOne)
  - [GroupedExplicitlyTwo(…)](grouping.h.md#GroupedExplicitlyOne)
  - [GroupedImplicitly(…)](grouping.h.md#GroupedImplicitly)
  - [myns::FunctionInsideNamespace()](namespace.h.md#myns::FunctionInsideNamespace)
  - [TemplateFunction(…)](templates.h.md#TemplateFunction)
  - [ThisShouldHaveNoDeclaration()](functions.h.md#ThisShouldHaveNoDeclaration)
  - [UndocumentedFunction()](functions.h.md#UndocumentedFunction)
  - [UngroupedOne()](grouping.h.md#UngroupedOne)
  - [UngroupedTwo()](grouping.h.md#UngroupedTwo)

## Enums

  - [MyFancyEnum](basics.h.md#MyFancyEnum)
  - [myns::EnumInsideNamespace](namespace.h.md#myns::EnumInsideNamespace)
  - [MySimpleEnum](basics.h.md#MySimpleEnum)
  - [NoDeclEnum](basics.h.md#NoDeclEnum)

## Macros

  - [API_FLAG_1](basics.h.md#API_FLAG_1)
  - [API_FLAG_2](basics.h.md#API_FLAG_2)
  - [DOCUMENTED_BUT_NO_DECL](basics.h.md#DOCUMENTED_BUT_NO_DECL)
  - [GROUPED_ONE](grouping.h.md#GROUPED_ONE)
  - [GROUPED_TWO](grouping.h.md#GROUPED_ONE)
  - [UNGROUPED_ONE](grouping.h.md#UNGROUPED_ONE)
  - [UNGROUPED_TWO](grouping.h.md#UNGROUPED_TWO)

