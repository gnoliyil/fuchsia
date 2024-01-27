#!/usr/bin/env fuchsia-vendored-python

# Copyright 2020 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""
This tool uses the contents of fidlc .json files to create tags for .fidl files.

When run via fx fidltags, it looks in the existing build directory, and creates
a file named fidl-tags in the root of the source tree for use with your editor.

See `fx fidltags` for help.
"""
import argparse
import sys
import fnmatch
import os
import json


class Tag(object):

    def __init__(self, tag, file, line, column):
        self.tag = tag
        self.file = file
        self.line = line
        self.column = column

    def __repr__(self):
        return f'Tag({self.tag}, {self.file}, {self.line}, {self.column})'


def parse_args():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        '--build-dir',
        required=True,
        help='Fuchsia build dir, e.g. out/default')
    parser.add_argument(
        '--output', default='fidl-tags', help='Output name of the tags file')
    return parser.parse_args()


def strip_library(name):
    """
    >>> strip_library("fuchsia.device/MAX_DEVICE_NAME_LEN")
    'MAX_DEVICE_NAME_LEN'
    >>> strip_library("SomethingGreat")
    'SomethingGreat'
    """
    return name[name.rfind('/') + 1:]  # -1 + 1 returns the whole thing


def get_location_pieces(location_json):
    file = location_json['filename']
    if file != 'generated':
        if file[:6] == '../../':
            file = file[6:]
    return (file, location_json['line'], location_json['column'])


def extract_consts(json):
    """
    >>> extract_consts([
    ...     {
    ...     "name": "fuchsia.device/MAX_DEVICE_NAME_LEN",
    ...     "location": {
    ...         "filename": "../../zircon/system/fidl/fuchsia-device/controller.fidl",
    ...         "line": 11,
    ...         "column": 14
    ...     },
    ...     "type": {
    ...         "kind": "primitive",
    ...         "subtype": "uint64"
    ...     },
    ...     "value": {
    ...         "kind": "literal",
    ...         "literal": {
    ...         "kind": "numeric",
    ...         "value": "32",
    ...         "expression": "32"
    ...         }
    ...     }
    ...     },
    ...     {
    ...     "name": "fuchsia.device/MAX_DEVICE_PATH_LEN",
    ...     "location": {
    ...         "filename": "../../zircon/system/fidl/fuchsia-device/controller.fidl",
    ...         "line": 13,
    ...         "column": 22
    ...     },
    ...     "type": {
    ...         "kind": "primitive",
    ...         "subtype": "uint64"
    ...     },
    ...     "value": {
    ...         "kind": "literal",
    ...         "literal": {
    ...         "kind": "numeric",
    ...         "value": "1024",
    ...         "expression": "1024"
    ...         }
    ...     }
    ...     }
    ... ])
    [Tag(MAX_DEVICE_NAME_LEN, zircon/system/fidl/fuchsia-device/controller.fidl, 11, 14),
     Tag(MAX_DEVICE_PATH_LEN, zircon/system/fidl/fuchsia-device/controller.fidl, 13, 22)]
    """
    result = []
    for c in json:
        tag = strip_library(c['name'])
        result.append(Tag(tag, *get_location_pieces(c['location'])))
    return result


def extract_name_and_members(json):
    """
    Extracts the tags from enum_, struct_, or table_declarations. They're
    similar enough that we can use the same function.

    >>> extract_name_and_members([
    ... {
    ... "name": "fuchsia.wlan.device/SupportedPhy",
    ... "location": {
    ...     "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...     "line": 10,
    ...     "column": 6
    ... },
    ... "type": "uint32",
    ... "members": [
    ...     {
    ...     "name": "DSSS",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 11,
    ...         "column": 5
    ...     },
    ...     "value": {
    ...         "kind": "literal",
    ...         "literal": {
    ...         "kind": "numeric",
    ...         "value": "0",
    ...         "expression": "0"
    ...         }
    ...     }
    ...     },
    ...     {
    ...     "name": "CCK",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 12,
    ...         "column": 5
    ...     },
    ...     "value": {
    ...         "kind": "literal",
    ...         "literal": {
    ...         "kind": "numeric",
    ...         "value": "1",
    ...         "expression": "1"
    ...         }
    ...     }
    ...     },
    ...     {
    ...     "name": "OFDM",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 13,
    ...         "column": 5
    ...     },
    ...     "value": {
    ...         "kind": "literal",
    ...         "literal": {
    ...         "kind": "numeric",
    ...         "value": "2",
    ...         "expression": "2"
    ...         }
    ...     }
    ...     },
    ... ]
    ... }])
    [Tag(SupportedPhy, garnet/lib/wlan/fidl/phy.fidl, 10, 6),
     Tag(DSSS, garnet/lib/wlan/fidl/phy.fidl, 11, 5),
     Tag(CCK, garnet/lib/wlan/fidl/phy.fidl, 12, 5),
     Tag(OFDM, garnet/lib/wlan/fidl/phy.fidl, 13, 5)]

    Struct declarations:

    >>> extract_name_and_members([
    ... {
    ... "name": "fuchsia.wlan.device/HtCapabilities",
    ... "location": {
    ...     "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...     "line": 31,
    ...     "column": 8
    ... },
    ... "members": [
    ...     {
    ...     "type": {
    ...         "kind": "primitive",
    ...         "subtype": "uint16"
    ...     },
    ...     "name": "ht_capability_info",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 32,
    ...         "column": 12
    ...     },
    ...     },
    ...     {
    ...     "type": {
    ...         "kind": "primitive",
    ...         "subtype": "uint8"
    ...     },
    ...     "name": "ampdu_params",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 33,
    ...         "column": 11
    ...     },
    ...     },
    ...     {
    ...     "type": {
    ...         "kind": "array",
    ...         "element_type": {
    ...         "kind": "primitive",
    ...         "subtype": "uint8"
    ...         },
    ...         "element_count": 16
    ...     },
    ...     "name": "supported_mcs_set",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 34,
    ...         "column": 21
    ...     },
    ...     },
    ...     {
    ...     "type": {
    ...         "kind": "primitive",
    ...         "subtype": "uint16"
    ...     },
    ...     "name": "ht_ext_capabilities",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 35,
    ...         "column": 12
    ...     },
    ...     },
    ...     {
    ...     "type": {
    ...         "kind": "primitive",
    ...         "subtype": "uint32"
    ...     },
    ...     "name": "tx_beamforming_capabilities",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 36,
    ...         "column": 12
    ...     },
    ...     },
    ...     {
    ...     "type": {
    ...         "kind": "primitive",
    ...         "subtype": "uint8"
    ...     },
    ...     "name": "asel_capabilities",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 37,
    ...         "column": 11
    ...     },
    ...     }
    ... ],
    ... },
    ... {
    ... "name": "fuchsia.wlan.device/VhtCapabilities",
    ... "location": {
    ...     "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...     "line": 40,
    ...     "column": 8
    ... },
    ... "members": [
    ...     {
    ...     "type": {
    ...         "kind": "primitive",
    ...         "subtype": "uint32"
    ...     },
    ...     "name": "vht_capability_info",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 41,
    ...         "column": 12
    ...     },
    ...     },
    ...     {
    ...     "type": {
    ...         "kind": "primitive",
    ...         "subtype": "uint64"
    ...     },
    ...     "name": "supported_vht_mcs_and_nss_set",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 42,
    ...         "column": 12
    ...     },
    ...     }
    ... ],
    ... },
    ... {
    ... "name": "fuchsia.wlan.device/ChannelList",
    ... "location": {
    ...     "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...     "line": 45,
    ...     "column": 8
    ... },
    ... "members": [
    ...     {
    ...     "type": {
    ...         "kind": "primitive",
    ...         "subtype": "uint16"
    ...     },
    ...     "name": "base_freq",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 46,
    ...         "column": 12
    ...     },
    ...     },
    ...     {
    ...     "type": {
    ...         "kind": "vector",
    ...         "element_type": {
    ...         "kind": "primitive",
    ...         "subtype": "uint8"
    ...         },
    ...         "maybe_element_count": 200,
    ...     },
    ...     "name": "channels",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 47,
    ...         "column": 23
    ...     },
    ...     },
    ... ],
    ... }
    ... ])
    [Tag(HtCapabilities, garnet/lib/wlan/fidl/phy.fidl, 31, 8),
     Tag(ht_capability_info, garnet/lib/wlan/fidl/phy.fidl, 32, 12),
     Tag(ampdu_params, garnet/lib/wlan/fidl/phy.fidl, 33, 11),
     Tag(supported_mcs_set, garnet/lib/wlan/fidl/phy.fidl, 34, 21),
     Tag(ht_ext_capabilities, garnet/lib/wlan/fidl/phy.fidl, 35, 12),
     Tag(tx_beamforming_capabilities, garnet/lib/wlan/fidl/phy.fidl, 36, 12),
     Tag(asel_capabilities, garnet/lib/wlan/fidl/phy.fidl, 37, 11),
     Tag(VhtCapabilities, garnet/lib/wlan/fidl/phy.fidl, 40, 8),
     Tag(vht_capability_info, garnet/lib/wlan/fidl/phy.fidl, 41, 12),
     Tag(supported_vht_mcs_and_nss_set, garnet/lib/wlan/fidl/phy.fidl, 42, 12),
     Tag(ChannelList, garnet/lib/wlan/fidl/phy.fidl, 45, 8),
     Tag(base_freq, garnet/lib/wlan/fidl/phy.fidl, 46, 12),
     Tag(channels, garnet/lib/wlan/fidl/phy.fidl, 47, 23)]

    Tables declarations (note reserved: True members to be excluded):

    >>> extract_name_and_members([
    ... {
    ... "name": "fuchsia.test.breakpoints/EventPayload",
    ... "location": {
    ...     "filename": "../../src/sys/component_manager/tests/fidl/breakpoints.fidl",
    ...     "line": 59,
    ...     "column": 7
    ... },
    ... "members": [
    ...     {
    ...     "name": "routing_payload",
    ...     "location": {
    ...         "filename": "../../src/sys/component_manager/tests/fidl/breakpoints.fidl",
    ...         "line": 61,
    ...         "column": 23
    ...     },
    ...     },
    ...     {
    ...     "name": "use_capability_payload",
    ...     "location": {
    ...         "filename": "../../src/sys/component_manager/tests/fidl/breakpoints.fidl",
    ...         "line": 64,
    ...         "column": 29
    ...     },
    ...     }
    ... ],
    ... },
    ... {
    ... "name": "fuchsia.test.breakpoints/RoutingPayload",
    ... "location": {
    ...     "filename": "../../src/sys/component_manager/tests/fidl/breakpoints.fidl",
    ...     "line": 68,
    ...     "column": 7
    ... },
    ... "members": [
    ...     {
    ...     "type": {
    ...         "kind": "identifier",
    ...         "identifier": "fuchsia.test.breakpoints/RoutingProtocol",
    ...     },
    ...     "name": "routing_protocol",
    ...     "location": {
    ...         "filename": "../../src/sys/component_manager/tests/fidl/breakpoints.fidl",
    ...         "line": 71,
    ...         "column": 24
    ...     },
    ...     },
    ...     {
    ...     "ordinal": 2,
    ...     "type": {
    ...         "kind": "string",
    ...         "maybe_element_count": 50,
    ...     },
    ...     "name": "capability",
    ...     "location": {
    ...         "filename": "../../src/sys/component_manager/tests/fidl/breakpoints.fidl",
    ...         "line": 74,
    ...         "column": 37
    ...     },
    ...     "size": 16,
    ...     "max_out_of_line": 56,
    ...     "alignment": 8,
    ...     "max_handles": 0
    ...     }
    ... ],
    ... },
    ... {
    ... "name": "fuchsia.test.breakpoints/UseCapabilityPayload",
    ... "location": {
    ...     "filename": "../../src/sys/component_manager/tests/fidl/breakpoints.fidl",
    ...     "line": 78,
    ...     "column": 7
    ... },
    ... "members": [
    ...     {
    ...     "type": {
    ...         "kind": "string",
    ...         "maybe_element_count": 50,
    ...     },
    ...     "name": "capability",
    ...     "location": {
    ...         "filename": "../../src/sys/component_manager/tests/fidl/breakpoints.fidl",
    ...         "line": 80,
    ...         "column": 37
    ...     },
    ...     },
    ...     {
    ...         "reserved": True,
    ...         "location": {
    ...             "column": 5,
    ...             "line": 43,
    ...             "filename": "../../sdk/fidl/fuchsia.feedback/data_provider.fidl"
    ...         }
    ...     }
    ... ],
    ... },
    ... ])
    [Tag(EventPayload, src/sys/component_manager/tests/fidl/breakpoints.fidl, 59, 7),
     Tag(routing_payload, src/sys/component_manager/tests/fidl/breakpoints.fidl, 61, 23),
     Tag(use_capability_payload, src/sys/component_manager/tests/fidl/breakpoints.fidl, 64, 29),
     Tag(RoutingPayload, src/sys/component_manager/tests/fidl/breakpoints.fidl, 68, 7),
     Tag(routing_protocol, src/sys/component_manager/tests/fidl/breakpoints.fidl, 71, 24),
     Tag(capability, src/sys/component_manager/tests/fidl/breakpoints.fidl, 74, 37),
     Tag(UseCapabilityPayload, src/sys/component_manager/tests/fidl/breakpoints.fidl, 78, 7),
     Tag(capability, src/sys/component_manager/tests/fidl/breakpoints.fidl, 80, 37)]

    Bits declarations:

    >>> extract_name_and_members([
    ... {
    ...  "name": "fuchsia.io2/ConnectionInfoQuery",
    ...  "location": {
    ...    "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...    "line": 33,
    ...    "column": 6,
    ...    "length": 19
    ...  },
    ...  "maybe_attributes": [
    ...    {
    ...      "name": "Doc",
    ...      "value": ""
    ...    }
    ...  ],
    ...  "type": {
    ...    "kind": "primitive",
    ...    "subtype": "uint64"
    ...  },
    ...  "mask": "7",
    ...  "members": [
    ...    {
    ...      "name": "REPRESENTATION",
    ...      "location": {
    ...        "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...        "line": 35,
    ...        "column": 5,
    ...        "length": 14
    ...      },
    ...      "value": {
    ...        "kind": "literal",
    ...        "value": "1",
    ...        "expression": "0x1",
    ...        "literal": {
    ...          "kind": "numeric",
    ...          "value": "1",
    ...          "expression": "0x1"
    ...        }
    ...      },
    ...      "maybe_attributes": [
    ...        {
    ...          "name": "Doc",
    ...          "value": " Requests [`ConnectionInfo.representation`]."
    ...        }
    ...      ]
    ...    },
    ...    {
    ...      "name": "RIGHTS",
    ...      "location": {
    ...        "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...        "line": 38,
    ...        "column": 5,
    ...        "length": 6
    ...      },
    ...      "value": {
    ...        "kind": "literal",
    ...        "value": "2",
    ...        "expression": "0x2",
    ...        "literal": {
    ...          "kind": "numeric",
    ...          "value": "2",
    ...          "expression": "0x2"
    ...        }
    ...      },
    ...      "maybe_attributes": [
    ...        {
    ...          "name": "Doc",
    ...          "value": " Requests [`ConnectionInfo.rights`]."
    ...        }
    ...      ]
    ...    },
    ...    {
    ...      "name": "AVAILABLE_OPERATIONS",
    ...      "location": {
    ...        "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...        "line": 41,
    ...        "column": 5,
    ...        "length": 20
    ...      },
    ...      "value": {
    ...        "kind": "literal",
    ...        "value": "4",
    ...        "expression": "0x4",
    ...        "literal": {
    ...          "kind": "numeric",
    ...          "value": "4",
    ...          "expression": "0x4"
    ...        }
    ...      },
    ...      "maybe_attributes": [
    ...        {
    ...          "name": "Doc",
    ...          "value": ""
    ...        }
    ...      ]
    ...    }
    ...  ],
    ...  "strict": True
    ... },
    ... {
    ...  "name": "fuchsia.io2/NodeProtocols",
    ...  "location": {
    ...    "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...    "line": 102,
    ...    "column": 6,
    ...    "length": 13
    ...  },
    ...  "maybe_attributes": [
    ...    {
    ...      "name": "Doc",
    ...      "value": ""
    ...    }
    ...  ],
    ...  "type": {
    ...    "kind": "primitive",
    ...    "subtype": "uint64"
    ...  },
    ...  "mask": "805306495",
    ...  "members": [
    ...    {
    ...      "name": "CONNECTOR",
    ...      "location": {
    ...        "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...        "line": 106,
    ...        "column": 5,
    ...        "length": 9
    ...      },
    ...      "value": {
    ...        "kind": "literal",
    ...        "value": "1",
    ...        "expression": "0x1",
    ...        "literal": {
    ...          "kind": "numeric",
    ...          "value": "1",
    ...          "expression": "0x1"
    ...        }
    ...      },
    ...      "maybe_attributes": [
    ...        {
    ...          "name": "Doc",
    ...          "value": ""
    ...        }
    ...      ]
    ...    },
    ...    {
    ...      "name": "DIRECTORY",
    ...      "location": {
    ...        "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...        "line": 110,
    ...        "column": 5,
    ...        "length": 9
    ...      },
    ...      "value": {
    ...        "kind": "literal",
    ...        "value": "2",
    ...        "expression": "0x2",
    ...        "literal": {
    ...          "kind": "numeric",
    ...          "value": "2",
    ...          "expression": "0x2"
    ...        }
    ...      },
    ...      "maybe_attributes": [
    ...        {
    ...          "name": "Doc",
    ...          "value": ""
    ...        }
    ...      ]
    ...    },
    ...    {
    ...      "name": "FILE",
    ...      "location": {
    ...        "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...        "line": 114,
    ...        "column": 5,
    ...        "length": 4
    ...      },
    ...      "value": {
    ...        "kind": "literal",
    ...        "value": "4",
    ...        "expression": "0x4",
    ...        "literal": {
    ...          "kind": "numeric",
    ...          "value": "4",
    ...          "expression": "0x4"
    ...        }
    ...      },
    ...      "maybe_attributes": [
    ...        {
    ...          "name": "Doc",
    ...          "value": ""
    ...        }
    ...      ]
    ...    },
    ...    {
    ...      "name": "MEMORY",
    ...      "location": {
    ...        "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...        "line": 121,
    ...        "column": 5,
    ...        "length": 6
    ...      },
    ...      "value": {
    ...        "kind": "literal",
    ...        "value": "8",
    ...        "expression": "0x8",
    ...        "literal": {
    ...          "kind": "numeric",
    ...          "value": "8",
    ...          "expression": "0x8"
    ...        }
    ...      },
    ...      "maybe_attributes": [
    ...        {
    ...          "name": "Doc",
    ...          "value": ""
    ...        }
    ...      ]
    ...    },
    ...    {
    ...      "name": "POSIX_SOCKET",
    ...      "location": {
    ...        "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...        "line": 125,
    ...        "column": 5,
    ...        "length": 12
    ...      },
    ...      "value": {
    ...        "kind": "literal",
    ...        "value": "16",
    ...        "expression": "0x10",
    ...        "literal": {
    ...          "kind": "numeric",
    ...          "value": "16",
    ...          "expression": "0x10"
    ...        }
    ...      },
    ...      "maybe_attributes": [
    ...        {
    ...          "name": "Doc",
    ...          "value": ""
    ...        }
    ...      ]
    ...    },
    ...    {
    ...      "name": "PIPE",
    ...      "location": {
    ...        "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...        "line": 129,
    ...        "column": 5,
    ...        "length": 4
    ...      },
    ...      "value": {
    ...        "kind": "literal",
    ...        "value": "32",
    ...        "expression": "0x20",
    ...        "literal": {
    ...          "kind": "numeric",
    ...          "value": "32",
    ...          "expression": "0x20"
    ...        }
    ...      },
    ...      "maybe_attributes": [
    ...        {
    ...          "name": "Doc",
    ...          "value": ""
    ...        }
    ...      ]
    ...    },
    ...    {
    ...      "name": "DEBUGLOG",
    ...      "location": {
    ...        "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...        "line": 133,
    ...        "column": 5,
    ...        "length": 8
    ...      },
    ...      "value": {
    ...        "kind": "literal",
    ...        "value": "64",
    ...        "expression": "0x40",
    ...        "literal": {
    ...          "kind": "numeric",
    ...          "value": "64",
    ...          "expression": "0x40"
    ...        }
    ...      },
    ...      "maybe_attributes": [
    ...        {
    ...          "name": "Doc",
    ...          "value": ""
    ...        }
    ...      ]
    ...    },
    ...    {
    ...      "name": "DEVICE",
    ...      "location": {
    ...        "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...        "line": 136,
    ...        "column": 5,
    ...        "length": 6
    ...      },
    ...      "value": {
    ...        "kind": "literal",
    ...        "value": "268435456",
    ...        "expression": "0x10000000",
    ...        "literal": {
    ...          "kind": "numeric",
    ...          "value": "268435456",
    ...          "expression": "0x10000000"
    ...        }
    ...      },
    ...      "maybe_attributes": [
    ...        {
    ...          "name": "Deprecated",
    ...          "value": "devices will be services in the future"
    ...        }
    ...      ]
    ...    },
    ...    {
    ...      "name": "TTY",
    ...      "location": {
    ...        "filename": "../../sdk/fidl/fuchsia.io2/connection-info.fidl",
    ...        "line": 139,
    ...        "column": 5,
    ...        "length": 3
    ...      },
    ...      "value": {
    ...        "kind": "literal",
    ...        "value": "536870912",
    ...        "expression": "0x20000000",
    ...        "literal": {
    ...          "kind": "numeric",
    ...          "value": "536870912",
    ...          "expression": "0x20000000"
    ...        }
    ...      },
    ...      "maybe_attributes": [
    ...        {
    ...          "name": "Deprecated",
    ...          "value": "tty functionalities may be covered by a tty service"
    ...        }
    ...      ]
    ...    }
    ...  ],
    ...  "strict": True
    ... }])
    [Tag(ConnectionInfoQuery, sdk/fidl/fuchsia.io2/connection-info.fidl, 33, 6),
     Tag(REPRESENTATION, sdk/fidl/fuchsia.io2/connection-info.fidl, 35, 5),
     Tag(RIGHTS, sdk/fidl/fuchsia.io2/connection-info.fidl, 38, 5),
     Tag(AVAILABLE_OPERATIONS, sdk/fidl/fuchsia.io2/connection-info.fidl, 41, 5),
     Tag(NodeProtocols, sdk/fidl/fuchsia.io2/connection-info.fidl, 102, 6),
     Tag(CONNECTOR, sdk/fidl/fuchsia.io2/connection-info.fidl, 106, 5),
     Tag(DIRECTORY, sdk/fidl/fuchsia.io2/connection-info.fidl, 110, 5),
     Tag(FILE, sdk/fidl/fuchsia.io2/connection-info.fidl, 114, 5),
     Tag(MEMORY, sdk/fidl/fuchsia.io2/connection-info.fidl, 121, 5),
     Tag(POSIX_SOCKET, sdk/fidl/fuchsia.io2/connection-info.fidl, 125, 5),
     Tag(PIPE, sdk/fidl/fuchsia.io2/connection-info.fidl, 129, 5),
     Tag(DEBUGLOG, sdk/fidl/fuchsia.io2/connection-info.fidl, 133, 5),
     Tag(DEVICE, sdk/fidl/fuchsia.io2/connection-info.fidl, 136, 5),
     Tag(TTY, sdk/fidl/fuchsia.io2/connection-info.fidl, 139, 5)]
    """
    result = []
    for x in json:
        tag = strip_library(x['name'])
        result.append(Tag(tag, *get_location_pieces(x['location'])))
        for member in x['members']:
            if member.get('reserved'):
                continue
            result.append(
                Tag(member['name'], *get_location_pieces(member['location'])))
    return result


def extract_protocols(json):
    """
    >>> extract_protocols([
    ... {
    ... "name": "fuchsia.wlan.device/Phy",
    ... "location": {
    ...     "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...     "line": 112,
    ...     "column": 10
    ... },
    ... "methods": [
    ...     {
    ...     "name": "Query",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 113,
    ...         "column": 5
    ...     },
    ...     },
    ...     {
    ...     "name": "CreateIface",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 114,
    ...         "column": 5
    ...     },
    ...     },
    ... ]
    ... },
    ... {
    ... "name": "fuchsia.wlan.device/Connector",
    ... "location": {
    ...     "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...     "line": 123,
    ...     "column": 10
    ... },
    ... "methods": [
    ...     {
    ...     "name": "Connect",
    ...     "location": {
    ...         "filename": "../../garnet/lib/wlan/fidl/phy.fidl",
    ...         "line": 124,
    ...         "column": 5
    ...     },
    ...     }
    ... ]
    ... },
    ... ])
    [Tag(Phy, garnet/lib/wlan/fidl/phy.fidl, 112, 10),
     Tag(Query, garnet/lib/wlan/fidl/phy.fidl, 113, 5),
     Tag(CreateIface, garnet/lib/wlan/fidl/phy.fidl, 114, 5),
     Tag(Connector, garnet/lib/wlan/fidl/phy.fidl, 123, 10),
     Tag(Connect, garnet/lib/wlan/fidl/phy.fidl, 124, 5)]

    Some special handling for Transport=Syscall to add the leading zx_ as an
    alternate name.

    >>> extract_protocols([
    ... {
    ... "name": "zz/profile",
    ... "location": {
    ...     "filename": "../../zircon/syscalls/profile.fidl",
    ...     "line": 38,
    ...     "column": 10
    ... },
    ... "maybe_attributes": [
    ...     {
    ...     "name": "Transport",
    ...     "value": "Syscall"
    ...     }
    ... ],
    ... "methods": [
    ...     {
    ...     "name": "profile_create",
    ...     "location": {
    ...         "filename": "../../zircon/syscalls/profile.fidl",
    ...         "line": 41,
    ...         "column": 5
    ...     },
    ...     }
    ... ]
    ... },
    ... {
    ... "name": "zz/socket",
    ... "location": {
    ...     "filename": "../../zircon/syscalls/socket.fidl",
    ...     "line": 9,
    ...     "column": 10
    ... },
    ... "maybe_attributes": [
    ...     {
    ...     "name": "Transport",
    ...     "value": "Syscall"
    ...     }
    ... ],
    ... "methods": [
    ...     {
    ...     "name": "socket_create",
    ...     "location": {
    ...         "filename": "../../zircon/syscalls/socket.fidl",
    ...         "line": 11,
    ...         "column": 5
    ...     },
    ...     },
    ...     {
    ...     "name": "socket_write",
    ...     "location": {
    ...         "filename": "../../zircon/syscalls/socket.fidl",
    ...         "line": 15,
    ...         "column": 5
    ...     },
    ...     },
    ... ]
    ... },
    ... ])
    [Tag(profile, zircon/syscalls/profile.fidl, 38, 10),
     Tag(profile_create, zircon/syscalls/profile.fidl, 41, 5),
     Tag(zx_profile_create, zircon/syscalls/profile.fidl, 41, 5),
     Tag(socket, zircon/syscalls/socket.fidl, 9, 10),
     Tag(socket_create, zircon/syscalls/socket.fidl, 11, 5),
     Tag(zx_socket_create, zircon/syscalls/socket.fidl, 11, 5),
     Tag(socket_write, zircon/syscalls/socket.fidl, 15, 5),
     Tag(zx_socket_write, zircon/syscalls/socket.fidl, 15, 5)]
    """

    def is_transport_syscall(x):
        attribs = x.get('maybe_attributes', [])
        for attrib in attribs:
            if attrib.get('name') == 'Transport' and attrib.get(
                    'value') == 'Syscall':
                return True
        return False

    result = []
    for i in json:
        tag = strip_library(i['name'])
        is_syscall = is_transport_syscall(i)
        result.append(Tag(tag, *get_location_pieces(i['location'])))
        for method in i['methods']:
            result.append(
                Tag(method['name'], *get_location_pieces(method['location'])))
            if is_syscall:
                result.append(
                    Tag(
                        'zx_' + method['name'],
                        *get_location_pieces(method['location'])))
    return result


def get_tags(json, tags):
    tags.extend(extract_name_and_members(json['bits_declarations']))
    tags.extend(extract_consts(json['const_declarations']))
    tags.extend(extract_name_and_members(json['enum_declarations']))
    tags.extend(extract_protocols(json['protocol_declarations']))
    tags.extend(extract_name_and_members(json['struct_declarations']))
    tags.extend(extract_name_and_members(json['table_declarations']))
    tags.extend(extract_name_and_members(json['union_declarations']))


def get_syscall_tags(json, tags):
    tags.extend


def main():
    args = parse_args()

    matches = []
    for root, dirnames, filenames in os.walk(args.build_dir):
        for filename in fnmatch.filter(filenames, '*.fidl.json'):
            matches.append(os.path.join(root, filename))

    # Include the syscalls ir file too.
    matches.append(
        os.path.join(args.build_dir, 'gen', 'zircon', 'vdso', 'zx.fidl.json'))

    tags = []
    for filename in matches:
        with open(filename) as f:
            get_tags(json.load(f), tags)

    tags = [x for x in tags if x.file != 'generated']
    tags.sort(key=lambda x: x.tag)

    with open(args.output, 'w') as f:
        f.write('!_TAG_FILE_SORTED\t1\tgenerated by generated-fidl-tags.py\n')
        for t in tags:
            f.write(
                '%s\t%s\t/\%%%dl\%%%dc/\n' % (t.tag, t.file, t.line, t.column))


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'test':
        import doctest
        doctest.testmod(optionflags=doctest.NORMALIZE_WHITESPACE)
    else:
        sys.exit(main())
