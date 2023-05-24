// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! An implementation of Linux's Netlink API for Fuchsia.
//!
//! Netlink is a socket-based API provided by Linux that user space applications
//! can use to interact with the kernel. The API is split up into several
//! protocol families each offering different functionality. This crate targets
//! the implementation of families related to networking.

#![deny(missing_docs)]

mod client;
pub mod messaging;
mod multicast_groups;
pub mod protocol_family;
mod routes;

use fuchsia_async as fasync;
use futures::{
    channel::mpsc::{self, UnboundedReceiver, UnboundedSender},
    future::Future,
    StreamExt as _,
};
use net_types::ip::{Ipv4, Ipv6};
use netlink_packet_core::NetlinkMessage;
use netlink_packet_route::RtnlMessage;

use crate::{
    client::{ClientTable, InternalClient},
    messaging::{Receiver, Sender, SenderReceiverProvider},
    protocol_family::{
        route::{NetlinkRoute, NetlinkRouteClient},
        ProtocolFamily,
    },
    routes::RoutesEventLoopError,
};

/// The tag added to all logs generated by this crate.
pub const NETLINK_LOG_TAG: &'static str = "netlink";

/// The implementation of the Netlink protocol suite.
pub struct Netlink<P: SenderReceiverProvider> {
    /// Sender to attach new `NETLINK_ROUTE` clients to the Netlink worker.
    route_client_sender: UnboundedSender<
        InternalClient<
            NetlinkRoute,
            P::Sender<<NetlinkRoute as ProtocolFamily>::Message>,
            P::Receiver<<NetlinkRoute as ProtocolFamily>::Message>,
        >,
    >,
}

impl<P: SenderReceiverProvider> Netlink<P> {
    /// Returns a newly instantiated [`Netlink`] and its asynchronous worker.
    ///
    /// Callers are responsible for polling the worker [`Future`], which drives
    /// the Netlink implementation's asynchronous work. The worker will never
    /// complete.
    pub fn new() -> (Self, impl Future<Output = ()> + Send) {
        let (route_client_sender, route_client_receiver) = mpsc::unbounded();
        (
            Netlink { route_client_sender },
            run_netlink_worker(NetlinkWorkerParams::<P> { route_client_receiver }),
        )
    }

    /// Creates a new client of the `NETLINK_ROUTE` protocol family.
    ///
    /// `sender` is used by Netlink to send messages to the client.
    /// `receiver` is used by Netlink to receive messages from the client.
    pub fn new_route_client(
        &self,
        sender: P::Sender<NetlinkMessage<RtnlMessage>>,
        receiver: P::Receiver<NetlinkMessage<RtnlMessage>>,
    ) -> Result<NetlinkRouteClient, NewClientError> {
        let (external_client, internal_client) =
            client::new_client_pair::<NetlinkRoute, _, _>(sender, receiver);
        self.route_client_sender.unbounded_send(internal_client).map_err(|e| {
            // Sending on an `UnboundedSender` can never fail with `is_full()`.
            debug_assert!(e.is_disconnected());
            NewClientError::Disconnected
        })?;
        Ok(NetlinkRouteClient(external_client))
    }
}

/// The possible error types when instantiating a new client.
pub enum NewClientError {
    /// The [`Netlink`] is disconnected from its associated worker, perhaps as a
    /// result of dropping the worker.
    Disconnected,
}

/// Parameters used to start the Netlink asynchronous worker.
struct NetlinkWorkerParams<P: SenderReceiverProvider> {
    /// Receiver of newly created `NETLINK_ROUTE` clients.
    route_client_receiver: UnboundedReceiver<
        InternalClient<
            NetlinkRoute,
            P::Sender<<NetlinkRoute as ProtocolFamily>::Message>,
            P::Receiver<<NetlinkRoute as ProtocolFamily>::Message>,
        >,
    >,
}

/// The worker encompassing all asynchronous Netlink work.
///
/// The worker is never expected to complete.
async fn run_netlink_worker<P: SenderReceiverProvider>(params: NetlinkWorkerParams<P>) {
    let NetlinkWorkerParams { route_client_receiver } = params;

    let route_clients = ClientTable::default();
    // TODO(https://github.com/rust-lang/rfcs/issues/2407) Rust does not support
    // cloning into closures, so clone the route_clients ahead of time.
    let clients1 = route_clients.clone();
    let clients2 = route_clients.clone();
    let clients3 = route_clients.clone();

    let _: Vec<()> = futures::future::join_all([
        // Accept new NETLINK_ROUTE clients.
        fasync::Task::spawn(async move {
            connect_new_clients::<NetlinkRoute, _, _>(clients1, route_client_receiver).await;
            panic!("route_client_receiver stream unexpectedly finished")
        }),
        // IPv4 Routes Worker.
        fasync::Task::spawn(async move {
            match routes::EventLoop::<P>::new(clients2).run::<Ipv4>().await {
                RoutesEventLoopError::Fidl(e) | RoutesEventLoopError::Netstack(e) => {
                    panic!("Ipv4 routes event loop error: {:?}", e)
                }
            }
        }),
        // IPv6 Routes Worker.
        fasync::Task::spawn(async move {
            match routes::EventLoop::<P>::new(clients3).run::<Ipv6>().await {
                RoutesEventLoopError::Fidl(e) | RoutesEventLoopError::Netstack(e) => {
                    panic!("Ipv6 routes event loop error: {:?}", e)
                }
            }
        }),
    ])
    .await;
}

/// Receives clients from the given receiver, adding them to the given table.
async fn connect_new_clients<F: ProtocolFamily, S: Sender<F::Message>, R: Receiver<F::Message>>(
    client_table: ClientTable<F, S, R>,
    client_receiver: UnboundedReceiver<InternalClient<F, S, R>>,
) {
    client_receiver.for_each(|client| futures::future::ready(client_table.add_client(client))).await
}
