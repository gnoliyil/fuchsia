// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package targets

import (
	"context"
	"net"
	"reflect"
	"testing"

	"github.com/google/go-cmp/cmp"
)

func TestAddressLogic(t *testing.T) {
	t.Run("percentage signs are escaped", func(t *testing.T) {
		inputs := []string{
			"[fe80::a019:b0ff:fe21:64bd%qemu]:40860",
			"[fe80::a019:b0ff:fe21:64bd%25qemu]:40860",
		}
		expected := "[fe80::a019:b0ff:fe21:64bd%25qemu]:40860"
		for _, input := range inputs {
			actual := escapePercentSign(input)
			if actual != expected {
				t.Errorf("failed to escape percentage sign:\nactual: %s\nexpected: %s", actual, expected)
			}
		}
	})

	t.Run("derivation of the local-scoped local host", func(t *testing.T) {
		inputs := []string{
			"[fe80::a019:b0ff:fe21:64bd%qemu]:40860",
			"[fe80::a019:b0ff:fe21:64bd%25qemu]:40860",
		}
		expected := "[fe80::a019:b0ff:fe21:64bd%25qemu]"
		for _, input := range inputs {
			actual := localScopedLocalHost(input)
			if actual != expected {
				t.Errorf("failed to derive host:\nactual: %s\nexpected: %s", actual, expected)
			}
		}
	})
}

func TestFromJSON(t *testing.T) {
	ctx := context.Background()

	tests := []struct {
		name     string
		obj      string
		expected reflect.Type
	}{
		{
			name:     "derive aemu target",
			obj:      `{"type": "aemu", "target": "x64"}`,
			expected: reflect.TypeOf(&AEMU{}),
		},
		{
			name:     "derive qemu target",
			obj:      `{"type": "qemu", "target": "arm64"}`,
			expected: reflect.TypeOf(&QEMU{}),
		},
		// Testing FromJSON for "device" and "gce" is complex given that
		// the constructor functions for those two targets perform a good amount
		// of side effects, for example, creating a "gce" target will try to
		// initialize a gce instance.
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			target, err := FromJSON(ctx, []byte(test.obj), Options{})
			if err != nil {
				t.Errorf("failed to derive target. err=%q", err)
			}
			if reflect.TypeOf(target) != test.expected {
				t.Errorf("expected target type %q, got %q", test.expected, reflect.TypeOf(target))
			}
		})
	}
}

// Test implementation of FuchsiaTarget using TargetInfo as its implementation
// of TestConfig.
type testTarget struct {
	FuchsiaTarget
	nodename string
	serial   string
	pdu      *targetPDU
	ipv4     net.IP
	ipv6     *net.IPAddr
}

func (t *testTarget) TestConfig(netboot bool) (any, error) { return TargetInfo(t, netboot, t.pdu) }
func (t *testTarget) IPv4() (net.IP, error)                { return t.ipv4, nil }
func (t *testTarget) IPv6() (*net.IPAddr, error)           { return t.ipv6, nil }
func (t *testTarget) Nodename() string                     { return t.nodename }
func (t *testTarget) SerialSocketPath() string             { return t.serial }
func (t *testTarget) SSHKey() string                       { return "" }

func TestTargetInfo(t *testing.T) {
	tests := []struct {
		name    string
		target  testTarget
		netboot bool
		want    targetInfo
	}{
		{
			name:    "valid",
			target:  testTarget{nodename: "node", serial: "serial", ipv4: net.IPv4zero, ipv6: &net.IPAddr{IP: net.IPv6zero}},
			netboot: false,
			want:    targetInfo{Type: "FuchsiaDevice", Nodename: "node", SerialSocket: "serial", IPv4: net.IPv4zero.String(), IPv6: net.IPv6zero.String(), PDU: nil},
		},
		{
			name:    "valid with netboot",
			target:  testTarget{nodename: "node", serial: "serial", ipv4: net.IPv4zero, ipv6: &net.IPAddr{IP: net.IPv6zero}},
			netboot: true,
			want:    targetInfo{Type: "FuchsiaDevice", Nodename: "node", SerialSocket: "serial", PDU: nil},
		},
		{
			name:    "valid no ip addresses",
			target:  testTarget{nodename: "node", serial: "serial"},
			netboot: false,
			want:    targetInfo{Type: "FuchsiaDevice", Nodename: "node", SerialSocket: "serial", PDU: nil},
		},
		{
			name: "valid with pdu",
			target: testTarget{nodename: "node", serial: "serial", pdu: &targetPDU{
				IP:   "192.168.1.1",
				MAC:  "12:34:56:78:9a:bc",
				Port: 1,
			}},
			netboot: false,
			want: targetInfo{Type: "FuchsiaDevice", Nodename: "node", SerialSocket: "serial", PDU: &targetPDU{
				IP:   "192.168.1.1",
				MAC:  "12:34:56:78:9a:bc",
				Port: 1,
			}},
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			got, err := test.target.TestConfig(test.netboot)
			if err != nil {
				t.Errorf("unexpected error from target.TestConfig(false): %s", err)
			}
			if diff := cmp.Diff(test.want, got); diff != "" {
				t.Errorf("TestConfig(false) mismatch (-want +got):\n%s", diff)
			}
		})
	}
}
