// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{device::Device, environment::Environment, matcher, service},
    anyhow::{format_err, Error},
    fs_management::format::DiskFormat,
    futures::{channel::mpsc, StreamExt},
};

pub struct Manager<E> {
    matcher: matcher::Matchers,
    environment: E,
}

impl<E: Environment> Manager<E> {
    pub fn new(
        config: &fshost_config::Config,
        ramdisk_path: Option<String>,
        environment: E,
    ) -> Self {
        Manager { matcher: matcher::Matchers::new(config, ramdisk_path), environment }
    }

    /// The main loop of fshost. Watch for new devices, match them against filesystems we expect,
    /// and then launch them appropriately.
    pub async fn device_handler(
        &mut self,
        device_stream: impl futures::Stream<Item = Box<dyn Device>>,
        mut shutdown_rx: mpsc::Receiver<service::FshostShutdownResponder>,
    ) -> Result<service::FshostShutdownResponder, Error> {
        let mut device_stream = Box::pin(device_stream).fuse();
        loop {
            // Wait for the next device to come in, or the shutdown signal to arrive.
            let mut device = futures::select! {
                responder = shutdown_rx.next() => {
                    let responder = responder
                        .ok_or_else(|| format_err!("shutdown signal stream ended unexpectedly"))?;
                    return Ok(responder);
                },
                maybe_device = device_stream.next() => {
                    if let Some(device) = maybe_device {
                        device
                    } else {
                        anyhow::bail!("block watcher returned none unexpectedly");
                    }
                },
            };

            let content_format = device.content_format().await.unwrap_or(DiskFormat::Unknown);
            tracing::info!(
                topological_path = %device.topological_path(),
                path = %device.path(),
                ?content_format,
                "Matching device"
            );

            match self.matcher.match_device(device.as_mut(), &mut self.environment).await {
                Ok(true) => {}
                // TODO(fxbug.dev/118209): //src/tests/installer and //src/tests/femu look for
                // "/dev/class/block/008 ignored"
                Ok(false) => tracing::info!("{} ignored", device.path()),
                Err(e) => {
                    tracing::error!(
                        path = %device.path(),
                        ?e,
                        "Failed to match device",
                    );
                }
            }
        }
    }

    pub async fn shutdown(self) -> Result<(), Error> {
        Ok(())
    }
}
