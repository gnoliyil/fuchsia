// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package fuzzer_corpus

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"path"

	"go.fuchsia.dev/fuchsia/tools/fidl/gidl/lib/config"
	"go.fuchsia.dev/fuchsia/tools/fidl/gidl/lib/ir"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

type distributionEntry struct {
	Source      string `json:"source"`
	Destination string `json:"destination"`
}

type testCase struct {
	name        string
	objectTypes []fidlgen.ObjectType
	bytes       []byte
}

func prependHeader(body []byte) []byte {
	// Header used during fuzzing.
	// It doesn't need to be very random as the fuzzer could mutate the bytes.
	// However, it is important that it contains the V2 wire format flag and the correct magic.
	header := []byte{
		0, 0, 0, 0, // txid
		2,    // v2 at rest flag
		0, 0, // other flags
		1,                      // magic number
		0, 0, 0, 0, 0, 0, 0, 0, // ordinal
	}
	return append(header, body...)
}

func getEncoding(encodings []ir.Encoding) (ir.Encoding, bool) {
	for _, encoding := range encodings {
		// Only supported encoding wire format: v2.
		if encoding.WireFormat == ir.V2WireFormat {
			return encoding, true
		}
	}

	return ir.Encoding{}, false
}

func getHandleDispositionEncoding(encodings []ir.HandleDispositionEncoding) (ir.HandleDispositionEncoding, bool) {
	for _, encoding := range encodings {
		// Only supported encoding wire format: v2.
		if encoding.WireFormat == ir.V2WireFormat {
			return encoding, true
		}
	}

	return ir.HandleDispositionEncoding{}, false
}

func getHandleObjectTypes(handles []ir.Handle, defs []ir.HandleDef) []fidlgen.ObjectType {
	var objectTypes []fidlgen.ObjectType
	for _, h := range handles {
		objectTypes = append(objectTypes, fidlgen.ObjectTypeFromHandleSubtype(defs[h].Subtype))
	}
	return objectTypes
}

func convertEncodeSuccesses(gtcs []ir.EncodeSuccess) []testCase {
	var tcs []testCase
	for _, gtc := range gtcs {
		encoding, ok := getHandleDispositionEncoding(gtc.Encodings)
		if !ok {
			continue
		}

		objectTypes := getHandleObjectTypes(ir.GetHandlesFromHandleDispositions(encoding.HandleDispositions), gtc.HandleDefs)

		tcs = append(tcs, testCase{
			name:        fmt.Sprintf("EncodeSuccess_%s", gtc.Name),
			objectTypes: objectTypes,
			bytes:       encoding.Bytes,
		})
		tcs = append(tcs, testCase{
			name:        fmt.Sprintf("HeaderAnd_EncodeSuccess_%s", gtc.Name),
			objectTypes: objectTypes,
			bytes:       prependHeader(encoding.Bytes),
		})
	}

	return tcs
}

func convertDecodeSuccesses(gtcs []ir.DecodeSuccess) (tcs []testCase) {
	for _, gtc := range gtcs {
		encoding, ok := getEncoding(gtc.Encodings)
		if !ok {
			continue
		}

		objectTypes := getHandleObjectTypes(encoding.Handles, gtc.HandleDefs)

		tcs = append(tcs, testCase{
			name:        fmt.Sprintf("DecodeSuccess_%s", gtc.Name),
			objectTypes: objectTypes,
			bytes:       encoding.Bytes,
		})
		tcs = append(tcs, testCase{
			name:        fmt.Sprintf("HeaderAnd_DecodeSuccess_%s", gtc.Name),
			objectTypes: objectTypes,
			bytes:       prependHeader(encoding.Bytes),
		})
	}

	return tcs
}

func convertDecodeFailures(gtcs []ir.DecodeFailure) (tcs []testCase) {
	for _, gtc := range gtcs {
		encoding, ok := getEncoding(gtc.Encodings)
		if !ok {
			continue
		}

		objectTypes := getHandleObjectTypes(encoding.Handles, gtc.HandleDefs)

		tcs = append(tcs, testCase{
			name:        fmt.Sprintf("DecodeFailure_%s", gtc.Name),
			objectTypes: objectTypes,
			bytes:       encoding.Bytes,
		})
		tcs = append(tcs, testCase{
			name:        fmt.Sprintf("HeaderAnd_DecodeFailure_%s", gtc.Name),
			objectTypes: objectTypes,
			bytes:       prependHeader(encoding.Bytes),
		})

	}

	return tcs
}

func getData(tc testCase) []byte {
	var buf bytes.Buffer

	// Put handle and message data at head of fuzzer input.
	for _, objectType := range tc.objectTypes {
		binary.Write(&buf, binary.LittleEndian, uint32(objectType))
	}
	buf.Write(tc.bytes)

	// Put length-encoding at the tail of fuzzer input.
	binary.Write(&buf, binary.LittleEndian, uint64(len(tc.objectTypes)))

	return buf.Bytes()
}

func writeTestCase(hostDir string, packageDataDir string, tc testCase) (distributionEntry, error) {
	data := getData(tc)

	filePath := path.Join(hostDir, tc.name)
	err := fidlgen.WriteFileIfChanged(filePath, data)
	if err != nil {
		return distributionEntry{}, err
	}

	return distributionEntry{
		Source:      filePath,
		Destination: path.Join("data", packageDataDir, tc.name),
	}, err
}

func GenerateConformanceTests(gidl ir.All, _ fidlgen.Root, config config.GeneratorConfig) ([]byte, error) {
	if config.FuzzerCorpusHostDir == "" {
		return nil, errors.New("Must specify --fuzzer-corpus-host-dir when generating fuzzer_corpus")
	}
	if config.FuzzerCorpusPackageDataDir == "" {
		return nil, errors.New("Must specify --fuzzer-corpus-package-data-dir when generating fuzzer_corpus")
	}

	var manifest []distributionEntry

	for _, tcs := range [][]testCase{
		convertEncodeSuccesses(gidl.EncodeSuccess),
		convertDecodeSuccesses(gidl.DecodeSuccess),
		convertDecodeFailures(gidl.DecodeFailure),
	} {
		for _, tc := range tcs {
			entry, err := writeTestCase(config.FuzzerCorpusHostDir, config.FuzzerCorpusPackageDataDir, tc)
			if err != nil {
				return nil, err
			}
			manifest = append(manifest, entry)
		}
	}

	return json.Marshal(manifest)
}
