// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// core
mod error;
mod message;
mod node;
mod power_manager;
mod shutdown_request;
mod types;
mod utils;

// nodes
mod activity_handler;
mod cpu_control_handler;
mod cpu_device_handler;
mod cpu_manager;
mod cpu_stats_handler;
mod crash_report_handler;
mod debug_service;
mod dev_control_handler;
mod input_settings_handler;
mod lid_shutdown;
mod platform_metrics;
mod shutdown_watcher;
mod syscall_handler;
mod system_power_mode_handler;
mod system_profile_handler;
mod system_shutdown_handler;
mod temperature_handler;
mod thermal_load_driver;
mod thermal_policy;
mod thermal_shutdown;
mod thermal_state_handler;

#[cfg(test)]
mod test;

use crate::power_manager::PowerManager;
use anyhow::Error;
use fuchsia_async as fasync;
use fuchsia_trace_provider;
use log;

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    // Setup logging
    fuchsia_syslog::init()?;
    log::info!("started");

    // Setup tracing
    fuchsia_trace_provider::trace_provider_create_with_fdio();

    // Set up the PowerManager
    let mut pm = PowerManager::new();

    // This future should never complete
    let result = pm.run().await;
    log::error!("Unexpected exit with result: {:?}", result);
    result
}
