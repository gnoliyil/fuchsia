// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {argh::FromArgs, ffx_core::ffx_command};

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "list-devices",
    description = "Prints all available audio devices on target",
    example = "ffx audio list-devices \n ffx audio --machine json list-devices"
)]
pub struct ListDevicesCommand {}
