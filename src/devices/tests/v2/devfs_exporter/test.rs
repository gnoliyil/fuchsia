// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Result,
    fidl_fuchsia_devfs_test as ft, fidl_fuchsia_driver_test as fdt, fuchsia_async as fasync,
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
    let args = fdt::RealmArgs {
        root_driver: Some("#meta/root.cm".to_string()),
        use_driver_framework_v2: Some(true),
        ..fdt::RealmArgs::EMPTY
    };
    instance.driver_test_realm_start(args).await?;
    // Connect to our driver.
    let dev = instance.driver_test_realm_connect_to_dev()?;
    let device =
        device_watcher::recursive_wait_and_open::<ft::DeviceMarker>(&dev, "root-device").await?;
    Ok(device.ping().await?)
}
