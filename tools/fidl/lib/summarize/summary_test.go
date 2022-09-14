// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package summarize

import (
	"strings"
	"testing"

	"github.com/google/go-cmp/cmp"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgentest"
)

var (
	// zxLibrary is a shortened version of zx_common.fidl, for tests.
	zxLibrary = `
library zx;
type obj_type = enum : uint32 {
  CHANNEL = 4;
};
resource_definition handle : uint32 {
  properties {
    subtype obj_type;
  };
};
`

	// l2Library is a sample dependency taken in by some tests.
	l2Library = `
library l2;
type T = struct {};
type UnaryArg = struct {
    num int32;
};
protocol Inverter {
    Invert(UnaryArg) -> (UnaryArg);
};
`
)

type summaryTestCase struct {
	name     string
	fidl     string
	dep      string
	expected string
}

func TestJSONSummaryFormat(t *testing.T) {
	tests := []summaryTestCase{
		{
			name: "library only",
			fidl: `library l;`,
			expected: `[
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "primitives 1",
			fidl: `
library l;
const OFFSET int8 = -33;
const ENABLED_FLAG bool = true;
`,
			expected: `[
    {
        "declaration": "bool",
        "kind": "const",
        "name": "l/ENABLED_FLAG",
        "value": "true"
    },
    {
        "declaration": "int8",
        "kind": "const",
        "name": "l/OFFSET",
        "value": "-33"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			// Same as above, except reordered.
			name: "primitives 2",
			fidl: `
library l;
const ENABLED_FLAG bool = true;
const OFFSET int8 = -33;
`,
			expected: `[
    {
        "declaration": "bool",
        "kind": "const",
        "name": "l/ENABLED_FLAG",
        "value": "true"
    },
    {
        "declaration": "int8",
        "kind": "const",
        "name": "l/OFFSET",
        "value": "-33"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "primitives 3",
			fidl: `
library l;
const ANSWER uint16 = 42;
const ANSWER_IN_BINARY uint16 = 0b101010;
`,
			expected: `[
    {
        "declaration": "uint16",
        "kind": "const",
        "name": "l/ANSWER",
        "value": "42"
    },
    {
        "declaration": "uint16",
        "kind": "const",
        "name": "l/ANSWER_IN_BINARY",
        "value": "42"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "primitives 4",
			fidl: `
library l;
const ENABLED_FLAG bool = true;
const OFFSET int8 = -33;
const ANSWER uint16 = 42;
const ANSWER_IN_BINARY uint16 = 0b101010;
const POPULATION_USA_2018 uint32 = 330000000;
const DIAMOND uint64 = 0x183c7effff7e3c18;
const FUCHSIA uint64 = 4054509061583223046;
const USERNAME string = "squeenze";
const MIN_TEMP float32 = -273.15;
const CONVERSION_FACTOR float64 = 1.41421358;
`,
			expected: `[
    {
        "declaration": "uint16",
        "kind": "const",
        "name": "l/ANSWER",
        "value": "42"
    },
    {
        "declaration": "uint16",
        "kind": "const",
        "name": "l/ANSWER_IN_BINARY",
        "value": "42"
    },
    {
        "declaration": "float64",
        "kind": "const",
        "name": "l/CONVERSION_FACTOR",
        "value": "1.41421"
    },
    {
        "declaration": "uint64",
        "kind": "const",
        "name": "l/DIAMOND",
        "value": "1746410393481133080"
    },
    {
        "declaration": "bool",
        "kind": "const",
        "name": "l/ENABLED_FLAG",
        "value": "true"
    },
    {
        "declaration": "uint64",
        "kind": "const",
        "name": "l/FUCHSIA",
        "value": "4054509061583223046"
    },
    {
        "declaration": "float32",
        "kind": "const",
        "name": "l/MIN_TEMP",
        "value": "-273.15"
    },
    {
        "declaration": "int8",
        "kind": "const",
        "name": "l/OFFSET",
        "value": "-33"
    },
    {
        "declaration": "uint32",
        "kind": "const",
        "name": "l/POPULATION_USA_2018",
        "value": "330000000"
    },
    {
        "declaration": "string",
        "kind": "const",
        "name": "l/USERNAME",
        "value": "squeenze"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "primitives 5, binary operator",
			fidl: `
library l;
const FOO uint8 = 1;
const BAR uint8 = 2;
const BAZ uint8 = FOO | BAR;
`,
			expected: `[
    {
        "declaration": "uint8",
        "kind": "const",
        "name": "l/BAR",
        "value": "2"
    },
    {
        "declaration": "uint8",
        "kind": "const",
        "name": "l/BAZ",
        "value": "3"
    },
    {
        "declaration": "uint8",
        "kind": "const",
        "name": "l/FOO",
        "value": "1"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "bits",
			fidl: `
library l;
type Bits1 = strict bits {
    BIT1 = 0x01;
    BIT2 = 0x02;
};
`,
			expected: `[
    {
        "kind": "bits/member",
        "name": "l/Bits1.BIT1",
        "value": "1"
    },
    {
        "kind": "bits/member",
        "name": "l/Bits1.BIT2",
        "value": "2"
    },
    {
        "declaration": "uint32",
        "kind": "bits",
        "name": "l/Bits1",
        "strictness": "strict"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "bits 2",
			fidl: `
library l;
type Bits1 = strict bits {
    BIT1 = 0x01;
    BIT2 = 0x02;
};
type Bits2 = strict bits {
    BIT1 = 0x01;
    BIT2 = 0x02;
};
`,
			expected: `[
    {
        "kind": "bits/member",
        "name": "l/Bits1.BIT1",
        "value": "1"
    },
    {
        "kind": "bits/member",
        "name": "l/Bits1.BIT2",
        "value": "2"
    },
    {
        "declaration": "uint32",
        "kind": "bits",
        "name": "l/Bits1",
        "strictness": "strict"
    },
    {
        "kind": "bits/member",
        "name": "l/Bits2.BIT1",
        "value": "1"
    },
    {
        "kind": "bits/member",
        "name": "l/Bits2.BIT2",
        "value": "2"
    },
    {
        "declaration": "uint32",
        "kind": "bits",
        "name": "l/Bits2",
        "strictness": "strict"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "bits 3",
			fidl: `
library l;
type Bits = flexible bits : uint8 {
  BIT1 = 0x01;
  BIT2 = 0x02;
};
`,
			expected: `[
    {
        "kind": "bits/member",
        "name": "l/Bits.BIT1",
        "value": "1"
    },
    {
        "kind": "bits/member",
        "name": "l/Bits.BIT2",
        "value": "2"
    },
    {
        "declaration": "uint8",
        "kind": "bits",
        "name": "l/Bits",
        "strictness": "flexible"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "enums",
			fidl: `
library l;
type Beverage = flexible enum : uint8 {
    WATER = 0;
    COFFEE = 1;
    TEA = 2;
    WHISKEY = 3;
};

// Underlying type is assumed to be uint32.
type Vessel = strict enum {
    CUP = 0;
    BOWL = 1;
    TUREEN = 2;
    JUG = 3;
};
`,
			expected: `[
    {
        "kind": "enum/member",
        "name": "l/Beverage.COFFEE",
        "value": "1"
    },
    {
        "kind": "enum/member",
        "name": "l/Beverage.TEA",
        "value": "2"
    },
    {
        "kind": "enum/member",
        "name": "l/Beverage.WATER",
        "value": "0"
    },
    {
        "kind": "enum/member",
        "name": "l/Beverage.WHISKEY",
        "value": "3"
    },
    {
        "declaration": "uint8",
        "kind": "enum",
        "name": "l/Beverage",
        "strictness": "flexible"
    },
    {
        "kind": "enum/member",
        "name": "l/Vessel.BOWL",
        "value": "1"
    },
    {
        "kind": "enum/member",
        "name": "l/Vessel.CUP",
        "value": "0"
    },
    {
        "kind": "enum/member",
        "name": "l/Vessel.JUG",
        "value": "3"
    },
    {
        "kind": "enum/member",
        "name": "l/Vessel.TUREEN",
        "value": "2"
    },
    {
        "declaration": "uint32",
        "kind": "enum",
        "name": "l/Vessel",
        "strictness": "strict"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "struct as precondition for arrays",
			fidl: `
library l;
type S = struct {
  x float32;
};
`,
			expected: `[
    {
        "declaration": "float32",
        "kind": "struct/member",
        "name": "l/S.x"
    },
    {
        "kind": "struct",
        "name": "l/S"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "struct with an element with default value",
			fidl: `
library l;
const VALUE string = "booyah!";
type S = struct {
  @allow_deprecated_struct_defaults
  x float32 = 0.314159;
  @allow_deprecated_struct_defaults
  foo string = "huzzah";
  @allow_deprecated_struct_defaults
  bar bool = true;
  @allow_deprecated_struct_defaults
  baz string = VALUE;
};
`,
			expected: `[
    {
        "declaration": "bool",
        "kind": "struct/member",
        "name": "l/S.bar",
        "value": "true"
    },
    {
        "declaration": "string",
        "kind": "struct/member",
        "name": "l/S.baz",
        "value": "booyah!"
    },
    {
        "declaration": "string",
        "kind": "struct/member",
        "name": "l/S.foo",
        "value": "huzzah"
    },
    {
        "declaration": "float32",
        "kind": "struct/member",
        "name": "l/S.x",
        "value": "0.314159"
    },
    {
        "kind": "struct",
        "name": "l/S"
    },
    {
        "declaration": "string",
        "kind": "const",
        "name": "l/VALUE",
        "value": "booyah!"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "arrays",
			fidl: `
library l;
type Arrays = struct {
    form array<float32, 16>;
    matrix array<array<string, 4>, 10>;
};
`,
			expected: `[
    {
        "declaration": "array<float32,16>",
        "kind": "struct/member",
        "name": "l/Arrays.form"
    },
    {
        "declaration": "array<array<string,4>,10>",
        "kind": "struct/member",
        "name": "l/Arrays.matrix"
    },
    {
        "kind": "struct",
        "name": "l/Arrays"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "strings",
			fidl: `
library l;
type Document = struct {
    title string:40;
    description string:optional;
};
`,
			expected: `[
    {
        "declaration": "string:optional",
        "kind": "struct/member",
        "name": "l/Document.description"
    },
    {
        "declaration": "string:40",
        "kind": "struct/member",
        "name": "l/Document.title"
    },
    {
        "kind": "struct",
        "name": "l/Document"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "vectors",
			fidl: `
library l;
type Vectors = struct {
    params vector<int32>:10;
    blob vector<uint8>;
    nullable_vector_of_strings vector<string>:<24, optional>;
    vector_of_nullable_strings vector<string:optional>;
    complex vector<vector<array<float32, 16>>>;
};
`,
			expected: `[
    {
        "declaration": "vector<uint8>",
        "kind": "struct/member",
        "name": "l/Vectors.blob"
    },
    {
        "declaration": "vector<vector<array<float32,16>>>",
        "kind": "struct/member",
        "name": "l/Vectors.complex"
    },
    {
        "declaration": "vector<string>:<24,optional>",
        "kind": "struct/member",
        "name": "l/Vectors.nullable_vector_of_strings"
    },
    {
        "declaration": "vector<int32>:10",
        "kind": "struct/member",
        "name": "l/Vectors.params"
    },
    {
        "declaration": "vector<string:optional>",
        "kind": "struct/member",
        "name": "l/Vectors.vector_of_nullable_strings"
    },
    {
        "kind": "struct",
        "name": "l/Vectors"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "handles",
			dep:  zxLibrary,
			fidl: `
library l;
using zx;
type Handles = resource struct {
    h zx.handle;
    c zx.handle:<CHANNEL, optional>;
};
`,
			expected: `[
    {
        "declaration": "zx/handle:<CHANNEL,optional>",
        "kind": "struct/member",
        "name": "l/Handles.c"
    },
    {
        "declaration": "zx/handle",
        "kind": "struct/member",
        "name": "l/Handles.h"
    },
    {
        "kind": "struct",
        "name": "l/Handles",
        "resourceness": "resource"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "struct local type reference",
			fidl: `
library l;
type A = struct {};
type B = struct {
	a A;
};
`,
			expected: `[
    {
        "kind": "struct",
        "name": "l/A"
    },
    {
        "declaration": "l/A",
        "kind": "struct/member",
        "name": "l/B.a"
    },
    {
        "kind": "struct",
        "name": "l/B"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "structs 2",
			fidl: `
library l;
type CirclePoint = struct {
    x float32;
    y float32;
};
type Color = struct {
    r float32;
    g float32;
    b float32;
};
type Circle = struct {
    filled bool;
    center CirclePoint;
    radius float32;
    color box<Color>;
    dashed bool;
};
`,
			expected: `[
    {
        "declaration": "l/CirclePoint",
        "kind": "struct/member",
        "name": "l/Circle.center"
    },
    {
        "declaration": "box<l/Color>",
        "kind": "struct/member",
        "name": "l/Circle.color"
    },
    {
        "declaration": "bool",
        "kind": "struct/member",
        "name": "l/Circle.dashed"
    },
    {
        "declaration": "bool",
        "kind": "struct/member",
        "name": "l/Circle.filled"
    },
    {
        "declaration": "float32",
        "kind": "struct/member",
        "name": "l/Circle.radius"
    },
    {
        "kind": "struct",
        "name": "l/Circle"
    },
    {
        "declaration": "float32",
        "kind": "struct/member",
        "name": "l/CirclePoint.x"
    },
    {
        "declaration": "float32",
        "kind": "struct/member",
        "name": "l/CirclePoint.y"
    },
    {
        "kind": "struct",
        "name": "l/CirclePoint"
    },
    {
        "declaration": "float32",
        "kind": "struct/member",
        "name": "l/Color.b"
    },
    {
        "declaration": "float32",
        "kind": "struct/member",
        "name": "l/Color.g"
    },
    {
        "declaration": "float32",
        "kind": "struct/member",
        "name": "l/Color.r"
    },
    {
        "kind": "struct",
        "name": "l/Color"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "tables",
			fidl: `
library l;
type Profile = table {
    1: locales vector<string>;
    2: calendars vector<string>;
    3: time_zones vector<string>;
};
`,
			expected: `[
    {
        "declaration": "vector<string>",
        "kind": "table/member",
        "name": "l/Profile.calendars"
    },
    {
        "declaration": "vector<string>",
        "kind": "table/member",
        "name": "l/Profile.locales"
    },
    {
        "declaration": "vector<string>",
        "kind": "table/member",
        "name": "l/Profile.time_zones"
    },
    {
        "kind": "table",
        "name": "l/Profile"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "unions",
			fidl: `
library l;
type Left = struct {};
type Right = struct {};
type Either = strict union {
    1: left Left;
    2: right Right;
};
`,
			expected: `[
    {
        "declaration": "l/Left",
        "kind": "union/member",
        "name": "l/Either.left"
    },
    {
        "declaration": "l/Right",
        "kind": "union/member",
        "name": "l/Either.right"
    },
    {
        "kind": "union",
        "name": "l/Either",
        "strictness": "strict"
    },
    {
        "kind": "struct",
        "name": "l/Left"
    },
    {
        "kind": "struct",
        "name": "l/Right"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "protocols 1",
			fidl: `
library l;
protocol Calculator {
    Add(struct { a int32; b int32; }) -> (struct { sum int32; });
};
`,
			expected: `[
    {
        "declaration": "(int32 a,int32 b) -> (int32 sum)",
        "kind": "protocol/member",
        "name": "l/Calculator.Add"
    },
    {
        "kind": "protocol",
        "name": "l/Calculator"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "protocols 2",
			fidl: `
library l;
type Foo = struct {};
type Bar = struct {};
protocol P {
    M(struct { b box<Bar>; }) -> (struct { c Foo; });
};
`,
			expected: `[
    {
        "kind": "struct",
        "name": "l/Bar"
    },
    {
        "kind": "struct",
        "name": "l/Foo"
    },
    {
        "declaration": "(box<l/Bar> b) -> (l/Foo c)",
        "kind": "protocol/member",
        "name": "l/P.M"
    },
    {
        "kind": "protocol",
        "name": "l/P"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "protocols 3",
			fidl: `
library l;
type Bar = struct {};
protocol P {};
protocol P2 {
    M1(resource struct { a client_end:P; });
    M2(resource struct { a client_end:<P, optional>; });
    M3(resource struct { a server_end:<P>; });
    M4(resource struct { a server_end:<P, optional>; });
};
`,
			expected: `[
    {
        "kind": "struct",
        "name": "l/Bar"
    },
    {
        "kind": "protocol",
        "name": "l/P"
    },
    {
        "declaration": "(client_end:l/P a)",
        "kind": "protocol/member",
        "name": "l/P2.M1"
    },
    {
        "declaration": "(client_end:<l/P,optional> a)",
        "kind": "protocol/member",
        "name": "l/P2.M2"
    },
    {
        "declaration": "(server_end:l/P a)",
        "kind": "protocol/member",
        "name": "l/P2.M3"
    },
    {
        "declaration": "(server_end:<l/P,optional> a)",
        "kind": "protocol/member",
        "name": "l/P2.M4"
    },
    {
        "kind": "protocol",
        "name": "l/P2"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "protocols 4",
			fidl: `
library l;
protocol P {
    -> F1(struct { a int32; });
    F2() -> (struct { a int32; });
	F3() -> (struct {}) error int32;
	F4();
};
`,
			expected: `[
    {
        "declaration": " -> (int32 a)",
        "kind": "protocol/member",
        "name": "l/P.F1"
    },
    {
        "declaration": "() -> (int32 a)",
        "kind": "protocol/member",
        "name": "l/P.F2"
    },
    {
        "declaration": "() -> (l/P_F3_Result result)",
        "kind": "protocol/member",
        "name": "l/P.F3"
    },
    {
        "declaration": "()",
        "kind": "protocol/member",
        "name": "l/P.F4"
    },
    {
        "kind": "protocol",
        "name": "l/P"
    },
    {
        "kind": "struct",
        "name": "l/P_F3_Response"
    },
    {
        "declaration": "int32",
        "kind": "union/member",
        "name": "l/P_F3_Result.err"
    },
    {
        "declaration": "l/P_F3_Response",
        "kind": "union/member",
        "name": "l/P_F3_Result.response"
    },
    {
        "kind": "union",
        "name": "l/P_F3_Result",
        "strictness": "strict"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "protocols with named payloads",
			fidl: `
library l;
type Payload = struct {
    a bool;
};
protocol P {
    -> M1(Payload);
    M2() -> (Payload) error uint32;
};
`,
			expected: `[
    {
        "declaration": " -> (bool a)",
        "kind": "protocol/member",
        "name": "l/P.M1"
    },
    {
        "declaration": "() -> (l/P_M2_Result result)",
        "kind": "protocol/member",
        "name": "l/P.M2"
    },
    {
        "kind": "protocol",
        "name": "l/P"
    },
    {
        "declaration": "uint32",
        "kind": "union/member",
        "name": "l/P_M2_Result.err"
    },
    {
        "declaration": "l/Payload",
        "kind": "union/member",
        "name": "l/P_M2_Result.response"
    },
    {
        "kind": "union",
        "name": "l/P_M2_Result",
        "strictness": "strict"
    },
    {
        "declaration": "bool",
        "kind": "struct/member",
        "name": "l/Payload.a"
    },
    {
        "kind": "struct",
        "name": "l/Payload"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "protocols with non-struct payloads",
			fidl: `
library l;
type U = flexible union {
    1: a uint64;
};
type T = table {
    1: b int32;
};
protocol P {
    M1(U)-> (T);
    M2(table {1: c bool; })-> (strict union {1: d uint32; });
};
`,
			expected: `[
    {
        "declaration": "(l/U payload) -> (l/T payload)",
        "kind": "protocol/member",
        "name": "l/P.M1"
    },
    {
        "declaration": "(l/PM2Request payload) -> (l/PM2Response payload)",
        "kind": "protocol/member",
        "name": "l/P.M2"
    },
    {
        "kind": "protocol",
        "name": "l/P"
    },
    {
        "declaration": "bool",
        "kind": "table/member",
        "name": "l/PM2Request.c"
    },
    {
        "kind": "table",
        "name": "l/PM2Request"
    },
    {
        "declaration": "uint32",
        "kind": "union/member",
        "name": "l/PM2Response.d"
    },
    {
        "kind": "union",
        "name": "l/PM2Response",
        "strictness": "strict"
    },
    {
        "declaration": "int32",
        "kind": "table/member",
        "name": "l/T.b"
    },
    {
        "kind": "table",
        "name": "l/T"
    },
    {
        "declaration": "uint64",
        "kind": "union/member",
        "name": "l/U.a"
    },
    {
        "kind": "union",
        "name": "l/U",
        "strictness": "flexible"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "protocols with errorable non-struct payloads",
			fidl: `
library l;
type T = table {
    1: b string;
};
protocol P {
    M1() -> (flexible union { 1: a bool; }) error uint32;
    M2() -> (T) error int32;
};
`,
			expected: `[
    {
        "declaration": "() -> (l/P_M1_Result result)",
        "kind": "protocol/member",
        "name": "l/P.M1"
    },
    {
        "declaration": "() -> (l/P_M2_Result result)",
        "kind": "protocol/member",
        "name": "l/P.M2"
    },
    {
        "kind": "protocol",
        "name": "l/P"
    },
    {
        "declaration": "bool",
        "kind": "union/member",
        "name": "l/P_M1_Response.a"
    },
    {
        "kind": "union",
        "name": "l/P_M1_Response",
        "strictness": "flexible"
    },
    {
        "declaration": "uint32",
        "kind": "union/member",
        "name": "l/P_M1_Result.err"
    },
    {
        "declaration": "l/P_M1_Response",
        "kind": "union/member",
        "name": "l/P_M1_Result.response"
    },
    {
        "kind": "union",
        "name": "l/P_M1_Result",
        "strictness": "strict"
    },
    {
        "declaration": "int32",
        "kind": "union/member",
        "name": "l/P_M2_Result.err"
    },
    {
        "declaration": "l/T",
        "kind": "union/member",
        "name": "l/P_M2_Result.response"
    },
    {
        "kind": "union",
        "name": "l/P_M2_Result",
        "strictness": "strict"
    },
    {
        "declaration": "string",
        "kind": "table/member",
        "name": "l/T.b"
    },
    {
        "kind": "table",
        "name": "l/T"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "check types",
			fidl: `
library l;
type S = struct {
    f1 string;
    f2 string:4;
    f3 string:<4, optional>;
};
`,
			expected: `[
    {
        "declaration": "string",
        "kind": "struct/member",
        "name": "l/S.f1"
    },
    {
        "declaration": "string:4",
        "kind": "struct/member",
        "name": "l/S.f2"
    },
    {
        "declaration": "string:<4,optional>",
        "kind": "struct/member",
        "name": "l/S.f3"
    },
    {
        "kind": "struct",
        "name": "l/S"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "with foreign library",
			dep:  l2Library,
			fidl: `
library l;
using l2;
type A = struct {
  a l2.T;
};
`,
			expected: `[
    {
        "declaration": "l2/T",
        "kind": "struct/member",
        "name": "l/A.a"
    },
    {
        "kind": "struct",
        "name": "l/A"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "protocol with foreign library",
			dep:  l2Library,
			fidl: `
library l;
using l2;
type Foo = struct {};
type Bar = struct {};
protocol Calculator {
    compose l2.Inverter;
    Halve(l2.UnaryArg) -> (l2.UnaryArg);
    Add(struct { a l2.T; b Bar; }) -> (struct { c Foo; });
};
`,
			expected: `[
    {
        "kind": "struct",
        "name": "l/Bar"
    },
    {
        "declaration": "(l2/T a,l/Bar b) -> (l/Foo c)",
        "kind": "protocol/member",
        "name": "l/Calculator.Add"
    },
    {
        "declaration": "(int32 num) -> (int32 num)",
        "kind": "protocol/member",
        "name": "l/Calculator.Halve"
    },
    {
        "declaration": "(int32 num) -> (int32 num)",
        "kind": "protocol/member",
        "name": "l/Calculator.Invert"
    },
    {
        "kind": "protocol",
        "name": "l/Calculator"
    },
    {
        "kind": "struct",
        "name": "l/Foo"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
		{
			name: "reserved keyword",
			fidl: `
library l;
type E = strict union {
    1: reserved;
    2: e int32;
};
type T = table {
    1: reserved;
    2: e int32;
};
`,
			expected: `[
    {
        "declaration": "int32",
        "kind": "union/member",
        "name": "l/E.e"
    },
    {
        "kind": "union",
        "name": "l/E",
        "strictness": "strict"
    },
    {
        "declaration": "int32",
        "kind": "table/member",
        "name": "l/T.e"
    },
    {
        "kind": "table",
        "name": "l/T"
    },
    {
        "kind": "library",
        "name": "l"
    }
]
`,
		},
	}
	runGenerateSummaryTests(t, tests)
}

func runGenerateSummaryTests(t *testing.T, tests []summaryTestCase) {
	t.Helper()
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			c := fidlgentest.EndToEndTest{T: t}
			if test.dep != "" {
				c = c.WithDependency(test.dep)
			}
			r := c.Single(test.fidl)
			b, err := GenerateSummary(r)
			if err != nil {
				t.Fatalf("while summarizing file: %v", err)
			}
			summary := string(b)
			actual := strings.Split(summary, "\n")
			expected := strings.Split(test.expected, "\n")

			if diff := cmp.Diff(expected, actual); diff != "" {
				t.Errorf("expected:\n---BEGIN---\n%+v\n---END---\n\n"+
					"actual:\n---BEGIN---\n%+v\n---END---\n\ndiff:\n%v\n\nroot: %+v",
					test.expected, summary, diff, r)
			}
		})
	}
}

func TestLoadSummariesJSON(t *testing.T) {
	tests := []struct {
		name string
		json string
		want [][]ElementStr
	}{
		{
			name: "eof",
			json: "",
			want: [][]ElementStr{{}},
		}, {
			name: "empty list",
			json: "[]",
			want: [][]ElementStr{{}},
		}, {
			name: "library only",
			json: `[{"kind": "library", "name": "l"}]`,
			want: [][]ElementStr{
				{{Kind: libraryKind, Name: "l"}},
			},
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			summaries, err := LoadSummariesJSON(strings.NewReader(tt.json))
			if err != nil {
				t.Fatalf("LoadSummariesJSON(%q) got unexpected error: %v", tt.json, err)
			}

			if diff := cmp.Diff(tt.want, summaries); diff != "" {
				t.Fatalf("LoadSummariesJSON(%q) got diff (-want,+got):\n%s", tt.json, diff)
			}
		})
	}
}
