// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;
use ffx_target_forward_sub_command::SubCommand;

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "forward", description = "Forward ports from a target")]
pub struct TargetCommand {
    #[argh(subcommand)]
    pub subcommand: SubCommand,
}
