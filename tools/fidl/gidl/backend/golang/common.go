// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package golang

import (
	"bytes"
	"fmt"
	"go/format"
	"io"
	"os"
	"strconv"
	"strings"
	"text/template"

	"go.fuchsia.dev/fuchsia/tools/fidl/gidl/lib/ir"
	"go.fuchsia.dev/fuchsia/tools/fidl/gidl/lib/mixer"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

// withGoFmt wraps a template that produces Go source code, and formats the
// execution result using go/format.
type withGoFmt struct {
	template *template.Template
}

func (w withGoFmt) Execute(wr io.Writer, data interface{}) error {
	var b bytes.Buffer
	if err := w.template.Execute(&b, data); err != nil {
		return err
	}
	formatted, err := format.Source(b.Bytes())
	if err != nil {
		fmt.Fprintf(os.Stderr, "gofmt failed: %s\n", err)
		_, err = wr.Write(b.Bytes())
		return err
	}
	_, err = wr.Write(formatted)
	return err
}

func buildBytes(bytes []byte) string {
	var builder strings.Builder
	builder.WriteString("[]byte{\n")
	for i, b := range bytes {
		builder.WriteString(fmt.Sprintf("0x%02x,", b))
		if i%8 == 7 {
			builder.WriteString("\n")
		}
	}
	builder.WriteString("}")
	return builder.String()
}

func buildHandleDefs(defs []ir.HandleDef) string {
	if len(defs) == 0 {
		return ""
	}
	var builder strings.Builder
	builder.WriteString("[]handleDef{\n")
	for i, d := range defs {
		var subtype string
		rights := d.Rights
		switch d.Subtype {
		case fidlgen.HandleSubtypeChannel:
			subtype = "zx.ObjTypeChannel"
			// Always use real rights instead of a "same rights" placeholder.
			if rights == fidlgen.HandleRightsSameRights {
				r, ok := ir.HandleRightsByName("channel_default")
				if !ok {
					panic("channel_default should be a supported rights name")
				}
				rights = r
			}
		case fidlgen.HandleSubtypeEvent:
			subtype = "zx.ObjTypeEvent"
			// Always use real rights instead of a "same rights" placeholder.
			if rights == fidlgen.HandleRightsSameRights {
				r, ok := ir.HandleRightsByName("event_default")
				if !ok {
					panic("event_default should be a supported rights name")
				}
				rights = r
			}
		default:
			panic(fmt.Sprintf("unsupported handle subtype: %s", d.Subtype))
		}
		// Write indices corresponding to the .gidl file handle_defs block.
		builder.WriteString(fmt.Sprintf(`
	// #%d:
	{
		subtype: %s,
		rights: %d,
	},
`, i, subtype, rights))
	}
	builder.WriteString("}")
	return builder.String()
}

func buildHandleInfos(handles []ir.Handle) string {
	if len(handles) == 0 {
		return "nil"
	}
	var builder strings.Builder
	builder.WriteString("[]zx.HandleInfo{")
	for _, handle := range handles {
		builder.WriteString(fmt.Sprintf(`
		{Handle: handles[%d], Type: handleDefs[%d].subtype, Rights: handleDefs[%d].rights},`, handle, handle, handle))
	}
	builder.WriteString("\n}")
	return builder.String()
}

func buildHandleDispositions(handleDispositions []ir.HandleDisposition) string {
	if len(handleDispositions) == 0 {
		return "nil"
	}
	var builder strings.Builder
	builder.WriteString("[]zx.HandleDisposition{")
	for _, handleDisposition := range handleDispositions {
		builder.WriteString(fmt.Sprintf(`
	{
		Operation: zx.HandleOpMove,
		Handle: handles[%d],
		Type: %d,
		Rights: %d,
		Result: zx.ErrOk,
	},`,
			handleDisposition.Handle, handleDisposition.Type, handleDisposition.Rights))
	}
	builder.WriteString("\n}")
	return builder.String()
}

func buildUnknownData(data ir.UnknownData) string {
	return fmt.Sprintf(
		"fidl.UnknownData{\nBytes: %s, \nHandles: %s,\n}",
		buildBytes(data.Bytes),
		buildHandleInfos(data.Handles))
}

func buildUnknownDataMap(fields []ir.Field) string {
	if len(fields) == 0 {
		return "nil"
	}
	var builder strings.Builder
	builder.WriteString("map[uint64]fidl.UnknownData{\n")
	for _, field := range fields {
		builder.WriteString(fmt.Sprintf(
			"%d: %s,",
			field.Key.UnknownOrdinal,
			buildUnknownData(field.Value.(ir.UnknownData))))
	}
	builder.WriteString("}")
	return builder.String()
}

func visit(value ir.Value, decl mixer.Declaration) string {
	switch value := value.(type) {
	case bool, int64, uint64, float64:
		switch decl := decl.(type) {
		case mixer.PrimitiveDeclaration:
			return fmt.Sprintf("%#v", value)
		case *mixer.BitsDecl, *mixer.EnumDecl:
			return fmt.Sprintf("%s(%d)", typeLiteral(decl), value)
		}
	case ir.RawFloat:
		switch decl.(*mixer.FloatDecl).Subtype() {
		case fidlgen.Float32:
			return fmt.Sprintf("math.Float32frombits(%#b)", value)
		case fidlgen.Float64:
			return fmt.Sprintf("math.Float64frombits(%#b)", value)
		}
	case string:
		if decl.IsNullable() {
			// Taking an address of a string literal is not allowed, so instead
			// we create a slice and get the address of its first element.
			return fmt.Sprintf("&[]string{%q}[0]", value)
		}
		return strconv.Quote(value)
	case ir.Handle:
		rawHandle := fmt.Sprintf("handles[%d]", value)
		switch decl := decl.(type) {
		case *mixer.ClientEndDecl:
			return fmt.Sprintf("%s{Channel: zx.Channel(%s)}", endpointDeclName(decl), rawHandle)
		case *mixer.ServerEndDecl:
			return fmt.Sprintf("%s{Channel: zx.Channel(%s)}", endpointDeclName(decl), rawHandle)
		case *mixer.HandleDecl:
			switch decl.Subtype() {
			case fidlgen.HandleSubtypeNone:
				return rawHandle
			case fidlgen.HandleSubtypeChannel:
				return fmt.Sprintf("zx.Channel(%s)", rawHandle)
			case fidlgen.HandleSubtypeEvent:
				return fmt.Sprintf("zx.Event(%s)", rawHandle)
			default:
				panic(fmt.Sprintf("Handle subtype not supported %s", decl.Subtype()))
			}
		}
	case ir.Record:
		if decl, ok := decl.(mixer.RecordDeclaration); ok {
			return onRecord(value, decl)
		}
	case []ir.Value:
		if decl, ok := decl.(mixer.ListDeclaration); ok {
			return onList(value, decl)
		}
	case nil:
		if _, ok := decl.(*mixer.HandleDecl); ok {
			return "zx.HandleInvalid"
		}
		if !decl.IsNullable() {
			panic(fmt.Sprintf("got nil for non-nullable type: %T", decl))
		}
		return "nil"
	}
	panic(fmt.Sprintf("not implemented: %T", value))
}

func onRecord(value ir.Record, decl mixer.RecordDeclaration) string {
	var fields []string
	if decl, ok := decl.(*mixer.UnionDecl); ok && len(value.Fields) >= 1 {
		field := value.Fields[0]
		fullName := declName(decl)
		var tagValue string
		if field.Key.IsUnknown() {
			tagValue = fmt.Sprintf("%d", field.Key.UnknownOrdinal)
		} else {
			fieldName := fidlgen.ToUpperCamelCase(field.Key.Name)
			tagValue = fmt.Sprintf("%s%s", fullName, fieldName)
		}
		parts := strings.Split(string(decl.Name()), "/")
		unqualifiedName := fidlgen.ToLowerCamelCase(parts[len(parts)-1])
		fields = append(fields,
			fmt.Sprintf("I_%sTag: %s", unqualifiedName, tagValue))
	}
	_, isTable := decl.(*mixer.TableDecl)
	var unknownTableFields []ir.Field
	for _, field := range value.Fields {
		if field.Key.IsUnknown() {
			if isTable {
				unknownTableFields = append(unknownTableFields, field)
			} else {
				fields = append(fields,
					fmt.Sprintf("I_unknownData: %s", buildUnknownData(field.Value.(ir.UnknownData))))
			}
			continue
		}
		fieldName := fidlgen.ToUpperCamelCase(field.Key.Name)
		fieldRhs := visit(field.Value, decl.Field(field.Key.Name))
		fields = append(fields, fmt.Sprintf("%s: %s", fieldName, fieldRhs))
		if isTable && field.Value != nil {
			fields = append(fields, fmt.Sprintf("%sPresent: true", fieldName))
		}
	}
	if len(unknownTableFields) > 0 {
		fields = append(fields,
			fmt.Sprintf("I_unknownData: %s", buildUnknownDataMap(unknownTableFields)))
	}

	if len(fields) == 0 {
		return fmt.Sprintf("%s{}", typeLiteral(decl))
	}
	// Insert newlines so that gofmt can produce good results.
	return fmt.Sprintf("%s{\n%s,\n}", typeLiteral(decl), strings.Join(fields, ",\n"))
}

func onList(value []ir.Value, decl mixer.ListDeclaration) string {
	elemDecl := decl.Elem()
	var elements []string
	for _, item := range value {
		elements = append(elements, visit(item, elemDecl))
	}
	if len(elements) == 0 {
		return fmt.Sprintf("%s{}", typeLiteral(decl))
	}
	// Insert newlines so that gofmt can produce good results.
	return fmt.Sprintf("%s{\n%s,\n}", typeLiteral(decl), strings.Join(elements, ",\n"))
}

func typeName(decl mixer.Declaration) string {
	return typeNameHelper(decl, "*")
}

func typeLiteral(decl mixer.Declaration) string {
	return typeNameHelper(decl, "&")
}

func typeNameHelper(decl mixer.Declaration, pointerPrefix string) string {
	if !decl.IsNullable() {
		pointerPrefix = ""
	}

	switch decl := decl.(type) {
	case mixer.PrimitiveDeclaration:
		return string(decl.Subtype())
	case mixer.NamedDeclaration:
		return pointerPrefix + declName(decl)
	case *mixer.StringDecl:
		return pointerPrefix + "string"
	case *mixer.ArrayDecl:
		return fmt.Sprintf("[%d]%s", decl.Size(), typeName(decl.Elem()))
	case *mixer.VectorDecl:
		return fmt.Sprintf("%s[]%s", pointerPrefix, typeName(decl.Elem()))
	case *mixer.HandleDecl:
		switch decl.Subtype() {
		case fidlgen.HandleSubtypeNone:
			return "zx.Handle"
		case fidlgen.HandleSubtypeChannel:
			return "zx.Channel"
		case fidlgen.HandleSubtypeEvent:
			return "zx.Event"
		default:
			panic(fmt.Sprintf("Handle subtype not supported %s", decl.Subtype()))
		}
	default:
		panic(fmt.Sprintf("unhandled case %T", decl))
	}
}

func declName(decl mixer.NamedDeclaration) string {
	return identifierName(decl.Name())
}

func endpointDeclName(decl mixer.EndpointDeclaration) string {
	switch decl.(type) {
	case *mixer.ClientEndDecl:
		return fmt.Sprintf("%sWithCtxInterface", identifierName(decl.ProtocolName()))
	case *mixer.ServerEndDecl:
		return fmt.Sprintf("%sWithCtxInterfaceRequest", identifierName(decl.ProtocolName()))
	default:
		panic(fmt.Sprintf("unhandled case %T", decl))
	}
}

// TODO(fxbug.dev/39407): Move into a common library outside GIDL.
func identifierName(qualifiedName string) string {
	parts := strings.Split(qualifiedName, "/")
	library_parts := strings.Split(strings.ToLower(parts[0]), ".")
	return strings.Join([]string{library_parts[len(library_parts)-1],
		fidlgen.ToUpperCamelCase(parts[1])}, ".")
}

// Go errors are defined in third_party/go/src/syscall/zx/fidl/errors.go.
var goErrorCodeNames = map[ir.ErrorCode]string{
	ir.EnvelopeBytesExceedMessageLength:   "ErrPayloadTooSmall",
	ir.EnvelopeHandlesExceedMessageLength: "ErrTooManyHandles",
	ir.ExceededMaxOutOfLineDepth:          "ErrExceededMaxOutOfLineDepth",
	ir.IncorrectHandleType:                "ErrIncorrectHandleType",
	ir.InvalidBoolean:                     "ErrInvalidBoolValue",
	ir.InvalidEmptyStruct:                 "ErrInvalidEmptyStruct",
	ir.InvalidInlineBitInEnvelope:         "ErrInvalidInlineBitValueInEnvelope",
	ir.InvalidInlineMarkerInEnvelope:      "ErrBadInlineIndicatorEncoding",
	ir.InvalidNumBytesInEnvelope:          "ErrInvalidNumBytesInEnvelope",
	ir.InvalidNumHandlesInEnvelope:        "ErrInvalidNumHandlesInEnvelope",
	ir.InvalidPaddingByte:                 "ErrNonZeroPadding",
	ir.InvalidPresenceIndicator:           "ErrBadRefEncoding",
	ir.InvalidHandlePresenceIndicator:     "ErrBadHandleEncoding",
	ir.MissingRequiredHandleRights:        "ErrMissingRequiredHandleRights",
	ir.NonEmptyStringWithNullBody:         "ErrUnexpectedNullRef",
	ir.NonEmptyVectorWithNullBody:         "ErrUnexpectedNullRef",
	ir.NonNullableTypeWithNullValue:       "ErrUnexpectedNullRef",
	ir.NonResourceUnknownHandles:          "ErrValueTypeHandles",
	ir.StrictBitsUnknownBit:               "ErrInvalidBitsValue",
	ir.StrictEnumUnknownValue:             "ErrInvalidEnumValue",
	ir.StrictUnionUnknownField:            "ErrInvalidXUnionTag",
	ir.StringCountExceeds32BitLimit:       "ErrStringTooLong",
	ir.StringNotUtf8:                      "ErrStringNotUTF8",
	ir.StringTooLong:                      "ErrStringTooLong",
	ir.TableCountExceeds32BitLimit:        "ErrUnexpectedOrdinal",
	ir.TooFewBytes:                        "ErrPayloadTooSmall",
	ir.TooFewBytesInPrimaryObject:         "ErrPayloadTooSmall",
	ir.TooFewHandles:                      "ErrNotEnoughHandles",
	ir.TooManyBytesInMessage:              "ErrTooManyBytesInMessage",
	ir.TooManyHandlesInMessage:            "ErrTooManyHandles",
	ir.UnionFieldNotSet:                   "ErrInvalidXUnionTag",
	ir.CountExceedsLimit:                  "ErrVectorTooLong",
	ir.UnexpectedOrdinal:                  "ErrUnexpectedOrdinal",
	ir.VectorCountExceeds32BitLimit:       "ErrVectorTooLong",
}

func goErrorCode(code ir.ErrorCode) (string, error) {
	if str, ok := goErrorCodeNames[code]; ok {
		return fmt.Sprintf("fidl.%s", str), nil
	}
	return "", fmt.Errorf("no go error string defined for error code %s", code)
}
