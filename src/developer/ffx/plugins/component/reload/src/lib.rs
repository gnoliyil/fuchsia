// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(fxbug.dev/120283): Remove. This is force-included as part of ffx_plugin.
use anyhow as _;
use async_trait::async_trait;
use component_debug::cli::reload_cmd;
use errors::FfxError;
use ffx_component::rcs::{connect_to_lifecycle_controller, connect_to_realm_query};
use ffx_component_reload_args::ReloadComponentCommand;
use fho::{FfxMain, FfxTool, SimpleWriter};
use fidl_fuchsia_developer_remotecontrol as rc;

#[derive(FfxTool)]
pub struct ReloadTool {
    #[command]
    cmd: ReloadComponentCommand,
    rcs: rc::RemoteControlProxy,
}

fho::embedded_plugin!(ReloadTool);

#[async_trait(?Send)]
impl FfxMain for ReloadTool {
    type Writer = SimpleWriter;

    async fn main(self, writer: Self::Writer) -> fho::Result<()> {
        let lifecycle_controller = connect_to_lifecycle_controller(&self.rcs).await?;
        let realm_query = connect_to_realm_query(&self.rcs).await?;

        // All errors from component_debug library are user-visible.
        reload_cmd(self.cmd.query, lifecycle_controller, realm_query, writer)
            .await
            .map_err(|e| FfxError::Error(e, 1))?;
        Ok(())
    }
}
