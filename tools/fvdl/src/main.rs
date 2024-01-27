// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Result;
use fvdl_emulator_common::args::{Args, VDLCommand};
use fvdl_emulator_common::vdl_files::VDLFiles;

#[fuchsia_async::run_singlethreaded]
async fn main() -> Result<()> {
    let Args { command, sdk } = argh::from_env();
    Ok(process_command(command, sdk).await?)
}

async fn process_command(command: VDLCommand, is_sdk: bool) -> Result<()> {
    if !is_sdk {
        println!("fvdl is not longer supported in-tree. Please use `ffx emu start`.");
        std::process::exit(1)
    }
    match command {
        VDLCommand::Start(start_command) => std::process::exit(
            VDLFiles::new(is_sdk, start_command.verbose)?.start_emulator(&start_command).await?,
        ),
        VDLCommand::Kill(stop_command) => {
            VDLFiles::new(is_sdk, false)?.stop_vdl(&stop_command).await?;
        }
    }
    Ok(())
}
