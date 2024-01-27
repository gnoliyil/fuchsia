// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package llcpp

import (
	"fmt"
	"strings"

	"go.fuchsia.dev/fuchsia/tools/fidl/gidl/lib/ir"
	"go.fuchsia.dev/fuchsia/tools/fidl/gidl/lib/mixer"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

// EqualityCheck contains the necessary information to render an equality check.
// See BuildEqualityCheck.
type EqualityCheck = struct {
	// InputVar is the name of the wire domain object to be checked.
	InputVar string

	// HelperStatements is a series of statements binding particular fields
	// in a wire domain object to named references. It should precede the
	// EqualityExpr when rendered.
	HelperStatements string

	// Expr is an expression checking the named references from
	// HelperStatements against their expected value.
	Expr string
}

// BuildEqualityCheck builds an ad-hoc equality test verifying that a wire domain object
// matches the expected value.
//
// In particular, an actual handle having the same KOID, type, and rights as the expected
// handle is considered equal to the expected, despite possibly having different handle
// numbers, to accommodate handle replacement.
func BuildEqualityCheck(actualVar string, expectedValue ir.Value, decl mixer.Declaration, handleKoidVectorName string) EqualityCheck {
	builder := equalityCheckBuilder{
		handleKoidVectorName: handleKoidVectorName,
	}
	resultValue := builder.visit(fidlExpr(actualVar), expectedValue, decl)
	resultBuild := builder.String()
	return EqualityCheck{
		InputVar:         actualVar,
		HelperStatements: resultBuild,
		Expr:             string(resultValue),
	}
}

// A boolean expression in C++ (i.e. the output of the check).
type boolExpr string

// A fidl expression in C++ (i.e. one of the input FIDL objects or subobjects).
type fidlExpr string

// Create a fidl expression from a formatted string.
func fidlSprintf(format string, vals ...interface{}) fidlExpr {
	return fidlExpr(fmt.Sprintf(format, vals...))
}

// Create a boolean expression from a formatted string.
func boolSprintf(format string, vals ...interface{}) boolExpr {
	return boolExpr(fmt.Sprintf(format, vals...))
}

// Join a list of boolean expressions into a new expression that is true iff all of the inputs are true.
func boolJoin(exprs []boolExpr) boolExpr {
	if len(exprs) == 0 {
		return "true"
	}

	var strs []string
	for _, expr := range exprs {
		strs = append(strs, string(expr))
	}
	return boolExpr(fmt.Sprintf("(%s)", strings.Join(strs, " && ")))
}

// Generator of new variable names from a sequence.
type varSeq int

func (v *varSeq) next() int {
	*v++
	return int(*v)
}

func (v *varSeq) nextBoolVar() boolExpr {
	return boolExpr(fmt.Sprintf("b%d", v.next()))
}
func (v *varSeq) nextFidlVar() fidlExpr {
	return fidlExpr(fmt.Sprintf("f%d", v.next()))
}

type equalityCheckBuilder struct {
	strings.Builder
	varSeq varSeq
	// Name of a C++ variable containing an vector of zx_koid_t of handle values
	// This is read-only and is used for checking handle koid equality.
	handleKoidVectorName string
}

func (b *equalityCheckBuilder) write(format string, vals ...interface{}) {
	b.WriteString(fmt.Sprintf(format, vals...))
}

func (b *equalityCheckBuilder) createAndAssignVar(val fidlExpr) fidlExpr {
	varName := b.varSeq.nextFidlVar()
	b.write("[[maybe_unused]] const auto& %s = %s;\n", varName, val)
	return varName
}

func (b *equalityCheckBuilder) construct(typename string, fmtStr string, args ...interface{}) fidlExpr {
	val := fmt.Sprintf(fmtStr, args...)
	return fidlSprintf("%s(%s)", typename, val)
}

func (b *equalityCheckBuilder) equals(actual, expected fidlExpr) boolExpr {
	return boolSprintf("(%s == %s)", actual, expected)
}

func (b *equalityCheckBuilder) visit(actualExpr fidlExpr, expectedValue ir.Value, decl mixer.Declaration) boolExpr {
	switch expectedValue := expectedValue.(type) {
	case bool:
		return b.equals(actualExpr, b.construct(typeName(decl), "%t", expectedValue))
	case int64, uint64, float64:
		switch decl := decl.(type) {
		case mixer.PrimitiveDeclaration, *mixer.EnumDecl:
			return b.equals(actualExpr, b.construct(typeName(decl), formatPrimitive(expectedValue)))
		case *mixer.BitsDecl:
			return b.equals(actualExpr, fidlSprintf("static_cast<%s>(%s)", declName(decl), formatPrimitive(expectedValue)))
		}
	case ir.RawFloat:
		switch decl.(*mixer.FloatDecl).Subtype() {
		case fidlgen.Float32:
			return b.equals(actualExpr, fidlSprintf("([] { uint32_t u = %#b; float f; memcpy(&f, &u, sizeof(float)); return f; })()", expectedValue))
		case fidlgen.Float64:
			return b.equals(actualExpr, fidlSprintf("([] { uint64_t u = %#b; double d; memcpy(&d, &u, sizeof(double)); return d; })()", expectedValue))
		}
	case string:
		return boolSprintf("(%[1]s.size() == %[3]d && memcmp(%[1]s.data(), %[2]q, %[3]d) == 0)", actualExpr, expectedValue, len(expectedValue))
	case ir.AnyHandle:
		switch decl := decl.(type) {
		case *mixer.HandleDecl:
			return b.visitHandle(actualExpr, expectedValue, decl, ownedHandle)
		case *mixer.ClientEndDecl:
			return b.visitClientEnd(actualExpr, expectedValue, decl)
		case *mixer.ServerEndDecl:
			return b.visitServerEnd(actualExpr, expectedValue, decl)
		}
	case ir.Record:
		switch decl := decl.(type) {
		case *mixer.StructDecl:
			return b.visitStruct(actualExpr, expectedValue, decl)
		case *mixer.TableDecl:
			return b.visitTable(actualExpr, expectedValue, decl)
		case *mixer.UnionDecl:
			return b.visitUnion(actualExpr, expectedValue, decl)
		}
	case []ir.Value:
		return b.visitList(actualExpr, expectedValue, decl.(mixer.ListDeclaration))
	case nil:
		switch decl.(type) {
		case *mixer.VectorDecl:
			return boolSprintf("(%s.data() == nullptr)", actualExpr)
		case *mixer.StringDecl:
			return boolSprintf("%s.is_null()", actualExpr)
		case *mixer.HandleDecl:
			return boolSprintf("!%s.is_valid()", actualExpr)
		case *mixer.UnionDecl:
			return boolSprintf("!%s.has_value()", actualExpr)
		case *mixer.StructDecl:
			return boolSprintf("(%s == nullptr)", actualExpr)
		}
	}
	panic(fmt.Sprintf("not implemented: %T (decl: %T)", expectedValue, decl))
}

type handleOwnership int64

const (
	unownedHandle handleOwnership = iota
	ownedHandle
)

func (b *equalityCheckBuilder) visitHandle(actualExpr fidlExpr, expectedValue ir.AnyHandle, decl *mixer.HandleDecl, ownership handleOwnership) boolExpr {
	actualVar := b.createAndAssignVar(actualExpr)
	resultVar := b.varSeq.nextBoolVar()
	var handleValueExpr string
	switch ownership {
	case unownedHandle:
		handleValueExpr = fmt.Sprintf("%s->get()", actualVar)
	case ownedHandle:
		handleValueExpr = fmt.Sprintf("%s.get()", actualVar)
	}
	// Check:
	// - Original handle's koid matches final handle (it could be replaced so can't check handle value).
	// - Type matches expectation.
	// - Rights matches expectation.
	b.write(`
    zx_info_handle_basic_t %[1]s_info;
    ZX_ASSERT(ZX_OK == zx_object_get_info(%[2]s, ZX_INFO_HANDLE_BASIC, &%[1]s_info, sizeof(%[1]s_info), nullptr, nullptr));
    bool %[1]s = %[1]s_info.koid == %[3]s[%[4]d]
	`, resultVar, handleValueExpr, b.handleKoidVectorName, expectedValue.GetHandle())
	if expectedValue, ok := expectedValue.(ir.RestrictedHandle); ok {
		b.write("&& %s_info.type == %d && %s_info.rights == %d",
			resultVar, expectedValue.Type, resultVar, expectedValue.Rights)
	}
	b.write(";")
	return resultVar
}

func (b *equalityCheckBuilder) visitClientEnd(actualExpr fidlExpr, expectedValue ir.AnyHandle, decl *mixer.ClientEndDecl) boolExpr {
	return b.visitHandle(fidlExpr(fmt.Sprintf("(%s).handle()", actualExpr)), expectedValue, decl.UnderlyingHandleDecl(), unownedHandle)
}

func (b *equalityCheckBuilder) visitServerEnd(actualExpr fidlExpr, expectedValue ir.AnyHandle, decl *mixer.ServerEndDecl) boolExpr {
	return b.visitHandle(fidlExpr(fmt.Sprintf("(%s).handle()", actualExpr)), expectedValue, decl.UnderlyingHandleDecl(), unownedHandle)
}

func (b *equalityCheckBuilder) visitStruct(actualExpr fidlExpr, expectedValue ir.Record, decl *mixer.StructDecl) boolExpr {
	op := "."
	if decl.IsNullable() {
		op = "->"
	}
	actualVar := b.createAndAssignVar(actualExpr)
	var fieldEquality []boolExpr
	for _, field := range expectedValue.Fields {
		actualFieldExpr := fidlSprintf("%s%s%s", actualVar, op, field.Key.Name)
		fieldEquality = append(fieldEquality,
			b.visit(actualFieldExpr, field.Value, decl.Field(field.Key.Name)))
	}
	return boolJoin(fieldEquality)
}

func (b *equalityCheckBuilder) visitTable(actualExpr fidlExpr, expectedValue ir.Record, decl *mixer.TableDecl) boolExpr {
	actualVar := b.createAndAssignVar(actualExpr)
	var fieldEquality []boolExpr
	expectedFieldValues := map[string]ir.Value{}
	for _, field := range expectedValue.Fields {
		if field.Key.IsUnknown() {
			panic("LLCPP does not support constructing unknown fields")
		}
		expectedFieldValues[field.Key.Name] = field.Value
	}
	for _, fieldName := range decl.FieldNames() {
		if expectedFieldValue, ok := expectedFieldValues[fieldName]; ok {
			fieldEquality = append(fieldEquality, boolSprintf("%s.has_%s()", actualVar, fieldName))
			actualFieldExpr := fidlSprintf("%s.%s()", actualVar, fieldName)
			fieldEquality = append(fieldEquality,
				b.visit(actualFieldExpr, expectedFieldValue, decl.Field(fieldName)))
		} else {
			fieldEquality = append(fieldEquality, boolSprintf("!%s.has_%s()", actualVar, fieldName))
		}
	}
	if len(fieldEquality) == 0 {
		return "true"
	}
	return boolJoin(fieldEquality)
}

func (b *equalityCheckBuilder) visitUnion(actualExpr fidlExpr, expectedValue ir.Record, decl *mixer.UnionDecl) boolExpr {
	actualVar := b.createAndAssignVar(actualExpr)
	if len(expectedValue.Fields) != 1 {
		panic("shouldn't happen")
	}
	field := expectedValue.Fields[0]
	if field.Key.IsUnknown() {
		panic("LLCPP does not support constructing unknown fields")
	}
	op := "."
	presenceCheck := ""
	if decl.IsNullable() {
		op = "->"
		presenceCheck = fmt.Sprintf("%s.has_value() && ", actualVar)
	}
	actualFieldExpr := fidlSprintf("%s%s%s()", actualVar, op, fidlgen.ToSnakeCase(field.Key.Name))
	fieldEquality := b.visit(actualFieldExpr, field.Value, decl.Field(field.Key.Name))
	return boolSprintf("(%s%s%sWhich() == %s::Tag::%s && %s)",
		presenceCheck, actualVar, op, declName(decl), fidlgen.ConstNameToKCamelCase(field.Key.Name), fieldEquality)
}

func (b *equalityCheckBuilder) visitList(actualExpr fidlExpr, expectedValue []ir.Value, decl mixer.ListDeclaration) boolExpr {
	actualVar := b.createAndAssignVar(actualExpr)
	var equalityChecks []boolExpr
	if _, ok := decl.(*mixer.VectorDecl); ok {
		equalityChecks = append(equalityChecks, boolSprintf("%s.count() == %d", actualVar, len(expectedValue)))
	}
	for i, item := range expectedValue {
		equalityChecks = append(equalityChecks, b.visit(fidlSprintf("%s[%d]", actualVar, i), item, decl.Elem()))
	}
	if len(equalityChecks) == 0 {
		return "true"
	}
	return boolJoin(equalityChecks)
}
