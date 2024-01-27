// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Error};
use async_utils::stream::{StreamItem, StreamWithEpitaph, WithEpitaph};
use bt_rfcomm::ServerChannel;
use fidl::endpoints::{create_request_stream, ClientEnd};
use fidl_fuchsia_bluetooth::ErrorCode;
use fidl_fuchsia_bluetooth_bredr as bredr;
use fuchsia_async as fasync;
use fuchsia_bluetooth::profile::{
    l2cap_connect_parameters, psm_from_protocol, ChannelParameters, Psm, ServiceDefinition,
};
use fuchsia_bluetooth::types::PeerId;
use fuchsia_inspect_derive::Inspect;
use futures::{
    self, channel::mpsc, future::BoxFuture, select, sink::SinkExt, stream::StreamExt, Future,
    FutureExt,
};
use std::collections::HashSet;
use std::convert::{TryFrom, TryInto};
use tracing::{info, trace, warn};

use crate::fidl_service::Service;
use crate::profile::*;
use crate::rfcomm::RfcommServer;
use crate::types::{AdvertiseParams, ServiceGroup, ServiceGroupHandle, Services};

/// The returned result of a Profile.Advertise request.
enum AdvertiseResult {
    /// The Advertise request needed RFCOMM - the client's event stream is returned.
    EventStream(StreamWithEpitaph<bredr::ConnectionReceiverEventStream, ServiceGroupHandle>),
}

/// A connection request from the upstream server.
#[derive(Debug)]
enum ConnectionEvent {
    /// A normal connection request.
    Request(bredr::ConnectionReceiverRequest),
    /// The upstream server canceled the advertisement, for any reason.
    AdvertisementCanceled,
}

/// The tasks associated with a service advertisement.
struct AdvertiseTasks {
    /// The Profile.Advertise() Future that resolves when advertisement terminates.
    pub adv_task: BoxFuture<'static, ()>,
    /// The relay task used to process incoming connection requests from the upstream
    /// server.
    pub relay_task: fasync::Task<()>,
}

/// The current advertisement status of the `ProfileRegistrar`.
enum AdvertiseStatus {
    /// Currently advertising with a set of tasks processing requests.
    Advertising(AdvertiseTasks),
    /// We are not currently advertising.
    NotAdvertising,
}

/// The ProfileRegistrar handles requests over the `fidl.fuchsia.bluetooth.bredr.Profile` service.
/// Clients can advertise, search, and connect to services.
/// The ProfileRegistrar can be thought of as a relay for clients using Profile. Clients not
/// requesting RFCOMM services will be relayed directly to the upstream server.
///
/// The ProfileRegistrar manages a single RFCOMM advertisement group. If a new client
/// requests to advertise services, the server will unregister the active advertisement, group
/// together all the services, and re-register.
/// The ProfileRegistrar also manages the `RfcommServer` - which is responsible for handling
/// connections over the RFCOMM PSM.
#[derive(Inspect)]
pub struct ProfileRegistrar {
    /// An upstream provider of the Profile service. Typically provided by bt-host.
    profile_upstream: bredr::ProfileProxy,
    /// The `active_registration` is the current processing task for connection requests
    /// from the upstream server.
    active_registration: AdvertiseStatus,
    /// The currently advertised services.
    registered_services: Services,
    /// Sender used to relay connection requests from the upstream server.
    connection_sender: Option<mpsc::Sender<ConnectionEvent>>,
    /// Advertise future relays, which complete Advertise calls that do not interact with
    /// RFCOMM.  These must be polled separately from the handler_fut because of how fidl
    /// asynchronous futures and select! interact.
    advertise_relay_tasks: Vec<fasync::Task<()>>,
    /// The RFCOMM server that handles allocating server channels, incoming
    /// l2cap connections, outgoing l2cap connections, and multiplexing channels.
    #[inspect(forward)]
    rfcomm_server: RfcommServer,
}

impl ProfileRegistrar {
    pub fn new(profile_upstream: bredr::ProfileProxy) -> Self {
        Self {
            profile_upstream,
            active_registration: AdvertiseStatus::NotAdvertising,
            registered_services: Services::new(),
            connection_sender: None,
            advertise_relay_tasks: Vec::new(),
            rfcomm_server: RfcommServer::new(),
        }
    }

    /// Creates and returns a Task representing the running server. The returned task will process
    /// `Profile` and `RfcommTest` requests from the `incoming_services` stream.
    pub fn start(self, incoming_services: mpsc::Receiver<Service>) -> fasync::Task<()> {
        let handler_fut = self.handle_fidl_requests(incoming_services);
        fasync::Task::spawn(handler_fut)
    }

    /// Returns true if the requested `new_psms` do not overlap with the currently registered PSMs.
    fn is_disjoint_psms(&self, new_psms: &HashSet<Psm>) -> bool {
        self.registered_services.psms().is_disjoint(new_psms)
    }

    /// Unregisters all the active services advertised by this server.
    /// This should be called when the upstream server drops the single service advertisement that
    /// this server manages.
    async fn unregister_all_services(&mut self) {
        self.registered_services = Services::new();
        self.rfcomm_server.free_all_server_channels().await;
    }

    /// Unregisters the group of services identified by `handle`. Re-registers any remaining
    /// services.
    /// This should be called when a profile client decides to stop advertising its services.
    async fn unregister_service(&mut self, handle: ServiceGroupHandle) -> Result<(), Error> {
        if !self.registered_services.contains(handle) {
            return Err(format_err!("Attempt to unregister non-existent service: {:?}", handle));
        }

        // Remove the entry for this client.
        let service_info = self.registered_services.remove(handle);
        self.rfcomm_server.free_server_channels(service_info.allocated_server_channels()).await;

        // Attempt to re-advertise.
        self.refresh_advertisement().await;
        Ok(())
    }

    /// Processes an incoming L2cap connection from the upstream server.
    ///
    /// If the connection PSM is not RFCOMM, relays directly to the client.
    ///
    /// Returns an error if the `protocol` is invalidly formatted, or if the provided
    /// PSM is not represented by a client of the `ProfileRegistrar`.
    fn handle_incoming_l2cap_connection(
        &mut self,
        peer_id: PeerId,
        channel: bredr::Channel,
        protocol: Vec<bredr::ProtocolDescriptor>,
    ) -> Result<(), Error> {
        trace!(%peer_id, "Received incoming L2CAP connection request {protocol:?}");
        let local = protocol.iter().map(|p| p.into()).collect();
        match psm_from_protocol(&local).ok_or(format_err!("No PSM provided"))? {
            Psm::RFCOMM => self.rfcomm_server.new_l2cap_connection(peer_id, channel.try_into()?),
            psm => {
                match self.registered_services.iter().find(|(_, client)| client.contains_psm(psm)) {
                    Some((_, client)) => client.relay_connected(peer_id.into(), channel, protocol),
                    None => {
                        return Err(format_err!(
                            "Connection request for non-advertised PSM {:?}",
                            psm
                        ))
                    }
                }
            }
        }
    }

    /// Validates that there is an active connection with the peer specified by `peer_id`. If not,
    /// creates and delivers the L2CAP connection to the RFCOMM server.
    async fn ensure_service_connection(&mut self, peer_id: PeerId) -> Result<(), ErrorCode> {
        if self.rfcomm_server.is_active_session(&peer_id) {
            return Ok(());
        }

        let l2cap_channel = match self
            .profile_upstream
            .connect(&peer_id.into(), &l2cap_connect_parameters(Psm::RFCOMM))
            .await
        {
            Ok(Ok(channel)) => channel.try_into().unwrap(),
            Ok(Err(e)) => {
                return Err(e);
            }
            Err(e) => {
                warn!(%peer_id, "Couldn't establish L2CAP connection {e:?}");
                return Err(ErrorCode::Failed);
            }
        };
        self.rfcomm_server
            .new_l2cap_connection(peer_id, l2cap_channel)
            .map_err(|_| ErrorCode::Failed)
    }

    /// Processes an outgoing L2Cap connection initiated by a client of the ProfileRegistrar.
    ///
    /// Returns an error if the connection request fails.
    async fn handle_outgoing_connection(
        &mut self,
        peer_id: PeerId,
        connection: bredr::ConnectParameters,
        responder: bredr::ProfileConnectResponder,
    ) -> Result<(), Error> {
        trace!(%peer_id, "Making outgoing connection request {connection:?}");
        // If the provided `connection` is for a non-RFCOMM PSM, simply forward the outbound
        // connection to the upstream Profile service.
        // Otherwise, route to the RFCOMM server.
        match &connection {
            bredr::ConnectParameters::L2cap { .. } => {
                let result = self
                    .profile_upstream
                    .connect(&peer_id.into(), &connection)
                    .await
                    .unwrap_or_else(|_fidl_error| Err(ErrorCode::Failed));
                let _ = responder.send(result);
            }
            bredr::ConnectParameters::Rfcomm(bredr::RfcommParameters { channel, .. }) => {
                let server_channel = match channel.map(ServerChannel::try_from) {
                    Some(Ok(sc)) => sc,
                    _ => {
                        let _ = responder.send(Err(ErrorCode::InvalidArguments));
                        return Ok(());
                    }
                };

                // Ensure there is an RFCOMM Session between us and the peer.
                if let Err(e) = self.ensure_service_connection(peer_id).await {
                    let _ = responder.send(Err(e));
                    return Ok(());
                }
                // Open the RFCOMM channel.
                self.rfcomm_server.open_rfcomm_channel(peer_id, server_channel, responder).await?;
            }
        }
        Ok(())
    }

    /// Advertises `params` to the provided `profile_upstream`.
    ///
    /// Returns a Future for the advertisement; this future should be polled in order to detect
    /// when the advertisement has finished.
    fn advertise(
        profile_upstream: bredr::ProfileProxy,
        params: AdvertiseParams,
        connect_client: ClientEnd<bredr::ConnectionReceiverMarker>,
    ) -> impl Future<Output = ()> {
        let fidl_services = params
            .services
            .iter()
            .map(bredr::ServiceDefinition::try_from)
            .collect::<Result<Vec<_>, _>>()
            .unwrap();
        profile_upstream
            .advertise(&fidl_services, &(&params.parameters).try_into().unwrap(), connect_client)
            .map(|_| ())
    }

    /// Processes requests from the ConnectionReceiver stream and relays to the `sender`.
    async fn connection_request_relay(
        mut connect_requests: bredr::ConnectionReceiverRequestStream,
        mut sender: mpsc::Sender<ConnectionEvent>,
    ) {
        while let Some(connect_request) = connect_requests.next().await {
            match connect_request {
                Ok(request) => {
                    let _ = sender.send(ConnectionEvent::Request(request)).await;
                }
                Err(e) => info!("Connection request error: {e:?}"),
            }
        }
        // The upstream server has dropped the ConnectionReceiver. Let the
        // receiver know that the advertisement has been canceled.
        let _ = sender.send(ConnectionEvent::AdvertisementCanceled).await;
    }

    /// Attempts to build and advertise services from `self.registered_services`.
    async fn refresh_advertisement(&mut self) {
        let status =
            std::mem::replace(&mut self.active_registration, AdvertiseStatus::NotAdvertising);
        match status {
            AdvertiseStatus::Advertising(AdvertiseTasks { adv_task, relay_task }) => {
                // If we are currently advertising, drop the stream processing task to unregister
                // the services. Wait for the advertisement to resolve before attempting to
                // re-advertise.
                drop(relay_task);
                let _ = adv_task.await;
                trace!("Finished waiting for unregistration");
            }
            AdvertiseStatus::NotAdvertising => {}
        }

        // We are ready to advertise. Attempt to build the advertisement parameters, and create
        // and save two tasks that 1) Make the Advertise request and wait for termination and
        // 2) Process incoming requests from the upstream server.
        if let Some(params) = self.registered_services.build_registration() {
            trace!("Advertising from registered services: {:?}", params);
            let (connect_client, connect_requests) =
                create_request_stream::<bredr::ConnectionReceiverMarker>().unwrap();
            // Spawn a task to advertise `params`.
            let adv_fut =
                ProfileRegistrar::advertise(self.profile_upstream.clone(), params, connect_client);
            let adv_task = adv_fut.boxed();
            // Spawn a task to handle incoming L2CAP connections.
            let relay_task = fasync::Task::spawn(ProfileRegistrar::connection_request_relay(
                connect_requests,
                self.connection_sender.clone().unwrap(),
            ));

            self.active_registration =
                AdvertiseStatus::Advertising(AdvertiseTasks { adv_task, relay_task });
        }
    }

    /// Handles an incoming request to advertise a group of `services`.
    ///
    /// At least one service in `services` must request RFCOMM.
    ///
    /// The RFCOMM-requesting services are assigned ServerChannels. The services are then
    /// registered together with the currently registered services.
    ///
    /// Returns the event stream for the receiver tagged with a unique identifier for the
    /// registered group of services. The event stream should be continuously polled in
    /// order to detect when the client terminates the advertisement.
    async fn add_managed_advertisement(
        &mut self,
        mut services: Vec<ServiceDefinition>,
        parameters: ChannelParameters,
        receiver: bredr::ConnectionReceiverProxy,
        responder: bredr::ProfileAdvertiseResponder,
    ) -> Result<StreamWithEpitaph<bredr::ConnectionReceiverEventStream, ServiceGroupHandle>, Error>
    {
        // Validate that the new PSMs are disjoint because we unregister and re-register as a group.
        let new_psms = psms_from_service_definitions(&services);
        if !self.is_disjoint_psms(&new_psms) {
            let _ = responder.send(Err(ErrorCode::Failed));
            return Err(format_err!("New advertisement requesting pre-allocated PSMs"));
        }

        // Create an entry for this group of services with a unique handle.
        let next_handle =
            self.registered_services.insert(ServiceGroup::new(receiver.clone(), parameters));

        // If the RfcommServer has enough free Server Channels, allocate and update
        // the RFCOMM-requesting services.
        let required_server_channels =
            services.iter().filter(|def| is_rfcomm_service_definition(def)).count();
        if required_server_channels > self.rfcomm_server.available_server_channels().await {
            let _ = responder.send(Err(ErrorCode::Failed));
            return Err(format_err!("RfcommServer not enough free Server Channels"));
        }
        for mut service in services.iter_mut().filter(|def| is_rfcomm_service_definition(def)) {
            let server_channel = self
                .rfcomm_server
                .allocate_server_channel(receiver.clone())
                .await
                .expect("just checked");
            update_svc_def_with_server_channel(&mut service, server_channel)?;
        }

        let service_info = self.registered_services.get_mut(next_handle).expect("just inserted");
        service_info.set_service_defs(services);
        service_info.set_responder(responder);

        // Attempt to re-advertise the updated services.
        self.refresh_advertisement().await;

        Ok(receiver.take_event_stream().with_epitaph(next_handle))
    }

    /// Makes a Profile.Advertise() request upstream, and returns a Future that relays the
    /// result to the `responder` upon termination.
    fn make_advertise_relay(
        &self,
        services: Vec<bredr::ServiceDefinition>,
        parameters: bredr::ChannelParameters,
        receiver: ClientEnd<bredr::ConnectionReceiverMarker>,
        responder: bredr::ProfileAdvertiseResponder,
    ) -> impl Future<Output = ()> {
        let adv_fut = self.profile_upstream.advertise(&services, &parameters, receiver);
        async move {
            let _ = adv_fut
                .await
                .and_then(|r| responder.send(r))
                .map_err(|e| trace!("Relayed advertisement terminated: {e:?}"));
        }
    }

    /// Handles a request over the Profile protocol.
    ///
    /// If the request was an advertisement, returns either the event stream associated with
    /// the advertise request, or a future to relay the advertisement request directly upstream.
    async fn handle_profile_request(
        &mut self,
        request: bredr::ProfileRequest,
    ) -> Option<AdvertiseResult> {
        match request {
            bredr::ProfileRequest::Advertise { services, parameters, receiver, responder } => {
                let services_local = services
                    .iter()
                    .map(ServiceDefinition::try_from)
                    .collect::<Result<Vec<_>, _>>()
                    .ok()?;
                trace!("Received advertise request: {:?}", services_local);
                if service_definitions_request_rfcomm(&services_local) {
                    let receiver = receiver.into_proxy().ok()?;
                    let parameters = ChannelParameters::try_from(&parameters).ok()?;
                    match self
                        .add_managed_advertisement(services_local, parameters, receiver, responder)
                        .await
                    {
                        Err(e) => warn!("Error handling advertise request: {e:?}"),
                        Ok(evt_stream) => return Some(AdvertiseResult::EventStream(evt_stream)),
                    }
                } else {
                    let relay_fut =
                        self.make_advertise_relay(services, parameters, receiver, responder);
                    self.advertise_relay_tasks.push(fasync::Task::spawn(relay_fut));
                    return None;
                }
            }
            bredr::ProfileRequest::Connect { peer_id, connection, responder, .. } => {
                let id = peer_id.into();
                if let Err(e) = self.handle_outgoing_connection(id, connection, responder).await {
                    warn!(%id, "Error establishing outgoing connection {e:?}");
                }
            }
            bredr::ProfileRequest::Search { service_uuid, attr_ids, results, .. } => {
                // Simply forward over the search to the Profile server.
                let _ = self.profile_upstream.search(service_uuid, &attr_ids, results);
            }
            bredr::ProfileRequest::ConnectSco { peer_id, initiator, params, receiver, .. } => {
                let _ = self.profile_upstream.connect_sco(&peer_id, initiator, &params, receiver);
            }
        }
        None
    }

    /// Handles incoming connection requests from the processing task of the active service
    /// advertisement.
    ///
    /// There are two relevant cases:
    ///   1) A connection request from the upstream Host Server. The incoming l2cap connection
    ///      must be handled - if PSM_RFCOMM, send to the RfcommServer, otherwise, relay
    ///      directly to the profile client.
    ///   2) An epitaph of the relay task signaling the advertisement has canceled. This is
    ///      usually due to an error in the upstream server. We must clear all of the services.
    async fn handle_connection_request(&mut self, request: ConnectionEvent) -> Result<(), Error> {
        match request {
            ConnectionEvent::Request(request) => {
                let bredr::ConnectionReceiverRequest::Connected {
                    peer_id, channel, protocol, ..
                } = request;
                self.handle_incoming_l2cap_connection(peer_id.into(), channel, protocol)
            }
            ConnectionEvent::AdvertisementCanceled => {
                // The upstream server unexpectedly dropped the advertisement. We must clean up
                // all of the state.
                self.unregister_all_services().await;
                Ok(())
            }
        }
    }

    /// Processes incoming service requests and events generated by clients of the
    /// `Profile` protocol.
    pub async fn handle_fidl_requests(mut self, mut incoming_services: mpsc::Receiver<Service>) {
        // `bredr.Profile` FIDL requests.
        let mut profile_requests = futures::stream::SelectAll::new();
        // `RfcommTest` FIDL requests.
        let mut test_requests = futures::stream::SelectAll::new();

        // Internal channel used to relay requests over the `ConnectionReceiver` protocol.
        let (connection_sender, mut connection_receiver) = mpsc::channel(0);
        self.connection_sender = Some(connection_sender);

        // All client service advertisement event streams. Used to determine when the
        // client wants to stop advertising its services.
        let mut client_event_streams = futures::stream::SelectAll::new();

        loop {
            select! {
                new_service = incoming_services.select_next_some() => {
                    match new_service {
                        Service::Profile(st) => profile_requests.push(st),
                        Service::RfcommTest(st) => test_requests.push(st),
                    }
                }
                profile_request = profile_requests.select_next_some() => {
                    let profile_request = match profile_request {
                        Ok(request) => request,
                        Err(e) => {
                            info!("Error from Profile request: {e:?}");
                            continue;
                        }
                    };
                    match self.handle_profile_request(profile_request).await {
                        Some(AdvertiseResult::EventStream(evt_stream)) => client_event_streams.push(evt_stream),
                        _ => {},
                    }
                }
                connection_request = connection_receiver.select_next_some() => {
                    // Incoming connection request from the upstream Profile server.
                    if let Err(e) = self.handle_connection_request(connection_request).await {
                        warn!("Error processing incoming l2cap connection request: {e:?}");
                    }
                }
                service_event = client_event_streams.next() => {
                    // `Profile` client has terminated their service advertisement.
                    if let Some(StreamItem::Epitaph(service_id)) = service_event {
                        // Unregister the service from the ProfileRegistrar.
                        info!("Client {service_id:?} unregistered service advertisement");
                        if let Err(e) = self.unregister_service(service_id).await {
                            warn!("Error unregistering service {service_id:?}: {e:?}");
                        }
                    }
                }
                request = test_requests.select_next_some() => {
                    match request {
                        Ok(req) => self.rfcomm_server.handle_test_request(req).await,
                        Err(e) => warn!("RfcommTest request is error: {e:?}"),
                    }
                }
                complete => break,
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use crate::types::tests::{other_service_definition, rfcomm_service_definition};

    use async_utils::PollExt;
    use fidl::endpoints::create_proxy_and_stream;
    use futures::pin_mut;
    use futures::task::{Context, Poll};
    use futures_test::task::new_count_waker;

    /// Returns true if the provided `service` has an assigned Server Channel.
    fn service_def_has_assigned_server_channel(service: &bredr::ServiceDefinition) -> bool {
        if let Some(protocol) = &service.protocol_descriptor_list {
            for descriptor in protocol {
                if descriptor.protocol == bredr::ProtocolIdentifier::Rfcomm {
                    if descriptor.params.is_empty() {
                        return false;
                    }
                    // The server channel should be the first element.
                    if let bredr::DataElement::Uint8(_) = descriptor.params[0] {
                        return true;
                    }
                }
            }
        }
        false
    }

    /// Creates a Profile::Search request.
    fn generate_search_request(
        exec: &mut fasync::TestExecutor,
    ) -> (bredr::ProfileRequest, bredr::SearchResultsRequestStream) {
        let (c, mut s) = create_proxy_and_stream::<bredr::ProfileMarker>().unwrap();
        let (results, server) = create_request_stream::<bredr::SearchResultsMarker>().unwrap();

        assert!(c.search(bredr::ServiceClassProfileIdentifier::AudioSink, &[], results).is_ok());

        match exec.run_until_stalled(&mut s.next()) {
            Poll::Ready(Some(Ok(x))) => (x, server),
            x => panic!("Expected ProfileRequest but got: {:?}", x),
        }
    }

    /// Creates a Profile::ConnectSco request.
    fn generate_connect_sco_request(
        exec: &mut fasync::TestExecutor,
    ) -> (bredr::ProfileRequest, bredr::ScoConnectionReceiverRequestStream) {
        let (profile_proxy, mut profile_request_stream) =
            create_proxy_and_stream::<bredr::ProfileMarker>().unwrap();
        let (receiver_client, receiver_server) =
            create_request_stream::<bredr::ScoConnectionReceiverMarker>().unwrap();

        assert!(profile_proxy
            .connect_sco(
                &PeerId(1).into(),
                /*initiator=*/ true,
                &[bredr::ScoConnectionParameters::default()],
                receiver_client
            )
            .is_ok());

        match exec.run_until_stalled(&mut profile_request_stream.next()) {
            Poll::Ready(Some(Ok(request))) => (request, receiver_server),
            x => panic!("Expected ProfileRequest but got: {:?}", x),
        }
    }

    /// Creates a Profile::Advertise request.
    /// Returns the associated request stream, and the Advertise request as a Future.
    fn make_advertise_request(
        client: &bredr::ProfileProxy,
        services: Vec<bredr::ServiceDefinition>,
    ) -> (
        bredr::ConnectionReceiverRequestStream,
        impl Future<Output = Result<Result<(), ErrorCode>, fidl::Error>>,
    ) {
        let (connection, connection_stream) =
            create_request_stream::<bredr::ConnectionReceiverMarker>().unwrap();
        let adv_fut = client.advertise(&services, &bredr::ChannelParameters::default(), connection);
        (connection_stream, adv_fut)
    }

    /// Creates a Profile::Connect request for an L2cap channel.
    /// Returns the associated result future
    fn make_l2cap_connection_request(
        client: &bredr::ProfileProxy,
        peer_id: PeerId,
        psm: u16,
    ) -> impl Future<Output = Result<Result<bredr::Channel, ErrorCode>, fidl::Error>> {
        client.connect(&peer_id.into(), &l2cap_connect_parameters(Psm::new(psm)))
    }

    fn new_client(
        exec: &mut fasync::TestExecutor,
        mut service_sender: mpsc::Sender<Service>,
    ) -> bredr::ProfileProxy {
        let (profile_client, profile_server) =
            create_proxy_and_stream::<bredr::ProfileMarker>().unwrap();
        let send_fut = service_sender.send(Service::Profile(profile_server));
        pin_mut!(send_fut);
        let _ = exec.run_until_stalled(&mut send_fut);
        profile_client
    }

    /// Starts the `ProfileRegistrar` processing task.
    fn setup_handler_fut(
        server: ProfileRegistrar,
    ) -> (mpsc::Sender<Service>, impl Future<Output = ()>) {
        let (service_sender, service_receiver) = mpsc::channel(0);
        let handler_fut = server.handle_fidl_requests(service_receiver);
        (service_sender, handler_fut)
    }

    /// Creates the ProfileRegistrar with the upstream Profile service.
    fn setup_server() -> (fasync::TestExecutor, ProfileRegistrar, bredr::ProfileRequestStream) {
        let exec = fasync::TestExecutor::new();
        let (client, server) = create_proxy_and_stream::<bredr::ProfileMarker>().unwrap();
        let profile_server = ProfileRegistrar::new(client);
        (exec, profile_server, server)
    }

    /// Exercises a service advertisement with an empty set of services.
    /// In practice, the upstream Host Server will reject this request but the RFCOMM
    /// server will still classify the request as non-RFCOMM only, and relay directly
    /// to the Profile Server.
    /// This test validates that the parameters are relayed directly to the Profile Server. Also
    /// validates that when the upstream Advertise call resolves, the result is relayed to the
    /// client.
    #[fuchsia::test]
    fn test_handle_empty_advertise_request() {
        let (mut exec, server, mut upstream_requests) = setup_server();

        let (service_sender, handler_fut) = setup_handler_fut(server);
        pin_mut!(handler_fut);
        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // A new client connects to `bt-rfcomm`.
        let client = {
            let client = new_client(&mut exec, service_sender.clone());
            let _ = exec.run_until_stalled(&mut handler_fut);
            client
        };

        // Client decides to advertise empty services.
        let services = vec![];
        let (_connection_stream, adv_fut) = make_advertise_request(&client, services);
        pin_mut!(adv_fut);
        exec.run_until_stalled(&mut adv_fut).expect_pending("should still be advertising");

        let _ = exec.run_until_stalled(&mut handler_fut);

        // The advertisement request should be relayed directly upstream.
        let responder = match exec.run_until_stalled(&mut upstream_requests.next()) {
            Poll::Ready(Some(Ok(bredr::ProfileRequest::Advertise { responder, .. }))) => responder,
            x => panic!("Expected advertise request, got: {:?}", x),
        };

        // Upstream decides to resolve the Advertise call.
        let _ = responder.send(Ok(()));

        let _ = exec.run_until_stalled(&mut handler_fut);
        // Client should be notified, and it's advertisement should terminate.
        assert!(exec.run_until_stalled(&mut adv_fut).is_ready());
    }

    /// Exercises a service advertisement with no RFCOMM services.
    /// The ProfileRegistrar server should classify the request as non-RFCOMM only, and relay
    /// directly to the upstream Profile Server.
    #[fuchsia::test]
    fn test_handle_advertise_request_with_no_rfcomm() -> Result<(), Error> {
        let (mut exec, server, mut upstream_requests) = setup_server();

        let (service_sender, handler_fut) = setup_handler_fut(server);
        pin_mut!(handler_fut);
        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // A new client connects to bt-rfcomm.
        let client = {
            let client = new_client(&mut exec, service_sender.clone());
            let _ = exec.run_until_stalled(&mut handler_fut);
            client
        };

        // Client decides to advertise.
        let services =
            vec![bredr::ServiceDefinition::try_from(&other_service_definition(Psm::new(1)))?];
        let (_connection_stream, adv_fut) = make_advertise_request(&client, services);
        pin_mut!(adv_fut);
        exec.run_until_stalled(&mut adv_fut).expect_pending("should be advertising");

        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // The advertisement request should be relayed directly upstream.
        match exec.run_until_stalled(&mut upstream_requests.next()) {
            Poll::Ready(Some(Ok(bredr::ProfileRequest::Advertise { .. }))) => {}
            x => panic!("Expected advertise request, got: {:?}", x),
        };
        Ok(())
    }

    /// Exercises connecting l2cap channels while advertising.
    /// The ProfileRegistrar should classify the request as non-RFCOMM, relaying the advertisement
    /// directly, and also relay all l2cap channel requests at the same time.
    #[fuchsia::test]
    fn test_connect_l2cap_channels_while_advertising() {
        let (mut exec, server, mut upstream_requests) = setup_server();

        let (service_sender, handler_fut) = setup_handler_fut(server);
        pin_mut!(handler_fut);
        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // A new client connects to bt-rfcomm.
        let client = {
            let client = new_client(&mut exec, service_sender.clone());
            let _ = exec.run_until_stalled(&mut handler_fut);
            client
        };

        // Client decides to advertise.
        let services =
            vec![
                bredr::ServiceDefinition::try_from(&other_service_definition(Psm::new(1))).unwrap()
            ];
        let (_connection_stream, adv_fut) = make_advertise_request(&client, services);
        pin_mut!(adv_fut);
        exec.run_until_stalled(&mut adv_fut).expect_pending("should still be advertising");

        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // The advertisement request should be relayed directly upstream.
        let _adv_responder = match exec.run_until_stalled(&mut upstream_requests.next()) {
            Poll::Ready(Some(Ok(bredr::ProfileRequest::Advertise { responder, .. }))) => responder,
            x => panic!("Expected advertise request, got: {:?}", x),
        };

        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");
        exec.run_until_stalled(&mut adv_fut).expect_pending("should still be advertising");

        // Connect an l2cap channel to a peer, which should be relayed directly upstream.

        let expected_peer_id = PeerId(1);
        let connect_fut =
            make_l2cap_connection_request(&client, expected_peer_id, bredr::PSM_AVDTP);
        pin_mut!(connect_fut);

        let _ = exec.run_until_stalled(&mut handler_fut);
        // The advertisement request should be relayed directly upstream.
        let connect_responder = match exec.run_until_stalled(&mut upstream_requests.next()) {
            Poll::Ready(Some(Ok(bredr::ProfileRequest::Connect {
                responder, peer_id, ..
            }))) => {
                assert_eq!(peer_id, expected_peer_id.into());
                responder
            }
            x => panic!("Expected connect request, got: {:?}", x),
        };

        let (waker, fut_wake_count) = new_count_waker();
        let mut counting_ctx = Context::from_waker(&waker);

        // Polling the handler should register the waker for this connect response.
        handler_fut
            .as_mut()
            .poll(&mut counting_ctx)
            .expect_pending("shouldn't be done while service_sender is live");

        let woke_count_before = fut_wake_count.get();

        // Send a response from the upstream Profile request
        connect_responder.send(Err(ErrorCode::TimedOut)).expect("upstream connect response");

        // Run all the other background tasks that got awoke.
        // This signals counting_ctx's waker because the advertising task is woken bv the send,
        // which will read from the FIDL channel and wake anyone who received a message.
        let _ = exec.run_until_stalled(&mut futures::future::pending::<()>());

        // Should be woken now
        let woke_count_after = fut_wake_count.get();
        assert!(
            woke_count_after > woke_count_before,
            "connection didn't get woken: {} <= {}",
            woke_count_after,
            woke_count_before
        );

        // Polling again should return the result.
        handler_fut
            .as_mut()
            .poll(&mut counting_ctx)
            .expect_pending("shouldn't be done while service_sender is live");
        match exec.run_until_stalled(&mut connect_fut) {
            Poll::Ready(Ok(Err(ErrorCode::TimedOut))) => {}
            x => panic!("Expected connect request to be complete, got {:?}", x),
        }
    }

    /// Service advertisement with only RFCOMM services. The services should be assigned
    /// Server Channels and be relayed upstream.
    #[fuchsia::test]
    fn test_handle_advertise_request_with_only_rfcomm() -> Result<(), Error> {
        let (mut exec, server, mut upstream_requests) = setup_server();

        let (service_sender, handler_fut) = setup_handler_fut(server);
        pin_mut!(handler_fut);
        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // A new client connects to bt-rfcomm.
        let client = {
            let client = new_client(&mut exec, service_sender.clone());
            let _ = exec.run_until_stalled(&mut handler_fut);
            client
        };

        // Client decides to advertise.
        let services = vec![bredr::ServiceDefinition::try_from(&rfcomm_service_definition(None))?];
        let (_connection_stream, adv_fut) = make_advertise_request(&client, services);
        pin_mut!(adv_fut);
        exec.run_until_stalled(&mut adv_fut).expect_pending("should still be advertising");

        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // Upstream should receive Advertise request for a service with an assigned server channel.
        match exec.run_until_stalled(&mut upstream_requests.next()) {
            Poll::Ready(Some(Ok(bredr::ProfileRequest::Advertise { services, .. }))) => {
                assert_eq!(services.len(), 1);
                assert!(service_def_has_assigned_server_channel(&services[0]));
            }
            x => panic!("Expected advertise request, got: {:?}", x),
        };
        Ok(())
    }

    /// Service advertisement with both RFCOMM and non-RFCOMM services. Only the RFCOMM
    /// services should be assigned Server Channels, and the group should be registered upstream.
    #[fuchsia::test]
    fn test_handle_advertise_request_with_all_services() -> Result<(), Error> {
        let (mut exec, server, mut upstream_requests) = setup_server();

        let (service_sender, handler_fut) = setup_handler_fut(server);
        pin_mut!(handler_fut);
        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // A new client connects to bt-rfcomm.
        let client = {
            let client = new_client(&mut exec, service_sender.clone());
            let _ = exec.run_until_stalled(&mut handler_fut);
            client
        };

        // Client decides to advertise.
        let services = vec![
            bredr::ServiceDefinition::try_from(&rfcomm_service_definition(None))?,
            bredr::ServiceDefinition::try_from(&other_service_definition(Psm::new(10)))?,
            bredr::ServiceDefinition::try_from(&rfcomm_service_definition(None))?,
        ];
        let n = services.len();
        let (_connection_stream, adv_fut) = make_advertise_request(&client, services);
        pin_mut!(adv_fut);
        exec.run_until_stalled(&mut adv_fut).expect_pending("should still be advertising");

        let _ = exec.run_until_stalled(&mut handler_fut);

        // Expect an advertise request with all n services - validate that the RFCOMM services
        // have assigned server channels.
        match exec.run_until_stalled(&mut upstream_requests.next()) {
            Poll::Ready(Some(Ok(bredr::ProfileRequest::Advertise { services, .. }))) => {
                assert_eq!(services.len(), n);
                let res = services
                    .into_iter()
                    .filter(|service| {
                        is_rfcomm_service_definition(&ServiceDefinition::try_from(service).unwrap())
                    })
                    .map(|service| service_def_has_assigned_server_channel(&service))
                    .fold(true, |acc, elt| acc || elt);
                assert!(res);
            }
            x => panic!("Expected advertise request, got: {:?}", x),
        }
        Ok(())
    }

    /// Tests handling two advertise requests with overlapping PSMs. The first one
    /// should succeed and be relayed upstream. The second one should fail since it
    /// is requesting already-allocated PSMs - the responder should be notified of the error.
    #[fuchsia::test]
    fn test_handle_advertise_requests_same_psm() -> Result<(), Error> {
        let (mut exec, server, mut upstream_requests) = setup_server();

        let (service_sender, handler_fut) = setup_handler_fut(server);
        pin_mut!(handler_fut);
        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // A new client connects to bt-rfcomm.
        let client1 = {
            let client = new_client(&mut exec, service_sender.clone());
            let _ = exec.run_until_stalled(&mut handler_fut);
            client
        };

        // Client decides to advertise.
        let psm = Psm::new(10);
        let services1 = vec![
            bredr::ServiceDefinition::try_from(&other_service_definition(psm))?,
            bredr::ServiceDefinition::try_from(&rfcomm_service_definition(None))?,
        ];
        let (_connection_stream1, adv_fut1) = make_advertise_request(&client1, services1);
        pin_mut!(adv_fut1);
        exec.run_until_stalled(&mut adv_fut1).expect_pending("should still be advertising");

        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // Save the Advertise parameters.
        let _adv_req1 = match exec.run_until_stalled(&mut upstream_requests.next()) {
            Poll::Ready(Some(Ok(request))) => request,
            x => panic!("Expected advertise request, got: {:?}", x),
        };

        // A different client connects to bt-rfcomm. It decides to try to advertise over same PSM.
        let client2 = {
            let client = new_client(&mut exec, service_sender.clone());
            let _ = exec.run_until_stalled(&mut handler_fut);
            client
        };
        let services2 = vec![
            bredr::ServiceDefinition::try_from(&other_service_definition(psm))?,
            bredr::ServiceDefinition::try_from(&rfcomm_service_definition(None))?,
        ];
        let (_connection_stream2, adv_fut2) = make_advertise_request(&client2, services2);
        pin_mut!(adv_fut2);
        exec.run_until_stalled(&mut adv_fut2).expect_pending("should should be advertising");

        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // Advertisement 1 is OK. Advertisement 2 should resolve immediately with an ErrorCode.
        exec.run_until_stalled(&mut adv_fut1).expect_pending("should still be advertising");
        match exec.run_until_stalled(&mut adv_fut2) {
            Poll::Ready(Ok(Err(e))) => assert_eq!(e, ErrorCode::Failed),
            x => panic!("Expected Ready with ErrorCode but got {:?}", x),
        }

        Ok(())
    }

    /// Tests that independent service advertisements from multiple clients are correctly
    /// grouped together and re-registered.
    #[fuchsia::test]
    fn test_handle_multiple_service_advertisements() -> Result<(), Error> {
        let (mut exec, server, mut upstream_requests) = setup_server();

        let (service_sender, handler_fut) = setup_handler_fut(server);
        pin_mut!(handler_fut);
        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // A new client connects to bt-rfcomm.
        let client1 = {
            let client = new_client(&mut exec, service_sender.clone());
            let _ = exec.run_until_stalled(&mut handler_fut);
            client
        };

        // Client decides to advertise.
        let psm1 = Psm::new(10);
        let services1 = vec![
            bredr::ServiceDefinition::try_from(&other_service_definition(psm1))?,
            bredr::ServiceDefinition::try_from(&rfcomm_service_definition(None))?,
        ];
        let n1 = services1.len();
        let (_connection_stream1, adv_fut1) = make_advertise_request(&client1, services1);
        pin_mut!(adv_fut1);
        exec.run_until_stalled(&mut adv_fut1).expect_pending("should still be advertising");

        exec.run_until_stalled(&mut handler_fut)
            .expect_pending("shouldn't be done while service_sender is live");

        // First advertisement request relayed upstream.
        let (_receiver1, responder1) = match exec.run_until_stalled(&mut upstream_requests.next()) {
            Poll::Ready(Some(Ok(bredr::ProfileRequest::Advertise {
                receiver, responder, ..
            }))) => (receiver, responder),
            x => panic!("Expected advertise request, got: {:?}", x),
        };

        // A different client connects to bt-rfcomm. It decides to try to advertise over same PSM.
        let client2 = {
            let client = new_client(&mut exec, service_sender.clone());
            let _ = exec.run_until_stalled(&mut handler_fut);
            client
        };
        // Client 2 decides to advertise three services.
        let psm2 = Psm::new(15);
        let n2 = 3;
        let services2 = vec![
            bredr::ServiceDefinition::try_from(&other_service_definition(psm2))?,
            bredr::ServiceDefinition::try_from(&rfcomm_service_definition(None))?,
            bredr::ServiceDefinition::try_from(&rfcomm_service_definition(None))?,
        ];
        let (_connection_stream2, adv_fut2) = make_advertise_request(&client2, services2);
        pin_mut!(adv_fut2);
        exec.run_until_stalled(&mut adv_fut2).expect_pending("should still be advertising");

        let _ = exec.run_until_stalled(&mut handler_fut);

        // We expect ProfileRegistrar to unregister the current active advertisement. Respond to
        // the unregister request by responding over the responder.
        let _ = responder1.send(Ok(()));
        let _ = exec.run_until_stalled(&mut handler_fut);
        let _ = exec.run_until_stalled(&mut futures::future::pending::<()>());

        // We expect a new advertisement upstream.
        let (_receiver2, _responder2) = match exec.run_until_stalled(&mut upstream_requests.next())
        {
            Poll::Ready(Some(Ok(bredr::ProfileRequest::Advertise {
                services,
                receiver,
                responder,
                ..
            }))) => {
                assert_eq!(services.len(), n1 + n2);
                (receiver, responder)
            }
            x => panic!("Expected advertise request, got: {:?}", x),
        };
        exec.run_until_stalled(&mut adv_fut1).expect_pending("should still be advertising");
        exec.run_until_stalled(&mut adv_fut2).expect_pending("should still be advertising");

        Ok(())
    }

    /// This test validates that client Search requests are relayed directly upstream.
    #[fuchsia::test]
    fn test_handle_search_request() {
        let (mut exec, mut server, mut profile_requests) = setup_server();

        let (search_request, _stream) = generate_search_request(&mut exec);

        let handle_fut = server.handle_profile_request(search_request);
        pin_mut!(handle_fut);

        assert!(exec.run_until_stalled(&mut handle_fut).is_ready());

        // The search request should be relayed directly to the Profile Server.
        let () = match exec.run_until_stalled(&mut profile_requests.next()) {
            Poll::Ready(Some(Ok(bredr::ProfileRequest::Search { .. }))) => {}
            x => panic!("Expected search request, got: {:?}", x),
        };
    }

    /// This test validates that client ConnectSco requests are relayed directly upstream.
    #[fuchsia::test]
    fn test_handle_connect_sco_request() {
        let (mut exec, mut server, mut profile_requests) = setup_server();

        let (connect_sco_request, _receiver_server) = generate_connect_sco_request(&mut exec);

        let handle_fut = server.handle_profile_request(connect_sco_request);
        pin_mut!(handle_fut);

        assert!(exec.run_until_stalled(&mut handle_fut).is_ready());

        // The connect request should be relayed directly to the Profile Server.
        let () = match exec.run_until_stalled(&mut profile_requests.next()) {
            Poll::Ready(Some(Ok(bredr::ProfileRequest::ConnectSco { .. }))) => {}
            x => panic!("Expected search request, got: {:?}", x),
        };
    }

    /// This tests validates that 1) The call to Profile.Advertise correctly registers upstream and
    /// 2) When the call resolves, the Future resolves.
    #[fuchsia::test]
    fn test_advertise_relay() {
        let mut exec = fasync::TestExecutor::new();
        let (upstream, mut upstream_server) =
            create_proxy_and_stream::<bredr::ProfileMarker>().unwrap();
        let (connect_client, _connect_requests) =
            create_request_stream::<bredr::ConnectionReceiverMarker>().unwrap();
        let params = AdvertiseParams { services: vec![], parameters: ChannelParameters::default() };

        let advertise_fut = ProfileRegistrar::advertise(upstream, params, connect_client);
        pin_mut!(advertise_fut);
        exec.run_until_stalled(&mut advertise_fut).expect_pending("should still be advertising");

        let (_connection_receiver, responder) = match exec
            .run_until_stalled(&mut upstream_server.next())
        {
            Poll::Ready(Some(Ok(bredr::ProfileRequest::Advertise {
                receiver, responder, ..
            }))) => (receiver, responder),
            x => panic!("Expected Advertise request but got: {:?}", x),
        };

        exec.run_until_stalled(&mut advertise_fut).expect_pending("should still be advertising");

        // Upstream server decides to terminate advertisement - we expect the Future to finish.
        let _ = responder.send(Ok(()));
        let _ = exec.run_until_stalled(&mut advertise_fut).expect("advertisement should finish");
    }

    /// This test validates that incoming connection requests are correctly relayed
    /// to the Sender of the connection task.
    #[fuchsia::test]
    fn test_connection_request_relay() {
        let mut exec = fasync::TestExecutor::new();

        let (connect_client, connect_requests) =
            create_proxy_and_stream::<bredr::ConnectionReceiverMarker>().unwrap();
        let (sender, mut receiver) = mpsc::channel(0);

        let relay_fut = ProfileRegistrar::connection_request_relay(connect_requests, sender);
        pin_mut!(relay_fut);

        let receiver_fut = receiver.next();
        pin_mut!(receiver_fut);

        // The task should still be active and no messages sent.
        exec.run_until_stalled(&mut relay_fut)
            .expect_pending("shouldn't be done while receiver is live");
        exec.run_until_stalled(&mut receiver_fut).expect_pending("should wait for connection");

        // Upstream server gives us a connection.
        let id = PeerId(123);
        let protocol = &[];
        assert!(connect_client.connected(&id.into(), bredr::Channel::default(), protocol).is_ok());

        // Run the relay fut - should still be running.
        exec.run_until_stalled(&mut relay_fut)
            .expect_pending("shouldn't be done while receiver is live");

        // The relay should've sent the ConnectionEvent to the receiver.
        match exec.run_until_stalled(&mut receiver_fut) {
            Poll::Ready(Some(ConnectionEvent::Request(
                bredr::ConnectionReceiverRequest::Connected { .. },
            ))) => {}
            x => panic!("Expected connection request but got {:?}", x),
        }

        // Upstream drops ConnectionReceiver client for some reason.
        drop(connect_client);

        // The relay should notify the sender that the stream has terminated (i.e relay Canceled).
        exec.run_until_stalled(&mut relay_fut)
            .expect_pending("shouldn't be done while receiver is live");
        match exec.run_until_stalled(&mut receiver_fut) {
            Poll::Ready(Some(ConnectionEvent::AdvertisementCanceled)) => {}
            x => panic!("Expected Canceled but got: {:?}", x),
        }

        // Relay should be finished since the channel is closed.
        let _ = exec.run_until_stalled(&mut futures::future::pending::<()>());
        exec.run_until_stalled(&mut relay_fut).expect("relay should finish");
    }
}
