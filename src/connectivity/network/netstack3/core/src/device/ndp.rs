// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The Neighbor Discovery Protocol (NDP).
//!
//! Neighbor Discovery for IPv6 as defined in [RFC 4861] defines mechanisms for
//! solving the following problems:
//! - Router Discovery
//! - Prefix Discovery
//! - Parameter Discovery
//! - Address Autoconfiguration
//! - Address resolution
//! - Next-hop determination
//! - Neighbor Unreachability Detection
//! - Duplicate Address Detection
//! - Redirect
//!
//! [RFC 4861]: https://tools.ietf.org/html/rfc4861

use std::collections::{HashMap, HashSet};
use std::fmt::Debug;
use std::num::{NonZeroU16, NonZeroU32, NonZeroU8};
use std::ops::RangeInclusive;
use std::time::Duration;

use log::{debug, error, trace};

use net_types::ip::{AddrSubnet, Ip, Ipv6, Ipv6Addr};
use net_types::{
    LinkLocalAddr, LinkLocalAddress, MulticastAddr, MulticastAddress, SpecifiedAddr,
    SpecifiedAddress, Witness,
};
use packet::{EmptyBuf, InnerPacketBuilder, Serializer};
use rand::{thread_rng, Rng};
use zerocopy::ByteSlice;

use crate::device::ethernet::EthernetNdpDevice;
use crate::device::{AddressState, DeviceId, DeviceLayerTimerId, DeviceProtocol, Tentative};
use crate::ip::{IpDeviceIdContext, IpProto};
use crate::wire::icmp::ndp::options::{NdpOption, PrefixInformation};
use crate::wire::icmp::ndp::{
    self, NeighborAdvertisement, NeighborSolicitation, Options, RouterAdvertisement,
    RouterSolicitation,
};
use crate::wire::icmp::{IcmpMessage, IcmpPacketBuilder, IcmpUnusedCode, Icmpv6Packet};
use crate::wire::ipv6::Ipv6PacketBuilder;
use crate::{Context, EventDispatcher, Instant, StackState, TimerId, TimerIdInner};

/// The IP packet hop limit for all NDP packets.
///
/// See [RFC 4861 section 4.1], [RFC 4861 section 4.2], [RFC 4861 section 4.2],
/// [RFC 4861 section 4.3], [RFC 4861 section 4.4], and [RFC 4861 section 4.5]
/// for more information.
///
/// [RFC 4861 section 4.1]: https://tools.ietf.org/html/rfc4861#section-4.1
/// [RFC 4861 section 4.2]: https://tools.ietf.org/html/rfc4861#section-4.2
/// [RFC 4861 section 4.3]: https://tools.ietf.org/html/rfc4861#section-4.3
/// [RFC 4861 section 4.4]: https://tools.ietf.org/html/rfc4861#section-4.4
/// [RFC 4861 section 4.5]: https://tools.ietf.org/html/rfc4861#section-4.5
const REQUIRED_NDP_IP_PACKET_HOP_LIMIT: u8 = 255;

//
// Default Router configurations
//
// See [`NdpRouterConfigurations`] for more details.
//

// Note that AdvSendAdvertisements MUST be FALSE by default so that a node will not accidentally
// start acting as a default router. Nodes must be explicitly configured by system management to
// send Router Advertisements.
const SHOULD_SEND_ADVERTISEMENTS_DEFAULT: bool = false;
const ROUTER_ADVERTISEMENTS_INTERVAL_DEFAULT: RangeInclusive<u16> = 200..=600;
const ADVERTISED_MANAGED_FLAG_DEFAULT: bool = false;
const ADVERTISED_OTHER_CONFIG_FLAG: bool = false;
const ADVERTISED_LINK_MTU: Option<NonZeroU32> = None;
const ADVERTISED_REACHABLE_TIME: u32 = 0;
const ADVERTISED_RETRANSMIT_TIMER: u32 = 0;
const ADVERTISED_CURRENT_HOP_LIMIT: u8 = 64;
// This call to `new_unchecked` is okay becase 1800 is non-zero, so we will not be violating
// `NonZeroU16`'s contract.
const ADVERTISED_DEFAULT_LIFETIME: Option<NonZeroU16> =
    unsafe { Some(NonZeroU16::new_unchecked(1800)) };

/// The number of NS messages to be sent to perform DAD
/// [RFC 4862 section 5.1]
///
/// [RFC 4862 section 5.1]: https://tools.ietf.org/html/rfc4862#section-5.1
pub(crate) const DUP_ADDR_DETECT_TRANSMITS: u8 = 1;

//
// Node Constants
//

/// The default value for the default hop limit to be used when sending IP
/// packets.
// We know the call to `new_unchecked` is safe because 64 is non-zero.
pub(crate) const HOP_LIMIT_DEFAULT: NonZeroU8 = unsafe { NonZeroU8::new_unchecked(64) };

/// The default value for *BaseReachableTime* as defined in
/// [RFC 4861 section 10].
///
/// [RFC 4861 section 10]: https://tools.ietf.org/html/rfc4861#section-10
const REACHABLE_TIME_DEFAULT: Duration = Duration::from_secs(30);

/// The default value for *RetransTimer* as defined in
/// [RFC 4861 section 10].
///
/// [RFC 4861 section 10]: https://tools.ietf.org/html/rfc4861#section-10
const RETRANS_TIMER_DEFAULT: Duration = Duration::from_secs(1);

/// The maximum number of multicast solicitations as defined in
/// [RFC 4861 section 10].
///
/// [RFC 4861 section 10]: https://tools.ietf.org/html/rfc4861#section-10
const MAX_MULTICAST_SOLICIT: u8 = 3;

//
// Host Constants
//

/// Maximum number of Router Solicitation messages that may be sent
/// when attempting to discover routers. Each message sent must be
/// seperated by at least `RTR_SOLICITATION_INTERVAL` as defined in
/// [RFC 4861 section 10].
///
/// [RFC 4861 section 10]: https://tools.ietf.org/html/rfc4861#section-10
pub(crate) const MAX_RTR_SOLICITATIONS: u8 = 3;

/// Minimum duration between router solicitation messages as defined in
/// [RFC 4861 section 10].
///
/// [RFC 4861 section 10]: https://tools.ietf.org/html/rfc4861#section-10
const RTR_SOLICITATION_INTERVAL: Duration = Duration::from_secs(4);

/// Amount of time to wait after sending `MAX_RTR_SOLICITATIONS` Router
/// Solicitation messages before determining that there are no routers on
/// the link for the purpose of IPv6 Stateless Address Autoconfiguration
/// if no Router Advertisement messages have been received as defined in
/// [RFC 4861 section 10].
///
/// This parameter is also used when a host sends its initial Router
/// Solicitation message, as per [RFC 4861 section 6.3.7]. Before a node
/// sends an initial solicitation, it SHOULD delay the transmission for
/// a random amount of time between 0 and `MAX_RTR_SOLICITATION_DELAY`.
/// This serves to alleviate congestion when many hosts start up on a
/// link at the same time.
///
/// [RFC 4861 section 10]: https://tools.ietf.org/html/rfc4861#section-10
/// [RFC 4861 section 6.3.7]: https://tools.ietf.org/html/rfc4861#section-6.3.7
const MAX_RTR_SOLICITATION_DELAY: Duration = Duration::from_secs(1);

//
// Router Constants
//

/// The number of initial Router Advertisements sent from a device when it transitions into an
/// advertising interface where the randomly chosen delay before sending the next Router
/// Advertisement SHOULD be saturated at `MAX_INITIAL_RTR_ADVERT_INTERVAL`.
///
/// See [RFC 4861 section 6.3.4] and [RFC 4861 section 10] for more details.
///
/// [RFC 4861 section 10]: https://tools.ietf.org/html/rfc4861#section-10
/// [RFC 4861 section 6.3.4]: https://tools.ietf.org/html/rfc4861#section-6.3.4
const MAX_INITIAL_RTR_ADVERTISEMENTS: u64 = 3;

/// The maximum delay before sending the next Router Advertisement for the first
/// `MAX_INITIAL_RTR_ADVERTISEMENTS` Router Advertisements when an interface transitions into an
/// advertising interface.
///
/// See [RFC 4861 section 6.3.4] and [RFC 4861 section 10] for more details.
///
/// [RFC 4861 section 10]: https://tools.ietf.org/html/rfc4861#section-10
/// [RFC 4861 section 6.3.4]: https://tools.ietf.org/html/rfc4861#section-6.3.4
const MAX_INITIAL_RTR_ADVERT_INTERVAL: Duration = Duration::from_secs(16);

/// The number of final Router Advertisements sent from a device when it transitions into a
/// non-advertising interface. The Router Lifetime field in these Router Advertisements will
/// be set to 0 to let hosts know not to use the router as a default router.
///
/// See See [RFC 4861 section 6.2.5] and [RFC 4861 section 10] for more details.
///
/// [RFC 4861 section 10]: https://tools.ietf.org/html/rfc4861#section-10
/// [RFC 4861 section 6.3.5]: https://tools.ietf.org/html/rfc4861#section-6.2.5
// This call to `new_unchecked` is okay becase 3 is non-zero, so we will not be violating
// `NonZeroU8`'s contract.
const MAX_FINAL_RTR_ADVERTISEMENTS: Option<NonZeroU8> =
    unsafe { Some(NonZeroU8::new_unchecked(3)) };

/// The minimum delay for sending a Router Advertisement message in response to a Router
/// Solicitation.
const MIN_RA_DELAY_TIME: Duration = Duration::from_millis(1);

/// The maximum delay for sending a Router Advertisement message in response to a Router
/// Solicitation.
///
/// See [RFC 4861 section 6.2.6] and [RFC 4861 section 10] for more details.
///
/// [RFC 4861 section 6.2.6]: https://tools.ietf.org/html/rfc4861#section-6.2.6
/// [RFC 4861 section 10]: https://tools.ietf.org/html/rfc4861#section-10
const MAX_RA_DELAY_TIME: Duration = Duration::from_millis(500);

/// The minimum delay between Router Advertisements sent to the IPv6 all nodes multicast address.
///
/// See [RFC 4861 section 6.2.6] and [RFC 4861 section 10] for more details.
///
/// [RFC 4861 section 6.2.6]: https://tools.ietf.org/html/rfc4861#section-6.2.6
/// [RFC 4861 section 10]: https://tools.ietf.org/html/rfc4861#section-10
const MIN_DELAY_BETWEEN_RAS: Duration = Duration::from_secs(3);

/// A link layer address that can be discovered using NDP.
pub(crate) trait LinkLayerAddress: Copy + Clone + Debug + PartialEq {
    /// The length, in bytes, expected for the `LinkLayerAddress`
    const BYTES_LENGTH: usize;

    /// Returns the underlying bytes of a `LinkLayerAddress`
    fn bytes(&self) -> &[u8];
    /// Attempts to construct a `LinkLayerAddress` from the provided bytes.
    ///
    /// `bytes` is guaranteed to be **exactly** `BYTES_LENGTH` long.
    fn from_bytes(bytes: &[u8]) -> Self;
}

/// A device layer protocol which can support NDP.
///
/// An `NdpDevice` is a device layer protocol which can support NDP.
pub(crate) trait NdpDevice: Sized {
    /// The link-layer address type used by this device.
    type LinkAddress: LinkLayerAddress + for<'a> From<&'a MulticastAddr<Ipv6Addr>>;

    /// Get a reference to a device's NDP state.
    fn get_ndp_state<D: EventDispatcher>(
        state: &StackState<D>,
        device_id: usize,
    ) -> &NdpState<Self, D>;

    /// Get a mutable reference to a device's NDP state.
    fn get_ndp_state_mut<D: EventDispatcher>(
        state: &mut StackState<D>,
        device_id: usize,
    ) -> &mut NdpState<Self, D>;

    /// Get the link layer address for a device.
    fn get_link_layer_addr<D: EventDispatcher>(
        state: &StackState<D>,
        device_id: usize,
    ) -> Self::LinkAddress;

    /// Get the link-local address for a device.
    ///
    /// If no link-local address is assigned for `device_id`, `None` will be returned. Otherwise,
    /// a `Some(a)` will be returned.
    fn get_link_local_addr<D: EventDispatcher>(
        state: &StackState<D>,
        device_id: usize,
    ) -> Option<Tentative<Ipv6Addr>>;

    /// Get a non-tentative IPv6 address for this device.
    ///
    /// Any **unicast** IPv6 address is a valid return value. Violating this
    /// rule may result in incorrect IP packets being sent.
    ///
    /// `get_ipv6_addr` may return a non-tentative link-local address if `device_id` deosn't have
    /// any non-tentative global addresses.
    fn get_ipv6_addr<D: EventDispatcher>(
        state: &StackState<D>,
        device_id: usize,
    ) -> Option<Ipv6Addr>;

    /// Returns the state of `address` on the device identified
    /// by `device_id` if it exists.
    ///
    /// `address` is guaranteed to be a valid unicast address.
    fn ipv6_addr_state<D: EventDispatcher>(
        state: &StackState<D>,
        device_id: usize,
        address: &Ipv6Addr,
    ) -> Option<AddressState>;

    /// Send a packet in a device layer frame.
    ///
    /// `send_ipv6_frame` accepts a device ID, a next hop IP address, and a
    /// `Serializer`. Implementers must resolve the destination link-layer
    /// address from the provided `next_hop` IPv6 address.
    ///
    /// # Panics
    ///
    /// May panic if `device_id` is not intialized. See [`crate::device::initialize_device`]
    /// for more information.
    fn send_ipv6_frame<D: EventDispatcher, S: Serializer<Buffer = EmptyBuf>>(
        ctx: &mut Context<D>,
        device_id: usize,
        next_hop: Ipv6Addr,
        body: S,
    ) -> Result<(), S>;

    /// Retrieves the complete `DeviceId` for a given `id`.
    fn get_device_id(id: usize) -> DeviceId;

    /// Notifies device layer that the link-layer address for the neighbor in
    /// `address` has been resolved to `link_address`.
    ///
    /// Implementers may use this signal to dispatch any packets that
    /// were queued waiting for address resolution.
    fn address_resolved<D: EventDispatcher>(
        ctx: &mut Context<D>,
        device_id: usize,
        address: &Ipv6Addr,
        link_address: Self::LinkAddress,
    );

    /// Notifies the device layer that the link-layer address resolution for
    /// the neighbor in `address` failed.
    fn address_resolution_failed<D: EventDispatcher>(
        ctx: &mut Context<D>,
        device_id: usize,
        address: &Ipv6Addr,
    );

    /// Notifies the device layer that a duplicate address has been detected. The
    /// device should want to remove the address.
    fn duplicate_address_detected<D: EventDispatcher>(
        ctx: &mut Context<D>,
        device_id: usize,
        addr: Ipv6Addr,
    );

    /// Notifies the device layer that the address is very likely (because DAD
    /// is not reliable) to be unique, it is time to mark it to be permanent.
    ///
    /// # Panics
    ///
    /// Panics if `addr` is not tentative on the devide identified by `device_id`.
    fn unique_address_determined<D: EventDispatcher>(
        state: &mut StackState<D>,
        device_id: usize,
        addr: Ipv6Addr,
    );

    /// Set Link MTU.
    ///
    /// `set_mtu` is used when a host receives a Router Advertisement with the MTU option.
    ///
    /// `set_mtu` MAY set the device's new MTU to a value less than `mtu` if the device does not
    /// support using `mtu` as its new MTU. `set_mtu` MUST NOT use a new MTU value that is greater
    /// than `mtu`.
    ///
    /// See [RFC 4861 section 6.3.4] for more information.
    ///
    /// # Panics
    ///
    /// `set_mtu` is allowed to panic if `mtu` is less than the minimum IPv6 MTU, [`IPV6_MIN_MTU`].
    ///
    /// [`IPV6_MIN_MTU`]: crate::ip::path_mtu::IPV6_MIN_MTU
    /// [RFC 4861 section 6.3.4]: https://tools.ietf.org/html/rfc4861#section-6.3.4
    fn set_mtu<D: EventDispatcher>(ctx: &mut StackState<D>, device_id: usize, mtu: u32);

    /// Set default hop limit for IP packets sent from `device_id`.
    ///
    /// # Panics
    ///
    /// Panics if the new hop limit is `0`.
    fn set_hop_limit<D: EventDispatcher>(
        state: &mut StackState<D>,
        device_id: usize,
        hop_limit: NonZeroU8,
    );

    /// Can `device_id` route IP packets not destined for it?
    ///
    /// If `is_router` returns `true`, we know that both the `device_id` and the netstack (`ctx`)
    /// have routing enabled; if `is_router` returns false, either `device_id` or the netstack
    /// (`ctx`) has routing disabled.
    fn is_router<D: EventDispatcher>(ctx: &Context<D>, device_id: usize) -> bool;

    /// Is `device_id` an advertising interface?
    fn is_advertising_interface<D: EventDispatcher>(ctx: &Context<D>, device_id: usize) -> bool {
        Self::is_router(ctx, device_id)
            && Self::get_ndp_state(ctx.state(), device_id)
                .configs
                .get_router_configurations()
                .get_should_send_advertisements()
    }

    /// Handle the case when a link-local address is resolved.
    ///
    /// `link_local_resolved` will start sending periodic router advertisements if `device_id` is
    /// configured to be an advertising interface.
    fn link_local_resolved<D: EventDispatcher>(ctx: &mut Context<D>, device_id: usize) {
        trace!(
            "link_local_resolved: link-local address on device {:?} resolved",
            Self::get_device_id(device_id)
        );

        if Self::is_router(ctx, device_id) {
            // If the device is operating as a router, and it is configured to be an advertising
            // interface, start sending periodic router advertisements.
            if Self::get_ndp_state(ctx.state(), device_id)
                .configs
                .get_router_configurations()
                .get_should_send_advertisements()
            {
                // At this point, we know that `devie_id` is an advertising interface and
                // has an assigned link-local address. Given this, we know that
                // `start_advertising_interface` will not panic.
                start_advertising_interface::<_, Self>(ctx, device_id);
            }
        }
    }

    /// Notifies the device layer that the address is very likely (because DAD
    /// is not reliable) to be unique, it is time to mark it to be permanent.
    ///
    /// If the address was the link-local address, periodic router advertisements
    /// will be started if `device_id` is an advertising interface.
    ///
    /// # Panics
    ///
    /// Panics if `addr` is not tentative on the devide identified by `device_id`.
    fn unique_address_determined_wrapper<D: EventDispatcher>(
        ctx: &mut Context<D>,
        device_id: usize,
        addr: Ipv6Addr,
    ) {
        // Let the device-layer know that `addr` is most likely not already used
        // on the link.
        Self::unique_address_determined(ctx.state_mut(), device_id, addr);

        if addr.is_linklocal() {
            // Here know know that we just resolved a link-local address becuase
            // we just checked that `addr` is link-local and we know that
            // `unique_address_determined` would have paniced if `addr` was not
            // tentative on `device_id`.
            Self::link_local_resolved(ctx, device_id);
        }
    }
}

/// Cleans up state associated with the device.
///
/// The contract is that after deinitialize is called, nothing else should be done
/// with the state.
pub(crate) fn deinitialize<D: EventDispatcher>(ctx: &mut Context<D>, device_id: usize) {
    // Remove all timers associated with the device
    // TODO(rheacock): this logic can be removed when NDP becomes contextified.
    ctx.dispatcher_mut().cancel_timeouts_with(|timer_id| match timer_id {
        TimerId(TimerIdInner::DeviceLayer(DeviceLayerTimerId::Ndp(inner_id))) => {
            let timer_device_id = inner_id.get_device_id();
            (timer_device_id.protocol == DeviceProtocol::Ethernet)
                && (timer_device_id.id == device_id)
        }
        _ => false,
    });
    // TODO(rheacock): Send any immediate packets, and potentially flag the state as uninitialized?
}

/// Per interface configurations for NDP.
#[derive(Debug, Clone)]
pub struct NdpConfigurations {
    /// Value for NDP's DUP_ADDR_DETECT_TRANSMITS parameter.
    ///
    /// As per [RFC 4862 section 5.1], the DUP_ADDR_DETECT_TRANSMITS
    /// is configurable per interface.
    ///
    /// A value of `None` means DAD will not be performed on the interface.
    ///
    /// Default: [`DUP_ADDR_DETECT_TRANSMITS`].
    ///
    /// [RFC 4862 section 5.1]: https://tools.ietf.org/html/rfc4862#section-5.1
    dup_addr_detect_transmits: Option<NonZeroU8>,

    /// Value for NDP's MAX_RTR_SOLICITATIONS parameter to configure
    /// how many router solicitation messages to send on interface enable.
    ///
    /// As per [RFC 4861 section 6.3.7], a host SHOULD transmit up to
    /// `MAX_RTR_SOLICITATIONS` Router Solicitation messages. Given the
    /// RFC does not require us to send `MAX_RTR_SOLICITATIONS` messages,
    /// we allow a configurable value, up to `MAX_RTR_SOLICITATIONS`.
    ///
    /// Default: [`MAX_RTR_SOLICITATIONS`].
    max_router_solicitations: Option<NonZeroU8>,

    /// Interface specific router configurations used by NDP.
    ///
    /// See [`NdpRouterConfigurations`] for more details.
    router_configurations: NdpRouterConfigurations,
}

impl Default for NdpConfigurations {
    fn default() -> Self {
        Self {
            dup_addr_detect_transmits: NonZeroU8::new(DUP_ADDR_DETECT_TRANSMITS),
            max_router_solicitations: NonZeroU8::new(MAX_RTR_SOLICITATIONS),
            router_configurations: NdpRouterConfigurations::default(),
        }
    }
}

impl NdpConfigurations {
    /// Get the value for NDP's DUP_ADDR_DETECT_TRANSMITS parameter.
    pub fn get_dup_addr_detect_transmits(&self) -> Option<NonZeroU8> {
        self.dup_addr_detect_transmits
    }

    /// Set the value for NDP's DUP_ADDR_DETECT_TRANSMITS parameter.
    ///
    /// A value of `None` means DAD will not be performed on the interface.
    pub fn set_dup_addr_detect_transmits(&mut self, v: Option<NonZeroU8>) {
        self.dup_addr_detect_transmits = v;
    }

    /// Get the value for NDP's MAX_RTR_SOLICITATIONS parameter.
    pub fn get_max_router_solicitations(&self) -> Option<NonZeroU8> {
        self.max_router_solicitations
    }

    /// Set the value for NDP's MAX_RTR_SOLICITATIONS parameter.
    ///
    /// A value of `None` means no router solicitations will be sent.
    /// `MAX_RTR_SOLICITATIONS` is the maximum possible value; values
    /// will be saturated at `MAX_RTR_SOLICITATIONS`.
    pub fn set_max_router_solicitations(&mut self, mut v: Option<NonZeroU8>) {
        if let Some(inner) = v {
            if inner.get() > MAX_RTR_SOLICITATIONS {
                v = NonZeroU8::new(MAX_RTR_SOLICITATIONS);
            }
        }

        self.max_router_solicitations = v;
    }

    /// Get the router configurations used by NDP.
    pub fn get_router_configurations(&self) -> &NdpRouterConfigurations {
        &self.router_configurations
    }

    /// Set the router configurations used by NDP.
    ///
    /// Note, unless the device is operating as a router (both netstack and the device has
    /// routing enabled (See [`crate::device::can_forward`]), these values will not be
    /// used. However, if a device or netstack configuration update occurs and the device
    /// ends up operating as a router, these values will be used for Router Advertisements.
    pub fn set_router_configurations(&mut self, v: NdpRouterConfigurations) {
        self.router_configurations = v;
    }
}

/// Interface specific router configurations used by NDP.
///
/// See [RFC 4861 section 6.2] for more information.
///
/// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
#[derive(Debug, Clone)]
pub struct NdpRouterConfigurations {
    /// A flag indicating whether or not the router sends periodic Router Advertisements and
    /// responds to Router Solicitations.
    ///
    /// Default: false.
    ///
    /// Note that AdvSendAdvertisements MUST be FALSE by default so that a node will not
    /// accidentally start acting as a default router. Nodes must be explicitly configured
    /// by system management to send Router Advertisements.
    ///
    /// See AdvSendAdvertisements in RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    should_send_advertisements: bool,

    /// The range of time allowed between sending unsolicited multicast Router Advertisements from
    /// the interface, in seconds.
    ///
    /// Maximum time MUST be no less than 4 seconds and no greater than 1800 seconds. Minimum time
    /// MUST be no less than 3 seconds and no greater than 0.75 * maximum time.
    ///
    /// Default: [200, 600].
    ///
    /// See MaxRtrAdvInterval and MinRtrAdvInterval in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    router_advertisements_interval: RangeInclusive<u16>,

    /// The value to be placed in the "Managed address configuration" flag field in the Router
    /// Advertisement.
    ///
    /// Default: false.
    ///
    /// See AdvManagedFlag in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    advertised_managed_flag: bool,

    /// The value to be placed in the "Other configuration" flag field in the Router Advertisement.
    ///
    /// Default: false.
    ///
    /// See AdvOtherConfigFlag in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    advertised_other_config_flag: bool,

    /// The value to be placed in the MTU options sent by the router. A value of `None` indicates
    /// that no MTU options are sent.
    ///
    /// Default: None.
    ///
    /// See AdvLinkMTU in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    advertised_link_mtu: Option<NonZeroU32>,

    /// The value to be placed in the Reachable Time field in the Router Advertisement messages sent
    /// by the router, in milliseconds. A value of 0 means unspecified (by this router).
    ///
    /// The value MUST be no greater than 3600000 milliseconds (1 hour).
    ///
    /// Default: 0.
    ///
    /// See AdvReachableTime in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    advertised_reachable_time: u32,

    /// The value to be placed in the Retrans Timer field in the Router Advertisement messages sent
    /// by the router. The value 0 means unspecified (by this router).
    ///
    /// Default: 0.
    ///
    /// See AdvRetransTimer in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    advertised_retransmit_timer: u32,

    /// The default value to be placed in the Cur Hop Limit field in the Router Advertisement
    /// messages sent by the router. The value should be set to the current diameter of the
    /// Internet.  The value zero means unspecified (by this router).
    ///
    /// Default: [`HOP_LIMIT_DEFAULT`].
    ///
    /// See AdvCurHopLimit in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    advertised_current_hop_limit: u8,

    /// The value to be placed in the Router Lifetime field of Router Advertisements sent from the
    /// interface, in seconds. MUST be either zero or between MaxRtrAdvInterval and 9000 seconds.
    /// A value of zero indicates that the router is not to be used as a default router.
    ///
    /// Default: 1800.
    ///
    /// See AdvDefaultLifetime in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    advertised_default_lifetime: Option<NonZeroU16>,

    /// A list of prefixes to be placed in Prefix Information options in Router Advertisement
    /// messages sent from the interface.
    ///
    /// Note, the link-local prefix SHOULD NOT be included in the list of advertised prefixes.
    ///
    /// See AdvPrefixList in in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    advertised_prefix_list: Vec<PrefixInformation>,
}

impl Default for NdpRouterConfigurations {
    fn default() -> Self {
        Self {
            should_send_advertisements: SHOULD_SEND_ADVERTISEMENTS_DEFAULT,
            router_advertisements_interval: ROUTER_ADVERTISEMENTS_INTERVAL_DEFAULT,
            advertised_managed_flag: ADVERTISED_MANAGED_FLAG_DEFAULT,
            advertised_other_config_flag: ADVERTISED_OTHER_CONFIG_FLAG,
            advertised_link_mtu: ADVERTISED_LINK_MTU,
            advertised_reachable_time: ADVERTISED_REACHABLE_TIME,
            advertised_retransmit_timer: ADVERTISED_RETRANSMIT_TIMER,
            advertised_current_hop_limit: ADVERTISED_CURRENT_HOP_LIMIT,
            advertised_default_lifetime: ADVERTISED_DEFAULT_LIFETIME,
            advertised_prefix_list: Vec::new(),
        }
    }
}

impl NdpRouterConfigurations {
    /// Create a new Router Advertisement from the configurations in this `NdpRouterConfigurations`.
    ///
    /// `is_final_ra_batch` is used to let `new_router_advertisement` know that the new Router
    /// Advertisement is part of a batch of final router advertisements when a device ceases to be
    /// an advertising interface. In this case, the Router Lifetime field will be set to 0 to inform
    /// hosts that the (ex-)router is no longer to be used as a default router.
    fn new_router_advertisement(&self, is_final_ra_batch: bool) -> RouterAdvertisement {
        let router_lifetime = if is_final_ra_batch {
            0
        } else {
            self.get_advertised_default_lifetime().map_or(0, |x| x.get())
        };

        RouterAdvertisement::new(
            self.get_advertised_current_hop_limit(),
            self.get_advertised_managed_flag(),
            self.get_advertised_other_config_flag(),
            router_lifetime,
            self.get_advertised_reachable_time(),
            self.get_advertised_retransmit_timer(),
        )
    }

    /// Get enable/disable status of sending periodic Router Advertisements and responding to Router
    /// Solicitations.
    pub fn get_should_send_advertisements(&self) -> bool {
        self.should_send_advertisements
    }

    /// Enable/disable sending periodic Router Advertisements and responding to Router
    /// Solicitations.
    ///
    /// See AdvSendAdvertisements in RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    pub fn set_should_send_advertisements(&mut self, v: bool) {
        self.should_send_advertisements = v;
    }

    /// Get the range of time allowed between sending unsolicited multicast Router Advertisements
    /// from the interface, in seconds.
    pub fn get_router_advertisements_interval(&self) -> RangeInclusive<u16> {
        self.router_advertisements_interval.clone()
    }

    /// Set the range of time allowed between sending unsolicited multicast Router Advertisements
    /// from the interface, in seconds.
    ///
    /// Maximum time MUST be no less than 4 seconds and no greater than 1800 seconds. Minimum time
    /// MUST be no less than 3 seconds and no greater than 0.75 * maximum time.
    ///
    /// If AdvDefaultLifetime is currently less than the new maximum time between sending
    /// unsolicited Router Advertisements, AdvDefaultLifetime will be updated to the new maximum
    /// time value.
    ///
    /// See MaxRtrAdvInterval and MinRtrAdvInterval in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    pub fn set_router_advertisements_interval(&mut self, v: RangeInclusive<u16>) {
        let start = *v.start();
        let end = *v.end();
        let start_upper_bound = (end * 3) / 4;

        if end < 4 {
            trace!("set_router_advertisements_interval: maximum time of {:?}s is less than 4s, ignoring", end);
            return;
        } else if end > 1800 {
            trace!("set_router_advertisements_interval: maximum time of {:?}s is greater than 1800s, ignoring", end);
            return;
        } else if start < 3 {
            trace!("set_router_advertisements_interval: minimum time of {:?}s is less than 3s, ignoring", start);
            return;
        } else if start > start_upper_bound {
            trace!("set_router_advertisements_interval: minimum time of {:?}s is greater than 0.75 * maximum time of {:?}s ( = {:?}s, ignoring", start, end, start_upper_bound);
            return;
        }

        if let Some(v) = self.advertised_default_lifetime {
            if v.get() < end {
                trace!("set_router_advertisements_interval: router_advertiements_interval of {:?} is less than new maximum router advertisements interval, setting to new max of {:?}", v.get(), end);
                self.advertised_default_lifetime = NonZeroU16::new(end);
            }
        }

        self.router_advertisements_interval = v;
    }

    /// Get the value to be placed in the "Managed address configuration" flag field in the Router
    /// Advertisement.
    pub fn get_advertised_managed_flag(&self) -> bool {
        self.advertised_managed_flag
    }

    /// Set the value to be placed in the "Managed address configuration" flag field in the Router
    /// Advertisement.
    ///
    /// See AdvManagedFlag in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    pub fn set_advertised_managed_flag(&mut self, v: bool) {
        self.advertised_managed_flag = v;
    }

    /// Get the value to be placed in the "Other configuration" flag field in the Router
    /// Advertisement.
    pub fn get_advertised_other_config_flag(&self) -> bool {
        self.advertised_other_config_flag
    }

    /// Set the value to be placed in the "Other configuration" flag field in the Router
    /// Advertisement.
    ///
    /// See AdvOtherConfigFlag in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    pub fn set_advertised_other_config_flag(&mut self, v: bool) {
        self.advertised_other_config_flag = v;
    }

    /// Get the value to be placed in the MTU options sent by the router. A value of `None`
    /// indicates that no MTU options are sent.
    pub fn get_advertised_link_mtu(&self) -> Option<NonZeroU32> {
        self.advertised_link_mtu
    }

    /// Set the value to be placed in the MTU options sent by the router. A value of `None`
    /// indicates that no MTU options are sent.
    ///
    /// See AdvLinkMTU in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    pub fn set_advertised_link_mtu(&mut self, v: Option<NonZeroU32>) {
        self.advertised_link_mtu = v;
    }

    /// Get the value to be placed in the Reachable Time field in the Router Advertisement messages
    /// sent by the router, in milliseconds. A value of 0 means unspecified (by this router).
    pub fn get_advertised_reachable_time(&self) -> u32 {
        self.advertised_reachable_time
    }

    /// Set the value to be placed in the Reachable Time field in the Router Advertisement messages
    /// sent by the router, in milliseconds. A value of 0 means unspecified (by this router).
    ///
    /// The value MUST be no greater than 3600000 milliseconds (1 hour).
    ///
    /// See AdvReachableTime in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    pub fn set_advertised_reachable_time(&mut self, v: u32) {
        if v > 3600000 {
            trace!("set_advertised_reachable_time: value of {:?} greater than 3600000ms (1hr), ignoring", v);
            return;
        }

        self.advertised_reachable_time = v;
    }

    /// Get the value to be placed in the Retrans Timer field in the Router Advertisement messages
    /// sent by the router. The value 0 means unspecified (by this router).
    pub fn get_advertised_retransmit_timer(&self) -> u32 {
        self.advertised_retransmit_timer
    }

    /// Set the value to be placed in the Retrans Timer field in the Router Advertisement messages
    /// sent by the router. The value 0 means unspecified (by this router).
    ///
    /// See AdvRetransTimer in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    pub fn set_advertised_retransmit_timer(&mut self, v: u32) {
        self.advertised_retransmit_timer = v;
    }

    /// Get the default value to be placed in the Cur Hop Limit field in the Router Advertisement
    /// messages sent by the router. The value should be set to the current diameter of the
    /// Internet.  The value zero means unspecified (by this router).
    pub fn get_advertised_current_hop_limit(&self) -> u8 {
        self.advertised_current_hop_limit
    }

    /// Set the default value to be placed in the Cur Hop Limit field in the Router Advertisement
    /// messages sent by the router. The value should be set to the current diameter of the
    /// Internet.  The value zero means unspecified (by this router).
    ///
    /// See AdvCurHopLimit in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    pub fn set_advertised_current_hop_limit(&mut self, v: u8) {
        self.advertised_current_hop_limit = v;
    }

    /// Get the value to be placed in the Router Lifetime field of Router Advertisements sent from
    /// the interface, in seconds. MUST be either zero or between MaxRtrAdvInterval and 9000
    /// seconds. A value of zero indicates that the router is not to be used as a default router.
    pub fn get_advertised_default_lifetime(&self) -> Option<NonZeroU16> {
        self.advertised_default_lifetime
    }

    /// Set the value to be placed in the Router Lifetime field of Router Advertisements sent from
    /// the interface, in seconds. MUST be either zero or between MaxRtrAdvInterval and 9000
    /// seconds. A value of zero indicates that the router is not to be used as a default router.
    ///
    /// See AdvDefaultLifetime in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    pub fn set_advertised_default_lifetime(&mut self, v: Option<NonZeroU16>) {
        if let Some(v) = v {
            let v = v.get();

            let lower_bound = *self.router_advertisements_interval.end();

            if v < lower_bound {
                trace!("set_advertised_default_lifetime: value of {:?} less than MaxRtrAdvInterval of {:?}, ignoring", v, lower_bound);
                return;
            } else if v > 9000 {
                trace!(
                    "set_advertised_default_lifetime: value of {:?} less than 9000s, ignoring",
                    v
                );
                return;
            }
        }

        self.advertised_default_lifetime = v;
    }

    /// Get the list of prefixes to be placed in Prefix Information options in Router Advertisement
    /// messages sent from the interface.
    pub fn get_advertised_prefix_list(&self) -> &Vec<PrefixInformation> {
        &self.advertised_prefix_list
    }

    /// Set the list of prefixes to be placed in Prefix Information options in Router Advertisement
    /// messages sent from the interface.
    ///
    /// Note, the link-local prefix SHOULD NOT be included in the list of advertised prefixes.
    ///
    /// See AdvPrefixList in in [RFC 4861 section 6.2] for more information.
    ///
    /// [RFC 4861 section 6.2]: https://tools.ietf.org/html/rfc4861#section-6.2
    pub fn set_advertised_prefix_list(&mut self, v: Vec<PrefixInformation>) {
        // TODO(ghanan): Check for duplicates and link local prefixes.
        self.advertised_prefix_list = v;
    }
}

/// The state associated with an instance of the Neighbor Discovery Protocol
/// (NDP).
///
/// Each device will contain an `NdpState` object to keep track of discovery
/// operations.
pub(crate) struct NdpState<ND: NdpDevice, D: EventDispatcher> {
    //
    // NDP operation data structures.
    //
    /// List of neighbors.
    neighbors: NeighborTable<ND::LinkAddress>,

    /// List of default routers, indexed by their link-local address.
    default_routers: HashSet<LinkLocalAddr<Ipv6Addr>>,

    /// List of on-link prefixes.
    on_link_prefixes: HashSet<AddrSubnet<Ipv6Addr>>,

    /// Number of Neighbor Solicitation messages left to send before we can
    /// assume that an IPv6 address is not currently in use.
    dad_transmits_remaining: HashMap<Ipv6Addr, u8>,

    /// Number of remaining Router Solicitation messages to send.
    router_solicitations_remaining: u8,

    /// Number of remaining final Router Advertisement messages to send.
    ///
    /// The Router Lifetime field in these Router Advertisement message will be set to 0 to let
    /// hosts know not to use the router as a default route.
    final_router_advertisements_remaining: u8,

    /// Number of Router Advertisements sent to the IPv6 all-nodes multicast address.
    ///
    /// Note, this value does not include Router Advertisements sent directly to a host (response to
    /// a Router Solicitation message).
    all_nodes_transmited_router_advertisements: u64,

    /// Instant when the last Router Advertisement to the all-nodes multicast address was sent.
    last_router_advertisement_instant: Option<D::Instant>,

    //
    // Interace parameters learned from Router Advertisements.
    //
    // See RFC 4861 section 6.3.2.
    //
    /// A base value used for computing the random `reachable_time` value.
    ///
    /// Default: `REACHABLE_TIME_DEFAULT`.
    ///
    /// See BaseReachableTime in [RFC 4861 section 6.3.2] for more details.
    ///
    /// [RFC 4861 section 6.3.2]: https://tools.ietf.org/html/rfc4861#section-6.3.2
    base_reachable_time: Duration,

    /// The time a neighbor is considered reachable after receiving a
    /// reachability confirmation.
    ///
    /// This value should be uniformly distributed between MIN_RANDOM_FACTOR (0.5)
    /// and MAX_RANDOM_FACTOR (1.5) times `base_reachable_time` milliseconds. A new
    /// random should be calculated when `base_reachable_time` changes (due to Router
    /// Advertisements) or at least every few hours even if no Router Advertisements
    /// are received.
    ///
    /// See ReachableTime in [RFC 4861 section 6.3.2] for more details.
    ///
    /// [RFC 4861 section 6.3.2]: https://tools.ietf.org/html/rfc4861#section-6.3.2
    reachable_time: Duration,

    /// The time between retransmissions of Neighbor Solicitation messages to
    /// a neighbor when resolving the address or when probing the reachability
    /// of a neighbor.
    ///
    /// Default: `RETRANS_TIMER_DEFAULT`.
    ///
    /// See RetransTimer in [RFC 4861 section 6.3.2] for more details.
    ///
    /// [RFC 4861 section 6.3.2]: https://tools.ietf.org/html/rfc4861#section-6.3.2
    retrans_timer: Duration,

    //
    // NDP configurations.
    //
    /// NDP Configurations.
    configs: NdpConfigurations,
}

impl<ND: NdpDevice, D: EventDispatcher> NdpState<ND, D> {
    pub(crate) fn new(configs: NdpConfigurations) -> Self {
        let mut ret = Self {
            neighbors: NeighborTable::default(),
            default_routers: HashSet::new(),
            dad_transmits_remaining: HashMap::new(),
            on_link_prefixes: HashSet::new(),
            router_solicitations_remaining: 0,
            final_router_advertisements_remaining: 0,
            all_nodes_transmited_router_advertisements: 0,
            last_router_advertisement_instant: None,

            base_reachable_time: REACHABLE_TIME_DEFAULT,
            reachable_time: REACHABLE_TIME_DEFAULT,
            retrans_timer: RETRANS_TIMER_DEFAULT,
            configs,
        };

        // Calculate an actually random `reachable_time` value instead of using
        // a constant.
        ret.recalculate_reachable_time();

        ret
    }

    //
    // NDP operation data structure helpers.
    //

    /// Do we know about the default router identified by `ip`?
    fn has_default_router(&self, ip: &LinkLocalAddr<Ipv6Addr>) -> bool {
        self.default_routers.contains(&ip)
    }

    /// Adds a new router to our list of default routers.
    fn add_default_router(&mut self, ip: LinkLocalAddr<Ipv6Addr>) {
        // Router must not already exist if we are adding it.
        assert!(self.default_routers.insert(ip));
    }

    /// Removes a router from our list of default routers.
    fn remove_default_router(&mut self, ip: &LinkLocalAddr<Ipv6Addr>) {
        // Router must exist if we are removing it.
        assert!(self.default_routers.remove(&ip));
    }

    /// Handle the invalidation of a default router.
    ///
    /// # Panics
    ///
    /// Panics if the router has not yet been discovered.
    fn invalidate_default_router(&mut self, ip: &LinkLocalAddr<Ipv6Addr>) {
        // As per RFC 4861 section 6.3.5:
        // Whenever the Lifetime of an entry in the Default Router List expires,
        // that entry is discarded.  When removing a router from the Default
        // Router list, the node MUST update the Destination Cache in such a way
        // that all entries using the router perform next-hop determination
        // again rather than continue sending traffic to the (deleted) router.

        self.remove_default_router(ip);

        // If a neighbor entry exists for the router, unmark it as a router.
        if let Some(state) = self.neighbors.get_neighbor_state_mut(&ip.get()) {
            state.is_router = false;
        }
    }

    /// Do we already know about this prefix?
    fn has_prefix(&self, addr_sub: &AddrSubnet<Ipv6Addr>) -> bool {
        self.on_link_prefixes.contains(addr_sub)
    }

    /// Adds a new prefix to our list of on-link prefixes.
    ///
    /// # Panics
    ///
    /// Panics if the prefix already exists in our list of on-link
    /// prefixes.
    fn add_prefix(&mut self, addr_sub: AddrSubnet<Ipv6Addr>) {
        assert!(self.on_link_prefixes.insert(addr_sub));
    }

    /// Removes a prefix from our list of on-link prefixes.
    ///
    /// # Panics
    ///
    /// Panics if the prefix doesn't exist in our list of on-link
    /// prefixes.
    fn remove_prefix(&mut self, addr_sub: &AddrSubnet<Ipv6Addr>) {
        assert!(self.on_link_prefixes.remove(addr_sub));
    }

    /// Handle the invalidation of a prefix.
    ///
    /// # Panics
    ///
    /// Panics if the prefix doesn't exist in our list of on-link
    /// prefixes.
    fn invalidate_prefix(&mut self, addr_sub: AddrSubnet<Ipv6Addr>) {
        // As per RFC 4861 section 6.3.5:
        // Whenever the invalidation timer expires for a Prefix List entry, that
        // entry is discarded. No existing Destination Cache entries need be
        // updated, however. Should a reachability problem arise with an
        // existing Neighbor Cache entry, Neighbor Unreachability Detection will
        // perform any needed recovery.

        self.remove_prefix(&addr_sub);
    }

    //
    // Interace parameters learned from Router Advertisements.
    //

    /// Set the base value used for computing the random `reachable_time` value.
    ///
    /// This method will also recalculate the `reachable_time` if the new base value
    /// is different from the current value. If the new base value is the same as
    /// the current value, `set_base_reachable_time` does nothing.
    pub(crate) fn set_base_reachable_time(&mut self, v: Duration) {
        assert_ne!(Duration::new(0, 0), v);

        if self.base_reachable_time == v {
            return;
        }

        self.base_reachable_time = v;

        self.recalculate_reachable_time();
    }

    /// Recalculate `reachable_time`.
    ///
    /// The new `reachable_time` will be a random value between a factor of
    /// MIN_RANDOM_FACTOR and MAX_RANDOM_FACTOR, as per [RFC 4861 section 6.3.2].
    ///
    /// [RFC 4861 section 6.3.2]: https://tools.ietf.org/html/rfc4861#section-6.3.2
    pub(crate) fn recalculate_reachable_time(&mut self) -> Duration {
        let base = self.base_reachable_time;
        let half = base / 2;
        let reachable_time = half + thread_rng().gen_range(Duration::new(0, 0), base);

        // Random value must between a factor of MIN_RANDOM_FACTOR (0.5) and
        // MAX_RANDOM_FACTOR (1.5), as per RFC 4861 section 6.3.2.
        assert!((reachable_time >= half) && (reachable_time <= (base + half)));

        self.reachable_time = reachable_time;
        reachable_time
    }

    /// Set the time between retransmissions of Neighbor Solicitation messages to
    /// a neighbor when resolving the address or when probing the reachability of
    /// a neighbor.
    pub(crate) fn set_retrans_timer(&mut self, v: Duration) {
        assert_ne!(Duration::new(0, 0), v);

        self.retrans_timer = v;
    }

    //
    // NDP Configurations.
    //

    /// Set the number of Neighbor Solicitation messages to send when performing DAD.
    pub(crate) fn set_dad_transmits(&mut self, v: Option<NonZeroU8>) {
        self.configs.set_dup_addr_detect_transmits(v);
    }
}

/// The identifier for timer events in NDP operations.
#[derive(Copy, Clone, PartialEq, Eq, Debug, Hash)]
pub(crate) struct NdpTimerId {
    device_id: DeviceId,
    inner: InnerNdpTimerId,
}

/// The types of NDP timers.
#[derive(Copy, Clone, PartialEq, Eq, Debug, Hash)]
pub(crate) enum InnerNdpTimerId {
    /// This is used to retry sending Neighbor Discovery Protocol requests.
    LinkAddressResolution { neighbor_addr: Ipv6Addr },
    /// This is used to resend Duplicate Address Detection Neighbor Solicitation
    /// messages if `DUP_ADDR_DETECTION_TRANSMITS` is greater than one.
    DadNsTransmit { addr: Ipv6Addr },
    /// Timer to send Router Solicitation messages.
    RouterSolicitationTransmit,
    /// Timer to invalidate a router.
    /// `ip` is the identifying IP of the router.
    RouterInvalidation { ip: LinkLocalAddr<Ipv6Addr> },
    /// Timer to invalidate a prefix.
    PrefixInvalidation { addr_subnet: AddrSubnet<Ipv6Addr> },
    /// Timer to send a Router Advertisement.
    RouterAdvertisementTransmit,
    // TODO: The RFC suggests that we SHOULD make a random delay to
    // join the solicitation group. When we support MLD, we probably
    // want one for that.
}

impl NdpTimerId {
    /// Creates a new `NdpTimerId` wrapped inside a `TimerId` with the provided
    /// `device_id` and `neighbor_addr`.
    pub(crate) fn new_link_address_resolution_timer_id<ND: NdpDevice>(
        device_id: usize,
        neighbor_addr: Ipv6Addr,
    ) -> TimerId {
        NdpTimerId {
            device_id: ND::get_device_id(device_id),
            inner: InnerNdpTimerId::LinkAddressResolution { neighbor_addr },
        }
        .into()
    }

    pub(crate) fn new_dad_ns_transmission_timer_id<ND: NdpDevice>(
        device_id: usize,
        tentative_addr: Ipv6Addr,
    ) -> TimerId {
        NdpTimerId {
            device_id: ND::get_device_id(device_id),
            inner: InnerNdpTimerId::DadNsTransmit { addr: tentative_addr },
        }
        .into()
    }

    pub(crate) fn new_router_solicitation_timer_id<ND: NdpDevice>(device_id: usize) -> TimerId {
        NdpTimerId {
            device_id: ND::get_device_id(device_id),
            inner: InnerNdpTimerId::RouterSolicitationTransmit,
        }
        .into()
    }

    pub(crate) fn new_router_invalidation_timer_id<ND: NdpDevice>(
        device_id: usize,
        ip: LinkLocalAddr<Ipv6Addr>,
    ) -> TimerId {
        NdpTimerId {
            device_id: ND::get_device_id(device_id),
            inner: InnerNdpTimerId::RouterInvalidation { ip },
        }
        .into()
    }

    pub(crate) fn new_prefix_invalidation_timer_id<ND: NdpDevice>(
        device_id: usize,
        addr_subnet: AddrSubnet<Ipv6Addr>,
    ) -> TimerId {
        NdpTimerId {
            device_id: ND::get_device_id(device_id),
            inner: InnerNdpTimerId::PrefixInvalidation { addr_subnet },
        }
        .into()
    }

    pub(crate) fn new_router_advertisement_transmit_timer_id<ND: NdpDevice>(
        device_id: usize,
    ) -> TimerId {
        NdpTimerId {
            device_id: ND::get_device_id(device_id),
            inner: InnerNdpTimerId::RouterAdvertisementTransmit,
        }
        .into()
    }

    pub(crate) fn get_device_id(&self) -> DeviceId {
        self.device_id
    }
}

impl From<NdpTimerId> for TimerId {
    fn from(v: NdpTimerId) -> Self {
        TimerId(TimerIdInner::DeviceLayer(DeviceLayerTimerId::Ndp(v)))
    }
}

/// Handles a timeout event.
///
/// This currently only supports Ethernet NDP, since we know that that is
/// the only case that the netstack currently handles. In the future, this may
/// be extended to support other hardware types.
pub(crate) fn handle_timeout<D: EventDispatcher>(ctx: &mut Context<D>, id: NdpTimerId) {
    match id.device_id.protocol() {
        DeviceProtocol::Ethernet => {
            handle_timeout_inner::<_, EthernetNdpDevice>(ctx, id.device_id.id(), id.inner)
        }
    }
}

fn handle_timeout_inner<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
    inner_id: InnerNdpTimerId,
) {
    match inner_id {
        InnerNdpTimerId::LinkAddressResolution { neighbor_addr } => {
            let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);
            if let Some(NeighborState {
                state: NeighborEntryState::Incomplete { transmit_counter },
                ..
            }) = ndp_state.neighbors.get_neighbor_state_mut(&neighbor_addr)
            {
                if *transmit_counter < MAX_MULTICAST_SOLICIT {
                    let retrans_timer = ndp_state.retrans_timer;

                    // Increase the transmit counter and send the solicitation again
                    *transmit_counter += 1;
                    send_neighbor_solicitation::<_, ND>(ctx, device_id, neighbor_addr);
                    ctx.dispatcher.schedule_timeout(
                        retrans_timer,
                        NdpTimerId::new_link_address_resolution_timer_id::<ND>(
                            device_id,
                            neighbor_addr,
                        ),
                    );
                } else {
                    // To make sure we don't get stuck in this neighbor unreachable
                    // state forever, remove the neighbor from the database:
                    ndp_state.neighbors.delete_neighbor_state(&neighbor_addr);
                    increment_counter!(ctx, "ndp::neighbor_solicitation_timeout");

                    ND::address_resolution_failed(ctx, device_id, &neighbor_addr);
                }
            } else {
                unreachable!("handle_timeout_inner: timer for neighbor {:?} address resolution should not exist if no entry exists", neighbor_addr);
            }
        }
        InnerNdpTimerId::DadNsTransmit { addr } => {
            // Get device NDP state.
            //
            // We know this call to unwrap will not fail because we will only reach here
            // if DAD has been started for some device - address pair. When we start DAD,
            // we setup the `NdpState` so we should have a valid entry.
            let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);
            let remaining = *ndp_state.dad_transmits_remaining.get(&addr).unwrap();

            // We have finished.
            if remaining == 0 {
                // We know `unwrap` will not fail because we just succesfully
                // called `get` then `unwrap` earlier.
                ndp_state.dad_transmits_remaining.remove(&addr).unwrap();

                // `unique_address_determined` may panic if we attempt to resolve an `addr`
                // that is not tentative on the device with id `device_id`. However, we
                // can only reach here if `addr` was tentative on `device_id` and we are
                // performing DAD so we know `unique_address_determined` will not panic.
                ND::unique_address_determined_wrapper(ctx, device_id, addr);
            } else {
                do_duplicate_address_detection::<D, ND>(ctx, device_id, addr);
            }
        }
        InnerNdpTimerId::RouterSolicitationTransmit => {
            do_router_solicitation::<_, ND>(ctx, device_id)
        }
        InnerNdpTimerId::RouterInvalidation { ip } => {
            // Invalidate the router.
            //
            // The call to `invalidate_default_router` may panic if `ip` does not reference a
            // known default router, but we will only reach here if we received an NDP Router
            // Advertisement from a router with a valid lifetime > 0, at which point this timeout.
            // would have been set. Givem this, we know that `invalidate_default_router` will not
            // panic.
            ND::get_ndp_state_mut(ctx.state_mut(), device_id).invalidate_default_router(&ip)
        }
        InnerNdpTimerId::PrefixInvalidation { addr_subnet } => {
            // Invalidate the prefix.
            //
            // The call to `invalidate_prefix` may panic if `addr_subnet` is not in the
            // list of on-link prefixes. However, we will only reach here if we received
            // an NDP Router Advertisement with the prefix option with the on-link flag
            // set. Given this we know that `addr_subnet` must exist if this timer was
            // fired so `invalidate_prefix` will not panic.
            ND::get_ndp_state_mut(ctx.state_mut(), device_id).invalidate_prefix(addr_subnet);
        }
        InnerNdpTimerId::RouterAdvertisementTransmit => {
            // Send the router advertisement to the IPv6 all-nodes multicast address.
            send_router_advertisement::<_, ND>(ctx, device_id, Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS);

            // Schedule the next Router Advertisement if `device_id` is an advertising interface or
            // it is sending its final Router Advertisements, as per RFC 4861 section 6.2.5.
            if ND::is_advertising_interface(ctx, device_id)
                || ND::get_ndp_state(ctx.state(), device_id).final_router_advertisements_remaining
                    != 0
            {
                schedule_next_router_advertisement::<_, ND>(ctx, device_id);
            }
        }
    }
}

/// Updates the NDP Configurations for a `device_id`.
///
/// Note, some values may not take effect immediately, and may only take effect the next time they
/// are used. These scenarios documented below:
///
///  - Updates to [`NdpConfiguration::dup_addr_detect_transmits`] will only take effect the next
///    time Duplicate Address Detection (DAD) is done. Any DAD processes that have already started
///    will continue using the old value.
///
///  - Updates to [`NdpConfiguration::max_router_solicitations`] will only take effect the next
///    time routers are explicitly solicited. Current router solicitation will continue using the
///    old value.
pub(crate) fn set_ndp_configurations<D: EventDispatcher, ND: NdpDevice + 'static>(
    ctx: &mut Context<D>,
    device_id: usize,
    configs: NdpConfigurations,
) {
    let existing_configs = get_ndp_configurations::<_, ND>(ctx, device_id).clone();

    // Update the configurations.
    ND::get_ndp_state_mut(ctx.state_mut(), device_id).configs = configs;

    // If the device was not a router before, then it won't be a router after any NDP configuration
    // change so we only check router-specific configuration changes if the device is a router.
    //
    // We also check to make sure the device has a non-tentative link-local address because
    // if we didn't have a link-local address, we would not have started any router advertisements
    // to update.
    if ND::is_router(ctx, device_id)
        && ND::get_link_local_addr(ctx.state(), device_id)
            .map(|a| !a.is_tentative())
            .unwrap_or(false)
    {
        let old_rc = existing_configs.get_router_configurations();
        let new_rc = get_ndp_configurations::<_, ND>(ctx, device_id).get_router_configurations();

        //
        // Check if device changes advertising interfacee status (should/should't send Router
        // Advertisements).
        //

        if !old_rc.get_should_send_advertisements() && new_rc.get_should_send_advertisements() {
            // If the device is now an advertising interface, start sending Router
            // Advertisements.
            start_advertising_interface::<_, ND>(ctx, device_id);
        } else if old_rc.get_should_send_advertisements()
            && !new_rc.get_should_send_advertisements()
        {
            // If the device is now not an advertising interface, stop sending Router
            // Advertisements after the final batch of Router Advertisements with Router Lifetime
            // set to 0 (to inform hosts to not use this node as a default router).
            stop_advertising_interface::<_, ND>(ctx, device_id);
        }

        //
        // Check for a change in min/max interval between sending Router Advertisements.
        //
        // If the time from now to the instant when the next Router Advertisement is to be sent is
        // greater than the new MaxRtrAdvInterval, schedule a new Router Advertisement.
        //
        // Note, we intentionally ignore the case when the next Router Advertisement to be sent is
        // less than the new MinRtrAdvInterval to make sure we do not unintentionally wait too long
        // before sending the next Router Advertisement.
        //

        let old_interval = old_rc.get_router_advertisements_interval();
        let new_interval = get_ndp_configurations::<_, ND>(ctx, device_id)
            .get_router_configurations()
            .get_router_advertisements_interval();

        if old_interval != new_interval {
            // If a Router Advertisement is scheduled, make sure the time it will fire is not
            // more than MaxRtrAdvInterval from now.
            if let Some(next_instant) = ctx.dispatcher().scheduled_instant(
                NdpTimerId::new_router_advertisement_transmit_timer_id::<ND>(device_id),
            ) {
                let now = ctx.dispatcher().now();
                let max_instant = now
                    .checked_add(Duration::from_secs((*new_interval.end()).into()))
                    .expect("Failed to calculate new max delay before NDP RA transmission");

                if next_instant > max_instant {
                    // Next RA will be sent too far from now. Reschedule it.
                    schedule_next_router_advertisement::<_, ND>(ctx, device_id);
                }
            }
        }
    }
}

/// Gets the NDP Configurations for a `device_id`.
pub(crate) fn get_ndp_configurations<D: EventDispatcher, ND: NdpDevice + 'static>(
    ctx: &Context<D>,
    device_id: usize,
) -> &NdpConfigurations {
    &ND::get_ndp_state(ctx.state(), device_id).configs
}

/// Look up the link layer address.
///
/// Begins the address resolution process if the link layer address
/// for `lookup_addr` is not already known.
pub(crate) fn lookup<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
    lookup_addr: Ipv6Addr,
) -> Option<ND::LinkAddress> {
    trace!("ndp::lookup: {:?}", lookup_addr);

    // If `lookup_addr` is a multicast address, get the corresponding
    // destination multicast mac address.
    if let Some(multicast_addr) = MulticastAddr::new(lookup_addr) {
        return Some(ND::LinkAddress::from(&multicast_addr));
    }

    // TODO(brunodalbo): Figure out what to do if a frame can't be sent
    let ndpstate = ND::get_ndp_state_mut(ctx.state_mut(), device_id);
    let result = ndpstate.neighbors.get_neighbor_state(&lookup_addr);

    match result {
        // TODO(ghanan): As long as have ever received a link layer address for
        //               `lookup_addr` from any NDP packet with the source link
        //               layer option, we would have stored that address. Here
        //               we simply return that address without checking the
        //               actual state of the neighbor entry. We should make sure
        //               that the entry is not Stale before returning the address.
        //               If it is stale, we should make sure it is reachable first.
        //               See RFC 4861 section 7.3.2 for more information.
        Some(NeighborState { link_address: Some(address), .. }) => Some(*address),

        // We do not know about the neighbor and need to start address resolution.
        None => {
            trace!("ndp::lookup: starting address resolution process for {:?}", lookup_addr);

            let retrans_timer = ndpstate.retrans_timer;

            // If we're not already waiting for a neighbor solicitation
            // response, mark it as Incomplete and send a neighbor solicitation,
            // also setting the transmission count to 1.
            ndpstate.neighbors.add_incomplete_neighbor_state(lookup_addr);

            send_neighbor_solicitation::<_, ND>(ctx, device_id, lookup_addr);

            // Also schedule a timer to retransmit in case we don't get
            // neighbor advertisements back.
            ctx.dispatcher.schedule_timeout(
                retrans_timer,
                NdpTimerId::new_link_address_resolution_timer_id::<ND>(device_id, lookup_addr),
            );

            // Returning `None` as we do not have a link-layer address
            // to give yet.
            None
        }

        // Address resolution is currently in progress.
        Some(NeighborState { state: NeighborEntryState::Incomplete { .. }, .. }) => {
            trace!(
                "ndp::lookup: still waiting for address resolution to complete for {:?}",
                lookup_addr
            );
            None
        }

        // TODO(ghanan): Handle case where a neighbor entry exists for a `link_addr`
        //               but no link address as been discovered.
        _ => unimplemented!("A neighbor entry exists but no link address is discovered"),
    }
}

/// Insert a neighbor to the known neighbors table.
///
/// This method only gets called when testing to force a neighbor
/// so link address lookups completes immediately without doing
/// address resolution.
#[cfg(test)]
pub(crate) fn insert_neighbor<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
    net: Ipv6Addr,
    hw: ND::LinkAddress,
) {
    // Neighbor `net` should be marked as reachable.
    ND::get_ndp_state_mut(ctx.state_mut(), device_id).neighbors.set_link_address(net, hw, true)
}

/// `NeighborState` keeps all state that NDP may want to keep about neighbors,
/// like link address resolution and reachability information, for example.
struct NeighborState<H> {
    is_router: bool,
    state: NeighborEntryState,
    link_address: Option<H>,
}

impl<H> NeighborState<H> {
    fn new() -> Self {
        Self {
            is_router: false,
            state: NeighborEntryState::Incomplete { transmit_counter: 0 },
            link_address: None,
        }
    }

    /// Is the neighbor incomplete (waiting for address resolution)?
    fn is_incomplete(&self) -> bool {
        if let NeighborEntryState::Incomplete { .. } = self.state {
            true
        } else {
            false
        }
    }

    /// Is the neighbor reachable?
    fn is_reachable(&self) -> bool {
        self.state == NeighborEntryState::Reachable
    }
}

/// The various states a Neighbor cache entry can be in.
///
/// See [RFC 4861 section 7.3.2].
///
/// [RFC 4861 section 7.3.2]: https://tools.ietf.org/html/rfc4861#section-7.3.2
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
enum NeighborEntryState {
    /// Address resolution is being performed on the entry.
    /// Specifically, a Neighbor Solicitation has been sent to
    /// the solicited-node multicast address of the target,
    /// but the corresponding Neighbor Advertisement has not
    /// yet been received.
    ///
    /// `transmit_counter` is the count of Neighbor Solicitation
    /// messages sent as part of the Address resolution process.
    Incomplete { transmit_counter: u8 },

    /// Positive confirmation was received within the last
    /// ReachableTime milliseconds that the forward path to
    /// the neighbor was functioning properly.  While
    /// `Reachable`, no special action takes place as packets
    /// are sent.
    Reachable,

    /// More than ReachableTime milliseconds have elapsed
    /// since the last positive confirmation was received that
    /// the forward path was functioning properly.  While
    /// stale, no action takes place until a packet is sent.
    ///
    /// The `Stale` state is entered upon receiving an
    /// unsolicited Neighbor Discovery message that updates
    /// the cached link-layer address.  Receipt of such a
    /// message does not confirm reachability, and entering
    /// the `Stale` state ensures reachability is verified
    /// quickly if the entry is actually being used.  However,
    /// reachability is not actually verified until the entry
    /// is actually used.
    Stale,

    /// More than ReachableTime milliseconds have elapsed
    /// since the last positive confirmation was received that
    /// the forward path was functioning properly, and a
    /// packet was sent within the last DELAY_FIRST_PROBE_TIME
    /// seconds.  If no reachability confirmation is received
    /// within DELAY_FIRST_PROBE_TIME seconds of entering the
    /// DELAY state, send a Neighbor Solicitation and change
    /// the state to PROBE.
    ///
    /// The DELAY state is an optimization that gives upper-
    /// layer protocols additional time to provide
    /// reachability confirmation in those cases where
    /// ReachableTime milliseconds have passed since the last
    /// confirmation due to lack of recent traffic.  Without
    /// this optimization, the opening of a TCP connection
    /// after a traffic lull would initiate probes even though
    /// the subsequent three-way handshake would provide a
    /// reachability confirmation almost immediately.
    Delay,

    /// A reachability confirmation is actively sought by
    /// retransmitting Neighbor Solicitations every
    /// RetransTimer milliseconds until a reachability
    /// confirmation is received.
    Probe,
}

struct NeighborTable<H> {
    table: HashMap<Ipv6Addr, NeighborState<H>>,
}

impl<H: PartialEq + Debug> NeighborTable<H> {
    /// Sets the link address for a neighbor.
    ///
    /// If `is_reachable` is `true`, the state of the neighbor will be
    /// set to `NeighborEntryState::Reachable`. Otherwise, it will be
    /// set to `NeighborEntryState::Stale` if the address was updated.
    /// A `false` value for `is_reachable` does not mean that the
    /// neighbor is unreachable, it just means that we do not know if it
    /// is reachable.
    fn set_link_address(&mut self, neighbor: Ipv6Addr, address: H, is_reachable: bool) {
        let address = Some(address);
        let neighbor_state = self.table.entry(neighbor).or_insert_with(NeighborState::new);

        trace!("set_link_address: setting link address for neighbor {:?} to address", address);

        if is_reachable {
            trace!("set_link_address: reachability is known, so setting state for neighbor {:?} to Reachable", neighbor);

            neighbor_state.state = NeighborEntryState::Reachable;
        } else if neighbor_state.link_address != address {
            trace!("set_link_address: new link addr different from old and reachability is unknown, so setting state for neighbor {:?} to Stale", neighbor);

            neighbor_state.state = NeighborEntryState::Stale;
        }

        neighbor_state.link_address = address;
    }
}

impl<H> NeighborTable<H> {
    /// Create a new incomplete state of a neighbor, setting the transmit counter to 1.
    fn add_incomplete_neighbor_state(&mut self, neighbor: Ipv6Addr) {
        let mut state = NeighborState::new();
        state.state = NeighborEntryState::Incomplete { transmit_counter: 1 };

        self.table.insert(neighbor, state);
    }

    /// Get the neighbor's state, if it exists.
    fn get_neighbor_state(&self, neighbor: &Ipv6Addr) -> Option<&NeighborState<H>> {
        self.table.get(neighbor)
    }

    /// Get a  the neighbor's mutable state, if it exists.
    fn get_neighbor_state_mut(&mut self, neighbor: &Ipv6Addr) -> Option<&mut NeighborState<H>> {
        self.table.get_mut(neighbor)
    }

    /// Delete the neighbor's state, if it exists.
    fn delete_neighbor_state(&mut self, neighbor: &Ipv6Addr) {
        self.table.remove(neighbor);
    }
}

impl<H> Default for NeighborTable<H> {
    fn default() -> Self {
        NeighborTable { table: HashMap::default() }
    }
}

/// Handle `device_id` becoming an advertising interface.
///
/// # Panics
///
/// Panics if `device_id` does not have an assigned (non-tentative) link-local address or if it is
/// not configured to be an advertising interface.
pub(crate) fn start_advertising_interface<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
) {
    trace!(
        "start_advertising_interface: making device {:?} an advertising advertising interface",
        ND::get_device_id(device_id)
    );

    // Reset the number of final Router Advertisements to send before ending Router Advertisements.
    ND::get_ndp_state_mut(ctx.state_mut(), device_id).final_router_advertisements_remaining = 0;

    // Start sending periodic router advertisements. May panic if `device_id` does not have an
    // assigned (non-tentative) link-local address or if it is not configured to be an advertising
    // interface.
    start_periodic_router_advertisements::<_, ND>(ctx, device_id);
}

/// Handle `device_id` ceasing to be an advertising interface.
///
/// # Panics
///
/// Panics if `device_id` is not operating as an advertising interface.
pub(crate) fn stop_advertising_interface<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
) {
    trace!(
        "stop_advertising_interface: making device {:?} a non-advertising advertising interface",
        ND::get_device_id(device_id)
    );

    // Not having a scheduled router advertisement implies `device_id` is already not an
    // advertising interface.
    assert!(ctx
        .dispatcher()
        .scheduled_instant(NdpTimerId::new_router_advertisement_transmit_timer_id::<ND>(device_id))
        .is_some());

    let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);

    // If `final_router_advertisements_remaining` is non-zero, that means that `device_id`
    // already ceased to be an advertising interface, meaning it isn't an advertising interface.
    assert_eq!(ndp_state.final_router_advertisements_remaining, 0);

    if let Some(final_ras) = MAX_FINAL_RTR_ADVERTISEMENTS {
        let final_ras = final_ras.get();

        trace!(
            "stop_advertising_interface: device {:?} is configured to send {:?} final Router Advertisements",
            ND::get_device_id(device_id),
            final_ras,
        );

        // Set the number of final Router Advertisements. We do not cancel the periodic Router
        // Advertisements yet since we will use the same timers to send the final Router
        // Advertisement.
        ndp_state.final_router_advertisements_remaining = final_ras;
    } else {
        trace!(
            "stop_advertising_interface: device {:?} is not configured to send any final Router Advertisements, stopping periodic Router Advertisement transmissions",
            ND::get_device_id(device_id),
        );

        // Stop sending periodic Router Advertisements. Will panic if `device_id` is not currently
        // sending periodic Router Advertisements, implying it is not an advertising interface.
        stop_periodic_router_advertisements::<_, ND>(ctx, device_id);
    }
}

/// Start sending periodic router advertisements.
///
/// # Panics
///
/// Panics if `device_id` does not have an assigned (non-tentative) link-local address or if it is
/// not configured to be an advertising interface.
fn start_periodic_router_advertisements<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
) {
    trace!(
        "ndp::start_periodic_router_advertisements: start sending router advertisements from device: {:?}",
        ND::get_device_id(device_id)
    );

    // Reset the number of Router Advertisements transmited to the all-nodes multicast address.
    ND::get_ndp_state_mut(ctx.state_mut(), device_id).all_nodes_transmited_router_advertisements =
        0;

    schedule_next_router_advertisement::<_, ND>(ctx, device_id);
}

/// Stop sending periodic router advertisements.
///
/// # Panics
///
/// Panics if `device_id` is not currently sending periodic Router Advertisements.
fn stop_periodic_router_advertisements<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
) {
    // Cancel the next periodic router advertisement timeout.
    //
    // May panic if we are not currently scheduled to send a periodic router advertisements.
    ctx.dispatcher_mut()
        .cancel_timeout(NdpTimerId::new_router_advertisement_transmit_timer_id::<ND>(device_id))
        .unwrap();
}

/// Schedule next unsolicited Router Advertisement message.
///
/// `schedule_next_router_advertisement` will overwrite any existing scheduled Router Advertisement
/// transmissions.
///
/// # Panics
///
/// Panics if `device_id` is not operating as a router, if it is not configured to send Router
/// Advertisements, or if it does not have an assigned (non-tentative) link-local address.
pub(crate) fn schedule_next_router_advertisement<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
) {
    trace!(
        "ndp::schedule_next_router_advertisement: scheduling the next router advertisement for device {:?}",
        ND::get_device_id(device_id)
    );

    // Calculate a random time from the interface's MinRtrAdvInterval and MaxRtrAdvInterval, as per
    // RFC 4861 section 6.2.4.
    let tx_range = ND::get_ndp_state(ctx.state_mut(), device_id)
        .configs
        .get_router_configurations()
        .get_router_advertisements_interval();

    let mut delay = Duration::from_secs(
        ctx.dispatcher_mut().rng().gen_range(tx_range.start(), tx_range.end()).into(),
    );

    // As per RFC 6.2.4, for the first few advertisements (up to `MAX_INITIAL_RTR_ADVERTISEMENTS`)
    // sent from an interface when it becomes an advertising interface, if the randomly chosen
    // interval is greater than `MAX_INITIAL_RTR_ADVERT_INTERVAL`, the timer SHOULD be set to
    // `MAX_INITIAL_RTR_ADVERT_INTERVAL` instead.  Using a smaller interval for the initial
    // advertisements increases the likelihood of a router being discovered quickly when it first
    // becomes available, in the presence of possible packet loss.
    //
    // TODO(ghanan): Make the choice to limit the delay of the first few advertisements configurable
    let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);
    if (ndp_state.all_nodes_transmited_router_advertisements < MAX_INITIAL_RTR_ADVERTISEMENTS)
        && (delay > MAX_INITIAL_RTR_ADVERT_INTERVAL)
    {
        trace!("schedule_next_router_advertisement: still sending the initial batch of router advertisements so limiting delay to MAX_INITIAL_RTR_ADVERT_INTERVAL ({:?})", MAX_INITIAL_RTR_ADVERT_INTERVAL);

        delay = MAX_INITIAL_RTR_ADVERT_INTERVAL;
    }

    // Schedule the timout to send the router advertisement.
    let instant = ctx
        .dispatcher
        .now()
        .checked_add(delay)
        .expect("Failed to calculate next NDP RA transmit instant");
    schedule_next_router_advertisement_instant::<_, ND>(ctx, device_id, instant);
}

/// Schedule next unsolicited Router Advertisement message at a specific instant in time.
///
/// `schedule_next_router_advertisement_instant` will overwrite any existing scheduled Router
/// Advertisement transmissions if `instant` is before the next scheduled Router Advertisement.
/// If the next scheduled Router Advertisement is before `instant`,
/// `schedule_next_router_advertisement_instant` will not update the timer.
///
/// # Panics
///
/// Panics if `device_id` does not have an assigned (non-tentative) link-local address or if it is
/// not either an advertising interface or sending the final Router Advertisements after ceasing to
/// be an advertising interface.
fn schedule_next_router_advertisement_instant<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
    instant: D::Instant,
) {
    // If we are attempting to send a router advertisement, we need to have a valid
    // link-local address, as per RFC 4861 section 4.2. The call to either `unwrap` may
    // panic if `device_id` does not have an assigned (non-tentative) link-local address,
    // but this is documented for this method.
    let _our_link_local_addr =
        ND::get_link_local_addr(ctx.state(), device_id).unwrap().try_into_permanent().unwrap();

    // Device MUST be in one of two scenarios if we are trying to schedule a Router Advertisement.
    // 1) Is an advertising interface.
    // 2) Sending the final Router Advertisements after ceasing to be an advertising interface.
    let is_advertising_interface = ND::is_advertising_interface(ctx, device_id);
    let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);
    let is_final_ra_batch = ndp_state.final_router_advertisements_remaining != 0;
    assert!(is_final_ra_batch || is_advertising_interface);

    let timer_id = NdpTimerId::new_router_advertisement_transmit_timer_id::<ND>(device_id);

    // If no existing Router Advertisement transmission is scheduled, scheduled the timer at
    // `instant`. If we already have a scheduled Router Advertisement, overwrite the timer if
    // `instant` is at a time before the existing timer, `next_instant`.
    let next_instant = ctx.dispatcher().scheduled_instant(timer_id);
    if next_instant.map_or(true, |i| i > instant) {
        trace!(
            "ndp::schedule_next_router_advertisement: scheduling the next router advertisement for device {:?} at {:?}, overwriting potentially existing timer that would have fired at {:?}",
            ND::get_device_id(device_id), instant, next_instant,
        );

        // Schedule the timeout to send the router advertisement.
        ctx.dispatcher_mut().schedule_timeout_instant(instant, timer_id);
    } else {
        trace!(
            "ndp::schedule_next_router_advertisement: the next router advertisement for device {:?} at {:?}, is before the new one at {:?}, so doing nothing",
            ND::get_device_id(device_id), next_instant, instant,
        );
    }
}

/// Start soliciting routers.
///
/// Does nothing if a device's MAX_RTR_SOLICITATIONS parameters is `0`.
///
/// # Panics
///
/// Panics if we attempt to start router solicitation as a router, or if
/// the device is already soliciting routers.
pub(crate) fn start_soliciting_routers<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
) {
    // MUST NOT be a router.
    assert!(!ND::is_router(ctx, device_id));

    let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);

    // MUST NOT already be performing router solicitation.
    assert_eq!(ndp_state.router_solicitations_remaining, 0);

    if let Some(v) = ndp_state.configs.max_router_solicitations {
        trace!(
            "ndp::start_soliciting_routers: start soliciting routers for device: {:?}",
            ND::get_device_id(device_id)
        );

        ndp_state.router_solicitations_remaining = v.get();

        // As per RFC 4861 section 6.3.7, delay the first transmission for a random amount of time
        // between 0 and `MAX_RTR_SOLICITATION_DELAY` to alleviate congestion when many hosts start
        // up on a link at the same time.
        let delay =
            ctx.dispatcher_mut().rng().gen_range(Duration::new(0, 0), MAX_RTR_SOLICITATION_DELAY);

        // MUST NOT already be performing router solicitation.
        assert!(ctx
            .dispatcher_mut()
            .schedule_timeout(delay, NdpTimerId::new_router_solicitation_timer_id::<ND>(device_id))
            .is_none());
    } else {
        trace!("ndp::start_soliciting_routers: device {:?} not configured to send any router solicitations", ND::get_device_id(device_id));
    }
}

/// Stop soliciting routers.
///
/// Does nothing if the device is not soliciting routers.
///
/// # Panics
///
/// Panics if we attempt to stop router solicitations on a router (this should never happen
/// as routers should not be soliciting other routers).
pub(crate) fn stop_soliciting_routers<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
) {
    trace!(
        "ndp::stop_soliciting_routers: stop soliciting routers for device: {:?}",
        ND::get_device_id(device_id)
    );

    assert!(!ND::is_router(ctx, device_id));

    ctx.dispatcher_mut()
        .cancel_timeout(NdpTimerId::new_router_solicitation_timer_id::<ND>(device_id));

    // No more router solicitations remaining since we are cancelling.
    ND::get_ndp_state_mut(ctx.state_mut(), device_id).router_solicitations_remaining = 0;
}

/// Solicit routers once amd schedule next message.
///
/// # Panics
///
/// Panics if we attempt to do router solicitation as a router or if
/// we are already done soliciting routers.
fn do_router_solicitation<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
) {
    assert!(!ND::is_router(ctx, device_id));

    let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);
    let remaining = &mut ndp_state.router_solicitations_remaining;

    assert!(*remaining > 0);
    *remaining -= 1;
    let remaining = *remaining;

    let src_ip = ND::get_ipv6_addr(ctx.state(), device_id);

    trace!(
        "do_router_solicitation: soliciting routers for device {:?} using src_ip {:?}",
        device_id,
        src_ip
    );

    send_router_solicitation::<_, ND>(ctx, device_id, src_ip);

    if remaining == 0 {
        trace!(
            "do_router_solicitation: done sending router solicitation messages for device {:?}",
            device_id
        );
        return;
    } else {
        // TODO(ghanan): Make the interval between messages configurable.
        ctx.dispatcher_mut().schedule_timeout(
            RTR_SOLICITATION_INTERVAL,
            NdpTimerId::new_router_solicitation_timer_id::<ND>(device_id),
        );
    }
}

/// Send a router solicitation packet.
///
/// # Panics
///
/// Panics if we attempt to send a router solicitation as a router.
fn send_router_solicitation<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
    src_ip: Option<Ipv6Addr>,
) {
    assert!(!ND::is_router(ctx, device_id));

    let src_ip = src_ip.unwrap_or(Ipv6::UNSPECIFIED_ADDRESS);

    trace!("send_router_solicitation: sending router solicitation from {:?}", src_ip);

    if !src_ip.is_specified() {
        // Must not include the source link layer address if the source address
        // is unspecified as per RFC 4861 section 4.1.
        send_ndp_packet::<_, ND, &[u8], _>(
            ctx,
            device_id,
            src_ip,
            Ipv6::ALL_ROUTERS_LINK_LOCAL_ADDRESS,
            RouterSolicitation::default(),
            &[],
        );
    } else {
        let src_ll = ND::get_link_layer_addr(ctx.state(), device_id);
        send_ndp_packet::<_, ND, &[u8], _>(
            ctx,
            device_id,
            src_ip,
            Ipv6::ALL_ROUTERS_LINK_LOCAL_ADDRESS,
            RouterSolicitation::default(),
            &[NdpOption::SourceLinkLayerAddress(src_ll.bytes())],
        );
    }
}

/// Begin the Duplicate Address Detection process.
///
/// If the device is configured to not do DAD, then this method will
/// immediately assign `tentative_addr` to the device.
///
/// # Panics
///
/// Panics if DAD is already being performed on this address.
pub(crate) fn start_duplicate_address_detection<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
    tentative_addr: Ipv6Addr,
) {
    let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);

    let transmits = ndp_state.configs.dup_addr_detect_transmits;

    if let Some(transmits) = transmits {
        // Must not already be performing DAD on the device.
        assert!(ndp_state
            .dad_transmits_remaining
            .insert(tentative_addr, transmits.get())
            .is_none());

        trace!("ndp::start_duplicate_address_detection: starting duplicate address detection for address {:?} on device {:?}", tentative_addr, ND::get_device_id(device_id));

        do_duplicate_address_detection::<D, ND>(ctx, device_id, tentative_addr);
    } else {
        // Must not already be performing DAD on the device.
        assert!(!ndp_state.dad_transmits_remaining.contains_key(&tentative_addr));

        // DAD is turned off since the interface's DUP_ADDR_DETECT_TRANSMIT parameter
        // is `None`.
        trace!("ndp::start_duplicate_address_detection: assigning address {:?} on device {:?} immediately because duplicate address detection is disabled", tentative_addr, ND::get_device_id(device_id));

        ND::unique_address_determined_wrapper(ctx, device_id, tentative_addr);
    }
}

/// Cancels the Duplicate Address Detection process.
///
/// Note, the address will now be in a tentative state forever unless the
/// caller assigns a new address to the device (DAD will restart), explicitly
/// restarts DAD, or the device receives a Neighbor Solicitation or Neighbor
/// Advertisement message (the address will be found to be a duplicate and
/// unassigned from the device).
///
/// # Panics
///
/// Panics if we are not currently performing DAD for `tentative_addr` on `device_id`.
pub(crate) fn cancel_duplicate_address_detection<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
    tentative_addr: Ipv6Addr,
) {
    trace!("ndp::cancel_duplicate_address_detection: cancelling duplicate address detection for address {:?} on device {:?}", tentative_addr, ND::get_device_id(device_id));

    ctx.dispatcher_mut().cancel_timeout(NdpTimerId::new_dad_ns_transmission_timer_id::<ND>(
        device_id,
        tentative_addr,
    ));

    // `unwrap` may panic if we have no entry in `dad_transmits_remaining` for
    // `tentative_addr` which means that we are not performing DAD on
    // `tentative_add`. This case is documented as a panic condition.
    ND::get_ndp_state_mut(ctx.state_mut(), device_id)
        .dad_transmits_remaining
        .remove(&tentative_addr)
        .unwrap();
}

/// Send another DAD message (Neighbor Solicitation).
///
/// # Panics
///
/// Panics if the DAD process has not been started for `tentative_addr` on `device_id`.
fn do_duplicate_address_detection<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
    tentative_addr: Ipv6Addr,
) {
    trace!("do_duplicate_address_detection: tentative_addr {:?}", tentative_addr);

    let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);

    // We MUST have already started the DAD process if we reach this point.
    let remaining = ndp_state.dad_transmits_remaining.get_mut(&tentative_addr).unwrap();
    assert!(*remaining > 0);
    *remaining -= 1;

    // Uses same RETRANS_TIMER definition per RFC 4862 section-5.1
    let retrans_timer = ndp_state.retrans_timer;

    let src_ll = ND::get_link_layer_addr(ctx.state(), device_id);
    send_ndp_packet::<_, ND, &[u8], _>(
        ctx,
        device_id,
        Ipv6::UNSPECIFIED_ADDRESS,
        tentative_addr.to_solicited_node_address().get(),
        NeighborSolicitation::new(tentative_addr),
        &[NdpOption::SourceLinkLayerAddress(src_ll.bytes())],
    );

    ctx.dispatcher_mut().schedule_timeout(
        retrans_timer,
        NdpTimerId::new_dad_ns_transmission_timer_id::<ND>(device_id, tentative_addr),
    );
}

fn send_neighbor_solicitation<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
    lookup_addr: Ipv6Addr,
) {
    trace!("send_neighbor_solicitation: lookip_addr {:?}", lookup_addr);

    // TODO(brunodalbo) when we send neighbor solicitations, we SHOULD set
    //  the source IP to the same IP as the packet that triggered the
    //  solicitation, so that when we hit the neighbor they'll have us in their
    //  cache, reducing overall burden on the network.
    if let Some(src_ip) = ND::get_ipv6_addr(ctx.state(), device_id) {
        assert!(src_ip.is_valid_unicast());
        let src_ll = ND::get_link_layer_addr(ctx.state(), device_id);
        let dst_ip = lookup_addr.to_solicited_node_address().get();
        send_ndp_packet::<_, ND, &[u8], _>(
            ctx,
            device_id,
            src_ip,
            dst_ip,
            NeighborSolicitation::new(lookup_addr),
            &[NdpOption::SourceLinkLayerAddress(src_ll.bytes())],
        );
    } else {
        // Nothing can be done if we don't have any ipv6 addresses to send
        // packets out to.
        debug!("Not sending NDP request, since we don't know our IPv6 address");
    }
}

fn send_neighbor_advertisement<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
    solicited: bool,
    device_addr: Ipv6Addr,
    dst_ip: Ipv6Addr,
) {
    debug!("send_neighbor_advertisement from {:?} to {:?}", device_addr, dst_ip);
    debug_assert!(device_addr.is_valid_unicast());
    // We currently only allow the destination address to be:
    // 1) a unicast address.
    // 2) a multicast destination but the message should be a unsolicited neighbor
    //    advertisement.
    // NOTE: this assertion may need change if more messages are to be allowed in the future.
    debug_assert!(dst_ip.is_valid_unicast() || (!solicited && dst_ip.is_multicast()));

    // We must call into the higher level send_ndp_packet function because it is
    // not guaranteed that we have actually saved the link layer address of the
    // destination ip. Typically, the solicitation request will carry that
    // information, but it is not necessary. So it is perfectly valid that
    // trying to send this advertisement will end up triggering a neighbor
    // solicitation to be sent.
    let src_ll = ND::get_link_layer_addr(ctx.state(), device_id);
    let options = [NdpOption::TargetLinkLayerAddress(src_ll.bytes())];
    send_ndp_packet::<_, ND, &[u8], _>(
        ctx,
        device_id,
        device_addr,
        dst_ip,
        NeighborAdvertisement::new(ND::is_router(ctx, device_id), solicited, false, device_addr),
        &options[..],
    );
}

/// Send a router advertisement message from `device_id` to `dst_ip`.
///
/// `dst_ip` is typically the source address of an invoking Router Solicitation, or the all-nodes
/// multicast address.
///
/// `send_router_advertisement` does nothing if `device_id` is configured to not send Router
/// Advertisements.
///
/// # Panics
///
/// Panics if `device_id` does not have an assigned (non-tentative) link-local address or if it is
/// not either an advertising interface or sending the final Router Advertisements after ceasing to
/// be an advertising interface.
fn send_router_advertisement<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
    dst_ip: Ipv6Addr,
) {
    // If we are attempting to send a router advertisement, we need to have a valid
    // link-local address, as per RFC 4861 section 4.2. The call to either `unwrap` may
    // panic if `device_id` does not have an assigned (non-tentative) link-local address,
    // but this is documented for this function.
    let src_ip =
        ND::get_link_local_addr(ctx.state(), device_id).unwrap().try_into_permanent().unwrap();

    let is_final_ra_batch =
        ND::get_ndp_state(ctx.state(), device_id).final_router_advertisements_remaining != 0;

    // Device MUST be in one of two scenarios if we reach this point:
    // 1) Is an advertising interface.
    // 2) Sending the final Router Advertisements after ceasing to be an advertising interface.
    assert!(is_final_ra_batch || ND::is_advertising_interface(ctx, device_id));

    let ra_type = if is_final_ra_batch { "final batch" } else { "normal" };

    trace!(
        "send_router_advertisement: sending {:?} router advertisement from {:?} (dev = {:?}) to {:?}",
        ra_type,
        src_ip,
        ND::get_device_id(device_id),
        dst_ip
    );

    let src_ll = ND::get_link_layer_addr(ctx.state(), device_id);
    let mut options = vec![NdpOption::SourceLinkLayerAddress(src_ll.bytes())];

    let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);

    let router_configurations = ndp_state.configs.get_router_configurations();

    // If the link mtu is set to `None`, do not include the mtu option.
    //
    // See AdvLinkMtu in RFC 4861 section 6.2.1 for more information.
    if let Some(mtu) = router_configurations.get_advertised_link_mtu() {
        options.push(NdpOption::MTU(mtu.get()));
    }

    let prefix_list = router_configurations.get_advertised_prefix_list().clone();
    for p in &prefix_list {
        // We know that `unwrap` will not panic because `new_unaligned` checks to make sure that the
        // byte slice we give it has exactly the number of bytes required for a `PrefixInformation`.
        // Here, we pass it the byte slice representation of a `PrefixInformation`, so we know that
        // `new_unaligned` will not return `None`.
        options.push(NdpOption::PrefixInformation(&p));
    }

    let message = router_configurations.new_router_advertisement(is_final_ra_batch);

    // If `dst_ip` is the IPv6 all-nodes multicast address, increment the counter for number of
    // Router Advertisements sent to the IPv6 all-nodes multicast address.
    if dst_ip == Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS {
        ndp_state.all_nodes_transmited_router_advertisements += 1;
    }

    // Attempt to send the router advertisement message.
    if send_ndp_packet::<_, ND, &[u8], _>(ctx, device_id, src_ip, dst_ip, message, &options[..])
        .is_ok()
    {
        if dst_ip == Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS {
            let now = ctx.dispatcher().now();
            let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);

            // Sent the frame successfully so update NDP state's `last_router_advertisement_instant`.
            ndp_state.last_router_advertisement_instant = Some(now);

            if is_final_ra_batch {
                ndp_state.final_router_advertisements_remaining -= 1;
            }
        }
    } else {
        error!("send_router_advertisement: failed to send router advertisement")
    };
}

/// Helper function to send ndp packet over an NdpDevice to `dst_ip`.
fn send_ndp_packet<D: EventDispatcher, ND: NdpDevice, B: ByteSlice, M>(
    ctx: &mut Context<D>,
    device_id: usize,
    src_ip: Ipv6Addr,
    dst_ip: Ipv6Addr,
    message: M,
    options: &[NdpOption],
) -> Result<(), ()>
where
    M: IcmpMessage<Ipv6, B, Code = IcmpUnusedCode>,
{
    trace!("send_ndp_packet: src_ip={:?} dst_ip={:?}", src_ip, dst_ip);

    ND::send_ipv6_frame(
        ctx,
        device_id,
        dst_ip,
        ndp::OptionsSerializer::<_>::new(options.iter())
            .into_serializer()
            .encapsulate(IcmpPacketBuilder::<Ipv6, B, M>::new(
                src_ip,
                dst_ip,
                IcmpUnusedCode,
                message,
            ))
            .encapsulate(Ipv6PacketBuilder::new(
                src_ip,
                dst_ip,
                REQUIRED_NDP_IP_PACKET_HOP_LIMIT,
                IpProto::Icmpv6,
            )),
    )
    .map_err(|_| ())
}

/// A handler for incoming NDP packets.
///
/// `NdpPacketHandler` is implemented by any `Context<D>` where `D:
/// EventDispatcher`, and it can also be mocked for use in testing.
pub(crate) trait NdpPacketHandler: IpDeviceIdContext {
    /// Receive an NDP packet.
    ///
    /// # Panics
    ///
    /// `receive_ndp_packet` panics if `packet` is not one of
    /// `RouterSolicitation`, `RouterAdvertisement`, `NeighborSolicitation`,
    /// `NeighborAdvertisement`, or `Redirect`.
    fn receive_ndp_packet<B: ByteSlice>(
        &mut self,
        device: Option<Self::DeviceId>,
        src_ip: Ipv6Addr,
        dst_ip: SpecifiedAddr<Ipv6Addr>,
        packet: Icmpv6Packet<B>,
    ) {
    }
}

impl<D: EventDispatcher> NdpPacketHandler for Context<D> {
    fn receive_ndp_packet<B: ByteSlice>(
        &mut self,
        device: Option<DeviceId>,
        src_ip: Ipv6Addr,
        dst_ip: SpecifiedAddr<Ipv6Addr>,
        packet: Icmpv6Packet<B>,
    ) {
        trace!("receive_ndp_packet");

        match device {
            Some(d) => {
                // TODO(brunodalbo) we're assuming the device Id is for an ethernet
                //  device, but it could be for another protocol.
                receive_ndp_packet_inner::<_, EthernetNdpDevice, _>(
                    self,
                    d.id(),
                    src_ip,
                    dst_ip,
                    packet,
                );
            }
            None => {
                // NDP needs a device identifier context to operate on.
                debug!("Got NDP packet without device identifier. Ignoring it.");
            }
        }
    }
}

fn duplicate_address_detected<D: EventDispatcher, ND: NdpDevice>(
    ctx: &mut Context<D>,
    device_id: usize,
    addr: Ipv6Addr,
) {
    cancel_duplicate_address_detection::<_, ND>(ctx, device_id, addr);

    // let's notify our device
    ND::duplicate_address_detected(ctx, device_id, addr);
}

fn receive_ndp_packet_inner<D: EventDispatcher, ND: NdpDevice, B>(
    ctx: &mut Context<D>,
    device_id: usize,
    src_ip: Ipv6Addr,
    dst_ip: SpecifiedAddr<Ipv6Addr>,
    packet: Icmpv6Packet<B>,
) where
    B: ByteSlice,
{
    // TODO(ghanan): Make sure the IP packet's hop limit was set to 255 as per RFC 4861 sections
    //               4.1, 4.2, 4.3, 4.4, and 4.5 (each type of NDP packet).

    match packet {
        Icmpv6Packet::RouterSolicitation(p) => {
            trace!("receive_ndp_packet_inner: Received NDP RS");

            if !ND::is_router(ctx, device_id) {
                // Hosts MUST silently discard Router Solicitation messages
                // as per RFC 4861 section 6.1.1.
                trace!(
                    "receive_ndp_packet_inner: device {:?} is not a router, discarding NDP RS",
                    ND::get_device_id(device_id)
                );
                return;
            }

            // TODO(ghanan): Make sure IP's hop limit is set to 255 as per RFC 4861 section 6.1.1.

            let source_link_layer_option = get_source_link_layer_option::<ND, _>(p.body());

            if !src_ip.is_specified() && source_link_layer_option.is_some() {
                // If the IP source address is the unspecified address and there is a
                // source link-layer address option in the message, we MUST silently
                // discard the Router Solicitation message as per RFC 4861 section 6.1.1.
                trace!("receive_ndp_packet_inner: source is unspcified but it has the source link-layer address option, discarding NDP RS");
                return;
            }

            increment_counter!(ctx, "ndp::rx_router_solicitation");

            let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);

            if !ndp_state.configs.get_router_configurations().get_should_send_advertisements() {
                // If a router's AdvSendAdvertisements flag is set to false, it doesn't send
                // Router Advertisements - even in response to Router Solicitations. See
                // AdvSendAdvertisements in RFC 4861 section 6.2.1 for more details.
                trace!("receive_ndp_packet_inner: device {:?} is not configured to send Router Advertisements, ignoring NDP RS", ND::get_device_id(device_id));
                return;
            }

            if let Some(link_addr) = source_link_layer_option {
                // Set the link address and mark the neighbor entry as stale if we either create it,
                // or updated an existing one, as per RFC 4861 section 6.2.6.
                ndp_state.neighbors.set_link_address(src_ip, link_addr, false);
            }

            if let Some(state) = ndp_state.neighbors.get_neighbor_state_mut(&src_ip) {
                // Set the neighbor's IsRouter flag to false, as per RFC 4861 section 6.2.6.
                //
                // This is because only hosts should solicit routers.
                state.is_router = false;
            }

            //
            // As per RFC 4861 section 6.2.6:
            // A router might process Router Solicitations as follows:
            //
            // - Upon receipt of a Router Solicitation, compute a random delay
            //   within the range 0 through `MAX_RA_DELAY_TIME`.  If the computed
            //   value corresponds to a time later than the time the next multicast
            //   Router Advertisement is scheduled to be sent, ignore the random
            //   delay and send the advertisement at the already-scheduled time.
            //
            // - If the router sent a multicast Router Advertisement (solicited or
            //   unsolicited) within the last `MIN_DELAY_BETWEEN_RAS` seconds,
            //   schedule the advertisement to be sent at a time corresponding to
            //   `MIN_DELAY_BETWEEN_RAS` plus the random value after the previous
            //   advertisement was sent.  This ensures that the multicast Router
            //   Advertisements are rate limited.
            //
            // - Otherwise, schedule the sending of a Router Advertisement at the
            //   time given by the random value.
            //
            // Routers have the option of responding to Router Solicitations by sending a Router
            // Advertisement directly to the soliciting host (the source address is not the
            // unspecified address), or by a Router Advertisement to the all nodes multicast
            // address. For now, we will only reply to Router Solicitations by sending the
            // Router Advertisement to the IPv6 all nodes multicast addres.
            //
            // As per RFC 4861 section 6.2.6, consecutive Router Advertisements
            // sent to the all-nodes multicast address MUST be rate limited to no
            // more than one advertisement every `MIN_DELAY_BETWEEN_RAS` seconds.
            //
            // TODO(ghanan): Support sending Router Advertisements directly to the soliciting host.
            //               The choice to send Router Advertisements directly to the host or the
            //               all nodes multicast address should be configurable at run-time.

            // Calculate random delay between `MIN_RA_DELAY_TIME` and `MAX_RA_DELAY_TIME`.
            let now = ctx.dispatcher().now();
            let delay = ctx.dispatcher_mut().rng().gen_range(MIN_RA_DELAY_TIME, MAX_RA_DELAY_TIME);
            let send_instant = now
                .checked_add(delay)
                .expect("Failed to calculate instant to reply to Router Solicitation");

            if ctx
                .dispatcher()
                .scheduled_instant(NdpTimerId::new_router_advertisement_transmit_timer_id::<ND>(
                    device_id,
                ))
                .map_or(false, |i| i < send_instant)
            {
                // Next scheduled Router Advertisement will be sent before `delay` time from
                // `now` so let the scheduled Router Advertisement be the reply to this
                // Router Solicitation message.
                trace!("receive_ndp_packet_inner: next scheduled RA will be the response this NDP RS from {:?} on device {:?}", src_ip, ND::get_device_id(device_id));
                return;
            }

            let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);

            if let Some(last_instant) = ndp_state.last_router_advertisement_instant {
                if now.duration_since(last_instant) <= MIN_DELAY_BETWEEN_RAS {
                    // We already sent a Router Advertisement to the all-nodes multicast address
                    // within `MIN_DELAY_BETWEEN_RAS` time, so schedule a new Router Advertisement
                    // to be sent at `delay` and `MIN_DELAY_BETWEEN_RAS` time after `last_instant`.
                    let next_instant = last_instant
                        .checked_add(delay)
                        .expect("Failed to calculate instant to send the next Router Advertisement")
                        .checked_add(MIN_DELAY_BETWEEN_RAS)
                        .expect(
                            "Failed to calculate instant to send the next Router Advertisement",
                        );

                    trace!("receive_ndp_packet_inner: already sent an RA within MIN_DELAY_BETWEEN_RAS ({:?}), scheduling RA reply for RS from {:?} to be sent at {:?} from device {:?}", MIN_DELAY_BETWEEN_RAS, src_ip, next_instant, ND::get_device_id(device_id));

                    schedule_next_router_advertisement_instant::<_, ND>(
                        ctx,
                        device_id,
                        next_instant,
                    );
                    return;
                }
            }

            // If we haven't scheduled the RA response yet, or an already scheduled RA transmission
            // is too far from now, schedule an RA transmission for after `delay` time.
            trace!("receive_ndp_packet_inner: scheduling an RA reply for RS from {:?} to be sent at {:?} from device {:?}", src_ip, send_instant, ND::get_device_id(device_id));

            schedule_next_router_advertisement_instant::<_, ND>(ctx, device_id, send_instant);
        }
        Icmpv6Packet::RouterAdvertisement(p) => {
            trace!("receive_ndp_packet_inner: Received NDP RA from router: {:?}", src_ip);

            let src_ip = if let Some(src_ip) = LinkLocalAddr::new(src_ip) {
                src_ip
            } else {
                // Nodes MUST silently discard any received Router Advertisement message
                // where the IP source address is not a link-local address as routers must
                // use their link-local address as the source for Router Advertisements so
                // hosts can uniquely identify routers, as per RFC 4861 section 6.1.2.
                trace!("receive_ndp_packet_inner: source is not a link-local address, discarding NDP RA");
                return;
            };

            // TODO(ghanan): Make sure IP's hop limit is set to 255 as per RFC 4861 section 6.1.2.

            increment_counter!(ctx, "ndp::rx_router_advertisement");

            if ND::is_router(ctx, device_id) {
                // TODO(ghanan): Handle receiving Router Advertisements when this node is a router.
                trace!("receive_ndp_packet_inner: received NDP RA as a router, discarding NDP RA");
                return;
            }

            let (state, dispatcher) = ctx.state_and_dispatcher();
            let ndp_state = ND::get_ndp_state_mut(state, device_id);
            let ra = p.message();

            let timer_id = NdpTimerId::new_router_invalidation_timer_id::<ND>(device_id, src_ip);

            if ra.router_lifetime() == 0 {
                if ndp_state.has_default_router(&src_ip) {
                    trace!("receive_ndp_packet_inner: NDP RA has zero-valued router lifetime, invaliding router: {:?}", src_ip);

                    // As per RFC 4861 section 6.3.4, immediately timeout the entry as specified in
                    // RFC 4861 section 6.3.5.

                    assert!(dispatcher.cancel_timeout(timer_id).is_some());

                    // `invalidate_default_router` may panic if `src_ip` does not reference a known
                    // default router, but we will only reach here if the router is already in our
                    // list of default routers, so we know `invalidate_default_router` will not
                    // panic.
                    ndp_state.invalidate_default_router(&src_ip);
                } else {
                    trace!("receive_ndp_packet_inner: NDP RA has zero-valued router lifetime, but the router {:?} is unknown so doing nothing", src_ip);
                }

            // As per RFC 4861 section 4.2, a zero-valued router lifetime only indicates the
            // router is not to be used as a default router and is only applied to its
            // usefulness as a default router; it does not apply to the other information
            // contained in this message's fields or options. Given this, we continue as normal.
            } else {
                if ndp_state.has_default_router(&src_ip) {
                    trace!(
                        "receive_ndp_packet_inner: NDP RA from an already known router: {:?}",
                        src_ip
                    );
                } else {
                    trace!("receive_ndp_packet_inner: NDP RA from a new router: {:?}", src_ip);

                    // TODO(ghanan): Make the number of default routers we store configurable?
                    ndp_state.add_default_router(src_ip);
                };

                // As per RFC 4861 section 4.2, the router livetime is in units of seconds.
                let timer_duration = Duration::from_secs(ra.router_lifetime().into());

                trace!("receive_ndp_packet_inner: NDP RA: updating invalidation timer to {:?} for router: {:?}", timer_duration, src_ip);
                // Reset invalidation timeout.
                dispatcher.schedule_timeout(timer_duration, timer_id);
            }

            // As per RFC 4861 section 6.3.4:
            // If the received Reachable Time value is non-zero, the host SHOULD set
            // its BaseReachableTime variable to the received value.  If the new
            // value differs from the previous value, the host SHOULD re-compute a
            // new random ReachableTime value.
            //
            // TODO(ghanan): Make the updating of this field from the RA message configurable
            //               since the RFC does not say we MUST update the field.
            //
            // TODO(ghanan): In most cases, the advertised Reachable Time value will be the same
            //               in consecutive Router Advertisements, and a host's BaseReachableTime
            //               rarely changes.  In such cases, an implementation SHOULD ensure that
            //               a new random value gets re-computed at least once every few hours.
            if ra.reachable_time() != 0 {
                let base_reachable_time = Duration::from_millis(ra.reachable_time().into());

                trace!("receive_ndp_packet_inner: NDP RA: updating base_reachable_time to {:?} for router: {:?}", base_reachable_time, src_ip);

                // As per RFC 4861 section 4.2, the reachable time field is the time in
                // milliseconds.
                ndp_state.set_base_reachable_time(base_reachable_time);
            }

            // As per RFC 4861 section 6.3.4:
            // The RetransTimer variable SHOULD be copied from the Retrans Timer
            // field, if the received value is non-zero.
            //
            // TODO(ghanan): Make the updating of this field from the RA message configurable
            //               since the RFC does not say we MUST update the field.
            if ra.retransmit_timer() != 0 {
                let retransmit_timer = Duration::from_millis(ra.retransmit_timer().into());

                trace!("receive_ndp_packet_inner: NDP RA: updating retrans_timer to {:?} for router: {:?}", retransmit_timer, src_ip);

                // As per RFC 4861 section 4.2, the retransmit timer field is the time in
                // milliseconds.
                ndp_state.set_retrans_timer(retransmit_timer);
            }

            // As per RFC 4861 section 6.3.4:
            // If the received Cur Hop Limit value is non-zero, the host SHOULD set
            // its CurHopLimit variable to the received value.
            //
            // TODO(ghanan): Make the updating of this field from the RA message configurable
            //               since the RFC does not say we MUST update the field.
            if let Some(hop_limit) = NonZeroU8::new(ra.current_hop_limit()) {
                trace!("receive_ndp_packet_inner: NDP RA: updating device's hop limit to {:?} for router: {:?}", ra.current_hop_limit(), src_ip);

                ND::set_hop_limit(state, device_id, hop_limit);
            }

            for option in p.body().iter() {
                match option {
                    // As per RFC 4861 section 6.3.4, if a Neighbor Cache entry is created
                    // for the router, its reachability state MUST be set to STALE as
                    // specified in Section 7.3.3.  If a cache entry already exists and is
                    // updated with a different link-layer address, the reachability state
                    // MUST also be set to STALE.
                    //
                    // TODO(ghanan): Mark NDP state as STALE as per the RFC once we implement
                    //               the RFC compliant states.
                    NdpOption::SourceLinkLayerAddress(a) => {
                        let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);
                        let link_addr =
                            ND::LinkAddress::from_bytes(&a[..ND::LinkAddress::BYTES_LENGTH]);

                        trace!("receive_ndp_packet_inner: NDP RA: setting link address for router {:?} to {:?}", src_ip, link_addr);

                        // Set the link address and mark it as stale if we either created
                        // the neighbor entry, or updated an existing one.
                        ndp_state.neighbors.set_link_address(src_ip.get(), link_addr, false);
                    }
                    NdpOption::MTU(mtu) => {
                        trace!("receive_ndp_packet_inner: mtu option with mtu = {:?}", mtu);

                        // TODO(ghanan): Make updating the MTU from an RA message configurable.
                        if mtu >= crate::ip::path_mtu::IPV6_MIN_MTU {
                            // `set_mtu` may panic if `mtu` is less than `IPV6_MIN_MTU` but we just
                            // checked to make sure that `mtu` is at least `IPV6_MIN_MTU` so we know
                            // `set_mtu` will not panic.
                            ND::set_mtu(ctx.state_mut(), device_id, mtu);
                        } else {
                            trace!("receive_ndp_packet_inner: NDP RA: not setting link MTU (from {:?}) to {:?} as it is less than IPV6_MIN_MTU", src_ip, mtu);
                        }
                    }
                    NdpOption::PrefixInformation(prefix_info) => {
                        let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);

                        trace!("receive_ndp_packet_inner: prefix information option with prefix = {:?}", prefix_info);

                        let addr_sub = match prefix_info.addr_subnet() {
                            None => {
                                trace!("receive_ndp_packet_inner: malformed prefix information, so ignoring");
                                continue;
                            }
                            Some(a) => a,
                        };

                        if !prefix_info.on_link_flag() {
                            // TODO(ghanan): Do something?
                            return;
                        }

                        if prefix_info.prefix().is_linklocal() {
                            // If the on-link flag is set and the prefix is the link-local prefix,
                            // ignore the option, as per RFC 4861 section 6.3.4.
                            trace!("receive_ndp_packet_inner: prefix is a link local, so ignoring");
                            continue;
                        }

                        // Timer ID for this prefix's invalidation.
                        let timer_id =
                            NdpTimerId::new_prefix_invalidation_timer_id::<ND>(device_id, addr_sub);

                        if prefix_info.valid_lifetime() == 0 {
                            if ndp_state.has_prefix(&addr_sub) {
                                trace!("receive_ndp_packet_inner: prefix is known and has valid lifetime = 0, so invaliding");

                                // If the on-link flag is set, the valid lifetime is 0 and the
                                // prefix is already present in our prefix list, timeout the prefix
                                // immediately, as per RFC 4861 section 6.3.4.

                                // Cancel the prefix invalidation timeout if it exists.
                                ctx.dispatcher_mut().cancel_timeout(timer_id);

                                let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);
                                ndp_state.invalidate_prefix(addr_sub);
                            } else {
                                // If the on-link flag is set, the valid lifetime is 0 and the
                                // prefix is not present in our prefix list, ignore the option, as
                                // per RFC 4861 section 6.3.4.
                                trace!("receive_ndp_packet_inner: prefix is unknown and is has valid lifetime = 0, so ignoring");
                            }

                            continue;
                        }

                        if !ndp_state.has_prefix(&addr_sub) {
                            // `add_prefix` may panic if the prefix already exists in
                            // our prefix list, but we will only reach here if it doesn't
                            // so we know `add_prefix` will not panic.
                            ndp_state.add_prefix(addr_sub);
                        }

                        // Reset invalidation timer.
                        if prefix_info.valid_lifetime() == std::u32::MAX {
                            // A valid lifetime of all 1 bits (== `std::u32::MAX`) represents
                            // infinity, as per RFC 4861 section 4.6.2. Given this, we do not need a
                            // timer to mark the prefix as invalid.
                            ctx.dispatcher_mut().cancel_timeout(timer_id);
                        } else {
                            ctx.dispatcher_mut().schedule_timeout(
                                Duration::from_secs(prefix_info.valid_lifetime().into()),
                                timer_id,
                            );
                        }
                    }
                    _ => {}
                }
            }

            // If the router exists in our router table, make sure it is marked as a router as
            // per RFC 4861 section 6.3.4.
            let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);
            if let Some(state) = ndp_state.neighbors.get_neighbor_state_mut(&src_ip) {
                state.is_router = true;
            }
        }
        Icmpv6Packet::NeighborSolicitation(p) => {
            trace!("receive_ndp_packet_inner: Received NDP NS");

            let target_address = p.message().target_address();

            // Is `target_address` a valid unicast address, and if so, is it associated with
            // our device? If not, drop the packet.
            if !target_address.is_valid_unicast()
                || ND::ipv6_addr_state(ctx.state(), device_id, target_address).is_none()
            {
                // just ignore packet, either it was not really meant for us or
                // is malformed.
                trace!("receive_ndp_packet_inner: Dropping NDP NS packet that is not meant for us or malformed");
                return;
            }

            // We know the call to `unwrap` will not panic because we just checked to make sure
            // that `target_address` is associated with `device_id`.
            let state = ND::ipv6_addr_state(ctx.state_mut(), device_id, target_address).unwrap();
            if state.is_tentative() {
                if !src_ip.is_specified() {
                    // If the source address of the packet is the unspecified address,
                    // the source of the packet is performing DAD for the same target
                    // address as our `my_addr`. A duplicate address has been detected.
                    trace!(
                        "receive_ndp_packet_inner: Received NDP NS: duplicate address {:?} detected on device {:?}", target_address, ND::get_device_id(device_id)
                    );

                    duplicate_address_detected::<_, ND>(ctx, device_id, *target_address);
                }

                // `target_address` is tentative on `device_id` so we do not continue processing
                // the NDP NS.
                return;
            }

            //
            // At this point, we gurantee the following is true because of the earlier checks:
            //
            //   1) The target address is a valid unicast address.
            //   2) The target address is an address that is on our device, `device_id`.
            //   3) The target address is not tentative.
            //

            increment_counter!(ctx, "ndp::rx_neighbor_solicitation");

            // If we have a source link layer address option, we take it and
            // save to our cache.
            if src_ip.is_specified() {
                // We only update the cache if it is not from an unspecified address,
                // i.e., it is not a DAD message. (RFC 4861)
                if let Some(ll) = get_source_link_layer_option::<ND, _>(p.body()) {
                    trace!("receive_ndp_packet_inner: Received NDP NS from {:?} has source link layer option w/ link address {:?}", src_ip, ll);

                    // Set the link address and mark it as stale if we either create
                    // the neighbor entry, or updated an existing one, as per RFC 4861
                    // section 7.2.3.
                    ND::get_ndp_state_mut(ctx.state_mut(), device_id)
                        .neighbors
                        .set_link_address(src_ip, ll, false);
                }

                trace!(
                    "receive_ndp_packet_inner: Received NDP NS: sending NA to source of NS {:?}",
                    src_ip
                );

                // Finally we ought to reply to the Neighbor Solicitation with a
                // Neighbor Advertisement.
                send_neighbor_advertisement::<_, ND>(ctx, device_id, true, *target_address, src_ip);
            } else {
                trace!(
                    "receive_ndp_packet_inner: Received NDP NS: sending NA to all nodes multicast"
                );

                // Send out Unsolicited Advertisement in response to neighbor who's
                // performing DAD, as described in RFC 4861 and 4862
                send_neighbor_advertisement::<_, ND>(
                    ctx,
                    device_id,
                    false,
                    *target_address,
                    Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS,
                )
            }
        }
        Icmpv6Packet::NeighborAdvertisement(p) => {
            trace!("receive_ndp_packet_inner: Received NDP NA");

            let message = p.message();
            let target_address = message.target_address();
            match ND::ipv6_addr_state(ctx.state(), device_id, target_address) {
                Some(AddressState::Tentative) => {
                    trace!("receive_ndp_packet_inner: NDP NA has a target address {:?} that is tentative on device {:?}", target_address, ND::get_device_id(device_id));
                    duplicate_address_detected::<_, ND>(ctx, device_id, *target_address);
                    return;
                }
                Some(AddressState::Assigned) => {
                    // RFC 4862 says this situation is out of the scope, so we
                    // just log out the situation for now.
                    //
                    // TODO(ghanan): Signal to bindings that a duplicate address is detected?
                    error!("receive_ndp_packet_inner: NDP NA: A duplicated address {:?} found on device {:?} when we are not in DAD process!", target_address, ND::get_device_id(device_id));
                    return;
                }
                // Do nothing.
                None => {}
            }

            increment_counter!(ctx, "ndp::rx_neighbor_advertisement");

            let ndp_state = ND::get_ndp_state_mut(ctx.state_mut(), device_id);

            let neighbor_state = if let Some(state) =
                ndp_state.neighbors.get_neighbor_state_mut(&src_ip)
            {
                state
            } else {
                // If the neighbor is not in the cache, we just ignore the advertisement, as
                // we're not yet interested in communicating with it, as per RFC 4861 section
                // 7.2.5.
                trace!("receive_ndp_packet_inner: Ignoring NDP NA from {:?} does not already exist in our list of neighbors, so discarding", src_ip);
                return;
            };

            let target_ll = get_target_link_layer_option::<ND, _>(p.body());

            if neighbor_state.is_incomplete() {
                // If we are in the Incomplete state, we should not have ever learned about a
                // link-layer address.
                assert!(neighbor_state.link_address.is_none());

                if let Some(address) = target_ll {
                    // Set the IsRouter flag as per RFC 4861 section 7.2.5.
                    trace!(
                        "receive_ndp_packet_inner: NDP RS from {:?} indicicates it {:?} a router",
                        src_ip,
                        if message.router_flag() { "is" } else { "isn't" }
                    );
                    neighbor_state.is_router = message.router_flag();

                    // Record the link-layer address.
                    //
                    // If the advertisement's Solicited flag is set, the state of the
                    // entry is set to REACHABLE; otherwise, it is set to STALE, as
                    // per RFC 4861 section 7.2.5.
                    //
                    // Note, since the neighbor's link address was `None` before, we will definitely
                    // update the address, so the state will be set to STALE if the solicited flag
                    // is unset.
                    trace!(
                        "receive_ndp_packet_inner: Resolving link address of {:?} to {:?}",
                        src_ip,
                        address
                    );
                    ndp_state.neighbors.set_link_address(src_ip, address, message.solicited_flag());

                    // Cancel the resolution timeout.
                    ctx.dispatcher.cancel_timeout(
                        NdpTimerId::new_link_address_resolution_timer_id::<ND>(device_id, src_ip),
                    );

                    // Send any packets queued for the neighbor awaiting address resolution.
                    ND::address_resolved(ctx, device_id, &src_ip, address);
                } else {
                    trace!("receive_ndp_packet_inner: Performing address resolution but the NDP NA from {:?} does not have a target link layer address option, so discarding", src_ip);
                    return;
                }

                return;
            }

            // If we are not in the Incomplete state, we should have (at some point) learned about a
            // link-layer address.
            assert!(neighbor_state.link_address.is_some());

            if !message.override_flag() {
                // As per RFC 4861 section 7.2.5:
                //
                // If the Override flag is clear and the supplied link-layer address differs from
                // that in the cache, then one of two actions takes places:
                //
                // a) If the state of the entry is REACHABLE, set it to STALE, but do not update the
                //    entry in any other way.
                //
                // b) Otherwise, the received advertisement should be ignored and MUST NOT update
                //    cache.
                if target_ll.map_or(false, |x| neighbor_state.link_address != Some(x)) {
                    if neighbor_state.is_reachable() {
                        trace!("receive_ndp_packet_inner: NDP RS from known reachable neighbor {:?} does not have override set, but supplied link addr is different, setting state to stale", src_ip);
                        neighbor_state.state = NeighborEntryState::Stale;
                    } else {
                        trace!("receive_ndp_packet_inner: NDP RS from known neighbor {:?} (with reachability unknown) does not have override set, but supplied link addr is different, ignoring", src_ip);
                    }
                }
            }

            // Ignore this unless `target_ll` is `Some`.
            let mut is_same = false;

            // If override is set, the link-layer address MUST be inserted into the cache (if one is
            // supplied and differs from the alreadt recoded address).
            if let Some(address) = target_ll {
                let address = Some(address);

                is_same = (neighbor_state.link_address == address);

                if !is_same && message.override_flag() {
                    neighbor_state.link_address = address;
                }
            }

            // If the override flag is set, or the supplied link-layer address is the same as that
            // in the cache, or no Target Link-Layer Address option was supplied:
            if message.override_flag() || target_ll.is_none() || is_same {
                // - If the solicited flag is set, the state of the entry MUST be set to REACHABLE.
                // - Else, if it was unset, and the link address was updated, the state MUST be set
                //   to STALE.
                // - Otherwise, the state remains the same.
                if message.solicited_flag() {
                    trace!("receive_ndp_packet_inner: NDP RS from {:?} is solicited and either has override set, link address isn't provided, or the provided address is not different, updating state to Reachable", src_ip);
                    neighbor_state.state = NeighborEntryState::Reachable;
                } else if message.override_flag() && target_ll.is_some() && !is_same {
                    trace!("receive_ndp_packet_inner: NDP RS from {:?} is unsolicited and the link address was updated, updating state to Stale", src_ip);

                    neighbor_state.state = NeighborEntryState::Stale;
                } else {
                    trace!("receive_ndp_packet_inner: NDP RS from {:?} is unsolicited and the link address was not updated, doing nothing", src_ip);
                }

                // Check if the neighbor transitioned from a router -> host.
                if neighbor_state.is_router && !message.router_flag() {
                    trace!("receive_ndp_packet_inner: NDP RS from {:?} informed us that it is no longer a router, updating is_router flag", src_ip);
                    neighbor_state.is_router = false;

                    if let Some(router_ll) = LinkLocalAddr::new(src_ip) {
                        // Invalidate the router as a default router if it is one of our default
                        // routers.
                        if ndp_state.has_default_router(&router_ll) {
                            trace!("receive_ndp_packet_inner: NDP RS from {:?} (known as a default router) informed us that it is no longer a router, invaliding the default router", src_ip);
                            ndp_state.invalidate_default_router(&router_ll);
                        }
                    }
                } else {
                    neighbor_state.is_router = message.router_flag();
                }
            }
        }
        Icmpv6Packet::Redirect(p) => log_unimplemented!((), "NDP Redirect not implemented"),
        _ => unreachable!("Invalid ICMP packet passed to NDP"),
    }
}

fn get_source_link_layer_option<ND: NdpDevice, B>(options: &Options<B>) -> Option<ND::LinkAddress>
where
    B: ByteSlice,
{
    options.iter().find_map(|o| match o {
        NdpOption::SourceLinkLayerAddress(a) => {
            if a.len() >= ND::LinkAddress::BYTES_LENGTH {
                Some(ND::LinkAddress::from_bytes(&a[..ND::LinkAddress::BYTES_LENGTH]))
            } else {
                None
            }
        }
        _ => None,
    })
}

fn get_target_link_layer_option<ND: NdpDevice, B>(options: &Options<B>) -> Option<ND::LinkAddress>
where
    B: ByteSlice,
{
    options.iter().find_map(|o| match o {
        NdpOption::TargetLinkLayerAddress(a) => {
            if a.len() >= ND::LinkAddress::BYTES_LENGTH {
                Some(ND::LinkAddress::from_bytes(&a[..ND::LinkAddress::BYTES_LENGTH]))
            } else {
                None
            }
        }
        _ => None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    use std::convert::TryFrom;

    use net_types::ethernet::Mac;
    use net_types::ip::AddrSubnet;
    use packet::{Buf, Buffer, ParseBuffer};

    use crate::device::{
        add_ip_addr_subnet, del_ip_addr, ethernet::EthernetNdpDevice, get_ip_addr_state,
        get_ip_addr_subnets, get_ipv6_hop_limit, get_mtu, is_in_ip_multicast, is_routing_enabled,
        set_routing_enabled,
    };
    use crate::ip::IPV6_MIN_MTU;
    use crate::testutil::{
        self, get_counter_val, get_dummy_config, get_other_ip_address, parse_ethernet_frame,
        parse_icmp_packet_in_ip_packet_in_ethernet_frame, run_for, set_logger_for_test,
        trigger_next_timer, DummyEventDispatcher, DummyEventDispatcherBuilder, DummyInstant,
        DummyNetwork, DUMMY_CONFIG_V6,
    };
    use crate::wire::icmp::ndp::{
        options::PrefixInformation, OptionsSerializer, RouterAdvertisement, RouterSolicitation,
    };
    use crate::wire::icmp::{IcmpEchoRequest, IcmpParseArgs, Icmpv6Packet};
    use crate::{Instant, StackStateBuilder, TimerId};

    const TEST_LOCAL_MAC: Mac = Mac::new([0, 1, 2, 3, 4, 5]);
    const TEST_REMOTE_MAC: Mac = Mac::new([6, 7, 8, 9, 10, 11]);

    fn local_ip() -> SpecifiedAddr<Ipv6Addr> {
        DUMMY_CONFIG_V6.local_ip
    }

    fn remote_ip() -> SpecifiedAddr<Ipv6Addr> {
        DUMMY_CONFIG_V6.remote_ip
    }

    fn router_advertisement_message(
        src_ip: Ipv6Addr,
        dst_ip: Ipv6Addr,
        current_hop_limit: u8,
        managed_flag: bool,
        other_config_flag: bool,
        router_lifetime: u16,
        reachable_time: u32,
        retransmit_timer: u32,
    ) -> Buf<Vec<u8>> {
        Buf::new(Vec::new(), ..)
            .encapsulate(IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                src_ip,
                dst_ip,
                IcmpUnusedCode,
                RouterAdvertisement::new(
                    current_hop_limit,
                    managed_flag,
                    other_config_flag,
                    router_lifetime,
                    reachable_time,
                    retransmit_timer,
                ),
            ))
            .serialize_vec_outer()
            .unwrap()
            .into_inner()
    }

    fn neighbor_advertisement_message(
        src_ip: Ipv6Addr,
        dst_ip: Ipv6Addr,
        router_flag: bool,
        solicited_flag: bool,
        override_flag: bool,
        mac: Option<Mac>,
    ) -> Buf<Vec<u8>> {
        let mac = mac.map(|x| x.bytes());

        let mut options = Vec::new();

        if let Some(ref mac) = mac {
            options.push(NdpOption::TargetLinkLayerAddress(mac));
        }

        OptionsSerializer::new(options.iter())
            .into_serializer()
            .encapsulate(IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                src_ip,
                dst_ip,
                IcmpUnusedCode,
                NeighborAdvertisement::new(router_flag, solicited_flag, override_flag, src_ip),
            ))
            .serialize_vec_outer()
            .unwrap()
            .unwrap_b()
    }

    fn check_router_config(
        c: &NdpRouterConfigurations,
        should_send: bool,
        interval: RangeInclusive<u16>,
        managed: bool,
        other_config: bool,
        mtu: Option<NonZeroU32>,
        reachable_time: u32,
        retransmit_timer: u32,
        hop_limit: u8,
        default_lifetime: Option<NonZeroU16>,
        prefix_list: &Vec<PrefixInformation>,
    ) {
        assert_eq!(c.get_should_send_advertisements(), should_send);
        assert_eq!(c.get_router_advertisements_interval(), interval);
        assert_eq!(c.get_advertised_managed_flag(), managed);
        assert_eq!(c.get_advertised_other_config_flag(), other_config);
        assert_eq!(c.get_advertised_link_mtu(), mtu);
        assert_eq!(c.get_advertised_reachable_time(), reachable_time);
        assert_eq!(c.get_advertised_retransmit_timer(), retransmit_timer);
        assert_eq!(c.get_advertised_current_hop_limit(), hop_limit);
        assert_eq!(c.get_advertised_default_lifetime(), default_lifetime);
        assert_eq!(c.get_advertised_prefix_list(), prefix_list);
    }

    /// Validate the initial Router Advertisements sent by `ctx` after making a device
    /// an advertising interface.
    ///
    /// By the time this method returns, 3 packets will be sent.
    fn validate_initial_ras_after_enable(
        ctx: &mut Context<DummyEventDispatcher>,
        device: DeviceId,
        ndp_configs: &NdpConfigurations,
        offset: usize,
    ) {
        // The first `MAX_INITIAL_RTR_ADVERTISEMENTS` messages will have a delay of at max
        // `MAX_INITIAL_RTR_ADVERT_INTERVAL`
        for i in 0..MAX_INITIAL_RTR_ADVERTISEMENTS {
            assert_eq!(ctx.dispatcher().frames_sent().len(), offset + usize::try_from(i).unwrap());
            let now = ctx.dispatcher().now();
            assert_eq!(
                ctx.dispatcher()
                    .timer_events()
                    .filter(|x| (*x.0 <= (now + MAX_INITIAL_RTR_ADVERT_INTERVAL))
                        && (*x.1
                            == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                                EthernetNdpDevice,
                            >(device.id())))
                    .count(),
                1
            );
            assert!(trigger_next_timer(ctx));
            assert_eq!(
                ctx.dispatcher().frames_sent().len(),
                offset + usize::try_from(i).unwrap() + 1
            );
            validate_simple_ra(
                &ctx.dispatcher().frames_sent()[offset + usize::try_from(i).unwrap()].1,
                ndp_configs
                    .get_router_configurations()
                    .get_advertised_default_lifetime()
                    .map_or(0, |x| x.get()),
            );
        }

        // Should still have the timer set, but now the time must be between the valid interval
        let interval = ndp_configs.get_router_configurations().get_router_advertisements_interval();
        let now = ctx.dispatcher().now();
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (*x.0 >= (now + Duration::from_secs((*interval.start()).into())))
                    && (*x.0 <= (now + Duration::from_secs((*interval.end()).into())))
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );
    }

    /// Validate the final Router Advertisements sent by `ctx` after making a device
    /// an advertising interface.
    ///
    /// By the time this method returns, `MAX_FINAL_RTR_ADVERTISEMENTS` packets will be sent.
    fn validate_final_ras(
        ctx: &mut Context<DummyEventDispatcher>,
        device: DeviceId,
        offset: usize,
    ) {
        let count = if let Some(x) = MAX_FINAL_RTR_ADVERTISEMENTS {
            x.get()
        } else {
            return;
        };

        for i in 0..usize::from(count) {
            assert!(trigger_next_timer(ctx));
            assert_eq!(ctx.dispatcher().frames_sent().len(), offset + i + 1);
            validate_simple_ra(&ctx.dispatcher().frames_sent()[offset + i].1, 0);
        }

        // Should have no more router advertisement timers.
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| *x.1
                    == NdpTimerId::new_router_advertisement_transmit_timer_id::<EthernetNdpDevice>(
                        device.id()
                    ))
                .count(),
            0
        );
    }

    /// Validate a simple Router Advertisement message (using default NDP configurations).
    fn validate_simple_ra(buf: &[u8], lifetime: u16) {
        let (src_mac, dst_mac, src_ip, dst_ip, message, code) =
            parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv6, _, RouterAdvertisement, _>(
                buf,
                |p| {
                    assert_eq!(p.body().iter().count(), 1);
                    assert!(p
                        .body()
                        .iter()
                        .any(|x| x == NdpOption::SourceLinkLayerAddress(&TEST_LOCAL_MAC.bytes())));
                },
            )
            .unwrap();
        assert_eq!(src_ip, TEST_LOCAL_MAC.to_ipv6_link_local().get());
        assert_eq!(dst_ip, Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS);
        assert_eq!(code, IcmpUnusedCode);
        assert_eq!(message, RouterAdvertisement::new(64, false, false, lifetime, 0, 0));
    }

    #[test]
    fn test_ndp_configurations() {
        let prefix = Vec::new();
        let def_life = NonZeroU16::new(1800);

        let mut c = NdpConfigurations::default();
        assert_eq!(c.get_dup_addr_detect_transmits(), NonZeroU8::new(DUP_ADDR_DETECT_TRANSMITS));
        assert_eq!(c.get_max_router_solicitations(), NonZeroU8::new(MAX_RTR_SOLICITATIONS));

        c.set_dup_addr_detect_transmits(None);
        assert_eq!(c.get_dup_addr_detect_transmits(), None);
        assert_eq!(c.get_max_router_solicitations(), NonZeroU8::new(MAX_RTR_SOLICITATIONS));
        check_router_config(
            c.get_router_configurations(),
            false,
            200..=600,
            false,
            false,
            None,
            0,
            0,
            64,
            def_life,
            &prefix,
        );

        c.set_dup_addr_detect_transmits(NonZeroU8::new(100));
        assert_eq!(c.get_dup_addr_detect_transmits(), NonZeroU8::new(100));
        assert_eq!(c.get_max_router_solicitations(), NonZeroU8::new(MAX_RTR_SOLICITATIONS));
        check_router_config(
            c.get_router_configurations(),
            false,
            200..=600,
            false,
            false,
            None,
            0,
            0,
            64,
            def_life,
            &prefix,
        );

        c.set_max_router_solicitations(None);
        assert_eq!(c.get_dup_addr_detect_transmits(), NonZeroU8::new(100));
        assert_eq!(c.get_max_router_solicitations(), None);
        check_router_config(
            c.get_router_configurations(),
            false,
            200..=600,
            false,
            false,
            None,
            0,
            0,
            64,
            def_life,
            &prefix,
        );

        c.set_max_router_solicitations(NonZeroU8::new(2));
        assert_eq!(c.get_dup_addr_detect_transmits(), NonZeroU8::new(100));
        assert_eq!(c.get_max_router_solicitations(), NonZeroU8::new(2));
        check_router_config(
            c.get_router_configurations(),
            false,
            200..=600,
            false,
            false,
            None,
            0,
            0,
            64,
            def_life,
            &prefix,
        );

        // Max Router Solicitations gets saturated at `MAX_RTR_SOLICITATIONS`.
        c.set_max_router_solicitations(NonZeroU8::new(5));
        assert_eq!(c.get_dup_addr_detect_transmits(), NonZeroU8::new(100));
        assert_eq!(c.get_max_router_solicitations(), NonZeroU8::new(MAX_RTR_SOLICITATIONS));
        check_router_config(
            c.get_router_configurations(),
            false,
            200..=600,
            false,
            false,
            None,
            0,
            0,
            64,
            def_life,
            &prefix,
        );

        let mut rc = NdpRouterConfigurations::default();
        rc.set_should_send_advertisements(true);
        rc.set_router_advertisements_interval(3..=4);
        rc.set_advertised_reachable_time(3600000);
        c.set_router_configurations(rc);
        assert_eq!(c.get_dup_addr_detect_transmits(), NonZeroU8::new(100));
        assert_eq!(c.get_max_router_solicitations(), NonZeroU8::new(MAX_RTR_SOLICITATIONS));
        check_router_config(
            c.get_router_configurations(),
            true,
            3..=4,
            false,
            false,
            None,
            3600000,
            0,
            64,
            def_life,
            &prefix,
        );
    }

    #[test]
    fn test_ndp_router_configurations() {
        let prefix = Vec::new();
        let def_life = NonZeroU16::new(1800);

        let mut c = NdpRouterConfigurations::default();
        check_router_config(&c, false, 200..=600, false, false, None, 0, 0, 64, def_life, &prefix);

        c.set_should_send_advertisements(true);
        check_router_config(&c, true, 200..=600, false, false, None, 0, 0, 64, def_life, &prefix);

        c.set_should_send_advertisements(false);
        check_router_config(&c, false, 200..=600, false, false, None, 0, 0, 64, def_life, &prefix);

        c.set_router_advertisements_interval(3..=4);
        check_router_config(&c, false, 3..=4, false, false, None, 0, 0, 64, def_life, &prefix);

        c.set_router_advertisements_interval(300..=500);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, def_life, &prefix);

        // Max cannot be less than 4.
        c.set_router_advertisements_interval(3..=3);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, def_life, &prefix);

        // Max cannot be greater than 1800
        c.set_router_advertisements_interval(3..=2000);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, def_life, &prefix);

        // Min cannot be less than 3.
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, def_life, &prefix);
        c.set_router_advertisements_interval(2..=500);

        // Min cannot be greater than 0.75 * max
        c.set_router_advertisements_interval(301..=400);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, def_life, &prefix);

        // Min cannot be less than max
        c.set_router_advertisements_interval(300..=200);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, def_life, &prefix);

        c.set_advertised_managed_flag(true);
        check_router_config(&c, false, 300..=500, true, false, None, 0, 0, 64, def_life, &prefix);

        c.set_advertised_managed_flag(false);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, def_life, &prefix);

        c.set_advertised_other_config_flag(true);
        check_router_config(&c, false, 300..=500, false, true, None, 0, 0, 64, def_life, &prefix);

        c.set_advertised_other_config_flag(false);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, def_life, &prefix);

        c.set_advertised_link_mtu(NonZeroU32::new(1500));
        check_router_config(
            &c,
            false,
            300..=500,
            false,
            false,
            NonZeroU32::new(1500),
            0,
            0,
            64,
            def_life,
            &prefix,
        );

        c.set_advertised_link_mtu(None);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, def_life, &prefix);

        c.set_advertised_reachable_time(500);
        check_router_config(
            &c,
            false,
            300..=500,
            false,
            false,
            None,
            500,
            0,
            64,
            def_life,
            &prefix,
        );

        c.set_advertised_reachable_time(3600000);
        check_router_config(
            &c,
            false,
            300..=500,
            false,
            false,
            None,
            3600000,
            0,
            64,
            def_life,
            &prefix,
        );

        // cannot be greater than 3600000
        c.set_advertised_reachable_time(3600001);
        check_router_config(
            &c,
            false,
            300..=500,
            false,
            false,
            None,
            3600000,
            0,
            64,
            def_life,
            &prefix,
        );

        c.set_advertised_reachable_time(0);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, def_life, &prefix);

        c.set_advertised_retransmit_timer(500);
        check_router_config(
            &c,
            false,
            300..=500,
            false,
            false,
            None,
            0,
            500,
            64,
            def_life,
            &prefix,
        );

        c.set_advertised_retransmit_timer(0);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, def_life, &prefix);

        c.set_advertised_current_hop_limit(50);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 50, def_life, &prefix);

        c.set_advertised_current_hop_limit(0);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 0, def_life, &prefix);

        c.set_advertised_current_hop_limit(64);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, def_life, &prefix);

        c.set_advertised_default_lifetime(None);
        check_router_config(&c, false, 300..=500, false, false, None, 0, 0, 64, None, &prefix);

        c.set_advertised_default_lifetime(NonZeroU16::new(600));
        check_router_config(
            &c,
            false,
            300..=500,
            false,
            false,
            None,
            0,
            0,
            64,
            NonZeroU16::new(600),
            &prefix,
        );

        // cannot be less than max router advertisement interval
        c.set_advertised_default_lifetime(NonZeroU16::new(499));
        check_router_config(
            &c,
            false,
            300..=500,
            false,
            false,
            None,
            0,
            0,
            64,
            NonZeroU16::new(600),
            &prefix,
        );

        // cannot be greater than 9000
        c.set_advertised_default_lifetime(NonZeroU16::new(9001));
        check_router_config(
            &c,
            false,
            300..=500,
            false,
            false,
            None,
            0,
            0,
            64,
            NonZeroU16::new(600),
            &prefix,
        );

        // updating router advertisement interval should update default lifetime to max if the new
        // max is greater than the lifetime.
        c.set_router_advertisements_interval(300..=800);
        check_router_config(
            &c,
            false,
            300..=800,
            false,
            false,
            None,
            0,
            0,
            64,
            NonZeroU16::new(800),
            &prefix,
        );

        let prefix = vec![PrefixInformation::new(
            64,
            true,
            false,
            500,
            400,
            Ipv6Addr::new([51, 52, 53, 54, 55, 56, 57, 58, 0, 0, 0, 0, 0, 0, 0, 0]),
        )];
        c.set_advertised_prefix_list(prefix.clone());
        check_router_config(
            &c,
            false,
            300..=800,
            false,
            false,
            None,
            0,
            0,
            64,
            NonZeroU16::new(800),
            &prefix,
        );

        let prefix = Vec::new();
        c.set_advertised_prefix_list(prefix.clone());
        check_router_config(
            &c,
            false,
            300..=800,
            false,
            false,
            None,
            0,
            0,
            64,
            NonZeroU16::new(800),
            &prefix,
        );
    }

    #[test]
    fn test_send_neighbor_solicitation_on_cache_miss() {
        set_logger_for_test();
        let mut ctx = DummyEventDispatcherBuilder::default().build();
        let dev_id = ctx.state_mut().device.add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, dev_id);
        // Now we have to manually assign the ip addresses, see `EthernetNdpDevice::get_ipv6_addr`
        add_ip_addr_subnet(&mut ctx, dev_id, AddrSubnet::new(local_ip().get(), 128).unwrap())
            .unwrap();

        lookup::<DummyEventDispatcher, EthernetNdpDevice>(&mut ctx, dev_id.id(), remote_ip().get());

        // Check that we send the original neighbor solicitation,
        // then resend a few times if we don't receive a response.
        for packet_num in 0..usize::from(MAX_MULTICAST_SOLICIT) {
            assert_eq!(ctx.dispatcher.frames_sent().len(), packet_num + 1);

            testutil::trigger_next_timer(&mut ctx);
        }
        // check that we hit the timeout after MAX_MULTICAST_SOLICIT
        assert_eq!(
            *ctx.state().test_counters.get("ndp::neighbor_solicitation_timeout"),
            1,
            "timeout counter at zero"
        );
    }

    #[test]
    fn test_address_resolution() {
        set_logger_for_test();
        let mut local = DummyEventDispatcherBuilder::default();
        local.add_device(TEST_LOCAL_MAC);
        let mut remote = DummyEventDispatcherBuilder::default();
        remote.add_device(TEST_REMOTE_MAC);
        let device_id = DeviceId::new_ethernet(0);

        let mut net = DummyNetwork::new(
            vec![("local", local.build()), ("remote", remote.build())].into_iter(),
            |ctx, dev| {
                if *ctx == "local" {
                    vec![("remote", device_id, None)]
                } else {
                    vec![("local", device_id, None)]
                }
            },
        );

        // let's try to ping the remote device from the local device:
        let req = IcmpEchoRequest::new(0, 0);
        let req_body = &[1, 2, 3, 4];
        let body = Buf::new(req_body.to_vec(), ..).encapsulate(
            IcmpPacketBuilder::<Ipv6, &[u8], _>::new(local_ip(), remote_ip(), IcmpUnusedCode, req),
        );
        // Manually assigning the addresses
        add_ip_addr_subnet(
            net.context("local"),
            device_id,
            AddrSubnet::new(local_ip().get(), 128).unwrap(),
        )
        .unwrap();
        add_ip_addr_subnet(
            net.context("remote"),
            device_id,
            AddrSubnet::new(remote_ip().get(), 128).unwrap(),
        )
        .unwrap();
        assert_eq!(net.context("local").dispatcher.frames_sent().len(), 0);
        assert_eq!(net.context("remote").dispatcher.frames_sent().len(), 0);

        crate::ip::send_ip_packet_from_device(
            net.context("local"),
            device_id,
            local_ip().get(),
            remote_ip().get(),
            remote_ip(),
            IpProto::Icmpv6,
            body,
            None,
        );
        // this should've triggered a neighbor solicitation to come out of local
        assert_eq!(net.context("local").dispatcher.frames_sent().len(), 1);
        // and a timer should've been started.
        assert_eq!(net.context("local").dispatcher.timer_events().count(), 1);

        net.step();
        // Neighbor entry for remote should be marked as Incomplete.
        assert_eq!(
            EthernetNdpDevice::get_ndp_state_mut::<_>(
                net.context("local").state_mut(),
                device_id.id()
            )
            .neighbors
            .get_neighbor_state(&remote_ip())
            .unwrap()
            .state,
            NeighborEntryState::Incomplete { transmit_counter: 1 }
        );

        assert_eq!(
            *net.context("remote").state().test_counters.get("ndp::rx_neighbor_solicitation"),
            1,
            "remote received solicitation"
        );
        assert_eq!(net.context("remote").dispatcher.frames_sent().len(), 1);

        // forward advertisement response back to local
        net.step();

        assert_eq!(
            *net.context("local").state().test_counters.get("ndp::rx_neighbor_advertisement"),
            1,
            "local received advertisement"
        );

        // at the end of the exchange, both sides should have each other on
        // their ndp tables:
        let local_neighbor = EthernetNdpDevice::get_ndp_state_mut::<_>(
            net.context("local").state_mut(),
            device_id.id(),
        )
        .neighbors
        .get_neighbor_state(&remote_ip())
        .unwrap();
        assert_eq!(local_neighbor.link_address.unwrap(), TEST_REMOTE_MAC,);
        // Remote must be reachable from local since it responded with
        // an NA message with the solicited flag set.
        assert_eq!(local_neighbor.state, NeighborEntryState::Reachable,);

        let remote_neighbor = EthernetNdpDevice::get_ndp_state_mut::<_>(
            net.context("remote").state_mut(),
            device_id.id(),
        )
        .neighbors
        .get_neighbor_state(&local_ip())
        .unwrap();
        assert_eq!(remote_neighbor.link_address.unwrap(), TEST_LOCAL_MAC,);
        // Local must be marked as stale because remote got an NS from it
        // but has not itself sent any packets to it and confirmed that
        // local actually received it.
        assert_eq!(remote_neighbor.state, NeighborEntryState::Stale);

        // and the local timer should've been unscheduled:
        assert_eq!(net.context("local").dispatcher.timer_events().count(), 0);

        // upon link layer resolution, the original ping request should've been
        // sent out:
        assert_eq!(net.context("local").dispatcher.frames_sent().len(), 1);
        net.step();
        assert_eq!(
            *net.context("remote").state().test_counters.get("receive_icmpv6_packet::echo_request"),
            1
        );

        // TODO(brunodalbo): we should be able to verify that remote also sends
        //  back an echo reply, but we're having some trouble with IPv6 link
        //  local addresses.
    }

    #[test]
    fn test_deinitialize_cancels_timers() {
        // Test that associated timers are cancelled when the NDP device
        // is deinitialized.

        set_logger_for_test();
        let mut ctx = DummyEventDispatcherBuilder::default().build();
        let dev_id = ctx.state_mut().device.add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, dev_id);
        // Now we have to manually assign the IP addresses, see `EthernetNdpDevice::get_ipv6_addr`
        add_ip_addr_subnet(&mut ctx, dev_id, AddrSubnet::new(local_ip().get(), 128).unwrap());

        lookup::<DummyEventDispatcher, EthernetNdpDevice>(&mut ctx, dev_id.id(), remote_ip().get());

        // This should have scheduled a timer
        assert_eq!(ctx.dispatcher.timer_events().count(), 1);

        // Deinitializing a different ID should not impact the current timer
        deinitialize(&mut ctx, dev_id.id + 1);
        assert_eq!(ctx.dispatcher.timer_events().count(), 1);

        // Deinitializing the correct ID should cancel the timer.
        deinitialize(&mut ctx, dev_id.id);
        assert_eq!(ctx.dispatcher.timer_events().count(), 0);
    }

    #[test]
    fn test_dad_duplicate_address_detected_solicitation() {
        // Tests whether a duplicate address will get detected by solicitation
        // In this test, two nodes having the same MAC address will come up
        // at the same time. And both of them will use the EUI address. Each
        // of them should be able to detect each other is using the same address,
        // so they will both give up using that address.
        set_logger_for_test();
        let mac = Mac::new([1, 2, 3, 4, 5, 6]);
        let addr =
            AddrSubnet::<_, SpecifiedAddr<_>>::new(mac.to_ipv6_link_local().get(), 128).unwrap();
        let multicast_addr = mac.to_ipv6_link_local().get().to_solicited_node_address();
        let mut local = DummyEventDispatcherBuilder::default();
        let mut remote = DummyEventDispatcherBuilder::default();
        let device_id = DeviceId::new_ethernet(0);

        let mut stack_builder = StackStateBuilder::default();
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_max_router_solicitations(None);
        stack_builder.device_builder().set_default_ndp_configs(ndp_configs);

        // We explicitly call `build_with` when building our contexts below because `build` will
        // set the default NDP parameter DUP_ADDR_DETECT_TRANSMITS to 0 (effectively disabling
        // DAD) so we use our own custom `StackStateBuilder` to set it to the default value
        // of `1` (see `DUP_ADDR_DETECT_TRANSMITS`).
        let mut net = DummyNetwork::new(
            vec![
                ("local", local.build_with(stack_builder.clone(), DummyEventDispatcher::default())),
                ("remote", remote.build_with(stack_builder, DummyEventDispatcher::default())),
            ]
            .into_iter(),
            |ctx, dev| {
                if *ctx == "local" {
                    vec![("remote", device_id, None)]
                } else {
                    vec![("local", device_id, None)]
                }
            },
        );

        // Create the devices (will start DAD at the same time).
        assert_eq!(
            net.context("local").state_mut().add_ethernet_device(mac, IPV6_MIN_MTU),
            device_id
        );
        crate::device::initialize_device(net.context("local"), device_id);
        assert_eq!(
            net.context("remote").state_mut().add_ethernet_device(mac, IPV6_MIN_MTU),
            device_id
        );
        crate::device::initialize_device(net.context("remote"), device_id);
        assert_eq!(net.context("local").dispatcher.frames_sent().len(), 1);
        assert_eq!(net.context("remote").dispatcher.frames_sent().len(), 1);

        // Both devices should be in the solicited-node multicast group.
        assert!(is_in_ip_multicast(net.context("local"), device_id, multicast_addr));
        assert!(is_in_ip_multicast(net.context("remote"), device_id, multicast_addr));

        net.step();

        // they should now realize the address they intend to use has a duplicate
        // in the local network
        assert_eq!(
            get_ip_addr_subnets::<_, Ipv6Addr>(net.context("local").state(), device_id).count(),
            0
        );
        assert_eq!(
            get_ip_addr_subnets::<_, Ipv6Addr>(net.context("remote").state(), device_id).count(),
            0
        );

        // Both devices should not be in the multicast group
        assert!(!is_in_ip_multicast(net.context("local"), device_id, multicast_addr));
        assert!(!is_in_ip_multicast(net.context("remote"), device_id, multicast_addr));
    }

    #[test]
    fn test_dad_duplicate_address_detected_advertisement() {
        // Tests whether a duplicate address will get detected by advertisement
        // In this test, one of the node first assigned itself the local_ip(),
        // then the second node comes up and it should be able to find out that
        // it cannot use the address because someone else has already taken that
        // address.
        set_logger_for_test();
        let mut local = DummyEventDispatcherBuilder::default();
        local.add_device(TEST_LOCAL_MAC);
        let mut remote = DummyEventDispatcherBuilder::default();
        remote.add_device(TEST_REMOTE_MAC);
        let device_id = DeviceId::new_ethernet(0);

        let mut net = DummyNetwork::new(
            vec![
                ("local", local.build::<DummyEventDispatcher>()),
                ("remote", remote.build::<DummyEventDispatcher>()),
            ]
            .into_iter(),
            |ctx, dev| {
                if *ctx == "local" {
                    vec![("remote", device_id, None)]
                } else {
                    vec![("local", device_id, None)]
                }
            },
        );

        // Enable DAD.
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_max_router_solicitations(None);
        crate::device::set_ndp_configurations(net.context("local"), device_id, ndp_configs.clone());
        crate::device::set_ndp_configurations(net.context("remote"), device_id, ndp_configs);

        println!("Setting new IP on local");

        let addr = AddrSubnet::new(local_ip().get(), 128).unwrap();
        let multicast_addr = local_ip().to_solicited_node_address();
        add_ip_addr_subnet(net.context("local"), device_id, addr).unwrap();
        // Only local should be in the solicited node multicast group.
        assert!(is_in_ip_multicast(net.context("local"), device_id, multicast_addr));
        assert!(!is_in_ip_multicast(net.context("remote"), device_id, multicast_addr));
        assert!(testutil::trigger_next_timer(net.context("local")));

        assert!(EthernetNdpDevice::ipv6_addr_state(
            net.context("local").state(),
            device_id.id(),
            &local_ip()
        )
        .unwrap()
        .is_assigned());

        println!("Set new IP on remote");

        add_ip_addr_subnet(net.context("remote"), device_id, addr).unwrap();
        // local & remote should be in the multicast group.
        assert!(is_in_ip_multicast(net.context("local"), device_id, multicast_addr));
        assert!(is_in_ip_multicast(net.context("remote"), device_id, multicast_addr));

        net.step();

        assert_eq!(
            get_ip_addr_subnets::<_, Ipv6Addr>(net.context("remote").state(), device_id).count(),
            0
        );
        // let's make sure that our local node still can use that address
        assert!(EthernetNdpDevice::ipv6_addr_state(
            net.context("local").state(),
            device_id.id(),
            &local_ip()
        )
        .unwrap()
        .is_assigned());

        // Only local should be in the solicited node multicast group.
        assert!(is_in_ip_multicast(net.context("local"), device_id, multicast_addr));
        assert!(!is_in_ip_multicast(net.context("remote"), device_id, multicast_addr));
    }

    #[test]
    fn test_dad_set_ipv6_address_when_ongoing() {
        // Test that we can make our tentative address change when DAD is ongoing.

        // We explicitly call `build_with` when building our context below because `build` will
        // set the default NDP parameter DUP_ADDR_DETECT_TRANSMITS to 0 (effectively disabling
        // DAD) so we use our own custom `StackStateBuilder` to set it to the default value
        // of `1` (see `DUP_ADDR_DETECT_TRANSMITS`).
        let mut ctx = DummyEventDispatcherBuilder::default()
            .build_with(StackStateBuilder::default(), DummyEventDispatcher::default());
        let dev_id = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, dev_id);
        let addr = local_ip();
        add_ip_addr_subnet(&mut ctx, dev_id, AddrSubnet::new(addr.get(), 128).unwrap()).unwrap();
        assert_eq!(
            EthernetNdpDevice::ipv6_addr_state(ctx.state(), dev_id.id(), &addr).unwrap(),
            AddressState::Tentative,
        );
        let addr = remote_ip();
        assert!(EthernetNdpDevice::ipv6_addr_state(ctx.state(), dev_id.id(), &addr).is_none(),);
        add_ip_addr_subnet(&mut ctx, dev_id, AddrSubnet::new(addr.get(), 128).unwrap()).unwrap();
        assert_eq!(
            EthernetNdpDevice::ipv6_addr_state(ctx.state(), dev_id.id(), &addr).unwrap(),
            AddressState::Tentative,
        );
    }

    #[test]
    fn test_dad_three_transmits_no_conflicts() {
        let mut stack_builder = StackStateBuilder::default();
        let mut ndp_configs = crate::device::ndp::NdpConfigurations::default();
        ndp_configs.set_max_router_solicitations(None);
        ndp_configs.set_dup_addr_detect_transmits(None);
        stack_builder.device_builder().set_default_ndp_configs(ndp_configs);
        let mut ctx = Context::new(stack_builder.build(), DummyEventDispatcher::default());
        let dev_id = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, dev_id);

        // Enable DAD.
        EthernetNdpDevice::get_ndp_state_mut(&mut ctx.state_mut(), dev_id.id())
            .set_dad_transmits(NonZeroU8::new(3));
        add_ip_addr_subnet(&mut ctx, dev_id, AddrSubnet::new(local_ip().get(), 128).unwrap())
            .unwrap();
        for i in 0..3 {
            assert!(testutil::trigger_next_timer(&mut ctx));
        }
        assert!(EthernetNdpDevice::ipv6_addr_state(ctx.state(), dev_id.id(), &local_ip())
            .unwrap()
            .is_assigned());
    }

    #[test]
    fn test_dad_three_transmits_with_conflicts() {
        // test if the implementation is correct when we have more than 1
        // NS packets to send.
        set_logger_for_test();
        let mac = Mac::new([1, 2, 3, 4, 5, 6]);
        let mut local = DummyEventDispatcherBuilder::default();
        local.add_device(mac);
        let mut remote = DummyEventDispatcherBuilder::default();
        remote.add_device(mac);
        let device_id = DeviceId::new_ethernet(0);
        let mut net = DummyNetwork::new(
            vec![("local", local.build()), ("remote", remote.build())].into_iter(),
            |ctx, dev| {
                if *ctx == "local" {
                    vec![("remote", device_id, None)]
                } else {
                    vec![("local", device_id, None)]
                }
            },
        );

        EthernetNdpDevice::get_ndp_state_mut(&mut net.context("local").state_mut(), device_id.id())
            .set_dad_transmits(NonZeroU8::new(3));
        EthernetNdpDevice::get_ndp_state_mut(
            &mut net.context("remote").state_mut(),
            device_id.id(),
        )
        .set_dad_transmits(NonZeroU8::new(3));

        add_ip_addr_subnet(
            net.context("local"),
            device_id,
            AddrSubnet::new(local_ip().get(), 128).unwrap(),
        )
        .unwrap();
        // during the first and second period, the remote host is still down.
        assert!(testutil::trigger_next_timer(net.context("local")));
        assert!(testutil::trigger_next_timer(net.context("local")));
        add_ip_addr_subnet(
            net.context("remote"),
            device_id,
            AddrSubnet::new(local_ip().get(), 128).unwrap(),
        )
        .unwrap();
        // the local host should have sent out 3 packets while the remote one
        // should only have sent out 1.
        assert_eq!(net.context("local").dispatcher.frames_sent().len(), 3);
        assert_eq!(net.context("remote").dispatcher.frames_sent().len(), 1);

        net.step();

        // lets make sure that all timers are cancelled properly
        assert_eq!(net.context("local").dispatcher.timer_events().count(), 0);
        assert_eq!(net.context("remote").dispatcher.timer_events().count(), 0);

        // they should now realize the address they intend to use has a duplicate
        // in the local network
        assert_eq!(
            get_ip_addr_subnets::<_, Ipv6Addr>(net.context("local").state(), device_id).count(),
            0
        );
        assert_eq!(
            get_ip_addr_subnets::<_, Ipv6Addr>(net.context("remote").state(), device_id).count(),
            0
        );
    }

    #[test]
    fn test_dad_multiple_ips_simultaneously() {
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let dev_id = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, dev_id);

        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);

        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_dup_addr_detect_transmits(NonZeroU8::new(3));
        ndp_configs.set_max_router_solicitations(None);
        crate::device::set_ndp_configurations(&mut ctx, dev_id, ndp_configs);

        // Add an IP.
        add_ip_addr_subnet(&mut ctx, dev_id, AddrSubnet::new(local_ip().get(), 128).unwrap())
            .unwrap();
        assert!(get_ip_addr_state(ctx.state(), dev_id, &local_ip()).unwrap().is_tentative());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);

        // Send another NS.
        run_for(&mut ctx, Duration::from_secs(1));
        assert_eq!(ctx.dispatcher().frames_sent().len(), 2);

        // Add another IP
        add_ip_addr_subnet(&mut ctx, dev_id, AddrSubnet::new(remote_ip().get(), 128).unwrap())
            .unwrap();
        assert!(get_ip_addr_state(ctx.state(), dev_id, &local_ip()).unwrap().is_tentative());
        assert!(get_ip_addr_state(ctx.state(), dev_id, &remote_ip()).unwrap().is_tentative());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 3);

        // Run to the end for DAD for local ip
        run_for(&mut ctx, Duration::from_secs(2));
        assert!(get_ip_addr_state(ctx.state(), dev_id, &local_ip()).unwrap().is_assigned());
        assert!(get_ip_addr_state(ctx.state(), dev_id, &remote_ip()).unwrap().is_tentative());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 6);

        // Run to the end for DAD for local ip
        run_for(&mut ctx, Duration::from_secs(1));
        assert!(get_ip_addr_state(ctx.state(), dev_id, &local_ip()).unwrap().is_assigned());
        assert!(get_ip_addr_state(ctx.state(), dev_id, &remote_ip()).unwrap().is_assigned());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 6);

        // No more timers.
        assert!(!trigger_next_timer(&mut ctx));
    }

    #[test]
    fn test_dad_cancel_when_ip_removed() {
        let mut stack_builder = StackStateBuilder::default();

        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let dev_id = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, dev_id);

        // Enable DAD.
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_dup_addr_detect_transmits(NonZeroU8::new(3));
        ndp_configs.set_max_router_solicitations(None);
        crate::device::set_ndp_configurations(&mut ctx, dev_id, ndp_configs);

        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);

        // Add an IP.
        add_ip_addr_subnet(&mut ctx, dev_id, AddrSubnet::new(local_ip().get(), 128).unwrap())
            .unwrap();
        assert!(get_ip_addr_state(ctx.state(), dev_id, &local_ip()).unwrap().is_tentative());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);

        // Send another NS.
        run_for(&mut ctx, Duration::from_secs(1));
        assert_eq!(ctx.dispatcher().frames_sent().len(), 2);

        // Add another IP
        add_ip_addr_subnet(&mut ctx, dev_id, AddrSubnet::new(remote_ip().get(), 128).unwrap())
            .unwrap();
        assert!(get_ip_addr_state(ctx.state(), dev_id, &local_ip()).unwrap().is_tentative());
        assert!(get_ip_addr_state(ctx.state(), dev_id, &remote_ip()).unwrap().is_tentative());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 3);

        // Run 1s
        run_for(&mut ctx, Duration::from_secs(1));
        assert!(get_ip_addr_state(ctx.state(), dev_id, &local_ip()).unwrap().is_tentative());
        assert!(get_ip_addr_state(ctx.state(), dev_id, &remote_ip()).unwrap().is_tentative());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 5);

        // Remove local ip
        del_ip_addr(&mut ctx, dev_id, &local_ip()).unwrap();
        assert!(get_ip_addr_state(ctx.state(), dev_id, &local_ip()).is_none());
        assert!(get_ip_addr_state(ctx.state(), dev_id, &remote_ip()).unwrap().is_tentative());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 5);

        // Run to the end for DAD for local ip
        run_for(&mut ctx, Duration::from_secs(2));
        assert!(get_ip_addr_state(ctx.state(), dev_id, &local_ip()).is_none());
        assert!(get_ip_addr_state(ctx.state(), dev_id, &remote_ip()).unwrap().is_assigned());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 6);

        // No more timers.
        assert!(!trigger_next_timer(&mut ctx));
    }

    #[test]
    fn test_receiving_router_solicitation_validity_check() {
        let config = get_dummy_config::<Ipv6Addr>();
        let src_ip = Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 192, 168, 0, 10]);
        let src_mac = [10, 11, 12, 13, 14, 15];
        let options = vec![NdpOption::SourceLinkLayerAddress(&src_mac[..])];

        //
        // Test receiving NDP RS when not a router (should not receive)
        //

        let mut ctx = DummyEventDispatcherBuilder::from_config(config.clone())
            .build::<DummyEventDispatcher>();
        let device = Some(DeviceId::new_ethernet(0));

        let mut icmpv6_packet_buf = OptionsSerializer::new(options.iter())
            .into_serializer()
            .encapsulate(IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                src_ip,
                config.local_ip,
                IcmpUnusedCode,
                RouterSolicitation::default(),
            ))
            .serialize_vec_outer()
            .unwrap();
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip, config.local_ip))
            .unwrap();

        ctx.receive_ndp_packet(device, src_ip, config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_solicitation"), 0);

        //
        // Test receiving NDP RS as a router (should receive)
        //

        let mut state_builder = StackStateBuilder::default();
        state_builder.ipv6_builder().forward(true);
        let mut ctx = DummyEventDispatcherBuilder::from_config(config.clone())
            .build_with(state_builder, DummyEventDispatcher::default());
        set_routing_enabled::<_, Ipv6>(&mut ctx, DeviceId::new_ethernet(0), true);
        icmpv6_packet_buf.reset();
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip, config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(device, src_ip, config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_solicitation"), 1);

        //
        // Test receiving NDP RS as a router, but source is unspecified and the source
        // link layer option is included (should not receive)
        //

        let unspecified_source = Ipv6Addr::default();
        let mut icmpv6_packet_buf = OptionsSerializer::new(options.iter())
            .into_serializer()
            .encapsulate(IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                unspecified_source,
                config.local_ip,
                IcmpUnusedCode,
                RouterSolicitation::default(),
            ))
            .serialize_vec_outer()
            .unwrap();
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(
                unspecified_source,
                config.local_ip,
            ))
            .unwrap();
        ctx.receive_ndp_packet(device, unspecified_source, config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_solicitation"), 1);
    }

    #[test]
    fn test_receiving_router_advertisement_validity_check() {
        let config = get_dummy_config::<Ipv6Addr>();
        let src_mac = [10, 11, 12, 13, 14, 15];
        let src_ip = Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 192, 168, 0, 10]);
        let mut ctx = DummyEventDispatcherBuilder::from_config(config.clone())
            .build::<DummyEventDispatcher>();
        let device = Some(DeviceId::new_ethernet(0));

        //
        // Test receiving NDP RA where source ip is not a link local address (should not receive)
        //

        let mut icmpv6_packet_buf =
            router_advertisement_message(src_ip, config.local_ip.get(), 1, false, false, 3, 4, 5);
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip, config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(device, src_ip, config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 0);

        //
        // Test receiving NDP RA where source ip is a link local address (should receive)
        //

        let src_ip = Mac::new(src_mac).to_ipv6_link_local().get();
        let mut icmpv6_packet_buf =
            router_advertisement_message(src_ip, config.local_ip.get(), 1, false, false, 3, 4, 5);
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip, config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(device, src_ip, config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 1);
    }

    #[test]
    fn test_receiving_router_advertisement_fixed_message() {
        let config = get_dummy_config::<Ipv6Addr>();
        let mut ctx = DummyEventDispatcherBuilder::from_config(config.clone())
            .build::<DummyEventDispatcher>();
        let device = Some(DeviceId::new_ethernet(0));
        let device_id = device.map(|x| x.id()).unwrap();
        let src_ip = config.remote_mac.to_ipv6_link_local();

        //
        // Receive a router advertisement for a brand new router with a valid lifetime.
        //

        let mut icmpv6_packet_buf = router_advertisement_message(
            src_ip.get(),
            config.local_ip.get(),
            1,
            false,
            false,
            3,
            4,
            5,
        );
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
            .unwrap();
        assert!(!EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id)
            .has_default_router(&src_ip));
        ctx.receive_ndp_packet(device, src_ip.get(), config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 1);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        // We should have the new router in our list with our NDP parameters updated.
        assert!(ndp_state.has_default_router(&src_ip));
        let base = Duration::from_millis(4);
        let min_reachable = base / 2;
        let max_reachable = min_reachable * 3;
        assert_eq!(ndp_state.base_reachable_time, base);
        assert!(
            ndp_state.reachable_time >= min_reachable && ndp_state.reachable_time <= max_reachable
        );
        assert_eq!(ndp_state.retrans_timer, Duration::from_millis(5));
        assert_eq!(get_ipv6_hop_limit(&ctx, device.unwrap()).get(), 1);

        //
        // Receive a new router advertisement for the same router with a valid lifetime.
        //

        let mut icmpv6_packet_buf = router_advertisement_message(
            src_ip.get(),
            config.local_ip.get(),
            7,
            false,
            false,
            9,
            10,
            11,
        );
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(device, src_ip.get(), config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 2);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        assert!(ndp_state.has_default_router(&src_ip));
        let base = Duration::from_millis(10);
        let min_reachable = base / 2;
        let max_reachable = min_reachable * 3;
        let reachable_time = ndp_state.reachable_time;
        assert_eq!(ndp_state.base_reachable_time, base);
        assert!(
            ndp_state.reachable_time >= min_reachable && ndp_state.reachable_time <= max_reachable
        );
        assert_eq!(ndp_state.retrans_timer, Duration::from_millis(11));
        assert_eq!(get_ipv6_hop_limit(&ctx, device.unwrap()).get(), 7);

        //
        // Receive a new router advertisement for the same router with a valid lifetime and
        // zero valued parameters.
        //

        // Zero value for Reachable Time should not update base_reachable_time.
        // Other non zero values should update.
        let mut icmpv6_packet_buf = router_advertisement_message(
            src_ip.get(),
            config.local_ip.get(),
            13,
            false,
            false,
            15,
            0,
            17,
        );
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(device, src_ip.get(), config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 3);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        assert!(ndp_state.has_default_router(&src_ip));
        // Should be the same value as before.
        assert_eq!(ndp_state.base_reachable_time, base);
        // Should be the same randomly calculated value as before.
        assert_eq!(ndp_state.reachable_time, reachable_time);
        // Should update to new value.
        assert_eq!(ndp_state.retrans_timer, Duration::from_millis(17));
        // Should update to new value.
        assert_eq!(get_ipv6_hop_limit(&ctx, device.unwrap()).get(), 13);

        // Zero value for Retransmit Time should not update our retrans_time.
        // Other non zero values should update.
        let mut icmpv6_packet_buf = router_advertisement_message(
            src_ip.get(),
            config.local_ip.get(),
            19,
            false,
            false,
            21,
            22,
            0,
        );
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(device, src_ip.get(), config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 4);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        assert!(ndp_state.has_default_router(&src_ip));
        // Should update to new value.
        let base = Duration::from_millis(22);
        let min_reachable = base / 2;
        let max_reachable = min_reachable * 3;
        assert_eq!(ndp_state.base_reachable_time, base);
        assert!(
            ndp_state.reachable_time >= min_reachable && ndp_state.reachable_time <= max_reachable
        );
        // Should be the same value as before.
        assert_eq!(ndp_state.retrans_timer, Duration::from_millis(17));
        // Should update to new value.
        assert_eq!(get_ipv6_hop_limit(&ctx, device.unwrap()).get(), 19);

        // Zero value for CurrHopLimit should not update our hop_limit.
        // Other non zero values should update.
        let mut icmpv6_packet_buf = router_advertisement_message(
            src_ip.get(),
            config.local_ip.get(),
            0,
            false,
            false,
            27,
            28,
            29,
        );
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(device, src_ip.get(), config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 5);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        assert!(ndp_state.has_default_router(&src_ip));
        // Should update to new value.
        let base = Duration::from_millis(28);
        let min_reachable = base / 2;
        let max_reachable = min_reachable * 3;
        assert_eq!(ndp_state.base_reachable_time, base);
        assert!(
            ndp_state.reachable_time >= min_reachable && ndp_state.reachable_time <= max_reachable
        );
        // Should update to new value.
        assert_eq!(ndp_state.retrans_timer, Duration::from_millis(29));
        // Should be the same value as before.
        assert_eq!(get_ipv6_hop_limit(&ctx, device.unwrap()).get(), 19);

        //
        // Receive new router advertisement with 0 router lifetime, but new parameters.
        //

        let mut icmpv6_packet_buf = router_advertisement_message(
            src_ip.get(),
            config.local_ip.get(),
            31,
            false,
            false,
            0,
            34,
            35,
        );
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(device, src_ip.get(), config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 6);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        // Router should no longer be in our list.
        assert!(!ndp_state.has_default_router(&src_ip));
        let base = Duration::from_millis(34);
        let min_reachable = base / 2;
        let max_reachable = min_reachable * 3;
        let reachable_time = ndp_state.reachable_time;
        assert_eq!(ndp_state.base_reachable_time, base);
        assert!(
            ndp_state.reachable_time >= min_reachable && ndp_state.reachable_time <= max_reachable
        );
        assert_eq!(ndp_state.retrans_timer, Duration::from_millis(35));
        assert_eq!(get_ipv6_hop_limit(&ctx, device.unwrap()).get(), 31);

        // Router invalidation timeout must have been cleared since we invalided with the
        // received router advertisement with lifetime 0.
        assert!(!trigger_next_timer(&mut ctx));

        //
        // Receive new router advertisement with non-0 router lifetime, but let it get invalidated
        //

        let mut icmpv6_packet_buf = router_advertisement_message(
            src_ip.get(),
            config.local_ip.get(),
            37,
            false,
            false,
            39,
            40,
            41,
        );
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(device, src_ip.get(), config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 7);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        // Router should be re-added.
        assert!(ndp_state.has_default_router(&src_ip));
        let base = Duration::from_millis(40);
        let min_reachable = base / 2;
        let max_reachable = min_reachable * 3;
        let reachable_time = ndp_state.reachable_time;
        assert_eq!(ndp_state.base_reachable_time, base);
        assert!(
            ndp_state.reachable_time >= min_reachable && ndp_state.reachable_time <= max_reachable
        );
        assert_eq!(ndp_state.retrans_timer, Duration::from_millis(41));
        assert_eq!(get_ipv6_hop_limit(&ctx, device.unwrap()).get(), 37);

        // Invaldate the router by triggering the timeout.
        assert!(trigger_next_timer(&mut ctx));
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        assert!(!ndp_state.has_default_router(&src_ip));

        // No more timers.
        assert!(!trigger_next_timer(&mut ctx));
    }

    #[test]
    fn test_sending_ipv6_packet_after_hop_limit_change() {
        // Sets the hop limit with a router advertisement
        // and sends a packet to make sure the packet uses
        // the new hop limit.
        fn inner_test(ctx: &mut Context<DummyEventDispatcher>, hop_limit: u8, frame_offset: usize) {
            let config = get_dummy_config::<Ipv6Addr>();
            let device = Some(DeviceId::new_ethernet(0));
            let device_id = device.map(|x| x.id()).unwrap();
            let src_ip = config.remote_mac.to_ipv6_link_local();

            let mut icmpv6_packet_buf = router_advertisement_message(
                src_ip.get(),
                config.local_ip.get(),
                hop_limit,
                false,
                false,
                0,
                0,
                0,
            );
            let icmpv6_packet = icmpv6_packet_buf
                .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
                .unwrap();
            assert!(!EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id)
                .has_default_router(&src_ip));
            ctx.receive_ndp_packet(device, src_ip.get(), config.local_ip, icmpv6_packet);
            assert_eq!(get_ipv6_hop_limit(&ctx, device.unwrap()).get(), hop_limit);
            crate::ip::send_ip_packet_from_device(
                ctx,
                device.unwrap(),
                config.local_ip.get(),
                config.remote_ip.get(),
                config.remote_ip,
                IpProto::Tcp,
                Buf::new(vec![0; 10], ..),
                None,
            )
            .unwrap();
            let (buf, _, _, _) =
                parse_ethernet_frame(&ctx.dispatcher().frames_sent()[frame_offset].1[..]).unwrap();
            // Packet's hop limit should be 100.
            assert_eq!(buf[7], hop_limit);
        }

        let mut ctx = DummyEventDispatcherBuilder::from_config(get_dummy_config::<Ipv6Addr>())
            .build::<DummyEventDispatcher>();

        // Set hop limit to 100.
        inner_test(&mut ctx, 100, 0);

        // Set hop limit to 30.
        inner_test(&mut ctx, 30, 1);
    }

    #[test]
    fn test_receiving_router_advertisement_source_link_layer_option() {
        let config = get_dummy_config::<Ipv6Addr>();
        let mut ctx = DummyEventDispatcherBuilder::from_config(config.clone())
            .build::<DummyEventDispatcher>();
        let device = Some(DeviceId::new_ethernet(0));
        let device_id = device.map(|x| x.id()).unwrap();
        let src_mac = Mac::new([10, 11, 12, 13, 14, 15]);
        let src_ip = src_mac.to_ipv6_link_local();
        let src_mac_bytes = src_mac.bytes();
        let options = vec![NdpOption::SourceLinkLayerAddress(&src_mac_bytes[..])];

        //
        // First receive a Router Advertisement without the source link layer and
        // make sure no new neighbor gets added.
        //

        let mut icmpv6_packet_buf = router_advertisement_message(
            src_ip.get(),
            config.local_ip.get(),
            1,
            false,
            false,
            3,
            4,
            5,
        );
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
            .unwrap();
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        assert!(!ndp_state.has_default_router(&src_ip));
        assert!(ndp_state.neighbors.get_neighbor_state(&src_ip).is_none());
        ctx.receive_ndp_packet(device, src_ip.get(), config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 1);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        // We should have the new router in our list with our NDP parameters updated.
        assert!(ndp_state.has_default_router(&src_ip));
        // Should still not have a neighbor added.
        assert!(ndp_state.neighbors.get_neighbor_state(&src_ip).is_none());

        //
        // Receive a new RA but with the source link layer option
        //

        let mut icmpv6_packet_buf = OptionsSerializer::new(options.iter())
            .into_serializer()
            .encapsulate(IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                src_ip.get(),
                config.local_ip,
                IcmpUnusedCode,
                RouterAdvertisement::new(1, false, false, 3, 4, 5),
            ))
            .serialize_vec_outer()
            .unwrap();
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(device, src_ip.get(), config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 2);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        assert!(ndp_state.has_default_router(&src_ip));
        let neighbor = ndp_state.neighbors.get_neighbor_state(&src_ip).unwrap();
        assert_eq!(neighbor.link_address.unwrap(), src_mac);
        assert!(neighbor.is_router);
        // Router should be marked stale as a neighbor.
        assert_eq!(neighbor.state, NeighborEntryState::Stale);

        // Trigger router invalidation.
        assert!(trigger_next_timer(&mut ctx));

        // Neighbor entry shouldn't change except for `is_router` which should now be `false`.
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        assert!(!ndp_state.has_default_router(&src_ip));
        let neighbor = ndp_state.neighbors.get_neighbor_state(&src_ip).unwrap();
        assert_eq!(neighbor.link_address.unwrap(), src_mac);
        assert!(!neighbor.is_router);
        assert_eq!(neighbor.state, NeighborEntryState::Stale);
    }

    #[test]
    fn test_receiving_router_advertisement_mtu_option() {
        fn packet_buf(src_ip: Ipv6Addr, dst_ip: Ipv6Addr, mtu: u32) -> Buf<Vec<u8>> {
            let options = &[NdpOption::MTU(mtu)];
            OptionsSerializer::new(options.iter())
                .into_serializer()
                .encapsulate(IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                    src_ip,
                    dst_ip,
                    IcmpUnusedCode,
                    RouterAdvertisement::new(1, false, false, 3, 4, 5),
                ))
                .serialize_vec_outer()
                .unwrap()
                .unwrap_b()
        }

        let config = get_dummy_config::<Ipv6Addr>();
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let hw_mtu = 5000;
        let device = ctx.state.add_ethernet_device(TEST_LOCAL_MAC, hw_mtu);
        let device_id = device.id();
        let src_mac = Mac::new([10, 11, 12, 13, 14, 15]);
        let src_ip = src_mac.to_ipv6_link_local();

        crate::device::initialize_device(&mut ctx, device);

        //
        // Receive a new RA with a valid mtu option (but the new mtu should only be 5000
        // as that is the max MTU of the device).
        //

        let mut icmpv6_packet_buf = packet_buf(src_ip.get(), config.local_ip.get(), 5781);
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(Some(device), src_ip.get(), config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 1);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        assert!(ndp_state.has_default_router(&src_ip));
        assert_eq!(get_mtu(ctx.state(), device), hw_mtu);

        //
        // Receive a new RA with an invalid MTU option (value is lower than IPv6 min mtu)
        //

        let mut icmpv6_packet_buf =
            packet_buf(src_ip.get(), config.local_ip.get(), crate::ip::path_mtu::IPV6_MIN_MTU - 1);
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(Some(device), src_ip.get(), config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 2);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        assert!(ndp_state.has_default_router(&src_ip));
        assert_eq!(get_mtu(ctx.state(), device), hw_mtu);

        //
        // Receive a new RA with a valid MTU option (value is exactly IPv6 min mtu)
        //

        let mut icmpv6_packet_buf =
            packet_buf(src_ip.get(), config.local_ip.get(), crate::ip::path_mtu::IPV6_MIN_MTU);
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(Some(device), src_ip.get(), config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 3);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        assert!(ndp_state.has_default_router(&src_ip));
        assert_eq!(get_mtu(ctx.state(), device), crate::ip::path_mtu::IPV6_MIN_MTU);
    }

    #[test]
    fn test_receiving_router_advertisement_prefix_option() {
        fn packet_buf(
            src_ip: Ipv6Addr,
            dst_ip: Ipv6Addr,
            prefix: Ipv6Addr,
            prefix_length: u8,
            on_link_flag: bool,
            autonomous_address_configuration_flag: bool,
            valid_lifetime: u32,
            preferred_lifetime: u32,
        ) -> Buf<Vec<u8>> {
            let p = PrefixInformation::new(
                prefix_length,
                on_link_flag,
                autonomous_address_configuration_flag,
                valid_lifetime,
                preferred_lifetime,
                prefix,
            );
            let options = &[NdpOption::PrefixInformation(&p)];
            OptionsSerializer::new(options.iter())
                .into_serializer()
                .encapsulate(IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                    src_ip,
                    dst_ip,
                    IcmpUnusedCode,
                    RouterAdvertisement::new(1, false, false, 0, 4, 5),
                ))
                .serialize_vec_outer()
                .unwrap()
                .unwrap_b()
        }

        let config = get_dummy_config::<Ipv6Addr>();
        let mut ctx = DummyEventDispatcherBuilder::from_config(config.clone())
            .build::<DummyEventDispatcher>();
        let device = DeviceId::new_ethernet(0);
        let device_id = device.id();
        let src_mac = Mac::new([10, 11, 12, 13, 14, 15]);
        let src_ip = src_mac.to_ipv6_link_local().get();
        let prefix = Ipv6Addr::new([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 192, 168, 0, 0]);
        let prefix_length = 120;
        let addr_subnet = AddrSubnet::new(prefix, prefix_length).unwrap();

        //
        // Receive a new RA with new prefix.
        //

        let mut icmpv6_packet_buf =
            packet_buf(src_ip, config.local_ip.get(), prefix, prefix_length, true, false, 100, 0);
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip, config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(Some(device), src_ip, config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 1);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        // Prefix should be in our list now.
        assert!(ndp_state.has_prefix(&addr_subnet));
        // Invalidation timeout should be set.
        assert_eq!(ctx.dispatcher().timer_events().count(), 1);

        //
        // Receive a RA with same prefix but valid_lifetime = 0;
        //

        let mut icmpv6_packet_buf =
            packet_buf(src_ip, config.local_ip.get(), prefix, prefix_length, true, false, 0, 0);
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip, config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(Some(device), src_ip, config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 2);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        // Should remove the prefix from our list now.
        assert!(!ndp_state.has_prefix(&addr_subnet));
        // Invalidation timeout should be unset.
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        //
        // Receive a new RA with new prefix (same as before but new since it isn't in our list
        // right now).
        //

        let mut icmpv6_packet_buf =
            packet_buf(src_ip, config.local_ip.get(), prefix, prefix_length, true, false, 100, 0);
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip, config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(Some(device), src_ip, config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 3);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        // Prefix should be in our list now.
        assert!(ndp_state.has_prefix(&addr_subnet));
        // Invalidation timeout should be set.
        assert_eq!(ctx.dispatcher().timer_events().count(), 1);

        //
        // Receive the exact same RA as before.
        //

        let mut icmpv6_packet_buf =
            packet_buf(src_ip, config.local_ip.get(), prefix, prefix_length, true, false, 100, 0);
        let icmpv6_packet = icmpv6_packet_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip, config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(Some(device), src_ip, config.local_ip, icmpv6_packet);
        assert_eq!(get_counter_val(&mut ctx, "ndp::rx_router_advertisement"), 4);
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        // Prefix should be in our list still.
        assert!(ndp_state.has_prefix(&addr_subnet));
        // Invalidation timeout should still be set.
        assert_eq!(ctx.dispatcher().timer_events().count(), 1);

        //
        // Timeout the prefix.
        //

        assert!(trigger_next_timer(&mut ctx));

        // Prefix should no longer be in our list.
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device_id);
        assert!(!ndp_state.has_prefix(&addr_subnet));

        // No more timers.
        assert!(!trigger_next_timer(&mut ctx));
    }

    #[test]
    fn test_host_send_router_solicitations() {
        fn validate_params(
            src_mac: Mac,
            src_ip: Ipv6Addr,
            message: RouterSolicitation,
            code: IcmpUnusedCode,
        ) {
            let dummy_config = get_dummy_config::<Ipv6Addr>();
            assert_eq!(src_mac, dummy_config.local_mac);
            assert_eq!(src_ip, dummy_config.local_mac.to_ipv6_link_local().get());
            assert_eq!(message, RouterSolicitation::default());
            assert_eq!(code, IcmpUnusedCode);
        }

        //
        // By default, we should send `MAX_RTR_SOLICITATIONS` number of Router
        // Solicitation messages.
        //

        let dummy_config = get_dummy_config::<Ipv6Addr>();

        let mut stack_builder = StackStateBuilder::default();
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_dup_addr_detect_transmits(None);
        stack_builder.device_builder().set_default_ndp_configs(ndp_configs);
        let mut ctx = Context::new(stack_builder.build(), DummyEventDispatcher::default());

        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        let device_id = ctx.state.add_ethernet_device(dummy_config.local_mac, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device_id);
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);

        let time = ctx.dispatcher().now();
        assert!(trigger_next_timer(&mut ctx));
        // Initial router solicitation should be a random delay between 0 and
        // `MAX_RTR_SOLICITATION_DELAY`.
        assert!(ctx.dispatcher().now().duration_since(time) < MAX_RTR_SOLICITATION_DELAY);
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        let (src_mac, dst_mac, src_ip, dst_ip, message, code) =
            parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv6, _, RouterSolicitation, _>(
                &ctx.dispatcher().frames_sent()[0].1,
                |_| {},
            )
            .unwrap();
        validate_params(src_mac, src_ip, message, code);

        // Should get 2 more router solicitation messages
        let time = ctx.dispatcher().now();
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher().now().duration_since(time), RTR_SOLICITATION_INTERVAL);
        let (src_mac, dst_mac, src_ip, dst_ip, message, code) =
            parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv6, _, RouterSolicitation, _>(
                &ctx.dispatcher().frames_sent()[1].1,
                |_| {},
            )
            .unwrap();
        validate_params(src_mac, src_ip, message, code);

        // Before the next one, lets assign an IP address (DAD won't be performed so
        // it will be assigned immediately. The router solicitation message will
        // now use the new assigned IP as the source.
        add_ip_addr_subnet(
            &mut ctx,
            device_id,
            AddrSubnet::new(dummy_config.local_ip.get(), 128).unwrap(),
        )
        .unwrap();
        let time = ctx.dispatcher().now();
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher().now().duration_since(time), RTR_SOLICITATION_INTERVAL);
        let (src_mac, dst_mac, src_ip, dst_ip, message, code) =
            parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv6, _, RouterSolicitation, _>(
                &ctx.dispatcher().frames_sent()[2].1,
                |p| {
                    // We should have a source link layer option now because we have a
                    // source ip address set.
                    assert_eq!(p.body().iter().count(), 1);
                    if let Some(ll) = get_source_link_layer_option::<EthernetNdpDevice, _>(p.body())
                    {
                        assert_eq!(ll, dummy_config.local_mac);
                    } else {
                        panic!("Should have a source link layer option");
                    }
                },
            )
            .unwrap();
        assert_eq!(src_mac, dummy_config.local_mac);
        assert_eq!(src_ip, dummy_config.local_ip.get());
        assert_eq!(message, RouterSolicitation::default());
        assert_eq!(code, IcmpUnusedCode);

        // No more timers.
        assert!(!trigger_next_timer(&mut ctx));
        // Should have only sent 3 packets (Router solicitations).
        assert_eq!(ctx.dispatcher().frames_sent().len(), 3);

        //
        // Configure MAX_RTR_SOLICITATIONS in the stack.
        //

        let mut stack_builder = StackStateBuilder::default();
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_dup_addr_detect_transmits(None);
        ndp_configs.set_max_router_solicitations(NonZeroU8::new(2));
        stack_builder.device_builder().set_default_ndp_configs(ndp_configs);
        let mut ctx = Context::new(stack_builder.build(), DummyEventDispatcher::default());

        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        let device_id = ctx.state.add_ethernet_device(dummy_config.local_mac, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device_id);
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);

        let time = ctx.dispatcher().now();
        assert!(trigger_next_timer(&mut ctx));
        // Initial router solicitation should be a random delay between 0 and
        // `MAX_RTR_SOLICITATION_DELAY`.
        assert!(ctx.dispatcher().now().duration_since(time) < MAX_RTR_SOLICITATION_DELAY);
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);

        // Should trigger 1 more router solicitations
        let time = ctx.dispatcher().now();
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher().now().duration_since(time), RTR_SOLICITATION_INTERVAL);
        assert_eq!(ctx.dispatcher().frames_sent().len(), 2);

        // Each packet would be the same.
        for f in ctx.dispatcher().frames_sent() {
            let (src_mac, dst_mac, src_ip, dst_ip, message, code) =
                parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv6, _, RouterSolicitation, _>(
                    &f.1,
                    |_| {},
                )
                .unwrap();
            validate_params(src_mac, src_ip, message, code);
        }

        // No more timers.
        assert!(!trigger_next_timer(&mut ctx));
    }

    #[test]
    fn test_router_solicitation_on_routing_enabled_changes() {
        //
        // Make sure that when an interface goes from host -> router, it stops sending Router
        // Solicitations, and starts sending them when it goes form router -> host as routers
        // should not send Router Solicitation messages, but hosts should.
        //

        let dummy_config = get_dummy_config::<Ipv6Addr>();

        //
        // If netstack is not set to forward packets, make sure router solicitations do not get
        // cancelled when we enable forwading on the device.
        //

        let mut state_builder = StackStateBuilder::default();
        state_builder.ipv6_builder().forward(false);
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_dup_addr_detect_transmits(None);
        state_builder.device_builder().set_default_ndp_configs(ndp_configs);
        let mut ctx = DummyEventDispatcherBuilder::default()
            .build_with(state_builder, DummyEventDispatcher::default());

        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        let device = ctx.state.add_ethernet_device(dummy_config.local_mac, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);
        let timer_id =
            NdpTimerId::new_router_solicitation_timer_id::<EthernetNdpDevice>(device.id());

        // Send the first router solicitation.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        let timers: Vec<(&DummyInstant, &TimerId)> =
            ctx.dispatcher().timer_events().filter(|x| *x.1 == timer_id).collect();
        assert_eq!(timers.len(), 1);
        assert!(trigger_next_timer(&mut ctx));

        // Should have sent a router solicitation and still have the timer setup.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        let (src_mac, dst_mac, src_ip, dst_ip, message, code) =
            parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv6, _, RouterSolicitation, _>(
                &ctx.dispatcher().frames_sent()[0].1,
                |_| {},
            )
            .unwrap();
        let timers: Vec<(&DummyInstant, &TimerId)> =
            ctx.dispatcher().timer_events().filter(|x| *x.1 == timer_id).collect();
        assert_eq!(timers.len(), 1);
        // Capture the instant when the timer was supposed to fire so we can make sure that a new
        // timer doesn't replace the current one.
        let instant = timers[0].0.clone();

        // Enable routing on device.
        set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);
        assert!(is_routing_enabled::<_, Ipv6>(&ctx, device));

        // Should have not send any new packets and still have the original router solicitation
        // timer set.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        let timers: Vec<(&DummyInstant, &TimerId)> =
            ctx.dispatcher().timer_events().filter(|x| *x.1 == timer_id).collect();
        assert_eq!(timers.len(), 1);
        assert_eq!(*timers[0].0, instant);

        //
        // Now make the netstack and a device actually routing capable.
        //

        let mut state_builder = StackStateBuilder::default();
        state_builder.ipv6_builder().forward(true);
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_dup_addr_detect_transmits(None);
        state_builder.device_builder().set_default_ndp_configs(ndp_configs);
        let mut ctx = DummyEventDispatcherBuilder::default()
            .build_with(state_builder, DummyEventDispatcher::default());

        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        let device = ctx.state.add_ethernet_device(dummy_config.local_mac, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);
        let timer_id =
            NdpTimerId::new_router_solicitation_timer_id::<EthernetNdpDevice>(device.id());

        // Send the first router solicitation.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        let timers: Vec<(&DummyInstant, &TimerId)> =
            ctx.dispatcher().timer_events().filter(|x| *x.1 == timer_id).collect();
        assert_eq!(timers.len(), 1);
        assert!(trigger_next_timer(&mut ctx));

        // Should have sent a frame and have a router solicitation timer setup.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        let (src_mac, dst_mac, src_ip, dst_ip, message, code) =
            parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv6, _, RouterSolicitation, _>(
                &ctx.dispatcher().frames_sent()[0].1,
                |_| {},
            )
            .unwrap();
        assert_eq!(ctx.dispatcher().timer_events().filter(|x| *x.1 == timer_id).count(), 1);

        // Enable routing on the device.
        set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);
        assert!(is_routing_enabled::<_, Ipv6>(&ctx, device));

        // Should have not sent any new packets, but unset the router solicitation timer.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        assert_eq!(ctx.dispatcher().timer_events().filter(|x| *x.1 == timer_id).count(), 0);

        // Unsetting routing should succeed.
        set_routing_enabled::<_, Ipv6>(&mut ctx, device, false);
        assert!(!is_routing_enabled::<_, Ipv6>(&ctx, device));
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        let timers: Vec<(&DummyInstant, &TimerId)> =
            ctx.dispatcher().timer_events().filter(|x| *x.1 == timer_id).collect();
        assert_eq!(timers.len(), 1);

        // Send the first router solicitation after being turned into a host.
        assert!(trigger_next_timer(&mut ctx));

        // Should have sent a router solicitation.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 2);
        let (src_mac, dst_mac, src_ip, dst_ip, message, code) =
            parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv6, _, RouterSolicitation, _>(
                &ctx.dispatcher().frames_sent()[1].1,
                |_| {},
            )
            .unwrap();
        assert_eq!(ctx.dispatcher().timer_events().filter(|x| *x.1 == timer_id).count(), 1);
    }

    #[test]
    fn test_set_ndp_configs_dup_addr_detect_transmits() {
        //
        // Test that updating the DupAddrDetectTransmits parameter on an interface updates
        // the number of DAD messages (NDP Neighbor Solicitations) sent before concluding
        // that an address is not a duplicate.
        //

        let dummy_config = get_dummy_config::<Ipv6Addr>();
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let device = ctx.state_mut().add_ethernet_device(dummy_config.local_mac, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        // Updating the IP should resolve immediately since DAD is turned off by
        // `DummyEventDispatcherBuilder::build`.
        add_ip_addr_subnet(
            &mut ctx,
            device,
            AddrSubnet::new(dummy_config.local_ip.get(), 128).unwrap(),
        )
        .unwrap();
        assert_eq!(
            EthernetNdpDevice::ipv6_addr_state(ctx.state(), device.id(), &dummy_config.local_ip)
                .unwrap(),
            AddressState::Assigned
        );
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        // Enable DAD for the device.
        let mut ndp_configs = crate::device::get_ndp_configurations(&ctx, device).clone();
        ndp_configs.set_dup_addr_detect_transmits(NonZeroU8::new(3));
        crate::device::set_ndp_configurations(&mut ctx, device, ndp_configs.clone());

        // Updating the IP should start the DAD process.
        add_ip_addr_subnet(
            &mut ctx,
            device,
            AddrSubnet::new(dummy_config.remote_ip.get(), 128).unwrap(),
        )
        .unwrap();
        assert_eq!(
            EthernetNdpDevice::ipv6_addr_state(ctx.state(), device.id(), &dummy_config.local_ip)
                .unwrap(),
            AddressState::Assigned
        );
        assert_eq!(
            EthernetNdpDevice::ipv6_addr_state(ctx.state(), device.id(), &dummy_config.remote_ip)
                .unwrap(),
            AddressState::Tentative
        );
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        assert_eq!(ctx.dispatcher().timer_events().count(), 1);

        // Disable DAD during DAD.
        ndp_configs.set_dup_addr_detect_transmits(None);
        crate::device::set_ndp_configurations(&mut ctx, device, ndp_configs.clone());

        // Allow aleady started DAD to complete (2 more more NS, 3 more timers).
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher().frames_sent().len(), 2);
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher().frames_sent().len(), 3);
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher().frames_sent().len(), 3);
        assert_eq!(
            EthernetNdpDevice::ipv6_addr_state(ctx.state(), device.id(), &dummy_config.remote_ip)
                .unwrap(),
            AddressState::Assigned
        );

        // Updating the IP should resolve immediately since DAD has just been turned off.
        let new_ip = get_other_ip_address::<Ipv6Addr>(3);
        add_ip_addr_subnet(&mut ctx, device, AddrSubnet::new(new_ip.get(), 128).unwrap()).unwrap();
        assert_eq!(
            EthernetNdpDevice::ipv6_addr_state(ctx.state(), device.id(), &dummy_config.local_ip)
                .unwrap(),
            AddressState::Assigned
        );
        assert_eq!(
            EthernetNdpDevice::ipv6_addr_state(ctx.state(), device.id(), &dummy_config.remote_ip)
                .unwrap(),
            AddressState::Assigned
        );
        assert_eq!(
            EthernetNdpDevice::ipv6_addr_state(ctx.state(), device.id(), &new_ip).unwrap(),
            AddressState::Assigned
        );
    }

    #[test]
    #[should_panic]
    fn test_send_router_advertisement_as_non_router_panic() {
        //
        // Attempting to send a router advertisement when a device is not a router should result in
        // a panic.
        //
        let dummy_config = get_dummy_config::<Ipv6Addr>();
        let mut stack_builder = StackStateBuilder::default();
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_dup_addr_detect_transmits(None);
        ndp_configs.set_max_router_solicitations(None);
        stack_builder.device_builder().set_default_ndp_configs(ndp_configs);
        stack_builder.ipv6_builder().forward(true);
        let mut ctx = Context::new(stack_builder.build(), DummyEventDispatcher::default());
        let device = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        // Enable sending router advertisements (`device`) is still not a router though.
        EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device.id())
            .configs
            .router_configurations
            .set_should_send_advertisements(true);

        // Should panic since `device` is not a router.
        send_router_advertisement::<_, EthernetNdpDevice>(
            &mut ctx,
            device.id(),
            Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS,
        );
    }

    #[test]
    #[should_panic]
    fn test_send_router_advertisement_without_linklocal() {
        //
        // Attempting to send a router advertisement when a device does not yet have a link-local
        // address should panic.
        //
        let dummy_config = get_dummy_config::<Ipv6Addr>();
        let mut stack_builder = StackStateBuilder::default();
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_max_router_solicitations(None);
        stack_builder.device_builder().set_default_ndp_configs(ndp_configs);
        stack_builder.ipv6_builder().forward(true);
        let mut ctx = Context::new(stack_builder.build(), DummyEventDispatcher::default());
        let device = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        // Make `device` a router (netstack is configured to forward packets already).
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);

        // Enable sending router advertisements (`device`) is still not a router though.
        EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device.id())
            .configs
            .router_configurations
            .set_should_send_advertisements(true);

        // Should panic since `device` is not a router.
        send_router_advertisement::<_, EthernetNdpDevice>(
            &mut ctx,
            device.id(),
            Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS,
        );
    }

    #[test]
    #[should_panic]
    fn test_send_router_advertisement_with_should_send_advertisement_unset_panic() {
        //
        // Attempting to send a router advertisements when it is configured to not do so should
        // result in a panic.
        //

        let dummy_config = get_dummy_config::<Ipv6Addr>();
        let mut stack_builder = StackStateBuilder::default();
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_dup_addr_detect_transmits(None);
        ndp_configs.set_max_router_solicitations(None);
        stack_builder.device_builder().set_default_ndp_configs(ndp_configs);
        stack_builder.ipv6_builder().forward(true);
        let mut ctx = Context::new(stack_builder.build(), DummyEventDispatcher::default());
        let device = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        // Make `device` a router (netstack is configured to forward packets already).
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);

        // Should panic since sending router advertisements is disabled.
        send_router_advertisement::<_, EthernetNdpDevice>(
            &mut ctx,
            device.id(),
            Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS,
        );
    }

    #[test]
    fn test_send_router_advertisement_after_dad() {
        //
        // Should send router advertisements after DAD (3 transmits) is completed for the
        // link-local address.
        //

        let dummy_config = get_dummy_config::<Ipv6Addr>();
        let mut stack_builder = StackStateBuilder::default();
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_max_router_solicitations(None);
        ndp_configs.set_dup_addr_detect_transmits(NonZeroU8::new(3));
        stack_builder.device_builder().set_default_ndp_configs(ndp_configs.clone());
        stack_builder.ipv6_builder().forward(true);
        let mut ctx = Context::new(stack_builder.build(), DummyEventDispatcher::default());
        let device = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        // Make `device` a router (netstack is configured to forward packets already).
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);

        // Enable sending router advertisements (`device`) is still not a router though.
        EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device.id())
            .configs
            .router_configurations
            .set_should_send_advertisements(true);

        // Finish up DAD.
        assert_eq!(
            EthernetNdpDevice::ipv6_addr_state(
                ctx.state(),
                device.id(),
                &TEST_LOCAL_MAC.to_ipv6_link_local().get()
            )
            .unwrap(),
            AddressState::Tentative
        );
        assert_eq!(run_for(&mut ctx, Duration::from_secs(3)), 3);
        assert_eq!(
            EthernetNdpDevice::ipv6_addr_state(
                ctx.state(),
                device.id(),
                &TEST_LOCAL_MAC.to_ipv6_link_local().get()
            )
            .unwrap(),
            AddressState::Assigned
        );

        // Send initial RAs
        //
        // 3 packets have already been sent because of DAD.
        validate_initial_ras_after_enable(&mut ctx, device, &ndp_configs, 3);
    }

    #[test]
    fn test_sending_router_advertisement() {
        //
        // Test that valid router advertisements are sent based on the device's
        // NDP router configurations (`NdpRouterConfigurations`).
        //

        let dummy_config = get_dummy_config::<Ipv6Addr>();
        let mut stack_builder = StackStateBuilder::default();
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_dup_addr_detect_transmits(None);
        ndp_configs.set_max_router_solicitations(None);
        stack_builder.device_builder().set_default_ndp_configs(ndp_configs);
        stack_builder.ipv6_builder().forward(true);
        let mut ctx = Context::new(stack_builder.build(), DummyEventDispatcher::default());
        let device = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        // Make `device` a router (netstack is configured to forward packets already).
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);

        // Enable and send router advertisements.
        EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device.id())
            .configs
            .router_configurations
            .set_should_send_advertisements(true);
        send_router_advertisement::<_, EthernetNdpDevice>(
            &mut ctx,
            device.id(),
            Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS,
        );
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        let (src_mac, dst_mac, src_ip, dst_ip, message, code) =
            parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv6, _, RouterAdvertisement, _>(
                &ctx.dispatcher().frames_sent()[0].1,
                |p| {
                    assert_eq!(p.body().iter().count(), 1);
                    assert!(p
                        .body()
                        .iter()
                        .any(|x| x == NdpOption::SourceLinkLayerAddress(&TEST_LOCAL_MAC.bytes())));
                },
            )
            .unwrap();
        assert_eq!(src_ip, TEST_LOCAL_MAC.to_ipv6_link_local().get());
        assert_eq!(dst_ip, Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS);
        assert_eq!(code, IcmpUnusedCode);
        assert_eq!(message, RouterAdvertisement::new(64, false, false, 1800, 0, 0));

        // Set new values for a new router advertisement.
        let mut router_configs =
            &mut EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device.id())
                .configs
                .router_configurations;
        router_configs.set_advertised_managed_flag(true);
        router_configs.set_advertised_link_mtu(NonZeroU32::new(1500));
        router_configs.set_advertised_reachable_time(50);
        router_configs.set_advertised_retransmit_timer(200);
        router_configs.set_advertised_current_hop_limit(75);
        router_configs.set_advertised_default_lifetime(NonZeroU16::new(2000));
        let prefix1 = PrefixInformation::new(
            64,
            true,
            false,
            500,
            400,
            Ipv6Addr::new([51, 52, 53, 54, 55, 56, 57, 58, 0, 0, 0, 0, 0, 0, 0, 0]),
        );
        let prefix2 = PrefixInformation::new(
            70,
            false,
            true,
            501,
            401,
            Ipv6Addr::new([51, 52, 53, 54, 55, 56, 57, 59, 0, 0, 0, 0, 0, 0, 0, 0]),
        );
        router_configs.set_advertised_prefix_list(vec![prefix1.clone(), prefix2.clone()]);
        send_router_advertisement::<_, EthernetNdpDevice>(
            &mut ctx,
            device.id(),
            Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS,
        );
        assert_eq!(ctx.dispatcher().frames_sent().len(), 2);
        let (src_mac, dst_mac, src_ip, dst_ip, message, code) =
            parse_icmp_packet_in_ip_packet_in_ethernet_frame::<Ipv6, _, RouterAdvertisement, _>(
                &ctx.dispatcher().frames_sent()[1].1,
                |p| {
                    assert_eq!(p.body().iter().count(), 4);

                    let mtu = 1500;
                    assert!(p.body().iter().any(|x| x == NdpOption::MTU(mtu)));
                    assert!(p.body().iter().any(|x| x == NdpOption::PrefixInformation(&prefix1)));
                    assert!(p.body().iter().any(|x| x == NdpOption::PrefixInformation(&prefix2)));
                    assert!(p
                        .body()
                        .iter()
                        .any(|x| x == NdpOption::SourceLinkLayerAddress(&TEST_LOCAL_MAC.bytes())));
                },
            )
            .unwrap();
        assert_eq!(src_ip, TEST_LOCAL_MAC.to_ipv6_link_local().get());
        assert_eq!(dst_ip, Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS);
        assert_eq!(code, IcmpUnusedCode);
        assert_eq!(message, RouterAdvertisement::new(75, true, false, 2000, 50, 200));
    }

    #[test]
    fn test_sending_unsolicited_router_advertisements() {
        let mut ndp_configs = NdpConfigurations::default();
        let mut ndp_rc_configs = NdpRouterConfigurations::default();
        ndp_rc_configs.set_should_send_advertisements(true);
        ndp_configs.set_router_configurations(ndp_rc_configs);
        ndp_configs.set_max_router_solicitations(None);
        ndp_configs.set_dup_addr_detect_transmits(None);

        //
        // When netstack is not configured to forward IP packets, should not send router
        // advertisements.
        //

        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let device = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        // Setting a non-router device to be an advertising interface should do nothing.
        crate::device::set_ndp_configurations(&mut ctx, device, ndp_configs.clone());
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        fn confirm_dad_frame_timer(ctx: &mut Context<DummyEventDispatcher>, device: DeviceId) {
            assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
            assert_eq!(ctx.dispatcher().timer_events().count(), 1);
            assert_eq!(
                *ctx.dispatcher().timer_events().last().unwrap().1,
                NdpTimerId::new_dad_ns_transmission_timer_id::<EthernetNdpDevice>(
                    device.id(),
                    TEST_LOCAL_MAC.to_ipv6_link_local().get()
                )
            );
        }

        //
        // Test sending router advertisements (after dad).
        //

        let mut state_builder = StackStateBuilder::default();
        state_builder.ipv6_builder().forward(true);
        let mut default_ndp_configs = NdpConfigurations::default();
        default_ndp_configs.set_max_router_solicitations(None);
        state_builder.device_builder().set_default_ndp_configs(default_ndp_configs.clone());
        let mut ctx = DummyEventDispatcherBuilder::default()
            .build_with(state_builder, DummyEventDispatcher::default());
        let device = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        assert_eq!(ctx.dispatcher().timer_events().count(), 1);

        //
        // Update configurations that affect advertising interface status
        // before DAD has completed (RAs should not be sent or started).
        //

        // Setting a non-router device to be an advertising interface should do nothing.
        crate::device::set_ndp_configurations(&mut ctx, device, ndp_configs.clone());
        confirm_dad_frame_timer(&mut ctx, device);

        // Setting a non-advertising interface to a router should do nothing.
        crate::device::set_ndp_configurations(&mut ctx, device, NdpConfigurations::default());
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);
        confirm_dad_frame_timer(&mut ctx, device);

        // Set to be an advertising interface.
        crate::device::set_ndp_configurations(&mut ctx, device, ndp_configs.clone());
        confirm_dad_frame_timer(&mut ctx, device);

        // Setting to be a non-router should end router advertisements (but should start router solicitations)
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, false);
        confirm_dad_frame_timer(&mut ctx, device);

        // Set back to a router.
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);
        confirm_dad_frame_timer(&mut ctx, device);

        // Setting to be a non-advertising interface should end router advertisements.
        crate::device::set_ndp_configurations(&mut ctx, device, NdpConfigurations::default());
        confirm_dad_frame_timer(&mut ctx, device);

        // Set back to being an advertising interface.
        crate::device::set_ndp_configurations(&mut ctx, device, ndp_configs.clone());
        confirm_dad_frame_timer(&mut ctx, device);

        // Reset device routing and advertising interface status.
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, false);
        crate::device::set_ndp_configurations(&mut ctx, device, default_ndp_configs);
        confirm_dad_frame_timer(&mut ctx, device);

        // Complete DAD.
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        //
        // Update configurations that affect advertising interface status
        // after DAD has completed (RAs may now be sent).
        //

        // Setting a non-router device to be an advertising interface should do nothing.
        crate::device::set_ndp_configurations(&mut ctx, device, ndp_configs.clone());
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        // Setting a non-advertising interface to a router should do nothing.
        crate::device::set_ndp_configurations(&mut ctx, device, NdpConfigurations::default());
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        // Set to be an advertising interface.
        crate::device::set_ndp_configurations(&mut ctx, device, ndp_configs.clone());
        validate_initial_ras_after_enable(&mut ctx, device, &ndp_configs, 1);

        // Setting to be a non-router should end router advertisements.
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, false);
        validate_final_ras(&mut ctx, device, 4);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        // Set back to a router.
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);
        validate_initial_ras_after_enable(&mut ctx, device, &ndp_configs, 7);

        // Setting to be a non-advertising interface should end router advertisements.
        crate::device::set_ndp_configurations(&mut ctx, device, NdpConfigurations::default());
        validate_final_ras(&mut ctx, device, 10);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        // Set back to being an advertising interface.
        crate::device::set_ndp_configurations(&mut ctx, device, ndp_configs.clone());
        validate_initial_ras_after_enable(&mut ctx, device, &ndp_configs, 13);
    }

    #[test]
    fn test_updating_ndp_router_config_router_advertisement_interval() {
        let mut ndp_configs = NdpConfigurations::default();
        let mut ndp_rc_configs = NdpRouterConfigurations::default();
        ndp_rc_configs.set_should_send_advertisements(true);
        ndp_rc_configs.set_router_advertisements_interval(200..=300);
        ndp_configs.set_router_configurations(ndp_rc_configs.clone());
        ndp_configs.set_dup_addr_detect_transmits(None);
        ndp_configs.set_max_router_solicitations(None);

        let mut state_builder = StackStateBuilder::default();
        state_builder.ipv6_builder().forward(true);
        state_builder.device_builder().set_default_ndp_configs(ndp_configs.clone());
        let mut ctx = DummyEventDispatcherBuilder::default()
            .build_with(state_builder, DummyEventDispatcher::default());
        let device = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);

        // Make device a router so it will start sending router advertisements
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);
        validate_initial_ras_after_enable(&mut ctx, device, &ndp_configs, 0);
        let now = ctx.dispatcher().now();
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (*x.0 >= (now + Duration::from_secs(200)))
                    && (*x.0 <= (now + Duration::from_secs(300)))
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );

        // Update the routing interval to some value that will make the timer get rescheduled
        // (new max delay is less than the time to the next Router Advertisement transmission).
        ndp_rc_configs.set_router_advertisements_interval(50..=100);
        ndp_configs.set_router_configurations(ndp_rc_configs.clone());
        crate::device::set_ndp_configurations(&mut ctx, device, ndp_configs.clone());
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (*x.0 >= (now + Duration::from_secs(50)))
                    && (*x.0 <= (now + Duration::from_secs(100)))
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );

        // Update the routing interval to some value that will not make the timer get rescheduled
        // (new max delay is greater than the time to the next Router Advertisement transmission).
        ndp_rc_configs.set_router_advertisements_interval(25..=300);
        ndp_configs.set_router_configurations(ndp_rc_configs.clone());
        crate::device::set_ndp_configurations(&mut ctx, device, ndp_configs.clone());
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (*x.0 >= (now + Duration::from_secs(50)))
                    && (*x.0 <= (now + Duration::from_secs(100)))
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );

        // Update the routing interval to some value that will not make the timer get rescheduled
        // (new min delay is greater than the time to the next Router Advertisement transmission).
        ndp_rc_configs.set_router_advertisements_interval(500..=1000);
        ndp_configs.set_router_configurations(ndp_rc_configs.clone());
        crate::device::set_ndp_configurations(&mut ctx, device, ndp_configs.clone());
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (*x.0 >= (now + Duration::from_secs(50)))
                    && (*x.0 <= (now + Duration::from_secs(150)))
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );

        // Resetting the advertising interface status should use the new updated interval.
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, false);
        validate_final_ras(&mut ctx, device, 3);
        assert_eq!(ctx.dispatcher().timer_events().count(), 0);
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);
        validate_initial_ras_after_enable(&mut ctx, device, &ndp_configs, 6);
        let now = ctx.dispatcher().now();
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (*x.0 >= (now + Duration::from_secs(500)))
                    && (*x.0 <= (now + Duration::from_secs(1000)))
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );
    }

    #[test]
    fn test_router_response_to_router_solicitation_from_unspecified_source() {
        /// Get a Router Solicitation ICMPv6 packet buffer.
        fn rs_msg(src_ip: Ipv6Addr, dst_ip: Ipv6Addr) -> Buf<Vec<u8>> {
            Buf::new(Vec::new(), ..)
                .encapsulate(IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                    src_ip,
                    dst_ip,
                    IcmpUnusedCode,
                    RouterSolicitation::default(),
                ))
                .serialize_vec_outer()
                .unwrap()
                .into_inner()
        }

        let mut ndp_configs = NdpConfigurations::default();
        let mut ndp_rc_configs = NdpRouterConfigurations::default();
        ndp_rc_configs.set_should_send_advertisements(true);
        ndp_rc_configs.set_router_advertisements_interval(200..=300);
        ndp_configs.set_router_configurations(ndp_rc_configs.clone());
        ndp_configs.set_dup_addr_detect_transmits(None);
        ndp_configs.set_max_router_solicitations(None);

        let dummy_config = get_dummy_config::<Ipv6Addr>();
        let mut state_builder = StackStateBuilder::default();
        state_builder.ipv6_builder().forward(true);
        state_builder.device_builder().set_default_ndp_configs(ndp_configs.clone());
        let mut ctx = DummyEventDispatcherBuilder::default()
            .build_with(state_builder, DummyEventDispatcher::default());
        let device = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        // Assign an address to the device (should be assigned immediately since DAD is disabled).
        add_ip_addr_subnet(
            &mut ctx,
            device,
            AddrSubnet::new(dummy_config.local_ip.get(), 128).unwrap(),
        )
        .unwrap();

        // Make device a router so it will start sending router advertisements
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);

        // Should not have sent anything yet.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);

        let now = ctx.dispatcher().now();
        // First `MAX_INITIAL_RTR_ADVERTISEMENTS` will have an interval of
        // `MAX_INITIAL_RTR_ADVERT_INTERVAL` because (3 left).
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (*x.0 == now.checked_add(MAX_INITIAL_RTR_ADVERT_INTERVAL).unwrap())
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );

        //
        // Receiving a Router Solicitation when the next scheduled Router Advertisement transmission
        // is too far away will update the Router Advertisement timer to be at max
        // `MAX_RA_DELAY_TIME`.
        //

        let src_ip = Ipv6Addr::default();
        let mut rs_buf = rs_msg(src_ip, dummy_config.local_ip.get());
        let rs = rs_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip, dummy_config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(Some(device), src_ip, dummy_config.local_ip, rs);
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (*x.0 <= now.checked_add(MAX_RA_DELAY_TIME).unwrap())
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );

        // Send the Router Advertisement.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        validate_simple_ra(&ctx.dispatcher().frames_sent()[0].1, 1800);

        //
        // Receiving a router solicitation close to the next scheduled router advertisement
        // transmission should not reschedule the router advertisement.
        //

        let now = ctx.dispatcher().now();
        // First `MAX_INITIAL_RTR_ADVERTISEMENTS` will have an interval of
        // `MAX_INITIAL_RTR_ADVERT_INTERVAL` because (2 left).
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (*x.0 == now.checked_add(MAX_INITIAL_RTR_ADVERT_INTERVAL).unwrap())
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );

        // Skip time to right before the next Router Advertisement transmission.
        run_for(&mut ctx, MAX_INITIAL_RTR_ADVERT_INTERVAL - MIN_RA_DELAY_TIME);
        let now = ctx.dispatcher().now();
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (x.0.duration_since(now) == MIN_RA_DELAY_TIME)
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );

        // Receiving a Router Solicitation.
        let mut rs_buf = rs_msg(src_ip, dummy_config.local_ip.get());
        let rs = rs_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip, dummy_config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(Some(device), src_ip, dummy_config.local_ip, rs);
        // Timer should not have been updated.
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (x.0.duration_since(now) == MIN_RA_DELAY_TIME)
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );

        // Send the Router Advertisement.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher().frames_sent().len(), 2);
        validate_simple_ra(&ctx.dispatcher().frames_sent()[1].1, 1800);

        //
        // Receiving a router solicitation within `MIN_DELAY_BETWEEN_RAS` of sending a Router
        // Advertisement to the all-nodes multicast address will update the transmission time
        // of the next Router Advertisement message to be at max `MIN_DELAY_BETWEEN_RAS` +
        // `MAX_RA_DELAY_TIME` from the time the last Router Advertisement was sent.
        //

        // Time the last router advertisement was sent.
        let last_instant = ctx.dispatcher.now();

        // Skip time by 1s (less than `MIN_DELAY_BETWEEN_RAS`).
        run_for(&mut ctx, MIN_DELAY_BETWEEN_RAS - Duration::from_secs(1));
        let now = ctx.dispatcher().now();
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (*x.0
                    == last_instant.checked_add(MAX_INITIAL_RTR_ADVERT_INTERVAL).unwrap())
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );

        // Receiving a Router Solicitation.
        let mut rs_buf = rs_msg(src_ip, dummy_config.local_ip.get());
        let rs = rs_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip, dummy_config.local_ip))
            .unwrap();
        ctx.receive_ndp_packet(Some(device), src_ip, dummy_config.local_ip, rs);
        // Timer should be updated to be at max `MIN_DELAY_BETWEEN_RAS` + `MAX_RA_DELAY_TIME` from
        // the time the last Router Advertisement was sent.
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (x.0.duration_since(last_instant)
                    <= (MIN_DELAY_BETWEEN_RAS + MAX_RA_DELAY_TIME))
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );

        // Send the Router Advertisement.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 2);
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher().frames_sent().len(), 3);
        validate_simple_ra(&ctx.dispatcher().frames_sent()[2].1, 1800);
    }

    #[test]
    fn test_router_response_to_router_solicitation_from_specified_source() {
        /// Get a Router Solicitation ICMPv6 packet buffer.
        fn rs_msg(src_ip: Ipv6Addr, src_mac: Mac, dst_ip: Ipv6Addr) -> Buf<Vec<u8>> {
            let mac_bytes = src_mac.bytes();
            let options = &[NdpOption::SourceLinkLayerAddress(&mac_bytes)];
            OptionsSerializer::<_>::new(options.iter())
                .into_serializer()
                .encapsulate(IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                    src_ip,
                    dst_ip,
                    IcmpUnusedCode,
                    RouterSolicitation::default(),
                ))
                .serialize_vec_outer()
                .unwrap()
                .unwrap_b()
        }

        let mut ndp_configs = NdpConfigurations::default();
        let mut ndp_rc_configs = NdpRouterConfigurations::default();
        ndp_rc_configs.set_should_send_advertisements(true);
        ndp_rc_configs.set_router_advertisements_interval(200..=300);
        ndp_configs.set_router_configurations(ndp_rc_configs.clone());
        ndp_configs.set_dup_addr_detect_transmits(None);
        ndp_configs.set_max_router_solicitations(None);

        let dummy_config = get_dummy_config::<Ipv6Addr>();
        let mut state_builder = StackStateBuilder::default();
        state_builder.ipv6_builder().forward(true);
        state_builder.device_builder().set_default_ndp_configs(ndp_configs.clone());
        let mut ctx = DummyEventDispatcherBuilder::default()
            .build_with(state_builder, DummyEventDispatcher::default());
        let device = ctx.state_mut().add_ethernet_device(TEST_LOCAL_MAC, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        // Assign an address to the device (should be assigned immediately since DAD is disabled).
        add_ip_addr_subnet(
            &mut ctx,
            device,
            AddrSubnet::new(dummy_config.local_ip.get(), 128).unwrap(),
        )
        .unwrap();

        // Make device a router so it will start sending router advertisements
        crate::device::set_routing_enabled::<_, Ipv6>(&mut ctx, device, true);

        // Should not have sent anything yet.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);

        let now = ctx.dispatcher().now();
        // First `MAX_INITIAL_RTR_ADVERTISEMENTS` will have an interval of
        // `MAX_INITIAL_RTR_ADVERT_INTERVAL` because (3 left).
        assert_eq!(
            ctx.dispatcher()
                .timer_events()
                .filter(|x| (*x.0 == now.checked_add(MAX_INITIAL_RTR_ADVERT_INTERVAL).unwrap())
                    && (*x.1
                        == NdpTimerId::new_router_advertisement_transmit_timer_id::<
                            EthernetNdpDevice,
                        >(device.id())))
                .count(),
            1
        );

        //
        // Receiving a Router Solicitation from a specified source with a source link address
        // should update our neighbor cache.
        //

        let mut rs_buf = rs_msg(
            dummy_config.remote_ip.get(),
            dummy_config.remote_mac,
            dummy_config.local_ip.get(),
        );
        let rs = rs_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(
                dummy_config.remote_ip,
                dummy_config.local_ip,
            ))
            .unwrap();
        ctx.receive_ndp_packet(
            Some(device),
            dummy_config.remote_ip.get(),
            dummy_config.local_ip,
            rs,
        );

        // Send the Router Advertisement.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 0);
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        validate_simple_ra(&ctx.dispatcher().frames_sent()[0].1, 1800);

        // source should be in our neighbor cache.
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device.id());
        let neighbor = ndp_state.neighbors.get_neighbor_state_mut(&dummy_config.remote_ip).unwrap();
        assert_eq!(neighbor.link_address.unwrap(), dummy_config.remote_mac);
        assert_eq!(neighbor.state, NeighborEntryState::Stale);
        assert_eq!(neighbor.is_router, false);

        //
        // Receiving a Router Solicitation from a specified source with a source link address
        // should update our neighbor cache, even if the neighbor already exists.
        //

        // Update neighbor entries to other values to check an update.
        neighbor.link_address = Some(Mac::new([99, 98, 97, 96, 95, 94]));
        neighbor.state = NeighborEntryState::Reachable;
        neighbor.is_router = true;

        let mut rs_buf = rs_msg(
            dummy_config.remote_ip.get(),
            dummy_config.remote_mac,
            dummy_config.local_ip.get(),
        );
        let rs = rs_buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(
                dummy_config.remote_ip,
                dummy_config.local_ip,
            ))
            .unwrap();
        ctx.receive_ndp_packet(
            Some(device),
            dummy_config.remote_ip.get(),
            dummy_config.local_ip,
            rs,
        );

        // Send the Router Advertisement.
        assert_eq!(ctx.dispatcher().frames_sent().len(), 1);
        assert!(trigger_next_timer(&mut ctx));
        assert_eq!(ctx.dispatcher().frames_sent().len(), 2);
        validate_simple_ra(&ctx.dispatcher().frames_sent()[1].1, 1800);

        // Source's neighbor entry should be updated.
        let ndp_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device.id());
        let neighbor = ndp_state.neighbors.get_neighbor_state(&dummy_config.remote_ip).unwrap();
        assert_eq!(neighbor.link_address.unwrap(), dummy_config.remote_mac);
        assert_eq!(neighbor.state, NeighborEntryState::Stale);
        assert_eq!(neighbor.is_router, false);
    }

    #[test]
    fn test_router_discovery() {
        let dummy_config = get_dummy_config::<Ipv6Addr>();
        let mut state_builder = StackStateBuilder::default();
        let mut ndp_configs = NdpConfigurations::default();
        ndp_configs.set_dup_addr_detect_transmits(None);
        state_builder.device_builder().set_default_ndp_configs(ndp_configs.clone());
        let mut host = DummyEventDispatcherBuilder::default()
            .build_with(state_builder, DummyEventDispatcher::default());
        let mut state_builder = StackStateBuilder::default();
        state_builder.ipv6_builder().forward(true);
        state_builder.device_builder().set_default_ndp_configs(ndp_configs);
        let mut router = DummyEventDispatcherBuilder::default()
            .build_with(state_builder, DummyEventDispatcher::default());
        let device = DeviceId::new_ethernet(0);
        let mut net =
            DummyNetwork::new(vec![("host", host), ("router", router)].into_iter(), |ctx, dev| {
                if *ctx == "host" {
                    vec![("router", device, None)]
                } else {
                    vec![("host", device, None)]
                }
            });

        // Create an interface that is configured to be an advertising interface.
        assert_eq!(
            device,
            net.context("router")
                .state_mut()
                .add_ethernet_device(dummy_config.remote_mac, IPV6_MIN_MTU)
        );
        crate::device::initialize_device(net.context("router"), device);
        set_routing_enabled::<_, Ipv6>(net.context("router"), device, true);
        let mut ndp_configs = NdpConfigurations::default();
        let mut ndp_rc_configs = NdpRouterConfigurations::default();
        ndp_rc_configs.set_should_send_advertisements(true);
        ndp_configs.set_router_configurations(ndp_rc_configs);
        ndp_configs.set_max_router_solicitations(None);
        ndp_configs.set_dup_addr_detect_transmits(None);
        crate::device::set_ndp_configurations(net.context("router"), device, ndp_configs);

        // Create an interface to be the host.
        assert_eq!(
            device,
            net.context("host")
                .state_mut()
                .add_ethernet_device(dummy_config.local_mac, IPV6_MIN_MTU)
        );
        crate::device::initialize_device(net.context("host"), device);

        // Host should not know about the router yet.
        let router_ll = dummy_config.remote_mac.to_ipv6_link_local();
        assert!(!EthernetNdpDevice::get_ndp_state(net.context("host").state(), device.id())
            .has_default_router(&router_ll));

        // Run the network for `MAX_RA_DELAY_TIME`.
        net.run_for(MAX_RA_DELAY_TIME);

        // Host should now know about the router
        assert!(EthernetNdpDevice::get_ndp_state(net.context("host").state(), device.id())
            .has_default_router(&router_ll));

        // Making the router a non-advertising interface should make host remove it from its default
        // router list.
        set_routing_enabled::<_, Ipv6>(net.context("router"), device, false);

        // Only need to run for `MAX_INITIAL_RTR_ADVERT_INTERVAL` time since router is still
        // currently sending the first `MAX_INITIAL_RTR_ADVERTISEMENTS` RAs so the interval between
        // RAs will be set to at max `MAX_INITIAL_RTR_ADVERT_INTERVAL`. We know the interval will
        // definitely be set to `MAX_INITIAL_RTR_ADVERT_INTERVAL` because the normally generated
        // value would be at minimum 200s (see `ROUTER_ADVERTISEMENTS_INTERVAL_DEFAULT`).
        net.run_for(MAX_INITIAL_RTR_ADVERT_INTERVAL);

        // Host should not know about the router anymore.
        let router_ll = dummy_config.remote_mac.to_ipv6_link_local();
        assert!(!EthernetNdpDevice::get_ndp_state(net.context("host").state(), device.id())
            .has_default_router(&router_ll));
    }

    #[test]
    fn test_receiving_neighbor_advertisements() {
        fn test_receiving_na_from_known_neighbor(
            ctx: &mut Context<DummyEventDispatcher>,
            src_ip: Ipv6Addr,
            dst_ip: SpecifiedAddr<Ipv6Addr>,
            device: DeviceId,
            router_flag: bool,
            solicited_flag: bool,
            override_flag: bool,
            mac: Option<Mac>,
            expected_state: NeighborEntryState,
            expected_router: bool,
            expected_link_addr: Option<Mac>,
        ) {
            let mut buf = neighbor_advertisement_message(
                src_ip,
                dst_ip.get(),
                router_flag,
                solicited_flag,
                override_flag,
                mac,
            );
            let packet =
                buf.parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip, dst_ip)).unwrap();
            ctx.receive_ndp_packet(Some(device), src_ip, dst_ip, packet);

            let neighbor_state = EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device.id())
                .neighbors
                .get_neighbor_state(&src_ip)
                .unwrap();
            assert_eq!(neighbor_state.state, expected_state);
            assert_eq!(neighbor_state.is_router, expected_router);
            assert_eq!(neighbor_state.link_address, expected_link_addr);
        }

        let config = get_dummy_config::<Ipv6Addr>();
        let mut ctx = DummyEventDispatcherBuilder::default().build::<DummyEventDispatcher>();
        let device = ctx.state_mut().add_ethernet_device(config.local_mac, IPV6_MIN_MTU);
        crate::device::initialize_device(&mut ctx, device);

        let neighbor_mac = config.remote_mac;
        let neighbor_ip = neighbor_mac.to_ipv6_link_local().get();
        let all_nodes_addr = SpecifiedAddr::new(Ipv6::ALL_NODES_LINK_LOCAL_ADDRESS).unwrap();

        // Should not know about the neighbor yet.
        assert!(EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device.id())
            .neighbors
            .get_neighbor_state(&neighbor_ip)
            .is_none());

        //
        // Receiving unsolicited NA from a neighbor we don't care about yet should do nothing.
        //

        // Receive the NA.
        let mut buf = neighbor_advertisement_message(
            neighbor_ip,
            all_nodes_addr.get(),
            false,
            false,
            false,
            None,
        );
        let packet = buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(neighbor_ip, all_nodes_addr))
            .unwrap();
        ctx.receive_ndp_packet(Some(device), neighbor_ip, all_nodes_addr, packet);

        // We still do not know about the neighbor since the NA was unsolicited and we never were
        // interested in the neighbor yet.
        assert!(EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device.id())
            .neighbors
            .get_neighbor_state(&neighbor_ip)
            .is_none());

        //
        // Receiving solicited NA from a neighbor we don't care about yet should do nothing (should
        // never happen).
        //

        // Receive the NA.
        let mut buf = neighbor_advertisement_message(
            neighbor_ip,
            all_nodes_addr.get(),
            false,
            true,
            false,
            None,
        );
        let packet = buf
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(neighbor_ip, all_nodes_addr))
            .unwrap();
        ctx.receive_ndp_packet(Some(device), neighbor_ip, all_nodes_addr, packet);

        // We still do not know about the neighbor since the NA was unsolicited and we never were
        // interested in the neighbor yet.
        assert!(EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device.id())
            .neighbors
            .get_neighbor_state(&neighbor_ip)
            .is_none());

        //
        // Receiving solicited NA from a neighbor we are trying to resolve, but no target link addr.
        //
        // Should do nothing (still INCOMPLETE).
        //

        // Create incomplete neighbor entry.
        let neighbors =
            &mut EthernetNdpDevice::get_ndp_state_mut(ctx.state_mut(), device.id()).neighbors;
        neighbors.add_incomplete_neighbor_state(neighbor_ip);

        test_receiving_na_from_known_neighbor(
            &mut ctx,
            neighbor_ip,
            config.local_ip,
            device,
            false,
            true,
            false,
            None,
            NeighborEntryState::Incomplete { transmit_counter: 1 },
            false,
            None,
        );

        //
        // Receiving solicited NA from a neighbor we are resolving, but with target link addr.
        //
        // Should update link layer address and set state to REACHABLE.
        //

        test_receiving_na_from_known_neighbor(
            &mut ctx,
            neighbor_ip,
            config.local_ip,
            device,
            false,
            true,
            false,
            Some(neighbor_mac),
            NeighborEntryState::Reachable,
            false,
            Some(neighbor_mac),
        );

        //
        // Receive unsolicited NA from a neighbor with router flag updated (no target link addr).
        //
        // Should update is_router to true.
        //

        test_receiving_na_from_known_neighbor(
            &mut ctx,
            neighbor_ip,
            config.local_ip,
            device,
            true,
            false,
            false,
            None,
            NeighborEntryState::Reachable,
            true,
            Some(neighbor_mac),
        );

        //
        // Receive unsolicited NA from a neighbor without router flag set and same target link addr.
        //
        // Should update is_router, state should be unchanged.
        //

        test_receiving_na_from_known_neighbor(
            &mut ctx,
            neighbor_ip,
            config.local_ip,
            device,
            false,
            false,
            false,
            Some(neighbor_mac),
            NeighborEntryState::Reachable,
            false,
            Some(neighbor_mac),
        );

        //
        // Receive unsolicted NA from a neighbor with new target link addr.
        //
        // Should NOT update link layer addr, but set state to STALE.
        //

        let new_mac = Mac::new([99, 98, 97, 96, 95, 94]);

        test_receiving_na_from_known_neighbor(
            &mut ctx,
            neighbor_ip,
            config.local_ip,
            device,
            false,
            false,
            false,
            Some(new_mac),
            NeighborEntryState::Stale,
            false,
            Some(neighbor_mac),
        );

        //
        // Receive unsolicted NA from a neighbor with new target link addr and override set.
        //
        // Should update link layer addr and set state to STALE.
        //

        test_receiving_na_from_known_neighbor(
            &mut ctx,
            neighbor_ip,
            config.local_ip,
            device,
            false,
            false,
            true,
            Some(new_mac),
            NeighborEntryState::Stale,
            false,
            Some(new_mac),
        );

        //
        // Receive solicted NA from a neighbor with the same link layer addr.
        //
        // Should not update link layer addr, but set state to REACHABLE.
        //

        test_receiving_na_from_known_neighbor(
            &mut ctx,
            neighbor_ip,
            config.local_ip,
            device,
            false,
            true,
            false,
            Some(new_mac),
            NeighborEntryState::Reachable,
            false,
            Some(new_mac),
        );

        //
        // Receive unsolicted NA from a neighbor with new target link addr and override set.
        //
        // Should update link layer addr, and set state to Stale.
        //

        test_receiving_na_from_known_neighbor(
            &mut ctx,
            neighbor_ip,
            config.local_ip,
            device,
            false,
            false,
            true,
            Some(neighbor_mac),
            NeighborEntryState::Stale,
            false,
            Some(neighbor_mac),
        );

        //
        // Receive solicted NA from a neighbor with new target link addr and override set.
        //
        // Should set state to Reachable.
        //

        test_receiving_na_from_known_neighbor(
            &mut ctx,
            neighbor_ip,
            config.local_ip,
            device,
            false,
            true,
            true,
            Some(neighbor_mac),
            NeighborEntryState::Reachable,
            false,
            Some(neighbor_mac),
        );

        //
        // Receive unsolicted NA from a neighbor with no target link addr and overide set.
        //
        // Should do nothing..
        //

        test_receiving_na_from_known_neighbor(
            &mut ctx,
            neighbor_ip,
            config.local_ip,
            device,
            false,
            false,
            true,
            None,
            NeighborEntryState::Reachable,
            false,
            Some(neighbor_mac),
        );
    }
}
