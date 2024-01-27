// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package mixer

import (
	"fmt"
	"math"
	"strconv"

	"go.fuchsia.dev/fuchsia/tools/fidl/gidl/lib/ir"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

// Declaration is the GIDL-level concept of a FIDL type. It is more convenient
// to work with in GIDL backends than fidl.Type. It also provides logic for
// testing if a GIDL value conforms to the declaration.
type Declaration interface {
	// IsNullable returns true for nullable types. For example, it returns false
	// for string and true for string?.
	IsNullable() bool

	// IsInlinableInEnvelope indicates if the inline size of the declaration <= 4.
	IsInlinableInEnvelope() bool

	// conforms verifies that the value conforms to this declaration.
	conforms(value ir.Value, ctx context) error
}

// Assert that wrappers conform to the Declaration interface.
var _ = []Declaration{
	(*BoolDecl)(nil),
	(*IntegerDecl)(nil),
	(*FloatDecl)(nil),
	(*StringDecl)(nil),
	(*HandleDecl)(nil),
	(*ClientEndDecl)(nil),
	(*ServerEndDecl)(nil),
	(*BitsDecl)(nil),
	(*EnumDecl)(nil),
	(*StructDecl)(nil),
	(*TableDecl)(nil),
	(*UnionDecl)(nil),
	(*ArrayDecl)(nil),
	(*VectorDecl)(nil),
}

// context stores the external information needed to determine if a value
// conforms to a declaration.
type context struct {
	// Handle definitions in scope, used for HandleDecl conformance.
	handleDefs []ir.HandleDef

	// If true, handle subtypes and rights are ignored in conforms(). This
	// allows us to express EncodeSuccess tests with incorrect handles (which
	// pass because bindings do not check handles during encoding).
	ignoreHandleSubtypeAndRights bool
}

type PrimitiveDeclaration interface {
	Declaration

	// Subtype returns the primitive subtype (bool, uint32, float64, etc.).
	Subtype() fidlgen.PrimitiveSubtype
}

// Assert that wrappers conform to the PrimitiveDeclaration interface.
var _ = []PrimitiveDeclaration{
	(*BoolDecl)(nil),
	(*IntegerDecl)(nil),
	(*FloatDecl)(nil),
}

type EndpointDeclaration interface {
	Declaration

	// ProtocolName returns the fully qualified name of the protocol.
	// TODO(fxbug.dev/39407): Return common.DeclName.
	ProtocolName() string
}

var _ = []EndpointDeclaration{
	(*ClientEndDecl)(nil),
	(*ServerEndDecl)(nil),
}

type NamedDeclaration interface {
	Declaration

	// Name returns the fully qualified name of this declaration, e.g.
	// "the.library.name/TheTypeName".
	// TODO(fxbug.dev/39407): Return common.DeclName.
	Name() string
}

// Assert that wrappers conform to the NamedDeclaration interface.
var _ = []NamedDeclaration{
	(*BitsDecl)(nil),
	(*EnumDecl)(nil),
	(*StructDecl)(nil),
	(*TableDecl)(nil),
	(*UnionDecl)(nil),
}

type RecordDeclaration interface {
	NamedDeclaration

	// IsResourceType returns true if the type is marked as a resource, meaning
	// it may contain handles.
	IsResourceType() bool

	// AllFields returns the names of all fields in the type.
	FieldNames() []string

	// LookupField returns the declaration for the field with the given name. It
	// returns false if no field with that name exists.
	LookupField(name string) (Declaration, bool)

	// Field is like LookupField but panics if the field doesn't exist.
	Field(name string) Declaration
}

// Assert that wrappers conform to the RecordDeclaration interface.
var _ = []RecordDeclaration{
	(*StructDecl)(nil),
	(*TableDecl)(nil),
	(*UnionDecl)(nil),
}

type ListDeclaration interface {
	Declaration

	// Elem returns the declaration for the list's element type.
	Elem() Declaration
}

// Assert that wrappers conform to the ListDeclaration interface.
var _ = []ListDeclaration{
	(*ArrayDecl)(nil),
	(*VectorDecl)(nil),
}

// Helper struct for implementing IsNullable on types that are never nullable.
type NeverNullable struct{}

func (NeverNullable) IsNullable() bool {
	return false
}

type NeverInlinable struct{}

func (NeverInlinable) IsInlinableInEnvelope() bool {
	return false
}

type AlwaysInlinable struct{}

func (AlwaysInlinable) IsInlinableInEnvelope() bool {
	return true
}

type BoolDecl struct {
	NeverNullable
	AlwaysInlinable
}

func (decl *BoolDecl) Subtype() fidlgen.PrimitiveSubtype {
	return fidlgen.Bool
}

func (decl *BoolDecl) conforms(value ir.Value, _ context) error {
	switch value.(type) {
	default:
		return fmt.Errorf("expecting bool, found %T (%v)", value, value)
	case bool:
		return nil
	}
}

type IntegerDecl struct {
	NeverNullable
	subtype fidlgen.PrimitiveSubtype
	lower   int64
	upper   uint64
}

func (decl *IntegerDecl) IsInlinableInEnvelope() bool {
	switch decl.subtype {
	case fidlgen.Uint64, fidlgen.Int64:
		return false
	default:
		return true
	}
}

func (decl *IntegerDecl) Subtype() fidlgen.PrimitiveSubtype {
	return decl.subtype
}

func (decl *IntegerDecl) conforms(value ir.Value, _ context) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting int64 or uint64, found %T (%v)", value, value)
	case int64:
		if value < 0 {
			if value < decl.lower {
				return fmt.Errorf("%d is out of range", value)
			}
		} else {
			if decl.upper < uint64(value) {
				return fmt.Errorf("%d is out of range", value)
			}
		}
		return nil
	case uint64:
		if decl.upper < value {
			return fmt.Errorf("%d is out of range", value)
		}
		return nil
	}
}

type FloatDecl struct {
	NeverNullable
	subtype fidlgen.PrimitiveSubtype
}

func (decl *FloatDecl) IsInlinableInEnvelope() bool {
	return decl.subtype == fidlgen.Float32
}

func (decl *FloatDecl) Subtype() fidlgen.PrimitiveSubtype {
	return decl.subtype
}

func (decl *FloatDecl) conforms(value ir.Value, _ context) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting float64, found %T (%s)", value, value)
	case float64:
		if math.IsNaN(value) {
			return fmt.Errorf("must use raw_float for NaN: %v", value)
		}
		if math.IsInf(value, 0) {
			return fmt.Errorf("must use raw_float for infinity: %v", value)
		}
		return nil
	case ir.RawFloat:
		if decl.subtype == fidlgen.Float32 && value > math.MaxUint32 {
			return fmt.Errorf("raw_float out of range for float32: %v", value)
		}
		return nil
	}
}

type StringDecl struct {
	NeverInlinable
	bound    *int
	nullable bool
}

func (decl *StringDecl) IsNullable() bool {
	return decl.nullable
}

func (decl *StringDecl) conforms(value ir.Value, _ context) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting string, found %T (%v)", value, value)
	case string:
		if decl.bound == nil {
			return nil
		}
		if bound := *decl.bound; bound < len(value) {
			return fmt.Errorf(
				"string '%s' is too long, expecting %d but was %d", value,
				bound, len(value))
		}
		return nil
	case nil:
		if decl.nullable {
			return nil
		}
		return fmt.Errorf("expecting non-null string, found nil")
	}
}

type HandleDecl struct {
	AlwaysInlinable
	subtype  fidlgen.HandleSubtype
	rights   fidlgen.HandleRights
	nullable bool
}

func (decl *HandleDecl) Subtype() fidlgen.HandleSubtype {
	return decl.subtype
}

func (decl *HandleDecl) Rights() fidlgen.HandleRights {
	return decl.rights
}

func (decl *HandleDecl) IsNullable() bool {
	return decl.nullable
}

// Returns true if x contains all the rights in y, or if either one is same_rights.
func containsRights(x, y fidlgen.HandleRights) bool {
	return x == fidlgen.HandleRightsSameRights || y == fidlgen.HandleRightsSameRights || y&^x == 0
}

func (decl *HandleDecl) conforms(value ir.Value, ctx context) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting handle, found %T (%v)", value, value)
	case ir.AnyHandle:
		handle := value.GetHandle()
		if v := int(handle); v < 0 || v >= len(ctx.handleDefs) {
			return fmt.Errorf("handle #%d out of range", handle)
		}
		if ctx.ignoreHandleSubtypeAndRights {
			return nil
		}
		def := ctx.handleDefs[handle]
		if decl.subtype != fidlgen.HandleSubtypeNone && def.Subtype != decl.subtype {
			return fmt.Errorf("handle #%d subtype '%s' does not match FIDL schema subtype '%s'", value, def.Subtype, decl.subtype)
		}
		if !containsRights(def.Rights, decl.rights) {
			return fmt.Errorf("handle #%d rights 0x%08x does not contain all FIDL schema rights 0x%08x", value, def.Rights, decl.rights)
		}
		if value, ok := value.(ir.RestrictedHandle); ok {
			if value.Type != fidlgen.ObjectTypeFromHandleSubtype(def.Subtype) {
				return fmt.Errorf("restrict(...) type %d does not match handle #%d subtype '%s'", value.Type, handle, def.Subtype)
			}
			if !containsRights(def.Rights, value.Rights) {
				return fmt.Errorf("restrict(...) rights 0x%08x are not a subset of handle #%d rights 0x%08x", value.Rights, handle, def.Rights)
			}
			if decl.rights != fidlgen.HandleRightsSameRights && value.Rights != decl.rights {
				return fmt.Errorf("restrict(...) rights 0x%08x do not match FIDL schema rights 0x%08x", value.Rights, decl.rights)
			}
		}
		return nil
	case nil:
		if decl.nullable {
			return nil
		}
		return fmt.Errorf("expecting non-null handle, found nil")
	}
}

type ClientEndDecl struct {
	AlwaysInlinable
	protocolDecl fidlgen.Protocol
	nullable     bool
}

func (decl *ClientEndDecl) IsNullable() bool {
	return decl.nullable
}

func (decl *ClientEndDecl) ProtocolName() string {
	return string(decl.protocolDecl.Name)
}

func (decl *ClientEndDecl) UnderlyingHandleDecl() *HandleDecl {
	rights, ok := ir.HandleRightsByName("channel_default")
	if !ok {
		panic("channel_default rights not found")
	}
	return &HandleDecl{
		subtype:  fidlgen.HandleSubtypeChannel,
		rights:   rights,
		nullable: decl.nullable,
	}
}

func (decl *ClientEndDecl) conforms(value ir.Value, ctx context) error {
	return decl.UnderlyingHandleDecl().conforms(value, ctx)
}

type ServerEndDecl struct {
	AlwaysInlinable
	protocolDecl fidlgen.Protocol
	nullable     bool
}

func (decl *ServerEndDecl) IsNullable() bool {
	return decl.nullable
}

func (decl *ServerEndDecl) ProtocolName() string {
	return string(decl.protocolDecl.Name)
}

func (decl *ServerEndDecl) UnderlyingHandleDecl() *HandleDecl {
	rights, ok := ir.HandleRightsByName("channel_default")
	if !ok {
		panic("channel_default rights not found")
	}
	return &HandleDecl{
		subtype:  fidlgen.HandleSubtypeChannel,
		rights:   rights,
		nullable: decl.nullable,
	}
}

func (decl *ServerEndDecl) conforms(value ir.Value, ctx context) error {
	return decl.UnderlyingHandleDecl().conforms(value, ctx)
}

type BitsDecl struct {
	NeverNullable
	Underlying IntegerDecl
	bitsDecl   fidlgen.Bits
}

func (decl *BitsDecl) IsInlinableInEnvelope() bool {
	return decl.Underlying.IsInlinableInEnvelope()
}

func (decl *BitsDecl) Name() string {
	return string(decl.bitsDecl.Name)
}

func (decl *BitsDecl) IsFlexible() bool {
	return decl.bitsDecl.IsFlexible()
}

func (decl *BitsDecl) conforms(value ir.Value, ctx context) error {
	err := decl.Underlying.conforms(value, ctx)
	if err != nil {
		return err
	}
	valueNum, ok := value.(uint64)
	if !ok {
		panic(fmt.Sprintf("bits value must be uint64, got %T", value))
	}
	if decl.bitsDecl.IsStrict() {
		mask, err := strconv.ParseUint(decl.bitsDecl.Mask, 10, 64)
		if err != nil {
			panic(fmt.Sprintf("cannot parse bits mask: %s", decl.bitsDecl.Mask))
		}
		if valueNum&mask != valueNum {
			return fmt.Errorf("value %d is invalid for strict bits %s", value, decl.Name())
		}
	}
	return nil
}

type EnumDecl struct {
	NeverNullable
	Underlying IntegerDecl
	enumDecl   fidlgen.Enum
}

func (decl *EnumDecl) IsInlinableInEnvelope() bool {
	return decl.Underlying.IsInlinableInEnvelope()
}

func (decl *EnumDecl) Name() string {
	return string(decl.enumDecl.Name)
}

func (decl *EnumDecl) IsFlexible() bool {
	return decl.enumDecl.IsFlexible()
}

func (decl *EnumDecl) conforms(value ir.Value, ctx context) error {
	err := decl.Underlying.conforms(value, ctx)
	if err != nil {
		return err
	}
	if decl.enumDecl.IsStrict() {
		str := fmt.Sprintf("%d", value)
		for _, member := range decl.enumDecl.Members {
			if str == member.Value.Value {
				return nil
			}
		}
		return fmt.Errorf("value %d is invalid for strict enum %s", value, decl.Name())
	}
	return nil
}

// StructDecl describes a struct declaration.
type StructDecl struct {
	structDecl fidlgen.Struct
	nullable   bool
	schema     Schema
}

func (decl *StructDecl) IsInlinableInEnvelope() bool {
	return decl.structDecl.TypeShapeV2.InlineSize <= 4
}

func (decl *StructDecl) IsNullable() bool {
	return decl.nullable
}

func (decl *StructDecl) IsResourceType() bool {
	return decl.structDecl.IsResourceType()
}

func (decl *StructDecl) Name() string {
	return string(decl.structDecl.Name)
}

func (decl *StructDecl) FieldNames() []string {
	var names []string
	for _, member := range decl.structDecl.Members {
		names = append(names, string(member.Name))
	}
	return names
}

func (decl *StructDecl) LookupField(name string) (Declaration, bool) {
	for _, member := range decl.structDecl.Members {
		if string(member.Name) == name {
			return decl.schema.lookupDeclByType(member.Type)
		}
	}
	return nil, false
}

func (decl *StructDecl) Field(name string) Declaration {
	if fieldDecl, ok := decl.LookupField(name); ok {
		return fieldDecl
	}
	panic(fmt.Sprintf("struct '%s' has no field '%s'", decl.Name(), name))
}

// recordConforms is a helper function for implementing Declarations.conforms on
// types that expect ir.Record, ir.DecodedRecord, or nil. The kind parameter
// should be "struct", "table", or "union".
func recordConforms(value ir.Value, kind string, decl NamedDeclaration, schema Schema) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting %s, found %T (%v)", kind, value, value)
	case ir.Record:
		if name := schema.qualifyName(value.Name); name != decl.Name() {
			return fmt.Errorf("expecting %s %s, found %s", kind, decl.Name(), name)
		}
		return nil
	case ir.DecodedRecord:
		if name := schema.qualifyName(value.Type); name != decl.Name() {
			return fmt.Errorf("expecting %s %s, found %s", kind, decl.Name(), name)
		}
		if decl.IsNullable() {
			return fmt.Errorf("the decode function cannot be used for a nullable %s", kind)
		}
		return nil
	case nil:
		if decl.IsNullable() {
			return nil
		}
		return fmt.Errorf("expecting non-null %s %s, found nil", kind, decl.Name())
	}
}

func (decl *StructDecl) conforms(value ir.Value, ctx context) error {
	if err := recordConforms(value, "struct", decl, decl.schema); err != nil {
		return err
	}
	record, ok := value.(ir.Record)
	if !ok {
		return nil
	}
	provided := make(map[string]struct{}, len(record.Fields))
	for _, field := range record.Fields {
		if field.Key.IsUnknown() {
			return fmt.Errorf("field %d: ordinal keys are invalid in structs", field.Key.UnknownOrdinal)
		}
		if fieldDecl, ok := decl.LookupField(field.Key.Name); !ok {
			return fmt.Errorf("field %s: unknown", field.Key.Name)
		} else if err := fieldDecl.conforms(field.Value, ctx); err != nil {
			return fmt.Errorf("field %s: %s", field.Key.Name, err)
		}
		provided[field.Key.Name] = struct{}{}
	}
	for _, member := range decl.structDecl.Members {
		// TODO(fxbug.dev/49939) Allow omitted non-nullable fields that have defaults.
		if _, ok := provided[string(member.Name)]; !ok && !member.Type.Nullable {
			return fmt.Errorf("missing non-nullable field %s in struct %s",
				member.Name, decl.Name())
		}
	}
	return nil
}

// TableDecl describes a table declaration.
type TableDecl struct {
	NeverNullable
	NeverInlinable
	tableDecl fidlgen.Table
	schema    Schema
}

func (decl *TableDecl) IsResourceType() bool {
	return decl.tableDecl.IsResourceType()
}

func (decl *TableDecl) Name() string {
	return string(decl.tableDecl.Name)
}

func (decl *TableDecl) FieldNames() []string {
	var names []string
	for _, member := range decl.tableDecl.Members {
		if !member.Reserved {
			names = append(names, string(member.Name))
		}
	}
	return names
}

func (decl *TableDecl) LookupField(name string) (Declaration, bool) {
	for _, member := range decl.tableDecl.Members {
		if string(member.Name) == name {
			return decl.schema.lookupDeclByType(*member.Type)
		}
	}
	return nil, false
}

func (decl *TableDecl) Field(name string) Declaration {
	if fieldDecl, ok := decl.LookupField(name); ok {
		return fieldDecl
	}
	panic(fmt.Sprintf("table '%s' has no field '%s'", decl.Name(), name))
}

func (decl *TableDecl) fieldByOrdinal(ordinal uint64) (Declaration, bool) {
	for _, member := range decl.tableDecl.Members {
		if uint64(member.Ordinal) == ordinal {
			// Ignore reserved members. This means that it is valid to specify
			// an unknown value for a reserved member, since they are treated
			// the same as unknown ordinals.
			if member.Reserved {
				continue
			}
			return decl.schema.lookupDeclByType(*member.Type)
		}
	}
	return nil, false
}

func (decl *TableDecl) conforms(value ir.Value, ctx context) error {
	if err := recordConforms(value, "table", decl, decl.schema); err != nil {
		return err
	}
	record, ok := value.(ir.Record)
	if !ok {
		return nil
	}
	for _, field := range record.Fields {
		// The mixer explicitly allows using unknown keys for strict unions to
		// enable encode failure tests. Bindings that do not allow constructing
		// a strict union with an unknown variant should be denylisted from these
		// tests.
		if field.Key.IsUnknown() {
			if _, ok := decl.fieldByOrdinal(field.Key.UnknownOrdinal); ok {
				return fmt.Errorf("field name must be used rather than ordinal %d", field.Key.UnknownOrdinal)
			}
			continue
		}
		if fieldDecl, ok := decl.LookupField(field.Key.Name); !ok {
			return fmt.Errorf("field %s: unknown", field.Key.Name)
		} else if err := fieldDecl.conforms(field.Value, ctx); err != nil {
			return fmt.Errorf("field %s: %s", field.Key.Name, err)
		}
	}
	return nil
}

// UnionDecl describes a union declaration.
type UnionDecl struct {
	NeverInlinable
	unionDecl fidlgen.Union
	nullable  bool
	schema    Schema
}

func (decl *UnionDecl) IsNullable() bool {
	return decl.nullable
}

func (decl *UnionDecl) IsResourceType() bool {
	return decl.unionDecl.IsResourceType()
}

func (decl *UnionDecl) Name() string {
	return string(decl.unionDecl.Name)
}

func (decl *UnionDecl) FieldNames() []string {
	var names []string
	for _, member := range decl.unionDecl.Members {
		names = append(names, string(member.Name))
	}
	return names
}

func (decl *UnionDecl) LookupField(name string) (Declaration, bool) {
	for _, member := range decl.unionDecl.Members {
		if string(member.Name) == name {
			return decl.schema.lookupDeclByType(*member.Type)
		}
	}
	return nil, false
}

func (decl *UnionDecl) Field(name string) Declaration {
	if fieldDecl, ok := decl.LookupField(name); ok {
		return fieldDecl
	}
	panic(fmt.Sprintf("union '%s' has no field '%s'", decl.Name(), name))
}

func (decl *UnionDecl) fieldByOrdinal(ordinal uint64) (Declaration, bool) {
	for _, member := range decl.unionDecl.Members {
		if uint64(member.Ordinal) == ordinal {
			return decl.schema.lookupDeclByType(*member.Type)
		}
	}
	return nil, false
}

func (decl *UnionDecl) conforms(value ir.Value, ctx context) error {
	if err := recordConforms(value, "union", decl, decl.schema); err != nil {
		return err
	}
	record, ok := value.(ir.Record)
	if !ok {
		return nil
	}
	if num := len(record.Fields); num != 1 {
		return fmt.Errorf("must have one field, found %d", num)
	}
	for _, field := range record.Fields {
		if field.Key.IsUnknown() {
			if _, ok := decl.fieldByOrdinal(field.Key.UnknownOrdinal); ok {
				return fmt.Errorf("field name must be used rather than ordinal %d", field.Key.UnknownOrdinal)
			}
			if decl.unionDecl.IsStrict() {
				return fmt.Errorf("cannot use unknown ordinal in a strict union")
			}
			continue
		}
		if fieldDecl, ok := decl.LookupField(field.Key.Name); !ok {
			return fmt.Errorf("field %s: unknown", field.Key.Name)
		} else if err := fieldDecl.conforms(field.Value, ctx); err != nil {
			return fmt.Errorf("field %s: %s", field.Key.Name, err)
		}
	}
	return nil
}

type ArrayDecl struct {
	NeverNullable
	// The array has type `typ`, and it contains `typ.ElementType` elements.
	typ    fidlgen.Type
	schema Schema
}

func (decl *ArrayDecl) IsInlinableInEnvelope() bool {
	return decl.typ.TypeShapeV2.InlineSize <= 4
}

func (decl *ArrayDecl) Elem() Declaration {
	elemType := *decl.typ.ElementType
	elemDecl, ok := decl.schema.lookupDeclByType(elemType)
	if !ok {
		panic(fmt.Sprintf("array element type %v not found in schema", elemType))
	}
	return elemDecl
}

func (decl *ArrayDecl) Size() int {
	return *decl.typ.ElementCount
}

func (decl *ArrayDecl) conforms(value ir.Value, ctx context) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting array, found %T (%v)", value, value)
	case []ir.Value:
		if len(value) != decl.Size() {
			return fmt.Errorf("expecting %d elements, got %d", decl.Size(), len(value))
		}
		elemDecl := decl.Elem()
		for i, elem := range value {
			if err := elemDecl.conforms(elem, ctx); err != nil {
				return fmt.Errorf("[%d]: %s", i, err)
			}
		}
		return nil
	}
}

type VectorDecl struct {
	NeverInlinable
	// The vector has type `typ`, and it contains `typ.ElementType` elements.
	typ    fidlgen.Type
	schema Schema
}

func (decl *VectorDecl) IsNullable() bool {
	return decl.typ.Nullable
}

func (decl *VectorDecl) Elem() Declaration {
	elemType := *decl.typ.ElementType
	elemDecl, ok := decl.schema.lookupDeclByType(elemType)
	if !ok {
		panic(fmt.Sprintf("vector element type %v not found in schema", elemType))
	}
	return elemDecl
}

func (decl *VectorDecl) MaxSize() (int, bool) {
	if decl.typ.ElementCount != nil {
		return *decl.typ.ElementCount, true
	}
	return 0, false
}

func (decl *VectorDecl) conforms(value ir.Value, ctx context) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting vector, found %T (%v)", value, value)
	case []ir.Value:
		if maxSize, ok := decl.MaxSize(); ok && len(value) > maxSize {
			return fmt.Errorf("expecting at most %d elements, got %d", maxSize, len(value))
		}
		elemDecl := decl.Elem()
		for i, elem := range value {
			if err := elemDecl.conforms(elem, ctx); err != nil {
				return fmt.Errorf("[%d]: %s", i, err)
			}
		}
		return nil
	case nil:
		if decl.typ.Nullable {
			return nil
		}
		return fmt.Errorf("expecting non-nullable vector, got nil")
	}
}

// Schema is the GIDL-level concept of a FIDL library. It provides functions to
// lookup types and return the corresponding Declaration.
type Schema struct {
	// TODO(fxbug.dev/39407): Use common.LibraryName.
	libraryName string
	// Maps fully qualified type names to fidl data structures:
	// *fidl.Struct, *fidl.Table, or *fidl.Union.
	// TODO(fxbug.dev/39407): Use common.DeclName.
	types map[string]interface{}
}

// BuildSchema builds a Schema from a FIDL library and handle definitions.
// Note: The returned schema contains pointers into fidl.
func BuildSchema(fidl fidlgen.Root) Schema {
	total := len(fidl.Bits) + len(fidl.Enums) + len(fidl.Structs) + len(fidl.Tables) + len(fidl.Unions)
	types := make(map[string]interface{}, total)
	// These loops must use fidl.Structs[i], fidl.Tables[i], etc. rather than
	// iterating `for i, decl := ...` and using &decl, because that would store
	// the same local variable address in every entry.
	for i := range fidl.Bits {
		decl := &fidl.Bits[i]
		types[string(decl.Name)] = decl
	}
	for i := range fidl.Enums {
		decl := &fidl.Enums[i]
		types[string(decl.Name)] = decl
	}
	for i := range fidl.Structs {
		decl := &fidl.Structs[i]
		types[string(decl.Name)] = decl
	}
	for i := range fidl.Tables {
		decl := &fidl.Tables[i]
		types[string(decl.Name)] = decl
	}
	for i := range fidl.Unions {
		decl := &fidl.Unions[i]
		types[string(decl.Name)] = decl
	}
	for i := range fidl.Protocols {
		decl := &fidl.Protocols[i]
		types[string(decl.Name)] = decl
	}
	return Schema{libraryName: string(fidl.Name), types: types}
}

// ExtractDeclaration extract the top-level declaration for the provided value,
// and ensures the value conforms to the schema. It also takes a list of handle
// definitions in scope, which can be nil if there are no handles.
func (s Schema) ExtractDeclaration(value ir.RecordLike, handleDefs []ir.HandleDef) (*StructDecl, error) {
	decl, err := s.ExtractDeclarationUnsafe(value)
	if err != nil {
		return nil, err
	}
	if err := decl.conforms(value, context{handleDefs: handleDefs}); err != nil {
		return nil, fmt.Errorf("value %v failed to conform to declaration (type %T): %v", value, decl, err)
	}
	return decl, nil
}

// ExtractDeclarationEncodeSuccess extract the top-level declaration for the
// provided value, and ensures the value conforms to the schema based on the
// rules for EncodeSuccess. It also takes a list of handle definitions in
// scope, which can be nil if there are no handles.
func (s Schema) ExtractDeclarationEncodeSuccess(value ir.RecordLike, handleDefs []ir.HandleDef) (*StructDecl, error) {
	decl, err := s.ExtractDeclarationUnsafe(value)
	if err != nil {
		return nil, err
	}
	if err := decl.conforms(value, context{handleDefs: handleDefs, ignoreHandleSubtypeAndRights: true}); err != nil {
		return nil, fmt.Errorf("value %v failed to conform to declaration (type %T): %v", value, decl, err)
	}
	return decl, nil
}

// ExtractDeclarationUnsafe extracts the top-level declaration for the provided
// value, but does not ensure the value conforms to the schema. This is used in
// cases where conformance is too strict (e.g. failure cases).
func (s Schema) ExtractDeclarationUnsafe(value ir.RecordLike) (*StructDecl, error) {
	return s.ExtractDeclarationByName(value.TypeName())
}

// ExtractDeclarationByName extracts the top-level declaration for the given
// unqualified type name. This is used in cases where only the type name is
// provided in the test (e.g. decoding-only tests).
func (s Schema) ExtractDeclarationByName(unqualifiedName string) (*StructDecl, error) {
	decl, ok := s.lookupDeclByName(unqualifiedName, false)
	if !ok {
		return nil, fmt.Errorf("unknown declaration %s", unqualifiedName)
	}
	structDecl, ok := decl.(*StructDecl)
	if !ok {
		return nil, fmt.Errorf("top-level message must be a struct; got %s (%T)",
			unqualifiedName, decl)
	}

	return structDecl, nil
}

func (s Schema) lookupDeclByName(unqualifiedName string, nullable bool) (Declaration, bool) {
	return s.lookupDeclByQualifiedName(s.qualifyName(unqualifiedName), nullable)
}

// TODO(fxbug.dev/39407): Take common.MemberName, return common.DeclName.
func (s Schema) qualifyName(unqualifiedName string) string {
	return fmt.Sprintf("%s/%s", s.libraryName, unqualifiedName)
}

// TODO(fxbug.dev/39407): Take common.DeclName.
func (s Schema) lookupDeclByQualifiedName(name string, nullable bool) (Declaration, bool) {
	typ, ok := s.types[name]
	if !ok {
		return nil, false
	}
	switch typ := typ.(type) {
	case *fidlgen.Bits:
		return &BitsDecl{
			bitsDecl:   *typ,
			Underlying: *lookupDeclByPrimitive(typ.Type.PrimitiveSubtype).(*IntegerDecl),
		}, true
	case *fidlgen.Enum:
		return &EnumDecl{
			enumDecl:   *typ,
			Underlying: *lookupDeclByPrimitive(typ.Type).(*IntegerDecl),
		}, true
	case *fidlgen.Struct:
		return &StructDecl{
			structDecl: *typ,
			nullable:   nullable,
			schema:     s,
		}, true
	case *fidlgen.Table:
		if nullable {
			panic(fmt.Sprintf("nullable table %s is not allowed", typ.Name))
		}
		return &TableDecl{
			tableDecl: *typ,
			schema:    s,
		}, true
	case *fidlgen.Union:
		return &UnionDecl{
			unionDecl: *typ,
			nullable:  nullable,
			schema:    s,
		}, true
	case *fidlgen.Protocol:
		return &ClientEndDecl{
			protocolDecl: *typ,
			nullable:     nullable,
		}, true
	}
	return nil, false
}

// LookupDeclByPrimitive looks up a message declaration by primitive subtype.
func lookupDeclByPrimitive(subtype fidlgen.PrimitiveSubtype) PrimitiveDeclaration {
	switch subtype {
	case fidlgen.Bool:
		return &BoolDecl{}
	case fidlgen.Int8:
		return &IntegerDecl{subtype: subtype, lower: math.MinInt8, upper: math.MaxInt8}
	case fidlgen.Int16:
		return &IntegerDecl{subtype: subtype, lower: math.MinInt16, upper: math.MaxInt16}
	case fidlgen.Int32:
		return &IntegerDecl{subtype: subtype, lower: math.MinInt32, upper: math.MaxInt32}
	case fidlgen.Int64:
		return &IntegerDecl{subtype: subtype, lower: math.MinInt64, upper: math.MaxInt64}
	case fidlgen.Uint8:
		return &IntegerDecl{subtype: subtype, lower: 0, upper: math.MaxUint8}
	case fidlgen.Uint16:
		return &IntegerDecl{subtype: subtype, lower: 0, upper: math.MaxUint16}
	case fidlgen.Uint32:
		return &IntegerDecl{subtype: subtype, lower: 0, upper: math.MaxUint32}
	case fidlgen.Uint64:
		return &IntegerDecl{subtype: subtype, lower: 0, upper: math.MaxUint64}
	case fidlgen.Float32:
		return &FloatDecl{subtype: subtype}
	case fidlgen.Float64:
		return &FloatDecl{subtype: subtype}
	default:
		panic(fmt.Sprintf("unsupported primitive subtype: %s", subtype))
	}
}

func (s Schema) lookupDeclByType(typ fidlgen.Type) (Declaration, bool) {
	switch typ.Kind {
	case fidlgen.StringType:
		return &StringDecl{
			bound:    typ.ElementCount,
			nullable: typ.Nullable,
		}, true
	case fidlgen.HandleType:
		return &HandleDecl{
			subtype:  typ.HandleSubtype,
			rights:   typ.HandleRights,
			nullable: typ.Nullable,
		}, true
	case fidlgen.PrimitiveType:
		return lookupDeclByPrimitive(typ.PrimitiveSubtype), true
	case fidlgen.IdentifierType:
		return s.lookupDeclByQualifiedName(string(typ.Identifier), typ.Nullable)
	case fidlgen.ArrayType:
		return &ArrayDecl{schema: s, typ: typ}, true
	case fidlgen.VectorType:
		return &VectorDecl{schema: s, typ: typ}, true
	case fidlgen.RequestType:
		fidlType := s.types[string(typ.RequestSubtype)]
		protocolDecl, ok := fidlType.(*fidlgen.Protocol)
		if !ok {
			panic(fmt.Sprintf("malformed FIDL schema: %+v refers to a %+v but we expect a protocol", typ.RequestSubtype, fidlType))
		}
		return &ServerEndDecl{protocolDecl: *protocolDecl, nullable: typ.Nullable}, true
	default:
		panic("not implemented")
	}
}
