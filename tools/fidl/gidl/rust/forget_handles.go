// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package rust

import (
	"fmt"
	"strings"

	gidlir "go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	gidlmixer "go.fuchsia.dev/fuchsia/tools/fidl/gidl/mixer"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

// Returns Rust code that resets all handles in expr (an expression of type
// "&mut _") to the invalid handle without closing the original handles.
func buildForgetHandles(expr string, value gidlir.Value, decl gidlmixer.Declaration) string {
	var b forgetHandleBuilder
	b.visit(expr, value, decl)
	return strings.TrimSpace(b.String())
}

type forgetHandleBuilder struct {
	strings.Builder
}

func (b *forgetHandleBuilder) write(format string, args ...interface{}) {
	b.WriteString(fmt.Sprintf(format, args...))
}

func (b *forgetHandleBuilder) visit(expr string, value gidlir.Value, decl gidlmixer.Declaration) {
	if decl.IsNullable() {
		// Unwrap the Option<...>.
		expr = fmt.Sprintf("%s.as_mut().unwrap()", expr)
		if _, ok := value.(gidlir.Record); ok {
			// Unwrap again for Option<Box<...>>.
			expr = fmt.Sprintf("%s.as_mut()", expr)
		}
	}
	switch value := value.(type) {
	case gidlir.Handle, gidlir.HandleWithRights:
		b.write("std::mem::forget(std::mem::replace(%s, Handle::invalid().into()));\n", expr)
	case gidlir.Record:
		decl := decl.(gidlmixer.RecordDeclaration)
		switch decl.(type) {
		case *gidlmixer.StructDecl:
			for _, field := range value.Fields {
				fieldExpr := fmt.Sprintf("(&mut %s.%s)", expr, field.Key.Name)
				b.visit(fieldExpr, field.Value, decl.Field(field.Key.Name))
			}
		case *gidlmixer.TableDecl:
			hasUnknown := false
			for _, field := range value.Fields {
				if field.Key.IsUnknown() {
					hasUnknown = true
					continue
				}
				fieldExpr := fmt.Sprintf("%s.%s.as_mut().unwrap()", expr, field.Key.Name)
				b.visit(fieldExpr, field.Value, decl.Field(field.Key.Name))
			}
			if decl.IsResourceType() && hasUnknown {
				b.write(`for data in %s.unknown_data.as_mut().unwrap().values_mut() {
	for h in data.handles.drain(..) { std::mem::forget(h); }
}
`, expr)
			}
		case *gidlmixer.UnionDecl:
			if len(value.Fields) != 1 {
				panic(fmt.Sprintf("union has %d fields, expected 1", len(value.Fields)))
			}
			field := value.Fields[0]
			if field.Key.IsKnown() {
				fieldName := fidlgen.ToUpperCamelCase(field.Key.Name)
				// Use another builder so that we only emit the match statement
				// if there are any handles within to forget.
				var inner forgetHandleBuilder
				inner.visit("x", field.Value, decl.Field(field.Key.Name))
				if inner.Len() == 0 {
					break
				}
				b.write(`match %s {
	%s::%s(x) => {
		%s
	}
	`, expr, declName(decl), fieldName, inner.String())
				if len(decl.FieldNames()) > 1 {
					b.write("_ => unreachable!(),")
				}
				b.write("}")
			} else {
				unknownData := field.Value.(gidlir.UnknownData)
				if len(unknownData.Handles) != 0 {
					if !decl.IsResourceType() {
						panic("non-resource type should not have unknown handles")
					}
					b.write(`match %s {
	#[allow(deprecated)]
	%s::__Unknown { data, .. } => {
		for h in data.handles.drain(..) { std::mem::forget(h); }
	}
	_ => unreachable!(),
}
`, expr, declName(decl))
				}
			}
		}
	case []gidlir.Value:
		elemDecl := decl.(gidlmixer.ListDeclaration).Elem()
		for i, elem := range value {
			b.visit(fmt.Sprintf("(&mut %s[%d])", expr, i), elem, elemDecl)
		}
	}
}
