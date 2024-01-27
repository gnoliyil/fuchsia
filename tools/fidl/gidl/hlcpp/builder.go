// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package hlcpp

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"strconv"
	"strings"

	"go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	"go.fuchsia.dev/fuchsia/tools/fidl/gidl/mixer"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

func escapeStr(value string) string {
	if fidlgen.PrintableASCII(value) {
		return strconv.Quote(value)
	}
	var (
		buf    bytes.Buffer
		src    = []byte(value)
		dstLen = hex.EncodedLen(len(src))
		dst    = make([]byte, dstLen)
	)
	hex.Encode(dst, src)
	buf.WriteRune('"')
	for i := 0; i < dstLen; i += 2 {
		buf.WriteString("\\x")
		buf.WriteByte(dst[i])
		buf.WriteByte(dst[i+1])
	}
	buf.WriteRune('"')
	return buf.String()
}

func buildHandleDef(def ir.HandleDef) string {
	switch def.Subtype {
	case fidlgen.HandleSubtypeChannel:
		return fmt.Sprintf("fidl::test::util::CreateChannel(%d)", def.Rights)
	case fidlgen.HandleSubtypeEvent:
		return fmt.Sprintf("fidl::test::util::CreateEvent(%d)", def.Rights)
	default:
		panic(fmt.Sprintf("unsupported handle subtype: %s", def.Subtype))
	}
}

func handleType(subtype fidlgen.HandleSubtype) string {
	switch subtype {
	case fidlgen.HandleSubtypeChannel:
		return "ZX_OBJ_TYPE_CHANNEL"
	case fidlgen.HandleSubtypeEvent:
		return "ZX_OBJ_TYPE_EVENT"
	default:
		panic(fmt.Sprintf("unsupported handle subtype: %s", subtype))
	}
}

func BuildHandleDefs(defs []ir.HandleDef) string {
	if len(defs) == 0 {
		return ""
	}
	var builder strings.Builder
	builder.WriteString("std::vector<zx_handle_t>{\n")
	for i, d := range defs {
		builder.WriteString(buildHandleDef(d))
		// Write indices corresponding to the .gidl file handle_defs block.
		builder.WriteString(fmt.Sprintf(", // #%d\n", i))
	}
	builder.WriteString("}")
	return builder.String()
}

func BuildHandleInfoDefs(defs []ir.HandleDef) string {
	if len(defs) == 0 {
		return ""
	}
	var builder strings.Builder
	builder.WriteString("std::vector<zx_handle_info_t>{\n")
	for i, d := range defs {
		builder.WriteString(fmt.Sprintf(`
// #%d
zx_handle_info_t{
	.handle = %s,
	.type = %s,
	.rights = %d,
	.unused = 0u,
},
`, i, buildHandleDef(d), handleType(d.Subtype), d.Rights))
	}
	builder.WriteString("}")
	return builder.String()
}

func newCppValueBuilder() cppValueBuilder {
	return cppValueBuilder{}
}

type cppValueBuilder struct {
	strings.Builder

	varidx          int
	handleExtractOp string
}

func (b *cppValueBuilder) newVar() string {
	b.varidx++
	return fmt.Sprintf("v%d", b.varidx)
}

func (b *cppValueBuilder) adoptHandle(decl mixer.Declaration, value ir.HandleWithRights) string {
	return fmt.Sprintf("%s(handle_defs[%d]%s)", typeName(decl), value.Handle, b.handleExtractOp)
}

func (b *cppValueBuilder) visit(value ir.Value, decl mixer.Declaration) string {
	switch value := value.(type) {
	case bool:
		return fmt.Sprintf("%t", value)
	case int64:
		intString := fmt.Sprintf("%dll", value)
		if value == -9223372036854775808 {
			intString = "-9223372036854775807ll - 1"
		}
		switch decl := decl.(type) {
		case *mixer.IntegerDecl:
			return intString
		case *mixer.BitsDecl:
			return fmt.Sprintf("%s(%s)", typeName(decl), intString)
		case *mixer.EnumDecl:
			return fmt.Sprintf("%s(%s)", typeName(decl), intString)
		}
	case uint64:
		switch decl := decl.(type) {
		case *mixer.IntegerDecl:
			return fmt.Sprintf("%dull", value)
		case *mixer.BitsDecl:
			return fmt.Sprintf("%s(%dull)", typeName(decl), value)
		case *mixer.EnumDecl:
			return fmt.Sprintf("%s(%dull)", typeName(decl), value)
		}
	case float64:
		switch decl := decl.(type) {
		case *mixer.FloatDecl:
			switch decl.Subtype() {
			case fidlgen.Float32:
				s := fmt.Sprintf("%g", value)
				if strings.Contains(s, ".") {
					return fmt.Sprintf("%sf", s)
				} else {
					return s
				}
			case fidlgen.Float64:
				return fmt.Sprintf("%g", value)
			}
		}
	case ir.RawFloat:
		switch decl.(*mixer.FloatDecl).Subtype() {
		case fidlgen.Float32:
			return fmt.Sprintf("([] { uint32_t u = %#b; float f; memcpy(&f, &u, 4); return f; })()", value)
		case fidlgen.Float64:
			return fmt.Sprintf("([] { uint64_t u = %#b; double d; memcpy(&d, &u, 8); return d; })()", value)
		}
	case string:
		return fmt.Sprintf("%s(%s, %d)", typeName(decl), escapeStr(value), len(value))
	case ir.HandleWithRights:
		switch decl := decl.(type) {
		case *mixer.HandleDecl:
			return b.adoptHandle(decl, value)
		case *mixer.ClientEndDecl:
			return fmt.Sprintf("%s(%s)", typeName(decl), b.adoptHandle(decl.UnderlyingHandleDecl(), value))
		case *mixer.ServerEndDecl:
			return fmt.Sprintf("%s(%s)", typeName(decl), b.adoptHandle(decl.UnderlyingHandleDecl(), value))
		}
	case ir.Record:
		return b.visitRecord(value, decl.(mixer.RecordDeclaration))
	case []ir.Value:
		switch decl := decl.(type) {
		case *mixer.ArrayDecl:
			return b.visitArray(value, decl)
		case *mixer.VectorDecl:
			return b.visitVector(value, decl)
		}
	case nil:
		return fmt.Sprintf("%s()", typeName(decl))
	}
	panic(fmt.Sprintf("not implemented: %T", value))
}

func (b *cppValueBuilder) visitRecord(value ir.Record, decl mixer.RecordDeclaration) string {
	containerVar := b.newVar()
	nullable := decl.IsNullable()
	if nullable {
		b.Builder.WriteString(fmt.Sprintf(
			"%s %s = std::make_unique<%s>();\n", typeName(decl), containerVar, declName(decl)))
	} else {
		b.Builder.WriteString(fmt.Sprintf("%s %s;\n", typeName(decl), containerVar))
	}

	_, isTable := decl.(*mixer.TableDecl)
	for _, field := range value.Fields {
		accessor := "."
		if nullable {
			accessor = "->"
		}
		b.Builder.WriteString("\n")

		if field.Key.IsUnknown() {
			if isTable {
				panic("unknown table fields not supported for HLCPP")
			}
			unknownData := field.Value.(ir.UnknownData)
			if decl.IsResourceType() {
				b.Builder.WriteString(fmt.Sprintf(
					"%s%sSetUnknownData(static_cast<fidl_xunion_tag_t>(%dlu), %s, %s);\n",
					containerVar, accessor, field.Key.UnknownOrdinal, BuildBytes(unknownData.Bytes),
					buildHandles(unknownData.Handles, b.handleExtractOp)))
			} else {
				b.Builder.WriteString(fmt.Sprintf(
					"%s%sSetUnknownData(static_cast<fidl_xunion_tag_t>(%dlu), %s);\n",
					containerVar, accessor, field.Key.UnknownOrdinal, BuildBytes(unknownData.Bytes)))
			}
			continue
		}

		fieldVar := b.visit(field.Value, decl.Field(field.Key.Name))

		switch decl.(type) {
		case *mixer.StructDecl:
			b.Builder.WriteString(fmt.Sprintf(
				"%s%s%s = %s;\n", containerVar, accessor, field.Key.Name, fieldVar))
		default:
			b.Builder.WriteString(fmt.Sprintf(
				"%s%sset_%s(%s);\n", containerVar, accessor, field.Key.Name, fieldVar))
		}
	}
	return fmt.Sprintf("std::move(%s)", containerVar)
}

func (b *cppValueBuilder) visitArray(value []ir.Value, decl *mixer.ArrayDecl) string {
	var elements []string
	elemDecl := decl.Elem()
	for _, item := range value {
		elements = append(elements, fmt.Sprintf("%s", b.visit(item, elemDecl)))
	}
	// Populate the array using aggregate initialization.
	return fmt.Sprintf("%s{%s}",
		typeName(decl), strings.Join(elements, ", "))
}

func (b *cppValueBuilder) visitVector(value []ir.Value, decl *mixer.VectorDecl) string {
	elemDecl := decl.Elem()
	var elements []string
	for _, item := range value {
		elements = append(elements, b.visit(item, elemDecl))
	}
	if _, ok := elemDecl.(mixer.PrimitiveDeclaration); ok {
		// Populate the vector using aggregate initialization.
		if decl.IsNullable() {
			return fmt.Sprintf("%s{{%s}}", typeName(decl), strings.Join(elements, ", "))
		}
		return fmt.Sprintf("%s{%s}", typeName(decl), strings.Join(elements, ", "))
	}
	vectorVar := b.newVar()
	// Populate the vector using push_back. We can't use an initializer list
	// because they always copy, which breaks if the element is a unique_ptr.
	b.Builder.WriteString(fmt.Sprintf("%s %s;\n", typeName(decl), vectorVar))
	if decl.IsNullable() && value != nil {
		b.Builder.WriteString(fmt.Sprintf("%s.emplace();\n", vectorVar))
	}
	accessor := "."
	if decl.IsNullable() {
		accessor = "->"
	}
	for _, element := range elements {
		b.Builder.WriteString(fmt.Sprintf("%s%spush_back(%s);\n", vectorVar, accessor, element))
	}
	return fmt.Sprintf("std::move(%s)", vectorVar)
}

func typeName(decl mixer.Declaration) string {
	switch decl := decl.(type) {
	case mixer.PrimitiveDeclaration:
		return primitiveTypeName(decl.Subtype())
	case *mixer.StringDecl:
		if decl.IsNullable() {
			return "::fidl::StringPtr"
		}
		return "std::string"
	case *mixer.ArrayDecl:
		return fmt.Sprintf("std::array<%s, %d>", typeName(decl.Elem()), decl.Size())
	case *mixer.VectorDecl:
		if decl.IsNullable() {
			return fmt.Sprintf("::fidl::VectorPtr<%s>", typeName(decl.Elem()))
		}
		return fmt.Sprintf("std::vector<%s>", typeName(decl.Elem()))
	case *mixer.HandleDecl:
		switch decl.Subtype() {
		case fidlgen.HandleSubtypeNone:
			return "zx::handle"
		case fidlgen.HandleSubtypeChannel:
			return "zx::channel"
		case fidlgen.HandleSubtypeEvent:
			return "zx::event"
		default:
			panic(fmt.Sprintf("Handle subtype not supported %s", decl.Subtype()))
		}
	case *mixer.ClientEndDecl:
		return fmt.Sprintf("fidl::InterfaceHandle<%s>", endpointDeclName(decl))
	case *mixer.ServerEndDecl:
		return fmt.Sprintf("fidl::InterfaceRequest<%s>", endpointDeclName(decl))
	case mixer.NamedDeclaration:
		if decl.IsNullable() {
			return fmt.Sprintf("std::unique_ptr<%s>", declName(decl))
		}
		return declName(decl)
	default:
		panic("unhandled case")
	}
}

func declName(decl mixer.NamedDeclaration) string {
	parts := strings.Split(decl.Name(), "/")
	library_parts := strings.Split(parts[0], ".")
	return strings.Join(append(library_parts, parts[1]), "::")
}

func endpointDeclName(decl mixer.EndpointDeclaration) string {
	parts := strings.Split(decl.ProtocolName(), "/")
	library_parts := strings.Split(parts[0], ".")
	return strings.Join(append(library_parts, parts[1]), "::")
}

func primitiveTypeName(subtype fidlgen.PrimitiveSubtype) string {
	switch subtype {
	case fidlgen.Bool:
		return "bool"
	case fidlgen.Uint8, fidlgen.Uint16, fidlgen.Uint32, fidlgen.Uint64,
		fidlgen.Int8, fidlgen.Int16, fidlgen.Int32, fidlgen.Int64:
		return fmt.Sprintf("%s_t", subtype)
	case fidlgen.Float32:
		return "float"
	case fidlgen.Float64:
		return "double"
	default:
		panic(fmt.Sprintf("unexpected subtype %s", subtype))
	}
}

func BuildBytes(bytes []byte) string {
	var builder strings.Builder
	builder.WriteString("std::vector<uint8_t>{")
	for i, b := range bytes {
		if i%8 == 0 {
			builder.WriteString("\n")
		}
		builder.WriteString(fmt.Sprintf("0x%02x,", b))
	}
	builder.WriteString("\n}")
	return builder.String()
}

func buildRawHandleImpl(handles []ir.Handle, handleType, handleExtractOp string) string {
	if len(handles) == 0 {
		return fmt.Sprintf("std::vector<%s>{}", handleType)
	}
	var builder strings.Builder
	builder.WriteString(fmt.Sprintf("std::vector<%s>{\n", handleType))
	for i, h := range handles {
		builder.WriteString(fmt.Sprintf("handle_defs[%d]%s,", h, handleExtractOp))
		if i%8 == 7 {
			builder.WriteString("\n")
		}
	}
	builder.WriteString("}")
	return builder.String()
}

func BuildRawHandles(handles []ir.Handle) string {
	return buildRawHandleImpl(handles, "zx_handle_t", "")
}

func BuildRawHandlesFromHandleInfos(handles []ir.Handle) string {
	return buildRawHandleImpl(handles, "zx_handle_t", ".handle")
}

func BuildRawHandleInfos(handles []ir.Handle) string {
	return buildRawHandleImpl(handles, "zx_handle_info_t", "")
}

func BuildRawHandleDispositions(handle_dispositions []ir.HandleDisposition) string {
	if len(handle_dispositions) == 0 {
		return fmt.Sprintf("std::vector<zx_handle_disposition_t>{}")
	}
	var builder strings.Builder
	builder.WriteString(fmt.Sprintf("std::vector<zx_handle_disposition_t>{"))
	for _, h := range handle_dispositions {
		builder.WriteString(fmt.Sprintf(`
{
	.operation = ZX_HANDLE_OP_MOVE,
	.handle = handle_defs[%d],
	.type = %d,
	.rights = %d,
	.result = ZX_OK,
},`, h.Handle, h.Type, h.Rights))
	}
	builder.WriteString("}")
	return builder.String()
}

func buildHandles(handles []ir.Handle, handleExtractOp string) string {
	if len(handles) == 0 {
		return "std::vector<zx::handle>{}"
	}
	var builder strings.Builder
	// Initializer-list vectors only work for copyable types. zx::handle has no
	// copy constructor, so we use an immediately-invoked lambda instead.
	builder.WriteString("([&handle_defs] {\n")
	builder.WriteString("std::vector<zx::handle> v;\n")
	for _, h := range handles {
		builder.WriteString(fmt.Sprintf("v.emplace_back(handle_defs[%d]%s);\n", h, handleExtractOp))
	}
	builder.WriteString("return v;\n")
	builder.WriteString("})()")
	return builder.String()
}
