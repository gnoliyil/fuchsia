// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// summarize is a library used to produce a FIDL API summary from the FIDL
// intermediate representation (IR) abstract syntax tree.  Please refer to the
// README.md file in this repository for usage hints.
package summarize

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"sort"

	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

type summary []Element

// SummaryFormat is a representation of a FIDL API summary.
//
// SummaryFormat implements flag.Setter for convenient use in command-line tools.
type SummaryFormat string

// SummaryFormat constants.
const (
	TextSummaryFormat SummaryFormat = "text"
	JSONSummaryFormat SummaryFormat = "json"
)

// String implements flag.Setter.
func (f *SummaryFormat) String() string {
	return string(*f)
}

// Set implements flag.Setter.
func (f *SummaryFormat) Set(value string) error {
	format := SummaryFormat(value)
	if _, err := format.formatter(); err != nil {
		return err
	}
	*f = format
	return nil
}

func (f SummaryFormat) formatter() (func(io.Writer, summary) error, error) {
	switch f {
	case TextSummaryFormat:
		return formatTextSummary, nil
	case JSONSummaryFormat:
		return formatJSONSummary, nil
	}
	return nil, fmt.Errorf("unimplemented summary format %q", string(f))
}

// GenerateSummary summarizes root and returns the serialized result in the given format.
func GenerateSummary(root fidlgen.Root, format SummaryFormat) ([]byte, error) {
	var b bytes.Buffer
	if err := WriteSummary(&b, root, format); err != nil {
		return nil, err
	}
	return b.Bytes(), nil
}

// WriteSummary summarizes root and writes the serialized result to the given Writer.
func WriteSummary(w io.Writer, root fidlgen.Root, format SummaryFormat) error {
	s := summarize(root)
	f, err := format.formatter()
	if err != nil {
		return err
	}
	return f(w, s)
}

func formatTextSummary(w io.Writer, s summary) error {
	for _, e := range s {
		fmt.Fprintf(w, "%v\n", e)
	}
	return nil
}

func formatJSONSummary(w io.Writer, s summary) error {
	e := json.NewEncoder(w)
	// 4-level indent is chosen to match `fx format-code`.
	e.SetIndent("", "    ")
	e.SetEscapeHTML(false)
	return e.Encode(serialize([]Element(s)))
}

// Element describes a single platform surface element.  Use Summarize to
// convert a FIDL AST into Elements.
type Element interface {
	// Stringer produces a string representation of this Element.
	fmt.Stringer
	// Member returns true if the Element is a member of something.
	Member() bool
	// Name returns the fully-qualified name of this Element.  For example,
	// "library/protocol.Method".
	Name() Name
	// Serialize converts an Element into a serializable representation.
	Serialize() ElementStr
}

// payloadDict contains a mapping of names to their underlying payload layouts,
// represented as parameterizer interfaces.
type payloadDict = map[fidlgen.EncodedCompoundIdentifier]parameterizer

// All implementers of Element.
var _ = []Element{
	(*bits)(nil),
	(*aConst)(nil),
	(*enum)(nil),
	(*method)(nil),
	(*protocol)(nil),
	(*library)(nil),
}

type summarizer struct {
	elements elementSlice
	symbols  symbolTable
}

// addElement adds an element for summarization.
func (s *summarizer) addElement(e Element) {
	s.elements = append(s.elements, e)
}

// Elements obtains the API elements in this summarizer.
func (s *summarizer) Elements() []Element {
	// Ensure predictable ordering of the reported Elements.
	sort.Sort(s.elements)
	return s.elements
}

// addUnions adds the elements corresponding to the FIDL unions.
func (s *summarizer) addUnions(unions []fidlgen.Union) {
	for _, st := range unions {
		for _, m := range st.Members {
			if m.Reserved {
				// Disregard reserved members.
				continue
			}
			s.addElement(newMember(
				&s.symbols, st.Name, m.Name, m.Type, fidlgen.UnionDeclType, nil))
		}
		s.addElement(
			newAggregateWithStrictness(
				st.Name, st.Resourceness, fidlgen.UnionDeclType, st.Strictness))
	}
}

// addTables adds the elements corresponding to the FIDL tables.
func (s *summarizer) addTables(tables []fidlgen.Table) {
	for _, st := range tables {
		for _, m := range st.Members {
			if m.Reserved {
				// Disregard reserved members
				continue
			}
			s.addElement(newMember(&s.symbols, st.Name, m.Name, m.Type, fidlgen.TableDeclType, nil))
		}
		s.addElement(newAggregate(st.Name, st.Resourceness, fidlgen.TableDeclType))
	}
}

// addStructs adds the elements corresponding to the FIDL structs.
func (s *summarizer) addStructs(structs []fidlgen.Struct) {
	for _, st := range structs {
		for _, m := range st.Members {
			s.addElement(newMember(
				&s.symbols, st.Name, m.Name, m.Type, fidlgen.StructDeclType, m.MaybeDefaultValue))
		}
		s.addElement(newAggregate(st.Name, st.Resourceness, fidlgen.StructDeclType))
	}
}

// registerStructs registers names of all the structs in the FIDL IR.
func (s *summarizer) registerStructs(structs []fidlgen.Struct) {
	for _, st := range structs {
		st := st
		s.symbols.addStruct(st.Name, &st)
	}
}

// registerPayloads registers names of all the payload layouts in the FIDL IR.
func (s *summarizer) registerPayloads(payloads payloadDict) {
	s.symbols.addPayloads(payloads)
}

func serialize(e []Element) []ElementStr {
	var ret []ElementStr
	for _, l := range e {
		ret = append(ret, l.Serialize())
	}
	return ret
}

// processStructs takes the list of structs, and excludes all structs that are
// used as anonymous transactional message bodies, as those are explicitly
// disregarded by the summarizer. All structs used as payloads are added to the
// payload map.
func processStructs(structs []fidlgen.Struct, mtum fidlgen.MethodTypeUsageMap, payloads payloadDict) ([]fidlgen.Struct, payloadDict) {
	out := make([]fidlgen.Struct, 0)
	for _, s := range structs {
		if k, ok := mtum[s.Name]; ok {
			payloads[s.Name] = &structPayload{Struct: s}
			if s.IsAnonymous() && k != fidlgen.UsedOnlyAsPayload {
				// Structs that are only used as anonymous message bodies are not
				// included in the summary output because their arguments are flattened
				// into the method signature instead.
				continue
			}
		}
		out = append(out, s)
	}
	return out, payloads
}

// processTables takes the list of tables and adds all tables used as payloads
// to the payload map.
func processTables(tables []fidlgen.Table, mtum fidlgen.MethodTypeUsageMap, payloads payloadDict) ([]fidlgen.Table, payloadDict) {
	out := make([]fidlgen.Table, 0)
	for _, t := range tables {
		if _, ok := mtum[t.Name]; ok {
			payloads[t.Name] = &tablePayload{Table: t}
		}
		out = append(out, t)
	}
	return out, payloads
}

// processUnions takes the list of unions and adds all unions used as payloads
// to the payload map.
func processUnions(unions []fidlgen.Union, mtum fidlgen.MethodTypeUsageMap, payloads payloadDict) ([]fidlgen.Union, payloadDict) {
	out := make([]fidlgen.Union, 0)
	for _, u := range unions {
		if _, ok := mtum[u.Name]; ok {
			payloads[u.Name] = &unionPayload{Union: u}
		}
		out = append(out, u)
	}
	return out, payloads
}

// Elements returns the API elements found in the supplied AST root in a
// canonical ordering.
func summarize(root fidlgen.Root) summary {
	var s summarizer

	// Do a first pass of the protocols, creating a map of all names of types that
	// are used as a transactional message bodies or payloads to the kind of
	// usage (message body, payload, or both).
	mtum := root.MethodTypeUsageMap()

	s.registerStructs(root.Structs)
	s.registerStructs(root.ExternalStructs)
	s.registerProtocolNames(root.Protocols)

	payloads := payloadDict{}
	structs, payloads := processStructs(root.Structs, mtum, payloads)
	tables, payloads := processTables(root.Tables, mtum, payloads)
	unions, payloads := processUnions(root.Unions, mtum, payloads)
	_, payloads = processStructs(root.ExternalStructs, mtum, payloads)
	s.registerPayloads(payloads)

	s.addConsts(root.Consts)
	s.addBits(root.Bits)
	s.addEnums(root.Enums)
	s.addStructs(structs)
	s.addTables(tables)
	s.addUnions(unions)
	s.addProtocols(root.Protocols)
	s.addElement(library{r: root})

	return summary(s.Elements())
}
