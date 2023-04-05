// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Duplicate Address Detection.

use core::num::NonZeroU8;

use log::debug;
use net_types::{ip::Ipv6Addr, MulticastAddr, UnicastAddr, Witness as _};
use packet_formats::{icmp::ndp::NeighborSolicitation, utils::NonZeroDuration};

use crate::{
    context::{EventContext, TimerContext, TimerHandler},
    device::AnyDevice,
    ip::{device::state::Ipv6DadState, DeviceIdContext},
};

/// A timer ID for duplicate address detection.
#[derive(Copy, Clone, Eq, PartialEq, Debug, Hash)]
pub(crate) struct DadTimerId<DeviceId> {
    pub(crate) device_id: DeviceId,
    pub(crate) addr: UnicastAddr<Ipv6Addr>,
}

/// Holds references to state associated with duplicate address detection.
pub(super) struct DadStateRef<'a> {
    /// A mutable reference to an address' state.
    ///
    /// `None` if the address is not recognized.
    pub(super) address_state: Option<&'a mut Ipv6DadState>,
    /// The time between DAD message retransmissions.
    pub(super) retrans_timer: &'a NonZeroDuration,
    /// The maximum number of DAD messages to send.
    pub(super) max_dad_transmits: &'a Option<NonZeroU8>,
}

/// The IP device context provided to DAD.
pub(super) trait Ipv6DeviceDadContext<C>: DeviceIdContext<AnyDevice> {
    /// Calls the function with the DAD state associated with the address.
    fn with_dad_state<O, F: FnOnce(DadStateRef<'_>) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        addr: UnicastAddr<Ipv6Addr>,
        cb: F,
    ) -> O;
}

/// The IP layer context provided to DAD.
pub(super) trait Ipv6LayerDadContext<C>: DeviceIdContext<AnyDevice> {
    /// Sends an NDP Neighbor Solicitation message for DAD to the local-link.
    ///
    /// The message will be sent with the unspecified (all-zeroes) source
    /// address.
    fn send_dad_packet(
        &mut self,
        ctx: &mut C,
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

/// The non-synchronized execution context for DAD.
pub(super) trait DadNonSyncContext<DeviceId>:
    TimerContext<DadTimerId<DeviceId>> + EventContext<DadEvent<DeviceId>>
{
}
impl<DeviceId, C: TimerContext<DadTimerId<DeviceId>> + EventContext<DadEvent<DeviceId>>>
    DadNonSyncContext<DeviceId> for C
{
}

/// The execution context for DAD.
pub(super) trait DadContext<C: DadNonSyncContext<Self::DeviceId>>:
    Ipv6DeviceDadContext<C> + Ipv6LayerDadContext<C>
{
}

impl<C: DadNonSyncContext<SC::DeviceId>, SC: Ipv6DeviceDadContext<C> + Ipv6LayerDadContext<C>>
    DadContext<C> for SC
{
}

/// An implementation for Duplicate Address Detection.
pub(crate) trait DadHandler<C>: DeviceIdContext<AnyDevice> {
    /// Starts duplicate address detection.
    ///
    /// # Panics
    ///
    /// Panics if tentative state for the address is not found.
    fn start_duplicate_address_detection(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        addr: UnicastAddr<Ipv6Addr>,
    );

    /// Stops duplicate address detection.
    ///
    /// Does nothing if DAD is not being performed on the address.
    fn stop_duplicate_address_detection(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        addr: UnicastAddr<Ipv6Addr>,
    );
}

enum DoDadVariation {
    Start,
    Continue,
}

fn do_duplicate_address_detection<C: DadNonSyncContext<SC::DeviceId>, SC: DadContext<C>>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    device_id: &SC::DeviceId,
    addr: UnicastAddr<Ipv6Addr>,
    variation: DoDadVariation,
) {
    let send_msg = sync_ctx.with_dad_state(
        device_id,
        addr,
        |DadStateRef { address_state, retrans_timer, max_dad_transmits }| {
            let address_state =
                address_state.unwrap_or_else(|| panic!("expected address to exist; addr: {addr}"));

            match variation {
                DoDadVariation::Start => {
                    *address_state =
                        Ipv6DadState::Tentative { dad_transmits_remaining: *max_dad_transmits };
                }
                DoDadVariation::Continue => {}
            }

            let remaining = match address_state {
                Ipv6DadState::Tentative { dad_transmits_remaining } => dad_transmits_remaining,
                Ipv6DadState::Uninitialized | Ipv6DadState::Assigned => {
                    panic!("expected address to be tentative; addr={}", addr)
                }
            };

            match remaining {
                None => {
                    *address_state = Ipv6DadState::Assigned;
                    ctx.on_event(DadEvent::AddressAssigned { device: device_id.clone(), addr });
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
                        ctx.schedule_timer(
                            retrans_timer.get(),
                            DadTimerId { device_id: device_id.clone(), addr }
                        ),
                        None,
                        "Unexpected DAD timer; addr={}, device_id={}",
                        addr,
                        device_id
                    );
                    debug!(
                        "performing DAD for {}; {} tries left",
                        addr.get(),
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
    let dst_ip = addr.to_solicited_node_address();
    let _: Result<(), _> =
        sync_ctx.send_dad_packet(ctx, device_id, dst_ip, NeighborSolicitation::new(addr.get()));
}

impl<C: DadNonSyncContext<SC::DeviceId>, SC: DadContext<C>> DadHandler<C> for SC {
    fn start_duplicate_address_detection(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        addr: UnicastAddr<Ipv6Addr>,
    ) {
        do_duplicate_address_detection(self, ctx, device_id, addr, DoDadVariation::Start)
    }

    fn stop_duplicate_address_detection(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        addr: UnicastAddr<Ipv6Addr>,
    ) {
        let _: Option<C::Instant> =
            ctx.cancel_timer(DadTimerId { device_id: device_id.clone(), addr });
    }
}

impl<C: DadNonSyncContext<SC::DeviceId>, SC: DadContext<C>>
    TimerHandler<C, DadTimerId<SC::DeviceId>> for SC
{
    fn handle_timer(
        &mut self,
        ctx: &mut C,
        DadTimerId { device_id, addr }: DadTimerId<SC::DeviceId>,
    ) {
        do_duplicate_address_detection(self, ctx, &device_id, addr, DoDadVariation::Continue)
    }
}

#[cfg(test)]
mod tests {
    use core::time::Duration;

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

    struct FakeDadContext {
        addr: UnicastAddr<Ipv6Addr>,
        state: Ipv6DadState,
        retrans_timer: NonZeroDuration,
        max_dad_transmits: Option<NonZeroU8>,
        ip_device_id_ctx: FakeIpDeviceIdCtx<FakeDeviceId>,
    }

    #[derive(Debug)]
    struct DadMessageMeta {
        dst_ip: MulticastAddr<Ipv6Addr>,
        message: NeighborSolicitation,
    }

    impl AsRef<FakeIpDeviceIdCtx<FakeDeviceId>> for FakeDadContext {
        fn as_ref(&self) -> &FakeIpDeviceIdCtx<FakeDeviceId> {
            &self.ip_device_id_ctx
        }
    }

    type FakeNonSyncCtxImpl = FakeNonSyncCtx<DadTimerId<FakeDeviceId>, DadEvent<FakeDeviceId>, ()>;

    type FakeCtxImpl = FakeSyncCtx<FakeDadContext, DadMessageMeta, FakeDeviceId>;

    impl Ipv6DeviceDadContext<FakeNonSyncCtxImpl> for FakeCtxImpl {
        fn with_dad_state<O, F: FnOnce(DadStateRef<'_>) -> O>(
            &mut self,
            &FakeDeviceId: &FakeDeviceId,
            request_addr: UnicastAddr<Ipv6Addr>,
            cb: F,
        ) -> O {
            let FakeDadContext {
                addr,
                state,
                retrans_timer,
                max_dad_transmits,
                ip_device_id_ctx: _,
            } = self.get_mut();
            cb(DadStateRef {
                address_state: (*addr == request_addr).then(|| state),
                retrans_timer,
                max_dad_transmits,
            })
        }
    }

    impl Ipv6LayerDadContext<FakeNonSyncCtxImpl> for FakeCtxImpl {
        fn send_dad_packet(
            &mut self,
            ctx: &mut FakeNonSyncCtxImpl,
            &FakeDeviceId: &FakeDeviceId,
            dst_ip: MulticastAddr<Ipv6Addr>,
            message: NeighborSolicitation,
        ) -> Result<(), ()> {
            self.send_frame(ctx, DadMessageMeta { dst_ip, message }, EmptyBuf)
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
                addr: DAD_ADDRESS,
                state: Ipv6DadState::Tentative { dad_transmits_remaining: None },
                retrans_timer: RETRANS_TIMER,
                max_dad_transmits: None,
                ip_device_id_ctx: Default::default(),
            }));
        DadHandler::start_duplicate_address_detection(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeDeviceId,
            OTHER_ADDRESS,
        );
    }

    #[test]
    #[should_panic(expected = "expected address to exist")]
    fn panic_unknown_address_handle_timer() {
        let FakeCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtx::with_sync_ctx(FakeCtxImpl::with_state(FakeDadContext {
                addr: DAD_ADDRESS,
                state: Ipv6DadState::Tentative { dad_transmits_remaining: None },
                retrans_timer: RETRANS_TIMER,
                max_dad_transmits: None,
                ip_device_id_ctx: Default::default(),
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
                addr: DAD_ADDRESS,
                state: Ipv6DadState::Assigned,
                retrans_timer: RETRANS_TIMER,
                max_dad_transmits: None,
                ip_device_id_ctx: Default::default(),
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
                addr: DAD_ADDRESS,
                state: Ipv6DadState::Tentative { dad_transmits_remaining: None },
                retrans_timer: RETRANS_TIMER,
                max_dad_transmits: None,
                ip_device_id_ctx: Default::default(),
            }));
        DadHandler::start_duplicate_address_detection(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeDeviceId,
            DAD_ADDRESS,
        );
        let FakeDadContext {
            addr: _,
            state,
            retrans_timer: _,
            max_dad_transmits: _,
            ip_device_id_ctx: _,
        } = sync_ctx.get_ref();
        assert_eq!(*state, Ipv6DadState::Assigned);
        assert_eq!(
            non_sync_ctx.take_events(),
            &[DadEvent::AddressAssigned { device: FakeDeviceId, addr: DAD_ADDRESS }][..]
        );
    }

    const DAD_TIMER_ID: DadTimerId<FakeDeviceId> =
        DadTimerId { addr: DAD_ADDRESS, device_id: FakeDeviceId };

    fn check_dad(
        sync_ctx: &FakeCtxImpl,
        non_sync_ctx: &FakeNonSyncCtxImpl,
        frames_len: usize,
        dad_transmits_remaining: Option<NonZeroU8>,
        retrans_timer: NonZeroDuration,
    ) {
        let FakeDadContext {
            addr: _,
            state,
            retrans_timer: _,
            max_dad_transmits: _,
            ip_device_id_ctx: _,
        } = sync_ctx.get_ref();
        assert_eq!(*state, Ipv6DadState::Tentative { dad_transmits_remaining });
        let frames = sync_ctx.frames();
        assert_eq!(frames.len(), frames_len, "frames = {:?}", frames);
        let (DadMessageMeta { dst_ip, message }, frame) =
            frames.last().expect("should have transmitted a frame");

        assert_eq!(*dst_ip, DAD_ADDRESS.to_solicited_node_address());
        assert_eq!(*message, NeighborSolicitation::new(DAD_ADDRESS.get()));

        let options = Options::parse(&frame[..]).expect("parse NDP options");
        assert_eq!(options.iter().count(), 0);
        non_sync_ctx
            .timer_ctx()
            .assert_timers_installed([(DAD_TIMER_ID, non_sync_ctx.now() + retrans_timer.get())]);
    }

    #[test]
    fn perform_dad() {
        const DAD_TRANSMITS_REQUIRED: u8 = 5;
        const RETRANS_TIMER: NonZeroDuration =
            unsafe { NonZeroDuration::new_unchecked(Duration::from_secs(1)) };

        let FakeCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtx::with_sync_ctx(FakeCtxImpl::with_state(FakeDadContext {
                addr: DAD_ADDRESS,
                state: Ipv6DadState::Tentative {
                    dad_transmits_remaining: NonZeroU8::new(DAD_TRANSMITS_REQUIRED),
                },
                retrans_timer: RETRANS_TIMER,
                max_dad_transmits: NonZeroU8::new(DAD_TRANSMITS_REQUIRED),
                ip_device_id_ctx: Default::default(),
            }));
        DadHandler::start_duplicate_address_detection(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeDeviceId,
            DAD_ADDRESS,
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
        let FakeDadContext {
            addr: _,
            state,
            retrans_timer: _,
            max_dad_transmits: _,
            ip_device_id_ctx: _,
        } = sync_ctx.get_ref();
        assert_eq!(*state, Ipv6DadState::Assigned);
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
                addr: DAD_ADDRESS,
                state: Ipv6DadState::Tentative {
                    dad_transmits_remaining: NonZeroU8::new(DAD_TRANSMITS_REQUIRED),
                },
                retrans_timer: RETRANS_TIMER,
                max_dad_transmits: NonZeroU8::new(DAD_TRANSMITS_REQUIRED),
                ip_device_id_ctx: Default::default(),
            }));
        DadHandler::start_duplicate_address_detection(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeDeviceId,
            DAD_ADDRESS,
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
            DAD_ADDRESS,
        );
        non_sync_ctx.timer_ctx().assert_no_timers_installed();
    }
}
