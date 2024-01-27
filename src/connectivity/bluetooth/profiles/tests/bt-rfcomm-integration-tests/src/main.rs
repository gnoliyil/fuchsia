// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    bt_rfcomm::{profile::server_channel_from_protocol, ServerChannel},
    fidl_fuchsia_bluetooth_bredr as bredr,
    fuchsia_async::{DurationExt, TimeoutExt},
    fuchsia_bluetooth::types::{Channel, PeerId, Uuid},
    fuchsia_component_test::{Capability, RealmInstance},
    fuchsia_zircon::Duration,
    futures::{pin_mut, stream::StreamExt},
    mock_piconet_client::{BtProfileComponent, PiconetHarness, PiconetMember},
    profile_client::{ProfileClient, ProfileEvent},
    std::convert::TryInto,
};

/// RFCOMM component URL.
/// The RFCOMM component is a unique component in that it functions as a proxy for the
/// `fuchsia.bluetooth.bredr.Profile` protocol. Consequently, it both connects to and provides the
/// `fuchsia.bluetooth.bredr.Profile` protocol.
/// It only manages Profile requests that require RFCOMM - any non-RFCOMM requests
/// are relayed to the upstream `fuchsia.bluetooth.bredr.Profile` provider.
const RFCOMM_URL: &str = "fuchsia-pkg://fuchsia.com/bt-rfcomm-integration-tests#meta/bt-rfcomm.cm";

/// The moniker for the RFCOMM component under test.
const RFCOMM_MONIKER: &str = "bt-rfcomm";
/// The moniker for a mock piconet member.
const MOCK_PICONET_MEMBER_MONIKER: &str = "mock-peer";

/// Timeout for data received over an RFCOMM channel.
///
/// This time is expected to be:
///   a) sufficient to avoid flakes due to infra or resource contention
///   b) short enough to still provide useful feedback in those cases where asynchronous operations
///      fail
///   c) short enough to fail before the overall infra-imposed test timeout (currently 5 minutes)
const RFCOMM_CHANNEL_TIMEOUT: Duration = Duration::from_seconds(2 * 60);

/// The SppClient in these integration tests is just an alias for the `ProfileClient`.
type SppClient = ProfileClient;

/// Returns a valid SPP Service Definition.
/// See SPP V12 Table 6.1.
pub fn spp_service_definition() -> bredr::ServiceDefinition {
    bredr::ServiceDefinition {
        service_class_uuids: Some(vec![Uuid::new16(
            bredr::ServiceClassProfileIdentifier::SerialPort as u16,
        )
        .into()]),
        protocol_descriptor_list: Some(vec![
            bredr::ProtocolDescriptor {
                protocol: bredr::ProtocolIdentifier::L2Cap,
                params: vec![], // Ignored for RFCOMM.
            },
            bredr::ProtocolDescriptor {
                protocol: bredr::ProtocolIdentifier::Rfcomm,
                params: vec![], // This will be populated by the RFCOMM component.
            },
        ]),
        profile_descriptors: Some(vec![bredr::ProfileDescriptor {
            profile_id: bredr::ServiceClassProfileIdentifier::SerialPort,
            major_version: 1,
            minor_version: 2,
        }]),
        ..Default::default()
    }
}

async fn add_rfcomm_component(
    test_harness: &mut PiconetHarness,
    name: String,
) -> BtProfileComponent {
    let expose = vec![Capability::protocol::<bredr::ProfileMarker>().into()];
    test_harness
        .add_profile_with_capabilities(name, RFCOMM_URL.to_string(), None, vec![], expose)
        .await
        .expect("failed to add RFCOMM profile")
}

/// Builds the test topology for the RFCOMM integration tests. Returns the test realm, an observer
/// for bt-rfcomm, and a piconet member which can be driven by the test.
async fn setup_test_topology() -> (RealmInstance, BtProfileComponent, PiconetMember) {
    let mut test_harness = PiconetHarness::new().await;

    // Add a mock piconet member.
    let spec = test_harness
        .add_mock_piconet_member(MOCK_PICONET_MEMBER_MONIKER.to_string(), None)
        .await
        .expect("failed to add mock piconet member");
    // Add bt-rfcomm which is under test.
    let rfcomm = add_rfcomm_component(&mut test_harness, RFCOMM_MONIKER.to_string()).await;

    let test_topology = test_harness.build().await.unwrap();

    // Once the test realm has been built, we can grab the piconet member to be driven
    // by this test.
    let test_driven_peer = PiconetMember::new_from_spec(spec, &test_topology)
        .expect("failed to create piconet member from spec");

    (test_topology, rfcomm, test_driven_peer)
}

/// Returns an SppClient that uses the `Profile` capability exposed by the RFCOMM component in the
/// test `topology`.
/// For the purposes of the integration tests, the RFCOMM client advertises SPP.
async fn setup_spp_client(
    topology: &RealmInstance,
    rfcomm_component: &BtProfileComponent,
) -> SppClient {
    let profile = rfcomm_component
        .connect_to_protocol::<bredr::ProfileMarker>(topology)
        .expect("Profile should be available");
    let spp = spp_service_definition();
    ProfileClient::advertise(profile, &vec![spp], bredr::ChannelParameters::default()).unwrap()
}

async fn expect_peer_advertising(
    search_results: &mut bredr::SearchResultsRequestStream,
    expected_id: PeerId,
) -> ServerChannel {
    let request = search_results.select_next_some().await.unwrap();
    let bredr::SearchResultsRequest::ServiceFound { peer_id, protocol, responder, .. } = request;
    assert_eq!(expected_id, peer_id.into());
    tracing::info!("Discovered service with protocol: {:?}", protocol);
    responder.send().unwrap();
    let protocol = protocol.unwrap().iter().map(Into::into).collect();
    server_channel_from_protocol(&protocol).expect("is rfcomm")
}

/// Tests that an RFCOMM-requesting client can register a service advertisement
/// which is discoverable by others in the piconet. If the client re-connects and
/// decides to re-advertise, the service should be discoverable.
#[fuchsia::test]
async fn register_rfcomm_service_advertisement_is_discovered() {
    let (test_topology, rfcomm_under_test, test_driven_peer) = setup_test_topology().await;

    // Poke at `bt-rfcomm` by registering a fake SPP client that uses the Profile protocol
    // provided by it.
    let spp_client = setup_spp_client(&test_topology, &rfcomm_under_test).await;

    // Test driven member searches for SPP services.
    let mut search_results = test_driven_peer
        .register_service_search(bredr::ServiceClassProfileIdentifier::SerialPort, vec![])
        .unwrap();
    // We expect it to discover `spp_client's` service advertisement.
    let _ = expect_peer_advertising(&mut search_results, rfcomm_under_test.peer_id()).await;

    // For some reason, the SPP client disconnects (component termination, error, etc..).
    drop(spp_client);

    // Client coming back should be OK.
    let _spp_client2 = setup_spp_client(&test_topology, &rfcomm_under_test).await;

    // We expect test driven member to discover `spp_client2s` service advertisement.
    let _ = expect_peer_advertising(&mut search_results, rfcomm_under_test.peer_id()).await;
}

#[fuchsia::test]
async fn multiple_rfcomm_clients_can_register_advertisements() {
    let (test_topology, rfcomm_under_test, test_driven_peer) = setup_test_topology().await;

    // Test driven member searches for SPP services.
    let mut search_results = test_driven_peer
        .register_service_search(bredr::ServiceClassProfileIdentifier::SerialPort, vec![])
        .unwrap();

    let _spp_client1 = setup_spp_client(&test_topology, &rfcomm_under_test).await;
    // `spp_client1`'s advertisement should be discovered.
    let _ = expect_peer_advertising(&mut search_results, rfcomm_under_test.peer_id()).await;

    let _spp_client2 = setup_spp_client(&test_topology, &rfcomm_under_test).await;
    // Because `bt-rfcomm` manages multiple RFCOMM service advertisements, we expect _2_ search
    // result events. This is because `bt-rfcomm` unregisters `_spp_client1`'s advertisement, groups
    // `_spp_client2`'s advertisement with it, and re-registers them together. As such, the new
    // "unified" advertisement consists of two services.
    let _ = expect_peer_advertising(&mut search_results, rfcomm_under_test.peer_id()).await;
    let _ = expect_peer_advertising(&mut search_results, rfcomm_under_test.peer_id()).await;

    // There should be no more search result events.
    assert!(futures::poll!(&mut search_results.next()).is_pending());
}

/// Verifies that the `send` RFCOMM channel can be written to and that the `data` is received by the
/// `receive` RFCOMM channel.
async fn send_and_expect_data(send: &Channel, receive: &Channel, data: Vec<u8>) {
    let n = data.len();
    assert_eq!(send.as_ref().write(&data[..]), Ok(n));

    // `read_datagram` occasionally returns ready with Ok(0) when there are no bytes read from the
    // socket. Instead of erroring, we retry the read operation until we actually receive a nonzero
    // amount of data over the channel.
    loop {
        let mut actual_bytes = Vec::new();
        let read_result = receive
            .read_datagram(&mut actual_bytes)
            .on_timeout(RFCOMM_CHANNEL_TIMEOUT.after_now(), move || Err(fidl::Status::TIMED_OUT))
            .await
            .expect("reading from channel is ok");

        if read_result != 0 {
            assert_eq!(actual_bytes, data);
            break;
        }
    }
}

/// Tests the connection flow between two Fuchsia RFCOMM components. Each RFCOMM component is driven
/// by an example SPP client. Validates that data can be exchanged between the two clients after
/// establishment.
#[fuchsia::test]
async fn rfcomm_component_connecting_to_another_rfcomm_component() {
    let mut test_harness = PiconetHarness::new().await;

    // Define two RFCOMM components to be tested against each other.
    let rfcomm_name1 = "this-bt-rfcomm";
    let rfcomm_name2 = "other-bt-rfcomm";
    let mut this_rfcomm = add_rfcomm_component(&mut test_harness, rfcomm_name1.to_string()).await;
    let mut other_rfcomm = add_rfcomm_component(&mut test_harness, rfcomm_name2.to_string()).await;

    let test_topology = test_harness.build().await.expect("topology should build");

    // Client for RFCOMM #1 is SPP (both advertisement and search).
    let mut client1 = setup_spp_client(&test_topology, &this_rfcomm).await;

    // Client for RFCOMM #2 searches for SPP peers - it should discover client #1.
    let other_rfcomm_proxy = other_rfcomm
        .connect_to_protocol::<bredr::ProfileMarker>(&test_topology)
        .expect("can connect to Profile");
    let (search_client, mut search_results) = fidl::endpoints::create_request_stream().unwrap();
    other_rfcomm_proxy
        .search(bredr::ServiceClassProfileIdentifier::SerialPort, &[], search_client)
        .expect("can register search");

    // Client #1's advertisement should be discovered - should also be echoed on observer.
    let rfcomm_channel_number =
        expect_peer_advertising(&mut search_results, this_rfcomm.peer_id()).await;
    other_rfcomm
        .expect_observer_service_found_request(this_rfcomm.peer_id())
        .await
        .expect("observer event");

    // Client #2 can now connect via RFCOMM to Client #1.
    let mut params = bredr::ConnectParameters::Rfcomm(bredr::RfcommParameters {
        channel: Some(rfcomm_channel_number.into()),
        ..Default::default()
    });
    let connect_fut = other_rfcomm_proxy.connect(&mut this_rfcomm.peer_id().into(), &mut params);
    pin_mut!(connect_fut);

    // Each client should be delivered one end of the RFCOMM channel.
    let (channel1, channel2): (Channel, Channel) =
        match futures::future::join(client1.next(), connect_fut).await {
            (Some(Ok(ProfileEvent::PeerConnected { id, channel, .. })), Ok(Ok(channel2))) => {
                assert_eq!(id, other_rfcomm.peer_id());
                (channel.try_into().unwrap(), channel2.try_into().unwrap())
            }
            x => panic!("Expected two channels, but got: {:?}", x),
        };
    // The connection request should be echoed on RFCOMM #1's observer since RFCOMM #2 initiated.
    this_rfcomm
        .expect_observer_connection_request(other_rfcomm.peer_id())
        .await
        .expect("should observe connection request");

    // Data can be exchanged in both directions.
    let data1 = vec![0x05, 0x06, 0x07, 0x08, 0x09];
    send_and_expect_data(&channel1, &channel2, data1).await;
    let data2 = vec![0x98, 0x97, 0x96, 0x95];
    send_and_expect_data(&channel2, &channel1, data2).await;
}

fn a2dp_service_definition() -> bredr::ServiceDefinition {
    // Minimal Sink definition. Note: This is not a complete definition, but enough to register.
    bredr::ServiceDefinition {
        service_class_uuids: Some(vec![Uuid::new16(0x110B).into()]), // Audio Sink UUID
        protocol_descriptor_list: Some(vec![bredr::ProtocolDescriptor {
            protocol: bredr::ProtocolIdentifier::L2Cap,
            params: vec![bredr::DataElement::Uint16(bredr::PSM_AVDTP)],
        }]),
        ..Default::default()
    }
}

/// Tests that a non-RFCOMM service advertisement can still be discovered when using the RFCOMM
/// component as an intermediary.
#[fuchsia::test]
async fn passthrough_advertisement_is_discovered() {
    let (test_topology, rfcomm_under_test, test_driven_peer) = setup_test_topology().await;

    // RFCOMM client advertises A2DP Sink - no RFCOMM needed.
    let profile = rfcomm_under_test
        .connect_to_protocol::<bredr::ProfileMarker>(&test_topology)
        .expect("Profile should be available");
    let a2dp = a2dp_service_definition();
    let _client =
        ProfileClient::advertise(profile.clone(), &vec![a2dp], bredr::ChannelParameters::default())
            .unwrap();

    // Test driven peer searches for A2DP Sink.
    let mut search_results = test_driven_peer
        .register_service_search(bredr::ServiceClassProfileIdentifier::AudioSink, vec![])
        .expect("can register search");
    // We expect it to discover `client's` service advertisement.
    let request = search_results.select_next_some().await.unwrap();
    let bredr::SearchResultsRequest::ServiceFound { peer_id, responder, .. } = request;
    assert_eq!(rfcomm_under_test.peer_id(), peer_id.into());
    // Ack the response so that the `search_results` channel doesn't get full.
    responder.send().unwrap();
}

/// Tests that a non-RFCOMM service search can still discover an advertisement when using the RFCOMM
/// component as an intermediary.
#[fuchsia::test]
async fn passthrough_search_discovers_advertisement() {
    let (test_topology, rfcomm_under_test, test_driven_peer) = setup_test_topology().await;

    // Test-driven peer advertises A2DP Sink.
    let a2dp = a2dp_service_definition();
    let mut connect_requests = test_driven_peer
        .register_service_advertisement(vec![a2dp])
        .expect("can register advertisement");

    // RFCOMM client searches for A2DP Sink.
    let profile = rfcomm_under_test
        .connect_to_protocol::<bredr::ProfileMarker>(&test_topology)
        .expect("Profile should be available");
    let mut client = ProfileClient::new(profile.clone());
    let _ = client
        .add_search(bredr::ServiceClassProfileIdentifier::AudioSink, &[])
        .expect("can register search");
    // We expect it to discover `test_driven_peer`s service advertisement.
    match client.next().await.unwrap() {
        Ok(ProfileEvent::SearchResult { id, .. }) => {
            assert_eq!(test_driven_peer.peer_id(), id);
        }
        x => panic!("Expected search result but got: {:?}", x),
    }

    // Passthrough L2CAP connection is OK - the channel endpoints should be delivered to the
    // `client` and `test_driven_peer`.
    let _client_channel = profile
        .connect(
            &mut test_driven_peer.peer_id().into(),
            &mut bredr::ConnectParameters::L2cap(bredr::L2capParameters {
                psm: Some(bredr::PSM_AVDTP),
                ..Default::default()
            }),
        )
        .await;

    let (other_id, _test_driven_peer_channel, _, _) = connect_requests
        .select_next_some()
        .await
        .expect("connection request")
        .into_connected()
        .unwrap();
    assert_eq!(other_id, rfcomm_under_test.peer_id().into());
}
