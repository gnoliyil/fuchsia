// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![warn(clippy::all)]
#![allow(clippy::let_unit_value)]
#![allow(clippy::enum_variant_names)]
#![allow(clippy::from_over_into)]
#![allow(clippy::too_many_arguments)]

use {
    crate::{base_packages::BasePackages, index::PackageIndex},
    anyhow::{anyhow, format_err, Context as _, Error},
    cobalt_sw_delivery_registry as metrics,
    fidl::endpoints::DiscoverableProtocolMarker as _,
    fidl_contrib::{protocol_connector::ConnectedProtocol, ProtocolConnector},
    fidl_fuchsia_io as fio,
    fidl_fuchsia_metrics::{
        MetricEvent, MetricEventLoggerFactoryMarker, MetricEventLoggerProxy, ProjectSpec,
    },
    fidl_fuchsia_update::CommitStatusProviderMarker,
    fuchsia_async as fasync,
    fuchsia_async::Task,
    fuchsia_component::client::connect_to_protocol,
    fuchsia_inspect as finspect,
    futures::join,
    futures::prelude::*,
    std::sync::{atomic::AtomicU32, Arc},
    tracing::{error, info},
    vfs::directory::{entry::DirectoryEntry as _, helper::DirectlyMutable as _},
};

mod base_packages;
mod cache_service;
mod compat;
mod gc_service;
mod index;
mod required_blobs;
mod retained_packages_service;

#[cfg(test)]
mod test_utils;

const COBALT_CONNECTOR_BUFFER_SIZE: usize = 1000;

struct CobaltConnectedService;
impl ConnectedProtocol for CobaltConnectedService {
    type Protocol = MetricEventLoggerProxy;
    type ConnectError = Error;
    type Message = MetricEvent;
    type SendError = Error;

    fn get_protocol(&mut self) -> future::BoxFuture<'_, Result<MetricEventLoggerProxy, Error>> {
        async {
            let (logger_proxy, server_end) =
                fidl::endpoints::create_proxy().context("failed to create proxy endpoints")?;
            let metric_event_logger_factory =
                connect_to_protocol::<MetricEventLoggerFactoryMarker>()
                    .context("Failed to connect to fuchsia::metrics::MetricEventLoggerFactory")?;

            metric_event_logger_factory
                .create_metric_event_logger(
                    ProjectSpec { project_id: Some(metrics::PROJECT_ID), ..Default::default() },
                    server_end,
                )
                .await?
                .map_err(|e| format_err!("Connection to MetricEventLogger refused {e:?}"))?;
            Ok(logger_proxy)
        }
        .boxed()
    }

    fn send_message<'a>(
        &'a mut self,
        protocol: &'a MetricEventLoggerProxy,
        mut msg: MetricEvent,
    ) -> future::BoxFuture<'a, Result<(), Error>> {
        async move {
            let fut = protocol.log_metric_events(&mut std::iter::once(&mut msg));
            fut.await?.map_err(|e| format_err!("Failed to log metric {e:?}"))?;
            Ok(())
        }
        .boxed()
    }
}

// pkg-cache is conceptually a binary, but is linked together as a library with other SWD binaries
// to save space via blob deduplication.
#[fuchsia::main(logging_tags = ["pkg-cache"])]
pub fn main() -> Result<(), Error> {
    fuchsia_trace_provider::trace_provider_create_with_fdio();

    let mut executor = fasync::LocalExecutor::new();
    executor.run_singlethreaded(main_inner().map_err(|err| {
        let err = anyhow!(err);
        error!("error running pkg-cache: {:#}", err);
        err
    }))
}

async fn main_inner() -> Result<(), Error> {
    info!("starting package cache service");
    let inspector = finspect::Inspector::default();

    let use_system_image = {
        let config = pkg_cache_config::Config::take_from_startup_handle();
        inspector.root().record_child("config", |config_node| config.record_inspect(config_node));
        config.use_system_image
    };

    let mut package_index = PackageIndex::new(inspector.root().create_child("index"));
    let blobfs = blobfs::Client::open_from_namespace_rwx().context("error opening blobfs")?;

    let (
        system_image,
        executability_restrictions,
        non_static_allow_list,
        base_packages,
        cache_packages,
    ) = if use_system_image {
        let boot_args = connect_to_protocol::<fidl_fuchsia_boot::ArgumentsMarker>()
            .context("error connecting to fuchsia.boot/Arguments")?;
        let system_image = system_image::SystemImage::new(blobfs.clone(), &boot_args)
            .await
            .context("Accessing contents of system_image package")?;
        inspector.root().record_string("system_image", system_image.hash().to_string());

        let (base_packages_res, cache_packages_res, non_static_allow_list) = join!(
            BasePackages::new(&blobfs, &system_image),
            async {
                let cache_packages =
                    system_image.cache_packages().await.context("reading cache_packages")?;
                index::load_cache_packages(&mut package_index, &cache_packages, &blobfs).await;
                let cache_packages = Arc::new(cache_packages);
                inspector.root().record_lazy_values(
                    "cache-packages",
                    cache_packages.record_lazy_inspect("cache-packages"),
                );
                Ok(cache_packages)
            },
            system_image.non_static_allow_list(),
        );
        let base_packages = base_packages_res.context("loading base packages")?;
        let cache_packages = cache_packages_res.map_or_else(
            |e: anyhow::Error| {
                error!("Failed to load cache packages: {e:#}");
                None
            },
            Some,
        );

        let executability_restrictions = system_image.load_executability_restrictions();

        (
            Some(system_image),
            executability_restrictions,
            non_static_allow_list,
            base_packages,
            cache_packages,
        )
    } else {
        info!("not loading system_image due to structured config");
        inspector.root().record_string("system_image", "ignored");
        (
            None,
            system_image::ExecutabilityRestrictions::Enforce,
            system_image::NonStaticAllowList::empty(),
            BasePackages::empty(),
            None,
        )
    };

    inspector
        .root()
        .record_string("executability-restrictions", format!("{executability_restrictions:?}"));
    inspector
        .root()
        .record_child("non_static_allow_list", |n| non_static_allow_list.record_inspect(n));

    let base_packages = Arc::new(base_packages);
    inspector.root().record_lazy_child("base-packages", base_packages.record_lazy_inspect());
    let non_static_allow_list = Arc::new(non_static_allow_list);
    let package_index = Arc::new(async_lock::RwLock::new(package_index));
    let scope = vfs::execution_scope::ExecutionScope::new();
    let (cobalt_sender, cobalt_fut) = ProtocolConnector::new_with_buffer_size(
        CobaltConnectedService,
        COBALT_CONNECTOR_BUFFER_SIZE,
    )
    .serve_and_log_errors();
    let cobalt_fut = Task::spawn(cobalt_fut);

    // Use VFS to serve the out dir because ServiceFs does not support OPEN_RIGHT_EXECUTABLE and
    // pkgfs/{packages|versions|system} require it.
    let svc_dir = vfs::pseudo_directory! {};
    let cache_inspect_node = inspector.root().create_child("fuchsia.pkg.PackageCache");
    {
        let package_index = Arc::clone(&package_index);
        let blobfs = blobfs.clone();
        let base_packages = Arc::clone(&base_packages);
        let non_static_allow_list = Arc::clone(&non_static_allow_list);
        let scope = scope.clone();
        let cobalt_sender = cobalt_sender.clone();
        let cache_inspect_id = Arc::new(AtomicU32::new(0));
        let cache_get_node = Arc::new(cache_inspect_node.create_child("get"));

        let () = svc_dir
            .add_entry(
                fidl_fuchsia_pkg::PackageCacheMarker::PROTOCOL_NAME,
                vfs::service::host(move |stream: fidl_fuchsia_pkg::PackageCacheRequestStream| {
                    cache_service::serve(
                        Arc::clone(&package_index),
                        blobfs.clone(),
                        Arc::clone(&base_packages),
                        cache_packages.clone(),
                        executability_restrictions,
                        Arc::clone(&non_static_allow_list),
                        scope.clone(),
                        stream,
                        cobalt_sender.clone(),
                        Arc::clone(&cache_inspect_id),
                        Arc::clone(&cache_get_node),
                    )
                    .unwrap_or_else(|e| {
                        error!(
                            "error handling fuchsia.pkg.PackageCache connection: {:#}",
                            anyhow!(e)
                        )
                    })
                }),
            )
            .context("adding fuchsia.pkg/PackageCache to /svc")?;
    }
    {
        let package_index = Arc::clone(&package_index);
        let blobfs = blobfs.clone();

        let () = svc_dir
            .add_entry(
                fidl_fuchsia_pkg::RetainedPackagesMarker::PROTOCOL_NAME,
                vfs::service::host(
                    move |stream: fidl_fuchsia_pkg::RetainedPackagesRequestStream| {
                        retained_packages_service::serve(
                            Arc::clone(&package_index),
                            blobfs.clone(),
                            stream,
                        )
                        .unwrap_or_else(|e| {
                            error!(
                                "error handling fuchsia.pkg/RetainedPackages connection: {:#}",
                                anyhow!(e)
                            )
                        })
                    },
                ),
            )
            .context("adding fuchsia.pkg/RetainedPackages to /svc")?;
    }
    {
        let blobfs = blobfs.clone();
        let base_packages = Arc::clone(&base_packages);
        let package_index = Arc::clone(&package_index);
        let commit_status_provider =
            fuchsia_component::client::connect_to_protocol::<CommitStatusProviderMarker>()
                .context("while connecting to commit status provider")?;

        let () = svc_dir
            .add_entry(
                fidl_fuchsia_space::ManagerMarker::PROTOCOL_NAME,
                vfs::service::host(move |stream: fidl_fuchsia_space::ManagerRequestStream| {
                    gc_service::serve(
                        blobfs.clone(),
                        Arc::clone(&base_packages),
                        Arc::clone(&package_index),
                        commit_status_provider.clone(),
                        stream,
                    )
                    .unwrap_or_else(|e| {
                        error!("error handling fuchsia.space/Manager connection: {:#}", anyhow!(e))
                    })
                }),
            )
            .context("adding fuchsia.space/Manager to /svc")?;
    }

    let out_dir = vfs::pseudo_directory! {
        "svc" => svc_dir,
        "pkgfs" =>
            crate::compat::pkgfs::make_dir(
                Arc::clone(&base_packages),
                Arc::clone(&package_index),
                Arc::clone(&non_static_allow_list),
                executability_restrictions,
                blobfs.clone(),
                system_image,
            )
            .context("serve pkgfs compat directories")?,
        inspect_runtime::DIAGNOSTICS_DIR => inspect_runtime::create_diagnostics_dir(inspector),
    };

    let () = out_dir.open(
        scope.clone(),
        fio::OpenFlags::RIGHT_READABLE
            | fio::OpenFlags::RIGHT_WRITABLE
            | fio::OpenFlags::RIGHT_EXECUTABLE,
        vfs::path::Path::dot(),
        fuchsia_runtime::take_startup_handle(fuchsia_runtime::HandleType::DirectoryRequest.into())
            .context("taking startup handle")?
            .into(),
    );
    let () = scope.wait().await;

    cobalt_fut.await;

    Ok(())
}
