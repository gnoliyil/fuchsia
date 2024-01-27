// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    fidl_fuchsia_location_namedplace::{
        RegulatoryRegionConfiguratorMarker, RegulatoryRegionConfiguratorProxy as ConfigProxy,
        RegulatoryRegionWatcherMarker, RegulatoryRegionWatcherProxy as WatcherProxy,
    },
    fidl_fuchsia_sys2 as fsys2,
    fuchsia_component::client::connect_to_protocol_at_dir_root,
    fuchsia_component_test::{Capability, ChildOptions, RealmBuilder, RealmInstance, Ref, Route},
};

const REGION_COMPONENT_NAME: &str = "regulatory_region";

#[fuchsia::test]
async fn from_none_state_sending_get_region_then_set_yields_expected_region() -> Result<(), Error> {
    // Set up handles.
    let test_context = new_test_context().await?;
    let (configurator, watcher) = (&test_context.configurator, &test_context.watcher);

    // Get the initial value so that it doesn't matter whether set or get is handled first in the
    // rest of the test.
    assert_eq!(None, watcher.get_region_update().await?);

    // **Caution**
    //
    // * Because `get_region_update()` and `set_region()` are sent on separate channels, we don't know the
    //   order in which they'll arrive at the service.
    // * Additionally, we don't have any guarantees about the order in which the service will
    //   process these messages.
    //
    // Consequently, it is non-deterministic whether this test exercises the hanging-get case, or
    // the update-already-available case.
    //
    //  Note, however, that we _do_ expect the `get_region_update()` request to be _sent_ before the
    // `set_region()` request, as the FIDL bindings send the request before returning the Future.
    const REGION: &'static str = "AA";
    let watch = watcher.get_region_update();
    configurator.set_region(REGION)?;
    assert_eq!(Some(REGION.to_string()), watch.await?);
    Ok(())
}

#[fuchsia::test]
async fn from_none_state_sending_set_then_get_region_yields_expected_region() -> Result<(), Error> {
    // Set up handles.
    let test_context = new_test_context().await?;
    let (configurator, watcher) = (&test_context.configurator, &test_context.watcher);

    // Get the initial value so that it doesn't matter whether set or get is handled first in the
    // rest of the test.
    assert_eq!(None, watcher.get_region_update().await?);

    // **Caution**
    //
    // * Because `get_region_update()` and `set_region()` are sent on separate channels, we don't know the
    //   order in which they'll arrive at the service.
    // * Additionally, we don't have any guarantees about the order in which the service will
    //   process these messages.
    //
    // Consequently, it is non-deterministic whether this test exercises the hanging-get case, or
    // the update-already-available case.
    const REGION: &'static str = "AA";
    configurator.set_region(REGION)?;
    assert_eq!(Some(REGION.to_string()), watcher.get_region_update().await?);
    Ok(())
}

#[fuchsia::test]
async fn from_some_state_sending_get_region_then_set_yields_expected_region() -> Result<(), Error> {
    // Set up handles.
    let test_context = new_test_context().await?;
    let (configurator, watcher) = (&test_context.configurator, &test_context.watcher);

    // Get the initial value so that it doesn't matter whether set or get is handled first in the
    // rest of the test.
    assert_eq!(None, watcher.get_region_update().await?);

    // Move the service from the None state to the Some state.
    const FIRST_REGION: &'static str = "AA";
    configurator.set_region(FIRST_REGION)?;
    watcher.get_region_update().await?;

    // **Caution**
    //
    // * Because `get_region_update()` and `set_region()` are sent on separate channels, we don't know the
    //   order in which they'll arrive at the service.
    // * Additionally, we don't have any guarantees about the order in which the service will
    //   process these messages.
    //
    // Consequently, it is non-deterministic whether this test exercises the hanging-get case, or
    // the update-already-available case.
    //
    // Note, however, that we _do_ expect the `get_region_update()` request to be _sent_ before the
    // `set_region()` request, as the FIDL bindings send the request before returning the Future.
    const SECOND_REGION: &'static str = "BB";
    let watch = watcher.get_region_update();
    configurator.set_region(SECOND_REGION)?;
    assert_eq!(Some(SECOND_REGION.to_string()), watch.await?);
    Ok(())
}

#[fuchsia::test]
async fn from_some_state_sending_set_then_get_region_yields_expected_region() -> Result<(), Error> {
    // Set up handles.
    let test_context = new_test_context().await?;
    let (configurator, watcher) = (&test_context.configurator, &test_context.watcher);

    // Get the initial value so that it doesn't matter whether set or get is handled first in the
    // rest of the test.
    assert_eq!(None, watcher.get_region_update().await?);

    // Move the service from the None state to the Some state.
    const FIRST_REGION: &'static str = "AA";
    configurator.set_region(FIRST_REGION)?;
    watcher.get_region_update().await?;

    // **Caution**
    //
    // * Because `get_region_update()` and `set_region()` are sent on separate channels, we don't know the
    //   order in which they'll arrive at the service.
    // * Additionally, we don't have any guarantees about the order in which the service will
    //   process these messages.
    //
    // Consequently, it is non-deterministic whether this test exercises the hanging-get case, or
    // the update-already-available case.
    const SECOND_REGION: &'static str = "BB";
    configurator.set_region(SECOND_REGION)?;
    assert_eq!(Some(SECOND_REGION.to_string()), watcher.get_region_update().await?);
    Ok(())
}

#[fuchsia::test]
async fn from_none_state_sending_get_region_yields_none() -> Result<(), Error> {
    // Set up handles.
    let test_context = new_test_context().await?;
    let (_configurator, watcher) = (&test_context.configurator, &test_context.watcher);

    // The initial update before setting anything should be None.
    assert_eq!(None, watcher.get_region_update().await?);
    Ok(())
}

#[fuchsia::test]
async fn from_some_state_reloading_service_yields_expected_region() -> Result<(), Error> {
    // Set up handles.
    let test_context = new_test_context().await?;
    let (configurator, watcher) = (&test_context.configurator, &test_context.watcher);

    // Get the initial value so that it doesn't matter whether set or get is handled first in the
    // rest of the test. Ignore the value because it depends on what ran previously.
    watcher.get_region_update().await?;

    const SECOND_REGION: &'static str = "CC";
    configurator.set_region(SECOND_REGION)?;
    assert_eq!(Some(SECOND_REGION.to_string()), watcher.get_region_update().await?);

    // Restart the service backing the protocols so that it will read the cached value.
    stop_component(&test_context._realm_instance, REGION_COMPONENT_NAME).await;
    let watcher = &test_context
        ._realm_instance
        .root
        .connect_to_protocol_at_exposed_dir::<RegulatoryRegionWatcherMarker>()
        .context("Failed to connect to Watcher protocol")?;
    assert_eq!(Some(SECOND_REGION.to_string()), watcher.get_region_update().await?);
    Ok(())
}

// Bundles together the handles needed to communicate with the Configurator and Watcher protocols.
// These items are bundled together to ensure that `realm_instance` outlives the
// protocols instances. Without that guarantee, the process backing the protocols my terminate
// prematurely.
struct TestContext {
    _realm_instance: RealmInstance, // May be unread; exists primarily for lifetime management.
    configurator: ConfigProxy,
    watcher: WatcherProxy,
}

async fn stop_component(realm_ref: &RealmInstance, child_name: &str) {
    let lifecycle = connect_to_protocol_at_dir_root::<fsys2::LifecycleControllerMarker>(
        realm_ref.root.get_exposed_dir(),
    )
    .expect("Failed to connect to LifecycleController");
    lifecycle.stop(&format!("./{}", child_name), false).await.unwrap().unwrap();
}

async fn new_test_context() -> Result<TestContext, Error> {
    // Create a new RealmBuilder instance, which we will use to define a new realm
    let builder = RealmBuilder::new().await?;
    let region = builder
        // Add regulatory_region to the realm, which will be fetched with a URL
        .add_child(REGION_COMPONENT_NAME, "#meta/regulatory_region.cm", ChildOptions::new())
        .await?;
    builder
        .add_route(
            Route::new()
                // Route the logsink to `regulatory_region`, so it can inform us of any issues
                .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                // Route the cache
                .capability(Capability::storage("cache"))
                .from(Ref::parent())
                .to(&region),
        )
        .await?;

    // Route the two regulatory fidl services to the realm parent
    builder
        .add_route(
            Route::new()
                .capability(Capability::protocol::<RegulatoryRegionConfiguratorMarker>())
                .capability(Capability::protocol::<RegulatoryRegionWatcherMarker>())
                .from(&region)
                .to(Ref::parent()),
        )
        .await?;

    builder
        .add_route(
            Route::new()
                .capability(Capability::protocol::<fsys2::LifecycleControllerMarker>())
                .from(Ref::framework())
                .to(Ref::parent()),
        )
        .await?;

    // Creates the realm, and add it to the collection to start its execution
    let realm_instance = builder.build().await?;

    // Connects to the two fidl services
    let configurator = realm_instance
        .root
        .connect_to_protocol_at_exposed_dir::<RegulatoryRegionConfiguratorMarker>()
        .context("Failed to connect to Configurator protocol")?;
    let watcher = realm_instance
        .root
        .connect_to_protocol_at_exposed_dir::<RegulatoryRegionWatcherMarker>()
        .context("Failed to connect to Watcher protocol")?;

    Ok(TestContext { _realm_instance: realm_instance, configurator, watcher })
}

// The tests below are for the deprecated get_update function

#[fuchsia::test]
async fn from_none_state_sending_get_then_set_yields_expected_region() -> Result<(), Error> {
    // Set up handles.
    let test_context = new_test_context().await?;
    let (configurator, watcher) = (&test_context.configurator, &test_context.watcher);

    // **Caution**
    //
    // * Because `get_update()` and `set_region()` are sent on separate channels, we don't know the
    //   order in which they'll arrive at the service.
    // * Additionally, we don't have any guarantees about the order in which the service will
    //   process these messages.
    //
    // Consequently, it is non-deterministic whether this test exercises the hanging-get case, or
    // the value-already-available case.
    //
    //  Note, however, that we _do_ expect the `get_update()` request to be _sent_ before the
    // `set_region()` request, as the FIDL bindings send the request before returning the Future.
    const REGION: &'static str = "AA";
    let watch = watcher.get_update();
    configurator.set_region(REGION)?;
    assert_eq!(REGION.to_string(), watch.await?);
    Ok(())
}

#[fuchsia::test]
async fn from_none_state_sending_set_then_get_yields_expected_region() -> Result<(), Error> {
    // Set up handles.
    let test_context = new_test_context().await?;
    let (configurator, watcher) = (&test_context.configurator, &test_context.watcher);

    // **Caution**
    //
    // * Because `get_update()` and `set_region()` are sent on separate channels, we don't know the
    //   order in which they'll arrive at the service.
    // * Additionally, we don't have any guarantees about the order in which the service will
    //   process these messages.
    //
    // Consequently, it is non-deterministic whether this test exercises the hanging-get case, or
    // the value-already-available case.
    const REGION: &'static str = "AA";
    configurator.set_region(REGION)?;
    assert_eq!(REGION.to_string(), watcher.get_update().await?);
    Ok(())
}

#[fuchsia::test]
async fn from_some_state_sending_get_then_set_yields_expected_region() -> Result<(), Error> {
    // Set up handles.
    let test_context = new_test_context().await?;
    let (configurator, watcher) = (&test_context.configurator, &test_context.watcher);

    // Move the service from the None state to the Some state.
    const FIRST_REGION: &'static str = "AA";
    configurator.set_region(FIRST_REGION)?;
    watcher.get_update().await?;

    // **Caution**
    //
    // * Because `get_update()` and `set_region()` are sent on separate channels, we don't know the
    //   order in which they'll arrive at the service.
    // * Additionally, we don't have any guarantees about the order in which the service will
    //   process these messages.
    //
    // Consequently, it is non-deterministic whether this test exercises the hanging-get case, or
    // the value-already-available case.
    //
    // Note, however, that we _do_ expect the `get_update()` request to be _sent_ before the
    // `set_region()` request, as the FIDL bindings send the request before returning the Future.
    const SECOND_REGION: &'static str = "BB";
    let watch = watcher.get_update();
    configurator.set_region(SECOND_REGION)?;
    assert_eq!(SECOND_REGION.to_string(), watch.await?);
    Ok(())
}

#[fuchsia::test]
async fn from_some_state_sending_set_then_get_yields_expected_region() -> Result<(), Error> {
    // Set up handles.
    let test_context = new_test_context().await?;
    let (configurator, watcher) = (&test_context.configurator, &test_context.watcher);

    // Move the service from the None state to the Some state.
    const FIRST_REGION: &'static str = "AA";
    configurator.set_region(FIRST_REGION)?;
    watcher.get_update().await?;

    // **Caution**
    //
    // * Because `get_update()` and `set_region()` are sent on separate channels, we don't know the
    //   order in which they'll arrive at the service.
    // * Additionally, we don't have any guarantees about the order in which the service will
    //   process these messages.
    //
    // Consequently, it is non-deterministic whether this test exercises the hanging-get case, or
    // the value-already-available case.
    const SECOND_REGION: &'static str = "BB";
    configurator.set_region(SECOND_REGION)?;
    assert_eq!(SECOND_REGION.to_string(), watcher.get_update().await?);
    Ok(())
}
