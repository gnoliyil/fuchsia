// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#![deny(warnings)]

use failure::Error;
use std::fs;
use std::path::PathBuf;
use std::process;
use structopt::StructOpt;

mod format;
mod merge;
mod opts;
mod validate;

fn main() {
    if let Err(msg) = run_cmx() {
        eprintln!("{}", msg);
        process::exit(1);
    }
}

fn run_cmx() -> Result<(), Error> {
    let opt = opts::Opt::from_args();
    match opt.cmd {
        opts::Commands::Validate { files } => validate::validate(files)?,
        opts::Commands::Merge { files, output } => merge::merge(files, output)?,
        opts::Commands::Format {
            file,
            pretty,
            output,
        } => format::format(&file, pretty, output)?,
    }
    if let Some(stamp_path) = opt.stamp {
        stamp(stamp_path)?;
    }
    Ok(())
}

fn stamp(stamp_path: PathBuf) -> Result<(), Error> {
    fs::File::create(stamp_path)?;
    Ok(())
}
