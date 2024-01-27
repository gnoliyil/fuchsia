// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package ir

import (
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

// Value represents any acceptable value used to represent a FIDL value.
// This type may wrap one of:
// - `string` for strings
// - `int64` for negative integers (of any size), bits, and enums
// - `uint64` for nonnegative integers (of any size), bits, and enums
// - `float64` or `RawFloat` for floating point numbers (of any size)
// - `bool` for booleans
// - `Handle` or `HandleWithRights` for handles
// - `Record` for structs, tables, and unions
// - `[]Value` for slices of values
// - `nil` for null values (only allowed for nullable types)
// - `UnknownData` for unknown variants of unions
type Value interface{}

// A RawFloat is an integer whose bytes specify an IEEE 754 single or double
// precision floating point number (sign bit = most significant bit). In the
// single-precision case, the value should be within uint32 range.
type RawFloat uint64

// A Handle is an index into the test's []HandleDef.
type Handle int

// A HandleWithRights is a Handle with rights information.
type HandleWithRights struct {
	Handle Handle
	Type   fidlgen.ObjectType
	Rights fidlgen.HandleRights
}

// Record represents a value for a struct, table, or union type.
type Record struct {
	// Unqualified type name.
	Name string
	// List of fields. Struct and table records can have any number of fields.
	// Union records should have one field set, or no fields set to indicate the
	// union is default initialized (not all backends support this).
	Fields []Field
}

// Field represents a field in a struct, table, or union value.
type Field struct {
	Key   FieldKey
	Value Value
}

// FieldKey designates a field in a struct, table, or union type. The key is
// either known (represented by name) or unknown (represented by ordinal).
//
// Only flexible tables and flexible unions can have unknown keys. Although
// known table/union fields (strict or flexible) have both a name and ordinal,
// FieldKey only stores the name.
type FieldKey struct {
	Name           string
	UnknownOrdinal uint64
}

// IsKnown returns true if f is a known (i.e. named) key.
func (f *FieldKey) IsKnown() bool {
	return f.Name != ""
}

// IsUnknown returns true if f is an unknown (i.e. ordinal) key.
func (f *FieldKey) IsUnknown() bool {
	return f.Name == ""
}

// UnknownData represents the raw payload of an envelope, e.g. the data
// corresponding to an unknown variant of a union
type UnknownData struct {
	Bytes   []byte
	Handles []Handle
}

// HasData returns true if u stores unknown data. For example, it returns true
// in the case of `MyUnion { 123: { bytes = [ repeat(0xab):8 ] } }` representing
// a domain object that stores unknown bytes and handles, but false in the case
// of `MyUnion { 123: null }`.
//
// TODO(fxbug.dev/85383): Fully implement RFC-0137 and then remove UnknownData
// since it should not be possible to store unknown data in any bindings.
func (u *UnknownData) HasData() bool {
	if u.Bytes == nil && u.Handles == nil {
		return false
	}
	if u.Bytes == nil {
		panic("UnknownData has handles but not bytes")
	}
	return true
}
