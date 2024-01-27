// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//go:build !build_with_native_toolchain

package filter

import (
	"testing"

	"fidl/fuchsia/net"
	"fidl/fuchsia/net/filter"

	"github.com/google/go-cmp/cmp"
	"github.com/google/go-cmp/cmp/cmpopts"

	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/link/loopback"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv4"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv6"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
	"gvisor.dev/gvisor/pkg/tcpip/transport/icmp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/tcp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/udp"
)

func maskAsAddr(s tcpip.Subnet) tcpip.Address {
	mask := s.Mask()
	return tcpip.AddrFromSlice(mask.AsSlice())
}

func TestFilterUpdates(t *testing.T) {
	cmpOpts := []cmp.Option{
		cmp.AllowUnexported(TCPSourcePortMatcher{}),
		cmp.AllowUnexported(TCPDestinationPortMatcher{}),
		cmp.AllowUnexported(UDPSourcePortMatcher{}),
		cmp.AllowUnexported(UDPDestinationPortMatcher{}),
		cmp.AllowUnexported(portMatcher{}),
		// We aren't testing NIC filtering here.
		cmpopts.IgnoreUnexported(filterDisabledNICMatcher{}),
	}

	ipv4Addr1Bytes := [4]uint8{10}
	const ipv4Addr1Prefix = 8
	ipv4Subnet1 := net.Subnet{
		Addr:      net.IpAddressWithIpv4(net.Ipv4Address{Addr: ipv4Addr1Bytes}),
		PrefixLen: ipv4Addr1Prefix,
	}
	ipv4TCPIPSubnet1 := tcpip.AddressWithPrefix{
		Address:   tcpip.AddrFrom4(ipv4Addr1Bytes),
		PrefixLen: ipv4Addr1Prefix,
	}.Subnet()

	ipv4Addr2Bytes := [4]uint8{192}
	const ipv4Addr2Prefix = 8
	ipv4Subnet2 := net.Subnet{
		Addr:      net.IpAddressWithIpv4(net.Ipv4Address{Addr: ipv4Addr2Bytes}),
		PrefixLen: ipv4Addr2Prefix,
	}
	ipv4TCPIPSubnet2 := tcpip.AddressWithPrefix{
		Address:   tcpip.AddrFrom4(ipv4Addr2Bytes),
		PrefixLen: ipv4Addr2Prefix,
	}.Subnet()

	ipv6Addr1Bytes := [16]uint8{0xfe, 0x80}
	const ipv6Addr1Prefix = 64
	ipv6Subnet1 := net.Subnet{
		Addr:      net.IpAddressWithIpv6(net.Ipv6Address{Addr: ipv6Addr1Bytes}),
		PrefixLen: ipv6Addr1Prefix,
	}
	ipv6TCPIPSubnet1 := tcpip.AddressWithPrefix{
		Address:   tcpip.AddrFrom16(ipv6Addr1Bytes),
		PrefixLen: ipv6Addr1Prefix,
	}.Subnet()

	ipv6Addr2Bytes := [16]uint8{0xa0}
	const ipv6Addr2Prefix = 96
	ipv6Subnet2 := net.Subnet{
		Addr:      net.IpAddressWithIpv6(net.Ipv6Address{Addr: ipv6Addr2Bytes}),
		PrefixLen: ipv6Addr2Prefix,
	}
	ipv6TCPIPSubnet2 := tcpip.AddressWithPrefix{
		Address:   tcpip.AddrFrom16(ipv6Addr2Bytes),
		PrefixLen: ipv6Addr2Prefix,
	}.Subnet()

	var emptyFilterDisabledNICMatcher filterDisabledNICMatcher
	emptyFilterDisabledNICMatcher.init()

	defaultV4Table := func(defaultTables *stack.IPTables) stack.Table {
		return defaultTables.GetTable(stack.FilterID, false /* ipv6 */)
	}
	defaultV6Table := func(defaultTables *stack.IPTables) stack.Table {
		return defaultTables.GetTable(stack.FilterID, true /* ipv6 */)
	}

	tests := []struct {
		name    string
		rules   []filter.Rule
		result  filter.FilterUpdateRulesResult
		v4Table func(*stack.IPTables) stack.Table
		v6Table func(*stack.IPTables) stack.Table
	}{
		{
			name:    "empty",
			rules:   nil,
			result:  filter.FilterUpdateRulesResultWithResponse(filter.FilterUpdateRulesResponse{}),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "port filter",
			rules: []filter.Rule{
				{
					Action:    filter.ActionDrop,
					Direction: filter.DirectionIncoming,
					Proto:     filter.SocketProtocolTcp,
					SrcPortRange: filter.PortRange{
						Start: 1,
						End:   2,
					},
					DstPortRange: filter.PortRange{
						Start: 3,
						End:   4,
					},
				},
				{
					Action:    filter.ActionDrop,
					Direction: filter.DirectionOutgoing,
					Proto:     filter.SocketProtocolIcmp,
					// Port ranges are ignored for ICMP.
				},
				{
					Action:    filter.ActionPass,
					Direction: filter.DirectionOutgoing,
					Proto:     filter.SocketProtocolUdp,
					SrcPortRange: filter.PortRange{
						Start: 9,
						End:   10,
					},
					DstPortRange: filter.PortRange{
						Start: 11,
						End:   12,
					},
				},
				{
					Action:    filter.ActionPass,
					Direction: filter.DirectionIncoming,
					Proto:     filter.SocketProtocolIcmpv6,
					// Port ranges are ignored for ICMPv6.
				},
			},
			result: filter.FilterUpdateRulesResultWithResponse(filter.FilterUpdateRulesResponse{}),
			v4Table: func(*stack.IPTables) stack.Table {
				return stack.Table{
					Rules: []stack.Rule{
						// Initial Accept for non input/output hooks.
						{
							Target: &stack.AcceptTarget{},
						},

						// Input chain.
						{
							// Don't run filters on an interface with filters disabled.
							Matchers: []stack.Matcher{
								&emptyFilterDisabledNICMatcher,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      tcp.ProtocolNumber,
								CheckProtocol: true,
							},
							Matchers: []stack.Matcher{
								NewTCPSourcePortMatcher(1, 2),
								NewTCPDestinationPortMatcher(3, 4),
							},
							Target: &stack.DropTarget{},
						},
						{
							Target: &stack.AcceptTarget{},
						},

						// Output chain.
						{
							// Don't run filters on an interface with filters disabled.
							Matchers: []stack.Matcher{
								&emptyFilterDisabledNICMatcher,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      icmp.ProtocolNumber4,
								CheckProtocol: true,
							},
							Matchers: nil,
							Target:   &stack.DropTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      udp.ProtocolNumber,
								CheckProtocol: true,
							},
							Matchers: []stack.Matcher{
								NewUDPSourcePortMatcher(9, 10),
								NewUDPDestinationPortMatcher(11, 12),
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Target: &stack.AcceptTarget{},
						},
					},
					BuiltinChains: [stack.NumHooks]int{
						stack.Prerouting:  0,
						stack.Input:       1,
						stack.Forward:     0,
						stack.Output:      4,
						stack.Postrouting: 0,
					},
				}
			},
			v6Table: func(*stack.IPTables) stack.Table {
				return stack.Table{
					Rules: []stack.Rule{
						// Initial Accept for non input/output hooks.
						{
							Target: &stack.AcceptTarget{},
						},

						// Input chain.
						{
							// Don't run filters on an interface with filters disabled.
							Matchers: []stack.Matcher{
								&emptyFilterDisabledNICMatcher,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      tcp.ProtocolNumber,
								CheckProtocol: true,
							},
							Matchers: []stack.Matcher{
								NewTCPSourcePortMatcher(1, 2),
								NewTCPDestinationPortMatcher(3, 4),
							},
							Target: &stack.DropTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      icmp.ProtocolNumber6,
								CheckProtocol: true,
							},
							Matchers: nil,
							Target:   &stack.AcceptTarget{},
						},
						{
							Target: &stack.AcceptTarget{},
						},

						// Output chain.
						{
							// Don't run filters on an interface with filters disabled.
							Matchers: []stack.Matcher{
								&emptyFilterDisabledNICMatcher,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      udp.ProtocolNumber,
								CheckProtocol: true,
							},
							Matchers: []stack.Matcher{
								NewUDPSourcePortMatcher(9, 10),
								NewUDPDestinationPortMatcher(11, 12),
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Target: &stack.AcceptTarget{},
						},
					},
					BuiltinChains: [stack.NumHooks]int{
						stack.Prerouting:  0,
						stack.Input:       1,
						stack.Forward:     0,
						stack.Output:      5,
						stack.Postrouting: 0,
					},
				}
			},
		},
		{
			name: "address filters",
			rules: []filter.Rule{
				{
					Action:               filter.ActionPass,
					Direction:            filter.DirectionIncoming,
					Proto:                filter.SocketProtocolTcp,
					SrcSubnet:            &ipv4Subnet1,
					SrcSubnetInvertMatch: false,
					DstSubnet:            nil,
					DstSubnetInvertMatch: true,
				},
				{
					Action:               filter.ActionDrop,
					Direction:            filter.DirectionIncoming,
					Proto:                filter.SocketProtocolIcmp,
					SrcSubnet:            nil,
					SrcSubnetInvertMatch: false,
					DstSubnet:            &ipv4Subnet2,
					DstSubnetInvertMatch: true,
				},
				{
					Action:               filter.ActionDrop,
					Direction:            filter.DirectionOutgoing,
					Proto:                filter.SocketProtocolUdp,
					SrcSubnet:            &ipv6Subnet1,
					SrcSubnetInvertMatch: true,
					DstSubnet:            nil,
					DstSubnetInvertMatch: false,
				},
				{
					Action:               filter.ActionPass,
					Direction:            filter.DirectionOutgoing,
					Proto:                filter.SocketProtocolIcmpv6,
					SrcSubnet:            nil,
					SrcSubnetInvertMatch: true,
					DstSubnet:            &ipv6Subnet2,
					DstSubnetInvertMatch: false,
				},
				{
					Action:               filter.ActionDrop,
					Direction:            filter.DirectionOutgoing,
					SrcSubnet:            &ipv4Subnet1,
					SrcSubnetInvertMatch: false,
					DstSubnet:            &ipv4Subnet2,
					DstSubnetInvertMatch: true,
				},
				{
					Action:               filter.ActionPass,
					Direction:            filter.DirectionIncoming,
					SrcSubnet:            &ipv6Subnet1,
					SrcSubnetInvertMatch: true,
					DstSubnet:            &ipv6Subnet2,
					DstSubnetInvertMatch: false,
				},
			},
			result: filter.FilterUpdateRulesResultWithResponse(filter.FilterUpdateRulesResponse{}),
			v4Table: func(*stack.IPTables) stack.Table {
				return stack.Table{
					Rules: []stack.Rule{
						// Initial Accept for non input/output hooks.
						{
							Target: &stack.AcceptTarget{},
						},

						// Input chain.
						{
							// Don't run filters on an interface with filters disabled.
							Matchers: []stack.Matcher{
								&emptyFilterDisabledNICMatcher,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      tcp.ProtocolNumber,
								CheckProtocol: true,
								Src:           ipv4TCPIPSubnet1.ID(),
								SrcMask:       maskAsAddr(ipv4TCPIPSubnet1),
								SrcInvert:     false,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      icmp.ProtocolNumber4,
								CheckProtocol: true,
								Dst:           ipv4TCPIPSubnet2.ID(),
								DstMask:       maskAsAddr(ipv4TCPIPSubnet2),
								DstInvert:     true,
							},
							Target: &stack.DropTarget{},
						},
						{
							Target: &stack.AcceptTarget{},
						},

						// Output chain.
						{
							// Don't run filters on an interface with filters disabled.
							Matchers: []stack.Matcher{
								&emptyFilterDisabledNICMatcher,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Src:       ipv4TCPIPSubnet1.ID(),
								SrcMask:   maskAsAddr(ipv4TCPIPSubnet1),
								SrcInvert: false,
								Dst:       ipv4TCPIPSubnet2.ID(),
								DstMask:   maskAsAddr(ipv4TCPIPSubnet2),
								DstInvert: true,
							},
							Target: &stack.DropTarget{},
						},
						{
							Target: &stack.AcceptTarget{},
						},
					},
					BuiltinChains: [stack.NumHooks]int{
						stack.Prerouting:  0,
						stack.Input:       1,
						stack.Forward:     0,
						stack.Output:      5,
						stack.Postrouting: 0,
					},
				}
			},
			v6Table: func(*stack.IPTables) stack.Table {
				return stack.Table{
					Rules: []stack.Rule{
						// Initial Accept for non input/output hooks.
						{
							Target: &stack.AcceptTarget{},
						},

						// Input chain.
						{
							// Don't run filters on an interface with filters disabled.
							Matchers: []stack.Matcher{
								&emptyFilterDisabledNICMatcher,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Src:       ipv6TCPIPSubnet1.ID(),
								SrcMask:   maskAsAddr(ipv6TCPIPSubnet1),
								SrcInvert: true,
								Dst:       ipv6TCPIPSubnet2.ID(),
								DstMask:   maskAsAddr(ipv6TCPIPSubnet2),
								DstInvert: false,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Target: &stack.AcceptTarget{},
						},

						// Output chain.
						{
							// Don't run filters on an interface with filters disabled.
							Matchers: []stack.Matcher{
								&emptyFilterDisabledNICMatcher,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      udp.ProtocolNumber,
								CheckProtocol: true,
								Src:           ipv6TCPIPSubnet1.ID(),
								SrcMask:       maskAsAddr(ipv6TCPIPSubnet1),
								SrcInvert:     true,
							},
							Matchers: nil,
							Target:   &stack.DropTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      icmp.ProtocolNumber6,
								CheckProtocol: true,
								Dst:           ipv6TCPIPSubnet2.ID(),
								DstMask:       maskAsAddr(ipv6TCPIPSubnet2),
								DstInvert:     false,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Target: &stack.AcceptTarget{},
						},
					},
					BuiltinChains: [stack.NumHooks]int{
						stack.Prerouting:  0,
						stack.Input:       1,
						stack.Forward:     0,
						stack.Output:      4,
						stack.Postrouting: 0,
					},
				}
			},
		},
		{
			name: "IPv4 src with IPv6 dst",
			rules: []filter.Rule{
				{
					Action:    filter.ActionDrop,
					SrcSubnet: &ipv4Subnet1,
					DstSubnet: &ipv6Subnet1,
				},
			},
			result:  filter.FilterUpdateRulesResultWithErr(filter.FilterUpdateRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "IPv6 src with IPv4 dst",
			rules: []filter.Rule{
				{
					Action:    filter.ActionDrop,
					SrcSubnet: &ipv6Subnet1,
					DstSubnet: &ipv4Subnet1,
				},
			},
			result:  filter.FilterUpdateRulesResultWithErr(filter.FilterUpdateRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "IPv4 src with ICMPv6",
			rules: []filter.Rule{
				{
					Action:    filter.ActionDrop,
					Proto:     filter.SocketProtocolIcmpv6,
					SrcSubnet: &ipv4Subnet1,
				},
			},
			result:  filter.FilterUpdateRulesResultWithErr(filter.FilterUpdateRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "IPv4 dst with ICMPv6",
			rules: []filter.Rule{
				{
					Action:    filter.ActionDrop,
					Proto:     filter.SocketProtocolIcmpv6,
					DstSubnet: &ipv4Subnet1,
				},
			},
			result:  filter.FilterUpdateRulesResultWithErr(filter.FilterUpdateRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "IPv6 src with ICMPv4",
			rules: []filter.Rule{
				{
					Action:    filter.ActionDrop,
					Proto:     filter.SocketProtocolIcmp,
					SrcSubnet: &ipv6Subnet1,
				},
			},
			result:  filter.FilterUpdateRulesResultWithErr(filter.FilterUpdateRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "IPv6 dst with ICMPv4",
			rules: []filter.Rule{
				{
					Action:    filter.ActionDrop,
					Proto:     filter.SocketProtocolIcmp,
					DstSubnet: &ipv6Subnet1,
				},
			},
			result:  filter.FilterUpdateRulesResultWithErr(filter.FilterUpdateRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "unrecognized proto",
			rules: []filter.Rule{
				{
					Action: filter.ActionDrop,
					Proto:  255,
				},
			},
			result:  filter.FilterUpdateRulesResultWithErr(filter.FilterUpdateRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "unrecognized action",
			rules: []filter.Rule{
				{
					Action: 255,
					Proto:  filter.SocketProtocolTcp,
				},
			},
			result:  filter.FilterUpdateRulesResultWithErr(filter.FilterUpdateRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "unrecognized direction",
			rules: []filter.Rule{
				{
					Action:    filter.ActionDrop,
					Direction: 255,
					Proto:     filter.SocketProtocolTcp,
				},
			},
			result:  filter.FilterUpdateRulesResultWithErr(filter.FilterUpdateRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "bad src port range",
			rules: []filter.Rule{
				{
					Action:    filter.ActionDrop,
					Direction: filter.DirectionIncoming,
					Proto:     filter.SocketProtocolTcp,
					SrcPortRange: filter.PortRange{
						Start: 2,
						End:   1,
					},
					DstPortRange: filter.PortRange{
						Start: 3,
						End:   4,
					},
				},
			},
			result:  filter.FilterUpdateRulesResultWithErr(filter.FilterUpdateRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "bad dst port range",
			rules: []filter.Rule{
				{
					Action:    filter.ActionDrop,
					Direction: filter.DirectionIncoming,
					Proto:     filter.SocketProtocolTcp,
					SrcPortRange: filter.PortRange{
						Start: 1,
						End:   2,
					},
					DstPortRange: filter.PortRange{
						Start: 4,
						End:   3,
					},
				},
			},
			result:  filter.FilterUpdateRulesResultWithErr(filter.FilterUpdateRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			// TODO(https://fxbug.dev/68501): Keep state is not supported.
			name: "keep state",
			rules: []filter.Rule{
				{
					Action:               filter.ActionPass,
					Direction:            filter.DirectionOutgoing,
					Proto:                filter.SocketProtocolTcp,
					SrcSubnet:            nil,
					SrcSubnetInvertMatch: true,
					DstSubnet:            &ipv6Subnet2,
					DstSubnetInvertMatch: false,
					KeepState:            true,
				},
			},
			result:  filter.FilterUpdateRulesResultWithErr(filter.FilterUpdateRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			s := stack.New(stack.Options{
				NetworkProtocols: []stack.NetworkProtocolFactory{ipv4.NewProtocol, ipv6.NewProtocol},
			})

			defaultTables := stack.DefaultTables(s.Clock(), s.Rand())

			expectedV4Table := test.v4Table(defaultTables)
			expectedV6Table := test.v6Table(defaultTables)

			f := New(s)
			if got, want := f.updateRules(test.rules, 0), test.result; got != want {
				t.Fatalf("got f.updateRules(_, 0) = %#v, want = %#v", got, want)
			}

			iptables := s.IPTables()
			v4table := iptables.GetTable(stack.FilterID, false /* ipv6 */)
			if diff := cmp.Diff(v4table, expectedV4Table, cmpOpts...); diff != "" {
				t.Errorf("IPv4 filter table mispatch (-want +got):\n%s", diff)
			}
			v6table := iptables.GetTable(stack.FilterID, true /* ipv6 */)
			if diff := cmp.Diff(v6table, expectedV6Table, cmpOpts...); diff != "" {
				t.Errorf("IPv6 filter table mispatch (-want +got):\n%s", diff)
			}
		})
	}
}

func TestFilterEnableInterface(t *testing.T) {
	const (
		nicID   = 1
		nicName = "lo"
	)

	tests := []struct {
		name    string
		actions func(*Filter)
		enabled bool
	}{
		{
			name: "Initial state",
			actions: func(*Filter) {
				/* do nothing */
			},
			enabled: false,
		},
		{
			name: "Enable",
			actions: func(f *Filter) {
				f.enableInterface(nicID)
			},
			enabled: true,
		},
		{
			name: "Enable, then Disable",
			actions: func(f *Filter) {
				f.enableInterface(nicID)
				f.disableInterface(nicID)
			},
			enabled: false,
		},
		{
			name: "Enable, then RemovedNIC",
			actions: func(f *Filter) {
				f.enableInterface(nicID)
				f.RemovedNIC(nicID)
			},
			enabled: false,
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			s := stack.New(stack.Options{
				NetworkProtocols: []stack.NetworkProtocolFactory{ipv6.NewProtocol},
			})
			nicOpts := stack.NICOptions{Name: nicName}
			if err := s.CreateNICWithOptions(nicID, loopback.New(), nicOpts); err != nil {
				t.Fatalf("CreateNICWithOptions(%d, _, %#v)= %s", nicID, nicOpts, err)
			}

			f := New(s)
			test.actions(f)

			enabled := !f.filterDisabledNICMatcher.nicDisabled(nicName)
			if got, want := enabled, test.enabled; got != want {
				t.Errorf("got enabled = %t, want = %t", got, want)
			}
		})
	}
}

func TestNATUpdates(t *testing.T) {
	const (
		nicID   = 1
		nicName = "nicName"
	)

	cmpOpts := []cmp.Option{
		// We aren't testing NIC filtering here.
		cmpopts.IgnoreUnexported(filterDisabledNICMatcher{}),
	}

	ipv4Addr1Bytes := [4]uint8{10}
	const ipv4Addr1Prefix = 8
	ipv4Subnet1 := net.Subnet{
		Addr:      net.IpAddressWithIpv4(net.Ipv4Address{Addr: ipv4Addr1Bytes}),
		PrefixLen: ipv4Addr1Prefix,
	}
	ipv4TCPIPSubnet1 := tcpip.AddressWithPrefix{
		Address:   tcpip.AddrFrom4(ipv4Addr1Bytes),
		PrefixLen: ipv4Addr1Prefix,
	}.Subnet()

	ipv4Addr2Bytes := [4]uint8{192}
	const ipv4Addr2Prefix = 8
	ipv4Subnet2 := net.Subnet{
		Addr:      net.IpAddressWithIpv4(net.Ipv4Address{Addr: ipv4Addr2Bytes}),
		PrefixLen: ipv4Addr2Prefix,
	}
	ipv4TCPIPSubnet2 := tcpip.AddressWithPrefix{
		Address:   tcpip.AddrFrom4(ipv4Addr2Bytes),
		PrefixLen: ipv4Addr2Prefix,
	}.Subnet()

	ipv6Addr1Bytes := [16]uint8{0xfe, 0x80}
	const ipv6Addr1Prefix = 64
	ipv6Subnet1 := net.Subnet{
		Addr:      net.IpAddressWithIpv6(net.Ipv6Address{Addr: ipv6Addr1Bytes}),
		PrefixLen: ipv6Addr1Prefix,
	}
	ipv6TCPIPSubnet1 := tcpip.AddressWithPrefix{
		Address:   tcpip.AddrFrom16(ipv6Addr1Bytes),
		PrefixLen: ipv6Addr1Prefix,
	}.Subnet()

	ipv6Addr2Bytes := [16]uint8{0xa0}
	const ipv6Addr2Prefix = 96
	ipv6Subnet2 := net.Subnet{
		Addr:      net.IpAddressWithIpv6(net.Ipv6Address{Addr: ipv6Addr2Bytes}),
		PrefixLen: ipv6Addr2Prefix,
	}
	ipv6TCPIPSubnet2 := tcpip.AddressWithPrefix{
		Address:   tcpip.AddrFrom16(ipv6Addr2Bytes),
		PrefixLen: ipv6Addr2Prefix,
	}.Subnet()

	var emptyFilterDisabledNICMatcher filterDisabledNICMatcher
	emptyFilterDisabledNICMatcher.init()

	defaultV4Table := func(defaultTables *stack.IPTables) stack.Table {
		return defaultTables.GetTable(stack.NATID, false /* ipv6 */)
	}
	defaultV6Table := func(defaultTables *stack.IPTables) stack.Table {
		return defaultTables.GetTable(stack.NATID, true /* ipv6 */)
	}

	tests := []struct {
		name    string
		rules   []filter.Nat
		result  filter.FilterUpdateNatRulesResult
		v4Table func(*stack.IPTables) stack.Table
		v6Table func(*stack.IPTables) stack.Table
	}{
		{
			name:    "empty",
			rules:   nil,
			result:  filter.FilterUpdateNatRulesResultWithResponse(filter.FilterUpdateNatRulesResponse{}),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "IPv4 src with ICMPv6",
			rules: []filter.Nat{
				{
					Proto:     filter.SocketProtocolIcmpv6,
					SrcSubnet: ipv4Subnet1,
				},
			},
			result:  filter.FilterUpdateNatRulesResultWithErr(filter.FilterUpdateNatRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "IPv6 src with ICMPv4",
			rules: []filter.Nat{
				{
					Proto:     filter.SocketProtocolIcmp,
					SrcSubnet: ipv6Subnet1,
				},
			},
			result:  filter.FilterUpdateNatRulesResultWithErr(filter.FilterUpdateNatRulesErrorBadRule),
			v4Table: defaultV4Table,
			v6Table: defaultV6Table,
		},
		{
			name: "Valid rules",
			rules: []filter.Nat{
				{
					Proto:       filter.SocketProtocolTcp,
					SrcSubnet:   ipv4Subnet1,
					OutgoingNic: nicID,
				},
				{
					Proto:     filter.SocketProtocolUdp,
					SrcSubnet: ipv4Subnet1,
				},
				{
					Proto:     filter.SocketProtocolIcmp,
					SrcSubnet: ipv4Subnet1,
				},
				{
					Proto:     filter.SocketProtocolAny,
					SrcSubnet: ipv4Subnet2,
				},
				{
					Proto:     filter.SocketProtocolTcp,
					SrcSubnet: ipv6Subnet1,
				},
				{
					Proto:     filter.SocketProtocolUdp,
					SrcSubnet: ipv6Subnet1,
				},
				{
					Proto:     filter.SocketProtocolIcmpv6,
					SrcSubnet: ipv6Subnet1,
				},
				{
					Proto:       filter.SocketProtocolAny,
					SrcSubnet:   ipv6Subnet2,
					OutgoingNic: nicID,
				},
			},
			result: filter.FilterUpdateNatRulesResultWithResponse(filter.FilterUpdateNatRulesResponse{}),
			v4Table: func(*stack.IPTables) stack.Table {
				return stack.Table{
					Rules: []stack.Rule{
						// Initial Accept for non-postrouting hooks.
						{
							Target: &stack.AcceptTarget{},
						},

						// Postrouting chain.
						{
							// Don't run filters on an interface with filters disabled.
							Matchers: []stack.Matcher{
								&emptyFilterDisabledNICMatcher,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:        header.TCPProtocolNumber,
								CheckProtocol:   true,
								Src:             ipv4TCPIPSubnet1.ID(),
								SrcMask:         maskAsAddr(ipv4TCPIPSubnet1),
								SrcInvert:       false,
								OutputInterface: nicName,
							},
							Target: &stack.MasqueradeTarget{NetworkProtocol: header.IPv4ProtocolNumber},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      header.UDPProtocolNumber,
								CheckProtocol: true,
								Src:           ipv4TCPIPSubnet1.ID(),
								SrcMask:       maskAsAddr(ipv4TCPIPSubnet1),
								SrcInvert:     false,
							},
							Target: &stack.MasqueradeTarget{NetworkProtocol: header.IPv4ProtocolNumber},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      header.ICMPv4ProtocolNumber,
								CheckProtocol: true,
								Src:           ipv4TCPIPSubnet1.ID(),
								SrcMask:       maskAsAddr(ipv4TCPIPSubnet1),
								SrcInvert:     false,
							},
							Target: &stack.MasqueradeTarget{NetworkProtocol: header.IPv4ProtocolNumber},
						},
						{
							Filter: stack.IPHeaderFilter{
								Src:       ipv4TCPIPSubnet2.ID(),
								SrcMask:   maskAsAddr(ipv4TCPIPSubnet2),
								SrcInvert: false,
							},
							Target: &stack.MasqueradeTarget{NetworkProtocol: header.IPv4ProtocolNumber},
						},
						{
							Target: &stack.AcceptTarget{},
						},
					},
					BuiltinChains: [stack.NumHooks]int{
						stack.Prerouting:  0,
						stack.Input:       0,
						stack.Forward:     0,
						stack.Output:      0,
						stack.Postrouting: 1,
					},
				}
			},
			v6Table: func(*stack.IPTables) stack.Table {
				return stack.Table{
					Rules: []stack.Rule{
						// Initial Accept for non-postrouting hooks.
						{
							Target: &stack.AcceptTarget{},
						},

						// Postrouting chain.
						{
							// Don't run filters on an interface with filters disabled.
							Matchers: []stack.Matcher{
								&emptyFilterDisabledNICMatcher,
							},
							Target: &stack.AcceptTarget{},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      header.TCPProtocolNumber,
								CheckProtocol: true,
								Src:           ipv6TCPIPSubnet1.ID(),
								SrcMask:       maskAsAddr(ipv6TCPIPSubnet1),
								SrcInvert:     false,
							},
							Target: &stack.MasqueradeTarget{NetworkProtocol: header.IPv6ProtocolNumber},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      header.UDPProtocolNumber,
								CheckProtocol: true,
								Src:           ipv6TCPIPSubnet1.ID(),
								SrcMask:       maskAsAddr(ipv6TCPIPSubnet1),
								SrcInvert:     false,
							},
							Target: &stack.MasqueradeTarget{NetworkProtocol: header.IPv6ProtocolNumber},
						},
						{
							Filter: stack.IPHeaderFilter{
								Protocol:      header.ICMPv6ProtocolNumber,
								CheckProtocol: true,
								Src:           ipv6TCPIPSubnet1.ID(),
								SrcMask:       maskAsAddr(ipv6TCPIPSubnet1),
								SrcInvert:     false,
							},
							Target: &stack.MasqueradeTarget{NetworkProtocol: header.IPv6ProtocolNumber},
						},
						{
							Filter: stack.IPHeaderFilter{
								Src:             ipv6TCPIPSubnet2.ID(),
								SrcMask:         maskAsAddr(ipv6TCPIPSubnet2),
								SrcInvert:       false,
								OutputInterface: nicName,
							},
							Target: &stack.MasqueradeTarget{NetworkProtocol: header.IPv6ProtocolNumber},
						},
						{
							Target: &stack.AcceptTarget{},
						},
					},
					BuiltinChains: [stack.NumHooks]int{
						stack.Prerouting:  0,
						stack.Input:       0,
						stack.Forward:     0,
						stack.Output:      0,
						stack.Postrouting: 1,
					},
				}
			},
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			s := stack.New(stack.Options{
				NetworkProtocols: []stack.NetworkProtocolFactory{ipv4.NewProtocol, ipv6.NewProtocol},
			})

			opts := stack.NICOptions{Name: nicName}
			if err := s.CreateNICWithOptions(nicID, loopback.New(), opts); err != nil {
				t.Fatalf("CreateNICWithOptions(%d, _, %#v): %s", nicID, opts, err)
			}

			defaultTables := stack.DefaultTables(s.Clock(), s.Rand())

			expectedV4Table := test.v4Table(defaultTables)
			expectedV6Table := test.v6Table(defaultTables)

			f := New(s)
			if got, want := f.updateNATRules(test.rules, 0), test.result; got != want {
				t.Fatalf("got f.updateNATRules(_, 0) = %#v, want = %#v", got, want)
			}

			iptables := s.IPTables()
			v4table := iptables.GetTable(stack.NATID, false /* ipv6 */)
			if diff := cmp.Diff(v4table, expectedV4Table, cmpOpts...); diff != "" {
				t.Errorf("IPv4 NAT table mispatch (-want +got):\n%s", diff)
			}
			v6table := iptables.GetTable(stack.NATID, true /* ipv6 */)
			if diff := cmp.Diff(v6table, expectedV6Table, cmpOpts...); diff != "" {
				t.Errorf("IPv6 NAT table mispatch (-want +got):\n%s", diff)
			}
		})
	}
}
