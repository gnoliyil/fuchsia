// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        channel::{CurrentChannelManager, TargetChannelManager},
        rate_limiter::RateLimiterMonotonic,
    },
    anyhow::{anyhow, Context as _, Error},
    fidl_fuchsia_update_channel::{ProviderRequest, ProviderRequestStream},
    fidl_fuchsia_update_channelcontrol::{ChannelControlRequest, ChannelControlRequestStream},
    fuchsia_zircon as zx,
    futures::prelude::*,
    std::sync::Arc,
    tracing::warn,
};

pub(crate) struct ChannelHandler {
    current_channel_manager: Arc<CurrentChannelManager>,
    target_channel_manager: Arc<TargetChannelManager>,
    warn_rate_limiter: RateLimiterMonotonic,
}

const GET_CURRENT_WARN_DELAY: zx::Duration = zx::Duration::from_minutes(30);

impl ChannelHandler {
    pub fn new(
        current_channel_manager: Arc<CurrentChannelManager>,
        target_channel_manager: Arc<TargetChannelManager>,
    ) -> Self {
        Self {
            current_channel_manager,
            target_channel_manager,
            warn_rate_limiter: RateLimiterMonotonic::from_delay(GET_CURRENT_WARN_DELAY),
        }
    }

    pub(crate) async fn handle_provider_request_stream(
        &self,
        mut stream: ProviderRequestStream,
    ) -> Result<(), Error> {
        while let Some(request) =
            stream.try_next().await.context("extracting request from stream")?
        {
            match request {
                ProviderRequest::GetCurrent { responder } => {
                    let channel = self.get_current();
                    responder.send(&channel).context("sending GetCurrent response")?;
                }
            }
        }
        Ok(())
    }

    pub(crate) async fn handle_control_request_stream(
        &self,
        mut stream: ChannelControlRequestStream,
    ) -> Result<(), Error> {
        while let Some(request) =
            stream.try_next().await.context("extracting request from stream")?
        {
            match request {
                ChannelControlRequest::GetCurrent { responder } => {
                    let channel = self.get_current();
                    responder.send(&channel).context("sending GetCurrent response")?;
                }
                ChannelControlRequest::GetTarget { responder } => {
                    if let Some(channel) = self.target_channel_manager.get_target_channel() {
                        responder.send(&channel).context("sending GetTarget response")?;
                    } else {
                        warn!("target channel not available");
                    }
                }
                ChannelControlRequest::GetTargetList { responder } => {
                    let channel_names = self.target_channel_manager.get_channel_list().await?;
                    responder.send(&channel_names).context("sending GetTargetList response")?;
                }
                ChannelControlRequest::SetTarget { channel, responder } => {
                    self.target_channel_manager.set_target_channel(channel);
                    responder.send().context("sending SetTarget response")?;
                }
            }
        }
        Ok(())
    }

    fn get_current(&self) -> String {
        self.current_channel_manager.read_current_channel().unwrap_or_else(|err| {
            self.warn_rate_limiter.rate_limit(|| {
                warn!("error getting current channel: {:#}", anyhow!(err));
            });
            // TODO: Remove this once we have channel in vbmeta (fxbug.dev/39970).
            self.target_channel_manager.get_target_channel().unwrap_or_default()
        })
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fidl::endpoints::create_proxy_and_stream,
        fidl_fuchsia_update_channel::{ProviderMarker, ProviderProxy},
        fidl_fuchsia_update_channelcontrol::{ChannelControlMarker, ChannelControlProxy},
        fuchsia_async as fasync,
        std::{fs, path::Path},
        tempfile::TempDir,
    };

    fn new_test_channel_handler(info_dir: &TempDir) -> ChannelHandler {
        new_test_channel_handler_with_channel(info_dir, "example")
    }

    fn new_test_channel_handler_with_channel(info_dir: &TempDir, channel: &str) -> ChannelHandler {
        ChannelHandler {
            current_channel_manager: Arc::new(CurrentChannelManager::new(channel.to_owned())),
            target_channel_manager: Arc::new(TargetChannelManager::new(
                crate::connect::ServiceConnector,
                info_dir.path(),
            )),
            warn_rate_limiter: RateLimiterMonotonic::from_delay(GET_CURRENT_WARN_DELAY),
        }
    }

    fn spawn_provider_handler(info_dir: &TempDir) -> ProviderProxy {
        let channel_handler = new_test_channel_handler(info_dir);
        spawn_provider_handler_with_channel_handler(channel_handler)
    }
    fn spawn_provider_handler_with_channel_handler(
        channel_handler: ChannelHandler,
    ) -> ProviderProxy {
        let (proxy, stream) =
            create_proxy_and_stream::<ProviderMarker>().expect("create_proxy_and_stream");
        fasync::Task::spawn(async move {
            channel_handler.handle_provider_request_stream(stream).map(|_| ()).await
        })
        .detach();
        proxy
    }

    fn spawn_channel_handler(info_dir: &TempDir) -> ChannelControlProxy {
        let channel_handler = new_test_channel_handler(info_dir);
        let (proxy, stream) =
            create_proxy_and_stream::<ChannelControlMarker>().expect("create_proxy_and_stream");
        fasync::Task::spawn(async move {
            channel_handler.handle_control_request_stream(stream).map(|_| ()).await
        })
        .detach();
        proxy
    }

    fn create_info_dir_with_channel(file_name: impl AsRef<Path>) -> TempDir {
        let tempdir = TempDir::new().expect("create tempdir");
        let path = tempdir.path().join(file_name);
        fs::write(&path, r#"{"version":"1","content":{"legacy_amber_source_name":"example"}}"#)
            .unwrap_or_else(|_| panic!("write {path:?}"));
        tempdir
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_fidl_get_current_works() {
        let tempdir = create_info_dir_with_channel("current_channel.json");
        let proxy = spawn_provider_handler(&tempdir);

        let res = proxy.get_current().await;

        assert_eq!(res.map_err(|e| e.to_string()), Ok("example".into()));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_fidl_channel_control_get_current_works() {
        let tempdir = create_info_dir_with_channel("current_channel.json");
        let proxy = spawn_channel_handler(&tempdir);

        let res = proxy.get_current().await;

        assert_eq!(res.map_err(|e| e.to_string()), Ok("example".into()));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_fidl_get_current_return_empty_string_if_current_channel_missing() {
        let tempdir = TempDir::new().expect("create tempdir");
        let proxy = spawn_provider_handler_with_channel_handler(
            new_test_channel_handler_with_channel(&tempdir, ""),
        );

        let res = proxy.get_current().await;

        assert_eq!(res.map_err(|e| e.to_string()), Ok("".into()));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_fidl_set_get_target_works() {
        let tempdir = TempDir::new().expect("create tempdir");
        let proxy = spawn_channel_handler(&tempdir);

        proxy.set_target("target-channel").await.unwrap();

        assert_eq!(proxy.get_target().await.unwrap(), "target-channel");
    }
}
