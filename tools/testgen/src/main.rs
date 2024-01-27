// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod cmd_integration_test;
mod flags;

use anyhow::{bail, Error, Result};

#[fuchsia::main(logging_tags = ["testgen"])]
async fn main() -> Result<(), Error> {
    let flags: flags::Flags = argh::from_env();
    let result = match flags.subcommand {
        flags::Subcommand::IntegrationTest(ref cmd) => cmd.run(&flags).await,
    };
    match result.err() {
        None => Ok(()),
        Some(e) => bail!("failed to run command: {:?}", e),
    }
}
