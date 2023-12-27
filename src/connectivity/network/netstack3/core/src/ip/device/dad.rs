// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Duplicate Address Detection.

use core::num::NonZeroU8;

use net_types::{
    ip::{Ipv6, Ipv6Addr},
    MulticastAddr, UnicastAddr, Witness as _,
};
use packet_formats::{icmp::ndp::NeighborSolicitation, utils::NonZeroDuration};
use tracing::debug;

use crate::{
    context::{EventContext, TimerContext, TimerHandler},
    device::{AnyDevice, DeviceIdContext},
    ip::device::{state::Ipv6DadState, IpAddressId as _, IpDeviceAddressIdContext},
};

/// A timer ID for duplicate address detection.
#[derive(Copy, Clone, Eq, PartialEq, Debug, Hash)]
pub(crate) struct DadTimerId<DeviceId> {
    pub(crate) device_id: DeviceId,
    pub(crate) addr: UnicastAddr<Ipv6Addr>,
}

impl<DeviceId> DadTimerId<DeviceId> {
    pub(super) fn device_id(&self) -> &DeviceId {
        let Self { device_id, addr: _ } = self;
        device_id
    }
}

pub(super) struct DadAddressStateRef<'a, CC> {
    /// A mutable reference to an address' state.
    ///
    /// `None` if the address is not recognized.
    pub(super) dad_state: &'a mut Ipv6DadState,
    /// The execution context available with the address's DAD state.
    pub(super) sync_ctx: &'a mut CC,
}

/// Holds references to state associated with duplicate address detection.
pub(super) struct DadStateRef<'a, CC> {
    pub(super) state: Option<DadAddressStateRef<'a, CC>>,
    /// The time between DAD message retransmissions.
    pub(super) retrans_timer: &'a NonZeroDuration,
    /// The maximum number of DAD messages to send.
    pub(super) max_dad_transmits: &'a Option<NonZeroU8>,
}

/// The execution context while performing DAD.
pub(super) trait DadAddressContext<BC>: IpDeviceAddressIdContext<Ipv6> {
    /// Calls the function with a mutable reference to the address's assigned
    /// flag.
    fn with_address_assigned<O, F: FnOnce(&mut bool) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        addr: &Self::AddressId,
        cb: F,
    ) -> O;

    /// Joins the multicast group on the device.
    fn join_multicast_group(
        &mut self,
        bindings_ctx: &mut BC,
        device_id: &Self::DeviceId,
        multicast_addr: MulticastAddr<Ipv6Addr>,
    );

    /// Leaves the multicast group on the device.
    fn leave_multicast_group(
        &mut self,
        bindings_ctx: &mut BC,
        device_id: &Self::DeviceId,
        multicast_addr: MulticastAddr<Ipv6Addr>,
    );
}

/// The execution context for DAD.
pub(super) trait DadContext<BC>:
    IpDeviceAddressIdContext<Ipv6> + DeviceIdContext<AnyDevice>
{
    type DadAddressCtx<'a>: DadAddressContext<
        BC,
        DeviceId = Self::DeviceId,
        AddressId = Self::AddressId,
    >;

    /// Returns the address ID for the given address value.
    fn get_address_id(
        &mut self,
        device_id: &Self::DeviceId,
        addr: UnicastAddr<Ipv6Addr>,
    ) -> Self::AddressId;

    /// Calls the function with the DAD state associated with the address.
    fn with_dad_state<O, F: FnOnce(DadStateRef<'_, Self::DadAddressCtx<'_>>) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        addr: &Self::AddressId,
        cb: F,
    ) -> O;

    /// Sends an NDP Neighbor Solicitation message for DAD to the local-link.
    ///
    /// The message will be sent with the unspecified (all-zeroes) source
    /// address.
    fn send_dad_packet(
        &mut self,
        bindings_ctx: &mut BC,
        device_id: &Self::DeviceId,
        dst_ip: MulticastAddr<Ipv6Addr>,
        message: NeighborSolicitation,
    ) -> Result<(), ()>;
}

#[derive(Debug, Eq, Hash, PartialEq)]
/// Events generated by duplicate address detection.
pub(super) enum DadEvent<DeviceId> {
    /// Duplicate address detection completed and the address is assigned.
    AddressAssigned {
        /// Device the address belongs to.
        device: DeviceId,
        /// The address that moved to the assigned state.
        addr: UnicastAddr<Ipv6Addr>,
    },
}

/// The bindings execution context for DAD.
pub(super) trait DadBindingsContext<DeviceId>:
    TimerContext<DadTimerId<DeviceId>> + EventContext<DadEvent<DeviceId>>
{
}
impl<DeviceId, BC: TimerContext<DadTimerId<DeviceId>> + EventContext<DadEvent<DeviceId>>>
    DadBindingsContext<DeviceId> for BC
{
}

/// An implementation for Duplicate Address Detection.
pub(crate) trait DadHandler<BC>:
    DeviceIdContext<AnyDevice> + IpDeviceAddressIdContext<Ipv6>
{
    /// Starts duplicate address detection.
    ///
    /// # Panics
    ///
    /// Panics if tentative state for the address is not found.
    fn start_duplicate_address_detection(
        &mut self,
        bindings_ctx: &mut BC,
        device_id: &Self::DeviceId,
        addr: &Self::AddressId,
    );

    /// Stops duplicate address detection.
    ///
    /// Does nothing if DAD is not being performed on the address.
    fn stop_duplicate_address_detection(
        &mut self,
        bindings_ctx: &mut BC,
        device_id: &Self::DeviceId,
        addr: &Self::AddressId,
    );
}

enum DoDadVariation {
    Start,
    Continue,
}

fn do_duplicate_address_detection<BC: DadBindingsContext<CC::DeviceId>, CC: DadContext<BC>>(
    core_ctx: &mut CC,
    bindings_ctx: &mut BC,
    device_id: &CC::DeviceId,
    addr: &CC::AddressId,
    variation: DoDadVariation,
) {
    let send_msg = core_ctx.with_dad_state(
        device_id,
        addr,
        |DadStateRef { state, retrans_timer, max_dad_transmits }| {
            let DadAddressStateRef { dad_state, sync_ctx } =
                state.unwrap_or_else(|| panic!("expected address to exist; addr: {addr:?}"));

            match variation {
                DoDadVariation::Start => {
                    // Mark the address as tentative/unassigned before joining
                    // the group so that the address is not used as the source
                    // for any outgoing MLD message.
                    *dad_state =
                        Ipv6DadState::Tentative { dad_transmits_remaining: *max_dad_transmits };
                    sync_ctx.with_address_assigned(device_id, addr, |assigned| *assigned = false);

                    // As per RFC 4861 section 5.6.2,
                    //
                    //   Before sending a Neighbor Solicitation, an interface MUST join
                    //   the all-nodes multicast address and the solicited-node
                    //   multicast address of the tentative address.
                    //
                    // Note that we join the all-nodes multicast address on interface
                    // enable.
                    sync_ctx.join_multicast_group(
                        bindings_ctx,
                        device_id,
                        addr.addr().to_solicited_node_address(),
                    );
                }
                DoDadVariation::Continue => {}
            }

            let remaining = match dad_state {
                Ipv6DadState::Tentative { dad_transmits_remaining } => dad_transmits_remaining,
                Ipv6DadState::Uninitialized | Ipv6DadState::Assigned => {
                    panic!("expected address to be tentative; addr={addr:?}")
                }
            };

            match remaining {
                None => {
                    *dad_state = Ipv6DadState::Assigned;
                    sync_ctx.with_address_assigned(device_id, addr, |assigned| *assigned = true);
                    bindings_ctx.on_event(DadEvent::AddressAssigned {
                        device: device_id.clone(),
                        addr: addr.addr_sub().addr(),
                    });
                    false
                }
                Some(non_zero_remaining) => {
                    *remaining = NonZeroU8::new(non_zero_remaining.get() - 1);

                    // Per RFC 4862 section 5.1,
                    //
                    //   DupAddrDetectTransmits ...
                    //      Autoconfiguration also assumes the presence of the variable
                    //      RetransTimer as defined in [RFC4861]. For autoconfiguration
                    //      purposes, RetransTimer specifies the delay between
                    //      consecutive Neighbor Solicitation transmissions performed
                    //      during Duplicate Address Detection (if
                    //      DupAddrDetectTransmits is greater than 1), as well as the
                    //      time a node waits after sending the last Neighbor
                    //      Solicitation before ending the Duplicate Address Detection
                    //      process.
                    assert_eq!(
                        bindings_ctx.schedule_timer(
                            retrans_timer.get(),
                            DadTimerId {
                                device_id: device_id.clone(),
                                addr: addr.addr_sub().addr()
                            }
                        ),
                        None,
                        "Unexpected DAD timer; addr={}, device_id={:?}",
                        addr.addr(),
                        device_id
                    );
                    debug!(
                        "performing DAD for {}; {} tries left",
                        addr.addr(),
                        remaining.map_or(0, NonZeroU8::get)
                    );
                    true
                }
            }
        },
    );

    if !send_msg {
        return;
    }

    // Do not include the source link-layer option when the NS
    // message as DAD messages are sent with the unspecified source
    // address which must not hold a source link-layer option.
    //
    // As per RFC 4861 section 4.3,
    //
    //   Possible options:
    //
    //      Source link-layer address
    //           The link-layer address for the sender. MUST NOT be
    //           included when the source IP address is the
    //           unspecified address. Otherwise, on link layers
    //           that have addresses this option MUST be included in
    //           multicast solicitations and SHOULD be included in
    //           unicast solicitations.
    //
    // TODO(https://fxbug.dev/85055): Either panic or guarantee that this error
    // can't happen statically.
    let dst_ip = addr.addr().to_solicited_node_address();
    let _: Result<(), _> = core_ctx.send_dad_packet(
        bindings_ctx,
        device_id,
        dst_ip,
        NeighborSolicitation::new(addr.addr().get()),
    );
}

impl<BC: DadBindingsContext<CC::DeviceId>, CC: DadContext<BC>> DadHandler<BC> for CC {
    fn start_duplicate_address_detection(
        &mut self,
        bindings_ctx: &mut BC,
        device_id: &Self::DeviceId,
        addr: &Self::AddressId,
    ) {
        do_duplicate_address_detection(self, bindings_ctx, device_id, addr, DoDadVariation::Start)
    }

    fn stop_duplicate_address_detection(
        &mut self,
        bindings_ctx: &mut BC,
        device_id: &Self::DeviceId,
        addr: &Self::AddressId,
    ) {
        self.with_dad_state(
            device_id,
            addr,
            |DadStateRef { state, retrans_timer: _, max_dad_transmits: _ }| {
                let DadAddressStateRef { dad_state, sync_ctx } =
                    state.unwrap_or_else(|| panic!("expected address to exist; addr: {addr:?}"));

                let leave_group = match dad_state {
                    Ipv6DadState::Assigned => true,
                    Ipv6DadState::Tentative { dad_transmits_remaining: _ } => {
                        assert_ne!(
                            bindings_ctx.cancel_timer(DadTimerId {
                                device_id: device_id.clone(),
                                addr: addr.addr_sub().addr(),
                            }),
                            None,
                        );

                        true
                    }
                    Ipv6DadState::Uninitialized => false,
                };

                // Undo the work we did when starting/performing DAD by putting
                // the address back into a tentative/unassigned state and
                // leaving the solicited node multicast group. We mark the
                // address as tentative/unassigned before leaving the group so
                // that the address is not used as the source for any outgoing
                // MLD message.
                *dad_state = Ipv6DadState::Uninitialized;
                sync_ctx.with_address_assigned(device_id, addr, |assigned| *assigned = false);
                if leave_group {
                    sync_ctx.leave_multicast_group(
                        bindings_ctx,
                        device_id,
                        addr.addr().to_solicited_node_address(),
                    );
                }
            },
        )
    }
}

impl<BC: DadBindingsContext<CC::DeviceId>, CC: DadContext<BC>>
    TimerHandler<BC, DadTimerId<CC::DeviceId>> for CC
{
    fn handle_timer(
        &mut self,
        bindings_ctx: &mut BC,
        DadTimerId { device_id, addr }: DadTimerId<CC::DeviceId>,
    ) {
        let addr_id = self.get_address_id(&device_id, addr);
        do_duplicate_address_detection(
            self,
            bindings_ctx,
            &device_id,
            &addr_id,
            DoDadVariation::Continue,
        )
    }
}

#[cfg(test)]
mod tests {
    use alloc::collections::hash_map::{Entry, HashMap};
    use core::time::Duration;

    use net_types::ip::{AddrSubnet, IpAddress as _};
    use packet::EmptyBuf;
    use packet_formats::icmp::ndp::Options;

    use super::*;
    use crate::{
        context::{
            testutil::{FakeCtx, FakeNonSyncCtx, FakeSyncCtx, FakeTimerCtxExt as _},
            InstantContext as _, SendFrameContext as _,
        },
        device::testutil::FakeDeviceId,
        ip::testutil::FakeIpDeviceIdCtx,
    };

    struct FakeDadAddressContext {
        addr: UnicastAddr<Ipv6Addr>,
        assigned: bool,
        groups: HashMap<MulticastAddr<Ipv6Addr>, usize>,
        ip_device_id_ctx: FakeIpDeviceIdCtx<FakeDeviceId>,
    }

    type FakeAddressCtxImpl = FakeSyncCtx<FakeDadAddressContext, (), FakeDeviceId>;

    impl AsRef<FakeIpDeviceIdCtx<FakeDeviceId>> for FakeDadAddressContext {
        fn as_ref(&self) -> &FakeIpDeviceIdCtx<FakeDeviceId> {
            &self.ip_device_id_ctx
        }
    }

    impl IpDeviceAddressIdContext<Ipv6> for FakeAddressCtxImpl {
        type AddressId = AddrSubnet<Ipv6Addr, UnicastAddr<Ipv6Addr>>;
    }

    impl DadAddressContext<FakeNonSyncCtxImpl> for FakeAddressCtxImpl {
        fn with_address_assigned<O, F: FnOnce(&mut bool) -> O>(
            &mut self,
            &FakeDeviceId: &Self::DeviceId,
            request_addr: &Self::AddressId,
            cb: F,
        ) -> O {
            let FakeDadAddressContext { addr, assigned, groups: _, ip_device_id_ctx: _ } =
                self.get_mut();
            assert_eq!(request_addr.addr(), *addr);
            cb(assigned)
        }

        fn join_multicast_group(
            &mut self,
            _bindings_ctx: &mut FakeNonSyncCtxImpl,
            &FakeDeviceId: &Self::DeviceId,
            multicast_addr: MulticastAddr<Ipv6Addr>,
        ) {
            let FakeDadAddressContext { addr: _, assigned: _, groups, ip_device_id_ctx: _ } =
                self.get_mut();
            *groups.entry(multicast_addr).or_default() += 1;
        }

        fn leave_multicast_group(
            &mut self,
            _bindings_ctx: &mut FakeNonSyncCtxImpl,
            &FakeDeviceId: &Self::DeviceId,
            multicast_addr: MulticastAddr<Ipv6Addr>,
        ) {
            let FakeDadAddressContext { addr: _, assigned: _, groups, ip_device_id_ctx: _ } =
                self.get_mut();
            match groups.entry(multicast_addr) {
                Entry::Vacant(_) => {}
                Entry::Occupied(mut e) => {
                    let v = e.get_mut();
                    const COUNT_BEFORE_REMOVE: usize = 1;
                    if *v == COUNT_BEFORE_REMOVE {
                        assert_eq!(e.remove(), COUNT_BEFORE_REMOVE);
                    } else {
                        *v -= 1
                    }
                }
            }
        }
    }

    struct FakeDadContext {
        state: Ipv6DadState,
        retrans_timer: NonZeroDuration,
        max_dad_transmits: Option<NonZeroU8>,
        address_ctx: FakeAddressCtxImpl,
    }

    #[derive(Debug)]
    struct DadMessageMeta {
        dst_ip: MulticastAddr<Ipv6Addr>,
        message: NeighborSolicitation,
    }

    impl AsRef<FakeIpDeviceIdCtx<FakeDeviceId>> for FakeDadContext {
        fn as_ref(&self) -> &FakeIpDeviceIdCtx<FakeDeviceId> {
            self.address_ctx.get_ref().as_ref()
        }
    }

    type FakeNonSyncCtxImpl = FakeNonSyncCtx<DadTimerId<FakeDeviceId>, DadEvent<FakeDeviceId>, ()>;

    type FakeCtxImpl = FakeSyncCtx<FakeDadContext, DadMessageMeta, FakeDeviceId>;

    fn get_address_id(addr: Ipv6Addr) -> AddrSubnet<Ipv6Addr, UnicastAddr<Ipv6Addr>> {
        AddrSubnet::new(addr, Ipv6Addr::BYTES * 8).unwrap()
    }

    impl IpDeviceAddressIdContext<Ipv6> for FakeCtxImpl {
        type AddressId = AddrSubnet<Ipv6Addr, UnicastAddr<Ipv6Addr>>;
    }

    impl DadContext<FakeNonSyncCtxImpl> for FakeCtxImpl {
        type DadAddressCtx<'a> = FakeAddressCtxImpl;

        fn get_address_id(
            &mut self,
            &FakeDeviceId: &Self::DeviceId,
            addr: UnicastAddr<Ipv6Addr>,
        ) -> Self::AddressId {
            get_address_id(addr.get())
        }

        fn with_dad_state<O, F: FnOnce(DadStateRef<'_, Self::DadAddressCtx<'_>>) -> O>(
            &mut self,
            &FakeDeviceId: &FakeDeviceId,
            request_addr: &Self::AddressId,
            cb: F,
        ) -> O {
            let FakeDadContext { state, retrans_timer, max_dad_transmits, address_ctx } =
                self.get_mut();
            cb(DadStateRef {
                state: (address_ctx.get_ref().addr == request_addr.addr())
                    .then(|| DadAddressStateRef { dad_state: state, sync_ctx: address_ctx }),
                retrans_timer,
                max_dad_transmits,
            })
        }

        fn send_dad_packet(
            &mut self,
            bindings_ctx: &mut FakeNonSyncCtxImpl,
            &FakeDeviceId: &FakeDeviceId,
            dst_ip: MulticastAddr<Ipv6Addr>,
            message: NeighborSolicitation,
        ) -> Result<(), ()> {
            self.send_frame(bindings_ctx, DadMessageMeta { dst_ip, message }, EmptyBuf)
                .map_err(|EmptyBuf| ())
        }
    }

    const RETRANS_TIMER: NonZeroDuration =
        unsafe { NonZeroDuration::new_unchecked(Duration::from_secs(1)) };
    const DAD_ADDRESS: UnicastAddr<Ipv6Addr> =
        unsafe { UnicastAddr::new_unchecked(Ipv6Addr::new([0xa, 0, 0, 0, 0, 0, 0, 1])) };
    const OTHER_ADDRESS: UnicastAddr<Ipv6Addr> =
        unsafe { UnicastAddr::new_unchecked(Ipv6Addr::new([0xa, 0, 0, 0, 0, 0, 0, 2])) };

    #[test]
    #[should_panic(expected = "expected address to exist")]
    fn panic_unknown_address_start() {
        let FakeCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtx::with_sync_ctx(FakeCtxImpl::with_state(FakeDadContext {
                state: Ipv6DadState::Tentative { dad_transmits_remaining: None },
                retrans_timer: RETRANS_TIMER,
                max_dad_transmits: None,
                address_ctx: FakeAddressCtxImpl::with_state(FakeDadAddressContext {
                    addr: DAD_ADDRESS,
                    assigned: false,
                    groups: HashMap::default(),
                    ip_device_id_ctx: Default::default(),
                }),
            }));
        DadHandler::start_duplicate_address_detection(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeDeviceId,
            &get_address_id(OTHER_ADDRESS.get()),
        );
    }

    #[test]
    #[should_panic(expected = "expected address to exist")]
    fn panic_unknown_address_handle_timer() {
        let FakeCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtx::with_sync_ctx(FakeCtxImpl::with_state(FakeDadContext {
                state: Ipv6DadState::Tentative { dad_transmits_remaining: None },
                retrans_timer: RETRANS_TIMER,
                max_dad_transmits: None,
                address_ctx: FakeAddressCtxImpl::with_state(FakeDadAddressContext {
                    addr: DAD_ADDRESS,
                    assigned: false,
                    groups: HashMap::default(),
                    ip_device_id_ctx: Default::default(),
                }),
            }));
        TimerHandler::handle_timer(
            &mut sync_ctx,
            &mut non_sync_ctx,
            DadTimerId { device_id: FakeDeviceId, addr: OTHER_ADDRESS },
        );
    }

    #[test]
    #[should_panic(expected = "expected address to be tentative")]
    fn panic_non_tentative_address_handle_timer() {
        let FakeCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtx::with_sync_ctx(FakeCtxImpl::with_state(FakeDadContext {
                state: Ipv6DadState::Assigned,
                retrans_timer: RETRANS_TIMER,
                max_dad_transmits: None,
                address_ctx: FakeAddressCtxImpl::with_state(FakeDadAddressContext {
                    addr: DAD_ADDRESS,
                    assigned: false,
                    groups: HashMap::default(),
                    ip_device_id_ctx: Default::default(),
                }),
            }));
        TimerHandler::handle_timer(
            &mut sync_ctx,
            &mut non_sync_ctx,
            DadTimerId { device_id: FakeDeviceId, addr: DAD_ADDRESS },
        );
    }

    #[test]
    fn dad_disabled() {
        let FakeCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtx::with_sync_ctx(FakeCtxImpl::with_state(FakeDadContext {
                state: Ipv6DadState::Tentative { dad_transmits_remaining: None },
                retrans_timer: RETRANS_TIMER,
                max_dad_transmits: None,
                address_ctx: FakeAddressCtxImpl::with_state(FakeDadAddressContext {
                    addr: DAD_ADDRESS,
                    assigned: false,
                    groups: HashMap::default(),
                    ip_device_id_ctx: Default::default(),
                }),
            }));
        DadHandler::start_duplicate_address_detection(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeDeviceId,
            &get_address_id(DAD_ADDRESS.get()),
        );
        let FakeDadContext { state, retrans_timer: _, max_dad_transmits: _, address_ctx } =
            sync_ctx.get_ref();
        assert_eq!(*state, Ipv6DadState::Assigned);
        let FakeDadAddressContext { addr: _, assigned, groups, ip_device_id_ctx: _ } =
            address_ctx.get_ref();
        assert!(*assigned);
        assert_eq!(groups, &HashMap::from([(DAD_ADDRESS.to_solicited_node_address(), 1)]));
        assert_eq!(
            non_sync_ctx.take_events(),
            &[DadEvent::AddressAssigned { device: FakeDeviceId, addr: DAD_ADDRESS }][..]
        );
    }

    const DAD_TIMER_ID: DadTimerId<FakeDeviceId> =
        DadTimerId { addr: DAD_ADDRESS, device_id: FakeDeviceId };

    fn check_dad(
        core_ctx: &FakeCtxImpl,
        bindings_ctx: &FakeNonSyncCtxImpl,
        frames_len: usize,
        dad_transmits_remaining: Option<NonZeroU8>,
        retrans_timer: NonZeroDuration,
    ) {
        let FakeDadContext { state, retrans_timer: _, max_dad_transmits: _, address_ctx } =
            core_ctx.get_ref();
        assert_eq!(*state, Ipv6DadState::Tentative { dad_transmits_remaining });
        let FakeDadAddressContext { addr: _, assigned, groups, ip_device_id_ctx: _ } =
            address_ctx.get_ref();
        assert!(!*assigned);
        assert_eq!(groups, &HashMap::from([(DAD_ADDRESS.to_solicited_node_address(), 1)]));
        let frames = core_ctx.frames();
        assert_eq!(frames.len(), frames_len, "frames = {:?}", frames);
        let (DadMessageMeta { dst_ip, message }, frame) =
            frames.last().expect("should have transmitted a frame");

        assert_eq!(*dst_ip, DAD_ADDRESS.to_solicited_node_address());
        assert_eq!(*message, NeighborSolicitation::new(DAD_ADDRESS.get()));

        let options = Options::parse(&frame[..]).expect("parse NDP options");
        assert_eq!(options.iter().count(), 0);
        bindings_ctx
            .timer_ctx()
            .assert_timers_installed([(DAD_TIMER_ID, bindings_ctx.now() + retrans_timer.get())]);
    }

    #[test]
    fn perform_dad() {
        const DAD_TRANSMITS_REQUIRED: u8 = 5;
        const RETRANS_TIMER: NonZeroDuration =
            unsafe { NonZeroDuration::new_unchecked(Duration::from_secs(1)) };

        let FakeCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtx::with_sync_ctx(FakeCtxImpl::with_state(FakeDadContext {
                state: Ipv6DadState::Tentative {
                    dad_transmits_remaining: NonZeroU8::new(DAD_TRANSMITS_REQUIRED),
                },
                retrans_timer: RETRANS_TIMER,
                max_dad_transmits: NonZeroU8::new(DAD_TRANSMITS_REQUIRED),
                address_ctx: FakeAddressCtxImpl::with_state(FakeDadAddressContext {
                    addr: DAD_ADDRESS,
                    assigned: false,
                    groups: HashMap::default(),
                    ip_device_id_ctx: Default::default(),
                }),
            }));
        DadHandler::start_duplicate_address_detection(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeDeviceId,
            &get_address_id(DAD_ADDRESS.get()),
        );

        for count in 0..=(DAD_TRANSMITS_REQUIRED - 1) {
            check_dad(
                &sync_ctx,
                &non_sync_ctx,
                usize::from(count + 1),
                NonZeroU8::new(DAD_TRANSMITS_REQUIRED - count - 1),
                RETRANS_TIMER,
            );
            assert_eq!(
                non_sync_ctx.trigger_next_timer(&mut sync_ctx, TimerHandler::handle_timer),
                Some(DAD_TIMER_ID)
            );
        }
        let FakeDadContext { state, retrans_timer: _, max_dad_transmits: _, address_ctx } =
            sync_ctx.get_ref();
        assert_eq!(*state, Ipv6DadState::Assigned);
        let FakeDadAddressContext { addr: _, assigned, groups, ip_device_id_ctx: _ } =
            address_ctx.get_ref();
        assert!(*assigned);
        assert_eq!(groups, &HashMap::from([(DAD_ADDRESS.to_solicited_node_address(), 1)]));
        assert_eq!(
            non_sync_ctx.take_events(),
            &[DadEvent::AddressAssigned { device: FakeDeviceId, addr: DAD_ADDRESS }][..]
        );
    }

    #[test]
    fn stop_dad() {
        const DAD_TRANSMITS_REQUIRED: u8 = 2;
        const RETRANS_TIMER: NonZeroDuration =
            unsafe { NonZeroDuration::new_unchecked(Duration::from_secs(2)) };

        let FakeCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtx::with_sync_ctx(FakeCtxImpl::with_state(FakeDadContext {
                state: Ipv6DadState::Tentative {
                    dad_transmits_remaining: NonZeroU8::new(DAD_TRANSMITS_REQUIRED),
                },
                retrans_timer: RETRANS_TIMER,
                max_dad_transmits: NonZeroU8::new(DAD_TRANSMITS_REQUIRED),
                address_ctx: FakeAddressCtxImpl::with_state(FakeDadAddressContext {
                    addr: DAD_ADDRESS,
                    assigned: false,
                    groups: HashMap::default(),
                    ip_device_id_ctx: Default::default(),
                }),
            }));
        DadHandler::start_duplicate_address_detection(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeDeviceId,
            &get_address_id(DAD_ADDRESS.get()),
        );
        check_dad(
            &sync_ctx,
            &non_sync_ctx,
            1,
            NonZeroU8::new(DAD_TRANSMITS_REQUIRED - 1),
            RETRANS_TIMER,
        );

        DadHandler::stop_duplicate_address_detection(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeDeviceId,
            &get_address_id(DAD_ADDRESS.get()),
        );
        non_sync_ctx.timer_ctx().assert_no_timers_installed();
        let FakeDadContext { state, retrans_timer: _, max_dad_transmits: _, address_ctx } =
            sync_ctx.get_ref();
        assert_eq!(*state, Ipv6DadState::Uninitialized);
        let FakeDadAddressContext { addr: _, assigned, groups, ip_device_id_ctx: _ } =
            address_ctx.get_ref();
        assert!(!*assigned);
        assert_eq!(groups, &HashMap::new());
    }
}
