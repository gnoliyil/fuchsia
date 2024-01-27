// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Common types for dealing with ip table entries.

use core::fmt::{Debug, Display, Formatter};

use net_types::{
    ip::{
        GenericOverIp, Ip, IpAddr, IpAddress, IpInvariant, Ipv4Addr, Ipv6Addr, Subnet, SubnetEither,
    },
    SpecifiedAddr,
};

/// `AddableEntry` is a routing entry that may be used to add a new entry to the
/// forwarding table.
///
/// See [`Entry`] for the type used to represent a route in the forwarding
/// table.
///
/// `AddableEntry` guarantees that at least one of the egress device or
/// gateway is set.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub struct AddableEntry<A: IpAddress, D> {
    subnet: Subnet<A>,
    device: Option<D>,
    gateway: Option<SpecifiedAddr<A>>,
}

impl<D, A: IpAddress> AddableEntry<A, D> {
    /// Creates a new [`AddableEntry`] with a specified gateway.
    pub fn with_gateway(subnet: Subnet<A>, device: Option<D>, gateway: SpecifiedAddr<A>) -> Self {
        Self { subnet, device, gateway: Some(gateway) }
    }

    /// Creates a new [`AddableEntry`] with a specified device.
    pub fn without_gateway(subnet: Subnet<A>, device: D) -> Self {
        Self { subnet, device: Some(device), gateway: None }
    }
}

/// An IPv4 forwarding entry or an IPv6 forwarding entry.
#[allow(missing_docs)]
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub enum AddableEntryEither<D> {
    V4(AddableEntry<Ipv4Addr, D>),
    V6(AddableEntry<Ipv6Addr, D>),
}

impl<D> AddableEntryEither<D> {
    /// Creates a new [`AddableEntryEither`] with the specified device as the
    /// next hop.
    pub fn without_gateway(subnet: SubnetEither, device: D) -> Self {
        match subnet {
            SubnetEither::V4(subnet) => AddableEntry::without_gateway(subnet, device).into(),
            SubnetEither::V6(subnet) => AddableEntry::without_gateway(subnet, device).into(),
        }
    }

    /// Gets the subnet, egress device and gateway.
    pub fn into_subnet_device_gateway(
        self,
    ) -> (SubnetEither, Option<D>, Option<IpAddr<SpecifiedAddr<Ipv4Addr>, SpecifiedAddr<Ipv6Addr>>>)
    {
        match self {
            AddableEntryEither::V4(AddableEntry { subnet, device, gateway }) => {
                (subnet.into(), device, gateway.map(|a| a.into()))
            }
            AddableEntryEither::V6(AddableEntry { subnet, device, gateway }) => {
                (subnet.into(), device, gateway.map(|a| a.into()))
            }
        }
    }
}

impl<A: IpAddress, D> From<AddableEntry<A, D>> for AddableEntryEither<D> {
    fn from(entry: AddableEntry<A, D>) -> AddableEntryEither<D> {
        #[derive(GenericOverIp)]
        struct EntryHolder<I: Ip, D>(AddableEntry<I::Addr, D>);
        let IpInvariant(entry_either) = A::Version::map_ip(
            EntryHolder(entry),
            |EntryHolder(entry)| IpInvariant(AddableEntryEither::V4(entry)),
            |EntryHolder(entry)| IpInvariant(AddableEntryEither::V6(entry)),
        );
        entry_either
    }
}

/// A forwarding entry.
///
/// `Entry` is a `Subnet` with an egress device and optional gateway.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub struct Entry<A: IpAddress, D> {
    /// The matching subnet.
    pub subnet: Subnet<A>,
    /// The destination device.
    pub device: D,
    /// An optional gateway if the subnet is not on link.
    // TODO (https://fxbug.dev/123288): Restrict `gateway` to `UnicastAddr`.
    pub gateway: Option<SpecifiedAddr<A>>,
}

impl<A: IpAddress, D: Debug> Display for Entry<A, D> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result<(), core::fmt::Error> {
        let Entry { subnet, device, gateway } = self;
        match gateway {
            Some(gateway) => write!(f, "{:?} (via {}) -> {}", device, gateway, subnet),
            None => write!(f, "{:?} -> {}", device, subnet),
        }
    }
}

/// An IPv4 forwarding entry or an IPv6 forwarding entry.
#[allow(missing_docs)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum EntryEither<D> {
    V4(Entry<Ipv4Addr, D>),
    V6(Entry<Ipv6Addr, D>),
}

impl<D> EntryEither<D> {
    /// Gets the subnet, egress device and gateway.
    pub fn into_subnet_device_gateway(
        self,
    ) -> (SubnetEither, D, Option<IpAddr<SpecifiedAddr<Ipv4Addr>, SpecifiedAddr<Ipv6Addr>>>) {
        match self {
            EntryEither::V4(Entry { subnet, device, gateway }) => {
                (subnet.into(), device, gateway.map(|a| a.into()))
            }
            EntryEither::V6(Entry { subnet, device, gateway }) => {
                (subnet.into(), device, gateway.map(|a| a.into()))
            }
        }
    }
}

impl<D> From<Entry<Ipv4Addr, D>> for EntryEither<D> {
    fn from(entry: Entry<Ipv4Addr, D>) -> EntryEither<D> {
        EntryEither::V4(entry)
    }
}

impl<D> From<Entry<Ipv6Addr, D>> for EntryEither<D> {
    fn from(entry: Entry<Ipv6Addr, D>) -> EntryEither<D> {
        EntryEither::V6(entry)
    }
}
