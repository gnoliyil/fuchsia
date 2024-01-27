// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{Context as _, Result};
use async_trait::async_trait;
use ffx_daemon_core::events::Queue;
use ffx_daemon_events::{DaemonEvent, TargetEvent};
use ffx_daemon_target::{target::Target, target_collection::TargetCollection};
use fidl::endpoints::Proxy;
use fidl_fuchsia_developer_ffx as ffx;
use fidl_fuchsia_developer_remotecontrol::RemoteControlProxy;
use std::rc::Rc;

#[async_trait(?Send)]
pub trait DaemonProtocolProvider {
    /// Opens a proxy for the given protocol. Returns a channel to the proxy.
    async fn open_protocol(&self, protocol_name: String) -> Result<fidl::Channel>;

    async fn open_target_proxy(
        &self,
        target_identifier: Option<String>,
        moniker: &str,
        capability_name: &str,
    ) -> Result<fidl::Channel>;

    async fn open_remote_control(
        &self,
        _target_identifier: Option<String>,
    ) -> Result<RemoteControlProxy> {
        unimplemented!()
    }

    /// Identical to open_target_proxy, but also returns a target info struct.
    async fn open_target_proxy_with_info(
        &self,
        target_identifier: Option<String>,
        moniker: &str,
        capability_name: &str,
    ) -> Result<(ffx::TargetInfo, fidl::Channel)>;

    async fn get_target_info(&self, target_identifier: Option<String>) -> Result<ffx::TargetInfo>;

    async fn get_target_event_queue(
        &self,
        _target_identifier: Option<String>,
    ) -> Result<(Rc<Target>, Queue<TargetEvent>)> {
        unimplemented!()
    }

    /// Returns a clone of the daemon event queue.
    async fn daemon_event_queue(&self) -> Queue<DaemonEvent> {
        unimplemented!()
    }

    /// Returns a copy of the daemon target collection.
    async fn get_target_collection(&self) -> Result<Rc<TargetCollection>> {
        unimplemented!()
    }
}

/// A struct containing the current protocol's active context when invoking the
/// handle function. This is intended to interface with the Daemon.
#[derive(Clone)]
pub struct Context {
    inner: Rc<dyn DaemonProtocolProvider>,
}

impl Context {
    pub fn new(t: impl DaemonProtocolProvider + 'static) -> Self {
        Self { inner: Rc::new(t) }
    }

    pub async fn open_target_proxy<P>(
        &self,
        target_identifier: Option<String>,
        moniker: &'static str,
    ) -> Result<P::Proxy>
    where
        P: fidl::endpoints::DiscoverableProtocolMarker,
    {
        let (_, proxy) = self.open_target_proxy_with_info::<P>(target_identifier, moniker).await?;
        Ok(proxy)
    }

    pub async fn open_target_proxy_with_info<P>(
        &self,
        target_identifier: Option<String>,
        moniker: &'static str,
    ) -> Result<(ffx::TargetInfo, P::Proxy)>
    where
        P: fidl::endpoints::DiscoverableProtocolMarker,
    {
        let (info, channel) = self
            .inner
            .open_target_proxy_with_info(target_identifier, moniker, P::PROTOCOL_NAME)
            .await?;
        let proxy = P::Proxy::from_channel(
            fidl::AsyncChannel::from_channel(channel).context("making async channel")?,
        );
        Ok((info, proxy))
    }

    pub async fn get_target_info(
        &self,
        target_identifier: Option<String>,
    ) -> Result<ffx::TargetInfo> {
        self.inner.get_target_info(target_identifier).await
    }

    /// Returns a copy of the target event queue, as well as a reference to the
    /// target that matched the query.
    ///
    /// Note: DO NOT use this unless you know what you're doing. This is
    /// placeholder code in lieu of completing the target_collection protocol
    /// API, and should not be used unless it is a blocker.
    pub async fn get_target_event_queue(
        &self,
        target_identifier: Option<String>,
    ) -> Result<(Rc<Target>, Queue<TargetEvent>)> {
        self.inner.get_target_event_queue(target_identifier).await
    }

    pub async fn daemon_event_queue(&self) -> Queue<DaemonEvent> {
        self.inner.daemon_event_queue().await
    }

    pub async fn open_remote_control(
        &self,
        target_identifier: Option<String>,
    ) -> Result<RemoteControlProxy> {
        self.inner.open_remote_control(target_identifier).await
    }

    /// Opens a protocol proxy on the daemon using the ConnectToProtocol function.
    ///
    /// This is not intended to be directly called by the user at risk of
    /// causing circular dependencies, so behavior when calling this directly
    /// is undefined.
    ///
    /// If you need to open another protocol from within your own protocol,
    /// first declare a dependency to the protocol endpoint in the `ffx_protocol`
    /// macro, which will generate the appropriate build-time dependency checks,
    /// and access functions.
    ///
    /// Example:
    /// ```rust
    /// use fidl_library_of_some_kind::DependentProtocolMarker;
    ///
    /// #[ffx_protocol(DependentProtocolMarker)]
    /// pub struct FooProtocol {}
    ///
    /// impl FooProtocol {
    ///
    ///   async fn example_func(&self, cx: &Context) -> Result<()> {
    ///     let proxy = self.open_dependent_protocol_proxy(cx).await?;
    ///     proxy.do_some_things().await?;
    ///     Ok(())
    ///   }
    /// }
    ///
    /// ```
    pub async fn open_protocol<S>(&self) -> Result<S::Proxy>
    where
        S: fidl::endpoints::DiscoverableProtocolMarker,
    {
        let channel = self
            .inner
            .open_protocol(
                <S as fidl::endpoints::DiscoverableProtocolMarker>::PROTOCOL_NAME.to_owned(),
            )
            .await?;
        let proxy = S::Proxy::from_channel(
            fidl::AsyncChannel::from_channel(channel).context("making protocol async channel")?,
        );
        Ok(proxy)
    }

    pub async fn get_target_collection(&self) -> Result<Rc<TargetCollection>> {
        self.inner.get_target_collection().await
    }
}
