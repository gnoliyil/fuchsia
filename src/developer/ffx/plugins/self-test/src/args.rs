// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;
use ffx_selftest_sub_command::SubCommand;

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "self-test", description = "Execute the ffx self-test (e2e) suite")]
pub struct SelftestCommand {
    #[argh(
        option,
        default = "280",
        description = "maximum runtime of entire test suite in seconds"
    )]
    pub timeout: u64,

    #[argh(
        option,
        default = "60",
        description = "maximum run time of a single test case in seconds"
    )]
    pub case_timeout: u64,

    #[argh(option, default = "true", description = "include target interaction tests")]
    pub include_target: bool,

    #[argh(option, description = "filter test cases")]
    pub filter: Option<String>,

    #[argh(subcommand)]
    pub subcommand: Option<SubCommand>,
}
