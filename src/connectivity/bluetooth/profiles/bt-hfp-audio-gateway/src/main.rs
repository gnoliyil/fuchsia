// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![recursion_limit = "1024"]

use {
    anyhow::{format_err, Error},
    battery_client::BatteryClient,
    fuchsia_component::server::ServiceFs,
    fuchsia_inspect_derive::Inspect,
    futures::{channel::mpsc, future, pin_mut},
    tracing::{debug, error, info, warn},
};

use crate::{
    config::AudioGatewayFeatureSupport, fidl_service::run_services, hfp::Hfp,
    profile::register_audio_gateway,
};

mod a2dp;
mod audio;
mod config;
mod error;
mod features;
mod fidl_service;
mod hfp;
mod inspect;
mod peer;
mod profile;
mod sco_connector;
mod service_definitions;

#[fuchsia::main(logging_tags = ["hfp-ag"])]
async fn main() -> Result<(), Error> {
    let mut fs = ServiceFs::new();

    let inspector = fuchsia_inspect::Inspector::default();
    if let Err(e) = inspect_runtime::serve(&inspector, &mut fs) {
        warn!("Couldn't serve inspect: {}", e);
    }

    let feature_support = AudioGatewayFeatureSupport::load();
    debug!(?feature_support, "Starting HFP Audio Gateway");
    let (profile_client, profile_svc) = register_audio_gateway(feature_support)?;

    // Power integration is optional - the HFP component will function even power integration is
    // unavailable.
    let battery_client = match BatteryClient::create() {
        Err(e) => {
            info!("Power integration unavailable: {:?}", e);
            None
        }
        Ok(batt) => Some(batt),
    };

    let (call_manager_sender, call_manager_receiver) = mpsc::channel(1);
    let (test_request_sender, test_request_receiver) = mpsc::channel(1);

    let audio = match audio::DaiAudioControl::discover().await {
        Err(e) => {
            warn!("Couldn't setup DAI audio: {:?}", e);
            return Err(format_err!("DAI failed"));
        }
        Ok(audio) => audio,
    };

    let mut hfp = Hfp::new(
        profile_client,
        profile_svc,
        battery_client,
        audio,
        call_manager_receiver,
        feature_support,
        test_request_receiver,
    );
    if let Err(e) = hfp.iattach(&inspector.root(), "hfp") {
        warn!("Couldn't attach inspect to HFP: {:?}", e);
    }

    let hfp = hfp.run();
    pin_mut!(hfp);

    let services = run_services(fs, call_manager_sender, test_request_sender);
    pin_mut!(services);

    info!("HFP Audio Gateway running.");

    match future::select(services, hfp).await {
        future::Either::Left((Ok(()), _)) => {
            warn!("Service FS directory handle closed. Exiting.");
        }
        future::Either::Left((Err(e), _)) => {
            error!("Error encountered running Service FS: {}. Exiting", e);
        }
        future::Either::Right((Ok(()), _)) => {
            warn!("All HFP related connections to this component have been disconnected. Exiting.");
        }
        future::Either::Right((Err(e), _)) => {
            error!("Error encountered running main HFP loop: {}. Exiting.", e);
        }
    }

    Ok(())
}
