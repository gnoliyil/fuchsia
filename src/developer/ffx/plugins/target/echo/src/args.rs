// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "echo", description = "run echo test against the target")]
pub struct EchoCommand {
    #[argh(positional)]
    /// text string to echo back and forth
    pub text: Option<String>,
}
