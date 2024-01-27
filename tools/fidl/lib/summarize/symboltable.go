// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package summarize

import (
	"fmt"
	"strings"

	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

type typePrinter = func(t fidlgen.Type) Type

// structPayload is a thin wrapper around the Struct type imported from fidlgen.
// It exists solely to implement the parameterizer interface over that type.
type structPayload struct {
	fidlgen.Struct
}

func (p *structPayload) Name() fidlgen.EncodedCompoundIdentifier {
	return p.Struct.Name
}

func (p *structPayload) AsParameters(tp typePrinter) string {
	members := p.Struct.Members
	var ps []string
	for _, m := range members {
		ps = append(ps, fmt.Sprintf("%v %v", tp(m.Type), m.Name))
	}
	return fmt.Sprintf("(%v)", strings.Join(ps, ","))
}

// tablePayload is a thin wrapper around the Table type imported from fidlgen.
// It exists solely to implement the parameterizer interface over that type.
type tablePayload struct {
	fidlgen.Table
}

func (p *tablePayload) Name() fidlgen.EncodedCompoundIdentifier {
	return p.Table.Name
}

func (p *tablePayload) AsParameters(_ typePrinter) string {
	return fmt.Sprintf("(%v payload)", p.Name())
}

// unionPayload is a thin wrapper around the Union type imported from fidlgen.
// It exists solely to implement the parameterizer interface over that type.
type unionPayload struct {
	fidlgen.Union
}

func (p *unionPayload) Name() fidlgen.EncodedCompoundIdentifier {
	return p.Union.Name
}

func (p *unionPayload) AsParameters(_ typePrinter) string {
	return fmt.Sprintf("(%v payload)", p.Name())
}

// parameterizer describes a FIDL type that may be rendered as a set of
// request/response parameters for a FIDL protocol method.
type parameterizer interface {
	// Name returns the name of the type in question.
	Name() fidlgen.EncodedCompoundIdentifier
	// AsParameters renders the type in question into a parameter list. This
	// rendering takes different forms depending on the layout of the underlying
	// type: structs are "flattened" into a list of their constituent elements,
	// while tables and unions are always a list pointing to the type in question,
	// always named "payload."
	AsParameters(typePrinter) string
}

// All implementers of parameterizer.
var _ = []parameterizer{
	(*structPayload)(nil),
	(*tablePayload)(nil),
	(*unionPayload)(nil),
}

// symbolTable knows how to represent a symbol's type as a string.
//
// In the RFC-0050 compatible syntax, knowledge of identifier types is required
// for this to be possible.
type symbolTable struct {
	// protocolNames contains all protocol names defined in the FIDL IR.  Used
	// for resolving server and client end references.
	protocolNames map[fidlgen.EncodedCompoundIdentifier]struct{}

	// structDecls contain all struct names from the FIDL IR.  Used for
	// resolving optional structs which have a different syntax
	// (box<Foo> instead of Foo:optional).
	structDecls map[fidlgen.EncodedCompoundIdentifier]*fidlgen.Struct

	// payloads contains all struct, table, or union layouts used in any of the
	// protocol methods in the library being summarized. Used for generating
	// parameter lists for method signatures.
	payloads payloadDict
}

// addProtocol registers that name corresponds to a FIDL protocol.
func (n *symbolTable) addProtocol(name fidlgen.EncodedCompoundIdentifier) {
	if n.protocolNames == nil {
		n.protocolNames = make(map[fidlgen.EncodedCompoundIdentifier]struct{})
	}
	n.protocolNames[name] = struct{}{}
}

// isProtocol returns true if name is a known protocol.
func (n *symbolTable) isProtocol(name fidlgen.EncodedCompoundIdentifier) bool {
	_, ok := n.protocolNames[name]
	return ok
}

// addStruct registers that `name` corresponds to a FIDL struct.
func (n *symbolTable) addStruct(name fidlgen.EncodedCompoundIdentifier, def *fidlgen.Struct) {
	if n.structDecls == nil {
		n.structDecls = make(map[fidlgen.EncodedCompoundIdentifier]*fidlgen.Struct)
	}
	n.structDecls[name] = def
}

// isStruct returns true if name is a known struct.
func (n *symbolTable) isStruct(name fidlgen.EncodedCompoundIdentifier) bool {
	_, ok := n.structDecls[name]
	return ok
}

// addPayloads registers that a map of each `name` that corresponds to a FIDL
// payload layout.
func (n *symbolTable) addPayloads(payloads payloadDict) {
	n.payloads = payloads
}

// getPayload returns the stored payload definition, if one exists.
func (n *symbolTable) getPayload(name fidlgen.EncodedCompoundIdentifier) parameterizer {
	if def, ok := n.payloads[name]; ok {
		return def
	}
	return nil
}

// fidlTypeString converts the FIDL type declaration into a string per RFC-0050.
func (n *symbolTable) fidlTypeString(t fidlgen.Type) Type {
	var ret typeString
	switch t.Kind {
	case fidlgen.PrimitiveType:
		ret.setLayout(string(t.PrimitiveSubtype))
	case fidlgen.StringType:
		ret.setLayout("string")
	case fidlgen.ArrayType:
		ret.setLayout("array")
		ret.addParam(string(n.fidlTypeString(*t.ElementType)))
	case fidlgen.VectorType:
		ret.setLayout("vector")
		ret.addParam(string(n.fidlTypeString(*t.ElementType)))
	case fidlgen.HandleType:
		if t.HandleSubtype != fidlgen.HandleSubtypeNone {
			ret.setLayout("zx/Handle")
			ret.addConstraint(strings.ToUpper(string(t.HandleSubtype)))
		} else {
			ret.setLayout("zx/Handle")
		}
		ret.addHandleRights(t.HandleRights)
	case fidlgen.IdentifierType: // E.g. struct, enum, bits, etc.
		if n.isProtocol(t.Identifier) {
			ret.setLayout("client_end")
			ret.addConstraint(string(t.Identifier))
		} else {
			ret.setLayout(string(t.Identifier))
		}
	case fidlgen.RequestType: // E.g. server end
		ret.setLayout("server_end")
		ret.addConstraint(string(t.RequestSubtype))
	default:
		ret.setLayout(fmt.Sprintf("<not_implemented:%#v>", t))
	}
	// ,N
	if t.ElementCount != nil {
		if t.Kind == fidlgen.ArrayType {
			ret.addParam(fmt.Sprintf("%d", *t.ElementCount))
		} else {
			ret.addConstraint(fmt.Sprintf("%d", *t.ElementCount))
		}
	}
	// :optional
	if t.Nullable {
		if n.isStruct(t.Identifier) {
			ret.addParam(ret.layout)
			ret.setLayout("box")
		} else {
			ret.addConstraint("optional")
		}
	}
	return Type(ret.String())
}

// typeString contains a declaration of a FIDL type, and knows how to print it
// out.
//
// See
// https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0050_syntax_revamp?hl=en
type typeString struct {
	layout      string
	params      []string
	constraints []string
}

func (t *typeString) setLayout(layout string) {
	t.layout = layout
}

func (t *typeString) addParam(param string) {
	if param == "" {
		return
	}
	t.params = append(t.params, param)
}

func (t *typeString) addConstraint(constraint string) {
	if constraint == "" {
		return
	}
	t.constraints = append(t.constraints, constraint)
}

// String writes out the typeString based on the RFC-0050 syntax rules.
func (t typeString) String() string {
	var ret []string
	if t.layout == "" {
		panic(fmt.Sprintf("layout not set: %#v", t))
	}
	ret = append(ret, t.layout)
	// <> omitted if no params.
	if len(t.params) != 0 {
		ret = append(ret, "<", strings.Join(t.params, ","), ">")
	}
	lc := len(t.constraints)
	if lc != 0 {
		ret = append(ret, ":")
		if lc > 1 {
			// "<>" omitted if one constraint or no constraints.
			ret = append(ret, "<")
		}
		ret = append(ret, strings.Join(t.constraints, ","))
		if lc > 1 {
			ret = append(ret, ">")
		}
	}
	return strings.Join(ret, "")
}

// handleRightsNames maps each individual HandleRights bit to a name.
var handleRightsNames map[fidlgen.HandleRights]string = map[fidlgen.HandleRights]string{
	fidlgen.HandleRightsNone:          "zx.NONE",
	fidlgen.HandleRightsDuplicate:     "zx.DUPLICATE",
	fidlgen.HandleRightsTransfer:      "zx.TRANSFER",
	fidlgen.HandleRightsRead:          "zx.READ",
	fidlgen.HandleRightsWrite:         "zx.WRITE",
	fidlgen.HandleRightsExecute:       "zx.EXECUTE",
	fidlgen.HandleRightsMap:           "zx.MAP",
	fidlgen.HandleRightsGetProperty:   "zx.GET_PROPERTY",
	fidlgen.HandleRightsSetProperty:   "zx.SET_PROPERTY",
	fidlgen.HandleRightsEnumerate:     "zx.ENUMERATE",
	fidlgen.HandleRightsDestroy:       "zx.DESTROY",
	fidlgen.HandleRightsSetPolicy:     "zx.SET_POLICY",
	fidlgen.HandleRightsGetPolicy:     "zx.GET_POLICY",
	fidlgen.HandleRightsSignal:        "zx.SIGNAL",
	fidlgen.HandleRightsSignalPeer:    "zx.SIGNAL_PEER",
	fidlgen.HandleRightsWait:          "zx.WAIT",
	fidlgen.HandleRightsInspect:       "zx.INSPECT",
	fidlgen.HandleRightsManageJob:     "zx.MANAGE_JOB",
	fidlgen.HandleRightsManageProcess: "zx.MANAGE_PROCESS",
	fidlgen.HandleRightsManageThread:  "zx.MANAGE_THREAD",
	fidlgen.HandleRightsApplyProfile:  "zx.APPLY_PROFILE",
}

// addHandleRights adds string representation of handle rights r into the type
// representation.
func (t *typeString) addHandleRights(r fidlgen.HandleRights) {
	var right fidlgen.HandleRights
	for right = 1; right != 0; right = right << 1 {
		if right&r != 0 {
			// While not all HandleRights bits have a name, ostensibly the ones that
			// are used do have a name, and the code below will work.  If ever there
			// is a new bit introduced without giving it a name, this will fail. But
			// since unnamed but used bit is a bug, failing is OK.  Fix by adding the
			// missing name to handleRightsNames above.
			t.addConstraint(handleRightsNames[right])
		}
	}
}
