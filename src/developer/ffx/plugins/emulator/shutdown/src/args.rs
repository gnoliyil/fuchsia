// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;

#[ffx_command()]
#[derive(FromArgs, Default, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "shutdown",
    description = "Shut down a running Fuchsia emulator.",
    example = "ffx emu shutdown
ffx emu shutdown --all
ffx emu shutdown fuchsia-emulator --persist",
    note = "By default, the shutdown command will remove an emulator's on-disk
working directory, which contains emulator state, staged image files, etc.

Use the --persist flag if you need to leave the working directory intact while
shutting down the emulator, for debugging or troubleshooting purposes. The
working directory will be left in place, and the emulator will be marked
[Inactive] in `ffx emu list` results until shutdown is called for that instance
without the --persist flag."
)]
pub struct ShutdownCommand {
    /// shut down and clean up all emulator instances running on the device.
    #[argh(switch)]
    pub all: bool,

    /// don't remove the state directory on shutdown, just terminate the emulator.
    #[argh(switch, short = 'p')]
    pub persist: bool,

    /// name of the emulator to shutdown, as specified to the start command.
    /// See a list of available instances by running `ffx emu list`. Default
    /// is "fuchsia-emulator", which matches the default for the start command.
    #[argh(positional, default = "\"fuchsia-emulator\".to_string()")]
    pub name: String,
}
