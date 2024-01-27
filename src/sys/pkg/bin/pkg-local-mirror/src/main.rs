// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{anyhow, Context as _},
    fidl_fuchsia_io as fio,
    fidl_fuchsia_pkg::LocalMirrorRequestStream,
    fuchsia_component::server::ServiceFs,
    futures::{future::TryFutureExt as _, StreamExt as _},
    pkg_local_mirror::PkgLocalMirror,
    tracing::{info, warn},
};

const USB_DIR_PATH: &str = "/usb/0/fuchsia_pkg";

#[fuchsia::main(logging_tags = ["pkg-local-mirror"])]
async fn main() -> Result<(), anyhow::Error> {
    info!("starting pkg-local-mirror");

    // TODO(fxbug.dev/59830): Get handle to USB directory using fuchsia.fs/Admin.GetRoot.
    let pkg_local_mirror = {
        let usb_dir =
            fuchsia_fs::directory::open_in_namespace(USB_DIR_PATH, fio::OpenFlags::RIGHT_READABLE)
                .with_context(|| format!("while opening usb dir: {}", USB_DIR_PATH))?;
        PkgLocalMirror::new(&usb_dir).await.context("creating PkgLocalMirror")?
    };

    let mut fs = ServiceFs::new_local();
    let _ = fs.take_and_serve_directory_handle().context("serving directory handle")?;

    fs.dir("svc").add_fidl_service(IncomingService::LocalMirror);

    let () = fs
        .for_each_concurrent(None, |incoming_service| match incoming_service {
            IncomingService::LocalMirror(stream) => {
                pkg_local_mirror.handle_request_stream(stream).unwrap_or_else(|e| {
                    warn!("error handling fuchsia.pkg/LocalMirror request stream {:#}", anyhow!(e))
                })
            }
        })
        .await;

    Ok(())
}

enum IncomingService {
    LocalMirror(LocalMirrorRequestStream),
}
