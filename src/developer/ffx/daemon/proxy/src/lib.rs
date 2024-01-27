// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{Context as _, Result};
use async_trait::async_trait;
use async_utils::async_once::Once;
use errors::{ffx_bail, ffx_error, FfxError};
use ffx_config::EnvironmentContext;
use ffx_core::Injector;
use ffx_daemon::{get_daemon_proxy_single_link, is_daemon_running_at_path, DaemonConfig};
use ffx_metrics::add_ffx_rcs_protocol_event;
use ffx_target::{get_remote_proxy, open_target_with_fut, TargetKind};
use ffx_writer::{Format, Writer};
use fidl::endpoints::{create_proxy, Proxy};
use fidl_fuchsia_developer_ffx::{
    DaemonError, DaemonProxy, FastbootMarker, FastbootProxy, TargetProxy, VersionInfo,
};
use fidl_fuchsia_developer_remotecontrol::RemoteControlProxy;
use futures::FutureExt;
use hoist::Hoist;
use timeout::timeout;

/// The different ways to check the daemon's version against the local process' information
#[derive(Clone, Debug)]
pub enum DaemonVersionCheck {
    /// Compare the buildid, requires the daemon to have been spawned by the same executable.
    SameBuildId(String),
    /// Compare details from VersionInfo other than buildid, requires the daemon to have been
    /// spawned by the same overall build.
    SameVersionInfo(VersionInfo),
}

pub struct Injection {
    env_context: EnvironmentContext,
    daemon_check: DaemonVersionCheck,
    format: Option<Format>,
    target: Option<TargetKind>,
    hoist: Hoist,
    daemon_once: Once<DaemonProxy>,
    remote_once: Once<RemoteControlProxy>,
}

const CONFIG_DAEMON_AUTOSTART: &str = "daemon.autostart";

impl std::fmt::Debug for Injection {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Injection").finish()
    }
}

impl Injection {
    pub fn new(
        env_context: EnvironmentContext,
        daemon_check: DaemonVersionCheck,
        hoist: Hoist,
        format: Option<Format>,
        target: Option<TargetKind>,
    ) -> Self {
        Self {
            env_context,
            daemon_check,
            hoist,
            format,
            target,
            daemon_once: Default::default(),
            remote_once: Default::default(),
        }
    }

    fn is_default_target(&self) -> bool {
        self.target.is_none()
    }

    #[tracing::instrument]
    async fn init_remote_proxy(&self) -> Result<RemoteControlProxy> {
        let daemon_proxy = self.daemon_factory().await?;
        let target = self.target.clone();
        let proxy_timeout = self.env_context.get_proxy_timeout().await?;
        get_remote_proxy(target, self.is_default_target(), daemon_proxy, proxy_timeout).await
    }

    #[tracing::instrument]
    async fn fastboot_factory_inner(&self) -> Result<FastbootProxy> {
        let daemon_proxy = self.daemon_factory().await?;
        let target = self.target.clone();
        let (target_proxy, target_proxy_fut) = open_target_with_fut(
            target,
            self.is_default_target(),
            daemon_proxy.clone(),
            self.env_context.get_proxy_timeout().await?,
        )?;
        target_proxy_fut.await?;
        let (fastboot_proxy, fastboot_server_end) = create_proxy::<FastbootMarker>()?;
        target_proxy.open_fastboot(fastboot_server_end)?;
        Ok(fastboot_proxy)
    }

    #[tracing::instrument]
    async fn target_factory_inner(&self) -> Result<TargetProxy> {
        let target = self.target.clone();
        let daemon_proxy = self.daemon_factory().await?;
        let (target_proxy, target_proxy_fut) = open_target_with_fut(
            target,
            self.is_default_target(),
            daemon_proxy.clone(),
            self.env_context.get_proxy_timeout().await?,
        )?;
        target_proxy_fut.await?;
        Ok(target_proxy)
    }

    async fn daemon_timeout_error(&self) -> Result<FfxError> {
        let target = self.target.as_ref().map(ToString::to_string);
        Ok(FfxError::DaemonError {
            err: DaemonError::Timeout,
            target,
            is_default_target: self.is_default_target(),
        })
    }
}

#[async_trait(?Send)]
impl Injector for Injection {
    // This could get called multiple times by the plugin system via multiple threads - so make sure
    // the spawning only happens one thread at a time.
    #[tracing::instrument]
    async fn daemon_factory(&self) -> Result<DaemonProxy> {
        let autostart = self.env_context.query(CONFIG_DAEMON_AUTOSTART).get().await.unwrap_or(true);
        let start_mode =
            if autostart { DaemonStart::AutoStart } else { DaemonStart::DoNotAutoStart };
        self.daemon_once
            .get_or_try_init(init_daemon_proxy(
                start_mode,
                self.hoist.clone(),
                self.env_context.clone(),
                self.daemon_check.clone(),
            ))
            .await
            .map(|proxy| proxy.clone())
    }

    #[tracing::instrument]
    async fn try_daemon(&self) -> Result<Option<DaemonProxy>> {
        let result = self
            .daemon_once
            .get_or_try_init(init_daemon_proxy(
                DaemonStart::DoNotAutoStart,
                self.hoist.clone(),
                self.env_context.clone(),
                self.daemon_check.clone(),
            ))
            .await
            .map(|proxy| proxy.clone())
            .ok();
        Ok(result)
    }

    #[tracing::instrument]
    async fn fastboot_factory(&self) -> Result<FastbootProxy> {
        let target = self.target.clone();
        let timeout_error = self.daemon_timeout_error().await?;
        let proxy_timeout = self.env_context.get_proxy_timeout().await?;
        timeout(proxy_timeout, self.fastboot_factory_inner()).await.map_err(|_| {
            tracing::warn!("Timed out getting fastboot proxy for: {:?}", target);
            timeout_error
        })?
    }

    #[tracing::instrument]
    async fn target_factory(&self) -> Result<TargetProxy> {
        let target = self.target.clone();
        let timeout_error = self.daemon_timeout_error().await?;
        let proxy_timeout = self.env_context.get_proxy_timeout().await?;
        timeout(proxy_timeout, self.target_factory_inner()).await.map_err(|_| {
            tracing::warn!("Timed out getting fastboot proxy for: {:?}", target);
            timeout_error
        })?
    }

    #[tracing::instrument]
    async fn remote_factory(&self) -> Result<RemoteControlProxy> {
        let target = self.target.clone();
        let timeout_error = self.daemon_timeout_error().await?;
        let proxy_timeout = self.env_context.get_proxy_timeout().await?;
        let proxy = timeout(proxy_timeout, async {
            self.remote_once
                .get_or_try_init(self.init_remote_proxy())
                .await
                .map(|proxy| proxy.clone())
        })
        .await
        .map_err(|_| {
            tracing::warn!("Timed out getting remote control proxy for: {:?}", target);
            timeout_error
        })?;

        if let Ok(proxy) = proxy.as_ref() {
            let proto_fut = proxy.as_channel().get_channel_proxy_protocol();
            let proto_timeout = std::time::Duration::from_millis(500);

            match timeout(proto_timeout, proto_fut).await {
                Ok(Some(proto)) => {
                    let proto = proto.as_str();
                    if let Err(e) = add_ffx_rcs_protocol_event(proto).await {
                        tracing::warn!(error = ?e, "Problem sending protocol metrics");
                    }
                }
                // Peer seems to have closed. That'll be handled up the stack.
                Ok(None) => (),
                Err(_) => {
                    // This can happen if for some reason this channel isn't proxied by Overnet.
                    // That shouldn't ever happen but it's worth avoiding a hang.
                    tracing::warn!("Timed out waiting for protocol report from Overnet");
                }
            }
        }

        proxy
    }

    async fn is_experiment(&self, key: &str) -> bool {
        ffx_config::get(key).await.unwrap_or(false)
    }

    async fn build_info(&self) -> Result<VersionInfo> {
        Ok::<VersionInfo, anyhow::Error>(ffx_build_version::build_info())
    }

    async fn writer(&self) -> Result<Writer> {
        Ok(Writer::new(self.format))
    }
}

#[derive(PartialEq, Debug, Eq)]
enum DaemonStart {
    AutoStart,
    DoNotAutoStart,
}

#[tracing::instrument]
async fn init_daemon_proxy(
    autostart: DaemonStart,
    hoist: Hoist,
    context: EnvironmentContext,
    version_check: DaemonVersionCheck,
) -> Result<DaemonProxy> {
    let ascendd_path = context.get_ascendd_path().await?;

    if cfg!(not(test)) && !is_daemon_running_at_path(&ascendd_path) {
        if autostart == DaemonStart::DoNotAutoStart {
            return Err(ffx_error!(
                "FFX Daemon was told not to autostart and no existing Daemon instance was found"
            )
            .into());
        }
        ffx_daemon::spawn_daemon(&context).await?;
    }

    let (nodeid, proxy, link) =
        get_daemon_proxy_single_link(&hoist, ascendd_path.clone(), None).await?;

    // Spawn off the link task, so that FIDL functions can be called (link IO makes progress).
    let link_task = fuchsia_async::Task::local(link.map(|_| ()));

    let daemon_version_info = timeout(context.get_proxy_timeout().await?, proxy.get_version_info())
        .await
        .context("timeout")
        .map_err(|_| {
            ffx_error!(
                "ffx was unable to query the version of the running ffx daemon. \
                                 Run `ffx doctor --restart-daemon` and try again."
            )
        })?
        .context("Getting hash from daemon")?;

    // Check the version against the given comparison scheme.
    tracing::debug!("Checking daemon version: {version_check:?}");
    tracing::debug!("Daemon version info: {daemon_version_info:?}");
    let matched_proxy = match (version_check, daemon_version_info) {
        (DaemonVersionCheck::SameBuildId(ours), VersionInfo { build_id: Some(daemon), .. })
            if ours == daemon =>
        {
            true
        }
        (DaemonVersionCheck::SameVersionInfo(ours), daemon)
            if ours.build_version == daemon.build_version
                && ours.commit_hash == daemon.commit_hash
                && ours.commit_timestamp == daemon.commit_timestamp =>
        {
            true
        }
        _ => false,
    };

    if matched_proxy {
        tracing::debug!("Found matching daemon version, using it.");
        link_task.detach();
        return Ok(proxy);
    }

    eprintln!("Daemon is a different version, attempting to restart");
    tracing::info!("Daemon is a different version, attempting to restart");

    // Tell the daemon to quit, and wait for the link task to finish.
    // TODO(raggi): add a timeout on this, if the daemon quit fails for some
    // reason, the link task would hang indefinitely.
    let (quit_result, _) = futures::future::join(proxy.quit(), link_task).await;

    if !quit_result.is_ok() {
        ffx_bail!(
            "ffx daemon upgrade failed unexpectedly. \n\
            Try running `ffx doctor --restart-daemon` and then retry your \
            command.\n\nError was: {:?}",
            quit_result
        )
    }

    if cfg!(not(test)) {
        ffx_daemon::spawn_daemon(&context).await?;
    }

    let (_nodeid, proxy, link) =
        get_daemon_proxy_single_link(&hoist, ascendd_path, Some(vec![nodeid])).await?;

    fuchsia_async::Task::local(link.map(|_| ())).detach();

    Ok(proxy)
}

#[cfg(test)]
mod test {
    use super::*;
    use ascendd;
    use async_lock::Mutex;
    use async_net::unix::UnixListener;
    use fidl::endpoints::{ClientEnd, DiscoverableProtocolMarker, RequestStream};
    use fidl_fuchsia_developer_ffx::{
        DaemonMarker, DaemonRequest, DaemonRequestStream, VersionInfo,
    };
    use fidl_fuchsia_overnet::{ServiceProviderRequest, ServiceProviderRequestStream};
    use fuchsia_async::Task;
    use futures::{AsyncReadExt, FutureExt, TryStreamExt};
    use hoist::OvernetInstance;
    use std::time::Duration;
    use std::{path::PathBuf, sync::Arc};

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_init_daemon_proxy_link_lost() {
        let test_env = ffx_config::test_init().await.expect("Failed to initialize test env");
        let sockpath = test_env.context.get_ascendd_path().await.expect("No ascendd path");

        // Start a listener that accepts and immediately closes the socket..
        let listener = UnixListener::bind(sockpath.to_owned()).unwrap();
        let _listen_task = Task::local(async move {
            loop {
                drop(listener.accept().await.unwrap());
            }
        });

        let res = init_daemon_proxy(
            DaemonStart::AutoStart,
            Hoist::new().unwrap(),
            test_env.context.clone(),
            DaemonVersionCheck::SameBuildId("testcurrenthash".to_owned()),
        )
        .await;
        let str = format!("{}", res.err().unwrap());
        assert!(str.contains("link lost"));
        assert!(str.contains("ffx doctor"));
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_init_daemon_proxy_timeout_no_connection() {
        let test_env = ffx_config::test_init().await.expect("Failed to initialize test env");
        let sockpath = test_env.context.get_ascendd_path().await.expect("No ascendd path");

        // Start a listener that never accepts the socket.
        let _listener = UnixListener::bind(sockpath.to_owned()).unwrap();

        let res = init_daemon_proxy(
            DaemonStart::AutoStart,
            Hoist::new().unwrap(),
            test_env.context.clone(),
            DaemonVersionCheck::SameBuildId("testcurrenthash".to_owned()),
        )
        .await;
        let str = format!("{}", res.err().unwrap());
        assert!(str.contains("Timed out"));
        assert!(str.contains("ffx doctor"));
    }

    async fn test_daemon(
        local_hoist: Hoist,
        sockpath: PathBuf,
        build_id: &str,
        sleep_secs: u64,
    ) -> Task<()> {
        let version_info = VersionInfo {
            exec_path: Some(std::env::current_exe().unwrap().to_string_lossy().to_string()),
            build_id: Some(build_id.to_owned()),
            ..Default::default()
        };
        let daemon_hoist = Arc::new(Hoist::new().unwrap());
        let listener = UnixListener::bind(&sockpath).unwrap();
        let local_link_task = local_hoist.start_socket_link(sockpath.clone(), hoist::Cso::Enabled);

        let (s, p) = fidl::Channel::create();
        daemon_hoist.publish_service(DaemonMarker::PROTOCOL_NAME, ClientEnd::new(p)).unwrap();

        let link_tasks = Arc::new(Mutex::new(Vec::<Task<()>>::new()));
        let link_tasks1 = link_tasks.clone();

        let listen_task = Task::local(async move {
            // let (sock, _addr) = listener.accept().await.unwrap();
            let mut stream = listener.incoming();
            while let Some(sock) = stream.try_next().await.unwrap_or(None) {
                fuchsia_async::Timer::new(Duration::from_secs(sleep_secs)).await;
                let hoist_clone = daemon_hoist.clone();
                link_tasks1.lock().await.push(Task::local(async move {
                    let (mut rx, mut tx) = sock.split();
                    ascendd::run_stream(
                        hoist_clone.node(),
                        &mut rx,
                        &mut tx,
                        Some("fake daemon".to_string()),
                        None,
                    )
                    .map(|r| eprintln!("link error: {:?}", r))
                    .await;
                }));
            }
        });

        let mut stream = ServiceProviderRequestStream::from_channel(
            fidl::AsyncChannel::from_channel(s).unwrap(),
        );

        // Now that we've completed setting up everything, return a task for the main loop
        // of the fake daemon.
        Task::local(async move {
            while let Some(ServiceProviderRequest::ConnectToService { chan, .. }) =
                stream.try_next().await.unwrap_or(None)
            {
                let link_tasks = link_tasks.clone();
                let mut stream = DaemonRequestStream::from_channel(
                    fidl::AsyncChannel::from_channel(chan).unwrap(),
                );
                while let Some(request) = stream.try_next().await.unwrap_or(None) {
                    match request {
                        DaemonRequest::GetVersionInfo { responder, .. } => {
                            responder.send(&version_info).unwrap()
                        }
                        DaemonRequest::Quit { responder, .. } => {
                            std::fs::remove_file(sockpath).unwrap();
                            listen_task.cancel().await;
                            responder.send(true).unwrap();
                            // This is how long the daemon sleeps for, which
                            // is a workaround for the fact that we have no
                            // way to "flush" the response over overnet due
                            // to the constraints of mesh routing.
                            fuchsia_async::Timer::new(Duration::from_millis(20)).await;
                            link_tasks.lock().await.clear();
                            return;
                        }
                        _ => {
                            panic!("unimplemented stub for request: {:?}", request);
                        }
                    }
                }
            }
            // Explicitly drop this in the task so it gets moved into it and isn't dropped
            // early.
            drop(local_link_task);
        })
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_init_daemon_proxy_hash_matches() {
        let test_env = ffx_config::test_init().await.expect("Failed to initialize test env");
        let sockpath = test_env.context.get_ascendd_path().await.expect("No ascendd path");
        let local_hoist = Hoist::new().unwrap();

        let sockpath1 = sockpath.to_owned();
        let local_hoist1 = local_hoist.clone();
        let daemons_task =
            test_daemon(local_hoist1.clone(), sockpath1.to_owned(), "testcurrenthash", 0).await;

        let proxy = init_daemon_proxy(
            DaemonStart::AutoStart,
            local_hoist.clone(),
            test_env.context.clone(),
            DaemonVersionCheck::SameBuildId("testcurrenthash".to_owned()),
        )
        .await
        .unwrap();
        proxy.quit().await.unwrap();
        daemons_task.await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_init_daemon_proxy_upgrade() {
        let test_env = ffx_config::test_init().await.expect("Failed to initialize test env");
        let sockpath = test_env.context.get_ascendd_path().await.expect("No ascendd path");
        let local_hoist = Hoist::new().unwrap();

        let sockpath1 = sockpath.to_owned();
        let local_hoist1 = local_hoist.clone();

        // Spawn two daemons, the first out of date, the second is up to date.
        // spawn the first daemon directly so we know it's all started up before we proceed
        let first_daemon =
            test_daemon(local_hoist1.clone(), sockpath1.to_owned(), "oldhash", 0).await;
        let daemons_task = Task::local(async move {
            // wait for the first daemon to exit before starting the second
            first_daemon.await;
            // Note: testcurrenthash is explicitly expected by #cfg in get_daemon_proxy
            // Note: The double awaits are because test_daemon is an async function that returns a task
            test_daemon(local_hoist1.clone(), sockpath1.to_owned(), "testcurrenthash", 0)
                .await
                .await;
        });

        let proxy = init_daemon_proxy(
            DaemonStart::AutoStart,
            local_hoist.clone(),
            test_env.context.clone(),
            DaemonVersionCheck::SameBuildId("testcurrenthash".to_owned()),
        )
        .await
        .unwrap();
        proxy.quit().await.unwrap();
        daemons_task.await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_init_daemon_blocked_for_4s_succeeds() {
        let test_env = ffx_config::test_init().await.expect("Failed to initialize test env");
        let sockpath = test_env.context.get_ascendd_path().await.expect("No ascendd path");
        let local_hoist = Hoist::new().unwrap();

        // Spawn two daemons, the first out of date, the second is up to date.
        let sockpath1 = sockpath.to_owned();
        let local_hoist1 = local_hoist.clone();
        let daemon_task =
            test_daemon(local_hoist1.clone(), sockpath1.to_owned(), "testcurrenthash", 4).await;

        let proxy = init_daemon_proxy(
            DaemonStart::AutoStart,
            local_hoist.clone(),
            test_env.context.clone(),
            DaemonVersionCheck::SameBuildId("testcurrenthash".to_owned()),
        )
        .await
        .unwrap();
        proxy.quit().await.unwrap();
        daemon_task.await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_init_daemon_blocked_for_6s_timesout() {
        let test_env = ffx_config::test_init().await.expect("Failed to initialize test env");
        let sockpath = test_env.context.get_ascendd_path().await.expect("No ascendd path");
        let local_hoist = Hoist::new().unwrap();

        // Spawn two daemons, the first out of date, the second is up to date.
        let sockpath1 = sockpath.to_owned();
        let local_hoist1 = local_hoist.clone();
        let _daemon_task =
            test_daemon(local_hoist1.clone(), sockpath1.to_owned(), "testcurrenthash", 6).await;

        let err = init_daemon_proxy(
            DaemonStart::AutoStart,
            local_hoist.clone(),
            test_env.context.clone(),
            DaemonVersionCheck::SameBuildId("testcurrenthash".to_owned()),
        )
        .await;
        assert!(err.is_err());
        let str = format!("{:?}", err);
        assert!(str.contains("Timed out"));
        assert!(str.contains("ffx doctor"));
    }
}
