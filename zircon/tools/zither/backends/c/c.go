// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package c

import (
	"embed"
	"fmt"
	"path/filepath"
	"sort"
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
	outputNamespaceOverride string
}

func NewGenerator(formatter fidlgen.Formatter, outputNamespaceOverride string) *Generator {
	gen := fidlgen.NewGenerator("CTemplates", templates, formatter, template.FuncMap{
		"LowerCaseWithUnderscores": LowerCaseWithUnderscores,
		"UpperCaseWithUnderscores": UpperCaseWithUnderscores,
		"Append":                   Append,
		"PrimitiveTypeName":        PrimitiveTypeName,
		"HeaderGuard": func(summary zither.FileSummary) string {
			return zither.CHeaderGuard(summary, "c", outputNamespaceOverride)
		},
		"StandardIncludes": StandardIncludes,
		"TypeName":         TypeName,
		"ConstMemberName":  ConstMemberName,
		"ConstValue":       ConstValue,
		"EnumMemberValue":  EnumMemberValue,
		"BitsMemberValue":  BitsMemberValue,
		"DescribeType":     DescribeType,
	})
	return &Generator{*gen, outputNamespaceOverride}
}

func (gen Generator) DeclOrder() zither.DeclOrder { return zither.DependencyDeclOrder }

func (gen Generator) DeclCallback(zither.Decl) {}

func (gen *Generator) Generate(summary zither.LibrarySummary, outputDir string) ([]string, error) {
	var outputs []string
	for _, summary := range summary.Files {
		output := filepath.Join(outputDir, zither.CHeaderPath(summary, "c", gen.outputNamespaceOverride))
		if err := gen.GenerateFile(output, "GenerateCFile", summary); err != nil {
			return nil, err
		}
		outputs = append(outputs, output)
	}

	readme := filepath.Join(outputDir, "README.md")
	if err := zither.WriteReadme(readme, summary); err != nil {
		return nil, err
	}
	outputs = append(outputs, readme)

	return outputs, nil
}

//
// Template functions.
//

// LowerCaseWithUnderscores is a wrapper around the zither-defined utility that
// also includes the library name.
func LowerCaseWithUnderscores(el zither.Element) string {
	decl, ok := el.(zither.Decl)
	if ok {
		libParts := decl.GetName().LibraryName().Parts()
		prefix := fidlgen.ToSnakeCase(strings.Join(libParts, "_"))
		return prefix + "_" + zither.LowerCaseWithUnderscores(el)
	}
	return zither.LowerCaseWithUnderscores(el)
}

// UpperCaseWithUnderscores is a wrapper around the zither-defined utility that
// also includes the library name.
func UpperCaseWithUnderscores(el zither.Element) string {
	return strings.ToUpper(LowerCaseWithUnderscores(el))
}

func Append(s, t string) string { return s + t }

// PrimitiveTypeName returns the C type name for a given a primitive FIDL type.
func PrimitiveTypeName(typ fidlgen.PrimitiveSubtype) string {
	switch typ {
	case fidlgen.Bool:
		return "bool"
	case fidlgen.ZxExperimentalUchar:
		return "char"
	case fidlgen.ZxExperimentalUsize64:
		return "size_t"
	case fidlgen.ZxExperimentalUintptr64:
		return "uintptr_t"
	case fidlgen.Int8, fidlgen.Int16, fidlgen.Int32, fidlgen.Int64,
		fidlgen.Uint8, fidlgen.Uint16, fidlgen.Uint32, fidlgen.Uint64:
		return string(typ) + "_t"
	default:
		panic(fmt.Errorf("unrecognized primitive type: %s", typ))
	}
}

func TypeName(decl zither.Decl) string {
	return LowerCaseWithUnderscores(decl) + "_t"
}

func ConstMemberName(parent zither.Decl, member zither.Member) string {
	return UpperCaseWithUnderscores(parent) + "_" + UpperCaseWithUnderscores(member)
}

// StandardIncludes gives the list of language standard headers used by a file.
func StandardIncludes(summary zither.FileSummary) []string {
	includes := make(map[string]struct{})
	for _, kind := range summary.TypeKinds() {
		switch kind {
		case zither.TypeKindInteger, zither.TypeKindOverlay:
			includes["stdint.h"] = struct{}{}
		case zither.TypeKindBool:
			includes["stdbool.h"] = struct{}{}
		case zither.TypeKindSize:
			includes["stddef.h"] = struct{}{}
		}
	}
	var vals []string
	for inc := range includes {
		vals = append(vals, inc)
	}
	sort.Strings(vals)
	return vals
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
		return UpperCaseWithUnderscores(c.Element.Decl)
	}

	switch c.Kind {
	case zither.TypeKindString:
		return fmt.Sprintf("%q", c.Value)
	case zither.TypeKindBool, zither.TypeKindInteger, zither.TypeKindSize:
		fidlType := fidlgen.PrimitiveSubtype(c.Type)
		cType := PrimitiveTypeName(fidlType)
		val := c.Value
		if c.Kind == zither.TypeKindInteger || c.Kind == zither.TypeKindSize {
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

func (info TypeInfo) String() string {
	return info.Type + info.ArraySuffix()
}

func DescribeType(desc zither.TypeDescriptor) TypeInfo {
	switch desc.Kind {
	case zither.TypeKindBool, zither.TypeKindInteger, zither.TypeKindSize:
		return TypeInfo{Type: PrimitiveTypeName(fidlgen.PrimitiveSubtype(desc.Type))}
	case zither.TypeKindEnum, zither.TypeKindBits, zither.TypeKindStruct,
		zither.TypeKindOverlay, zither.TypeKindAlias, zither.TypeKindHandle:
		return TypeInfo{Type: TypeName(desc.Decl)}
	case zither.TypeKindArray:
		info := DescribeType(*desc.ElementType)
		info.ArrayCounts = append(info.ArrayCounts, *desc.ElementCount)
		return info
	case zither.TypeKindStringArray:
		return TypeInfo{Type: "char", ArrayCounts: []int{*desc.ElementCount}}
	case zither.TypeKindPointer:
		info := DescribeType(*desc.ElementType)
		info.Type += "*"
		return info
	case zither.TypeKindVoidPointer:
		return TypeInfo{Type: "void*"}
	default:
		panic(fmt.Sprintf("unsupported type kind: %v", desc.Kind))
	}
}
