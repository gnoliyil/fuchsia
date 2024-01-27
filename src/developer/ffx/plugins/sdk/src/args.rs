// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;
use std::path::PathBuf;

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "sdk", description = "Modify or query the installed SDKs")]
pub struct SdkCommand {
    #[argh(subcommand)]
    pub sub: SubCommand,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand)]
pub enum SubCommand {
    Version(VersionCommand),
    Set(SetCommand),
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "version", description = "Retrieve the version of the current SDK")]
pub struct VersionCommand {}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "set", description = "Set sdk-related configuration options")]
pub struct SetCommand {
    #[argh(subcommand)]
    pub sub: SetSubCommand,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand)]
pub enum SetSubCommand {
    Root(SetRootCommand),
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "root", description = "Sets the path to the root of the preferred SDK")]
pub struct SetRootCommand {
    #[argh(positional)]
    /// path to the sdk root
    pub path: PathBuf,
}
