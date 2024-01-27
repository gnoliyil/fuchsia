// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        doctor::{create_tables, validate_routes, RouteReport},
        query::get_cml_moniker_from_query,
    },
    anyhow::Result,
    fidl_fuchsia_sys2 as fsys,
    moniker::{AbsoluteMoniker, AbsoluteMonikerBase, RelativeMoniker, RelativeMonikerBase},
};

pub async fn doctor_cmd_print<W: std::io::Write>(
    query: String,
    route_validator: fsys::RouteValidatorProxy,
    realm_explorer: fsys::RealmExplorerProxy,
    mut writer: W,
) -> Result<()> {
    let moniker = get_cml_moniker_from_query(&query, &realm_explorer).await?;

    // Convert the absolute moniker into a relative moniker w.r.t. root.
    // LifecycleController expects relative monikers only.
    let relative_moniker = RelativeMoniker::scope_down(&AbsoluteMoniker::root(), &moniker).unwrap();

    writeln!(writer, "Moniker: {}", &moniker)?;

    let reports = validate_routes(&route_validator, relative_moniker).await?;

    let (use_table, expose_table) = create_tables(reports);
    use_table.print(&mut writer)?;
    writeln!(writer, "")?;

    expose_table.print(&mut writer)?;
    writeln!(writer, "")?;

    Ok(())
}

pub async fn doctor_cmd_serialized(
    query: String,
    route_validator: fsys::RouteValidatorProxy,
    realm_explorer: fsys::RealmExplorerProxy,
) -> Result<Vec<RouteReport>> {
    let moniker = get_cml_moniker_from_query(&query, &realm_explorer).await?;

    // Convert the absolute moniker into a relative moniker w.r.t. root.
    // LifecycleController expects relative monikers only.
    let relative_moniker = RelativeMoniker::scope_down(&AbsoluteMoniker::root(), &moniker).unwrap();

    validate_routes(&route_validator, relative_moniker).await
}
