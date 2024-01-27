// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This entire library is actually only used to create a test binary, so mark the entire module to
// only build in test configuration
#![cfg(test)]

use {
    anyhow::{format_err, Context as _, Error},
    bt_test_harness::{
        emulator::{self, add_bredr_peer, add_le_peer, default_bredr_peer, default_le_peer},
        host_driver::{
            realm::HostDriverRealm,
            v2::{expectation as host_expectation, HostDriverHarness},
        },
    },
    fidl::endpoints::Proxy as _,
    fidl_fuchsia_bluetooth::{self as fbt, DeviceClass, MAJOR_DEVICE_CLASS_TOY},
    fidl_fuchsia_bluetooth_host as fhost,
    fidl_fuchsia_bluetooth_sys::{self as fsys, TechnologyType},
    fidl_fuchsia_bluetooth_test::{EmulatorSettings, HciError, PeerProxy},
    fuchsia_async::TimeoutExt as _,
    fuchsia_bluetooth::{
        constants::INTEGRATION_TIMEOUT,
        expectation::{
            self,
            asynchronous::{ExpectableExt, ExpectableStateExt},
            peer,
        },
        types::{Address, HostInfo, PeerId},
    },
    hci_emulator_client::Emulator,
    std::convert::TryInto as _,
};

// Tests that creating and destroying a fake HCI device binds and unbinds the bt-host driver.
#[test_harness::run_singlethreaded_test]
async fn test_lifecycle(_: ()) {
    let realm = HostDriverRealm::create().await.unwrap();
    let address = Address::Public([1, 2, 3, 4, 5, 6]);
    let settings = EmulatorSettings {
        address: Some(address.into()),
        hci_config: None,
        extended_advertising: None,
        acl_buffer_settings: None,
        le_acl_buffer_settings: None,
        ..Default::default()
    };

    let dev_dir = realm.dev().unwrap();
    let mut emulator = Emulator::create(dev_dir).await.unwrap();
    let host = emulator.publish_and_wait_for_host(settings).await.unwrap();
    let (proxy, server_end) = fidl::endpoints::create_proxy::<fhost::HostMarker>().unwrap();
    host.open(server_end).unwrap();
    let info: HostInfo = proxy
        .watch_state()
        .await
        .context("Is bt-gap running? If so, try stopping it and re-running these tests")
        .unwrap()
        .try_into()
        .unwrap();

    // The bt-host should have been initialized with the address that we initially configured.
    assert_eq!(info.addresses.as_slice(), &[address]);

    // Remove the bt-hci device and check that the test device is also destroyed.
    emulator.destroy_and_wait().await.unwrap();

    // Check that the bt-host device is also destroyed.
    let _: (fidl::Signals, fidl::Signals) =
        futures::future::try_join(host.as_channel().on_closed(), proxy.as_channel().on_closed())
            .on_timeout(INTEGRATION_TIMEOUT, || panic!("timed out waiting for device to close"))
            .await
            .unwrap();
}

// Tests that the bt-host driver assigns the local name to "fuchsia" when initialized.
#[test_harness::run_singlethreaded_test]
async fn test_default_local_name(harness: HostDriverHarness) {
    const NAME: &str = "fuchsia";
    let _ = harness
        .when_satisfied(emulator::expectation::local_name_is(NAME), INTEGRATION_TIMEOUT)
        .await
        .unwrap();
    let _ =
        host_expectation::host_state(&harness, expectation::host_driver::name(NAME)).await.unwrap();
}

// Tests that the local name assigned to a bt-host is reflected in `AdapterState` and propagated
// down to the controller.
#[test_harness::run_singlethreaded_test]
async fn test_set_local_name(harness: HostDriverHarness) {
    const NAME: &str = "test1234";
    let proxy = harness.aux().host.clone();
    let result = proxy.set_local_name(NAME).await.unwrap();
    assert_eq!(Ok(()), result);

    let _ = harness
        .when_satisfied(emulator::expectation::local_name_is(NAME), INTEGRATION_TIMEOUT)
        .await
        .unwrap();
    let _ =
        host_expectation::host_state(&harness, expectation::host_driver::name(NAME)).await.unwrap();
}

// Tests that the device class assigned to a bt-host gets propagated down to the controller.
#[test_harness::run_singlethreaded_test]
async fn test_set_device_class(harness: HostDriverHarness) {
    let mut device_class = DeviceClass { value: MAJOR_DEVICE_CLASS_TOY + 4 };
    let proxy = harness.aux().host.clone();
    let result = proxy.set_device_class(&mut device_class).await.unwrap();
    assert_eq!(Ok(()), result);

    let _ = harness
        .when_satisfied(emulator::expectation::device_class_is(device_class), INTEGRATION_TIMEOUT)
        .await
        .unwrap();
}

// Tests that host state updates when discoverable mode is turned on.
// TODO(armansito): Test for FakeHciDevice state changes.
#[test_harness::run_singlethreaded_test]
async fn test_discoverable(harness: HostDriverHarness) {
    let proxy = harness.aux().host.clone();

    // Disabling discoverable mode when not discoverable should succeed.
    let result = proxy.set_discoverable(false).await.unwrap();
    assert_eq!(Ok(()), result);

    // Enable discoverable mode.
    let result = proxy.set_discoverable(true).await.unwrap();
    assert_eq!(Ok(()), result);
    let _ = host_expectation::host_state(&harness, expectation::host_driver::discoverable(true))
        .await
        .unwrap();

    // Disable discoverable mode
    let result = proxy.set_discoverable(false).await.unwrap();
    assert_eq!(Ok(()), result);
    let _ = host_expectation::host_state(&harness, expectation::host_driver::discoverable(false))
        .await
        .unwrap();

    // Disabling discoverable mode when not discoverable should succeed.
    let result = proxy.set_discoverable(false).await.unwrap();
    assert_eq!(Ok(()), result);
}

// Tests that host state updates when discovery is started and stopped.
// TODO(armansito): Test for FakeHciDevice state changes.
#[test_harness::run_singlethreaded_test]
async fn test_discovery(harness: HostDriverHarness) {
    let proxy = harness.aux().host.clone();

    // Start discovery. "discovering" should get set to true.
    let result = proxy.start_discovery().await.unwrap();
    assert_eq!(Ok(()), result);
    let _ = host_expectation::host_state(&harness, expectation::host_driver::discovering(true))
        .await
        .unwrap();

    let address = Address::Random([1, 0, 0, 0, 0, 0]);
    let fut = add_le_peer(harness.aux().as_ref(), default_le_peer(&address));
    let _peer = fut.await.unwrap();

    // The host should discover a fake peer.
    let _ = host_expectation::peer(&harness, peer::name("Fake").and(peer::address(address)))
        .await
        .unwrap();

    // Stop discovery. "discovering" should get set to false.
    proxy.stop_discovery().unwrap();
    let _ = host_expectation::host_state(&harness, expectation::host_driver::discovering(false))
        .await
        .unwrap();
}

// Tests that "close" cancels all operations.
// TODO(armansito): Test for FakeHciDevice state changes.
#[test_harness::run_singlethreaded_test]
async fn test_close(harness: HostDriverHarness) {
    // Enable all procedures.
    let proxy = harness.aux().host.clone();
    let result = proxy.start_discovery().await.unwrap();
    assert_eq!(Ok(()), result);
    let result = proxy.set_discoverable(true).await.unwrap();
    assert_eq!(Ok(()), result);

    let active_state = expectation::host_driver::discoverable(true)
        .and(expectation::host_driver::discovering(true));
    let _ = host_expectation::host_state(&harness, active_state).await.unwrap();

    // Close should cancel these procedures.
    proxy.close().unwrap();

    let closed_state_update = expectation::host_driver::discoverable(false)
        .and(expectation::host_driver::discovering(false));

    let _ = host_expectation::host_state(&harness, closed_state_update).await.unwrap();
}

#[test_harness::run_singlethreaded_test]
async fn test_watch_peers(harness: HostDriverHarness) {
    // `HostDriverHarness` internally calls `Host.WatchPeers()` to monitor peers and satisfy peer
    // expectations. `harness.peers()` represents the local cache monitored using this method.
    // Peers should be initially empty.
    assert_eq!(0, harness.write_state().peers().len());

    // Calling `Host.WatchPeers()` directly will hang since the harness already calls this
    // internally. We issue our own request and verify that it gets satisfied later.

    // Add a LE and a BR/EDR peer with the given addresses.
    let le_peer_address = Address::Random([1, 0, 0, 0, 0, 0]);
    let bredr_peer_address = Address::Public([2, 0, 0, 0, 0, 0]);

    let fut = add_le_peer(harness.aux().as_ref(), default_le_peer(&le_peer_address));
    let _le_peer = fut.await.unwrap();
    let fut = add_bredr_peer(harness.aux().as_ref(), default_bredr_peer(&bredr_peer_address));
    let _bredr_peer = fut.await.unwrap();

    // At this stage the fake peers are registered with the emulator but bt-host does not know about
    // them yet. Check that `watch_fut` is still unsatisfied.
    assert_eq!(0, harness.write_state().peers().len());

    // Wait for all fake devices to be discovered.
    let proxy = harness.aux().host.clone();
    let result = proxy.start_discovery().await.unwrap();
    assert_eq!(Ok(()), result);
    let expected_le =
        peer::address(le_peer_address).and(peer::technology(TechnologyType::LowEnergy));
    let expected_bredr =
        peer::address(bredr_peer_address).and(peer::technology(TechnologyType::Classic));

    let _ = host_expectation::peer(&harness, expected_le).await.unwrap();
    let _ = host_expectation::peer(&harness, expected_bredr).await.unwrap();
    assert_eq!(2, harness.write_state().peers().len());
}

#[test_harness::run_singlethreaded_test]
async fn test_connect(harness: HostDriverHarness) {
    let address1 = Address::Random([1, 0, 0, 0, 0, 0]);
    let address2 = Address::Random([2, 0, 0, 0, 0, 0]);
    let fut = add_le_peer(harness.aux().as_ref(), default_le_peer(&address1));
    let _peer1 = fut.await.unwrap();
    let fut = add_le_peer(harness.aux().as_ref(), default_le_peer(&address2));
    let peer2 = fut.await.unwrap();

    // Configure `peer2` to return an error for the connection attempt.
    peer2.assign_connection_status(HciError::ConnectionTimeout).await.unwrap();

    let proxy = harness.aux().host.clone();

    // Start discovery and let bt-host process the fake devices.
    let result = proxy.start_discovery().await.unwrap();
    assert_eq!(Ok(()), result);

    let _ = host_expectation::peer(&harness, peer::address(address1)).await.unwrap();
    let _ = host_expectation::peer(&harness, peer::address(address2)).await.unwrap();

    let peers = harness.write_state().peers().clone();
    assert_eq!(2, peers.len());

    // Obtain bt-host assigned IDs of the devices.
    let success_id = peers
        .iter()
        .find(|x| x.1.address == address1)
        .ok_or(format_err!("success peer not found"))
        .unwrap()
        .0
        .clone();
    let failure_id = peers
        .iter()
        .find(|x| x.1.address == address2)
        .ok_or(format_err!("error peer not found"))
        .unwrap()
        .0
        .clone();
    let mut success_id: fbt::PeerId = success_id.into();
    let mut failure_id: fbt::PeerId = failure_id.into();

    // Connecting to the failure peer should result in an error.
    let status = proxy.connect(&mut failure_id).await.unwrap();
    assert_eq!(Err(fsys::Error::Failed), status);

    // Connecting to the success peer should return success and the peer should become connected.
    let status = proxy.connect(&mut success_id).await.unwrap();
    assert_eq!(Ok(()), status);

    let connected = peer::identifier(success_id.into()).and(peer::connected(true));
    let _ = host_expectation::peer(&harness, connected).await.unwrap();
}

async fn wait_for_test_peer(
    harness: HostDriverHarness,
    address: &Address,
) -> Result<(PeerId, PeerProxy), Error> {
    let fut = add_le_peer(harness.aux().as_ref(), default_le_peer(&address));
    let proxy = fut.await.unwrap();

    // Start discovery and let bt-host process the fake LE peer.
    let host = harness.aux().host.clone();
    let result = host.start_discovery().await.unwrap();
    assert_eq!(Ok(()), result);

    let le_dev = expectation::peer::address(address.clone());
    let _ = host_expectation::peer(&harness, le_dev).await.unwrap();

    let peer_id = harness
        .write_state()
        .peers()
        .iter()
        .find(|(_, p)| p.address == *address)
        .ok_or(format_err!("could not find peer with address: {}", address))
        .unwrap()
        .0
        .clone();
    Ok((peer_id, proxy))
}

// TODO(fxbug.dev/1525) - Add a test for disconnect failure when a connection attempt is outgoing, provided
// that we can provide a manner of doing so that will not flake.

/// Disconnecting from an unknown device should succeed
#[test_harness::run_singlethreaded_test]
async fn test_disconnect_unknown_device(harness: HostDriverHarness) {
    let mut unknown_id = PeerId(0).into();
    let fut = harness.aux().host.disconnect(&mut unknown_id);
    let status = fut.await.unwrap();
    assert_eq!(Ok(()), status);
}

/// Disconnecting from a known, unconnected device should succeed
#[test_harness::run_singlethreaded_test]
async fn test_disconnect_unconnected_device(harness: HostDriverHarness) {
    let address = Address::Random([1, 0, 0, 0, 0, 0]);
    let (id, _proxy) = wait_for_test_peer(harness.clone(), &address).await.unwrap();
    let mut id = id.into();
    let fut = harness.aux().host.disconnect(&mut id);
    let status = fut.await.unwrap();
    assert_eq!(Ok(()), status);
}

/// Disconnecting from a connected device should succeed and result in the device being disconnected
#[test_harness::run_singlethreaded_test]
async fn test_disconnect_connected_device(harness: HostDriverHarness) {
    let address = Address::Random([1, 0, 0, 0, 0, 0]);
    let (id, _proxy) = wait_for_test_peer(harness.clone(), &address).await.unwrap();
    let mut id = id.into();
    let proxy = harness.aux().host.clone();

    let status = proxy.connect(&mut id).await.unwrap();
    assert_eq!(Ok(()), status);

    let connected = peer::address(address).and(peer::connected(true));
    let disconnected = peer::address(address).and(peer::connected(false));

    let _ = host_expectation::peer(&harness, connected).await.unwrap();
    let status = proxy.disconnect(&mut id).await.unwrap();
    assert_eq!(Ok(()), status);

    let _ = host_expectation::peer(&harness, disconnected).await.unwrap();
}

#[test_harness::run_singlethreaded_test]
async fn test_forget(harness: HostDriverHarness) {
    let address = Address::Random([1, 0, 0, 0, 0, 0]);
    let (id, _proxy) = wait_for_test_peer(harness.clone(), &address).await.unwrap();
    let mut id = id.into();
    let proxy = harness.aux().host.clone();

    // Wait for fake peer to be discovered (`wait_for_test_peer` starts discovery).
    let expected_peer = expectation::peer::address(address);
    let _ = host_expectation::peer(&harness, expected_peer.clone()).await.unwrap();

    // Connecting to the peer should return success and the peer should become connected.
    let status = proxy.connect(&mut id).await.unwrap();
    assert_eq!(Ok(()), status);
    let _ = host_expectation::peer(&harness, expected_peer.and(expectation::peer::connected(true)))
        .await
        .unwrap();

    // Forgetting the peer should result in its removal.
    let status = proxy.forget(&mut id).await.unwrap();
    assert_eq!(Ok(()), status);
    host_expectation::no_peer(&harness, id.into()).await.unwrap();

    // TODO(fxbug.dev/1472): Test that the link closes by querying fake HCI.
}
