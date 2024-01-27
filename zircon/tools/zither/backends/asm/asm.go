// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package asm

import (
	"embed"
	"fmt"
	"path/filepath"
	"strconv"
	"text/template"

	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
	"go.fuchsia.dev/fuchsia/zircon/tools/zither"
	"go.fuchsia.dev/fuchsia/zircon/tools/zither/backends/c"
)

//go:embed templates/*
var templates embed.FS

// Generator provides Assembly data layout bindings.
type Generator struct {
	fidlgen.Generator
	outputNamespaceOverride string
}

func NewGenerator(formatter fidlgen.Formatter, outputNamespaceOverride string) *Generator {
	gen := fidlgen.NewGenerator("AsmTemplates", templates, formatter, template.FuncMap{
		"MemberName": MemberName,
		"HeaderGuard": func(summary zither.FileSummary) string {
			return zither.CHeaderGuard(summary, "asm", outputNamespaceOverride)
		},
		"UpperCaseWithUnderscores": c.UpperCaseWithUnderscores,
		"ConstValue":               ConstValue,
	})
	return &Generator{*gen, outputNamespaceOverride}
}

func (gen Generator) DeclOrder() zither.DeclOrder { return zither.DependencyDeclOrder }

func (gen Generator) DeclCallback(zither.Decl) {}

func (gen *Generator) Generate(summary zither.LibrarySummary, outputDir string) ([]string, error) {
	var outputs []string
	for _, summary := range summary.Files {
		output := filepath.Join(outputDir, zither.CHeaderPath(summary, "asm", gen.outputNamespaceOverride))
		if err := gen.GenerateFile(output, "GenerateAsmFile", summary); err != nil {
			return nil, err
		}
		outputs = append(outputs, output)
	}
	return outputs, nil
}

//
// Template functions.
//

func MemberName(decl zither.Decl, member zither.Member) string {
	return c.UpperCaseWithUnderscores(decl) + "_" + c.UpperCaseWithUnderscores(member)
}

func ConstValue(cnst zither.Const) string {
	if cnst.Element != nil {
		if cnst.Element.Member != nil {
			return MemberName(cnst.Element.Decl, cnst.Element.Member)
		}
		if cnst.Kind == zither.TypeKindBits {
			val, err := strconv.Atoi(cnst.Value)
			if err != nil {
				panic(fmt.Sprintf("%s has malformed integral value: %s", cnst.Name, err))
			}
			return fmt.Sprintf("(%#b)", val)
		}
		return c.UpperCaseWithUnderscores(cnst.Element.Decl)
	}

	switch cnst.Kind {
	case zither.TypeKindString:
		return fmt.Sprintf("%q", cnst.Value)
	case zither.TypeKindBool:
		if cnst.Value == "true" {
			return "(1)"
		}
		return "(0)"
	case zither.TypeKindInteger, zither.TypeKindSize:
		return fmt.Sprintf("(%s)", cnst.Value)
	case zither.TypeKindEnum, zither.TypeKindBits:
		// Enum and bits constants should have been handled above.
		panic(fmt.Sprintf("enum and bits constants must be given by an `Element` value: %#v", cnst))
	default:
		panic(fmt.Sprintf("%s has unknown constant kind: %s", cnst.Name, cnst.Type))
	}
}
