// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Neighbor unreachability detection.

use alloc::{
    collections::{
        hash_map::{Entry, HashMap},
        VecDeque,
    },
    vec::Vec,
};
use core::{fmt::Debug, hash::Hash, marker::PhantomData, num::NonZeroU8};

use assert_matches::assert_matches;
use derivative::Derivative;
use net_types::{ip::Ip, SpecifiedAddr};
use packet::{Buf, BufferMut, Serializer};
use packet_formats::utils::NonZeroDuration;

use crate::{
    context::{TimerContext, TimerHandler},
    device::{
        link::{LinkAddress, LinkDevice},
        AnyDevice, DeviceIdContext,
    },
};

/// The maximum number of multicast solicitations as defined in [RFC 4861
/// section 10].
///
/// [RFC 4861 section 10]: https://tools.ietf.org/html/rfc4861#section-10
const MAX_MULTICAST_SOLICIT: u8 = 3;

const MAX_PENDING_FRAMES: usize = 10;

/// The type of message with a dynamic neighbor update.
#[derive(Copy, Clone)]
pub(crate) enum DynamicNeighborUpdateSource {
    /// Indicates an update from a neighbor probe message.
    ///
    /// E.g. NDP Neighbor Solicitation.
    Probe,

    /// Indicates an update from a neighbor confirmation message.
    ///
    /// E.g. NDP Neighbor Advertisement.
    Confirmation,
}

#[derive(Debug)]
#[cfg_attr(test, derive(PartialEq, Eq))]
enum NeighborState<LinkAddr> {
    Dynamic(DynamicNeighborState<LinkAddr>),
    Static(LinkAddr),
}

#[derive(Debug)]
#[cfg_attr(test, derive(PartialEq, Eq))]
pub(crate) enum DynamicNeighborState<LinkAddr> {
    Incomplete { transmit_counter: Option<NonZeroU8>, pending_frames: VecDeque<Buf<Vec<u8>>> },
    Complete { link_address: LinkAddr },
}

impl<LinkAddr> DynamicNeighborState<LinkAddr> {
    fn new_incomplete_with_pending_frame(remaining_tries: u8, frame: Buf<Vec<u8>>) -> Self {
        DynamicNeighborState::Incomplete {
            transmit_counter: NonZeroU8::new(remaining_tries),
            pending_frames: [frame].into(),
        }
    }
}

#[cfg(test)]
pub(crate) mod testutil {
    use super::*;

    pub(crate) fn assert_dynamic_neighbor_with_addr<
        I: Ip,
        D: LinkDevice,
        C: NonSyncNudContext<I, D, SC::DeviceId>,
        SC: NudContext<I, D, C>,
    >(
        sync_ctx: &mut SC,
        device_id: SC::DeviceId,
        neighbor: SpecifiedAddr<I::Addr>,
        expected_link_addr: D::Address,
    ) {
        sync_ctx.with_nud_state_mut(&device_id, |NudState { neighbors }| {
            assert_matches!(
                neighbors.get(&neighbor),
                Some(
                    NeighborState::Dynamic(DynamicNeighborState::Complete { link_address })
                ) => {
                    assert_eq!(link_address, &expected_link_addr)
                }
            )
        })
    }

    pub(crate) fn assert_neighbor_unknown<
        I: Ip,
        D: LinkDevice,
        C: NonSyncNudContext<I, D, SC::DeviceId>,
        SC: NudContext<I, D, C>,
    >(
        sync_ctx: &mut SC,
        device_id: SC::DeviceId,
        neighbor: SpecifiedAddr<I::Addr>,
    ) {
        sync_ctx.with_nud_state_mut(&device_id, |NudState { neighbors }| {
            assert_matches!(neighbors.get(&neighbor), None)
        })
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub(crate) struct NudTimerId<I: Ip, D: LinkDevice, DeviceId> {
    device_id: DeviceId,
    lookup_addr: SpecifiedAddr<I::Addr>,
    _marker: PhantomData<D>,
}

#[derive(Derivative)]
#[derivative(Default(bound = ""))]
#[cfg_attr(test, derive(Debug, PartialEq, Eq))]
pub(crate) struct NudState<I: Ip, LinkAddr> {
    // TODO(https://fxbug.dev/126138): Key neighbors by `UnicastAddr`.
    neighbors: HashMap<SpecifiedAddr<I::Addr>, NeighborState<LinkAddr>>,
}

/// The non-synchronized context for NUD.
pub(crate) trait NonSyncNudContext<I: Ip, D: LinkDevice, DeviceId>:
    TimerContext<NudTimerId<I, D, DeviceId>>
{
}

impl<I: Ip, D: LinkDevice, DeviceId, C: TimerContext<NudTimerId<I, D, DeviceId>>>
    NonSyncNudContext<I, D, DeviceId> for C
{
}

/// The execution context for NUD for a link device.
pub(crate) trait NudContext<I: Ip, D: LinkDevice, C: NonSyncNudContext<I, D, Self::DeviceId>>:
    DeviceIdContext<D>
{
    /// Returns the amount of time between neighbor probe/solicitation messages.
    fn retrans_timer(&mut self, device_id: &Self::DeviceId) -> NonZeroDuration;

    /// Calls the function with a mutable reference to the NUD state.
    fn with_nud_state_mut<O, F: FnOnce(&mut NudState<I, D::Address>) -> O>(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O;

    /// Sends a neighbor probe/solicitation message.
    fn send_neighbor_solicitation(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        lookup_addr: SpecifiedAddr<I::Addr>,
    );
}

/// The execution context for NUD for a link device, with a buffer.
pub(crate) trait BufferNudContext<
    B: BufferMut,
    I: Ip,
    D: LinkDevice,
    C: NonSyncNudContext<I, D, Self::DeviceId>,
>: NudContext<I, D, C>
{
    type BufferSenderCtx<'a>: BufferNudSenderContext<B, I, D, C, DeviceId = Self::DeviceId>;

    /// Calls the function with a mutable reference to the NUD state and the
    /// synchronized context.
    fn with_nud_state_mut_and_buf_ctx<
        O,
        F: FnOnce(&mut NudState<I, D::Address>, &mut Self::BufferSenderCtx<'_>) -> O,
    >(
        &mut self,
        device_id: &Self::DeviceId,
        cb: F,
    ) -> O;
}

/// The execution context for NUD for a link device that allows sending IP
/// packets to specific neighbors.
pub(crate) trait BufferNudSenderContext<
    B: BufferMut,
    I: Ip,
    D: LinkDevice,
    C: NonSyncNudContext<I, D, Self::DeviceId>,
>: DeviceIdContext<D>
{
    /// Send an IP frame to the neighbor with the specified link address.
    fn send_ip_packet_to_neighbor_link_addr<S: Serializer<Buffer = B>>(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        neighbor_link_addr: D::Address,
        body: S,
    ) -> Result<(), S>;
}

/// An implementation of NUD for the IP layer.
pub(crate) trait NudIpHandler<I: Ip, C>: DeviceIdContext<AnyDevice> {
    /// Handles an incoming neighbor probe message.
    ///
    /// For IPv6, this can be an NDP Neighbor Solicitation or an NDP Router
    /// Advertisement message.
    fn handle_neighbor_probe(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        neighbor: SpecifiedAddr<I::Addr>,
        link_addr: &[u8],
    );

    /// Handles an incoming neighbor confirmation message.
    ///
    /// For IPv6, this can be an NDP Neighbor Advertisement.
    fn handle_neighbor_confirmation(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        neighbor: SpecifiedAddr<I::Addr>,
        link_addr: &[u8],
    );

    /// Clears the neighbor table.
    fn flush_neighbor_table(&mut self, ctx: &mut C, device_id: &Self::DeviceId);
}

/// Specifies the link-layer address of a neighbor.
#[derive(Debug, Clone, Copy, Eq, PartialEq)]
pub enum NeighborLinkAddr<A: LinkAddress> {
    /// The destination is a known neighbor with the given link-layer address.
    Resolved(A),
    /// The destination is not a known neighbor.
    PendingNeighborResolution,
}

/// An implementation of NUD for a link device.
pub(crate) trait NudHandler<I: Ip, D: LinkDevice, C>: DeviceIdContext<D> {
    /// Sets a dynamic neighbor's entry state to the specified values in
    /// response to the source packet.
    // TODO(https://fxbug.dev/126138): Require that neighbor is a `UnicastAddr`.
    fn set_dynamic_neighbor(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        neighbor: SpecifiedAddr<I::Addr>,
        link_addr: D::Address,
        source: DynamicNeighborUpdateSource,
    );

    /// Sets a static neighbor entry for the neighbor.
    ///
    /// If no entry exists, a new one may be created. If an entry already
    /// exists, it will be updated with the provided link address and set
    /// to be a static entry.
    ///
    /// Dynamic updates for the neighbor will be ignored for static entries.
    // TODO(https://fxbug.dev/126138): Require that neighbor is a `UnicastAddr`.
    fn set_static_neighbor(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        neighbor: SpecifiedAddr<I::Addr>,
        link_addr: D::Address,
    );

    /// Clears the neighbor table.
    fn flush(&mut self, ctx: &mut C, device_id: &Self::DeviceId);

    /// Resolve the link-address of a device's neighbor.
    // TODO(https://fxbug.dev/126138): Require that dst is a `UnicastAddr`.
    fn resolve_link_addr(
        &mut self,
        device: &Self::DeviceId,
        dst: &SpecifiedAddr<I::Addr>,
    ) -> NeighborLinkAddr<D::Address>;
}

/// An implementation of NUD for a link device, with a buffer.
pub(crate) trait BufferNudHandler<B: BufferMut, I: Ip, D: LinkDevice, C>:
    DeviceIdContext<D>
{
    /// Send an IP packet to the neighbor.
    ///
    /// If the neighbor's link address is not known, link address resolution
    /// is performed.
    fn send_ip_packet_to_neighbor<S: Serializer<Buffer = B>>(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        neighbor: SpecifiedAddr<I::Addr>,
        body: S,
    ) -> Result<(), S>;
}

impl<I: Ip, D: LinkDevice, C: NonSyncNudContext<I, D, SC::DeviceId>, SC: NudContext<I, D, C>>
    TimerHandler<C, NudTimerId<I, D, SC::DeviceId>> for SC
{
    fn handle_timer(
        &mut self,
        ctx: &mut C,
        NudTimerId { device_id, lookup_addr, _marker }: NudTimerId<I, D, SC::DeviceId>,
    ) {
        let do_solicit = self.with_nud_state_mut(&device_id, |NudState { neighbors }| {
            let transmit_counter = match neighbors
                .get_mut(&lookup_addr)
                .expect("timer fired for invalid entry")
            {
                NeighborState::Dynamic(DynamicNeighborState::Incomplete {
                    transmit_counter,
                    pending_frames: _,
                }) => transmit_counter,
                NeighborState::Static(_)
                | NeighborState::Dynamic(DynamicNeighborState::Complete { link_address: _ }) => {
                    unreachable!("timer should only fire for incomplete entry")
                }
            };

            match transmit_counter {
                Some(c) => {
                    *transmit_counter = NonZeroU8::new(c.get() - 1);
                    true
                }
                None => {
                    // Failed to complete neighbor resolution and no more probes to
                    // send.
                    assert_matches!(
                        neighbors.remove(&lookup_addr),
                        Some(e) => {
                            let _: NeighborState<_> = e;
                        }
                    );
                    false
                }
            }
        });

        if do_solicit {
            solicit_neighbor(self, ctx, &device_id, lookup_addr)
        }
    }
}

fn solicit_neighbor<
    I: Ip,
    D: LinkDevice,
    C: NonSyncNudContext<I, D, SC::DeviceId>,
    SC: NudContext<I, D, C>,
>(
    sync_ctx: &mut SC,
    ctx: &mut C,
    device_id: &SC::DeviceId,
    lookup_addr: SpecifiedAddr<I::Addr>,
) {
    sync_ctx.send_neighbor_solicitation(ctx, device_id, lookup_addr);

    let retrans_timer = sync_ctx.retrans_timer(device_id);
    assert_eq!(
        ctx.schedule_timer(
            retrans_timer.get(),
            NudTimerId { device_id: device_id.clone(), lookup_addr, _marker: PhantomData },
        ),
        None
    );
}

impl<
        I: Ip,
        D: LinkDevice,
        C: NonSyncNudContext<I, D, SC::DeviceId>,
        SC: BufferNudContext<Buf<Vec<u8>>, I, D, C>,
    > NudHandler<I, D, C> for SC
{
    fn set_dynamic_neighbor(
        &mut self,
        ctx: &mut C,
        device_id: &SC::DeviceId,
        neighbor: SpecifiedAddr<I::Addr>,
        link_address: D::Address,
        source: DynamicNeighborUpdateSource,
    ) {
        self.with_nud_state_mut_and_buf_ctx(device_id, |NudState { neighbors }, sync_ctx| {
            match neighbors.entry(neighbor) {
                Entry::Vacant(e) => match source {
                    DynamicNeighborUpdateSource::Probe => {
                        let _: &mut NeighborState<_> =
                            e.insert(NeighborState::Dynamic(DynamicNeighborState::Complete {
                                link_address,
                            }));
                    }
                    DynamicNeighborUpdateSource::Confirmation => {}
                },
                Entry::Occupied(e) => match e.into_mut() {
                    NeighborState::Dynamic(e) => {
                        match core::mem::replace(e, DynamicNeighborState::Complete { link_address })
                        {
                            DynamicNeighborState::Incomplete {
                                transmit_counter: _,
                                pending_frames,
                            } => {
                                assert_ne!(
                                    ctx.cancel_timer(NudTimerId {
                                        device_id: device_id.clone(),
                                        lookup_addr: neighbor,
                                        _marker: PhantomData,
                                    }),
                                    None,
                                    "previously incomplete entry for {} should have had a timer",
                                    neighbor
                                );

                                // Send out pending packets while holding the NUD lock to prevent a
                                // potential ordering violation.
                                //
                                // If we drop the NUD lock before sending out these queued packets,
                                // another thread could take the NUD lock, observe that neighbor
                                // resolution is complete, and send a packet *before* these pending
                                // packets are sent out, resulting in out-of-order transmission to
                                // the device.
                                for body in pending_frames {
                                    // Ignore any errors on sending the IP packet, because a failure
                                    // at this point is not actionable for the caller: failing to
                                    // send a previously-queued packet doesn't mean that updating
                                    // the neighbor entry should fail.
                                    let _: Result<(), _> = sync_ctx
                                        .send_ip_packet_to_neighbor_link_addr(
                                            ctx,
                                            device_id,
                                            link_address,
                                            body,
                                        );
                                }
                            }
                            DynamicNeighborState::Complete { link_address: _ } => {}
                        }
                    }
                    NeighborState::Static(_) => {}
                },
            }
        });
    }

    fn set_static_neighbor(
        &mut self,
        ctx: &mut C,
        device_id: &SC::DeviceId,
        neighbor: SpecifiedAddr<I::Addr>,
        link_address: D::Address,
    ) {
        self.with_nud_state_mut_and_buf_ctx(device_id, |NudState { neighbors }, sync_ctx| {
            match neighbors.insert(neighbor, NeighborState::Static(link_address)) {
                Some(NeighborState::Dynamic(DynamicNeighborState::Incomplete {
                    transmit_counter: _,
                    pending_frames,
                })) => {
                    assert_ne!(
                        ctx.cancel_timer(NudTimerId {
                            device_id: device_id.clone(),
                            lookup_addr: neighbor,
                            _marker: PhantomData,
                        }),
                        None,
                        "previously incomplete entry for {} should have had a timer",
                        neighbor
                    );

                    for body in pending_frames {
                        // Ignore any errors on sending the IP packet, because a failure at this
                        // point is not actionable for the caller: failing to send a previously-
                        // queued packet doesn't mean that updating the neighbor entry should fail.
                        let _: Result<(), _> = sync_ctx.send_ip_packet_to_neighbor_link_addr(
                            ctx,
                            device_id,
                            link_address,
                            body,
                        );
                    }
                }
                None
                | Some(NeighborState::Static(_))
                | Some(NeighborState::Dynamic(DynamicNeighborState::Complete {
                    link_address: _,
                })) => {}
            }
        });
    }

    fn flush(&mut self, ctx: &mut C, device_id: &Self::DeviceId) {
        let previously_incomplete = self.with_nud_state_mut(device_id, |NudState { neighbors }| {
            let mut previously_incomplete = Vec::new();

            neighbors.retain(|neighbor, state| {
                match state {
                    NeighborState::Dynamic(state) => {
                        match state {
                            DynamicNeighborState::Incomplete {
                                transmit_counter: _,
                                pending_frames: _,
                            } => {
                                previously_incomplete.push(*neighbor);
                            }
                            DynamicNeighborState::Complete { link_address: _ } => {}
                        }

                        // Only flush dynamic entries.
                        false
                    }
                    NeighborState::Static(_) => true,
                }
            });

            previously_incomplete
        });

        previously_incomplete.into_iter().for_each(|neighbor| {
            assert_ne!(
                ctx.cancel_timer(NudTimerId {
                    device_id: device_id.clone(),
                    lookup_addr: neighbor,
                    _marker: PhantomData,
                }),
                None,
                "previously incomplete entry for {} should have had a timer",
                neighbor
            );
        });
    }

    // TODO(https://fxbug.dev/120878): Initiate a neighbor probe if the neighbor is
    // not found, or if its entry is incomplete.
    fn resolve_link_addr(
        &mut self,
        device: &SC::DeviceId,
        dst: &SpecifiedAddr<I::Addr>,
    ) -> NeighborLinkAddr<D::Address> {
        NudContext::with_nud_state_mut(
            self,
            device,
            |NudState { neighbors }: &mut NudState<_, _>| {
                neighbors.get(dst).map_or(
                    NeighborLinkAddr::PendingNeighborResolution,
                    |neighbor_state| match neighbor_state {
                        NeighborState::Static(link_address)
                        | NeighborState::Dynamic(DynamicNeighborState::Complete { link_address }) => {
                            NeighborLinkAddr::Resolved(link_address.clone())
                        }
                        NeighborState::Dynamic(DynamicNeighborState::Incomplete {
                            transmit_counter: _,
                            pending_frames: _,
                        }) => NeighborLinkAddr::PendingNeighborResolution,
                    },
                )
            },
        )
    }
}

impl<
        B: BufferMut,
        I: Ip,
        D: LinkDevice,
        C: NonSyncNudContext<I, D, SC::DeviceId>,
        SC: BufferNudContext<B, I, D, C>,
    > BufferNudHandler<B, I, D, C> for SC
{
    fn send_ip_packet_to_neighbor<S: Serializer<Buffer = B>>(
        &mut self,
        ctx: &mut C,
        device_id: &Self::DeviceId,
        lookup_addr: SpecifiedAddr<I::Addr>,
        body: S,
    ) -> Result<(), S> {
        let do_solicit =
            self.with_nud_state_mut_and_buf_ctx(device_id, |NudState { neighbors }, sync_ctx| {
                match neighbors.entry(lookup_addr) {
                    Entry::Vacant(e) => {
                        let _: &mut NeighborState<_> = e.insert(NeighborState::Dynamic(
                            DynamicNeighborState::new_incomplete_with_pending_frame(
                                MAX_MULTICAST_SOLICIT - 1,
                                body.serialize_vec_outer()
                                    .map_err(|(_err, s)| s)?
                                    .map_a(|b| Buf::new(b.as_ref().to_vec(), ..))
                                    .into_inner(),
                            ),
                        ));

                        Ok(true)
                    }
                    Entry::Occupied(e) => match e.into_mut() {
                        NeighborState::Dynamic(DynamicNeighborState::Incomplete {
                            transmit_counter: _,
                            pending_frames,
                        }) => {
                            // We don't accept new packets when the queue is full
                            // because earlier packets are more likely to initiate
                            // connections whereas later packets are more likely to
                            // carry data. E.g. A TCP SYN/SYN-ACK is likely to appear
                            // before a TCP segment with data and dropping the
                            // SYN/SYN-ACK may result in the TCP peer not processing the
                            // segment with data since the segment completing the
                            // handshake has not been received and handled yet.
                            if pending_frames.len() < MAX_PENDING_FRAMES {
                                pending_frames.push_back(
                                    body.serialize_vec_outer()
                                        .map_err(|(_err, s)| s)?
                                        .map_a(|b| Buf::new(b.as_ref().to_vec(), ..))
                                        .into_inner(),
                                );
                            }

                            Ok(false)
                        }
                        NeighborState::Dynamic(DynamicNeighborState::Complete { link_address })
                        | NeighborState::Static(link_address) => {
                            // Send the IP packet while holding the NUD lock to prevent a potential
                            // ordering violation.
                            //
                            // If we drop the NUD lock before sending out this packet, another
                            // thread could take the NUD lock and send a packet *before* this packet
                            // is sent out, resulting in out-of-order transmission to the device.
                            sync_ctx.send_ip_packet_to_neighbor_link_addr(
                                ctx,
                                device_id,
                                *link_address,
                                body,
                            )?;

                            Ok(false)
                        }
                    },
                }
            })?;

        if do_solicit {
            solicit_neighbor(self, ctx, device_id, lookup_addr);
        }

        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use alloc::{vec, vec::Vec};
    use core::num::NonZeroU64;

    use ip_test_macro::ip_test;
    use lock_order::Locked;
    use net_declare::{net_ip_v4, net_ip_v6};
    use net_types::{
        ethernet::Mac,
        ip::{AddrSubnet, IpAddress as _, Ipv4, Ipv4Addr, Ipv6, Ipv6Addr},
        UnicastAddr, Witness as _,
    };
    use packet::{Buf, InnerPacketBuilder as _, Serializer as _};
    use packet_formats::{
        ethernet::{EtherType, EthernetFrameLengthCheck},
        icmp::{
            ndp::{
                options::NdpOptionBuilder, NeighborAdvertisement, NeighborSolicitation,
                OptionSequenceBuilder, RouterAdvertisement,
            },
            IcmpPacketBuilder, IcmpUnusedCode,
        },
        ip::Ipv6Proto,
        ipv6::Ipv6PacketBuilder,
        testutil::{parse_ethernet_frame, parse_icmp_packet_in_ip_packet_in_ethernet_frame},
    };
    use test_case::test_case;

    use super::*;
    use crate::{
        context::{
            testutil::{
                handle_timer_helper_with_sc_ref_mut, FakeCtxWithSyncCtx, FakeNonSyncCtx,
                FakeSyncCtx, FakeTimerCtxExt as _, WrappedFakeSyncCtx,
            },
            InstantContext, SendFrameContext as _,
        },
        device::{
            ethernet::EthernetLinkDevice,
            link::testutil::{FakeLinkAddress, FakeLinkDevice, FakeLinkDeviceId},
            ndp::testutil::{neighbor_advertisement_ip_packet, neighbor_solicitation_ip_packet},
            testutil::FakeWeakDeviceId,
            update_ipv6_configuration, EthernetDeviceId,
        },
        ip::{
            device::{
                slaac::SlaacConfiguration,
                testutil::UpdateIpDeviceConfigurationAndFlagsTestIpExt as _,
                Ipv6DeviceConfigurationUpdate,
            },
            icmp::REQUIRED_NDP_IP_PACKET_HOP_LIMIT,
            receive_ip_packet, FrameDestination,
        },
        testutil::{
            FakeEventDispatcherConfig, TestIpExt as _, DEFAULT_INTERFACE_METRIC,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
        },
        NonSyncContext, SyncCtx,
    };

    struct FakeNudContext<I: Ip, LinkAddr> {
        retrans_timer: NonZeroDuration,
        nud: NudState<I, LinkAddr>,
    }

    type FakeCtxImpl<I> = WrappedFakeSyncCtx<
        FakeNudContext<I, FakeLinkAddress>,
        (),
        FakeNudMessageMeta<I>,
        FakeLinkDeviceId,
    >;

    type FakeInnerCtxImpl<I> = FakeSyncCtx<(), FakeNudMessageMeta<I>, FakeLinkDeviceId>;

    #[derive(Debug, PartialEq, Eq)]
    enum FakeNudMessageMeta<I: Ip> {
        NeighborSolicitation { lookup_addr: SpecifiedAddr<I::Addr> },
        IpFrame { dst_link_address: FakeLinkAddress },
    }

    type FakeNonSyncCtxImpl<I> =
        FakeNonSyncCtx<NudTimerId<I, FakeLinkDevice, FakeLinkDeviceId>, (), ()>;

    impl<I: Ip> DeviceIdContext<FakeLinkDevice> for FakeCtxImpl<I> {
        type DeviceId = FakeLinkDeviceId;
        type WeakDeviceId = FakeWeakDeviceId<FakeLinkDeviceId>;

        fn downgrade_device_id(&self, device_id: &Self::DeviceId) -> Self::WeakDeviceId {
            FakeWeakDeviceId(device_id.clone())
        }

        fn is_device_installed(&self, _device_id: &Self::DeviceId) -> bool {
            true
        }

        fn upgrade_weak_device_id(
            &self,
            weak_device_id: &Self::WeakDeviceId,
        ) -> Option<Self::DeviceId> {
            let FakeWeakDeviceId(id) = weak_device_id;
            Some(id.clone())
        }
    }

    impl<I: Ip> NudContext<I, FakeLinkDevice, FakeNonSyncCtxImpl<I>> for FakeCtxImpl<I> {
        fn retrans_timer(&mut self, &FakeLinkDeviceId: &FakeLinkDeviceId) -> NonZeroDuration {
            self.outer.retrans_timer
        }

        fn with_nud_state_mut<O, F: FnOnce(&mut NudState<I, FakeLinkAddress>) -> O>(
            &mut self,
            &FakeLinkDeviceId: &FakeLinkDeviceId,
            cb: F,
        ) -> O {
            cb(&mut self.outer.nud)
        }

        fn send_neighbor_solicitation(
            &mut self,
            ctx: &mut FakeNonSyncCtxImpl<I>,
            &FakeLinkDeviceId: &FakeLinkDeviceId,
            lookup_addr: SpecifiedAddr<I::Addr>,
        ) {
            self.inner
                .send_frame(
                    ctx,
                    FakeNudMessageMeta::NeighborSolicitation { lookup_addr },
                    Buf::new(Vec::new(), ..),
                )
                .unwrap()
        }
    }

    impl<B: BufferMut, I: Ip> BufferNudContext<B, I, FakeLinkDevice, FakeNonSyncCtxImpl<I>>
        for FakeCtxImpl<I>
    {
        type BufferSenderCtx<'a> = FakeInnerCtxImpl<I>;

        fn with_nud_state_mut_and_buf_ctx<
            O,
            F: FnOnce(&mut NudState<I, FakeLinkAddress>, &mut Self::BufferSenderCtx<'_>) -> O,
        >(
            &mut self,
            _device_id: &Self::DeviceId,
            cb: F,
        ) -> O {
            let Self { outer, inner } = self;
            cb(&mut outer.nud, inner)
        }
    }

    impl<B: BufferMut, I: Ip> BufferNudSenderContext<B, I, FakeLinkDevice, FakeNonSyncCtxImpl<I>>
        for FakeInnerCtxImpl<I>
    {
        fn send_ip_packet_to_neighbor_link_addr<S: Serializer<Buffer = B>>(
            &mut self,
            ctx: &mut FakeNonSyncCtxImpl<I>,
            _device_id: &FakeLinkDeviceId,
            dst_link_address: FakeLinkAddress,
            body: S,
        ) -> Result<(), S> {
            self.send_frame(ctx, FakeNudMessageMeta::IpFrame { dst_link_address }, body)
        }
    }

    const ONE_SECOND: NonZeroDuration =
        NonZeroDuration::from_nonzero_secs(const_unwrap::const_unwrap_option(NonZeroU64::new(1)));

    fn check_lookup_has<I: Ip>(
        sync_ctx: &mut FakeCtxImpl<I>,
        ctx: &mut FakeNonSyncCtxImpl<I>,
        lookup_addr: SpecifiedAddr<I::Addr>,
        expected_link_addr: FakeLinkAddress,
    ) {
        assert_matches!(
            sync_ctx.outer.nud.neighbors.get(&lookup_addr),
            Some(
                NeighborState::Dynamic(DynamicNeighborState::Complete { link_address })
                | NeighborState::Static(link_address)
            ) => {
                assert_eq!(link_address, &expected_link_addr)
            }
        );
        ctx.timer_ctx().assert_no_timers_installed();
    }

    trait TestIpExt: Ip {
        const LOOKUP_ADDR1: SpecifiedAddr<Self::Addr>;
        const LOOKUP_ADDR2: SpecifiedAddr<Self::Addr>;
        const LOOKUP_ADDR3: SpecifiedAddr<Self::Addr>;
    }

    impl TestIpExt for Ipv4 {
        // Safe because the address is non-zero.
        const LOOKUP_ADDR1: SpecifiedAddr<Ipv4Addr> =
            unsafe { SpecifiedAddr::new_unchecked(net_ip_v4!("192.168.0.1")) };
        const LOOKUP_ADDR2: SpecifiedAddr<Ipv4Addr> =
            unsafe { SpecifiedAddr::new_unchecked(net_ip_v4!("192.168.0.2")) };
        const LOOKUP_ADDR3: SpecifiedAddr<Ipv4Addr> =
            unsafe { SpecifiedAddr::new_unchecked(net_ip_v4!("192.168.0.3")) };
    }

    impl TestIpExt for Ipv6 {
        // Safe because the address is non-zero.
        const LOOKUP_ADDR1: SpecifiedAddr<Ipv6Addr> =
            unsafe { SpecifiedAddr::new_unchecked(net_ip_v6!("fe80::1")) };
        const LOOKUP_ADDR2: SpecifiedAddr<Ipv6Addr> =
            unsafe { SpecifiedAddr::new_unchecked(net_ip_v6!("fe80::2")) };
        const LOOKUP_ADDR3: SpecifiedAddr<Ipv6Addr> =
            unsafe { SpecifiedAddr::new_unchecked(net_ip_v6!("fe80::3")) };
    }

    #[ip_test]
    fn comfirmation_should_not_create_entry<I: Ip + TestIpExt>() {
        let FakeCtxWithSyncCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtxWithSyncCtx::with_sync_ctx(FakeCtxImpl::<I>::with_inner_and_outer_state(
                (),
                FakeNudContext { retrans_timer: ONE_SECOND, nud: Default::default() },
            ));

        let link_addr = FakeLinkAddress([1]);
        NudHandler::set_dynamic_neighbor(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeLinkDeviceId,
            I::LOOKUP_ADDR1,
            link_addr,
            DynamicNeighborUpdateSource::Confirmation,
        );
        assert_eq!(sync_ctx.outer.nud, Default::default());
    }

    const LINK_ADDR1: FakeLinkAddress = FakeLinkAddress([1]);
    const LINK_ADDR2: FakeLinkAddress = FakeLinkAddress([2]);
    const LINK_ADDR3: FakeLinkAddress = FakeLinkAddress([3]);

    #[ip_test]
    #[test_case(true; "set_with_dynamic")]
    #[test_case(false; "set_with_static")]
    fn pending_frames<I: Ip + TestIpExt>(dynamic: bool) {
        let FakeCtxWithSyncCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtxWithSyncCtx::with_sync_ctx(FakeCtxImpl::<I>::with_inner_and_outer_state(
                (),
                FakeNudContext { retrans_timer: ONE_SECOND, nud: Default::default() },
            ));
        assert_eq!(sync_ctx.inner.take_frames(), []);

        // Send up to the maximum number of pending frames to some neighbor
        // which requires resolution. This should cause all frames to be queued
        // pending resolution completion.
        const MAX_PENDING_FRAMES_U8: u8 = MAX_PENDING_FRAMES as u8;
        let expected_pending_frames =
            (0..MAX_PENDING_FRAMES_U8).map(|i| Buf::new(vec![i], ..)).collect::<VecDeque<_>>();

        for body in expected_pending_frames.iter() {
            assert_eq!(
                BufferNudHandler::send_ip_packet_to_neighbor(
                    &mut sync_ctx,
                    &mut non_sync_ctx,
                    &FakeLinkDeviceId,
                    I::LOOKUP_ADDR1,
                    body.clone()
                ),
                Ok(())
            );
        }
        // Should have only sent out a single neighbor probe message.
        assert_eq!(
            sync_ctx.inner.take_frames(),
            [(
                FakeNudMessageMeta::NeighborSolicitation { lookup_addr: I::LOOKUP_ADDR1 },
                Vec::new()
            )]
        );
        assert_matches!(
            sync_ctx.outer.nud.neighbors.get(&I::LOOKUP_ADDR1),
            Some(NeighborState::Dynamic(DynamicNeighborState::Incomplete {
                transmit_counter: _,
                pending_frames
            })) => {
                assert_eq!(pending_frames, &expected_pending_frames);
            }
        );

        // The next frame should be dropped.
        assert_eq!(
            BufferNudHandler::send_ip_packet_to_neighbor(
                &mut sync_ctx,
                &mut non_sync_ctx,
                &FakeLinkDeviceId,
                I::LOOKUP_ADDR1,
                Buf::new([123], ..),
            ),
            Ok(())
        );
        assert_eq!(sync_ctx.inner.take_frames(), []);
        assert_matches!(
            sync_ctx.outer.nud.neighbors.get(&I::LOOKUP_ADDR1),
            Some(NeighborState::Dynamic(DynamicNeighborState::Incomplete {
                transmit_counter: _,
                pending_frames
            })) => {
                assert_eq!(pending_frames, &expected_pending_frames);
            }
        );

        // Completing resolution should result in all queued packets to be sent.
        if dynamic {
            NudHandler::set_dynamic_neighbor(
                &mut sync_ctx,
                &mut non_sync_ctx,
                &FakeLinkDeviceId,
                I::LOOKUP_ADDR1,
                LINK_ADDR1,
                DynamicNeighborUpdateSource::Confirmation,
            );
        } else {
            NudHandler::set_static_neighbor(
                &mut sync_ctx,
                &mut non_sync_ctx,
                &FakeLinkDeviceId,
                I::LOOKUP_ADDR1,
                LINK_ADDR1,
            );
        }
        non_sync_ctx.timer_ctx().assert_no_timers_installed();
        assert_eq!(
            sync_ctx.inner.take_frames(),
            expected_pending_frames
                .into_iter()
                .map(|p| (
                    FakeNudMessageMeta::IpFrame { dst_link_address: LINK_ADDR1 },
                    p.as_ref().to_vec()
                ))
                .collect::<Vec<_>>()
        );
    }

    #[ip_test]
    fn static_neighbor<I: Ip + TestIpExt>() {
        let FakeCtxWithSyncCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtxWithSyncCtx::with_sync_ctx(FakeCtxImpl::<I>::with_inner_and_outer_state(
                (),
                FakeNudContext { retrans_timer: ONE_SECOND, nud: Default::default() },
            ));

        NudHandler::set_static_neighbor(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeLinkDeviceId,
            I::LOOKUP_ADDR1,
            LINK_ADDR1,
        );
        non_sync_ctx.timer_ctx().assert_no_timers_installed();
        assert_eq!(sync_ctx.inner.take_frames(), []);
        check_lookup_has(&mut sync_ctx, &mut non_sync_ctx, I::LOOKUP_ADDR1, LINK_ADDR1);

        // Dynamic entries should not overwrite static entries.
        NudHandler::set_dynamic_neighbor(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeLinkDeviceId,
            I::LOOKUP_ADDR1,
            LINK_ADDR2,
            DynamicNeighborUpdateSource::Probe,
        );
        check_lookup_has(&mut sync_ctx, &mut non_sync_ctx, I::LOOKUP_ADDR1, LINK_ADDR1);
    }

    #[ip_test]
    fn dynamic_neighbor<I: Ip + TestIpExt>() {
        let FakeCtxWithSyncCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtxWithSyncCtx::with_sync_ctx(FakeCtxImpl::<I>::with_inner_and_outer_state(
                (),
                FakeNudContext { retrans_timer: ONE_SECOND, nud: Default::default() },
            ));

        NudHandler::set_dynamic_neighbor(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeLinkDeviceId,
            I::LOOKUP_ADDR1,
            LINK_ADDR1,
            DynamicNeighborUpdateSource::Probe,
        );
        non_sync_ctx.timer_ctx().assert_no_timers_installed();
        assert_eq!(sync_ctx.inner.take_frames(), []);
        check_lookup_has(&mut sync_ctx, &mut non_sync_ctx, I::LOOKUP_ADDR1, LINK_ADDR1);

        // Dynamic entries may be overwritten by new dynamic entries.
        NudHandler::set_dynamic_neighbor(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeLinkDeviceId,
            I::LOOKUP_ADDR1,
            LINK_ADDR2,
            DynamicNeighborUpdateSource::Probe,
        );
        check_lookup_has(&mut sync_ctx, &mut non_sync_ctx, I::LOOKUP_ADDR1, LINK_ADDR2);
        assert_eq!(sync_ctx.inner.take_frames(), []);

        // A static entry may overwrite a dynamic entry.
        NudHandler::set_static_neighbor(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeLinkDeviceId,
            I::LOOKUP_ADDR1,
            LINK_ADDR3,
        );
        check_lookup_has(&mut sync_ctx, &mut non_sync_ctx, I::LOOKUP_ADDR1, LINK_ADDR3);
        assert_eq!(sync_ctx.inner.take_frames(), []);
    }

    #[ip_test]
    fn send_solicitation_on_lookup<I: Ip + TestIpExt>() {
        let FakeCtxWithSyncCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtxWithSyncCtx::with_sync_ctx(FakeCtxImpl::<I>::with_inner_and_outer_state(
                (),
                FakeNudContext { retrans_timer: ONE_SECOND, nud: Default::default() },
            ));
        non_sync_ctx.timer_ctx().assert_no_timers_installed();
        assert_eq!(sync_ctx.inner.take_frames(), []);

        let mut pending_frames = VecDeque::new();
        let mut send_ip_packet_to_neighbor =
            |sync_ctx: &mut FakeCtxImpl<I>, non_sync_ctx: &mut FakeNonSyncCtxImpl<I>, body: u8| {
                let body = [body];
                assert_eq!(
                    BufferNudHandler::send_ip_packet_to_neighbor(
                        sync_ctx,
                        non_sync_ctx,
                        &FakeLinkDeviceId,
                        I::LOOKUP_ADDR1,
                        Buf::new(body, ..),
                    ),
                    Ok(())
                );

                pending_frames.push_back(Buf::new(body.to_vec(), ..));

                let FakeNudContext { retrans_timer: _, nud } = &sync_ctx.outer;
                assert_eq!(
                    nud.neighbors,
                    HashMap::from([(
                        I::LOOKUP_ADDR1,
                        NeighborState::Dynamic(DynamicNeighborState::Incomplete {
                            transmit_counter: NonZeroU8::new(MAX_MULTICAST_SOLICIT - 1),
                            pending_frames: pending_frames.clone(),
                        }),
                    )])
                );
                non_sync_ctx.timer_ctx().assert_timers_installed([(
                    NudTimerId {
                        device_id: FakeLinkDeviceId,
                        lookup_addr: I::LOOKUP_ADDR1,
                        _marker: PhantomData,
                    },
                    non_sync_ctx.now() + ONE_SECOND.get(),
                )]);
            };

        send_ip_packet_to_neighbor(&mut sync_ctx, &mut non_sync_ctx, 1);
        assert_eq!(
            sync_ctx.inner.take_frames(),
            [(
                FakeNudMessageMeta::NeighborSolicitation { lookup_addr: I::LOOKUP_ADDR1 },
                Vec::new()
            )]
        );

        send_ip_packet_to_neighbor(&mut sync_ctx, &mut non_sync_ctx, 2);
        assert_eq!(sync_ctx.inner.take_frames(), []);

        // Complete link resolution.
        NudHandler::set_dynamic_neighbor(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeLinkDeviceId,
            I::LOOKUP_ADDR1,
            LINK_ADDR1,
            DynamicNeighborUpdateSource::Confirmation,
        );
        check_lookup_has(&mut sync_ctx, &mut non_sync_ctx, I::LOOKUP_ADDR1, LINK_ADDR1);

        let FakeNudContext { retrans_timer: _, nud } = &sync_ctx.outer;
        assert_eq!(
            nud.neighbors,
            HashMap::from([(
                I::LOOKUP_ADDR1,
                NeighborState::Dynamic(DynamicNeighborState::Complete { link_address: LINK_ADDR1 }),
            )])
        );
        assert_eq!(
            sync_ctx.inner.take_frames(),
            pending_frames
                .into_iter()
                .map(|f| (
                    FakeNudMessageMeta::IpFrame { dst_link_address: LINK_ADDR1 },
                    f.as_ref().to_vec(),
                ))
                .collect::<Vec<_>>()
        );
    }

    #[ip_test]
    fn solicitation_failure<I: Ip + TestIpExt>() {
        let FakeCtxWithSyncCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtxWithSyncCtx::with_sync_ctx(FakeCtxImpl::<I>::with_inner_and_outer_state(
                (),
                FakeNudContext { retrans_timer: ONE_SECOND, nud: Default::default() },
            ));
        non_sync_ctx.timer_ctx().assert_no_timers_installed();
        assert_eq!(sync_ctx.inner.take_frames(), []);

        let body = [1];
        let pending_frames = VecDeque::from([Buf::new(body.to_vec(), ..)]);
        assert_eq!(
            BufferNudHandler::send_ip_packet_to_neighbor(
                &mut sync_ctx,
                &mut non_sync_ctx,
                &FakeLinkDeviceId,
                I::LOOKUP_ADDR1,
                Buf::new(body, ..),
            ),
            Ok(())
        );

        let timer_id = NudTimerId {
            device_id: FakeLinkDeviceId,
            lookup_addr: I::LOOKUP_ADDR1,
            _marker: PhantomData,
        };
        for i in 1..=MAX_MULTICAST_SOLICIT {
            let FakeNudContext { retrans_timer, nud } = &sync_ctx.outer;
            let retrans_timer = retrans_timer.get();

            assert_eq!(
                nud.neighbors,
                HashMap::from([(
                    I::LOOKUP_ADDR1,
                    NeighborState::Dynamic(DynamicNeighborState::Incomplete {
                        transmit_counter: NonZeroU8::new(MAX_MULTICAST_SOLICIT - i),
                        pending_frames: pending_frames.clone(),
                    }),
                )])
            );

            non_sync_ctx
                .timer_ctx()
                .assert_timers_installed([(timer_id, non_sync_ctx.now() + ONE_SECOND.get())]);
            assert_eq!(
                sync_ctx.inner.take_frames(),
                [(
                    FakeNudMessageMeta::NeighborSolicitation { lookup_addr: I::LOOKUP_ADDR1 },
                    Vec::new()
                )]
            );

            assert_eq!(
                non_sync_ctx.trigger_timers_for(
                    retrans_timer,
                    handle_timer_helper_with_sc_ref_mut(&mut sync_ctx, TimerHandler::handle_timer),
                ),
                [timer_id]
            );
        }

        let FakeNudContext { retrans_timer: _, nud } = &sync_ctx.outer;
        assert_eq!(nud.neighbors, HashMap::new());
        non_sync_ctx.timer_ctx().assert_no_timers_installed();
        assert_eq!(sync_ctx.inner.take_frames(), []);
    }

    #[ip_test]
    fn flush_entries<I: Ip + TestIpExt>() {
        let FakeCtxWithSyncCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtxWithSyncCtx::with_sync_ctx(FakeCtxImpl::<I>::with_inner_and_outer_state(
                (),
                FakeNudContext { retrans_timer: ONE_SECOND, nud: Default::default() },
            ));
        non_sync_ctx.timer_ctx().assert_no_timers_installed();
        assert_eq!(sync_ctx.inner.take_frames(), []);

        NudHandler::set_static_neighbor(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeLinkDeviceId,
            I::LOOKUP_ADDR1,
            LINK_ADDR1,
        );
        NudHandler::set_dynamic_neighbor(
            &mut sync_ctx,
            &mut non_sync_ctx,
            &FakeLinkDeviceId,
            I::LOOKUP_ADDR2,
            LINK_ADDR2,
            DynamicNeighborUpdateSource::Probe,
        );
        let body = [3];
        let pending_frames = VecDeque::from([Buf::new(body.to_vec(), ..)]);
        assert_eq!(
            BufferNudHandler::send_ip_packet_to_neighbor(
                &mut sync_ctx,
                &mut non_sync_ctx,
                &FakeLinkDeviceId,
                I::LOOKUP_ADDR3,
                Buf::new(body, ..),
            ),
            Ok(())
        );

        let FakeNudContext { retrans_timer: _, nud } = &sync_ctx.outer;
        assert_eq!(
            nud.neighbors,
            HashMap::from([
                (I::LOOKUP_ADDR1, NeighborState::Static(LINK_ADDR1),),
                (
                    I::LOOKUP_ADDR2,
                    NeighborState::Dynamic(DynamicNeighborState::Complete {
                        link_address: LINK_ADDR2
                    }),
                ),
                (
                    I::LOOKUP_ADDR3,
                    NeighborState::Dynamic(DynamicNeighborState::Incomplete {
                        transmit_counter: NonZeroU8::new(MAX_MULTICAST_SOLICIT - 1),
                        pending_frames: pending_frames,
                    }),
                ),
            ])
        );
        non_sync_ctx.timer_ctx().assert_timers_installed([(
            NudTimerId {
                device_id: FakeLinkDeviceId,
                lookup_addr: I::LOOKUP_ADDR3,
                _marker: PhantomData,
            },
            non_sync_ctx.now() + ONE_SECOND.get(),
        )]);

        // Flushing the table should clear all dynamic entries and timers.
        NudHandler::flush(&mut sync_ctx, &mut non_sync_ctx, &FakeLinkDeviceId);
        let FakeNudContext { retrans_timer: _, nud } = &sync_ctx.outer;
        assert_eq!(
            nud.neighbors,
            HashMap::from([(I::LOOKUP_ADDR1, NeighborState::Static(LINK_ADDR1),),])
        );
        non_sync_ctx.timer_ctx().assert_no_timers_installed();
    }

    fn assert_neighbors<
        'a,
        I: Ip,
        C: NonSyncContext + NonSyncNudContext<I, EthernetLinkDevice, EthernetDeviceId<C>>,
    >(
        sync_ctx: &'a SyncCtx<C>,
        device_id: &EthernetDeviceId<C>,
        expected: HashMap<SpecifiedAddr<I::Addr>, NeighborState<Mac>>,
    ) where
        Locked<&'a SyncCtx<C>, crate::lock_ordering::Unlocked>: NudContext<I, EthernetLinkDevice, C>
            + DeviceIdContext<EthernetLinkDevice, DeviceId = EthernetDeviceId<C>>,
    {
        NudContext::<I, EthernetLinkDevice, _>::with_nud_state_mut(
            &mut Locked::new(sync_ctx),
            device_id,
            |NudState { neighbors }| assert_eq!(*neighbors, expected),
        )
    }

    #[ip_test]
    #[test_case(None, NeighborLinkAddr::PendingNeighborResolution; "no_neighbor")]
    #[test_case(Some(DynamicNeighborUpdateSource::Confirmation),
                NeighborLinkAddr::PendingNeighborResolution; "incomplete_neighbor")]
    #[test_case(Some(DynamicNeighborUpdateSource::Probe),
                NeighborLinkAddr::Resolved(LINK_ADDR1); "complete_neighbor")]
    fn resolve_link_addr<I: Ip + TestIpExt>(
        initial_neighbor_state: Option<DynamicNeighborUpdateSource>,
        expected_result: NeighborLinkAddr<FakeLinkAddress>,
    ) {
        let FakeCtxWithSyncCtx { mut sync_ctx, mut non_sync_ctx } =
            FakeCtxWithSyncCtx::with_sync_ctx(FakeCtxImpl::<I>::with_inner_and_outer_state(
                (),
                FakeNudContext { retrans_timer: ONE_SECOND, nud: Default::default() },
            ));
        non_sync_ctx.timer_ctx().assert_no_timers_installed();
        assert_eq!(sync_ctx.inner.take_frames(), []);

        if let Some(source) = initial_neighbor_state {
            NudHandler::set_dynamic_neighbor(
                &mut sync_ctx,
                &mut non_sync_ctx,
                &FakeLinkDeviceId,
                I::LOOKUP_ADDR1,
                LINK_ADDR1,
                source,
            );
        }

        assert_eq!(
            NudHandler::resolve_link_addr(&mut sync_ctx, &FakeLinkDeviceId, &I::LOOKUP_ADDR1),
            expected_result
        );
    }

    #[test]
    fn router_advertisement_with_source_link_layer_option_should_add_neighbor() {
        let FakeEventDispatcherConfig {
            local_mac,
            remote_mac,
            local_ip: _,
            remote_ip: _,
            subnet: _,
        } = Ipv6::FAKE_CONFIG;

        let crate::testutil::FakeCtx { sync_ctx, mut non_sync_ctx } =
            crate::testutil::FakeCtx::default();
        let sync_ctx = &sync_ctx;
        let device_id = crate::device::add_ethernet_device(
            sync_ctx,
            local_mac,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
            DEFAULT_INTERFACE_METRIC,
        )
        .into();
        Ipv6::set_ip_device_enabled(sync_ctx, &mut non_sync_ctx, &device_id, true, false);

        let remote_mac_bytes = remote_mac.bytes();
        let options = vec![NdpOptionBuilder::SourceLinkLayerAddress(&remote_mac_bytes[..])];

        let src_ip = remote_mac.to_ipv6_link_local().addr();
        let dst_ip = Ipv6::ALL_NODES_LINK_LOCAL_MULTICAST_ADDRESS.get();
        let ra_packet_buf = |options: &[NdpOptionBuilder<'_>]| {
            OptionSequenceBuilder::new(options.iter())
                .into_serializer()
                .encapsulate(IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                    src_ip,
                    dst_ip,
                    IcmpUnusedCode,
                    RouterAdvertisement::new(0, false, false, 0, 0, 0),
                ))
                .encapsulate(Ipv6PacketBuilder::new(
                    src_ip,
                    dst_ip,
                    REQUIRED_NDP_IP_PACKET_HOP_LIMIT,
                    Ipv6Proto::Icmpv6,
                ))
                .serialize_vec_outer()
                .unwrap()
                .unwrap_b()
        };

        // First receive a Router Advertisement without the source link layer
        // and make sure no new neighbor gets added.
        receive_ip_packet::<_, _, Ipv6>(
            &sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            FrameDestination::Multicast,
            ra_packet_buf(&[][..]),
        );
        let link_device_id = device_id.clone().try_into().unwrap();
        assert_neighbors::<Ipv6, _>(&sync_ctx, &link_device_id, Default::default());

        // RA with a source link layer option should create a new entry.
        receive_ip_packet::<_, _, Ipv6>(
            &sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            FrameDestination::Multicast,
            ra_packet_buf(&options[..]),
        );
        assert_neighbors::<Ipv6, _>(
            &sync_ctx,
            &link_device_id,
            HashMap::from([(
                {
                    let src_ip: UnicastAddr<_> = src_ip.into_addr();
                    src_ip.into_specified()
                },
                NeighborState::Dynamic(DynamicNeighborState::Complete {
                    link_address: remote_mac.get(),
                }),
            )]),
        );
    }

    const LOCAL_IP: Ipv6Addr = net_ip_v6!("fe80::1");
    const OTHER_IP: Ipv6Addr = net_ip_v6!("fe80::2");
    const MULTICAST_IP: Ipv6Addr = net_ip_v6!("ff02::1234");

    #[test_case(LOCAL_IP, None, true; "targeting assigned address")]
    #[test_case(LOCAL_IP, NonZeroU8::new(1), false; "targeting tentative address")]
    #[test_case(OTHER_IP, None, false; "targeting other host")]
    #[test_case(MULTICAST_IP, None, false; "targeting multicast address")]
    fn ns_response(target_addr: Ipv6Addr, dad_transmits: Option<NonZeroU8>, expect_handle: bool) {
        let FakeEventDispatcherConfig {
            local_mac,
            remote_mac,
            local_ip: _,
            remote_ip: _,
            subnet: _,
        } = Ipv6::FAKE_CONFIG;

        let crate::testutil::FakeCtx { sync_ctx, mut non_sync_ctx } =
            crate::testutil::FakeCtx::default();
        let sync_ctx = &sync_ctx;
        let link_device_id = crate::device::add_ethernet_device(
            sync_ctx,
            local_mac,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
            DEFAULT_INTERFACE_METRIC,
        );
        let device_id = link_device_id.clone().into();
        Ipv6::set_ip_device_enabled(sync_ctx, &mut non_sync_ctx, &device_id, true, false);

        // Set DAD config after enabling the device so that the default address
        // does not perform DAD.
        let _: Ipv6DeviceConfigurationUpdate = update_ipv6_configuration(
            sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            Ipv6DeviceConfigurationUpdate {
                dad_transmits: Some(dad_transmits),
                ..Default::default()
            },
        )
        .unwrap();
        crate::device::add_ip_addr_subnet(
            &sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            AddrSubnet::new(LOCAL_IP, Ipv6Addr::BYTES * 8).unwrap(),
        )
        .unwrap();
        if let Some(NonZeroU8 { .. }) = dad_transmits {
            // Take DAD message.
            assert_matches!(
                &non_sync_ctx.take_frames()[..],
                [(got_device_id, got_frame)] => {
                    assert_eq!(got_device_id, &link_device_id);

                    let (src_mac, dst_mac, got_src_ip, got_dst_ip, ttl, message, code) =
                        parse_icmp_packet_in_ip_packet_in_ethernet_frame::<
                            Ipv6,
                            _,
                            NeighborSolicitation,
                            _,
                        >(got_frame, EthernetFrameLengthCheck::NoCheck, |_| {})
                            .unwrap();
                    let dst_ip = LOCAL_IP.to_solicited_node_address();
                    assert_eq!(src_mac, local_mac.get());
                    assert_eq!(dst_mac, dst_ip.into());
                    assert_eq!(got_src_ip, Ipv6::UNSPECIFIED_ADDRESS);
                    assert_eq!(got_dst_ip, dst_ip.get());
                    assert_eq!(ttl, REQUIRED_NDP_IP_PACKET_HOP_LIMIT);
                    assert_eq!(message.target_address(), &LOCAL_IP);
                    assert_eq!(code, IcmpUnusedCode);
                }
            );
        }

        // Send a neighbor solicitation with the test target address to the
        // host.
        let src_ip = remote_mac.to_ipv6_link_local().addr();
        let snmc = target_addr.to_solicited_node_address();
        let dst_ip = snmc.get();
        receive_ip_packet::<_, _, Ipv6>(
            &sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            FrameDestination::Multicast,
            neighbor_solicitation_ip_packet(**src_ip, dst_ip, target_addr, *remote_mac),
        );

        // Check if a neighbor advertisement was sent as a response and the
        // new state of the neighbor table.
        let expected_neighbors = if expect_handle {
            assert_matches!(
                &non_sync_ctx.take_frames()[..],
                [(got_device_id, got_frame)] => {
                    assert_eq!(got_device_id, &link_device_id);

                    let (src_mac, dst_mac, got_src_ip, got_dst_ip, ttl, message, code) =
                        parse_icmp_packet_in_ip_packet_in_ethernet_frame::<
                            Ipv6,
                            _,
                            NeighborAdvertisement,
                            _,
                        >(got_frame, EthernetFrameLengthCheck::NoCheck, |_| {})
                            .unwrap();
                    assert_eq!(src_mac, local_mac.get());
                    assert_eq!(dst_mac, remote_mac.get());
                    assert_eq!(got_src_ip, target_addr);
                    assert_eq!(got_dst_ip, src_ip.into_addr());
                    assert_eq!(ttl, REQUIRED_NDP_IP_PACKET_HOP_LIMIT);
                    assert_eq!(message.target_address(), &target_addr);
                    assert_eq!(code, IcmpUnusedCode);
                }
            );

            HashMap::from([(
                {
                    let src_ip: UnicastAddr<_> = src_ip.into_addr();
                    src_ip.into_specified()
                },
                NeighborState::Dynamic(DynamicNeighborState::Complete {
                    link_address: remote_mac.get(),
                }),
            )])
        } else {
            assert_matches!(&non_sync_ctx.take_frames()[..], []);
            HashMap::default()
        };

        assert_neighbors::<Ipv6, _>(&sync_ctx, &link_device_id, expected_neighbors);
    }

    #[test]
    fn ipv6_integration() {
        let FakeEventDispatcherConfig {
            local_mac,
            remote_mac,
            local_ip: _,
            remote_ip: _,
            subnet: _,
        } = Ipv6::FAKE_CONFIG;

        let crate::testutil::FakeCtx { sync_ctx, mut non_sync_ctx } =
            crate::testutil::FakeCtx::default();
        let sync_ctx = &sync_ctx;
        let eth_device_id = crate::device::add_ethernet_device(
            sync_ctx,
            local_mac,
            IPV6_MIN_IMPLIED_MAX_FRAME_SIZE,
            DEFAULT_INTERFACE_METRIC,
        );
        let device_id = eth_device_id.clone().into();
        // Configure the device to generate a link-local address.
        let _: Ipv6DeviceConfigurationUpdate = update_ipv6_configuration(
            sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            Ipv6DeviceConfigurationUpdate {
                slaac_config: Some(SlaacConfiguration {
                    enable_stable_addresses: true,
                    ..Default::default()
                }),
                ..Default::default()
            },
        )
        .unwrap();
        Ipv6::set_ip_device_enabled(sync_ctx, &mut non_sync_ctx, &device_id, true, false);

        let neighbor_ip = remote_mac.to_ipv6_link_local().addr();
        let neighbor_ip: UnicastAddr<_> = neighbor_ip.into_addr();
        let dst_ip = Ipv6::ALL_NODES_LINK_LOCAL_MULTICAST_ADDRESS.get();
        let na_packet_buf = |solicited_flag, override_flag| {
            neighbor_advertisement_ip_packet(
                *neighbor_ip,
                dst_ip,
                false, /* router_flag */
                solicited_flag,
                override_flag,
                *remote_mac,
            )
        };

        // NeighborAdvertisements should not create a new entry even if
        // the advertisement has both the solicited and override flag set.
        receive_ip_packet::<_, _, Ipv6>(
            &sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            FrameDestination::Multicast,
            na_packet_buf(false, false),
        );
        let link_device_id = device_id.clone().try_into().unwrap();
        assert_neighbors::<Ipv6, _>(&sync_ctx, &link_device_id, Default::default());
        receive_ip_packet::<_, _, Ipv6>(
            &sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            FrameDestination::Multicast,
            na_packet_buf(true, true),
        );
        assert_neighbors::<Ipv6, _>(&sync_ctx, &link_device_id, Default::default());

        assert_eq!(non_sync_ctx.take_frames(), []);

        // Trigger a neighbor solicitation to be sent.
        let body = [u8::MAX];
        let pending_frames = VecDeque::from([Buf::new(body.to_vec(), ..)]);
        assert_matches!(
            BufferNudHandler::<_, Ipv6, EthernetLinkDevice, _>::send_ip_packet_to_neighbor(
                &mut Locked::new(sync_ctx),
                &mut non_sync_ctx,
                &eth_device_id,
                neighbor_ip.into_specified(),
                Buf::new(body, ..),
            ),
            Ok(())
        );
        assert_matches!(
            &non_sync_ctx.take_frames()[..],
            [(got_device_id, got_frame)] => {
                assert_eq!(got_device_id, &eth_device_id);

                let (src_mac, dst_mac, got_src_ip, got_dst_ip, ttl, message, code) = parse_icmp_packet_in_ip_packet_in_ethernet_frame::<
                    Ipv6,
                    _,
                    NeighborSolicitation,
                    _,
                >(got_frame, EthernetFrameLengthCheck::NoCheck, |_| {})
                    .unwrap();
                let target = neighbor_ip;
                let snmc = target.to_solicited_node_address();
                assert_eq!(src_mac, local_mac.get());
                assert_eq!(dst_mac, snmc.into());
                assert_eq!(got_src_ip, local_mac.to_ipv6_link_local().addr().into());
                assert_eq!(got_dst_ip, snmc.get());
                assert_eq!(ttl, 255);
                assert_eq!(message.target_address(), &target.get());
                assert_eq!(code, IcmpUnusedCode);
            }
        );
        assert_neighbors::<Ipv6, _>(
            &sync_ctx,
            &link_device_id,
            HashMap::from([(
                neighbor_ip.into_specified(),
                NeighborState::Dynamic(DynamicNeighborState::Incomplete {
                    transmit_counter: NonZeroU8::new(MAX_MULTICAST_SOLICIT - 1),
                    pending_frames: pending_frames,
                }),
            )]),
        );

        // A Neighbor advertisement should now update the entry.
        receive_ip_packet::<_, _, Ipv6>(
            &sync_ctx,
            &mut non_sync_ctx,
            &device_id,
            FrameDestination::Multicast,
            na_packet_buf(true, true),
        );
        assert_neighbors::<Ipv6, _>(
            &sync_ctx,
            &link_device_id,
            HashMap::from([(
                neighbor_ip.into_specified(),
                NeighborState::Dynamic(DynamicNeighborState::Complete {
                    link_address: remote_mac.get(),
                }),
            )]),
        );
        let frames = non_sync_ctx.take_frames();
        let (got_device_id, got_frame) = assert_matches!(&frames[..], [x] => x);
        assert_eq!(got_device_id, &eth_device_id);

        let (payload, src_mac, dst_mac, ether_type) =
            parse_ethernet_frame(got_frame, EthernetFrameLengthCheck::NoCheck).unwrap();
        assert_eq!(src_mac, local_mac.get());
        assert_eq!(dst_mac, remote_mac.get());
        assert_eq!(ether_type, Some(EtherType::Ipv6));
        assert_eq!(payload, body);

        // Disabling the device should clear the neighbor table.
        Ipv6::set_ip_device_enabled(sync_ctx, &mut non_sync_ctx, &device_id, false, true);
        assert_neighbors::<Ipv6, _>(&sync_ctx, &link_device_id, HashMap::new());
        non_sync_ctx.timer_ctx().assert_no_timers_installed();
    }
}
