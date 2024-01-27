// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    component_events::{events::*, matcher::*},
    fidl::endpoints::{create_proxy, Proxy},
    fidl_fuchsia_component as fcomponent, fidl_fuchsia_component_decl as fdecl,
    fidl_fuchsia_io as fio, fidl_fuchsia_sys2 as fsys,
    fuchsia_component::client::connect_to_protocol,
};

#[fuchsia::main]
async fn main() {
    // Create the dynamic child
    let realm = connect_to_protocol::<fcomponent::RealmMarker>().unwrap();
    let mut collection_ref = fdecl::CollectionRef { name: String::from("coll") };
    let child_decl = fdecl::Child {
        name: Some(String::from("storage_user")),
        url: Some(String::from("#meta/storage_user.cm")),
        startup: Some(fdecl::StartupMode::Lazy),
        environment: None,
        ..Default::default()
    };

    realm
        .create_child(&mut collection_ref, child_decl, fcomponent::CreateChildArgs::default())
        .await
        .unwrap()
        .unwrap();

    // Start child
    let mut child_ref =
        fdecl::ChildRef { name: "storage_user".to_string(), collection: Some("coll".to_string()) };
    let (exposed_dir, server_end) = create_proxy::<fio::DirectoryMarker>().unwrap();

    realm.open_exposed_dir(&mut child_ref, server_end).await.unwrap().unwrap();
    let _ = fuchsia_component::client::connect_to_protocol_at_dir_root::<fcomponent::BinderMarker>(
        &exposed_dir,
    )
    .expect("failed to connect to fuchsia.component.Binder");

    let mut event_stream = EventStream::open().await.unwrap();

    // Expect the dynamic child to stop
    EventMatcher::ok()
        .stop(Some(ExitStatusMatcher::Clean))
        .moniker("./coll:storage_user")
        .wait::<Stopped>(&mut event_stream)
        .await
        .unwrap();

    // Destroy the child
    realm.destroy_child(&mut child_ref).await.unwrap().unwrap();

    // Expect the dynamic child to be destroyed
    EventMatcher::ok()
        .moniker("./coll:storage_user")
        .wait::<Destroyed>(&mut event_stream)
        .await
        .unwrap();

    // Ensure that memfs does not have a directory for the dynamic child
    let realm_query =
        fuchsia_component::client::connect_to_protocol::<fsys::RealmQueryMarker>().unwrap();
    let (exposed_dir, server_end) = create_proxy().unwrap();
    realm_query
        .open(
            "./memfs",
            fsys::OpenDirType::ExposedDir,
            fio::OpenFlags::RIGHT_READABLE,
            fio::ModeType::empty(),
            ".",
            server_end,
        )
        .await
        .unwrap()
        .unwrap();
    let exposed_dir = fio::DirectoryProxy::new(exposed_dir.into_channel().unwrap());
    let memfs_dir = fuchsia_fs::directory::open_directory(
        &exposed_dir,
        "memfs",
        fio::OpenFlags::RIGHT_READABLE,
    )
    .await
    .unwrap();
    let entries = fuchsia_fs::directory::readdir(&memfs_dir).await.unwrap();
    assert!(entries.is_empty());
}
