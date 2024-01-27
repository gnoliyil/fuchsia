// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Useful constants for tests.

/// IPv4 constants.
pub mod ipv4 {
    /// A default IPv4 time-to-live value.
    pub const DEFAULT_TTL: u8 = 64;
}

/// IPv6 constants.
pub mod ipv6 {
    use net_declare::{net_ip_v6, net_subnet_v6};
    use net_types::ip as net_types_ip;

    /// A default IPv6 hop limit value.
    pub const DEFAULT_HOP_LIMIT: u8 = 64;

    /// A globally-routable IPv6 prefix.
    pub const GLOBAL_PREFIX: net_types_ip::Subnet<net_types_ip::Ipv6Addr> =
        net_subnet_v6!("2001:f1f0:4060:1::/64");

    /// An IPv6 address in `GLOBAL_PREFIX`.
    pub const GLOBAL_ADDR: net_types_ip::Ipv6Addr = net_ip_v6!("2001:f1f0:4060:1::1");

    /// A link-local IPv6 address.
    ///
    /// fe80::1
    pub const LINK_LOCAL_ADDR: net_types_ip::Ipv6Addr = net_ip_v6!("fe80::1");

    /// The prefix length for the link-local subnet.
    pub const LINK_LOCAL_SUBNET_PREFIX: u8 = 64;
}

/// Ethernet constants.
pub mod eth {
    use net_declare::net_mac;
    use net_types::ethernet::Mac;

    /// A MAC address.
    ///
    /// 02:00:00:00:00:01
    pub const MAC_ADDR: Mac = net_mac!("02:00:00:00:00:01");
}
