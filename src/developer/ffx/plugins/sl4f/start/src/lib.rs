// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Result;
use errors::{ffx_bail, ffx_error};
use ffx_component::rcs::connect_to_lifecycle_controller;
use ffx_core::ffx_plugin;
use ffx_sl4f_start_args::StartCommand;
use fidl_fuchsia_component as fcomponent;
use fidl_fuchsia_component_decl as fdecl;
use fidl_fuchsia_developer_remotecontrol::RemoteControlProxy;
use fidl_fuchsia_sl4f_ffx::Sl4fBridgeProxy;
use fidl_fuchsia_sys2 as fsys;
use fidl_fuchsia_sys2::LifecycleControllerProxy;

const COLLECTION_NAME: &'static str = "ffx-laboratory";
const PARENT_MONIKER: &'static str = "./core";
const PROXY_NAME: &'static str = "sl4f_proxy_server";
const PROXY_URL: &'static str =
    "fuchsia-pkg://fuchsia.com/sl4f-ffx-proxy-server#meta/sl4f_proxy_server.cm";
const PROXY_MONIKER: &'static str = "/core/sl4f_bridge_server";
const SL4F_MONIKER: &'static str = "/core/sl4f";

async fn create_remote_component(
    lifecycle_controller: &LifecycleControllerProxy,
    name: &str,
    url: &str,
    moniker: &str,
) -> Result<()> {
    let collection = fdecl::CollectionRef { name: COLLECTION_NAME.to_string() };
    let decl = fdecl::Child {
        name: Some(name.to_string()),
        url: Some(url.to_string()),
        startup: Some(fdecl::StartupMode::Lazy),
        environment: None,
        ..Default::default()
    };
    let create_result = lifecycle_controller
        .create_instance(PARENT_MONIKER, &collection, &decl, fcomponent::CreateChildArgs::default())
        .await
        .map_err(|e| ffx_error!("FIDL error while creating component instance: {:?}", e))?;

    match create_result {
        Err(fsys::CreateError::InstanceAlreadyExists) => {
            println!("Component instance already exists: {}", moniker);
            println!("  Restarting component: {}", moniker);
            // This component already exists, but the user has asked it to be recreated.
            let child = fdecl::ChildRef {
                name: name.to_string(),
                collection: Some(COLLECTION_NAME.to_string()),
            };
            println!("  Destroying prior instance of component: {}", moniker);
            let destroy_result =
                lifecycle_controller.destroy_instance(PARENT_MONIKER, &child).await.map_err(
                    |e| ffx_error!("FIDL error while destroying component instance: {:?}", e),
                )?;

            if let Err(e) = destroy_result {
                ffx_bail!("Lifecycle protocol could not destroy component instance: {:?}", e);
            }

            println!("  Recreating component: {}", moniker);
            let create_result = lifecycle_controller
                .create_instance(
                    PARENT_MONIKER,
                    &collection,
                    &decl,
                    fcomponent::CreateChildArgs::default(),
                )
                .await
                .map_err(|e| ffx_error!("FIDL error while creating component instance: {:?}", e))?;

            if let Err(e) = create_result {
                ffx_bail!("Lifecycle protocol could not recreate component instance: {:?}", e);
            }
        }
        Err(e) => {
            ffx_bail!("Lifecycle protocol could not create component instance: {:?}", e);
        }
        Ok(()) => {}
    }
    Ok(())
}

async fn start_remote_component(
    lifecycle_controller: &LifecycleControllerProxy,
    moniker: &str,
) -> Result<()> {
    // LifecycleController accepts RelativeMonikers only.
    let moniker = format!(".{}", moniker.to_string());
    let (_, binder_server) = fidl::endpoints::create_endpoints::<fcomponent::BinderMarker>();
    let start_result = lifecycle_controller
        .start_instance(&moniker, binder_server)
        .await
        .map_err(|e| ffx_error!("FIDL error while starting the component instance: {}", e))?;

    match start_result {
        Ok(()) => {}
        Err(e) => {
            ffx_bail!("Lifecycle protocol could not start the component instance: {:?}", e);
        }
    }
    Ok(())
}

#[ffx_plugin(Sl4fBridgeProxy = "daemon::protocol")]
pub async fn start(
    rcs_proxy: RemoteControlProxy,
    _proxy: Sl4fBridgeProxy,
    _cmd: StartCommand,
) -> Result<()> {
    let lifecycle_controller = connect_to_lifecycle_controller(&rcs_proxy).await?;

    // There are three servers to start:
    // 1) The device-side proxy.
    println!("  Starting the device-side proxy.");
    create_remote_component(&lifecycle_controller, &PROXY_NAME, &PROXY_URL, &PROXY_MONIKER).await?;
    start_remote_component(&lifecycle_controller, &PROXY_MONIKER).await?;

    // 2) The SL4F component on-device.
    println!("  Starting SL4F on the device.");
    start_remote_component(&lifecycle_controller, &SL4F_MONIKER).await?;

    // 3) The host-side proxy is started automatically by the ffx protocol.
    Ok(())
}
