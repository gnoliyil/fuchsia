// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package mixer

import (
	"fmt"
	"math"

	gidlir "go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	fidl "go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

// ValueVisitor is an API that walks GIDL values.
type ValueVisitor interface {
	OnBool(value bool)
	OnInt64(value int64, typ fidl.PrimitiveSubtype)
	OnUint64(value uint64, typ fidl.PrimitiveSubtype)
	OnFloat64(value float64, typ fidl.PrimitiveSubtype)
	OnString(value string, decl *StringDecl)
	OnHandle(value gidlir.Handle, decl *HandleDecl)
	OnBits(value interface{}, decl *BitsDecl)
	OnEnum(value interface{}, decl *EnumDecl)
	OnStruct(value gidlir.Record, decl *StructDecl)
	OnTable(value gidlir.Record, decl *TableDecl)
	OnUnion(value gidlir.Record, decl *UnionDecl)
	OnArray(value []interface{}, decl *ArrayDecl)
	OnVector(value []interface{}, decl *VectorDecl)
	OnNull(decl Declaration)
}

// Visit is the entry point into visiting a value, it dispatches appropriately
// into the visitor.
func Visit(visitor ValueVisitor, value interface{}, decl Declaration) {
	switch value := value.(type) {
	case bool:
		visitor.OnBool(value)
	case int64:
		switch decl := decl.(type) {
		case *BitsDecl:
			visitor.OnBits(value, decl)
		case *EnumDecl:
			visitor.OnEnum(value, decl)
		case *IntegerDecl:
			visitor.OnInt64(value, decl.Subtype())
		default:
			panic(fmt.Sprintf("int64 value has non-integer decl: %T", decl))
		}
	case uint64:
		switch decl := decl.(type) {
		case *BitsDecl:
			visitor.OnBits(value, decl)
		case *EnumDecl:
			visitor.OnEnum(value, decl)
		case *IntegerDecl:
			visitor.OnUint64(value, decl.Subtype())
		default:
			panic(fmt.Sprintf("uint64 value has non-integer decl: %T", decl))
		}
	case float64:
		visitor.OnFloat64(value, decl.(*FloatDecl).Subtype())
	case string:
		switch decl := decl.(type) {
		case *StringDecl:
			visitor.OnString(value, decl)
		default:
			panic(fmt.Sprintf("string value has non-string decl: %T", decl))
		}
	case gidlir.Handle:
		switch decl := decl.(type) {
		case *HandleDecl:
			visitor.OnHandle(value, decl)
		default:
			panic(fmt.Sprintf("handle value has non-handle decl: %T", decl))
		}
	case gidlir.Record:
		switch decl := decl.(type) {
		case *StructDecl:
			visitor.OnStruct(value, decl)
		case *TableDecl:
			visitor.OnTable(value, decl)
		case *UnionDecl:
			visitor.OnUnion(value, decl)
		default:
			panic(fmt.Sprintf("expected %T, got %T: %v", decl, value, value))
		}
	case []interface{}:
		switch decl := decl.(type) {
		case *ArrayDecl:
			visitor.OnArray(value, decl)
		case *VectorDecl:
			visitor.OnVector(value, decl)
		default:
			panic(fmt.Sprintf("not implemented: %T", decl))
		}
	case nil:
		if !decl.IsNullable() {
			panic(fmt.Sprintf("got nil for non-nullable type: %T", decl))
		}
		visitor.OnNull(decl)
	default:
		panic(fmt.Sprintf("not implemented: %T", value))
	}
}

// Declaration is the GIDL-level concept of a FIDL type. It is more convenient
// to work with in GIDL backends than fidl.Type. It also provides logic for
// testing if a GIDL value conforms to the declaration.
type Declaration interface {
	// IsNullable returns true for nullable types. For example, it returns false
	// for string and true for string?.
	IsNullable() bool

	// conforms verifies that the value conforms to this declaration.
	conforms(value interface{}, ctx context) error
}

// context stores the external information needed to determine if a value
// conforms to a declaration.
type context struct {
	// Handle definitions in scope, used for HandleDecl conformance.
	handleDefs []gidlir.HandleDef
}

// Assert that wrappers conform to the Declaration interface.
var _ = []Declaration{
	&BoolDecl{},
	&IntegerDecl{},
	&FloatDecl{},
	&StringDecl{},
	&HandleDecl{},
	&BitsDecl{},
	&EnumDecl{},
	&StructDecl{},
	&TableDecl{},
	&UnionDecl{},
	&ArrayDecl{},
	&VectorDecl{},
}

type PrimitiveDeclaration interface {
	Declaration

	// Subtype returns the primitive subtype (bool, uint32, float64, etc.).
	Subtype() fidl.PrimitiveSubtype
}

// Assert that wrappers conform to the PrimitiveDeclaration interface.
var _ = []PrimitiveDeclaration{
	&BoolDecl{},
	&IntegerDecl{},
	&FloatDecl{},
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
	&BitsDecl{},
	&EnumDecl{},
	&StructDecl{},
	&TableDecl{},
	&UnionDecl{},
}

type RecordDeclaration interface {
	NamedDeclaration

	// IsResourceType returns true if the type is marked as a resource, meaning
	// it may contain handles.
	IsResourceType() bool

	// AllFields returns the names of all fields in the type.
	FieldNames() []string

	// Field returns the declaration for the field with the given name. It
	// returns false if no field with that name exists.
	Field(name string) (Declaration, bool)
}

// Assert that wrappers conform to the RecordDeclaration interface.
var _ = []RecordDeclaration{
	&StructDecl{},
	&TableDecl{},
	&UnionDecl{},
}

type ListDeclaration interface {
	Declaration

	// Elem returns the declaration for the list's element type.
	Elem() Declaration
}

// Assert that wrappers conform to the ListDeclaration interface.
var _ = []ListDeclaration{
	&ArrayDecl{},
	&VectorDecl{},
}

// Helper struct for implementing IsNullable on types that are never nullable.
type NeverNullable struct{}

func (NeverNullable) IsNullable() bool {
	return false
}

type BoolDecl struct {
	NeverNullable
}

func (decl *BoolDecl) Subtype() fidl.PrimitiveSubtype {
	return fidl.Bool
}

func (decl *BoolDecl) conforms(value interface{}, _ context) error {
	switch value.(type) {
	default:
		return fmt.Errorf("expecting bool, found %T (%v)", value, value)
	case bool:
		return nil
	}
}

type IntegerDecl struct {
	NeverNullable
	subtype fidl.PrimitiveSubtype
	lower   int64
	upper   uint64
}

func (decl *IntegerDecl) Subtype() fidl.PrimitiveSubtype {
	return decl.subtype
}

func (decl *IntegerDecl) conforms(value interface{}, _ context) error {
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
	subtype fidl.PrimitiveSubtype
}

func (decl *FloatDecl) Subtype() fidl.PrimitiveSubtype {
	return decl.subtype
}

func (decl *FloatDecl) conforms(value interface{}, _ context) error {
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
	case gidlir.RawFloat:
		if decl.subtype == fidl.Float32 && value > math.MaxUint32 {
			return fmt.Errorf("raw_float out of range for float32: %v", value)
		}
		return nil
	}
}

type StringDecl struct {
	bound    *int
	nullable bool
}

func (decl *StringDecl) IsNullable() bool {
	return decl.nullable
}

func (decl *StringDecl) conforms(value interface{}, _ context) error {
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
	subtype fidl.HandleSubtype
	// TODO(fxbug.dev/41920): Add a field for handle rights.
	nullable bool
}

func (decl *HandleDecl) Subtype() fidl.HandleSubtype {
	return decl.subtype
}

func (decl *HandleDecl) IsNullable() bool {
	return decl.nullable
}

func (decl *HandleDecl) conforms(value interface{}, ctx context) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting handle, found %T (%v)", value, value)
	case gidlir.Handle:
		if v := int(value); v < 0 || v >= len(ctx.handleDefs) {
			return fmt.Errorf("handle #%d out of range", value)
		}
		if decl.subtype == fidl.Handle {
			// The declaration is an untyped handle. Any subtype conforms.
			return nil
		}
		if subtype := ctx.handleDefs[value].Subtype; subtype != decl.subtype {
			return fmt.Errorf("expecting handle<%s>, found handle<%s>", decl.subtype, subtype)
		}
		return nil
	case nil:
		if decl.nullable {
			return nil
		}
		return fmt.Errorf("expecting non-null handle, found nil")
	}
}

type BitsDecl struct {
	NeverNullable
	Underlying IntegerDecl
	bitsDecl   fidl.Bits
}

func (decl *BitsDecl) Name() string {
	return string(decl.bitsDecl.Name)
}

func (decl *BitsDecl) IsFlexible() bool {
	return decl.bitsDecl.IsFlexible()
}

func (decl *BitsDecl) conforms(value interface{}, ctx context) error {
	// TODO(fxbug.dev/7847): Require a valid bits member when strict
	return decl.Underlying.conforms(value, ctx)
}

type EnumDecl struct {
	NeverNullable
	Underlying IntegerDecl
	enumDecl   fidl.Enum
}

func (decl *EnumDecl) Name() string {
	return string(decl.enumDecl.Name)
}

func (decl *EnumDecl) IsFlexible() bool {
	return decl.enumDecl.IsFlexible()
}

func (decl *EnumDecl) conforms(value interface{}, ctx context) error {
	// TODO(fxbug.dev/7847): Require a valid enum member when strict
	return decl.Underlying.conforms(value, ctx)
}

// StructDecl describes a struct declaration.
type StructDecl struct {
	structDecl fidl.Struct
	nullable   bool
	schema     Schema
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

func (decl *StructDecl) Field(name string) (Declaration, bool) {
	for _, member := range decl.structDecl.Members {
		if string(member.Name) == name {
			return decl.schema.lookupDeclByType(member.Type)
		}
	}
	return nil, false
}

// recordConforms is a helper function for implementing Declarations.conforms on
// types that expect a gidlir.Record value. It takes the kind ("struct", etc.),
// expected identifier, schema, and nullability, and returns the record or an
// error. It can also return (nil, nil) when value is nil and nullable is true.
func recordConforms(value interface{}, kind string, decl NamedDeclaration, schema Schema) (*gidlir.Record, error) {
	switch value := value.(type) {
	default:
		return nil, fmt.Errorf("expecting %s, found %T (%v)", kind, value, value)
	case gidlir.Record:
		if name := schema.qualifyName(value.Name); name != decl.Name() {
			return nil, fmt.Errorf("expecting %s %s, found %s", kind, decl.Name(), name)
		}
		return &value, nil
	case nil:
		if decl.IsNullable() {
			return nil, nil
		}
		return nil, fmt.Errorf("expecting non-null %s %s, found nil", kind, decl.Name())
	}
}

func (decl *StructDecl) conforms(value interface{}, ctx context) error {
	record, err := recordConforms(value, "struct", decl, decl.schema)
	if err != nil {
		return err
	}
	if record == nil {
		return nil
	}
	provided := make(map[string]struct{}, len(record.Fields))
	for _, field := range record.Fields {
		if field.Key.IsUnknown() {
			return fmt.Errorf("field %d: ordinal keys are invalid in structs", field.Key.UnknownOrdinal)
		}
		if fieldDecl, ok := decl.Field(field.Key.Name); !ok {
			return fmt.Errorf("field %s: unknown", field.Key.Name)
		} else if err := fieldDecl.conforms(field.Value, ctx); err != nil {
			return fmt.Errorf("field %s: %s", field.Key.Name, err)
		}
		provided[field.Key.Name] = struct{}{}
	}
	for _, member := range decl.structDecl.Members {
		// TODO(fxbug.dev/49939) Allow omitted non-nullable fields that have defaults.
		if _, ok := provided[string(member.Name)]; !ok && !member.Type.Nullable {
			panic(fmt.Sprintf("missing non-nullable field %s in struct %s",
				member.Name, decl.Name()))
		}
	}
	return nil
}

// TableDecl describes a table declaration.
type TableDecl struct {
	NeverNullable
	tableDecl fidl.Table
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

func (decl *TableDecl) Field(name string) (Declaration, bool) {
	for _, member := range decl.tableDecl.Members {
		if string(member.Name) == name {
			return decl.schema.lookupDeclByType(member.Type)
		}
	}
	return nil, false
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
			return decl.schema.lookupDeclByType(member.Type)
		}
	}
	return nil, false
}

func (decl *TableDecl) conforms(value interface{}, ctx context) error {
	record, err := recordConforms(value, "table", decl, decl.schema)
	if err != nil {
		return err
	}
	if record == nil {
		panic("tables cannot be nullable")
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
		if fieldDecl, ok := decl.Field(field.Key.Name); !ok {
			return fmt.Errorf("field %s: unknown", field.Key.Name)
		} else if err := fieldDecl.conforms(field.Value, ctx); err != nil {
			return fmt.Errorf("field %s: %s", field.Key.Name, err)
		}
	}
	return nil
}

// UnionDecl describes a union declaration.
type UnionDecl struct {
	unionDecl fidl.Union
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

func (decl *UnionDecl) Field(name string) (Declaration, bool) {
	for _, member := range decl.unionDecl.Members {
		if string(member.Name) == name {
			return decl.schema.lookupDeclByType(member.Type)
		}
	}
	return nil, false
}

func (decl *UnionDecl) fieldByOrdinal(ordinal uint64) (Declaration, bool) {
	for _, member := range decl.unionDecl.Members {
		if uint64(member.Ordinal) == ordinal {
			return decl.schema.lookupDeclByType(member.Type)
		}
	}
	return nil, false
}

func (decl *UnionDecl) conforms(value interface{}, ctx context) error {
	record, err := recordConforms(value, "union", decl, decl.schema)
	if err != nil {
		return err
	}
	if record == nil {
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
		if fieldDecl, ok := decl.Field(field.Key.Name); !ok {
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
	typ    fidl.Type
	schema Schema
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

func (decl *ArrayDecl) conforms(value interface{}, ctx context) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting array, found %T (%v)", value, value)
	case []interface{}:
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
	// The vector has type `typ`, and it contains `typ.ElementType` elements.
	typ    fidl.Type
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

func (decl *VectorDecl) conforms(value interface{}, ctx context) error {
	switch value := value.(type) {
	default:
		return fmt.Errorf("expecting vector, found %T (%v)", value, value)
	case []interface{}:
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
func BuildSchema(fidl fidl.Root) Schema {
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
	return Schema{libraryName: string(fidl.Name), types: types}
}

// ExtractDeclaration extract the top-level declaration for the provided value,
// and ensures the value conforms to the schema. It also takes a list of handle
// definitions in scope, which can be nil if there are no handles.
func (s Schema) ExtractDeclaration(value interface{}, handleDefs []gidlir.HandleDef) (*StructDecl, error) {
	decl, err := s.ExtractDeclarationUnsafe(value)
	if err != nil {
		return nil, err
	}
	if err := decl.conforms(value, context{handleDefs}); err != nil {
		return nil, fmt.Errorf("value %v failed to conform to declaration (type %T): %v", value, decl, err)
	}
	return decl, nil
}

// ExtractDeclarationUnsafe extracts the top-level declaration for the provided
// value, but does not ensure the value conforms to the schema. This is used in
// cases where conformance is too strict (e.g. failure cases).
func (s Schema) ExtractDeclarationUnsafe(value interface{}) (*StructDecl, error) {
	switch value := value.(type) {
	case gidlir.Record:
		return s.ExtractDeclarationByName(value.Name)
	default:
		return nil, fmt.Errorf("top-level message must be a struct; got %s (%T)", value, value)
	}
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
	case *fidl.Bits:
		return &BitsDecl{
			bitsDecl:   *typ,
			Underlying: *lookupDeclByPrimitive(typ.Type.PrimitiveSubtype).(*IntegerDecl),
		}, true
	case *fidl.Enum:
		return &EnumDecl{
			enumDecl:   *typ,
			Underlying: *lookupDeclByPrimitive(typ.Type).(*IntegerDecl),
		}, true
	case *fidl.Struct:
		return &StructDecl{
			structDecl: *typ,
			nullable:   nullable,
			schema:     s,
		}, true
	case *fidl.Table:
		if nullable {
			panic(fmt.Sprintf("nullable table %s is not allowed", typ.Name))
		}
		return &TableDecl{
			tableDecl: *typ,
			schema:    s,
		}, true
	case *fidl.Union:
		return &UnionDecl{
			unionDecl: *typ,
			nullable:  nullable,
			schema:    s,
		}, true
	}
	return nil, false
}

// LookupDeclByPrimitive looks up a message declaration by primitive subtype.
func lookupDeclByPrimitive(subtype fidl.PrimitiveSubtype) PrimitiveDeclaration {
	switch subtype {
	case fidl.Bool:
		return &BoolDecl{}
	case fidl.Int8:
		return &IntegerDecl{subtype: subtype, lower: math.MinInt8, upper: math.MaxInt8}
	case fidl.Int16:
		return &IntegerDecl{subtype: subtype, lower: math.MinInt16, upper: math.MaxInt16}
	case fidl.Int32:
		return &IntegerDecl{subtype: subtype, lower: math.MinInt32, upper: math.MaxInt32}
	case fidl.Int64:
		return &IntegerDecl{subtype: subtype, lower: math.MinInt64, upper: math.MaxInt64}
	case fidl.Uint8:
		return &IntegerDecl{subtype: subtype, lower: 0, upper: math.MaxUint8}
	case fidl.Uint16:
		return &IntegerDecl{subtype: subtype, lower: 0, upper: math.MaxUint16}
	case fidl.Uint32:
		return &IntegerDecl{subtype: subtype, lower: 0, upper: math.MaxUint32}
	case fidl.Uint64:
		return &IntegerDecl{subtype: subtype, lower: 0, upper: math.MaxUint64}
	case fidl.Float32:
		return &FloatDecl{subtype: subtype}
	case fidl.Float64:
		return &FloatDecl{subtype: subtype}
	default:
		panic(fmt.Sprintf("unsupported primitive subtype: %s", subtype))
	}
}

func (s Schema) lookupDeclByType(typ fidl.Type) (Declaration, bool) {
	switch typ.Kind {
	case fidl.StringType:
		return &StringDecl{
			bound:    typ.ElementCount,
			nullable: typ.Nullable,
		}, true
	case fidl.HandleType:
		return &HandleDecl{
			subtype:  typ.HandleSubtype,
			nullable: typ.Nullable,
		}, true
	case fidl.PrimitiveType:
		return lookupDeclByPrimitive(typ.PrimitiveSubtype), true
	case fidl.IdentifierType:
		return s.lookupDeclByQualifiedName(string(typ.Identifier), typ.Nullable)
	case fidl.ArrayType:
		return &ArrayDecl{schema: s, typ: typ}, true
	case fidl.VectorType:
		return &VectorDecl{schema: s, typ: typ}, true
	default:
		// TODO(fxbug.dev/36441): HandleType, RequestType
		panic("not implemented")
	}
}
