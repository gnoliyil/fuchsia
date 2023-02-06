// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Error;
use ffx_core::ffx_plugin;
use ffx_scrutiny_fvm_args::ScrutinyFvmCommand;
use scrutiny_config::{ConfigBuilder, ModelConfig};
use scrutiny_frontend::{command_builder::CommandBuilder, launcher};

#[ffx_plugin()]
pub async fn scrutiny_fvm(cmd: ScrutinyFvmCommand) -> Result<(), Error> {
    // An empty model can be used, because we do not need any artifacts other than the fvm in
    // order to complete the extraction.
    let model = ModelConfig::empty();
    let command = CommandBuilder::new("tool.fvm.extract")
        .param("input", cmd.input)
        .param("output", cmd.output)
        .build();
    let config = ConfigBuilder::with_model(model).command(command).build();
    launcher::launch_from_config(config)?;

    Ok(())
}
