// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Result;
use ffx_core::ffx_plugin;
use ffx_lib_args::FfxBuiltIn;
use std::{env, process::Command};

#[ffx_plugin()]
pub async fn help(_cmd: FfxBuiltIn) -> Result<()> {
    let ffx_path = env::current_exe()?;
    Command::new(ffx_path).arg("help").status()?;
    Ok(())
}
