// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! An implementation of Linux's Netlink API for Fuchsia.
//!
//! Netlink is a socket-based API provided by Linux that user space applications
//! can use to interact with the kernel. The API is split up into several
//! protocol families each offering different functionality. This crate targets
//! the implementation of families related to networking.

#![deny(missing_docs, unused)]

mod client;
mod errors;
pub mod interfaces;
pub(crate) mod logging;
pub mod messaging;
pub mod multicast_groups;
mod netlink_packet;
pub mod protocol_family;
mod routes;
mod rules;

use fuchsia_async as fasync;
use futures::{
    channel::mpsc::{self, UnboundedReceiver, UnboundedSender},
    future::Future,
    FutureExt as _, StreamExt as _,
};
use net_types::ip::{Ipv4, Ipv6};
use netlink_packet_route::RtnlMessage;

use crate::{
    client::{ClientIdGenerator, ClientTable, InternalClient},
    errors::EventLoopError,
    logging::log_debug,
    messaging::{Receiver, Sender, SenderReceiverProvider},
    protocol_family::{
        route::{NetlinkRoute, NetlinkRouteClient, NetlinkRouteRequestHandler},
        NetlinkFamilyRequestHandler as _, ProtocolFamily,
    },
    rules::RuleTable,
};

/// The tag added to all logs generated by this crate.
pub const NETLINK_LOG_TAG: &'static str = "netlink";

/// The implementation of the Netlink protocol suite.
pub struct Netlink<P: SenderReceiverProvider> {
    /// Generator of new Client IDs.
    id_generator: ClientIdGenerator,
    /// Sender to attach new `NETLINK_ROUTE` clients to the Netlink worker.
    route_client_sender: UnboundedSender<
        ClientWithReceiver<
            NetlinkRoute,
            P::Sender<<NetlinkRoute as ProtocolFamily>::InnerMessage>,
            P::Receiver<<NetlinkRoute as ProtocolFamily>::InnerMessage>,
        >,
    >,
}

impl<P: SenderReceiverProvider> Netlink<P> {
    /// Returns a newly instantiated [`Netlink`] and its asynchronous worker.
    ///
    /// Callers are responsible for polling the worker [`Future`], which drives
    /// the Netlink implementation's asynchronous work. The worker will never
    /// complete.
    pub fn new<H: interfaces::InterfacesHandler>(
        interfaces_handler: H,
    ) -> (Self, impl Future<Output = ()> + Send) {
        let (route_client_sender, route_client_receiver) = mpsc::unbounded();
        (
            Netlink { id_generator: ClientIdGenerator::default(), route_client_sender },
            run_netlink_worker(NetlinkWorkerParams::<_, P> {
                interfaces_handler,
                route_client_receiver,
            }),
        )
    }

    /// Creates a new client of the `NETLINK_ROUTE` protocol family.
    ///
    /// `sender` is used by Netlink to send messages to the client.
    /// `receiver` is used by Netlink to receive messages from the client.
    ///
    /// Closing the `receiver` will close this client, disconnecting `sender`.
    pub fn new_route_client(
        &self,
        sender: P::Sender<RtnlMessage>,
        receiver: P::Receiver<RtnlMessage>,
    ) -> Result<NetlinkRouteClient, NewClientError> {
        let Netlink { id_generator, route_client_sender } = self;
        let (external_client, internal_client) =
            client::new_client_pair::<NetlinkRoute, _>(id_generator.new_id(), sender);
        route_client_sender
            .unbounded_send(ClientWithReceiver { client: internal_client, receiver })
            .map_err(|e| {
                // Sending on an `UnboundedSender` can never fail with `is_full()`.
                debug_assert!(e.is_disconnected());
                NewClientError::Disconnected
            })?;
        Ok(NetlinkRouteClient(external_client))
    }
}

/// A wrapper to hold an [`InternalClient`], and its [`Receiver`] of requests.
struct ClientWithReceiver<
    F: ProtocolFamily,
    S: Sender<F::InnerMessage>,
    R: Receiver<F::InnerMessage>,
> {
    client: InternalClient<F, S>,
    receiver: R,
}

/// The possible error types when instantiating a new client.
pub enum NewClientError {
    /// The [`Netlink`] is disconnected from its associated worker, perhaps as a
    /// result of dropping the worker.
    Disconnected,
}

/// Parameters used to start the Netlink asynchronous worker.
struct NetlinkWorkerParams<H, P: SenderReceiverProvider> {
    interfaces_handler: H,
    /// Receiver of newly created `NETLINK_ROUTE` clients.
    route_client_receiver: UnboundedReceiver<
        ClientWithReceiver<
            NetlinkRoute,
            P::Sender<<NetlinkRoute as ProtocolFamily>::InnerMessage>,
            P::Receiver<<NetlinkRoute as ProtocolFamily>::InnerMessage>,
        >,
    >,
}

/// The worker encompassing all asynchronous Netlink work.
///
/// The worker is never expected to complete.
async fn run_netlink_worker<H: interfaces::InterfacesHandler, P: SenderReceiverProvider>(
    params: NetlinkWorkerParams<H, P>,
) {
    let NetlinkWorkerParams { interfaces_handler, route_client_receiver } = params;

    let route_clients = ClientTable::default();
    let (interfaces_request_sink, interfaces_request_stream) = mpsc::channel(1);
    let (v4_routes_request_sink, v4_routes_request_stream) = mpsc::channel(1);
    let (v6_routes_request_sink, v6_routes_request_stream) = mpsc::channel(1);

    let _: Vec<()> = futures::future::join_all([
        // Accept new NETLINK_ROUTE clients.
        {
            let route_clients = route_clients.clone();
            fasync::Task::spawn(async move {
                connect_new_clients::<NetlinkRoute, _, _>(
                    route_clients,
                    route_client_receiver,
                    NetlinkRouteRequestHandler {
                        interfaces_request_sink,
                        v4_routes_request_sink,
                        v6_routes_request_sink,
                        rules_request_handler: RuleTable::default(),
                    },
                )
                .await;
                panic!("route_client_receiver stream unexpectedly finished")
            })
        },
        // IPv4 Routes Worker.
        {
            let route_clients = route_clients.clone();
            fasync::Task::spawn(async move {
                let worker = match routes::EventLoop::<
                    P::Sender<<NetlinkRoute as ProtocolFamily>::InnerMessage>,
                    Ipv4,
                >::new(route_clients, v4_routes_request_stream)
                {
                    Ok(worker) => worker,
                    Err(EventLoopError::Fidl(e)) => {
                        panic!("Routes event loop creation error: {:?}", e)
                    }
                    Err(EventLoopError::Netstack(_)) => {
                        unreachable!(
                            "The Netstack variant is not returned when creating a new worker"
                        );
                    }
                };
                panic!("Ipv4 routes event loop error: {:?}", worker.run().await);
            })
        },
        // Ipv6 Routes Worker.
        {
            let route_clients = route_clients.clone();
            fasync::Task::spawn(async move {
                let worker = match routes::EventLoop::<
                    P::Sender<<NetlinkRoute as ProtocolFamily>::InnerMessage>,
                    Ipv6,
                >::new(route_clients, v6_routes_request_stream)
                {
                    Ok(worker) => worker,
                    Err(EventLoopError::Fidl(e)) => {
                        panic!("Routes event loop creation error: {:?}", e)
                    }
                    Err(EventLoopError::Netstack(_)) => {
                        unreachable!(
                            "The Netstack variant is not returned when creating a new worker"
                        );
                    }
                };
                panic!("Ipv6 routes event loop error: {:?}", worker.run().await);
            })
        },
        // Interfaces Worker.
        {
            fasync::Task::spawn(async move {
                let worker = match interfaces::EventLoop::new(interfaces_handler, route_clients) {
                    Ok(worker) => worker,
                    Err(EventLoopError::Fidl(e)) => {
                        panic!("Interfaces event loop creation error: {:?}", e)
                    }
                    Err(EventLoopError::Netstack(_)) => {
                        unreachable!(
                            "The Netstack variant is not returned when creating a new worker"
                        );
                    }
                };
                panic!(
                    "Interfaces event loop error: {:?}",
                    worker.run(interfaces_request_stream).await
                );
            })
        },
    ])
    .await;
}

/// Receives clients from the given receiver, adding them to the given table.
///
/// A "Request Handler" Task will be spawned for each received client. The given
/// `request_handler_impl` defines how the requests will be handled.
async fn connect_new_clients<
    F: ProtocolFamily,
    S: Sender<F::InnerMessage>,
    R: Receiver<F::InnerMessage>,
>(
    client_table: ClientTable<F, S>,
    client_receiver: UnboundedReceiver<ClientWithReceiver<F, S, R>>,
    request_handler_impl: F::RequestHandler<S>,
) {
    client_receiver
        // Drive each client concurrently with `for_each_concurrent`. Note that
        // because each client is spawned in a separate Task, they will run in
        // parallel.
        .for_each_concurrent(None, |ClientWithReceiver { client, receiver }| {
            client_table.add_client(client.clone());
            spawn_client_request_handler::<F, S, R>(client, receiver, request_handler_impl.clone())
                .then(|client| futures::future::ready(client_table.remove_client(client)))
        })
        .await
}

/// Spawns a [`Task`] to handle requests from the given client.
///
/// The task terminates when the underlying `Receiver` closes, yielding the
/// original client.
fn spawn_client_request_handler<
    F: ProtocolFamily,
    S: Sender<F::InnerMessage>,
    R: Receiver<F::InnerMessage>,
>(
    client: InternalClient<F, S>,
    receiver: R,
    handler: F::RequestHandler<S>,
) -> fasync::Task<InternalClient<F, S>> {
    // State needed to handle an individual request, that is cycled through the
    // `fold` combinator below.
    struct FoldState<C, H> {
        client: C,
        handler: H,
    }
    fasync::Task::spawn(
        // Use `fold` for two reasons. First, it processes requests serially,
        // ensuring requests are handled in order. Second, it allows us to
        // "hand-off" the client/handler from one request to the other, avoiding
        // copies for each request.
        receiver
            .fold(
                FoldState { client, handler },
                |FoldState { mut client, mut handler }, req| async {
                    log_debug!("{} Received request: {:?}", client, req);
                    handler.handle_request(req, &mut client).await;
                    FoldState { client, handler }
                },
            )
            .map(|FoldState { client, handler: _ }: FoldState<_, _>| client),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    use assert_matches::assert_matches;
    use futures::channel::mpsc;

    use crate::{
        messaging::testutil::SentMessage,
        protocol_family::testutil::{
            new_fake_netlink_message, FakeNetlinkRequestHandler, FakeProtocolFamily,
        },
    };

    #[fasync::run_singlethreaded(test)]
    async fn test_spawn_client_request_handler() {
        let (mut req_sender, req_receiver) = mpsc::channel(0);
        let (mut client_sink, client) = crate::client::testutil::new_fake_client::<
            FakeProtocolFamily,
        >(crate::client::testutil::CLIENT_ID_1, &[]);

        let mut client_task = spawn_client_request_handler::<FakeProtocolFamily, _, _>(
            client,
            req_receiver,
            FakeNetlinkRequestHandler,
        )
        .fuse();

        assert_matches!((&mut client_task).now_or_never(), None);
        assert_eq!(&client_sink.take_messages()[..], &[]);

        // Send a message and expect to see the response on the `client_sink`.
        // NB: Use the sender's channel size as a synchronization method; If a
        // second message could be sent, the first *must* have been handled.
        req_sender.try_send(new_fake_netlink_message()).expect("should send without error");
        let could_send_fut = futures::future::poll_fn(|ctx| req_sender.poll_ready(ctx)).fuse();
        futures::pin_mut!(client_task, could_send_fut);
        futures::select!(
            res = could_send_fut => res.expect("should be able to send without error"),
            _client = client_task => panic!("client task unexpectedly finished"),
        );
        assert_eq!(
            &client_sink.take_messages()[..],
            &[SentMessage::unicast(new_fake_netlink_message())]
        );

        // Close the sender, and expect the Task to exit.
        req_sender.close_channel();
        let _client = client_task.await;
        assert_eq!(&client_sink.take_messages()[..], &[]);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_connect_new_clients() {
        let client_table = ClientTable::default();
        let (client_sender, client_receiver) = futures::channel::mpsc::unbounded();
        let mut client_acceptor_fut = Box::pin(
            connect_new_clients::<FakeProtocolFamily, _, _>(
                client_table.clone(),
                client_receiver,
                FakeNetlinkRequestHandler,
            )
            .fuse(),
        );

        assert_eq!((&mut client_acceptor_fut).now_or_never(), None);

        // Connect Client 1.
        let (mut _client_sink1, client1) = crate::client::testutil::new_fake_client::<
            FakeProtocolFamily,
        >(crate::client::testutil::CLIENT_ID_1, &[]);
        let (mut req_sender1, req_receiver1) = mpsc::channel(0);
        client_sender
            .unbounded_send(ClientWithReceiver { client: client1, receiver: req_receiver1 })
            .expect("should send without error");

        // Connect Client 2.
        let (mut client_sink2, client2) = crate::client::testutil::new_fake_client::<
            FakeProtocolFamily,
        >(crate::client::testutil::CLIENT_ID_2, &[]);
        let (mut req_sender2, req_receiver2) = mpsc::channel(0);
        client_sender
            .unbounded_send(ClientWithReceiver { client: client2, receiver: req_receiver2 })
            .expect("should send without error");

        // Send a request to Client 2, and verify it's handled despite Client 1
        // being open (e.g. concurrent handling of requests across clients).
        // NB: Use the sender's channel size as a synchronization method; If a
        // second message could be sent, the first *must* have been handled.
        req_sender2.try_send(new_fake_netlink_message()).expect("should send without error");
        let could_send_fut = futures::future::poll_fn(|ctx| req_sender2.poll_ready(ctx)).fuse();
        futures::pin_mut!(client_acceptor_fut, could_send_fut);
        futures::select!(
            res = could_send_fut => res.expect("should be able to send without error"),
            () = client_acceptor_fut => panic!("client acceptor unexpectedly finished"),
        );
        assert_eq!(
            &client_table.client_ids()[..],
            [client::testutil::CLIENT_ID_1, client::testutil::CLIENT_ID_2]
        );
        assert_eq!(
            &client_sink2.take_messages()[..],
            &[SentMessage::unicast(new_fake_netlink_message())]
        );

        // Close the two clients, and verify the acceptor fut is still pending.
        req_sender1.close_channel();
        req_sender2.close_channel();
        assert_eq!((&mut client_acceptor_fut).now_or_never(), None);

        // Close the client_sender, and verify the acceptor fut finishes.
        client_sender.close_channel();
        client_acceptor_fut.await;

        // Confirm the clients have been cleaned up from the client table.
        assert_eq!(&client_table.client_ids()[..], []);
    }
}
