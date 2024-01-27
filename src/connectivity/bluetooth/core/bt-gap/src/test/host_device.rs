// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Error},
    fidl::endpoints::RequestStream,
    fidl_fuchsia_bluetooth_host::{HostControlHandle, HostMarker, HostRequest, HostRequestStream},
    fidl_fuchsia_bluetooth_sys::HostInfo as FidlHostInfo,
    fuchsia_bluetooth::types::{
        bonding_data::example, Address, BondingData, HostId, HostInfo, Peer, PeerId,
    },
    futures::{future, join, stream::StreamExt},
    parking_lot::RwLock,
    std::sync::Arc,
    test_util::assert_gt,
};

use crate::host_device::{HostDevice, HostListener};

// An impl that ignores all events
impl HostListener for () {
    type PeerUpdatedFut = future::Ready<()>;
    fn on_peer_updated(&mut self, _peer: Peer) -> Self::PeerRemovedFut {
        future::ready(())
    }

    type PeerRemovedFut = future::Ready<()>;
    fn on_peer_removed(&mut self, _id: PeerId) -> Self::PeerRemovedFut {
        future::ready(())
    }

    type HostBondFut = future::Ready<Result<(), anyhow::Error>>;
    fn on_new_host_bond(&mut self, _data: BondingData) -> Self::HostBondFut {
        future::ok(())
    }

    type HostInfoFut = future::Ready<Result<(), anyhow::Error>>;
    fn on_host_updated(&mut self, _info: HostInfo) -> Self::HostInfoFut {
        future::ok(())
    }
}

// Create a HostDevice with a fake channel, set local name and check it is updated
#[fuchsia::test]
async fn host_device_set_local_name() -> Result<(), Error> {
    let (client, server) = fidl::endpoints::create_proxy_and_stream::<HostMarker>()?;
    let address = Address::Public([0, 0, 0, 0, 0, 0]);
    let host = HostDevice::mock(HostId(1), address, "/dev/class/bt-host/test".to_string(), client);
    let expected_name = "EXPECTED_NAME".to_string();
    let info = Arc::new(RwLock::new(host.info()));
    let server = Arc::new(RwLock::new(server));

    // Assign a name and verify that that it gets written to the bt-host over FIDL.
    let set_name = host.set_name(expected_name.clone());
    let expect_fidl = expect_call(server.clone(), |_, e| match e {
        HostRequest::SetLocalName { local_name, responder } => {
            info.write().local_name = Some(local_name);
            responder.send(Ok(()))?;
            Ok(())
        }
        _ => Err(format_err!("Unexpected!")),
    });
    let (set_name_result, expect_result) = join!(set_name, expect_fidl);
    let _ = set_name_result.expect("failed to set name");
    let _ = expect_result.expect("FIDL result unsatisfied");

    refresh_host(host.clone(), server.clone(), info.read().clone()).await;
    let host_name = host.info().local_name.clone();
    assert!(host_name == Some(expected_name));
    Ok(())
}

// Test that we can establish a host discovery session, then stop discovery on the host when
// the session token is dropped
#[fuchsia::test]
async fn test_discovery_session() -> Result<(), Error> {
    let (client, server) = fidl::endpoints::create_proxy_and_stream::<HostMarker>()?;

    let address = Address::Public([0, 0, 0, 0, 0, 0]);
    let host = HostDevice::mock(HostId(1), address, "/dev/class/bt-host/test".to_string(), client);
    let info = Arc::new(RwLock::new(host.info()));
    let server = Arc::new(RwLock::new(server));

    // Simulate request to establish discovery session
    let establish_discovery_session = host.establish_discovery_session();
    let expect_fidl = expect_call(server.clone(), |_, e| match e {
        HostRequest::StartDiscovery { responder } => {
            info.write().discovering = true;
            responder.send(Ok(()))?;
            Ok(())
        }
        _ => Err(format_err!("Unexpected!")),
    });

    let (discovery_result, expect_result) = join!(establish_discovery_session, expect_fidl);
    let session = discovery_result.expect("did not receive discovery session token");
    let _ = expect_result.expect("FIDL result unsatisfied");

    // Assert that host is now marked as discovering
    refresh_host(host.clone(), server.clone(), info.read().clone()).await;
    let is_discovering = host.info().discovering.clone();
    assert!(is_discovering);

    // Simulate drop of discovery session
    let expect_fidl = expect_call(server.clone(), |_, e| match e {
        HostRequest::StopDiscovery { control_handle: _ } => {
            info.write().discovering = false;
            Ok(())
        }
        _ => Err(format_err!("Unexpected!")),
    });
    std::mem::drop(session);
    expect_fidl.await.expect("FIDL result unsatisfied");

    // Assert that host is no longer marked as discovering
    refresh_host(host.clone(), server.clone(), info.read().clone()).await;
    let is_discovering = host.info().discovering.clone();
    assert!(!is_discovering);

    Ok(())
}

// Create a HostDevice with a fake channel, restore bonds, check that the FIDL method is called
// over multiple paginated iterations
#[fuchsia::test(allow_stalls = false)]
async fn host_device_restore_bonds() -> Result<(), Error> {
    let (client, server) = fidl::endpoints::create_proxy_and_stream::<HostMarker>()?;
    let address = Address::Public([0, 0, 0, 0, 0, 0]);
    let host = HostDevice::mock(HostId(1), address, "/dev/class/bt-host/test".to_string(), client);

    // TODO(fxbug.dev/80564): Assume that 256 bonds is enough to cause HostDevice to use multiple
    // FIDL calls to transmit all of the bonds (at this time, the maximum message size is 64 KiB
    // and a fully-populated BondingData without peer service records is close to 700 B).
    let bonds = (0..=255).map(|i| {
        example::bond(Address::Public([i, i, i, i, i, i]), Address::Public([i, i, i, i, i, i]))
    });
    let restore_bonds = host.restore_bonds(bonds.collect());

    // Create testing Host server that responds to each request with a completed future until
    // restore_bonds yields a result
    let mut num_restore_bonds_calls: i32 = 0;
    let run_host = server.take_until(restore_bonds).for_each(|request| {
        match request {
            Ok(HostRequest::RestoreBonds { responder, .. }) => {
                // Succeed always by not sending back any rejected bonds
                let _ = responder.send(&[]);
                num_restore_bonds_calls += 1;
            }
            x => panic!("Expected RestoreBonds Request but got: {:?}", x),
        }
        future::ready(())
    });

    let _ = run_host.await;
    assert_gt!(num_restore_bonds_calls, 1);
    Ok(())
}

// TODO(fxbug.dev/39373): Add host.fidl emulation to bt-fidl-mocks and use that instead.
async fn expect_call<F>(stream: Arc<RwLock<HostRequestStream>>, f: F) -> Result<(), Error>
where
    F: FnOnce(Arc<HostControlHandle>, HostRequest) -> Result<(), Error>,
{
    let control_handle = Arc::new(stream.read().control_handle());
    let mut stream = stream.write();
    if let Some(event) = stream.next().await {
        let event = event?;
        f(control_handle, event)
    } else {
        Err(format_err!("No event received"))
    }
}

// Updates host with new info
async fn refresh_host(host: HostDevice, server: Arc<RwLock<HostRequestStream>>, info: HostInfo) {
    let refresh = host.refresh_test_host_info();
    let expect_fidl = expect_call(server, |_, e| match e {
        HostRequest::WatchState { responder } => {
            responder.send(&FidlHostInfo::from(info))?;
            Ok(())
        }
        _ => Err(format_err!("Unexpected!")),
    });

    let (refresh_result, expect_result) = join!(refresh, expect_fidl);
    let _ = refresh_result.expect("did not receive HostInfo update");
    let _ = expect_result.expect("FIDL result unsatisfied");
}
