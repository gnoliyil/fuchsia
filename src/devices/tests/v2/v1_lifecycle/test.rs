// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Result,
    fidl_fuchsia_driver_test as fdt, fidl_fuchsia_lifecycle_test as ft, fuchsia_async as fasync,
    fuchsia_component_test::RealmBuilder,
    fuchsia_driver_test::{DriverTestRealmBuilder, DriverTestRealmInstance},
};

#[fasync::run_singlethreaded(test)]
async fn test_devfs_exporter() -> Result<()> {
    // Create the RealmBuilder.
    let builder = RealmBuilder::new().await?;
    builder.driver_test_realm_setup().await?;
    // Build the Realm.
    let instance = builder.build().await?;
    // Start the DriverTestRealm.
    let args = fdt::RealmArgs { ..fdt::RealmArgs::EMPTY };
    instance.driver_test_realm_start(args).await?;
    // Connect to our driver.
    let dev = instance.driver_test_realm_connect_to_dev()?;
    let device = device_watcher::recursive_wait_and_open::<ft::DeviceMarker>(
        &dev,
        "sys/test/root/lifecycle-device",
    )
    .await?;
    device.ping().await?;

    let response = device.get_string().await.unwrap();
    assert_eq!(response, "hello world!");

    device.stop().await?;

    Ok(())
}
