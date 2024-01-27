// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Rather than reuse existing _ext types, we define intermediary types for
//! JSON serialization to avoid coupling too closely to particular FIDL
//! protocols.

use fidl_fuchsia_net_routes_ext as froutes_ext;
use net_types::{ip::IpAddress as _, Witness as _};
use thiserror::Error;

#[derive(serde::Serialize)]
pub(crate) struct Subnet<T> {
    pub(crate) addr: T,
    pub(crate) prefix_len: u8,
}

impl From<fidl_fuchsia_net_ext::Subnet> for Subnet<std::net::IpAddr> {
    fn from(ext: fidl_fuchsia_net_ext::Subnet) -> Subnet<std::net::IpAddr> {
        let fidl_fuchsia_net_ext::Subnet {
            addr: fidl_fuchsia_net_ext::IpAddress(addr),
            prefix_len,
        } = ext;
        Subnet { addr, prefix_len }
    }
}

impl<A: net_types::ip::IpAddress> From<net_types::ip::Subnet<A>> for Subnet<std::net::IpAddr> {
    fn from(sub: net_types::ip::Subnet<A>) -> Subnet<std::net::IpAddr> {
        let addr = sub.network().to_ip_addr().into();
        let prefix_len = sub.prefix();
        Subnet { addr, prefix_len }
    }
}

#[derive(serde::Serialize)]
pub(crate) struct Addresses {
    pub(crate) ipv4: Vec<Subnet<std::net::Ipv4Addr>>,
    pub(crate) ipv6: Vec<Subnet<std::net::Ipv6Addr>>,
}

impl<I: Iterator<Item = fidl_fuchsia_net_ext::Subnet>> From<I> for Addresses {
    fn from(addresses: I) -> Addresses {
        use itertools::Itertools as _;

        let (ipv4, ipv6): (Vec<_>, Vec<_>) = addresses.into_iter().partition_map(
            |fidl_fuchsia_net_ext::Subnet {
                 addr: fidl_fuchsia_net_ext::IpAddress(addr),
                 prefix_len,
             }| {
                match addr {
                    std::net::IpAddr::V4(addr) => {
                        itertools::Either::Left(Subnet { addr, prefix_len })
                    }
                    std::net::IpAddr::V6(addr) => {
                        itertools::Either::Right(Subnet { addr, prefix_len })
                    }
                }
            },
        );
        Addresses { ipv4, ipv6 }
    }
}

#[derive(serde::Serialize)]
pub(crate) enum DeviceClass {
    Loopback,
    Virtual,
    Ethernet,
    Wlan,
    Ppp,
    Bridge,
    WlanAp,
}

impl From<fidl_fuchsia_net_interfaces::DeviceClass> for DeviceClass {
    fn from(device_class: fidl_fuchsia_net_interfaces::DeviceClass) -> Self {
        match device_class {
            fidl_fuchsia_net_interfaces::DeviceClass::Loopback(
                fidl_fuchsia_net_interfaces::Empty,
            ) => Self::Loopback,
            fidl_fuchsia_net_interfaces::DeviceClass::Device(device_class) => match device_class {
                fidl_fuchsia_hardware_network::DeviceClass::Virtual => Self::Virtual,
                fidl_fuchsia_hardware_network::DeviceClass::Ethernet => Self::Ethernet,
                fidl_fuchsia_hardware_network::DeviceClass::Wlan => Self::Wlan,
                fidl_fuchsia_hardware_network::DeviceClass::Ppp => Self::Ppp,
                fidl_fuchsia_hardware_network::DeviceClass::Bridge => Self::Bridge,
                fidl_fuchsia_hardware_network::DeviceClass::WlanAp => Self::WlanAp,
            },
        }
    }
}

#[derive(serde::Serialize)]
/// Intermediary struct for serializing interface properties into JSON.
pub(crate) struct InterfaceView {
    pub(crate) nicid: u64,
    pub(crate) name: String,
    pub(crate) device_class: DeviceClass,
    pub(crate) online: bool,
    pub(crate) addresses: Addresses,
    pub(crate) mac: Option<fidl_fuchsia_net_ext::MacAddress>,
}

impl From<(fidl_fuchsia_net_interfaces_ext::Properties, Option<fidl_fuchsia_net::MacAddress>)>
    for InterfaceView
{
    fn from(
        t: (fidl_fuchsia_net_interfaces_ext::Properties, Option<fidl_fuchsia_net::MacAddress>),
    ) -> InterfaceView {
        let (
            fidl_fuchsia_net_interfaces_ext::Properties {
                id,
                name,
                device_class,
                online,
                addresses,
                has_default_ipv4_route: _,
                has_default_ipv6_route: _,
            },
            mac,
        ) = t;
        InterfaceView {
            nicid: id.get(),
            name,
            device_class: device_class.into(),
            online,
            addresses: addresses
                .into_iter()
                .map(|fidl_fuchsia_net_interfaces_ext::Address { addr, valid_until: _ }| {
                    addr.into()
                })
                .into(),
            mac: mac.map(Into::into),
        }
    }
}

#[derive(serde::Serialize)]
/// Intermediary struct for serializing IP forwarding table entries into JSON.
pub struct ForwardingEntry {
    #[serde(rename = "destination")]
    subnet: Subnet<std::net::IpAddr>,
    #[serde(rename = "nicid")]
    device_id: u64,
    #[serde(rename = "gateway")]
    next_hop: Option<std::net::IpAddr>,
    metric: u32,
}

/// Errors returned when converting from [`froutes_ext::InstalledRoute`]
/// to [`ForwardingEntry`].
#[derive(Debug, Error)]
pub enum ForwardingEntryConversionError {
    #[error("the route's action was unknown")]
    UnknownRouteAction,
}

impl<I: net_types::ip::Ip> TryFrom<froutes_ext::InstalledRoute<I>> for ForwardingEntry {
    type Error = ForwardingEntryConversionError;
    fn try_from(route: froutes_ext::InstalledRoute<I>) -> Result<Self, Self::Error> {
        let froutes_ext::InstalledRoute {
            route: froutes_ext::Route { destination, action, properties: _ },
            effective_properties: froutes_ext::EffectiveRouteProperties { metric },
        } = route;
        let (device_id, next_hop) = match action {
            froutes_ext::RouteAction::Forward(froutes_ext::RouteTarget {
                outbound_interface,
                next_hop,
            }) => (outbound_interface, next_hop),
            froutes_ext::RouteAction::Unknown => {
                return Err(ForwardingEntryConversionError::UnknownRouteAction)
            }
        };
        let subnet = destination.into();
        let next_hop = next_hop.map(|next_hop| next_hop.get().to_ip_addr().into());
        Ok(Self { subnet, device_id, next_hop, metric })
    }
}

pub struct NeighborTableEntryIteratorItemVariants<T> {
    existing: T,
    added: T,
    changed: T,
    removed: T,
    idle: T,
}

impl<T> NeighborTableEntryIteratorItemVariants<T> {
    pub fn select(self, item: &fidl_fuchsia_net_neighbor::EntryIteratorItem) -> T {
        use fidl_fuchsia_net_neighbor::EntryIteratorItem;
        let Self { existing, added, changed, removed, idle } = self;
        match item {
            EntryIteratorItem::Existing(_) => existing,
            EntryIteratorItem::Added(_) => added,
            EntryIteratorItem::Changed(_) => changed,
            EntryIteratorItem::Removed(_) => removed,
            EntryIteratorItem::Idle(_) => idle,
        }
    }
}

impl<T> IntoIterator for NeighborTableEntryIteratorItemVariants<T> {
    type Item = T;
    type IntoIter = <[T; 5] as IntoIterator>::IntoIter;

    fn into_iter(self) -> Self::IntoIter {
        let Self { existing, added, changed, removed, idle } = self;
        [existing, added, changed, removed, idle].into_iter()
    }
}

pub const DISPLAYED_NEIGH_ENTRY_VARIANTS: NeighborTableEntryIteratorItemVariants<&'static str> =
    NeighborTableEntryIteratorItemVariants {
        existing: "EXISTING",
        added: "ADDED",
        changed: "CHANGED",
        removed: "REMOVED",
        idle: "IDLE",
    };

/// Intermediary type for serializing Entry (e.g. into JSON).
#[derive(serde::Serialize)]
pub struct NeighborTableEntry {
    interface: u64,
    neighbor: std::net::IpAddr,
    state: &'static str,
    mac: Option<fidl_fuchsia_net_ext::MacAddress>,
}

impl From<fidl_fuchsia_net_neighbor_ext::Entry> for NeighborTableEntry {
    fn from(
        fidl_fuchsia_net_neighbor_ext::Entry {
            interface,
            neighbor,
            state,
            mac,
            // Ignored since the tabular format ignores this field.
            updated_at: _,
        }: fidl_fuchsia_net_neighbor_ext::Entry,
    ) -> NeighborTableEntry {
        let fidl_fuchsia_net_ext::IpAddress(neighbor) = neighbor.into();
        NeighborTableEntry {
            interface,
            neighbor,
            state: fidl_fuchsia_net_neighbor_ext::display_entry_state(&state),
            mac: mac.map(|mac| mac.into()),
        }
    }
}
