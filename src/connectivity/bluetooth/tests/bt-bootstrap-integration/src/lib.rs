// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context as _},
    bt_test_harness::{
        access::{AccessHarness, AccessState},
        bootstrap::BootstrapHarness,
    },
    fidl_fuchsia_bluetooth_sys as sys,
    fuchsia_bluetooth::expectation::{
        asynchronous::{ExpectableExt, ExpectableStateExt},
        Predicate,
    },
    fuchsia_bluetooth::{
        constants::INTEGRATION_TIMEOUT,
        types::{Address, BondingData, HostData, Identity, LeBondData, OneOrBoth, PeerId},
    },
    std::collections::HashSet,
};

/// An example identity for an Hci Emulator backed host
fn example_emulator_identity() -> Identity {
    // Hci Emulators currently default to address 0
    let emulator_address = Address::Public([0, 0, 0, 0, 0, 0]);
    let peer_address = Address::Public([1, 0, 0, 0, 0, 0]);

    let host_data = HostData {
        irk: Some(sys::Key { value: [16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1] }),
    };
    let csrk = sys::PeerKey {
        data: sys::Key { value: [0; 16] },
        security: sys::SecurityProperties {
            authenticated: false,
            secure_connections: true,
            encryption_key_size: 0,
        },
    };
    let le_data = LeBondData {
        connection_parameters: None,
        services: vec![],
        peer_ltk: None,
        local_ltk: None,
        irk: None,
        csrk: Some(csrk),
    };
    let bonds = vec![BondingData {
        identifier: PeerId(42),
        address: peer_address,
        local_address: emulator_address,
        name: None,
        data: OneOrBoth::Left(le_data),
    }];
    Identity { host: host_data, bonds }
}

#[test_harness::run_singlethreaded_test]
async fn test_add_and_commit_identities((bootstrap, access): (BootstrapHarness, AccessHarness)) {
    let bootstrap = bootstrap.aux().clone();

    let initial_device_empty_pred = Predicate::<AccessState>::predicate(
        |access| access.peers.is_empty(),
        "no peers in access state",
    );
    let _ = access
        .when_satisfied(initial_device_empty_pred, INTEGRATION_TIMEOUT)
        .await
        .expect("initial devices not empty!");
    let identity = example_emulator_identity();

    let expected_peers: HashSet<PeerId> = identity.bonds.iter().map(|b| b.identifier).collect();

    bootstrap.add_identities(&[identity.into()]).context("Error adding identities").unwrap();
    bootstrap
        .commit()
        .await
        .unwrap()
        .map_err(|e| format_err!("Error committing bonds: {:?}", e))
        .unwrap();

    let pred = Predicate::<AccessState>::predicate(
        move |access| expected_peers == access.peers.keys().cloned().collect(),
        "known device identifiers == expected device identifiers",
    );
    let _ = access.when_satisfied(pred, INTEGRATION_TIMEOUT).await.unwrap();
}
