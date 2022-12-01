// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::fidl_server::{FidlServer, OutgoingService},
    crate::virtual_machine::FuchsiaVirtualMachine,
    anyhow::Context,
    fuchsia_component::server,
};

mod fidl_server;
mod virtual_machine;

#[fuchsia::main(logging = true, threads = 1)]
async fn main() -> Result<(), anyhow::Error> {
    let mut fs = server::ServiceFs::new();
    fs.dir("svc")
        .add_fidl_service(OutgoingService::Guest)
        .add_fidl_service(OutgoingService::GuestLifecycle);
    fs.take_and_serve_directory_handle().context("Error starting server")?;
    let mut server = FidlServer::<FuchsiaVirtualMachine>::new();
    server.run(fs).await;
    Ok(())
}
