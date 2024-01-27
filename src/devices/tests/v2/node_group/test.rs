// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{anyhow, Error, Result},
    fidl::endpoints::DiscoverableProtocolMarker,
    fidl_fuchsia_driver_test as fdt, fidl_fuchsia_nodegroup_test as ft, fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_component_test::{ChildOptions, LocalComponentHandles, RealmBuilder},
    fuchsia_driver_test::{DriverTestRealmBuilder, DriverTestRealmInstance},
    fuchsia_zircon as zx,
    futures::{channel::mpsc, StreamExt, TryStreamExt},
};

const WAITER_NAME: &'static str = "waiter";

async fn waiter_serve(mut stream: ft::WaiterRequestStream, mut sender: mpsc::Sender<()>) {
    while let Some(ft::WaiterRequest::Ack { status, .. }) =
        stream.try_next().await.expect("Stream failed")
    {
        assert_eq!(status, zx::Status::OK.into_raw());
        sender.try_send(()).expect("Sender failed")
    }
}

async fn waiter_component(
    handles: LocalComponentHandles,
    sender: mpsc::Sender<()>,
) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(move |stream: ft::WaiterRequestStream| {
        fasync::Task::spawn(waiter_serve(stream, sender.clone())).detach()
    });
    fs.serve_connection(handles.outgoing_dir)?;
    Ok(fs.collect::<()>().await)
}

#[fasync::run_singlethreaded(test)]
async fn test_nodegroup() -> Result<()> {
    let (sender, mut receiver) = mpsc::channel(1);

    // Create the RealmBuilder.
    let builder = RealmBuilder::new().await?;
    builder.driver_test_realm_setup().await?;
    let waiter = builder
        .add_local_child(
            WAITER_NAME,
            move |handles: LocalComponentHandles| {
                Box::pin(waiter_component(handles, sender.clone()))
            },
            ChildOptions::new(),
        )
        .await?;
    builder.driver_test_realm_add_offer::<ft::WaiterMarker>((&waiter).into()).await?;
    // Build the Realm.
    let instance = builder.build().await?;

    let offers = vec![fdt::Offer {
        protocol_name: ft::WaiterMarker::PROTOCOL_NAME.to_string(),
        collection: fdt::Collection::BootDrivers,
    }];

    // Start the DriverTestRealm.
    let args = fdt::RealmArgs {
        root_driver: Some("#meta/root.cm".to_string()),
        use_driver_framework_v2: Some(true),
        offers: Some(offers),
        ..Default::default()
    };
    instance.driver_test_realm_start(args).await?;

    // Wait for the 3 drivers to call Waiter.Done.
    receiver.next().await.ok_or(anyhow!("Receiver failed"))?;
    receiver.next().await.ok_or(anyhow!("Receiver failed"))?;
    receiver.next().await.ok_or(anyhow!("Receiver failed"))?;
    receiver.next().await.ok_or(anyhow!("Receiver failed"))
}
