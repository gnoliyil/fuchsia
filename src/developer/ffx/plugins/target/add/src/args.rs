// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq, Clone)]
#[argh(
    subcommand,
    name = "add",
    description = "Make the daemon aware of a specific target",
    example = "To add a remote target forwarded via ssh:

    $ ffx target add 127.0.0.1:8022

Or to add a target using its IPV6:

    $ ffx target add fe80::32fd:38ff:fea8:a00a",
    note = "Manually add a target based on its IP address. The command accepts IPV4
or IPV6 addresses, including a port number: `<addr> = <ip addr:port>`.

Typically, the daemon automatically discovers targets as they come online.
However, manually adding a target allows for specifying a port number or
address, often used for remote workflows.

This command will attempt to connect to the target in order to verify that RCS can
be used, allowing for typical FFX related workflows. If you do not wish to use
this, then you can run use the `--nowait` flag to return immediately. This can be
useful for debugging connection issues.

If you send SIGINT (Ctrl-C) to the command before the connection to the target is
verified, the target will be removed. If RCS cannot be connected to (e.g. some
connectivity error is encountered), the target will also be removed.
"
)]

pub struct AddCommand {
    #[argh(positional)]
    /// IP of the target.
    pub addr: String,

    #[argh(switch, short = 'n')]
    /// do not wait for a connection to be verified on the Fuchsia device.
    pub nowait: bool,
}
