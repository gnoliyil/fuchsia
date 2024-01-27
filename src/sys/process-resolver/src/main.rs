// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{self, Context, Error};
use fidl_fuchsia_process::ResolverRequestStream;
use fuchsia_component::server::ServiceFs;
use futures::prelude::*;
use tracing::info;

// When this feature is enabled, process-resolver will use the pkg-resolver, which can resolve
// packages from base, cache, and universe, to resolve packages.
#[cfg(feature = "auto_update_packages")]
static PACKAGE_RESOLVER_CAPABILITY_PATH: &str = "/svc/fuchsia.pkg.PackageResolver-full";
#[cfg(feature = "auto_update_packages")]
const AUTO_UPDATE_ENABLED: bool = true;

// When this feature is not enabled, process-resolver will use the base_resolver, which can only
// resolve packages from base, to resolve packages.
#[cfg(not(feature = "auto_update_packages"))]
static PACKAGE_RESOLVER_CAPABILITY_PATH: &str = "/svc/fuchsia.pkg.PackageResolver-base";
#[cfg(not(feature = "auto_update_packages"))]
const AUTO_UPDATE_ENABLED: bool = false;

mod resolve;
enum IncomingRequest {
    Resolver(ResolverRequestStream),
}

#[fuchsia::main]
async fn main() -> Result<(), Error> {
    let mut service_fs = ServiceFs::new_local();

    service_fs.dir("svc").add_fidl_service(IncomingRequest::Resolver);

    service_fs
        .take_and_serve_directory_handle()
        .context("failed to serve outgoing namespace")
        .unwrap();

    info!("ProcessResolver initialized [auto_update_enabled: {}]", AUTO_UPDATE_ENABLED);

    service_fs
        .for_each_concurrent(None, |IncomingRequest::Resolver(stream)| async move {
            resolve::serve(stream, PACKAGE_RESOLVER_CAPABILITY_PATH).await;
        })
        .await;

    Ok(())
}
