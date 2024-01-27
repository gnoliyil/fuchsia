// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Error};
use fidl_fuchsia_bluetooth_bredr as bredr;
use fuchsia_bluetooth::detachable_map::DetachableMap;
use fuchsia_bluetooth::profile::Psm;
use fuchsia_bluetooth::types::{Channel, PeerId};
use futures::stream::StreamExt;
use futures::Future;
use parking_lot::RwLock;
use std::collections::{HashMap, HashSet};
use std::{convert::TryInto, sync::Arc};
use tracing::{info, warn};

mod search;
pub mod service;

use self::{
    search::SearchSet,
    service::{RegistrationHandle, ServiceSet},
};
use crate::profile::{build_l2cap_descriptor, parse_service_definitions};
use crate::types::ServiceRecord;

/// Default SDU size the peer is capable of accepting. This is chosen as the default
/// max size of the underlying fuchsia_bluetooth::Channel, as this is sufficient for
/// audio streaming.
const DEFAULT_TX_SDU_SIZE: usize = Channel::DEFAULT_MAX_TX;

/// The primary object that represents a peer in the piconet.
/// `MockPeer` facilitates the registering, advertising, and connecting of services
/// for a peer.
/// Each `MockPeer` contains it's own NestedEnvironment, allowing for the sandboxed
/// starting of profiles.
pub struct MockPeer {
    /// The unique identifier for this peer.
    id: PeerId,

    /// The PeerObserver relay for this peer. This is used to send updates about this peer.
    /// If not set, no updates will be relayed.
    observer: Option<bredr::PeerObserverProxy>,

    /// Manages the active searches for this peer.
    search_mgr: Arc<RwLock<SearchSet>>,

    /// The ServiceSet handles the registration and unregistration of services
    /// that this peer provides.
    service_mgr: Arc<RwLock<ServiceSet>>,

    /// Outstanding advertised services and their connection receivers.
    services: DetachableMap<RegistrationHandle, bredr::ConnectionReceiverProxy>,
}

impl MockPeer {
    pub fn new(id: PeerId, observer: Option<bredr::PeerObserverProxy>) -> Self {
        // TODO(fxbug.dev/55462): If provided, take event stream of `observer` and listen for close.
        Self {
            id,
            observer,
            search_mgr: Arc::new(RwLock::new(SearchSet::new())),
            service_mgr: Arc::new(RwLock::new(ServiceSet::new(id))),
            services: DetachableMap::new(),
        }
    }

    pub fn peer_id(&self) -> PeerId {
        self.id
    }

    /// Returns the set of active searches, identified by their Service Class ID.
    pub fn get_active_searches(&self) -> HashSet<bredr::ServiceClassProfileIdentifier> {
        self.search_mgr.read().get_active_searches()
    }

    /// Returns the advertised services of this peer that conform to the provided `ids`.
    pub fn get_advertised_services(
        &self,
        ids: &HashSet<bredr::ServiceClassProfileIdentifier>,
    ) -> HashMap<bredr::ServiceClassProfileIdentifier, Vec<ServiceRecord>> {
        self.service_mgr.read().get_service_records(ids)
    }

    /// Notifies the `observer` with the ServiceFound update from the ServiceRecord.
    fn relay_service_found(observer: &bredr::PeerObserverProxy, record: ServiceRecord) {
        let mut response = record.to_service_found_response().unwrap();
        let mut protocol = response.protocol.as_mut().map(|v| v.iter_mut());
        let protocol = protocol
            .as_mut()
            .map(|v| -> &mut dyn ExactSizeIterator<Item = &mut bredr::ProtocolDescriptor> { v });
        let _ = observer.service_found(
            &mut response.id.into(),
            protocol,
            &mut response.attributes.iter_mut(),
        );
    }

    /// Notifies the `observer` with the connection on `psm` established by peer `other`.
    fn relay_connected(
        observer: &bredr::PeerObserverProxy,
        other: PeerId,
        mut protocol: Vec<bredr::ProtocolDescriptor>,
    ) {
        let _ = observer.peer_connected(&mut other.into(), &mut protocol.iter_mut());
    }

    /// Notifies all the searches for service class `id` with the published
    /// services in `services`.
    /// The relevant services of a peer are provided in `services`.
    /// If the ServiceFound response was successful, relays the data to the PeerObserver.
    pub fn notify_searches(
        &mut self,
        id: &bredr::ServiceClassProfileIdentifier,
        services: Vec<ServiceRecord>,
    ) {
        let proxy = self.observer.clone();
        let mut w_search_mgr = self.search_mgr.write();
        for service in &services {
            let notifs = w_search_mgr.notify_searches(id, service.clone());
            // Relay the number of searches notified to the observer.
            if let Some(proxy) = proxy.as_ref() {
                for _ in 0..notifs {
                    Self::relay_service_found(proxy, service.clone());
                }
            }
        }
    }

    /// Attempts to register the services, as a group, specified by `services`.
    ///
    /// Returns 1) The ServiceClassProfileIds of the registered services and 2) A future that should
    /// be polled in order to remove the advertisement when `proxy` is closed.
    /// Returns an Error if any service in the provided `services` are invalid, or if
    /// registration with the ServiceSet failed.
    pub fn new_advertisement(
        &mut self,
        services: Vec<bredr::ServiceDefinition>,
        proxy: bredr::ConnectionReceiverProxy,
    ) -> Result<(HashSet<bredr::ServiceClassProfileIdentifier>, impl Future<Output = ()>), Error>
    {
        let service_records = parse_service_definitions(services)?;
        let registration_handle = self
            .service_mgr
            .write()
            .register_service(service_records)
            .ok_or(format_err!("Registration of services failed"))?;

        let service_event_stream = proxy.take_event_stream();

        if let Some(s) = self.services.insert(registration_handle, proxy) {
            warn!("Replaced a service at {}: {:?}", registration_handle, s);
        }
        let ids = self
            .service_mgr
            .read()
            .get_service_ids_for_registration_handle(&registration_handle)
            .unwrap_or(&HashSet::new())
            .clone();

        let peer_id = self.id.clone();
        let reg_handle_clone = registration_handle.clone();
        let detached_service = self.services.get(&registration_handle).expect("just added");
        let service_mgr_clone = self.service_mgr.clone();
        let closed_fut = async move {
            let _ = service_event_stream.map(|_| ()).collect::<()>().await;
            detached_service.detach();
            let removed = service_mgr_clone.write().unregister_service(&reg_handle_clone);
            info!("Peer {} unregistered service advertisement: {:?}", peer_id, removed);
        };

        Ok((ids, closed_fut))
    }

    /// Creates a new connection between this peer and the `other` peer.
    ///
    /// `other` specifies the PeerId of the peer initiating the connection.
    /// `psm` is the L2CAP PSM number of the connection.
    ///
    /// Returns the created Channel if the provided `psm` is valid.
    pub fn new_connection(&self, other: PeerId, psm: Psm) -> Result<bredr::Channel, Error> {
        let reg_handle = self
            .service_mgr
            .read()
            .psm_registered(psm)
            .ok_or(format_err!("PSM {:?} not registered", psm))?;
        let proxy = self
            .services
            .get(&reg_handle)
            .and_then(|p| p.upgrade())
            .ok_or(format_err!("Connection receiver doesn't exist"))?;

        // Build the L2CAP descriptor and notify the receiver.
        let mut protocol = build_l2cap_descriptor(psm);
        let (local, remote) = Channel::create_with_max_tx(DEFAULT_TX_SDU_SIZE);
        proxy.connected(&mut other.into(), remote.try_into()?, &mut protocol.iter_mut())?;

        // Notify observer relay of the connection.
        let _ = self.observer.as_ref().map(|o| Self::relay_connected(o, other, protocol));

        local.try_into().map_err(|e| format_err!("{e:?}"))
    }

    /// Registers a new search for the provided `service_uuid`.
    /// Returns a future that should be polled in order to remove the search
    /// when `proxy` is closed.
    pub fn new_search(
        &mut self,
        service_uuid: bredr::ServiceClassProfileIdentifier,
        attr_ids: Vec<u16>,
        proxy: bredr::SearchResultsProxy,
    ) -> impl Future<Output = ()> {
        let search_stream = proxy.take_event_stream();

        let search_handle = {
            let mut w_search_mgr = self.search_mgr.write();
            w_search_mgr.add(service_uuid, attr_ids, proxy)
        };

        let peer_id = self.id.clone();
        let uuid = service_uuid.clone();
        let search_mgr_clone = self.search_mgr.clone();
        let closed_fut = async move {
            let _ = search_stream.map(|_| ()).collect::<()>().await;
            if search_mgr_clone.write().remove(search_handle) {
                info!("Peer {} unregistering service search {:?}", peer_id, uuid);
            }
        };

        closed_fut
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use {
        assert_matches::assert_matches,
        bt_rfcomm::{
            profile::{is_rfcomm_protocol, server_channel_from_protocol},
            ServerChannel,
        },
        fidl::endpoints::create_proxy_and_stream,
        fuchsia_async as fasync,
        fuchsia_bluetooth::profile::ProtocolDescriptor,
        futures::{pin_mut, task::Poll},
        std::convert::TryFrom,
    };

    use crate::{
        profile::tests::{a2dp_service_definition, rfcomm_service_definition},
        types::RegisteredServiceId,
    };

    fn create_mock_peer(id: PeerId) -> Result<(MockPeer, bredr::PeerObserverRequestStream), Error> {
        let (proxy, stream) = create_proxy_and_stream::<bredr::PeerObserverMarker>().unwrap();
        Ok((MockPeer::new(id, Some(proxy)), stream))
    }

    /// Builds and registers a search for an `id` with no attributes.
    /// Returns 1) The RequestStream used to process search results and 2) A future
    /// that signals the termination of the service search.
    fn build_and_register_search(
        mock_peer: &mut MockPeer,
        id: bredr::ServiceClassProfileIdentifier,
    ) -> (bredr::SearchResultsRequestStream, impl Future<Output = ()>) {
        let (client, stream) = create_proxy_and_stream::<bredr::SearchResultsMarker>()
            .expect("couldn't create endpoints");
        let search_fut = mock_peer.new_search(id, vec![], client);
        (stream, search_fut)
    }

    /// Builds and registers an A2DP Sink service advertisement.
    /// Returns 1) ServerEnd of the ConnectionReceiver that will be used to receive l2cap
    /// connections, 2) A future that signals the termination of the service advertisement
    /// and 3) The ServiceClassProfileIds that were registered.
    fn build_and_register_service(
        mock_peer: &mut MockPeer,
    ) -> (
        bredr::ConnectionReceiverRequestStream,
        impl Future<Output = ()>,
        HashSet<bredr::ServiceClassProfileIdentifier>,
    ) {
        // Build the A2DP Sink Service Definition.
        let (a2dp_def, expected_record) = a2dp_service_definition(Psm::new(bredr::PSM_AVDTP));

        // Register the service.
        let (receiver, stream) =
            create_proxy_and_stream::<bredr::ConnectionReceiverMarker>().unwrap();
        let (svc_ids, adv_fut) =
            mock_peer.new_advertisement(vec![a2dp_def], receiver).expect("advertisement is ok");
        assert_eq!(expected_record.service_ids().clone(), svc_ids);

        (stream, adv_fut, svc_ids)
    }

    /// Expects a ServiceFound call to the `stream` for the `other_peer`. Returns the primary
    /// protocol associated with the service.
    ///
    /// Panics if the call doesn't happen.
    fn expect_search_service_found(
        exec: &mut fasync::TestExecutor,
        stream: &mut bredr::SearchResultsRequestStream,
        other_peer: PeerId,
    ) -> Option<Vec<ProtocolDescriptor>> {
        let service_found_fut = stream.select_next_some();
        pin_mut!(service_found_fut);

        match exec.run_until_stalled(&mut service_found_fut) {
            Poll::Ready(Ok(bredr::SearchResultsRequest::ServiceFound {
                peer_id,
                protocol,
                responder,
                ..
            })) => {
                let _ = responder.send();
                assert_eq!(other_peer, peer_id.into());
                protocol.map(|p| p.iter().map(Into::into).collect())
            }
            x => panic!("Expected ServiceFound request but got: {:?}", x),
        }
    }

    /// Expects a ServiceFound event on the observer `stream` for the `other_peer`.
    /// Panics if the call doesn't happen.
    fn expect_observer_service_found(
        exec: &mut fasync::TestExecutor,
        stream: &mut bredr::PeerObserverRequestStream,
        other_peer: PeerId,
    ) {
        let observer_fut = stream.select_next_some();
        pin_mut!(observer_fut);

        match exec.run_until_stalled(&mut observer_fut) {
            Poll::Ready(Ok(bredr::PeerObserverRequest::ServiceFound {
                peer_id,
                responder,
                ..
            })) => {
                let _ = responder.send();
                assert_eq!(other_peer, peer_id.into());
            }
            x => panic!("Expected ServiceFound request but got: {:?}", x),
        }
    }

    /// Tests registration of a new service followed by unregistration when the
    /// client drops the ConnectionReceiver.
    #[test]
    fn registered_service_is_unregistered_when_receiver_disconnects() -> Result<(), Error> {
        let mut exec = fasync::TestExecutor::new();

        let id = PeerId(234);
        let (mut mock_peer, _observer_stream) = create_mock_peer(id)?;

        let (stream, adv_fut, svc_ids) = build_and_register_service(&mut mock_peer);
        pin_mut!(adv_fut);

        // Services should be advertised.
        let advertised_service_records = mock_peer.get_advertised_services(&svc_ids);
        assert_eq!(svc_ids, advertised_service_records.keys().cloned().collect());
        assert!(exec.run_until_stalled(&mut adv_fut).is_pending());

        // Client decides to not advertise its service anymore by dropping ServerEnd.
        // The advertisement future should resolve.
        drop(stream);
        assert!(exec.run_until_stalled(&mut adv_fut).is_ready());

        // Advertised services for the previously registered ServiceClassProfileIds should be gone.
        let advertised_service_records = mock_peer.get_advertised_services(&svc_ids);
        assert_eq!(HashSet::new(), advertised_service_records.keys().cloned().collect());

        Ok(())
    }

    /// Tests the registration of a new service and establishing a connection
    /// over potentially registered PSMs.
    #[test]
    fn register_l2cap_service_with_connection_success() -> Result<(), Error> {
        let mut exec = fasync::TestExecutor::new();

        let id = PeerId(2392);
        let (mut mock_peer, mut observer_stream) = create_mock_peer(id)?;

        // Build the A2DP Sink Service Definition.
        let (mut stream, _adv_fut, _svc_ids) = build_and_register_service(&mut mock_peer);

        // There should be no connection updates yet.
        assert_matches!(exec.run_until_stalled(&mut stream.next()), Poll::Pending);

        // An incoming connection request for PSM_AVCTP is invalid because PSM_AVCTP has not
        // been registered as a service.
        let remote_peer = PeerId(987);
        assert_matches!(mock_peer.new_connection(remote_peer, Psm::AVCTP), Err(_));

        // An incoming connection request for PSM_AVDTP is valid, since it was registered
        // in `a2dp_def`. There should be a new connection request on the stream.
        assert_matches!(mock_peer.new_connection(remote_peer, Psm::AVDTP), Ok(_));
        match exec.run_until_stalled(&mut stream.next()) {
            Poll::Ready(Some(Ok(bredr::ConnectionReceiverRequest::Connected {
                peer_id,
                channel,
                ..
            }))) => {
                assert_eq!(remote_peer, peer_id.into());
                assert_eq!(channel.channel_mode, Some(bredr::ChannelMode::Basic));
                assert_eq!(channel.max_tx_sdu_size, Some(DEFAULT_TX_SDU_SIZE as u16));
            }
            x => panic!("Expected Ready but got: {:?}", x),
        }
        // This should also be echo'ed on the observer.
        match exec.run_until_stalled(&mut observer_stream.next()) {
            Poll::Ready(Some(Ok(bredr::PeerObserverRequest::PeerConnected {
                peer_id,
                responder,
                ..
            }))) => {
                assert_eq!(remote_peer, peer_id.into());
                let _ = responder.send();
            }
            x => panic!("Expected Ready but got: {:?}", x),
        }

        Ok(())
    }

    /// Tests the registration of new searches. There can be multiple searches for the
    /// same ServiceClassProfileIdentifier.
    /// Tests notifying the outstanding searches with an advertised service.
    #[test]
    fn register_multiple_searches_success() -> Result<(), Error> {
        let mut exec = fasync::TestExecutor::new();

        let id = PeerId(2824);
        let (mut mock_peer, mut observer_stream) = create_mock_peer(id)?;

        // The new search should be stored.
        let (mut stream1, search1) = build_and_register_search(
            &mut mock_peer,
            bredr::ServiceClassProfileIdentifier::AudioSink,
        );
        pin_mut!(search1);
        let mut expected_searches: HashSet<_> =
            vec![bredr::ServiceClassProfileIdentifier::AudioSink].into_iter().collect();
        assert_eq!(expected_searches, mock_peer.get_active_searches());

        // Adding a search for the same Service Class ID is OK.
        let (mut stream2, search2) = build_and_register_search(
            &mut mock_peer,
            bredr::ServiceClassProfileIdentifier::AudioSink,
        );
        pin_mut!(search2);
        assert_eq!(expected_searches, mock_peer.get_active_searches());

        // Adding different search is OK.
        let (mut stream3, search3) = build_and_register_search(
            &mut mock_peer,
            bredr::ServiceClassProfileIdentifier::AvRemoteControl,
        );
        pin_mut!(search3);
        let _ = expected_searches.insert(bredr::ServiceClassProfileIdentifier::AvRemoteControl);
        assert_eq!(expected_searches, mock_peer.get_active_searches());

        // All three futures that listen for search termination should still be active.
        assert!(exec.run_until_stalled(&mut search1).is_pending());
        assert!(exec.run_until_stalled(&mut search2).is_pending());
        assert!(exec.run_until_stalled(&mut search3).is_pending());

        // Build a fake service as a registered record and notify any A2DP Sink searches.
        let other_peer = PeerId(999);
        let (_, mut record) = a2dp_service_definition(Psm::new(19));
        record.register_service_record(RegisteredServiceId::new(other_peer, 789)); // random
        let services = vec![record];
        mock_peer.notify_searches(&bredr::ServiceClassProfileIdentifier::AudioSink, services);

        // Only `stream1` and `stream2` correspond to searches for AudioSink.
        let _ = expect_search_service_found(&mut exec, &mut stream1, other_peer);
        let _ = expect_search_service_found(&mut exec, &mut stream2, other_peer);
        match exec.run_until_stalled(&mut stream3.next()) {
            Poll::Pending => {}
            x => panic!("Expected Pending but got: {:?}", x),
        }

        // Validate that the search results were observed by the PeerObserver for this MockPeer.
        // We expect the observer to be updated twice, since two different searches were notified.
        expect_observer_service_found(&mut exec, &mut observer_stream, other_peer);
        expect_observer_service_found(&mut exec, &mut observer_stream, other_peer);

        Ok(())
    }

    /// Tests that a service search is correctly terminated when the the client drops
    /// the ServerEnd of the SearchResults channel.
    #[test]
    fn service_search_terminates_when_client_disconnects() -> Result<(), Error> {
        let mut exec = fasync::TestExecutor::new();

        let id = PeerId(5604);
        let (mut mock_peer, _observer_stream) = create_mock_peer(id)?;

        // The new search should be stored.
        let (stream1, search1) = build_and_register_search(
            &mut mock_peer,
            bredr::ServiceClassProfileIdentifier::AvRemoteControlTarget,
        );
        pin_mut!(search1);

        let expected_searches: HashSet<_> =
            vec![bredr::ServiceClassProfileIdentifier::AvRemoteControlTarget].into_iter().collect();
        assert_eq!(expected_searches, mock_peer.get_active_searches());
        assert!(exec.run_until_stalled(&mut search1).is_pending());

        // Client decides it doesn't want to search for AudioSink anymore.
        drop(stream1);
        assert!(exec.run_until_stalled(&mut search1).is_ready());
        assert_eq!(HashSet::new(), mock_peer.get_active_searches());

        Ok(())
    }

    /// Validates that a service search gets notified again when a client disconnects,
    /// reconnects, and re-advertises an identical service.
    #[test]
    fn service_search_gets_notified_when_service_re_registers() -> Result<(), Error> {
        let mut exec = fasync::TestExecutor::new();

        let id = PeerId(9999);
        let (mut mock_peer, _observer_stream) = create_mock_peer(id)?;

        // Peer searches for A2DP Sink in the piconet.
        let (mut stream, _search) = build_and_register_search(
            &mut mock_peer,
            bredr::ServiceClassProfileIdentifier::AudioSink,
        );

        // Build a fake A2DP Sink service for some remote.
        let remote_peer = PeerId(2324);
        let random_handle = 889;
        let (_, mut record) = a2dp_service_definition(Psm::new(19));
        let mut record_clone = record.clone();

        // Register the record and notify the mock peer who's searching for it.
        record.register_service_record(RegisteredServiceId::new(remote_peer, random_handle));
        mock_peer.notify_searches(&bredr::ServiceClassProfileIdentifier::AudioSink, vec![record]);

        // Should be notified on the peer's search stream.
        let _ = expect_search_service_found(&mut exec, &mut stream, remote_peer);

        // The remote peer associated with the A2DP Sink service reconnects with an identical service.
        record_clone.register_service_record(RegisteredServiceId::new(remote_peer, random_handle));
        mock_peer
            .notify_searches(&bredr::ServiceClassProfileIdentifier::AudioSink, vec![record_clone]);

        // Even though it's an identical service, it should still be relayed to the `mock_peer`
        // because the `remote_peer` re-advertised it (e.g re-registered it).
        let _ = expect_search_service_found(&mut exec, &mut stream, remote_peer);

        Ok(())
    }

    #[test]
    fn rfcomm_connection_with_no_advertisement_returns_error() {
        let _exec = fasync::TestExecutor::new();

        // New mock peer with no RFCOMM advertisement.
        let id = PeerId(71);
        let (mock_peer, _observer_stream) = create_mock_peer(id).expect("valid mock peer");

        let other = PeerId(72);
        assert_matches!(mock_peer.new_connection(other, Psm::RFCOMM), Err(_));
    }

    #[test]
    fn rfcomm_connection_relayed_to_peer() {
        let mut exec = fasync::TestExecutor::new();

        let id = PeerId(80);
        let (mut mock_peer, mut observer_stream) = create_mock_peer(id).expect("valid mock peer");

        // Register some RFCOMM service with a random channel number.
        let (rfcomm_def, _) =
            rfcomm_service_definition(ServerChannel::try_from(4).expect("valid channel"));
        let (receiver, mut stream) =
            create_proxy_and_stream::<bredr::ConnectionReceiverMarker>().unwrap();
        let (_, _adv_fut) =
            mock_peer.new_advertisement(vec![rfcomm_def], receiver).expect("valid advertisement");

        // Some other peer wants to connect to the RFCOMM service.
        let other_peer = PeerId(81);
        let _other_channel =
            mock_peer.new_connection(other_peer, Psm::RFCOMM).expect("connection should work");

        // `mock_peer` should receive connection request.
        let _mock_peer_channel = match exec.run_until_stalled(&mut stream.next()) {
            Poll::Ready(Some(Ok(bredr::ConnectionReceiverRequest::Connected {
                peer_id,
                channel,
                ..
            }))) => {
                assert_eq!(other_peer, peer_id.into());
                channel
            }
            x => panic!("Expected Connected but got: {:?}", x),
        };

        // Should be echoed on the observer.
        match exec.run_until_stalled(&mut observer_stream.next()) {
            Poll::Ready(Some(Ok(bredr::PeerObserverRequest::PeerConnected {
                peer_id,
                responder,
                ..
            }))) => {
                assert_eq!(other_peer, peer_id.into());
                let _ = responder.send();
            }
            x => panic!("Expected PeerConnected but got: {:?}", x),
        }
    }

    #[test]
    fn rfcomm_advertisement_relayed_to_peer_search() {
        let mut exec = fasync::TestExecutor::new();

        let id = PeerId(81);
        let (mut mock_peer, mut observer_stream) = create_mock_peer(id).expect("valid mock peer");

        // Peer searches for SPP services in the piconet.
        let (mut search_results, _search_fut) = build_and_register_search(
            &mut mock_peer,
            bredr::ServiceClassProfileIdentifier::SerialPort,
        );

        // Some random SPP service is discovered.
        let other_peer = PeerId(82);
        let random_handle = 1234;
        let random_rfcomm_channel = ServerChannel::try_from(5).expect("valid channel");
        let (_, mut spp_record) = rfcomm_service_definition(random_rfcomm_channel);
        spp_record.register_service_record(RegisteredServiceId::new(other_peer, random_handle));

        // Since the `mock_peer` is searching for SPP services, it should be notified of the service.
        mock_peer
            .notify_searches(&bredr::ServiceClassProfileIdentifier::SerialPort, vec![spp_record]);
        let discovered_protocol =
            expect_search_service_found(&mut exec, &mut search_results, other_peer)
                .expect("Should have protocol");
        assert!(is_rfcomm_protocol(&discovered_protocol));
        assert_eq!(server_channel_from_protocol(&discovered_protocol), Some(random_rfcomm_channel));

        // Should be echoed on the observer.
        expect_observer_service_found(&mut exec, &mut observer_stream, other_peer);
    }
}
