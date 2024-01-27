// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;
use ffx_daemon_plugin_sub_command::SubCommand;

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "daemon", description = "Interact with/control the ffx daemon")]
pub struct DaemonCommand {
    #[argh(subcommand)]
    pub subcommand: SubCommand,
}
