// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package c

import (
	"bytes"
	_ "embed"
	"fmt"
	"text/template"

	"go.fuchsia.dev/fuchsia/tools/fidl/gidl/config"
	libhlcpp "go.fuchsia.dev/fuchsia/tools/fidl/gidl/hlcpp"
	"go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	libllcpp "go.fuchsia.dev/fuchsia/tools/fidl/gidl/llcpp/lib"
	"go.fuchsia.dev/fuchsia/tools/fidl/gidl/mixer"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

var (
	//go:embed conformance.tmpl
	conformanceTmplText string

	conformanceTmpl = template.Must(template.New("conformanceTmpl").Parse(conformanceTmplText))
)

type conformanceTmplInput struct {
	EncodeSuccessCases []encodeSuccessCase
	DecodeSuccessCases []decodeSuccessCase
	DecodeFailureCases []decodeFailureCase
}

type encodeSuccessCase struct {
	Name, WireFormatVersion, HandleDefs, ValueBuild, ValueVar, Bytes, Handles string
	FuchsiaOnly, CheckHandleRights                                            bool
}

type decodeSuccessCase struct {
	Name, HandleDefs, HandleKoidVectorName, ValueBuild, ValueVar, ValueType string
	Equality                                                                libllcpp.EqualityCheck
	Bytes, Handles, WireFormatVersion                                       string
	FuchsiaOnly                                                             bool
}

type decodeFailureCase struct {
	Name, HandleDefs, ValueType, Bytes, Handles, ErrorCode, WireFormatVersion string
	FuchsiaOnly                                                               bool
}

// Generate generates C tests.
func GenerateConformanceTests(gidl ir.All, fidl fidlgen.Root, config config.GeneratorConfig) ([]byte, error) {
	schema := mixer.BuildSchema(fidl)
	encodeSuccessCases, err := encodeSuccessCases(gidl.EncodeSuccess, schema)
	if err != nil {
		return nil, err
	}
	decodeSuccessCases, err := decodeSuccessCases(gidl.DecodeSuccess, schema)
	if err != nil {
		return nil, err
	}
	decodeFailureCases, err := decodeFailureCases(gidl.DecodeFailure, schema)
	if err != nil {
		return nil, err
	}
	var buf bytes.Buffer
	err = conformanceTmpl.Execute(&buf, conformanceTmplInput{
		EncodeSuccessCases: encodeSuccessCases,
		DecodeSuccessCases: decodeSuccessCases,
		DecodeFailureCases: decodeFailureCases,
	})
	return buf.Bytes(), err
}

func encodeSuccessCases(gidlEncodeSuccesses []ir.EncodeSuccess, schema mixer.Schema) ([]encodeSuccessCase, error) {
	var encodeSuccessCases []encodeSuccessCase
	for _, encodeSuccess := range gidlEncodeSuccesses {
		decl, err := schema.ExtractDeclarationEncodeSuccess(encodeSuccess.Value, encodeSuccess.HandleDefs)
		if err != nil {
			return nil, fmt.Errorf("encode success %s: %s", encodeSuccess.Name, err)
		}
		if containsUnionOrTable(decl) {
			continue
		}
		handleDefs := libhlcpp.BuildHandleDefs(encodeSuccess.HandleDefs)
		valueBuild, valueVar := libllcpp.BuildValueAllocator("allocator", encodeSuccess.Value, decl, libllcpp.HandleReprRaw)
		fuchsiaOnly := decl.IsResourceType() || len(encodeSuccess.HandleDefs) > 0
		for _, encoding := range encodeSuccess.Encodings {
			if !wireFormatSupported(encoding.WireFormat) {
				continue
			}
			encodeSuccessCases = append(encodeSuccessCases, encodeSuccessCase{
				Name:              testCaseName(encodeSuccess.Name, encoding.WireFormat),
				WireFormatVersion: wireFormatName(encoding.WireFormat),
				HandleDefs:        handleDefs,
				ValueBuild:        valueBuild,
				ValueVar:          valueVar,
				Bytes:             libhlcpp.BuildBytes(encoding.Bytes),
				Handles:           libhlcpp.BuildRawHandleDispositions(encoding.HandleDispositions),
				FuchsiaOnly:       fuchsiaOnly,
				CheckHandleRights: encodeSuccess.CheckHandleRights,
			})
		}
	}
	return encodeSuccessCases, nil
}

func decodeSuccessCases(gidlDecodeSuccesses []ir.DecodeSuccess, schema mixer.Schema) ([]decodeSuccessCase, error) {
	var decodeSuccessCases []decodeSuccessCase
	for _, decodeSuccess := range gidlDecodeSuccesses {
		decl, err := schema.ExtractDeclaration(decodeSuccess.Value, decodeSuccess.HandleDefs)
		if err != nil {
			return nil, fmt.Errorf("decode success %s: %s", decodeSuccess.Name, err)
		}
		if containsUnionOrTable(decl) {
			continue
		}
		handleDefs := libhlcpp.BuildHandleInfoDefs(decodeSuccess.HandleDefs)
		valueBuild, valueVar := libllcpp.BuildValueAllocator("allocator", decodeSuccess.Value, decl, libllcpp.HandleReprInfo)
		equalityInputVar := "actual"
		handleKoidVectorName := "handle_koids"
		equality := libllcpp.BuildEqualityCheck(equalityInputVar, decodeSuccess.Value, decl, handleKoidVectorName)
		fuchsiaOnly := decl.IsResourceType() || len(decodeSuccess.HandleDefs) > 0
		for _, encoding := range decodeSuccess.Encodings {
			if !wireFormatSupported(encoding.WireFormat) {
				continue
			}
			decodeSuccessCases = append(decodeSuccessCases, decodeSuccessCase{
				Name:                 testCaseName(decodeSuccess.Name, encoding.WireFormat),
				HandleDefs:           handleDefs,
				HandleKoidVectorName: handleKoidVectorName,
				ValueBuild:           valueBuild,
				ValueVar:             valueVar,
				ValueType:            libllcpp.ConformanceType(ir.TypeFromValue(decodeSuccess.Value)),
				Equality:             equality,
				Bytes:                libhlcpp.BuildBytes(encoding.Bytes),
				Handles:              libhlcpp.BuildRawHandleInfos(encoding.Handles),
				FuchsiaOnly:          fuchsiaOnly,
				WireFormatVersion:    wireFormatName(encoding.WireFormat),
			})
		}
	}
	return decodeSuccessCases, nil
}

func decodeFailureCases(gidlDecodeFailurees []ir.DecodeFailure, schema mixer.Schema) ([]decodeFailureCase, error) {
	var decodeFailureCases []decodeFailureCase
	for _, decodeFailure := range gidlDecodeFailurees {
		decl, err := schema.ExtractDeclarationByName(decodeFailure.Type)
		if err != nil {
			return nil, fmt.Errorf("decode failure %s: %s", decodeFailure.Name, err)
		}
		if containsUnionOrTable(decl) {
			continue
		}
		handleDefs := libhlcpp.BuildHandleInfoDefs(decodeFailure.HandleDefs)
		valueType := libllcpp.ConformanceType(decodeFailure.Type)
		errorCode := cErrorCode(decodeFailure.Err)
		fuchsiaOnly := decl.IsResourceType() || len(decodeFailure.HandleDefs) > 0
		for _, encoding := range decodeFailure.Encodings {
			if !wireFormatSupported(encoding.WireFormat) {
				continue
			}
			decodeFailureCases = append(decodeFailureCases, decodeFailureCase{
				Name:              testCaseName(decodeFailure.Name, encoding.WireFormat),
				HandleDefs:        handleDefs,
				ValueType:         valueType,
				Bytes:             libhlcpp.BuildBytes(encoding.Bytes),
				Handles:           libhlcpp.BuildRawHandleInfos(encoding.Handles),
				ErrorCode:         errorCode,
				FuchsiaOnly:       fuchsiaOnly,
				WireFormatVersion: wireFormatName(encoding.WireFormat),
			})
		}
	}
	return decodeFailureCases, nil
}

func wireFormatSupported(wireFormat ir.WireFormat) bool {
	return wireFormat == ir.V2WireFormat
}

func testCaseName(baseName string, wireFormat ir.WireFormat) string {
	return fmt.Sprintf("%s_%s", baseName, fidlgen.ToUpperCamelCase(wireFormat.String()))
}

func wireFormatName(wireFormat ir.WireFormat) string {
	return fmt.Sprintf("FIDL_WIRE_FORMAT_VERSION_%s", fidlgen.ToUpperCamelCase(wireFormat.String()))
}

func containsUnionOrTable(decl mixer.Declaration) bool {
	return containsUnionOrTableInternal(decl, 0)
}

func containsUnionOrTableInternal(decl mixer.Declaration, depth int) bool {
	if depth > 32 {
		return false
	}
	switch decl := decl.(type) {
	case *mixer.TableDecl, *mixer.UnionDecl:
		return true
	case *mixer.StructDecl:
		for _, fieldName := range decl.FieldNames() {
			if containsUnionOrTableInternal(decl.Field(fieldName), depth+1) {
				return true
			}
		}
		return false
	case mixer.ListDeclaration:
		return containsUnionOrTableInternal(decl.Elem(), depth+1)
	default:
		return false
	}
}

func cErrorCode(code ir.ErrorCode) string {
	if code == ir.TooFewBytesInPrimaryObject {
		return "ZX_ERR_BUFFER_TOO_SMALL"
	}
	// TODO(fxbug.dev/35381) Implement different codes for different FIDL error cases.
	return "ZX_ERR_INVALID_ARGS"
}
