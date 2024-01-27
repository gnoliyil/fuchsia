// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context, Error},
    fidl_fuchsia_fs::{AdminRequestStream, AdminShutdownResponder},
    fidl_fuchsia_io as fio,
    fuchsia_component::server::ServiceFs,
    fuchsia_fatfs::FatFs,
    fuchsia_runtime::HandleType,
    fuchsia_zircon::{self as zx, Status},
    futures::stream::{StreamExt, TryStreamExt},
    remote_block_device::RemoteBlockClientSync,
    std::sync::Arc,
    tracing::error,
    vfs::{execution_scope::ExecutionScope, path::Path},
};

enum Services {
    Admin(AdminRequestStream),
}

async fn handle(
    stream: Services,
    fs: Arc<FatFs>,
    scope: &ExecutionScope,
) -> Result<Option<AdminShutdownResponder>, Error> {
    match stream {
        Services::Admin(mut stream) => {
            while let Some(request) = stream.try_next().await.context("Reading request")? {
                if let Some(shutdown_responder) = fs.handle_admin(scope, request).await {
                    return Ok(Some(shutdown_responder));
                }
            }
        }
    }
    Ok(None)
}

#[fuchsia::main(threads = 10)]
async fn main() -> Result<(), Error> {
    // Open the remote block device.
    let client_end = zx::Channel::from(
        fuchsia_runtime::take_startup_handle(fuchsia_runtime::HandleInfo::new(
            HandleType::User0,
            1,
        ))
        .ok_or(format_err!("Missing device handle"))?,
    )
    .into();
    let remote_block_client = RemoteBlockClientSync::new(client_end)?;
    let device = remote_block_device::Cache::new(remote_block_client)?;

    // VFS initialization.
    let scope = ExecutionScope::new();

    // Start the filesystem and open the root directory.
    let fatfs = FatFs::new(Box::new(device)).map_err(|_| Status::IO)?;
    let (proxy, server) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()?;
    let root = fatfs.get_root()?;
    root.clone().open(
        scope.clone(),
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        Path::dot(),
        server.into_channel().into(),
    );

    // Export the root directory in our outgoing directory.
    let mut fs = ServiceFs::new();
    fs.add_remote("root", proxy);
    fs.add_fidl_service(Services::Admin);
    fs.take_and_serve_directory_handle()?;

    let fatfs = Arc::new(fatfs);

    // Handle all ServiceFs connections. VFS connections will be spawned as separate tasks.
    const MAX_CONCURRENT: usize = 10_000;

    let shutdown_responder = fs
        .map(|r| Ok(r)) // <- so that we can use try_for_each_concurrent.
        .try_for_each_concurrent(MAX_CONCURRENT, |request| async {
            match handle(request, Arc::clone(&fatfs), &scope).await {
                Ok(Some(shutdown_responder)) => return Err(shutdown_responder),
                Ok(None) => {}
                Err(error) => error!(?error),
            }
            Ok(())
        })
        .await;

    // At this point all direct connections to ServiceFs will have been closed (and cannot be
    // resurrected), but before we finish, we must wait for all VFS connections to be closed.
    scope.wait().await;

    root.close().unwrap_or_else(|e| error!("Failed to close root: {:?}", e));

    // Make sure that fatfs has been cleanly shut down.
    fatfs.shut_down().unwrap_or_else(|e| error!("Failed to shutdown fatfs: {:?}", e));

    if let Err(r) = shutdown_responder {
        let _ = r.send();
    }

    Ok(())
}
