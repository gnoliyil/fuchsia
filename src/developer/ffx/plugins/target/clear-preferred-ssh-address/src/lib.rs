// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{Context as _, Result};
use ffx_core::ffx_plugin;
use ffx_target_clear_preferred_ssh_address_args::ClearPreferredSshAddressCommand;
use fidl_fuchsia_developer_ffx as ffx;

#[ffx_plugin()]
pub async fn clear_preferred_ssh_address(
    target_proxy: ffx::TargetProxy,
    _cmd: ClearPreferredSshAddressCommand,
) -> Result<()> {
    target_proxy
        .clear_preferred_ssh_address()
        .await
        .context("clear_preferred_ssh_address failed")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn setup_fake_target_server() -> ffx::TargetProxy {
        setup_fake_target_proxy(move |req| match req {
            ffx::TargetRequest::ClearPreferredSshAddress { responder } => {
                responder.send().expect("clear_preferred_ssh_address failed");
            }
            r => panic!("got unexpected request: {:?}", r),
        })
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn clear_preferred_ssh_address_invoked() {
        clear_preferred_ssh_address(setup_fake_target_server(), ClearPreferredSshAddressCommand {})
            .await
            .expect("clear_preferred_ssh_address failed");
    }
}
