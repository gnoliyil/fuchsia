// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    argh::FromArgs,
    display_utils::{Controller, PixelFormat},
    fuchsia_async as fasync, fuchsia_trace_provider,
    futures::{
        future::{FutureExt, TryFutureExt},
        select,
    },
    rgb::Rgb888,
};

mod commands;
mod draw;
mod fps;
mod rgb;

/// Top-level list of this tool's command-line arguments
#[derive(FromArgs)]
struct Args {
    #[argh(subcommand)]
    cmd: SubCommands,
}

/// Show information about all currently attached displays
#[derive(FromArgs)]
#[argh(subcommand, name = "info")]
struct InfoArgs {
    /// ID of the display to show
    #[argh(positional)]
    id: Option<u64>,

    /// show the raw FIDL structure contents
    #[argh(switch)]
    fidl: bool,
}

/// Show the active refresh rate for one or more displays
#[derive(FromArgs)]
#[argh(subcommand, name = "vsync")]
struct VsyncArgs {
    /// ID of the display to show
    #[argh(positional)]
    id: Option<u64>,

    /// screen fill color, using CSS hex syntax (rrggbb) without a leading #.
    /// Default to 0000ff.
    #[argh(option, default = "Rgb888{r: 0x00, g: 0x00, b: 0xff}")]
    color: Rgb888,

    /// pixel format. Default to ARGB8888.
    #[argh(option, default = "PixelFormat::Argb8888")]
    pixel_format: PixelFormat,
}

/// Play a double buffered animation using fence synchronization.
#[derive(FromArgs)]
#[argh(subcommand, name = "squares")]
struct SquaresArgs {
    /// ID of the display to play the animation on
    #[argh(positional)]
    id: Option<u64>,
}

#[derive(FromArgs)]
#[argh(subcommand)]
enum SubCommands {
    Info(InfoArgs),
    Vsync(VsyncArgs),
    Squares(SquaresArgs),
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    fuchsia_trace_provider::trace_provider_create_with_fdio();

    let args: Args = argh::from_env();
    let controller = Controller::init().await?;

    let fidl_events_future = controller.handle_events().err_into();
    let cmd_future = async {
        match args.cmd {
            SubCommands::Info(args) => commands::show_display_info(&controller, args.id, args.fidl),
            SubCommands::Vsync(args) => {
                commands::vsync(&controller, args.id, args.color, args.pixel_format).await
            }
            SubCommands::Squares(args) => commands::squares(&controller, args.id).await,
        }
    };

    select! {
        result1 = fidl_events_future.fuse() => result1,
        result2 = cmd_future.fuse() => result2,
    }
}
