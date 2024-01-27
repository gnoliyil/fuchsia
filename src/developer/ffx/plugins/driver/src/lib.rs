// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use anyhow::{Context, Result};
use component_debug::capability;
use ffx_driver_args::DriverCommand;
use fho::{FfxMain, FfxTool, SimpleWriter};
use fidl::endpoints::ProtocolMarker;
use fidl_fuchsia_developer_remotecontrol as rc;
use fidl_fuchsia_device_manager as fdm;
use fidl_fuchsia_driver_development as fdd;
use fidl_fuchsia_driver_playground as fdp;
use fidl_fuchsia_driver_registrar as fdr;
use fidl_fuchsia_io as fio;
use fidl_fuchsia_sys2 as fsys;
use fidl_fuchsia_test_manager as ftm;
use fuchsia_zircon_status::Status;
use selectors::{self, VerboseError};

struct DriverConnector {
    remote_control: Option<rc::RemoteControlProxy>,
}

impl DriverConnector {
    fn new(remote_control: Option<rc::RemoteControlProxy>) -> Self {
        Self { remote_control }
    }

    async fn get_component_with_capability<S: ProtocolMarker>(
        &self,
        capability: &str,
        default_selector: &str,
        select: bool,
    ) -> Result<S::Proxy> {
        async fn remotecontrol_connect<S: ProtocolMarker>(
            remote_control: &rc::RemoteControlProxy,
            selector: &str,
        ) -> Result<S::Proxy> {
            let (proxy, server_end) = fidl::endpoints::create_proxy::<S>()
                .with_context(|| format!("failed to create proxy to {}", S::DEBUG_NAME))?;
            let _: rc::ServiceMatch = remote_control
                .connect(
                    selectors::parse_selector::<VerboseError>(selector)?,
                    server_end.into_channel(),
                )
                .await?
                .map_err(|e| {
                    anyhow::anyhow!(
                        "failed to connect to {} as {}: {:?}",
                        S::DEBUG_NAME,
                        selector,
                        e
                    )
                })?;
            Ok(proxy)
        }

        // Gets monikers for components that expose a capability matching the given |query|.
        // This moniker is eventually converted into a selector and is used to connecting to
        // the capability.
        async fn find_components_with_capability(
            rcs_proxy: &rc::RemoteControlProxy,
            query: &str,
        ) -> Result<Vec<String>> {
            let (query_proxy, query_server) =
                fidl::endpoints::create_proxy::<fsys::RealmQueryMarker>()
                    .context("creating realm query proxy")?;
            rcs_proxy
                .root_realm_query(query_server)
                .await?
                .map_err(|i| Status::ok(i).unwrap_err())
                .context("opening query")?;

            Ok(capability::get_all_route_segments(query.to_string(), &query_proxy)
                .await?
                .iter()
                .filter_map(|segment| {
                    if let capability::RouteSegment::ExposeBy { moniker, .. } = segment {
                        // Remove the leading `/` so it can be converted into a selector
                        // later on.
                        Some(moniker.to_string().split_off(1))
                    } else {
                        None
                    }
                })
                .collect())
        }

        /// Find the components that expose a given capability, and let the user
        /// request which component they would like to connect to.
        async fn user_choose_selector(
            remote_control: &rc::RemoteControlProxy,
            capability: &str,
        ) -> Result<String> {
            let capabilities = find_components_with_capability(&remote_control, capability).await?;
            println!("Please choose which component to connect to:");
            for (i, component) in capabilities.iter().enumerate() {
                println!("    {}: {}", i, component)
            }

            let mut line_editor = rustyline::Editor::<()>::new();
            loop {
                let line = line_editor.readline("$ ")?;
                let choice = line.trim().parse::<usize>();
                if choice.is_err() {
                    println!("Error: please choose a value.");
                    continue;
                }
                let choice = choice.unwrap();
                if choice >= capabilities.len() {
                    println!("Error: please choose a correct value.");
                    continue;
                }
                // We have to escape colons in the capability name to distinguish them from the
                // syntactically meaningful colons in the ':expose:" string.
                return Ok(capabilities[choice].replace(":", "\\:") + ":expose:" + capability);
            }
        }

        if let Some(ref remote_control) = self.remote_control {
            let selector = match select {
                true => user_choose_selector(remote_control, capability).await?,
                false => default_selector.to_string(),
            };
            remotecontrol_connect::<S>(&remote_control, &selector).await
        } else {
            anyhow::bail!("Failed to get remote control proxy");
        }
    }
}

#[async_trait::async_trait]
impl driver_connector::DriverConnector for DriverConnector {
    async fn get_driver_development_proxy(
        &self,
        select: bool,
    ) -> Result<fdd::DriverDevelopmentProxy> {
        self.get_component_with_capability::<fdd::DriverDevelopmentMarker>(
            "fuchsia.driver.development.DriverDevelopment",
            "bootstrap/driver_manager:expose:fuchsia.driver.development.DriverDevelopment",
            select,
        )
        .await
        .context("Failed to get driver development component")
    }

    async fn get_dev_proxy(&self, select: bool) -> Result<fio::DirectoryProxy> {
        self.get_component_with_capability::<fio::DirectoryMarker>(
            "dev",
            "bootstrap/devfs:expose:dev-topological",
            select,
        )
        .await
        .context("Failed to get dev component")
    }

    async fn get_device_watcher_proxy(&self) -> Result<fdm::DeviceWatcherProxy> {
        self.get_component_with_capability::<fdm::DeviceWatcherMarker>(
            "fuchsia.hardware.usb.DeviceWatcher",
            "bootstrap/driver_manager:expose:fuchsia.hardware.usb.DeviceWatcher",
            false,
        )
        .await
        .context("Failed to get device watcher component")
    }

    async fn get_driver_registrar_proxy(&self, select: bool) -> Result<fdr::DriverRegistrarProxy> {
        self.get_component_with_capability::<fdr::DriverRegistrarMarker>(
            "fuchsia.driver.registrar.DriverRegistrar",
            "bootstrap/driver_index:expose:fuchsia.driver.registrar.DriverRegistrar",
            select,
        )
        .await
        .context("Failed to get driver registrar component")
    }

    async fn get_tool_runner_proxy(&self, select: bool) -> Result<fdp::ToolRunnerProxy> {
        self.get_component_with_capability::<fdp::ToolRunnerMarker>(
            "fuchsia.driver.playground.ToolRunner",
            "core/driver_playground:expose:fuchsia.driver.playground.ToolRunner",
            select,
        )
        .await
        .context("Failed to get tool runner component")
    }

    async fn get_run_builder_proxy(&self) -> Result<ftm::RunBuilderProxy> {
        self.get_component_with_capability::<ftm::RunBuilderMarker>(
            "",
            "core/test_manager:expose:fuchsia.test.manager.RunBuilder",
            false,
        )
        .await
        .context("Failed to get RunBuilder component")
    }
}

#[derive(FfxTool)]
pub struct DriverTool {
    remote_control: fho::Result<rc::RemoteControlProxy>,
    #[command]
    cmd: DriverCommand,
}

fho::embedded_plugin!(DriverTool);

#[async_trait::async_trait(?Send)]
impl FfxMain for DriverTool {
    type Writer = SimpleWriter;

    async fn main(self, mut writer: Self::Writer) -> fho::Result<()> {
        driver_tools::driver(
            self.cmd.into(),
            DriverConnector::new(self.remote_control.ok()),
            &mut writer,
        )
        .await
        .map_err(Into::into)
    }
}
