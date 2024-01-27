// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;
use fidl_fuchsia_developer_ffx::RepositoryStorageType;

#[ffx_command()]
#[derive(FromArgs, PartialEq, Debug)]
#[argh(
    subcommand,
    name = "register",
    description = "Make the target aware of a specific repository"
)]
pub struct RegisterCommand {
    #[argh(option, short = 'r')]
    /// register this repository, rather than the default.
    pub repository: Option<String>,

    /// enable persisting this repository across reboots.
    #[argh(option, from_str_fn(parse_storage_type))]
    pub storage_type: Option<RepositoryStorageType>,

    /// set up a rewrite rule mapping each `alias` host to
    /// to the repository identified by `name`.
    #[argh(option)]
    pub alias: Vec<String>,
}

fn parse_storage_type(arg: &str) -> Result<RepositoryStorageType, String> {
    match arg {
        "ephemeral" => Ok(RepositoryStorageType::Ephemeral),
        "persistent" => Ok(RepositoryStorageType::Persistent),
        _ => Err(format!("unknown storage type {}", arg)),
    }
}
