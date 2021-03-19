// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package fidlgen_cpp

import (
	"testing"

	"github.com/google/go-cmp/cmp/cmpopts"

	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgentest"
)

var exampleProtocol = newProtocol(protocolInner{
	Methods: []Method{
		newMethod(methodInner{
			Name:        "OneWay",
			HasRequest:  true,
			HasResponse: false,
		}),
		newMethod(methodInner{
			Name:        "TwoWay",
			HasRequest:  true,
			HasResponse: true,
		}),
		newMethod(methodInner{
			Name:        "Event",
			HasRequest:  false,
			HasResponse: true,
		}),
	},
})

func toNames(methods []Method) []string {
	var s []string
	for _, m := range methods {
		s = append(s, m.Name)
	}
	return s
}

func TestMatchOneWayMethods(t *testing.T) {
	expectEqual(t, toNames(exampleProtocol.OneWayMethods), []string{"OneWay"})
}

func TestMatchTwoWayMethods(t *testing.T) {
	expectEqual(t, toNames(exampleProtocol.TwoWayMethods), []string{"TwoWay"})
}

func TestMatchClientMethods(t *testing.T) {
	expectEqual(t, toNames(exampleProtocol.ClientMethods), []string{"OneWay", "TwoWay"})
}

func TestMatchEvents(t *testing.T) {
	expectEqual(t, toNames(exampleProtocol.Events), []string{"Event"})
}

//
// Test allocation strategies
//

func TestWireBindingsAllocation(t *testing.T) {
	cases := []struct {
		desc          string
		fidl          string
		actualChooser func(p *Protocol) allocation
		expected      allocation
		expectedType  string
	}{
		{
			desc:          "client request inlined",
			fidl:          "protocol P { Method(array<uint8>:496 a); };",
			actualChooser: func(p *Protocol) allocation { return p.Methods[0].Request.ClientAllocation },
			expected: allocation{
				IsStack: true,
				Size:    512,
			},
			expectedType: "::fidl::internal::InlineMessageBuffer<512>",
		},
		{
			desc:          "client request boxed due to message size",
			fidl:          "protocol P { Method(array<uint8>:497 a); };",
			actualChooser: func(p *Protocol) allocation { return p.Methods[0].Request.ClientAllocation },
			expected: allocation{
				IsStack: false,
				Size:    0,
			},
			expectedType: "::fidl::internal::BoxedMessageBuffer<520>",
		},
		{
			desc: "client request inlined despite message flexibility",
			fidl: "flexible union Flexible { 1: int32 a; };" +
				"protocol P { Method(Flexible a); };",
			actualChooser: func(p *Protocol) allocation { return p.Methods[0].Request.ClientAllocation },
			expected: allocation{
				IsStack: true,
				Size:    48,
			},
			expectedType: "::fidl::internal::InlineMessageBuffer<48>",
		},
		{
			desc: "client response boxed due to message flexibility",
			fidl: "flexible union Flexible { 1: int32 a; };" +
				"protocol P { Method() -> (Flexible a); };",
			actualChooser: func(p *Protocol) allocation { return p.Methods[0].Response.ClientAllocation },
			expected: allocation{
				IsStack: false,
				Size:    0,
			},
			expectedType: "::fidl::internal::BoxedMessageBuffer<ZX_CHANNEL_MAX_MSG_BYTES>",
		},
		{
			desc:          "server response inlined",
			fidl:          "protocol P { Method() -> (array<uint8>:496 a); };",
			actualChooser: func(p *Protocol) allocation { return p.Methods[0].Response.ServerAllocation },
			expected: allocation{
				IsStack: true,
				Size:    512,
			},
			expectedType: "::fidl::internal::InlineMessageBuffer<512>",
		},
		{
			desc:          "server response boxed due to message size",
			fidl:          "protocol P { Method() -> (array<uint8>:497 a); };",
			actualChooser: func(p *Protocol) allocation { return p.Methods[0].Response.ServerAllocation },
			expected: allocation{
				IsStack: false,
				Size:    0,
			},
			expectedType: "::fidl::internal::BoxedMessageBuffer<520>",
		},
		{
			desc: "server response inlined despite message flexibility",
			fidl: "flexible union Flexible { 1: int32 a; };" +
				"protocol P { Method() -> (Flexible a); };",
			actualChooser: func(p *Protocol) allocation { return p.Methods[0].Response.ServerAllocation },
			expected: allocation{
				IsStack: true,
				Size:    48,
			},
			expectedType: "::fidl::internal::InlineMessageBuffer<48>",
		},
		{
			desc: "client sync event handling inlined",
			fidl: "protocol P {" +
				"    -> Event1(int32 a);" +
				"    -> Event2(int32 a, int32 b);" +
				"};",
			actualChooser: func(p *Protocol) allocation { return p.SyncEventAllocation },
			expected: allocation{
				IsStack: true,
				Size:    24,
			},
			expectedType: "::fidl::internal::InlineMessageBuffer<24>",
		},
		{
			desc: "client sync event handling boxed due to message size",
			fidl: "protocol P {" +
				"    -> Event1(array<uint8>:497 a);" +
				"    -> Event2(int32 a, int32 b);" +
				"};",
			actualChooser: func(p *Protocol) allocation { return p.SyncEventAllocation },
			expected: allocation{
				IsStack: false,
				Size:    0,
			},
			expectedType: "::fidl::internal::BoxedMessageBuffer<520>",
		},
		{
			desc: "client sync event handling boxed due to message flexibility",
			fidl: "table Flexible {};" +
				"protocol P {" +
				"    -> Event1(Flexible f);" +
				"    -> Event2(int32 a, int32 b);" +
				"};",
			actualChooser: func(p *Protocol) allocation { return p.SyncEventAllocation },
			expected: allocation{
				IsStack: false,
				Size:    0,
			},
			expectedType: "::fidl::internal::BoxedMessageBuffer<ZX_CHANNEL_MAX_MSG_BYTES>",
		},
		{
			desc: "client sync event handling inlined ignoring flexible two-way response",
			fidl: "table Flexible {};" +
				"protocol P {" +
				"    Method() -> (Flexible f);" +
				"    -> Event2(int32 a, int32 b);" +
				"};",
			actualChooser: func(p *Protocol) allocation { return p.SyncEventAllocation },
			expected: allocation{
				IsStack: true,
				Size:    32,
			},
			expectedType: "::fidl::internal::InlineMessageBuffer<32>",
		},
	}
	for _, ex := range cases {
		t.Run(ex.desc, func(t *testing.T) {
			root := compile(fidlgentest.EndToEndTest{T: t}.Single("library example; " + ex.fidl))
			var protocols []*Protocol
			for _, decl := range root.Decls {
				if p, ok := decl.(*Protocol); ok {
					protocols = append(protocols, p)
				}
			}
			if len(protocols) != 1 {
				t.Fatal("Must have a single protocol defined")
			}

			p := protocols[0]
			actual := ex.actualChooser(p)
			expectEqual(t, actual, ex.expected, cmpopts.IgnoreUnexported(allocation{}))
			expectEqual(t, actual.ByteBufferType(), ex.expectedType)
		})
	}
}

func TestHlMessagingProtocolAssociatedNames(t *testing.T) {
	fidl := `
library fuchsia.foobar;

// Regular protocol
protocol P {};
`
	root := compile(fidlgentest.EndToEndTest{T: t}.Single(fidl))

	messaging := root.Decls[0].(*Protocol).hlMessaging
	assertEqual(t, messaging.ProtocolMarker.String(), "::fuchsia::foobar::P")
	assertEqual(t, messaging.InterfaceAliasForStub.String(), "::fuchsia::foobar::P_Stub::P_clazz")
	assertEqual(t, messaging.Proxy.String(), "::fuchsia::foobar::P_Proxy")
	assertEqual(t, messaging.Stub.String(), "::fuchsia::foobar::P_Stub")
	assertEqual(t, messaging.EventSender.String(), "::fuchsia::foobar::P_EventSender")
	assertEqual(t, messaging.SyncInterface.String(), "::fuchsia::foobar::P_Sync")
	assertEqual(t, messaging.SyncProxy.String(), "::fuchsia::foobar::P_SyncProxy")
	assertEqual(t, messaging.RequestEncoder.String(), "::fuchsia::foobar::P_RequestEncoder")
	assertEqual(t, messaging.RequestDecoder.String(), "::fuchsia::foobar::P_RequestDecoder")
	assertEqual(t, messaging.ResponseEncoder.String(), "::fuchsia::foobar::P_ResponseEncoder")
	assertEqual(t, messaging.ResponseDecoder.String(), "::fuchsia::foobar::P_ResponseDecoder")
}
