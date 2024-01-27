// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq, Clone)]
#[argh(
    subcommand,
    name = "wait",
    description = "Wait until able to establish a remote control connection to the target.",
    error_code(1, "Timeout while getting ssh address")
)]

pub struct WaitCommand {
    #[argh(option, short = 't', default = "120")]
    /// the timeout in seconds [default = 120]
    pub timeout: usize,
}
