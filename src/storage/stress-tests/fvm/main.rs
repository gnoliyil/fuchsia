// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod environment;
pub mod instance_actor;
pub mod volume;
pub mod volume_actor;
pub mod vslice;

use {
    argh::FromArgs, environment::FvmEnvironment, fuchsia_async as fasync, stress_test::run_test,
    tracing::Level,
};

#[derive(Clone, Debug, FromArgs)]
/// Creates an instance of fvm and performs stressful operations on it
pub struct Args {
    /// seed to use for this stressor instance
    #[argh(option, short = 's')]
    seed: Option<u64>,

    /// number of operations to complete before exiting.
    #[argh(option, short = 'o')]
    num_operations: Option<u64>,

    /// if set, the test runs for this time limit before exiting successfully.
    #[argh(option, short = 't')]
    time_limit_secs: Option<u64>,

    /// filter logging by level (off, error, warn, info, debug, trace)
    #[argh(option, short = 'l')]
    log_filter: Option<Level>,

    /// number of volumes in FVM.
    /// each volume operates on a different thread and will perform
    /// the required number of operations before exiting.
    /// defaults to 3 volumes.
    #[argh(option, short = 'n', default = "3")]
    num_volumes: u64,

    /// size of one block of the ramdisk (in bytes)
    #[argh(option, default = "512")]
    ramdisk_block_size: u64,

    /// number of blocks in the ramdisk
    /// defaults to 106MiB ramdisk
    #[argh(option, default = "217088")]
    ramdisk_block_count: u64,

    /// size of one slice in FVM (in bytes)
    #[argh(option, default = "32768")]
    fvm_slice_size: u64,

    /// limits the maximum slices in a single extend operation
    #[argh(option, default = "1024")]
    max_slices_in_extend: u64,

    /// controls the density of the partition.
    #[argh(option, default = "65536")]
    max_vslice_count: u64,

    /// controls how often the ramdisk is unbound
    #[argh(option, short = 'd')]
    disconnect_secs: Option<u64>,

    /// parameter passed in by rust test runner
    #[argh(switch)]
    // TODO(fxbug.dev/84729)
    #[allow(unused)]
    nocapture: bool,
}

#[fasync::run_singlethreaded(test)]
async fn test() {
    // Get arguments from command line
    let args: Args = argh::from_env();

    // Initialize logging
    let mut options = diagnostics_log::PublishOptions::default();
    if let Some(filter) = args.log_filter {
        options = options.minimum_severity(filter);
    }
    diagnostics_log::initialize(options).unwrap();

    // Setup the fvm environment
    let env = FvmEnvironment::new(args).await;

    // Run the test
    run_test(env).await;
}
