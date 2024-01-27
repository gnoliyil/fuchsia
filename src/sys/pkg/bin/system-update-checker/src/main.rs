// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![warn(clippy::all)]
#![allow(clippy::let_unit_value)]
#![allow(clippy::type_complexity)]

mod apply;
mod channel;
mod channel_handler;
mod check;
mod connect;
mod errors;
mod rate_limiter;
mod update_manager;
mod update_monitor;
mod update_service;

use {
    crate::{
        channel_handler::ChannelHandler,
        update_service::{RealUpdateManager, UpdateService},
    },
    anyhow::{anyhow, Context as _, Error},
    fidl_fuchsia_update_channel::ProviderRequestStream,
    fidl_fuchsia_update_channelcontrol::ChannelControlRequestStream,
    fuchsia_component::server::ServiceFs,
    fuchsia_inspect as finspect,
    futures::{prelude::*, stream::FuturesUnordered},
    std::sync::Arc,
    tracing::{error, warn},
};

const MAX_CONCURRENT_CONNECTIONS: usize = 100;
const DEFAULT_UPDATE_PACKAGE_URL: &str = "fuchsia-pkg://fuchsia.com/update";

#[fuchsia::main(threads = 1, logging_tags = ["system-update-checker"])]
async fn main() -> Result<(), Error> {
    main_inner().await.map_err(|err| {
        // anyhow with alternate formatting prints the Display impl of each error in the chain.
        let err = anyhow!(err);
        error!("error running system-update-checker: {:#}", err);
        err
    })
}

async fn main_inner() -> Result<(), Error> {
    let inspector = finspect::Inspector::default();

    let target_channel_manager =
        channel::TargetChannelManager::new(connect::ServiceConnector, "/config/data");
    if let Err(e) = target_channel_manager.update().await {
        error!("while updating the target channel: {:#}", anyhow!(e));
    }
    let target_channel_manager = Arc::new(target_channel_manager);

    let futures = FuturesUnordered::new();

    let (current_channel_manager, current_channel_notifier) =
        channel::build_current_channel_manager_and_notifier(connect::ServiceConnector).await?;
    futures.push(current_channel_notifier.run().boxed());
    let current_channel_manager = Arc::new(current_channel_manager);

    let (update_manager, update_manager_fut) = RealUpdateManager::new(
        Arc::clone(&target_channel_manager),
        inspector.root().create_child("update-manager"),
    )
    .await
    .start();
    futures.push(update_manager_fut.boxed());

    let mut fs = ServiceFs::new();
    let update_manager_clone = update_manager.clone();
    let channel_handler =
        Arc::new(ChannelHandler::new(current_channel_manager, target_channel_manager));
    let channel_handler_clone = Arc::clone(&channel_handler);
    let channel_handler_provider_clone = Arc::clone(&channel_handler);

    fs.dir("svc")
        .add_fidl_service(move |stream| {
            IncomingServices::Manager(stream, UpdateService::new(update_manager_clone.clone()))
        })
        .add_fidl_service(move |stream| {
            IncomingServices::Provider(stream, Arc::clone(&channel_handler_provider_clone))
        })
        .add_fidl_service(move |stream| {
            IncomingServices::ChannelControl(stream, Arc::clone(&channel_handler_clone))
        });

    inspect_runtime::serve(&inspector, &mut fs)?;

    fs.take_and_serve_directory_handle().context("ServiceFs::take_and_serve_directory_handle")?;
    futures.push(
        fs.for_each_concurrent(MAX_CONCURRENT_CONNECTIONS, |incoming_service| {
            handle_incoming_service(incoming_service)
                .unwrap_or_else(|e| error!("error handling client connection: {:#}", anyhow!(e)))
        })
        .boxed(),
    );

    futures.collect::<()>().await;

    Ok(())
}

enum IncomingServices {
    Manager(fidl_fuchsia_update::ManagerRequestStream, UpdateService),
    Provider(ProviderRequestStream, Arc<ChannelHandler>),
    ChannelControl(ChannelControlRequestStream, Arc<ChannelHandler>),
}

async fn handle_incoming_service(incoming_service: IncomingServices) -> Result<(), Error> {
    match incoming_service {
        IncomingServices::Manager(request_stream, mut update_service) => {
            update_service.handle_request_stream(request_stream).await
        }
        IncomingServices::Provider(request_stream, handler) => {
            handler.handle_provider_request_stream(request_stream).await
        }
        IncomingServices::ChannelControl(request_stream, handler) => {
            handler.handle_control_request_stream(request_stream).await
        }
    }
}
