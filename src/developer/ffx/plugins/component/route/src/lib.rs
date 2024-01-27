// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Result;
use component_debug::{cli, route::RouteReport};
use errors::FfxError;
use ffx_component::rcs;
use ffx_component_route_args::RouteCommand;
use ffx_core::ffx_plugin;
use ffx_writer::Writer;
use fidl_fuchsia_developer_remotecontrol as rc;

#[ffx_plugin]
pub async fn cmd(
    rcs_proxy: rc::RemoteControlProxy,
    args: RouteCommand,
    #[ffx(machine = RouteReport)] writer: Writer,
) -> Result<()> {
    let realm_query = rcs::connect_to_realm_query(&rcs_proxy).await?;
    let route_validator = rcs::connect_to_route_validator(&rcs_proxy).await?;

    // All errors from component_debug library are user-visible.
    if writer.is_machine() {
        let output =
            cli::route_cmd_serialized(args.target, args.filter, route_validator, realm_query)
                .await
                .map_err(|e| FfxError::Error(e, 1))?;
        writer.machine(&output)
    } else {
        cli::route_cmd_print(args.target, args.filter, route_validator, realm_query, writer)
            .await
            .map_err(|e| FfxError::Error(e, 1))?;
        Ok(())
    }
}
