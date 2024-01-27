// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;

#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "list-devices",
    description = "List devices",
    example = "To list all devices:

    $ driver list-devices -v",
    error_code(1, "Failed to connect to the driver development service")
)]
pub struct ListDevicesCommand {
    /// device filter - either an exact topo path, or a fuzzy match on the device's name.
    #[argh(positional)]
    pub device: Option<String>,

    /// if this exists, the user will be prompted for a component to select.
    #[argh(switch, short = 's', long = "select")]
    pub select: bool,

    /// list all device properties.
    #[argh(switch, short = 'v', long = "verbose")]
    pub verbose: bool,

    /// require an exact match on the device filter, instead of a substring match.
    #[argh(switch, long = "exact")]
    pub exact: bool,

    /// return a non-zero exit code if no matching devices are found.
    #[argh(switch, long = "fail-on-missing")]
    pub fail_on_missing: bool,
}
