// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl::endpoints::create_proxy,
    fidl_fuchsia_component::RealmMarker,
    fidl_fuchsia_component_decl::ChildRef,
    fidl_test_ping::PingMarker,
    fuchsia_async as fasync,
    fuchsia_component::client::{connect_to_protocol, connect_to_protocol_at_dir_root},
};

#[fasync::run_singlethreaded(test)]
async fn base_resolver_test() {
    let realm =
        connect_to_protocol::<RealmMarker>().expect("failed to connect to fuchsia.component.Realm");
    let (exposed_dir, server_end) = create_proxy().expect("failed to create proxy");
    realm
        .open_exposed_dir(&ChildRef { name: "base-component".into(), collection: None }, server_end)
        .await
        .expect("failed to call open_exposed_dir FIDL")
        .expect("failed to open exposed dir of child");
    let ping = connect_to_protocol_at_dir_root::<PingMarker>(&exposed_dir)
        .expect("failed to connect to Ping protocol");
    assert_eq!(ping.ping("ping").await.expect("Ping FIDL call failed"), "ping pong");
}

#[fasync::run_singlethreaded(test)]
async fn base_resolver_resolves_subpackages() {
    let realm =
        connect_to_protocol::<RealmMarker>().expect("failed to connect to fuchsia.component.Realm");
    let (exposed_dir, server_end) = create_proxy().expect("failed to create proxy");
    realm
        .open_exposed_dir(
            &ChildRef { name: "base-superpackage-component".into(), collection: None },
            server_end,
        )
        .await
        .expect("failed to call open_exposed_dir FIDL")
        .expect("failed to open exposed dir of child");
    let ping = connect_to_protocol_at_dir_root::<PingMarker>(&exposed_dir)
        .expect("failed to connect to Ping protocol");
    assert_eq!(
        ping.ping("ping").await.expect("Ping FIDL call failed"),
        "forwarded ping and returned ping pong"
    );
}
