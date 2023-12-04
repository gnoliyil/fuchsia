// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The integrations for protocols built on top of an IP device.

use alloc::boxed::Box;
use core::{
    borrow::Borrow,
    marker::PhantomData,
    num::NonZeroU8,
    ops::{Deref as _, DerefMut as _},
    sync::atomic::AtomicU16,
};

use lock_order::{lock::LockFor, relation::LockBefore, Locked};
use net_types::{
    ip::{AddrSubnet, Ip, Ipv4, Ipv4Addr, Ipv6, Ipv6Addr, Mtu},
    LinkLocalUnicastAddr, MulticastAddr, SpecifiedAddr, UnicastAddr, Witness as _,
};
use packet::{BufferMut, EmptyBuf, Serializer};
use packet_formats::{
    icmp::{
        ndp::{NeighborSolicitation, RouterSolicitation},
        IcmpUnusedCode,
    },
    utils::NonZeroDuration,
};

use crate::{
    context::{CounterContext, InstantContext, SendFrameContext},
    device::{AnyDevice, DeviceIdContext},
    error::{ExistsError, NotFoundError},
    ip::{
        self,
        device::{
            self, add_ipv6_addr_subnet_with_config,
            dad::{DadAddressContext, DadAddressStateRef, DadContext, DadHandler, DadStateRef},
            del_ipv6_addr_with_config, get_ipv6_hop_limit, is_ip_device_enabled,
            is_ip_forwarding_enabled, join_ip_multicast_with_config,
            leave_ip_multicast_with_config,
            nud::{self, ConfirmationFlags, NudIpHandler},
            route_discovery::{
                Ipv6DiscoveredRoute, Ipv6DiscoveredRoutesContext, Ipv6RouteDiscoveryContext,
                Ipv6RouteDiscoveryState,
            },
            router_solicitation::{RsContext, RsHandler},
            slaac::{
                SlaacAddressEntry, SlaacAddressEntryMut, SlaacAddresses, SlaacAddrsMutAndConfig,
                SlaacContext, SlaacCounters,
            },
            state::{
                DualStackIpDeviceState, IpDeviceConfiguration, IpDeviceFlags,
                Ipv4DeviceConfiguration, Ipv6AddrConfig, Ipv6AddressFlags, Ipv6AddressState,
                Ipv6DeviceConfiguration, SlaacConfig,
            },
            DelIpv6Addr, IpAddressId, IpDeviceIpExt, IpDeviceNonSyncContext, IpDeviceSendContext,
            IpDeviceStateContext, RemovedReason,
        },
        gmp::{
            self,
            igmp::{IgmpContext, IgmpGroupState, IgmpPacketMetadata, IgmpStateContext},
            mld::{MldContext, MldFrameMetadata, MldGroupState, MldStateContext},
            GmpHandler, GmpQueryHandler, GmpState, MulticastGroupSet,
        },
        socket::ipv6_source_address_selection::SasCandidate,
        types::AddableMetric,
        AddressStatus, IpLayerIpExt, IpStateContext, Ipv4PresentAddressStatus,
        Ipv6PresentAddressStatus, NonSyncContext, SyncCtx, DEFAULT_TTL,
    },
    DeviceId,
};

pub(crate) struct SlaacAddrs<'a, C: NonSyncContext> {
    pub(crate) sync_ctx: SyncCtxWithIpDeviceConfiguration<
        'a,
        &'a Ipv6DeviceConfiguration,
        crate::lock_ordering::IpDeviceConfiguration<Ipv6>,
        C,
    >,
    pub(crate) device_id: <SyncCtxWithIpDeviceConfiguration<
        'a,
        &'a Ipv6DeviceConfiguration,
        crate::lock_ordering::IpDeviceConfiguration<Ipv6>,
        C,
    > as DeviceIdContext<AnyDevice>>::DeviceId,
    pub(crate) config: &'a Ipv6DeviceConfiguration,
    pub(crate) _marker: PhantomData<C>,
}

impl<'a, C: NonSyncContext> CounterContext<SlaacCounters> for SlaacAddrs<'a, C> {
    fn with_counters<O, F: FnOnce(&SlaacCounters) -> O>(&self, cb: F) -> O {
        cb(self.sync_ctx.sync_ctx.unlocked_access::<crate::lock_ordering::SlaacCounters>())
    }
}

impl<'a, C: NonSyncContext> SlaacAddresses<C> for SlaacAddrs<'a, C> {
    fn for_each_addr_mut<F: FnMut(SlaacAddressEntryMut<'_, C::Instant>)>(&mut self, mut cb: F) {
        let SlaacAddrs { sync_ctx, device_id, config: _, _marker } = self;
        let SyncCtxWithIpDeviceConfiguration { config: _, sync_ctx } = sync_ctx;
        crate::device::integration::with_ip_device_state(sync_ctx, device_id, |mut state| {
            let addrs = state.read_lock::<crate::lock_ordering::IpDeviceAddresses<Ipv6>>();
            addrs.iter().for_each(|entry| {
                let addr_sub = entry.addr_sub;
                let mut entry =
                    Locked::<_, crate::lock_ordering::IpDeviceAddresses<Ipv6>>::new_locked(
                        &**entry,
                    );
                let mut state = entry.write_lock::<crate::lock_ordering::Ipv6DeviceAddressState>();
                let Ipv6AddressState {
                    config,
                    flags: Ipv6AddressFlags { deprecated, assigned: _ },
                } = &mut *state;

                match config {
                    Ipv6AddrConfig::Slaac(config) => {
                        cb(SlaacAddressEntryMut { addr_sub, config, deprecated })
                    }
                    Ipv6AddrConfig::Manual(_manual_config) => {}
                }
            })
        })
    }

    fn with_addrs<
        O,
        F: FnOnce(Box<dyn Iterator<Item = SlaacAddressEntry<C::Instant>> + '_>) -> O,
    >(
        &mut self,
        cb: F,
    ) -> O {
        let SlaacAddrs { sync_ctx, device_id, config: _, _marker } = self;
        device::IpDeviceStateContext::<Ipv6, C>::with_address_ids(
            sync_ctx,
            device_id,
            |addrs, sync_ctx| {
                cb(Box::new(addrs.filter_map(|addr_id| {
                    device::IpDeviceAddressContext::<Ipv6, _>::with_ip_address_state(
                        sync_ctx,
                        device_id,
                        &addr_id,
                        |Ipv6AddressState {
                             flags: Ipv6AddressFlags { deprecated, assigned: _ },
                             config,
                         }| {
                            let addr_sub = addr_id.addr_sub();
                            match config {
                                Ipv6AddrConfig::Slaac(config) => Some(SlaacAddressEntry {
                                    addr_sub,
                                    config: *config,
                                    deprecated: *deprecated,
                                }),
                                Ipv6AddrConfig::Manual(_manual_config) => None,
                            }
                        },
                    )
                })))
            },
        )
    }

    fn add_addr_sub_and_then<O, F: FnOnce(SlaacAddressEntryMut<'_, C::Instant>, &mut C) -> O>(
        &mut self,
        ctx: &mut C,
        add_addr_sub: AddrSubnet<Ipv6Addr, UnicastAddr<Ipv6Addr>>,
        slaac_config: SlaacConfig<C::Instant>,
        and_then: F,
    ) -> Result<O, ExistsError> {
        let SlaacAddrs { sync_ctx, device_id, config, _marker } = self;

        add_ipv6_addr_subnet_with_config(
            sync_ctx,
            ctx,
            device_id,
            add_addr_sub.to_witness(),
            Ipv6AddrConfig::Slaac(slaac_config),
            config,
        )
        .map(|entry| {
            let addr_sub = entry.addr_sub;
            let mut entry = Locked::<_, crate::lock_ordering::IpDeviceAddresses<Ipv6>>::new_locked(
                entry.deref(),
            );
            let mut state = entry.write_lock::<crate::lock_ordering::Ipv6DeviceAddressState>();
            let Ipv6AddressState { config, flags: Ipv6AddressFlags { deprecated, assigned: _ } } =
                &mut *state;
            and_then(
                SlaacAddressEntryMut {
                    addr_sub,
                    config: assert_matches::assert_matches!(
                        config,
                        Ipv6AddrConfig::Slaac(c) => c
                    ),
                    deprecated,
                },
                ctx,
            )
        })
    }

    fn remove_addr(
        &mut self,
        ctx: &mut C,
        addr: &UnicastAddr<Ipv6Addr>,
    ) -> Result<(AddrSubnet<Ipv6Addr, UnicastAddr<Ipv6Addr>>, SlaacConfig<C::Instant>), NotFoundError>
    {
        let SlaacAddrs { sync_ctx, device_id, config, _marker } = self;
        del_ipv6_addr_with_config(
            sync_ctx,
            ctx,
            device_id,
            DelIpv6Addr::SpecifiedAddr(addr.into_specified()),
            RemovedReason::Manual,
            config,
        )
        .map(|(addr_sub, config)| {
            assert_eq!(&addr_sub.addr(), addr);
            match config {
                Ipv6AddrConfig::Slaac(s) => (addr_sub, s),
                Ipv6AddrConfig::Manual(_manual_config) => {
                    unreachable!(
                        "address {addr_sub} on device {device_id:?} should have been a SLAAC \
                        address; config = {config:?}",
                    );
                }
            }
        })
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::IpDeviceGmp<Ipv4>>> IgmpStateContext<C>
    for Locked<&SyncCtx<C>, L>
{
    fn with_igmp_state<
        O,
        F: FnOnce(&MulticastGroupSet<Ipv4Addr, IgmpGroupState<C::Instant>>) -> O,
    >(
        &mut self,
        device: &Self::DeviceId,
        cb: F,
    ) -> O {
        crate::device::integration::with_ip_device_state(self, device, |mut state| {
            let state = state.read_lock::<crate::lock_ordering::IpDeviceGmp<Ipv4>>();
            cb(&state)
        })
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::IpDeviceGmp<Ipv6>>> MldStateContext<C>
    for Locked<&SyncCtx<C>, L>
{
    fn with_mld_state<
        O,
        F: FnOnce(&MulticastGroupSet<Ipv6Addr, MldGroupState<C::Instant>>) -> O,
    >(
        &mut self,
        device: &Self::DeviceId,
        cb: F,
    ) -> O {
        crate::device::integration::with_ip_device_state(self, device, |mut state| {
            let state = state.read_lock::<crate::lock_ordering::IpDeviceGmp<Ipv6>>();
            cb(&state)
        })
    }
}

/// Iterator over a device and its status for an address.
///
/// This is functionally identical to using `Iterator::filter_map` on the
/// provided devices and yielding devices with the address assigned (and the
/// status), but is named so that it can be used as an associated type.
pub(crate) struct FilterPresentWithDevices<
    I: IpLayerIpExt,
    Devices: Iterator<Item = Accessor::DeviceId>,
    Accessor: DeviceIdContext<AnyDevice>,
    C,
> {
    devices: Devices,
    addr: SpecifiedAddr<I::Addr>,
    state_accessor: Accessor,
    assignment_state: fn(
        &mut Accessor,
        &Accessor::DeviceId,
        SpecifiedAddr<I::Addr>,
    ) -> AddressStatus<I::AddressStatus>,
    _marker: PhantomData<C>,
}

impl<
        I: IpLayerIpExt,
        Devices: Iterator<Item = Accessor::DeviceId>,
        Accessor: DeviceIdContext<AnyDevice>,
        C,
    > FilterPresentWithDevices<I, Devices, Accessor, C>
{
    fn new(
        devices: Devices,
        state_accessor: Accessor,
        assignment_state: fn(
            &mut Accessor,
            &Accessor::DeviceId,
            SpecifiedAddr<I::Addr>,
        ) -> AddressStatus<I::AddressStatus>,
        addr: SpecifiedAddr<I::Addr>,
    ) -> Self {
        Self { devices, addr, state_accessor, assignment_state, _marker: PhantomData }
    }
}

impl<
        's,
        C: InstantContext,
        I: Ip + IpLayerIpExt + IpDeviceIpExt,
        Devices: Iterator<Item = Accessor::DeviceId>,
        Accessor: IpDeviceStateContext<I, C> + GmpQueryHandler<I, C>,
    > Iterator for FilterPresentWithDevices<I, Devices, Accessor, C>
where
    <I as IpDeviceIpExt>::State<C::Instant>: 's,
{
    type Item = (Accessor::DeviceId, I::AddressStatus);
    fn next(&mut self) -> Option<Self::Item> {
        let Self { devices, addr, state_accessor, assignment_state, _marker } = self;
        devices
            .filter_map(|d| match assignment_state(state_accessor, &d, *addr) {
                AddressStatus::Present(status) => Some((d, status)),
                AddressStatus::Unassigned => None,
            })
            .next()
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::IpDeviceGmp<Ipv4>>>
    ip::IpDeviceStateContext<Ipv4, C> for Locked<&SyncCtx<C>, L>
{
    fn with_next_packet_id<O, F: FnOnce(&AtomicU16) -> O>(&self, cb: F) -> O {
        cb(self.unlocked_access::<crate::lock_ordering::Ipv4StateNextPacketId>())
    }

    fn get_local_addr_for_remote(
        &mut self,
        device_id: &Self::DeviceId,
        _remote: Option<SpecifiedAddr<Ipv4Addr>>,
    ) -> Option<SpecifiedAddr<Ipv4Addr>> {
        device::IpDeviceStateContext::<Ipv4, _>::with_address_ids(
            self,
            device_id,
            |mut addrs, _sync_ctx| addrs.next().as_ref().map(IpAddressId::addr),
        )
    }

    fn get_hop_limit(&mut self, _device_id: &Self::DeviceId) -> NonZeroU8 {
        DEFAULT_TTL
    }

    fn address_status_for_device(
        &mut self,
        dst_ip: SpecifiedAddr<Ipv4Addr>,
        device_id: &Self::DeviceId,
    ) -> AddressStatus<Ipv4PresentAddressStatus> {
        if dst_ip.is_limited_broadcast() {
            return AddressStatus::Present(Ipv4PresentAddressStatus::LimitedBroadcast);
        }

        assignment_state_v4(self, device_id, dst_ip)
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::IpDeviceConfiguration<Ipv4>>>
    ip::IpDeviceContext<Ipv4, C> for Locked<&SyncCtx<C>, L>
{
    fn is_ip_device_enabled(&mut self, device_id: &Self::DeviceId) -> bool {
        is_ip_device_enabled::<Ipv4, _, _>(self, device_id)
    }

    type DeviceAndAddressStatusIter<'a, 's> =
        FilterPresentWithDevices<Ipv4, <Self as device::IpDeviceConfigurationContext<Ipv4, C>>::DevicesIter<'a>, <Self as device::IpDeviceConfigurationContext<Ipv4, C>>::DeviceAddressAndGroupsAccessor<'a>, C> where
                Self: 's;

    fn with_address_statuses<'a, F: FnOnce(Self::DeviceAndAddressStatusIter<'_, 'a>) -> R, R>(
        &'a mut self,
        addr: SpecifiedAddr<Ipv4Addr>,
        cb: F,
    ) -> R {
        device::IpDeviceConfigurationContext::<Ipv4, _>::with_devices_and_state(
            self,
            |devices, state| {
                cb(FilterPresentWithDevices::new(devices, state, assignment_state_v4, addr))
            },
        )
    }

    fn is_device_forwarding_enabled(&mut self, device_id: &Self::DeviceId) -> bool {
        is_ip_forwarding_enabled::<Ipv4, _, _>(self, device_id)
    }

    fn get_mtu(&mut self, device_id: &Self::DeviceId) -> Mtu {
        device::IpDeviceConfigurationContext::<Ipv4, _>::get_mtu(self, device_id)
    }

    fn confirm_reachable(
        &mut self,
        ctx: &mut C,
        device: &Self::DeviceId,
        neighbor: SpecifiedAddr<<Ipv4 as Ip>::Addr>,
    ) {
        match device {
            DeviceId::Ethernet(id) => {
                nud::confirm_reachable::<Ipv4, _, _, _>(self, ctx, id, neighbor)
            }
            DeviceId::Loopback(_) => {}
        }
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::IpState<Ipv6>>>
    ip::IpDeviceStateContext<Ipv6, C> for Locked<&SyncCtx<C>, L>
{
    fn with_next_packet_id<O, F: FnOnce(&()) -> O>(&self, cb: F) -> O {
        cb(&())
    }

    fn get_local_addr_for_remote(
        &mut self,
        device_id: &Self::DeviceId,
        remote: Option<SpecifiedAddr<Ipv6Addr>>,
    ) -> Option<SpecifiedAddr<Ipv6Addr>> {
        device::IpDeviceStateContext::<Ipv6, C>::with_address_ids(
            self,
            device_id,
            |addrs, sync_ctx| {
                crate::ip::socket::ipv6_source_address_selection::select_ipv6_source_address(
                    remote,
                    device_id,
                    addrs.map(|addr_id| {
                        device::IpDeviceAddressContext::<Ipv6, _>::with_ip_address_state(
                            sync_ctx,
                            device_id,
                            &addr_id,
                            |Ipv6AddressState { flags, config: _ }| SasCandidate {
                                addr_sub: addr_id.addr_sub(),
                                flags: *flags,
                                device: device_id.clone(),
                            },
                        )
                    }),
                )
                .map(|a| a.into_specified())
            },
        )
    }

    fn get_hop_limit(&mut self, device_id: &Self::DeviceId) -> NonZeroU8 {
        get_ipv6_hop_limit(self, device_id)
    }

    fn address_status_for_device(
        &mut self,
        addr: SpecifiedAddr<Ipv6Addr>,
        device_id: &Self::DeviceId,
    ) -> AddressStatus<<Ipv6 as IpLayerIpExt>::AddressStatus> {
        assignment_state_v6(self, device_id, addr)
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::IpDeviceConfiguration<Ipv6>>>
    ip::IpDeviceContext<Ipv6, C> for Locked<&SyncCtx<C>, L>
{
    fn is_ip_device_enabled(&mut self, device_id: &Self::DeviceId) -> bool {
        is_ip_device_enabled::<Ipv6, _, _>(self, device_id)
    }

    type DeviceAndAddressStatusIter<'a, 's> =
        FilterPresentWithDevices<Ipv6, <Self as device::IpDeviceConfigurationContext<Ipv6, C>>::DevicesIter<'a>, <Self as device::IpDeviceConfigurationContext<Ipv6, C>>::DeviceAddressAndGroupsAccessor<'a>, C> where
                Self: 's;

    fn with_address_statuses<'a, F: FnOnce(Self::DeviceAndAddressStatusIter<'_, 'a>) -> R, R>(
        &'a mut self,
        addr: SpecifiedAddr<Ipv6Addr>,
        cb: F,
    ) -> R {
        device::IpDeviceConfigurationContext::<Ipv6, _>::with_devices_and_state(
            self,
            |devices, state| {
                cb(FilterPresentWithDevices::new(devices, state, assignment_state_v6, addr))
            },
        )
    }

    fn is_device_forwarding_enabled(&mut self, device_id: &Self::DeviceId) -> bool {
        is_ip_forwarding_enabled::<Ipv6, _, _>(self, device_id)
    }

    fn get_mtu(&mut self, device_id: &Self::DeviceId) -> Mtu {
        device::IpDeviceConfigurationContext::<Ipv6, _>::get_mtu(self, device_id)
    }

    fn confirm_reachable(
        &mut self,
        ctx: &mut C,
        device: &Self::DeviceId,
        neighbor: SpecifiedAddr<<Ipv6 as Ip>::Addr>,
    ) {
        match device {
            DeviceId::Ethernet(id) => {
                nud::confirm_reachable::<Ipv6, _, _, _>(self, ctx, id, neighbor)
            }
            DeviceId::Loopback(_) => {}
        }
    }
}

fn assignment_state_v4<
    C: InstantContext,
    SC: IpDeviceStateContext<Ipv4, C> + GmpQueryHandler<Ipv4, C>,
>(
    sync_ctx: &mut SC,
    device: &SC::DeviceId,
    addr: SpecifiedAddr<Ipv4Addr>,
) -> AddressStatus<Ipv4PresentAddressStatus> {
    if MulticastAddr::new(addr.get())
        .map_or(false, |addr| GmpQueryHandler::gmp_is_in_group(sync_ctx, device, addr))
    {
        return AddressStatus::Present(Ipv4PresentAddressStatus::Multicast);
    }

    sync_ctx.with_address_ids(device, |mut addrs, _sync_ctx| {
        addrs
            .find_map(|addr_id| {
                let dev_addr = addr_id.addr_sub();
                let (dev_addr, subnet) = dev_addr.addr_subnet();

                if dev_addr == addr {
                    Some(AddressStatus::Present(Ipv4PresentAddressStatus::Unicast))
                } else if addr.get() == subnet.broadcast() {
                    Some(AddressStatus::Present(Ipv4PresentAddressStatus::SubnetBroadcast))
                } else {
                    None
                }
            })
            .unwrap_or(AddressStatus::Unassigned)
    })
}

fn assignment_state_v6<
    C: IpDeviceNonSyncContext<Ipv6, SC::DeviceId>,
    SC: device::Ipv6DeviceContext<C> + GmpQueryHandler<Ipv6, C>,
>(
    sync_ctx: &mut SC,
    device: &SC::DeviceId,
    addr: SpecifiedAddr<Ipv6Addr>,
) -> AddressStatus<Ipv6PresentAddressStatus> {
    if MulticastAddr::new(addr.get())
        .map_or(false, |addr| GmpQueryHandler::gmp_is_in_group(sync_ctx, device, addr))
    {
        return AddressStatus::Present(Ipv6PresentAddressStatus::Multicast);
    }

    let addr_id = match sync_ctx.get_address_id(device, addr) {
        Ok(o) => o,
        Err(NotFoundError) => return AddressStatus::Unassigned,
    };

    let assigned = sync_ctx.with_ip_address_state(
        device,
        &addr_id,
        |Ipv6AddressState { flags: Ipv6AddressFlags { deprecated: _, assigned }, config: _ }| {
            *assigned
        },
    );

    if assigned {
        AddressStatus::Present(Ipv6PresentAddressStatus::UnicastAssigned)
    } else {
        AddressStatus::Present(Ipv6PresentAddressStatus::UnicastTentative)
    }
}
pub(crate) struct SyncCtxWithIpDeviceConfiguration<'a, Config, L, C: NonSyncContext> {
    pub config: Config,
    pub sync_ctx: Locked<&'a SyncCtx<C>, L>,
}

impl<'a, I: gmp::IpExt + IpDeviceIpExt, C: NonSyncContext>
    device::WithIpDeviceConfigurationMutInner<I, C>
    for SyncCtxWithIpDeviceConfiguration<
        'a,
        &mut I::Configuration,
        crate::lock_ordering::IpDeviceConfiguration<I>,
        C,
    >
where
    Locked<&'a SyncCtx<C>, crate::lock_ordering::IpDeviceConfiguration<I>>:
        device::IpDeviceStateContext<I, C, DeviceId = crate::DeviceId<C>>,
    for<'s> SyncCtxWithIpDeviceConfiguration<
        's,
        &'s I::Configuration,
        crate::lock_ordering::IpDeviceConfiguration<I>,
        C,
    >: IpDeviceStateContext<I, C, DeviceId = Self::DeviceId>
        + GmpHandler<I, C>
        + NudIpHandler<I, C>,
    DualStackIpDeviceState<C::Instant>:
        LockFor<crate::lock_ordering::IpDeviceFlags<I>, Data = IpDeviceFlags>,
    crate::lock_ordering::IpDeviceConfiguration<I>:
        LockBefore<crate::lock_ordering::IpDeviceFlags<I>>,
{
    type IpDeviceStateCtx<'s> = SyncCtxWithIpDeviceConfiguration<'s, &'s I::Configuration, crate::lock_ordering::IpDeviceConfiguration<I>, C> where Self: 's;

    fn ip_device_configuration_and_ctx(
        &mut self,
    ) -> (&I::Configuration, Self::IpDeviceStateCtx<'_>) {
        let Self { config, sync_ctx } = self;
        let config = &**config;
        (config, SyncCtxWithIpDeviceConfiguration { config, sync_ctx: sync_ctx.as_owned() })
    }

    fn with_configuration_and_flags_mut<
        O,
        F: FnOnce(&mut I::Configuration, &mut IpDeviceFlags) -> O,
    >(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O {
        let Self { config, sync_ctx } = self;
        crate::device::integration::with_ip_device_state(sync_ctx, device_id, |mut state| {
            let mut flags = state.lock::<crate::lock_ordering::IpDeviceFlags<I>>();
            cb(*config, &mut *flags)
        })
    }
}

impl<'a, C: NonSyncContext> device::WithIpv6DeviceConfigurationMutInner<C>
    for SyncCtxWithIpDeviceConfiguration<
        'a,
        &mut Ipv6DeviceConfiguration,
        crate::lock_ordering::IpDeviceConfiguration<Ipv6>,
        C,
    >
where
    for<'s> SyncCtxWithIpDeviceConfiguration<
        's,
        &'s Ipv6DeviceConfiguration,
        crate::lock_ordering::IpDeviceConfiguration<Ipv6>,
        C,
    >: IpDeviceStateContext<Ipv6, C, DeviceId = Self::DeviceId>
        + GmpHandler<Ipv6, C>
        + NudIpHandler<Ipv6, C>
        + DadHandler<C>
        + RsHandler<C>,
{
    type Ipv6DeviceStateCtx<'s> = SyncCtxWithIpDeviceConfiguration<'s, &'s Ipv6DeviceConfiguration,crate::lock_ordering::IpDeviceConfiguration<Ipv6>, C> where Self: 's;

    fn ipv6_device_configuration_and_ctx(
        &mut self,
    ) -> (&Ipv6DeviceConfiguration, Self::Ipv6DeviceStateCtx<'_>) {
        let Self { config, sync_ctx } = self;
        let config = &**config;
        (config, SyncCtxWithIpDeviceConfiguration { config, sync_ctx: sync_ctx.as_owned() })
    }
}

impl<'a, Config, C: NonSyncContext, L> DeviceIdContext<AnyDevice>
    for SyncCtxWithIpDeviceConfiguration<'a, Config, L, C>
{
    type DeviceId = <Locked<&'a SyncCtx<C>, L> as DeviceIdContext<AnyDevice>>::DeviceId;
    type WeakDeviceId = <Locked<&'a SyncCtx<C>, L> as DeviceIdContext<AnyDevice>>::WeakDeviceId;

    fn downgrade_device_id(&self, device_id: &Self::DeviceId) -> Self::WeakDeviceId {
        let Self { config: _, sync_ctx } = self;
        <Locked<&'a SyncCtx<C>, L> as DeviceIdContext<AnyDevice>>::downgrade_device_id(
            sync_ctx, device_id,
        )
    }

    fn upgrade_weak_device_id(
        &self,
        weak_device_id: &Self::WeakDeviceId,
    ) -> Option<Self::DeviceId> {
        let Self { config: _, sync_ctx } = self;
        <Locked<&'a SyncCtx<C>, L> as DeviceIdContext<AnyDevice>>::upgrade_weak_device_id(
            sync_ctx,
            weak_device_id,
        )
    }
}

impl<'a, Config: Borrow<Ipv6DeviceConfiguration>, C: NonSyncContext> SlaacContext<C>
    for SyncCtxWithIpDeviceConfiguration<
        'a,
        Config,
        crate::lock_ordering::IpDeviceConfiguration<Ipv6>,
        C,
    >
{
    type SlaacAddrs<'s> = SlaacAddrs<'s, C>;

    fn with_slaac_addrs_mut_and_configs<
        O,
        F: FnOnce(SlaacAddrsMutAndConfig<'_, C, Self::SlaacAddrs<'_>>) -> O,
    >(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O {
        let Self { config, sync_ctx } = self;
        let retrans_timer =
            device::Ipv6DeviceContext::with_retrans_timer(sync_ctx, device_id, |retrans_timer| {
                retrans_timer.get()
            });
        let interface_identifier = device::Ipv6DeviceContext::get_eui64_iid(sync_ctx, device_id)
            .unwrap_or_else(Default::default);

        let config = Borrow::borrow(config);
        let Ipv6DeviceConfiguration {
            dad_transmits,
            max_router_solicitations: _,
            slaac_config,
            ip_config: _,
        } = *config;

        let sync_ctx = SyncCtxWithIpDeviceConfiguration { config, sync_ctx: sync_ctx.as_owned() };

        let mut addrs =
            SlaacAddrs { sync_ctx, device_id: device_id.clone(), config, _marker: PhantomData };

        cb(SlaacAddrsMutAndConfig {
            addrs: &mut addrs,
            config: slaac_config,
            dad_transmits,
            retrans_timer,
            interface_identifier,
            _marker: PhantomData,
        })
    }
}

impl<C: NonSyncContext, L: LockBefore<crate::lock_ordering::IpState<Ipv6>>> DadAddressContext<C>
    for SyncCtxWithIpDeviceConfiguration<'_, &'_ Ipv6DeviceConfiguration, L, C>
{
    fn with_address_assigned<O, F: FnOnce(&mut bool) -> O>(
        &mut self,
        _: &Self::DeviceId,
        addr: &Self::AddressId,
        cb: F,
    ) -> O {
        let mut entry = Locked::<_, L>::new_locked(addr.deref());
        let mut state = entry.write_lock::<crate::lock_ordering::Ipv6DeviceAddressState>();
        let Ipv6AddressState { flags: Ipv6AddressFlags { deprecated: _, assigned }, config: _ } =
            &mut *state;

        cb(assigned)
    }

    fn join_multicast_group(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        multicast_addr: MulticastAddr<Ipv6Addr>,
    ) {
        let Self { config, sync_ctx } = self;
        let config = Borrow::borrow(&*config);
        join_ip_multicast_with_config(
            &mut SyncCtxWithIpDeviceConfiguration { config, sync_ctx: sync_ctx.as_owned() },
            ctx,
            device_id,
            multicast_addr,
            config,
        )
    }

    fn leave_multicast_group(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        multicast_addr: MulticastAddr<Ipv6Addr>,
    ) {
        let Self { config, sync_ctx } = self;
        let config = Borrow::borrow(&*config);
        leave_ip_multicast_with_config(
            &mut SyncCtxWithIpDeviceConfiguration { config, sync_ctx: sync_ctx.as_owned() },
            ctx,
            device_id,
            multicast_addr,
            config,
        )
    }
}

impl<'a, Config: Borrow<Ipv6DeviceConfiguration>, C: NonSyncContext> DadContext<C>
    for SyncCtxWithIpDeviceConfiguration<
        'a,
        Config,
        crate::lock_ordering::IpDeviceConfiguration<Ipv6>,
        C,
    >
{
    type DadAddressCtx<'b> = SyncCtxWithIpDeviceConfiguration<
        'b,
        &'b Ipv6DeviceConfiguration,
        crate::lock_ordering::Ipv6DeviceAddressDad,
        C,
    >;

    fn get_address_id(
        &mut self,
        device_id: &Self::DeviceId,
        addr: UnicastAddr<Ipv6Addr>,
    ) -> Self::AddressId {
        device::IpDeviceStateContext::<Ipv6, C>::get_address_id(
            self,
            device_id,
            addr.into_specified(),
        )
        .expect("DAD address must always exist")
    }

    fn with_dad_state<O, F: FnOnce(DadStateRef<'_, Self::DadAddressCtx<'_>>) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        addr: &Self::AddressId,
        cb: F,
    ) -> O {
        let Self { config, sync_ctx } = self;
        let retrans_timer =
            device::Ipv6DeviceContext::<C>::with_retrans_timer(sync_ctx, device_id, |s| *s);

        let mut entry = {
            // Get a `Locked` at the same lock-level of our `sync_ctx`. We show
            // add an empty assignment here for readability to make it clear that
            // we are operating at the same lock-level.
            type CurrentLockLevel = crate::lock_ordering::IpDeviceConfiguration<Ipv6>;
            let _: &mut Locked<_, CurrentLockLevel> = sync_ctx;
            Locked::<_, CurrentLockLevel>::new_locked(addr.deref())
        };

        let config = Borrow::borrow(&*config);

        let (mut dad_state, mut sync_ctx) = {
            // The type/lock-levevel is specified when `Self::DadAddressCtx<'_>` is
            // impl-ed but we explicitly specify it here as well to audit this in
            // the event of a lock-level change.
            type AcquireLockLevel = crate::lock_ordering::Ipv6DeviceAddressDad;
            (
                entry.lock::<AcquireLockLevel>(),
                SyncCtxWithIpDeviceConfiguration {
                    config,
                    sync_ctx: sync_ctx.cast_locked::<AcquireLockLevel>(),
                },
            )
        };

        cb(DadStateRef {
            state: Some(DadAddressStateRef {
                dad_state: dad_state.deref_mut(),
                sync_ctx: &mut sync_ctx,
            }),
            retrans_timer: &retrans_timer,
            max_dad_transmits: &config.dad_transmits,
        })
    }

    fn send_dad_packet(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        dst_ip: MulticastAddr<Ipv6Addr>,
        message: NeighborSolicitation,
    ) -> Result<(), ()> {
        let Self { config: _, sync_ctx } = self;
        crate::ip::icmp::send_ndp_packet(
            sync_ctx,
            ctx,
            device_id,
            None,
            dst_ip.into_specified(),
            EmptyBuf,
            IcmpUnusedCode,
            message,
        )
        .map_err(|EmptyBuf| ())
    }
}

impl<'a, Config: Borrow<Ipv6DeviceConfiguration>, C: NonSyncContext> RsContext<C>
    for SyncCtxWithIpDeviceConfiguration<
        'a,
        Config,
        crate::lock_ordering::IpDeviceConfiguration<Ipv6>,
        C,
    >
{
    type LinkLayerAddr = <Locked<&'a SyncCtx<C>, crate::lock_ordering::IpDeviceConfiguration<Ipv6>> as device::Ipv6DeviceContext<C>>::LinkLayerAddr;

    /// Calls the callback with a mutable reference to the remaining number of
    /// router soliciations to send and the maximum number of router solications
    /// to send.
    fn with_rs_remaining_mut_and_max<
        O,
        F: FnOnce(&mut Option<NonZeroU8>, Option<NonZeroU8>) -> O,
    >(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O {
        let Self { config, sync_ctx } = self;
        crate::device::integration::with_ip_device_state(sync_ctx, device_id, |mut state| {
            let mut state = state.lock::<crate::lock_ordering::Ipv6DeviceRouterSolicitations>();
            cb(&mut state, Borrow::borrow(&*config).max_router_solicitations)
        })
    }

    /// Gets the device's link-layer address bytes, if the device supports
    /// link-layer addressing.
    fn get_link_layer_addr_bytes(
        &mut self,
        device_id: &Self::DeviceId,
    ) -> Option<Self::LinkLayerAddr> {
        let Self { config: _, sync_ctx } = self;
        device::Ipv6DeviceContext::get_link_layer_addr_bytes(sync_ctx, device_id)
    }

    /// Sends an NDP Router Solicitation to the local-link.
    ///
    /// The callback is called with a source address suitable for an outgoing
    /// router solicitation message and returns the message body.
    fn send_rs_packet<
        S: Serializer<Buffer = EmptyBuf>,
        F: FnOnce(Option<UnicastAddr<Ipv6Addr>>) -> S,
    >(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        message: RouterSolicitation,
        body: F,
    ) -> Result<(), S> {
        let Self { config: _, sync_ctx } = self;
        let dst_ip = Ipv6::ALL_ROUTERS_LINK_LOCAL_MULTICAST_ADDRESS.into_specified();
        let src_ip = device::IpDeviceStateContext::<Ipv6, C>::with_address_ids(
            sync_ctx,
            device_id,
            |addrs, sync_ctx| {
                crate::ip::socket::ipv6_source_address_selection::select_ipv6_source_address(
                    Some(dst_ip),
                    device_id,
                    addrs.map(|addr_id| {
                        device::IpDeviceAddressContext::<Ipv6, _>::with_ip_address_state(
                            sync_ctx,
                            device_id,
                            &addr_id,
                            |Ipv6AddressState { flags, config: _ }| SasCandidate {
                                addr_sub: addr_id.addr_sub(),
                                flags: *flags,
                                device: device_id.clone(),
                            },
                        )
                    }),
                )
            },
        );
        crate::ip::icmp::send_ndp_packet(
            sync_ctx,
            ctx,
            device_id,
            src_ip.map(|a| a.into_specified()),
            dst_ip,
            body(src_ip),
            IcmpUnusedCode,
            message,
        )
    }
}

impl<C: NonSyncContext> Ipv6DiscoveredRoutesContext<C>
    for Locked<&SyncCtx<C>, crate::lock_ordering::Ipv6DeviceRouteDiscovery>
{
    fn add_discovered_ipv6_route(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        Ipv6DiscoveredRoute { subnet, gateway }: Ipv6DiscoveredRoute,
    ) -> Result<(), crate::error::ExistsError> {
        let device_id = device_id.clone();
        let entry = crate::ip::types::AddableEntry {
            subnet,
            device: device_id,
            gateway: gateway.map(|g| (*g).into_specified()),
            metric: AddableMetric::MetricTracksInterface,
        };

        // TODO(https://fxbug.dev/129219): Rather than perform a synchronous
        // check for whether the route already exists, use a routes-admin
        // RouteSet to track the NDP-added route.
        let already_exists = self.with_ip_routing_table(
            |_sync_ctx, table: &crate::ip::forwarding::ForwardingTable<Ipv6, _>| {
                table.iter_table().any(|table_entry: &crate::ip::types::Entry<Ipv6Addr, _>| {
                    &crate::ip::types::AddableEntry::from(table_entry.clone()) == &entry
                })
            },
        );

        if already_exists {
            return Err(crate::error::ExistsError);
        }

        crate::request_context_add_route(ctx, entry.into());
        Ok(())
    }

    fn del_discovered_ipv6_route(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        Ipv6DiscoveredRoute { subnet, gateway }: Ipv6DiscoveredRoute,
    ) {
        crate::request_context_del_routes_v6(
            ctx,
            subnet,
            device_id,
            gateway.map(|g| (*g).into_specified()),
        );
    }
}

impl<'a, Config, C: NonSyncContext> Ipv6RouteDiscoveryContext<C>
    for SyncCtxWithIpDeviceConfiguration<
        'a,
        Config,
        crate::lock_ordering::IpDeviceConfiguration<Ipv6>,
        C,
    >
{
    type WithDiscoveredRoutesMutCtx<'b> =
        Locked<&'b SyncCtx<C>, crate::lock_ordering::Ipv6DeviceRouteDiscovery>;

    fn with_discovered_routes_mut<
        F: FnOnce(&mut Ipv6RouteDiscoveryState, &mut Self::WithDiscoveredRoutesMutCtx<'_>),
    >(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) {
        let Self { config: _, sync_ctx } = self;
        crate::device::integration::with_ip_device_state_and_sync_ctx(
            sync_ctx,
            device_id,
            |mut state, sync_ctx| {
                let mut state = state.lock::<crate::lock_ordering::Ipv6DeviceRouteDiscovery>();
                // We lock the state according to the IPv6 route discovery
                // lock level but the callback needs access to the sync context
                // so we cast its lock-level to IPv6 route discovery so that
                // only locks that may be acquired _after_ the IPv6 route
                // discovery lock may be acquired.
                //
                // Note that specifying the lock-level type in `cast_locked`
                // is not explicitly needed as `Self::WithDiscoveredRoutesMutCtx`
                // includes the lock-level, but we do it here to be explicit
                // and catch changes to the associated type.
                //
                // TODO(https://fxbug.dev/126743): Support this natively in the
                // lock-order crate.
                cb(
                    &mut state,
                    &mut sync_ctx.cast_locked::<crate::lock_ordering::Ipv6DeviceRouteDiscovery>(),
                )
            },
        )
    }
}

impl<'a, Config, C: NonSyncContext> device::Ipv6DeviceContext<C>
    for SyncCtxWithIpDeviceConfiguration<
        'a,
        Config,
        crate::lock_ordering::IpDeviceConfiguration<Ipv6>,
        C,
    >
{
    type LinkLayerAddr = <Locked<&'a SyncCtx<C>, crate::lock_ordering::IpDeviceConfiguration<Ipv6>> as device::Ipv6DeviceContext<C>>::LinkLayerAddr;

    fn get_link_layer_addr_bytes(
        &mut self,
        device_id: &Self::DeviceId,
    ) -> Option<Self::LinkLayerAddr> {
        let Self { config: _, sync_ctx } = self;
        device::Ipv6DeviceContext::get_link_layer_addr_bytes(sync_ctx, device_id)
    }

    fn get_eui64_iid(&mut self, device_id: &Self::DeviceId) -> Option<[u8; 8]> {
        let Self { config: _, sync_ctx } = self;
        device::Ipv6DeviceContext::get_eui64_iid(sync_ctx, device_id)
    }

    fn set_link_mtu(&mut self, device_id: &Self::DeviceId, mtu: Mtu) {
        let Self { config: _, sync_ctx } = self;
        device::Ipv6DeviceContext::set_link_mtu(sync_ctx, device_id, mtu)
    }

    fn with_retrans_timer<O, F: FnOnce(&NonZeroDuration) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O {
        let Self { config: _, sync_ctx } = self;
        device::Ipv6DeviceContext::with_retrans_timer(sync_ctx, device_id, cb)
    }

    fn with_retrans_timer_mut<O, F: FnOnce(&mut NonZeroDuration) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O {
        let Self { config: _, sync_ctx } = self;
        device::Ipv6DeviceContext::with_retrans_timer_mut(sync_ctx, device_id, cb)
    }
}

impl<'a, Config, I: IpDeviceIpExt, L, C: NonSyncContext> device::IpDeviceAddressIdContext<I>
    for SyncCtxWithIpDeviceConfiguration<'a, Config, L, C>
where
    Locked<&'a SyncCtx<C>, L>: device::IpDeviceAddressIdContext<I>,
{
    type AddressId = <Locked<&'a SyncCtx<C>, L> as device::IpDeviceAddressIdContext<I>>::AddressId;
}

impl<'a, Config, I: IpDeviceIpExt, C: NonSyncContext, L> device::IpDeviceAddressContext<I, C>
    for SyncCtxWithIpDeviceConfiguration<'a, Config, L, C>
where
    Locked<&'a SyncCtx<C>, L>: device::IpDeviceAddressContext<I, C>,
{
    fn with_ip_address_state<O, F: FnOnce(&I::AddressState<C::Instant>) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        addr_id: &Self::AddressId,
        cb: F,
    ) -> O {
        let Self { config: _, sync_ctx } = self;
        device::IpDeviceAddressContext::<I, C>::with_ip_address_state(
            sync_ctx, device_id, addr_id, cb,
        )
    }

    fn with_ip_address_state_mut<O, F: FnOnce(&mut I::AddressState<C::Instant>) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        addr_id: &Self::AddressId,
        cb: F,
    ) -> O {
        let Self { config: _, sync_ctx } = self;
        device::IpDeviceAddressContext::<I, C>::with_ip_address_state_mut(
            sync_ctx, device_id, addr_id, cb,
        )
    }
}

impl<'a, Config, I: IpDeviceIpExt, C: NonSyncContext, L> device::IpDeviceStateContext<I, C>
    for SyncCtxWithIpDeviceConfiguration<'a, Config, L, C>
where
    Locked<&'a SyncCtx<C>, L>: device::IpDeviceStateContext<I, C>,
{
    type IpDeviceAddressCtx<'b> =
        <Locked<&'a SyncCtx<C>, L> as device::IpDeviceStateContext<I, C>>::IpDeviceAddressCtx<'b>;

    fn with_ip_device_flags<O, F: FnOnce(&IpDeviceFlags) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O {
        let Self { config: _, sync_ctx } = self;
        device::IpDeviceStateContext::<I, C>::with_ip_device_flags(sync_ctx, device_id, cb)
    }

    fn add_ip_address(
        &mut self,
        device_id: &Self::DeviceId,
        addr: AddrSubnet<I::Addr, I::AssignedWitness>,
        config: I::AddressConfig<C::Instant>,
    ) -> Result<Self::AddressId, ExistsError> {
        let Self { config: _, sync_ctx } = self;
        device::IpDeviceStateContext::<I, C>::add_ip_address(sync_ctx, device_id, addr, config)
    }

    fn remove_ip_address(
        &mut self,
        device_id: &Self::DeviceId,
        addr: Self::AddressId,
    ) -> (AddrSubnet<I::Addr, I::AssignedWitness>, I::AddressConfig<C::Instant>) {
        let Self { config: _, sync_ctx } = self;
        device::IpDeviceStateContext::<I, C>::remove_ip_address(sync_ctx, device_id, addr)
    }

    fn get_address_id(
        &mut self,
        device_id: &Self::DeviceId,
        addr: SpecifiedAddr<I::Addr>,
    ) -> Result<Self::AddressId, NotFoundError> {
        let Self { config: _, sync_ctx } = self;
        device::IpDeviceStateContext::<I, C>::get_address_id(sync_ctx, device_id, addr)
    }

    fn with_address_ids<
        O,
        F: FnOnce(
            Box<dyn Iterator<Item = Self::AddressId> + '_>,
            &mut Self::IpDeviceAddressCtx<'_>,
        ) -> O,
    >(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O {
        let Self { config: _, sync_ctx } = self;
        device::IpDeviceStateContext::<I, C>::with_address_ids(sync_ctx, device_id, cb)
    }

    fn with_default_hop_limit<O, F: FnOnce(&NonZeroU8) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O {
        let Self { config: _, sync_ctx } = self;
        device::IpDeviceStateContext::<I, C>::with_default_hop_limit(sync_ctx, device_id, cb)
    }

    fn with_default_hop_limit_mut<O, F: FnOnce(&mut NonZeroU8) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O {
        let Self { config: _, sync_ctx } = self;
        device::IpDeviceStateContext::<I, C>::with_default_hop_limit_mut(sync_ctx, device_id, cb)
    }

    fn join_link_multicast_group(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        multicast_addr: MulticastAddr<I::Addr>,
    ) {
        let Self { config: _, sync_ctx } = self;
        device::IpDeviceStateContext::<I, C>::join_link_multicast_group(
            sync_ctx,
            ctx,
            device_id,
            multicast_addr,
        )
    }

    fn leave_link_multicast_group(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        multicast_addr: MulticastAddr<I::Addr>,
    ) {
        let Self { config: _, sync_ctx } = self;
        device::IpDeviceStateContext::<I, C>::leave_link_multicast_group(
            sync_ctx,
            ctx,
            device_id,
            multicast_addr,
        )
    }
}

impl<'a, Config: Borrow<Ipv4DeviceConfiguration>, C: NonSyncContext> IgmpContext<C>
    for SyncCtxWithIpDeviceConfiguration<
        'a,
        Config,
        crate::lock_ordering::IpDeviceConfiguration<Ipv4>,
        C,
    >
{
    /// Calls the function with a mutable reference to the device's IGMP state
    /// and whether or not IGMP is enabled for the `device`.
    fn with_igmp_state_mut<
        O,
        F: FnOnce(GmpState<'_, Ipv4Addr, IgmpGroupState<C::Instant>>) -> O,
    >(
        &mut self,
        device: &Self::DeviceId,
        cb: F,
    ) -> O {
        let Self { config, sync_ctx } = self;
        let Ipv4DeviceConfiguration {
            ip_config: IpDeviceConfiguration { gmp_enabled, forwarding_enabled: _ },
        } = Borrow::borrow(&*config);

        crate::device::integration::with_ip_device_state(sync_ctx, device, |mut state| {
            // Note that changes to `ip_enabled` is not possible in this context
            // since IP enabled changes are only performed while the IP device
            // configuration lock is held exclusively. Since we have access to
            // the IP device configuration here (`config`), we know changes to
            // IP enabled are not possible.
            let ip_enabled = state.lock::<crate::lock_ordering::IpDeviceFlags<Ipv4>>().ip_enabled;
            let mut state = state.write_lock::<crate::lock_ordering::IpDeviceGmp<Ipv4>>();
            let enabled = ip_enabled && *gmp_enabled;
            cb(GmpState { enabled, groups: &mut state })
        })
    }

    fn get_ip_addr_subnet(&mut self, device: &Self::DeviceId) -> Option<AddrSubnet<Ipv4Addr>> {
        let Self { config: _, sync_ctx } = self;
        crate::ip::device::get_ipv4_addr_subnet(sync_ctx, device)
    }
}

impl<'a, Config, C: NonSyncContext>
    SendFrameContext<C, IgmpPacketMetadata<<Self as DeviceIdContext<AnyDevice>>::DeviceId>>
    for SyncCtxWithIpDeviceConfiguration<
        'a,
        Config,
        crate::lock_ordering::IpDeviceConfiguration<Ipv4>,
        C,
    >
{
    fn send_frame<S>(
        &mut self,
        ctx: &mut C,
        meta: IgmpPacketMetadata<<Self as DeviceIdContext<AnyDevice>>::DeviceId>,
        body: S,
    ) -> Result<(), S>
    where
        S: Serializer,
        S::Buffer: BufferMut,
    {
        let Self { config: _, sync_ctx } = self;
        IpDeviceSendContext::<Ipv4, _>::send_ip_frame(
            sync_ctx,
            ctx,
            &meta.device,
            meta.dst_ip.into_specified(),
            body,
        )
    }
}

impl<
        'a,
        Config: Borrow<Ipv6DeviceConfiguration>,
        C: NonSyncContext,
        L: LockBefore<crate::lock_ordering::IpState<Ipv6>>,
    > MldContext<C> for SyncCtxWithIpDeviceConfiguration<'a, Config, L, C>
{
    fn with_mld_state_mut<O, F: FnOnce(GmpState<'_, Ipv6Addr, MldGroupState<C::Instant>>) -> O>(
        &mut self,
        device: &Self::DeviceId,
        cb: F,
    ) -> O {
        let Self { config, sync_ctx } = self;
        let Ipv6DeviceConfiguration {
            dad_transmits: _,
            max_router_solicitations: _,
            slaac_config: _,
            ip_config: IpDeviceConfiguration { gmp_enabled, forwarding_enabled: _ },
        } = Borrow::borrow(&*config);

        crate::device::integration::with_ip_device_state(sync_ctx, device, |mut state| {
            let ip_enabled = state.lock::<crate::lock_ordering::IpDeviceFlags<Ipv6>>().ip_enabled;
            let mut state = state.write_lock::<crate::lock_ordering::IpDeviceGmp<Ipv6>>();
            let enabled = ip_enabled && *gmp_enabled;
            cb(GmpState { enabled, groups: &mut state })
        })
    }

    fn get_ipv6_link_local_addr(
        &mut self,
        device: &Self::DeviceId,
    ) -> Option<LinkLocalUnicastAddr<Ipv6Addr>> {
        let Self { config: _, sync_ctx } = self;
        device::IpDeviceStateContext::<Ipv6, C>::with_address_ids(
            sync_ctx,
            device,
            |mut addrs, sync_ctx| {
                addrs.find_map(|addr_id| {
                    device::IpDeviceAddressContext::<Ipv6, _>::with_ip_address_state(
                        sync_ctx,
                        device,
                        &addr_id,
                        |Ipv6AddressState {
                             flags: Ipv6AddressFlags { deprecated: _, assigned },
                             config: _,
                         }| {
                            if *assigned {
                                LinkLocalUnicastAddr::new(addr_id.addr_sub().addr())
                            } else {
                                None
                            }
                        },
                    )
                })
            },
        )
    }
}

impl<'a, Config, C: NonSyncContext, L: LockBefore<crate::lock_ordering::IpState<Ipv6>>>
    SendFrameContext<C, MldFrameMetadata<<Self as DeviceIdContext<AnyDevice>>::DeviceId>>
    for SyncCtxWithIpDeviceConfiguration<'a, Config, L, C>
{
    fn send_frame<S>(
        &mut self,
        ctx: &mut C,
        meta: MldFrameMetadata<<Self as DeviceIdContext<AnyDevice>>::DeviceId>,
        body: S,
    ) -> Result<(), S>
    where
        S: Serializer,
        S::Buffer: BufferMut,
    {
        let Self { config: _, sync_ctx } = self;
        IpDeviceSendContext::<Ipv6, _>::send_ip_frame(
            sync_ctx,
            ctx,
            &meta.device,
            meta.dst_ip.into_specified(),
            body,
        )
    }
}

impl<'a, Config, I: IpDeviceIpExt, C: NonSyncContext> NudIpHandler<I, C>
    for SyncCtxWithIpDeviceConfiguration<
        'a,
        Config,
        crate::lock_ordering::IpDeviceConfiguration<I>,
        C,
    >
where
    Locked<&'a SyncCtx<C>, crate::lock_ordering::IpDeviceConfiguration<I>>: NudIpHandler<I, C>,
{
    fn handle_neighbor_probe(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        neighbor: SpecifiedAddr<I::Addr>,
        link_addr: &[u8],
    ) {
        let Self { config: _, sync_ctx } = self;
        NudIpHandler::<I, C>::handle_neighbor_probe(sync_ctx, ctx, device_id, neighbor, link_addr)
    }

    fn handle_neighbor_confirmation(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        neighbor: SpecifiedAddr<I::Addr>,
        link_addr: &[u8],
        flags: ConfirmationFlags,
    ) {
        let Self { config: _, sync_ctx } = self;
        NudIpHandler::<I, C>::handle_neighbor_confirmation(
            sync_ctx, ctx, device_id, neighbor, link_addr, flags,
        )
    }

    fn flush_neighbor_table(&mut self, ctx: &mut C, device_id: &Self::DeviceId) {
        let Self { config: _, sync_ctx } = self;
        NudIpHandler::<I, C>::flush_neighbor_table(sync_ctx, ctx, device_id)
    }
}
