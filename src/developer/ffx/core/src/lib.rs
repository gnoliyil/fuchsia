// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub use core_macros::{ffx_command, ffx_plugin};

use anyhow::Result;
use async_trait::async_trait;
use ffx_writer::Writer;
use fidl_fuchsia_developer_ffx::{DaemonProxy, FastbootProxy, TargetProxy, VersionInfo};
use fidl_fuchsia_developer_remotecontrol::RemoteControlProxy;

/// Exports used in macros
#[doc(hidden)]
pub mod macro_deps {
    pub use anyhow;
    pub use errors;
    pub use fidl;
    pub use fidl_fuchsia_developer_ffx;
    pub use fuchsia_async;
    pub use futures;
    pub use rcs;
    pub use selectors;
}

#[async_trait(?Send)]
pub trait Injector {
    async fn daemon_factory(&self) -> Result<DaemonProxy>;
    /// Attempts to get a handle to the ffx daemon.
    async fn try_daemon(&self) -> Result<Option<DaemonProxy>>;
    async fn remote_factory(&self) -> Result<RemoteControlProxy>;
    async fn fastboot_factory(&self) -> Result<FastbootProxy>;
    async fn target_factory(&self) -> Result<TargetProxy>;
    async fn is_experiment(&self, key: &str) -> bool;
    async fn build_info(&self) -> Result<VersionInfo>;
    async fn writer(&self) -> Result<Writer>;
}
