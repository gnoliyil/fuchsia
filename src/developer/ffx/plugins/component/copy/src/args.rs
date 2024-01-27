// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "copy",
    description = "allows copying files to/from a component's namespace. \n\
                   Paths may be a host or remote path and at least two paths must be supplied.",
    example = "To copy from target to host: \n\
               ffx component copy /some/moniker::/path/file./txt /path/on/host/file.txt \n\n\
               To copy to target from host: \n\
               ffx component copy /path/on/host/file.txt /some/moniker::/path/file.txt\n\n\
               To copy from target to target: \n\
               ffx component copy /some/moniker::/path/file.txt /some/moniker::/path/file.txt\n\n\
               To copy using wildcards: \n\
               ffx component copy /some/moniker::/* /some/directory",
    note = "To learn more about the command see https://fuchsia.dev/fuchsia-src/development/sdk/ffx/copy-files-to-and-from-a-component"
)]

pub struct CopyComponentCommand {
    #[argh(positional)]
    /// paths containing a host filepath or a path in a component's namespace.
    pub paths: Vec<String>,
    /// flag used to print messages such as successfully copied paths and paths ignored to the console.
    #[argh(switch, short = 'v')]
    pub verbose: bool,
}
