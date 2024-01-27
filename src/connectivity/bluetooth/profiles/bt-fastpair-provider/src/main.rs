// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Error;
use fuchsia_component::server::ServiceFs;
use fuchsia_inspect_derive::Inspect;
use futures::{channel::mpsc, future, pin_mut};
use tracing::{debug, info, warn};

mod advertisement;
mod config;
mod fidl_client;
mod fidl_service;
mod gatt_service;
mod host_watcher;
mod message_stream;
mod pairing;
mod provider;
mod types;

use config::Config;
use fidl_service::run_services;
use provider::Provider;

#[fuchsia::main(logging_tags = ["bt-fastpair-provider"])]
async fn main() -> Result<(), Error> {
    let provider_config = Config::load()?;
    debug!("Starting Fast Pair Provider: {:?}", provider_config);

    let mut fs = ServiceFs::new();
    let inspector = fuchsia_inspect::Inspector::default();
    if let Err(e) = inspect_runtime::serve(&inspector, &mut fs) {
        warn!("Couldn't serve inspect: {}", e);
    }

    // Set up the metrics logger.
    let metrics_logger = bt_metrics::MetricsLogger::new();

    let (fidl_service_sender, fidl_service_receiver) = mpsc::channel(1);
    let mut server = Provider::new(provider_config, metrics_logger).await?;
    if let Err(e) = server.iattach(&inspector.root(), "provider") {
        warn!("Couldn't attach inspect to the Provider server: {:?}", e);
    }
    let fast_pair = server.run(fidl_service_receiver);

    let services = run_services(fs, fidl_service_sender);
    pin_mut!(fast_pair, services);
    info!("Fast Pair Provider component running.");

    match future::select(fast_pair, services).await {
        future::Either::Left((result, _)) => {
            warn!("Fast Pair main loop finished: {:?}", result);
        }
        future::Either::Right((result, _)) => {
            warn!("Service FS unexpectedly finished: {:?}. Exiting", result);
        }
    }
    Ok(())
}
