// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package c

import (
	"embed"
	"fmt"
	"path/filepath"
	"strconv"
	"strings"
	"text/template"

	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
	"go.fuchsia.dev/fuchsia/zircon/tools/zither"
)

//go:embed templates/*
var templates embed.FS

// Generator provides C data layout bindings.
type Generator struct {
	fidlgen.Generator
}

func NewGenerator(formatter fidlgen.Formatter) *Generator {
	gen := fidlgen.NewGenerator("CTemplates", templates, formatter, template.FuncMap{
		"Append":                   Append,
		"PrimitiveTypeName":        PrimitiveTypeName,
		"HeaderGuard":              HeaderGuard,
		"StandardIncludes":         StandardIncludes,
		"TypeName":                 TypeName,
		"ConstMemberName":          ConstMemberName,
		"LowerCaseWithUnderscores": zither.LowerCaseWithUnderscores,
		"UpperCaseWithUnderscores": zither.UpperCaseWithUnderscores,
		"ConstValue":               ConstValue,
		"EnumMemberValue":          EnumMemberValue,
		"BitsMemberValue":          BitsMemberValue,
		"StructMemberTypeInfo":     StructMemberTypeInfo,
	})
	return &Generator{*gen}
}

func (gen Generator) DeclOrder() zither.DeclOrder {
	return zither.DependencyDeclOrder
}

func (gen *Generator) Generate(summaries []zither.FileSummary, outputDir string) ([]string, error) {
	parts := summaries[0].Library.Parts()
	outputDir = filepath.Join(outputDir, filepath.Join(parts...))

	var outputs []string
	for _, summary := range summaries {
		output := filepath.Join(outputDir, summary.Name+".h")
		if err := gen.GenerateFile(output, "GenerateCFile", summary); err != nil {
			return nil, err
		}
		outputs = append(outputs, output)
	}
	return outputs, nil
}

//
// Template functions.
//

func Append(s, t string) string { return s + t }

// PrimitiveTypeName returns the C type name for a given a primitive FIDL type.
func PrimitiveTypeName(typ fidlgen.PrimitiveSubtype) string {
	switch typ {
	case fidlgen.Bool:
		return "bool"
	case fidlgen.Int8, fidlgen.Int16, fidlgen.Int32, fidlgen.Int64,
		fidlgen.Uint8, fidlgen.Uint16, fidlgen.Uint32, fidlgen.Uint64:
		return string(typ) + "_t"
	default:
		panic(fmt.Errorf("unrecognized primitive type: %s", typ))
	}
}

func TypeName(decl zither.Decl) string {
	return zither.LowerCaseWithUnderscores(decl) + "_t"
}

func ConstMemberName(parent zither.Decl, member zither.Member) string {
	return zither.UpperCaseWithUnderscores(parent) + "_" + zither.UpperCaseWithUnderscores(member)
}

// HeaderGuard returns the header guard preprocessor variable for a given file.
func HeaderGuard(summary zither.FileSummary) string {
	parts := append(strings.Split(summary.Name, "."), "h")
	parts = append(summary.Library.Parts(), parts...)
	return fidlgen.ConstNameToAllCapsSnake(strings.Join(parts, "_")) + "_"
}

// StandardIncludes gives the list of language standard headers used by a file.
func StandardIncludes(summary zither.FileSummary) []string {
	var includes []string
	for kind := range summary.TypeKinds {
		switch kind {
		case zither.TypeKindInteger:
			includes = append(includes, "stdint.h")
		case zither.TypeKindBool:
			includes = append(includes, "stdbool.h")
		}
	}
	return includes
}

// ConstValue returns the right-hand side of a generated C "constant" declaration.
func ConstValue(c zither.Const) string {
	if c.Element != nil {
		if c.Element.Member != nil {
			return ConstMemberName(c.Element.Decl, c.Element.Member)
		}
		if c.Kind == zither.TypeKindBits {
			val, err := strconv.Atoi(c.Value)
			if err != nil {
				panic(fmt.Sprintf("%s has malformed integral value: %s", c.Name, err))
			}
			return fmt.Sprintf("((%s)(%su))", TypeName(c.Element.Decl), fmt.Sprintf("%#b", val))
		}
		return zither.UpperCaseWithUnderscores(c.Element.Decl)
	}

	switch c.Kind {
	case zither.TypeKindString:
		return fmt.Sprintf("%q", c.Value)
	case zither.TypeKindBool, zither.TypeKindInteger:
		fidlType := fidlgen.PrimitiveSubtype(c.Type)
		cType := PrimitiveTypeName(fidlType)
		val := c.Value
		if c.Kind == zither.TypeKindInteger {
			// In the case of an signed type, the unsigned-ness will be
			// explicitly cast away.
			val += "u"
		}
		return fmt.Sprintf("((%s)(%s))", cType, val)
	case zither.TypeKindEnum, zither.TypeKindBits:
		// Enum and bits constants should have been handled above.
		panic(fmt.Sprintf("enum and bits constants must be given by an `Element` value: %#v", c))
	default:
		panic(fmt.Sprintf("%s has unknown constant kind: %s", c.Name, c.Type))
	}
}

// EnumMemberValue returns the value of a generated C "enum" member.
func EnumMemberValue(enum zither.Enum, member zither.EnumMember) string {
	return fmt.Sprintf("((%s)(%su))", TypeName(enum), member.Value)
}

// BitsMemberValue returns the value of a generated C bitset member.
func BitsMemberValue(bits zither.Bits, member zither.BitsMember) string {
	return fmt.Sprintf("((%s)(1u << %d))", TypeName(bits), member.Index)
}

// TypeInfo gives a basic description of a type, accounting for array nesting.
//
// For example, the type `uint32[4][5][6]` is encoded as
// `TypeInfo{"uint32", []int{4, 5, 6}}`.
type TypeInfo struct {
	// Type is the underlying type, modulo array nesting.
	Type string

	// ArrayCounts gives the successive element counts within any array
	// nesting.
	ArrayCounts []int
}

// ArraySuffix is the suffix to append to the member name to indicate any array
// nesting encoded within the type. If the type is not an array, this returns an
// empty string.
func (info TypeInfo) ArraySuffix() string {
	suffix := ""
	for _, count := range info.ArrayCounts {
		suffix += fmt.Sprintf("[%d]", count)
	}
	return suffix
}

// StructMemberTypeInfo returns the type info of a given struct member.
func StructMemberTypeInfo(member zither.StructMember) TypeInfo {
	return structMemberTypeInfo(member.Type)
}

func structMemberTypeInfo(desc zither.TypeDescriptor) TypeInfo {
	switch desc.Kind {
	case zither.TypeKindBool, zither.TypeKindInteger:
		return TypeInfo{Type: PrimitiveTypeName(fidlgen.PrimitiveSubtype(desc.Type))}
	case zither.TypeKindEnum, zither.TypeKindBits, zither.TypeKindStruct:
		return TypeInfo{Type: TypeName(desc.Decl)}
	case zither.TypeKindArray:
		info := structMemberTypeInfo(*desc.ElementType)
		info.ArrayCounts = append(info.ArrayCounts, *desc.ElementCount)
		return info
	default:
		panic(fmt.Sprintf("unsupported type kind: %v", desc.Kind))
	}
}
