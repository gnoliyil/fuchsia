// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl::endpoints, fidl_fidl_test_components as ftest, fidl_fuchsia_component as fcomponent,
    fidl_fuchsia_component_decl as fdecl, fidl_fuchsia_io as fio, fuchsia_async as fasync,
    fuchsia_component::client, fuchsia_zircon as zx, tracing::*,
};

#[fuchsia::main]
async fn main() {
    info!("Started collection realm");
    let realm = client::connect_to_protocol::<fcomponent::RealmMarker>()
        .expect("could not connect to Realm service");

    // Create a "trigger realm" child component.
    info!("Creating child");
    {
        let mut collection_ref = fdecl::CollectionRef { name: "coll".to_string() };
        let child_decl = fdecl::Child {
            name: Some("parent".to_string()),
            url: Some("#meta/trigger_realm.cm".to_string()),
            startup: Some(fdecl::StartupMode::Lazy),
            environment: None,
            ..fdecl::Child::EMPTY
        };
        realm
            .create_child(&mut collection_ref, child_decl, fcomponent::CreateChildArgs::EMPTY)
            .await
            .unwrap_or_else(|e| panic!("create_child failed: {:?}", e))
            .unwrap_or_else(|e| panic!("failed to create child: {:?}", e));
    }

    // Start the child, causing its eager children to start as well.
    info!("Starting the child");
    {
        let mut child_ref =
            fdecl::ChildRef { name: "parent".to_string(), collection: Some("coll".to_string()) };
        let (dir, server_end) = endpoints::create_proxy::<fio::DirectoryMarker>().unwrap();
        realm
            .open_exposed_dir(&mut child_ref, server_end)
            .await
            .unwrap_or_else(|e| panic!("open_exposed_dir failed: {:?}", e))
            .unwrap_or_else(|e| panic!("failed to open exposed dir of child: {:?}", e));
        let trigger = client::connect_to_protocol_at_dir_root::<ftest::TriggerMarker>(&dir)
            .expect("failed to open trigger service");
        trigger.run().await.expect("trigger failed");
    }

    // Destroy the child.
    info!("Destroying child");
    {
        let mut child_ref =
            fdecl::ChildRef { name: "parent".to_string(), collection: Some("coll".to_string()) };
        realm
            .destroy_child(&mut child_ref)
            .await
            .expect("destroy_child failed")
            .expect("failed to destroy child");
    }

    info!("Done");
    loop {
        fasync::Timer::new(fasync::Time::after(zx::Duration::from_hours(1))).await;
    }
}
