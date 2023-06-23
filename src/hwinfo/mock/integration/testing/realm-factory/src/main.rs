// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod realm_factory_impl;
mod realm_factory_server;
use crate::realm_factory_impl::*;
use crate::realm_factory_server::*;

use {
    anyhow::{Error, Result},
    fidl_server::*,
    fidl_test_mock::RealmFactoryRequestStream,
    fuchsia_component::server::ServiceFs,
    futures::StreamExt,
    tracing::*,
};

enum IncomingService {
    RealmFactory(RealmFactoryRequestStream),
}

#[fuchsia::main(logging = true)]
async fn main() -> Result<(), Error> {
    info!("starting");

    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(IncomingService::RealmFactory);
    fs.take_and_serve_directory_handle()?;
    fs.for_each_concurrent(0, |service: IncomingService| async {
        match service {
            IncomingService::RealmFactory(stream) => {
                let server = RealmFactoryServer::new(RealmFactoryImpl::new());
                serve_async(stream, server).await.expect("serve RealmFactory");
            }
        };
    })
    .await;
    Ok(())
}
