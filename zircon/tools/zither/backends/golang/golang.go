// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package golang

import (
	"embed"
	"fmt"
	"path/filepath"
	"strconv"
	"text/template"

	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
	"go.fuchsia.dev/fuchsia/zircon/tools/zither"
)

//go:embed templates/*
var templates embed.FS

// Generator provides go data layout bindings.
type Generator struct {
	fidlgen.Generator
}

func NewGenerator(formatter fidlgen.Formatter) *Generator {
	gen := fidlgen.NewGenerator("GoTemplates", templates, formatter, template.FuncMap{
		"PackageBasename": PackageBasename,
		"PackageImports":  PackageImports,
		"Name":            zither.UpperCamelCase,
		"ConstMemberName": ConstMemberName,
		"ConstType":       ConstType,
		"ConstValue":      ConstValue,
		"DescribeType":    DescribeType,
	})
	return &Generator{*gen}
}

func (gen Generator) DeclOrder() zither.DeclOrder {
	// Go enforces no parsing order for declarations.
	return zither.SourceDeclOrder
}

func (gen *Generator) Generate(summaries []zither.FileSummary, outputDir string) ([]string, error) {
	pkgPath := filepath.Join(summaries[0].Library.Parts()...)
	outputDir = filepath.Join(outputDir, pkgPath)

	var outputs []string
	// Generate a file containing the package's name
	pkgName := filepath.Join(outputDir, "pkg_name.txt")
	if err := fidlgen.WriteFileIfChanged(pkgName, []byte(pkgPath)); err != nil {
		return nil, err
	}
	outputs = append(outputs, pkgName)

	for _, summary := range summaries {
		output := filepath.Join(outputDir, summary.Name()+".go")
		if err := gen.GenerateFile(output, "GenerateGoFile", summary); err != nil {
			return nil, err
		}
		outputs = append(outputs, output)
	}
	return outputs, nil
}

//
// Template functions.
//

func PrimitiveTypeName(typ fidlgen.PrimitiveSubtype) string {
	switch typ {
	case fidlgen.ZxExperimentalUchar:
		return "byte"
	case fidlgen.ZxExperimentalUsize:
		return "uint"
	default:
		return string(typ)
	}
}

func PackageBasename(lib fidlgen.LibraryName) string {
	parts := lib.Parts()
	return parts[len(parts)-1]
}

func PackageImports(summary zither.FileSummary) []string {
	var imports []string
	for _, kind := range summary.TypeKinds() {
		switch kind {
		case zither.TypeKindVoidPointer:
			imports = append(imports, "unsafe")
		}
	}
	return imports
}

func ConstMemberName(parent zither.Decl, member zither.Member) string {
	return zither.UpperCamelCase(parent) + zither.UpperCamelCase(member)
}

func ConstType(c zither.Const) string {
	switch c.Kind {
	case zither.TypeKindBool, zither.TypeKindString:
		return c.Type
	case zither.TypeKindInteger, zither.TypeKindSize:
		return PrimitiveTypeName(fidlgen.PrimitiveSubtype(c.Type))
	case zither.TypeKindEnum, zither.TypeKindBits:
		return zither.UpperCamelCase(c.Element.Decl)
	default:
		panic(fmt.Sprintf("%s has unknown constant kind: %s", c.Name, c.Type))
	}
}

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
			return fmt.Sprintf("%#b", val)
		}
		return zither.UpperCamelCase(c.Element.Decl)
	}

	switch c.Kind {
	case zither.TypeKindString:
		return fmt.Sprintf("%q", c.Value)
	case zither.TypeKindBool, zither.TypeKindInteger, zither.TypeKindSize:
		return c.Value
	case zither.TypeKindEnum, zither.TypeKindBits:
		// Enum and bits constants should have been handled above.
		panic(fmt.Sprintf("enum and bits constants must be given by an `Element` value: %#v", c))
	default:
		panic(fmt.Sprintf("%s has unknown constant kind: %s", c.Name, c.Type))
	}
}

func DescribeType(desc zither.TypeDescriptor) string {
	switch desc.Kind {
	case zither.TypeKindBool, zither.TypeKindInteger, zither.TypeKindSize:
		return PrimitiveTypeName(fidlgen.PrimitiveSubtype(desc.Type))
	case zither.TypeKindEnum, zither.TypeKindBits, zither.TypeKindStruct:
		layout, _ := fidlgen.MustReadName(desc.Type).SplitMember()
		return fidlgen.ToUpperCamelCase(layout.DeclarationName())
	case zither.TypeKindAlias, zither.TypeKindHandle:
		// TODO(fxbug.dev/105758): This assumes that the alias/handle was defined
		// within the same package. That's true now, but this would need to be
		// re-evaluated if/when zither supports library dependencies and the IR
		// preserves imported alias names.
		return fidlgen.ToUpperCamelCase(fidlgen.MustReadName(desc.Type).DeclarationName())
	case zither.TypeKindArray:
		return fmt.Sprintf("[%d]", *desc.ElementCount) + DescribeType(*desc.ElementType)
	case zither.TypeKindStringArray:
		return fmt.Sprintf("[%d]byte", *desc.ElementCount)
	case zither.TypeKindPointer:
		return "*" + DescribeType(*desc.ElementType)
	case zither.TypeKindVoidPointer:
		return "unsafe.Pointer"
	default:
		panic(fmt.Sprintf("unsupported type kind: %v", desc.Kind))
	}
}
