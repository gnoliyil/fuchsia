// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "run",
    description = "Creates and starts a component instance in an existing collection
within the component topology.",
    example = "To create a component instance from the `hello-world-rust` component URL:

    $ ffx component run /core/ffx-laboratory:hello-world fuchsia-pkg://fuchsia.com/hello-world#meta/hello-world-rust.cm",
    note = "This command is a shorthand for the following:

    $ ffx component create <moniker> <component-url>
    $ ffx component start <moniker>

To learn more about running components, see https://fuchsia.dev/go/components/run"
)]

pub struct RunComponentCommand {
    #[argh(positional)]
    /// moniker of a component instance in an existing collection.
    /// The component instance will be added to the collection.
    pub moniker: String,

    #[argh(positional)]
    /// url of the component to create and then start.
    pub url: String,

    #[argh(switch, short = 'r')]
    /// destroy and recreate the component instance if it already exists
    pub recreate: bool,

    #[argh(switch, short = 'f')]
    /// start printing logs from the started component after it has started
    pub follow_logs: bool,
}
