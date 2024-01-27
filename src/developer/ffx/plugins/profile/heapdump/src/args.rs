// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {argh::FromArgs, ffx_core::ffx_command, ffx_profile_heapdump_sub_command::SubCommand};

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "heapdump", description = "Profile and dump heap memory usage")]
pub struct HeapdumpCommand {
    #[argh(subcommand)]
    pub subcommand: SubCommand,
}
