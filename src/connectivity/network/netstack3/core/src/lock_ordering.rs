// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Lock ordering for Netstack3 core.
//!
//! This module contains the "lock ordering" for Netstack3 core: it describes
//! the order in which additional locks can be acquired while other locks are
//! held. In general, code that is written to avoid deadlocks must respect the
//! same lock ordering.
//!
//! Netstack3 core relies on the [`lock_order`] crate to ensure that all
//! possible code paths respect the lock ordering defined here. By leveraging
//! the types and traits from `lock_order`, we can guarantee at compile time
//! that there are no opportunities for deadlock. The way this works is that
//! each lock in [`SyncCtx`] and its associated per-device state gets assigned a
//! type in this module. Then, for a pair of locks `A` and `B`, where `B` is
//! allowed to be acquired while `A` is locked, there is a corresponding
//! declaration of the [`lock_order::relation::LockAfter`] trait (done via the
//! [`impl_lock_after`] macro).
//!
//! Notionally, the lock ordering forms a [directed acyclic graph (DAG)], where
//! the nodes in the graph are locks that can be acquired, and each edge
//! represents a `LockAfter` implementation. For a lock `B` to be acquired while
//! some lock `A` is held, the node for `B` in the graph must be reachable from
//! `A`, either via direct edge or via an indirect path that traverses
//! other nodes. This can also be thought of as a [strict partial order] between
//! locks, where `A < B` means `B` can be acquired with `A` held.
//!
//! Ideally we'd represent the lock ordering here that way too, but it doesn't
//! work well with the blanket impls of `LockAfter` emitted by the `impl_lock_after` macro.
//! We want to use `impl_lock_after` since it helps prevent deadlocks (see
//! documentation for [`lock_order::relation`]). The problem can be illustrated
//! by this reduced example. Suppose we have a lock ordering DAG like this:
//!
//! ```text
//! ┌────────────────────┐
//! │LoopbackRx          │
//! └┬──────────────────┬┘
//! ┌▽─────────────┐  ┌─▽────────────┐
//! │IpState<Ipv4> │  │IpState<Ipv6> │
//! └┬─────────────┘  └┬─────────────┘
//! ┌▽─────────────────▽┐
//! │DevicesState       │
//! └───────────────────┘
//!```
//!
//! With the blanket `LockAfter` impls, we'd get this:
//! ```no_run
//! impl<X> LockAfter<X> for DevicesState where IpState<Ipv4>>: LockAfter<X> {}
//! // and
//! impl<X> LockAfter<X> for DevicesState where IpState<Ipv6>>: LockAfter<X> {}
//! ```
//!
//! Since `X` could be `LoopbackRx`, we'd have duplicate impls of
//! `LockAfter<LoopbackRx> for DevicesState`.
//!
//! To work around this, we pick a different graph for the lock ordering that
//! won't produce duplicate blanket impls. The property we need is that every
//! path in the original graph is also present in our alternate graph. Luckily,
//! graph theory proves that this is always possible: a [topological sort] has
//! precisely this property, and every DAG has at least one topological sort.
//! For the graph above, that could look something like this:
//!
//! ```text
//! ┌────────────────────┐
//! │LoopbackRx          │
//! └┬───────────────────┘
//! ┌▽─────────────┐
//! │IpState<Ipv4> │
//! └┬─────────────┘
//! ┌▽─────────────┐
//! │IpState<Ipv6> │
//! └┬─────────────┘
//! ┌▽──────────────────┐
//! │DevicesState       │
//! └───────────────────┘
//! ```
//!
//! Note that every possible lock ordering path in the original graph is present
//! (directly or transitively) in the second graph. There are additional paths,
//! like `IpState<Ipv4> -> IpState<Ipv6>`, but those don't matter since
//!   a) if they become load-bearing, they need to be in the original DAG, and
//!   b) load-bearing or not, the result is still a valid lock ordering graph.
//!
//! The lock ordering described in this module is likewise a modification of
//! the ideal graph. Instead of specifying the actual DAG, we make nice to the
//! Rust compiler so we can use the transitive blanket impls instead. Since the
//! graph doesn't have multiple paths between any two nodes (that's what causes
//! the duplicate blanket impls), it ends up being a [multitree]. While we could
//! turn it into a (linear) total ordering, we keep the tree structure so that
//! we can more faithfully represent that some locks can't be held at the same
//! time by putting them on different branches.
//!
//! If, in the future, someone comes up with a better way to declare the lock
//! ordering graph and preserve cycle rejection, we should be able to migrate
//! the definitions here without affecting the usages.
//!
//! [`SyncCtx`]: crate::SyncCtx
//! [directed acyclic graph (DAG)]: https://en.wikipedia.org/wiki/Directed_acyclic_graph
//! [strict partial order]: https://en.wikipedia.org/wiki/Partially_ordered_set#Strict_partial_orders
//! [topological sort]: https://en.wikipedia.org/wiki/Topological_sorting
//! [multitree]: https://en.wikipedia.org/wiki/Multitree

pub(crate) use lock_order::Unlocked;

use core::{convert::Infallible as Never, marker::PhantomData};

use lock_order::{impl_lock_after, relation::LockAfter};
use net_types::ip::{Ipv4, Ipv6};

pub(crate) struct TcpSockets<I>(PhantomData<I>, Never);
pub(crate) struct TcpIsnGenerator<I>(PhantomData<I>, Never);

pub(crate) struct UdpSockets<I>(PhantomData<I>, Never);

pub(crate) enum Ipv4StateNextPacketId {}

// This is not a real lock level, but it is useful for writing bounds that
// require "before IPv4" or "before IPv6".
pub(crate) struct IpState<I>(PhantomData<I>, Never);
pub(crate) struct IpStatePmtuCache<I>(PhantomData<I>, Never);
pub(crate) struct IpStateFragmentCache<I>(PhantomData<I>, Never);
pub(crate) struct IpStateRoutingTable<I>(PhantomData<I>, Never);

pub(crate) enum DeviceLayerStateOrigin {}
pub(crate) enum DeviceLayerState {}
pub(crate) enum AnyDeviceSockets {}
pub(crate) enum DeviceSockets {}
pub(crate) struct EthernetDeviceIpState<I>(PhantomData<I>, Never);
pub(crate) enum EthernetDeviceStaticState {}
pub(crate) enum EthernetDeviceDynamicState {}

pub(crate) enum EthernetIpv4Arp {}
pub(crate) enum EthernetIpv6Nud {}
pub(crate) enum EthernetTxQueue {}
pub(crate) enum EthernetTxDequeue {}

pub(crate) enum LoopbackRxQueue {}
pub(crate) enum LoopbackRxDequeue {}
pub(crate) enum LoopbackTxQueue {}
pub(crate) enum LoopbackTxDequeue {}

impl LockAfter<Unlocked> for LoopbackTxDequeue {}
impl_lock_after!(LoopbackTxDequeue => EthernetTxDequeue);
impl_lock_after!(EthernetTxDequeue => LoopbackRxDequeue);
impl_lock_after!(LoopbackRxDequeue => TcpSockets<Ipv4>);

// Ideally we'd have separate impls `LoopbackRxDequeue => TcpSockets<Ipv4>` and
// for `Ipv6`, but that doesn't play well with the blanket impls. Linearize IPv4
// and IPv6, and TCP and UDP, like for `IpState` below.
impl_lock_after!(TcpSockets<Ipv4> => TcpSockets<Ipv6>);
impl_lock_after!(TcpSockets<Ipv6> => UdpSockets<Ipv4>);
impl_lock_after!(UdpSockets<Ipv4> => UdpSockets<Ipv6>);
impl_lock_after!(UdpSockets<Ipv6> => IpState<Ipv4>);

impl_lock_after!(IpState<Ipv4> => IpStateRoutingTable<Ipv4>);
impl_lock_after!(IpState<Ipv6> => IpStateRoutingTable<Ipv6>);
impl_lock_after!(IpState<Ipv4> => IpStatePmtuCache<Ipv4>);
impl_lock_after!(IpState<Ipv6> => IpStatePmtuCache<Ipv6>);
impl_lock_after!(IpState<Ipv4> => IpStateFragmentCache<Ipv4>);
impl_lock_after!(IpState<Ipv6> => IpStateFragmentCache<Ipv6>);
impl_lock_after!(IpState<Ipv4> => EthernetIpv4Arp);
impl_lock_after!(IpState<Ipv6> => EthernetIpv6Nud);

// Ideally we'd say `IpState<Ipv4> => SomeLowerLock` and then separately
// `IpState<Ipv6> => SomeLowerLock`. The compiler doesn't like that because
// it introduces duplicate blanket impls of `LockAfter<L> for SomeLowerLock`
// that we need to get the transitivity of lock ordering. It's safe to linearize
// IPv4 and IPv6 state access and it lets us continue getting transitivity, so
// we do that here.
impl_lock_after!(IpState<Ipv4> => IpState<Ipv6>);

// The loopback data path operates at L3 so we can enqueue packets without going
// through the device layer lock.
impl_lock_after!(IpState<Ipv6> => LoopbackTxQueue);
impl_lock_after!(IpState<Ipv6> => EthernetTxQueue);
impl_lock_after!(LoopbackTxQueue => LoopbackRxQueue);

impl_lock_after!(IpState<Ipv6> => AnyDeviceSockets);

impl_lock_after!(AnyDeviceSockets => DeviceLayerState);
impl_lock_after!(DeviceLayerState => EthernetDeviceIpState<Ipv4>);
// TODO(https://fxbug.dev/120973): Double-check that locking IPv4 ethernet state
// before IPv6 is correct and won't interfere with dual-stack sockets.
impl_lock_after!(EthernetDeviceIpState<Ipv4> => EthernetDeviceIpState<Ipv6>);
impl_lock_after!(DeviceLayerState => EthernetDeviceDynamicState);
impl_lock_after!(DeviceLayerState => DeviceSockets);
