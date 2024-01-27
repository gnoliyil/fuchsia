// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This program launches `fuchsia.examples.services.BankAccount` service providers and consumes
//! their instances.
//!
//! This program is written as a test so that it can be easily launched with `fx test`.

use {
    fidl_fuchsia_component as fcomponent, fidl_fuchsia_component_decl as fdecl,
    fidl_fuchsia_examples_services as fexamples, fidl_fuchsia_io as fio,
    fuchsia_component::client as fclient, tracing::*,
};

const COLLECTION_NAME: &'static str = "account_providers";

#[fuchsia::test]
async fn read_and_write_to_multiple_service_instances() {
    // Launch two BankAccount providers into the `account_providers` collection.
    let realm = fuchsia_component::client::connect_to_protocol::<fcomponent::RealmMarker>()
        .expect("connect to Realm service");
    start_provider(&realm, "a", "provider-a#meta/default.cm").await;
    start_provider(&realm, "b", "provider-b#meta/default.cm").await;

    let service_dir = fclient::open_service::<fexamples::BankAccountMarker>()
        .expect("failed to open service dir");
    let instances = fuchsia_fs::directory::readdir(&service_dir)
        .await
        .expect("failed to read entries from service_dir")
        .into_iter()
        .map(|dirent| dirent.name);

    // Debit both bank accounts by $5.
    for instance in instances {
        let proxy = fclient::connect_to_service_instance::<fexamples::BankAccountMarker>(&instance)
            .expect("failed to connect to service instance");
        let read_only_account = proxy.connect_to_read_only().expect("read_only protocol");
        let owner = read_only_account.get_owner().await.expect("failed to get owner");
        let initial_balance = read_only_account.get_balance().await.expect("failed to get_balance");
        info!(%owner, balance = %initial_balance, "retrieved account");

        let read_write_account = proxy.connect_to_read_write().expect("read_write protocol");
        assert_eq!(read_write_account.get_owner().await.expect("failed to get_owner"), owner);
        assert_eq!(
            read_write_account.get_balance().await.expect("failed to get_balance"),
            initial_balance
        );
        info!(%owner, "debiting account");
        read_write_account.debit(5).await.expect("failed to debit");
        assert_eq!(
            read_write_account.get_balance().await.expect("failed to get_balance"),
            initial_balance - 5
        );
    }
}

async fn start_provider(
    realm: &fcomponent::RealmProxy,
    name: &str,
    url: &str,
) -> fio::DirectoryProxy {
    info!(%name, %url, "creating BankAccount provider");
    let child_args = fcomponent::CreateChildArgs { numbered_handles: None, ..Default::default() };
    realm
        .create_child(
            &mut fdecl::CollectionRef { name: COLLECTION_NAME.to_string() },
            fdecl::Child {
                name: Some(name.to_string()),
                url: Some(url.to_string()),
                startup: Some(fdecl::StartupMode::Lazy),
                environment: None,
                ..Default::default()
            },
            child_args,
        )
        .await
        .expect("failed to make create_child FIDL call")
        .expect("failed to create_child");

    let (exposed_dir, exposed_dir_server_end) =
        fidl::endpoints::create_proxy::<fio::DirectoryMarker>()
            .expect("failed to create endpoints");
    info!(%name, %url, "open exposed dir of BankAccount provider");
    realm
        .open_exposed_dir(
            &mut fdecl::ChildRef {
                name: name.to_string(),
                collection: Some(COLLECTION_NAME.to_string()),
            },
            exposed_dir_server_end,
        )
        .await
        .expect("failed to make open_exposed_dir FIDL call")
        .expect("failed to open_exposed_dir");
    exposed_dir
}
