// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{anyhow, format_err, Context, Error};
use fidl::endpoints::{ControlHandle, DiscoverableProtocolMarker, Proxy, RequestStream, ServerEnd};
use fidl_fuchsia_component as fcomponent;
use fidl_fuchsia_component_decl as fdecl;
use fidl_fuchsia_component_runner as frunner;
use fidl_fuchsia_io as fio;
use fidl_fuchsia_process as fprocess;
use fidl_fuchsia_starnix_container as fstarcontainer;
use fidl_fuchsia_starnix_developer as fstardev;
use fuchsia_async as fasync;
use fuchsia_component::client::{self as fclient, connect_to_protocol};
use fuchsia_runtime::{HandleInfo, HandleType};
use fuchsia_zircon as zx;
use fuchsia_zircon::HandleBased;
use futures::{FutureExt, StreamExt, TryStreamExt};
use kernel_config::generate_kernel_config;
use rand::Rng;
use std::sync::Arc;
use vfs::directory::{entry::DirectoryEntry, helper::DirectlyMutable};

/// The name of the collection that the starnix_kernel is run in.
const KERNEL_COLLECTION: &str = "kernels";

#[fuchsia::main(logging_tags = ["starnix_manager"])]
async fn main() -> Result<(), Error> {
    const KERNELS_DIRECTORY: &str = "kernels";
    const SVC_DIRECTORY: &str = "svc";

    let outgoing_dir_handle =
        fuchsia_runtime::take_startup_handle(fuchsia_runtime::HandleType::DirectoryRequest.into())
            .ok_or(anyhow!("Failed to get startup handle"))?;
    let outgoing_dir_server_end =
        fidl::endpoints::ServerEnd::new(zx::Channel::from(outgoing_dir_handle));

    let outgoing_dir = vfs::directory::immutable::simple();
    let kernels_dir = vfs::directory::immutable::simple();
    outgoing_dir.add_entry(KERNELS_DIRECTORY, kernels_dir.clone())?;

    let svc_dir = vfs::directory::immutable::simple();
    svc_dir.add_entry(
        fstardev::ManagerMarker::PROTOCOL_NAME,
        vfs::service::host(move |requests| async move {
            serve_starnix_manager(requests).await.expect("Error serving starnix manager.");
        }),
    )?;
    svc_dir.add_entry(
        frunner::ComponentRunnerMarker::PROTOCOL_NAME,
        vfs::service::host(move |requests| {
            let kernels_dir = kernels_dir.clone();
            async move {
                serve_component_runner(requests, kernels_dir.clone())
                    .await
                    .expect("Error serving component runner.");
            }
        }),
    )?;
    outgoing_dir.add_entry(SVC_DIRECTORY, svc_dir.clone())?;

    let execution_scope = vfs::execution_scope::ExecutionScope::new();
    outgoing_dir.open(
        execution_scope.clone(),
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        vfs::path::Path::dot(),
        outgoing_dir_server_end,
    );
    execution_scope.wait().await;

    Ok(())
}

pub async fn serve_starnix_manager(
    mut request_stream: fstardev::ManagerRequestStream,
) -> Result<(), Error> {
    while let Some(event) = request_stream.try_next().await? {
        match event {
            fstardev::ManagerRequest::Start { url, responder } => {
                let args = fcomponent::CreateChildArgs {
                    numbered_handles: None,
                    ..fcomponent::CreateChildArgs::EMPTY
                };
                if let Err(e) = create_child_component(url, args).await {
                    tracing::error!("failed to create child component: {}", e);
                }
                responder.send()?;
            }
            fstardev::ManagerRequest::StartShell { params, controller, .. } => {
                start_shell(params, controller).await?;
            }
            fstardev::ManagerRequest::VsockConnect { container, port, bridge_socket, .. } => {
                connect_to_vsock(port, bridge_socket, &container).unwrap_or_else(|e| {
                    tracing::error!("failed to connect to vsock {:?}", e);
                });
            }
        }
    }
    Ok(())
}

pub async fn serve_component_runner(
    mut request_stream: frunner::ComponentRunnerRequestStream,
    kernels_dir: Arc<vfs::directory::immutable::Simple>,
) -> Result<(), Error> {
    while let Some(event) = request_stream.try_next().await? {
        match event {
            frunner::ComponentRunnerRequest::Start { start_info, controller, .. } => {
                create_new_kernel(&kernels_dir, start_info, controller).await?;
            }
        }
    }
    Ok(())
}

async fn start_shell(
    params: fstardev::ShellParams,
    controller: ServerEnd<fstardev::ShellControllerMarker>,
) -> Result<(), Error> {
    let controller_handle_info = fprocess::HandleInfo {
        handle: controller.into_channel().into_handle(),
        id: HandleInfo::new(HandleType::User0, 0).as_raw(),
    };
    let numbered_handles = vec![
        handle_info_from_socket(params.standard_in, 0)?,
        handle_info_from_socket(params.standard_out, 1)?,
        handle_info_from_socket(params.standard_err, 2)?,
        controller_handle_info,
    ];
    let args = fcomponent::CreateChildArgs {
        numbered_handles: Some(numbered_handles),
        ..fcomponent::CreateChildArgs::EMPTY
    };

    let url = params.url.ok_or(anyhow!("No shell URL specified"))?;
    create_child_component(url, args).await
}

/// Connects `bridge_socket` to the vsocket at `port` in the specified container.
///
/// Returns an error if the FIDL connection to the container failed.
fn connect_to_vsock(port: u32, bridge_socket: fidl::Socket, container: &str) -> Result<(), Error> {
    let service_prefix = "/".to_string() + container;
    let container =
        fclient::connect_to_protocol_at::<fstarcontainer::ControllerMarker>(&service_prefix)?;

    container
        .vsock_connect(port, bridge_socket)
        .context("Failed to call vsock connect on container")
}

/// Creates a `HandleInfo` from the provided socket and file descriptor.
///
/// The file descriptor is encoded as a `PA_HND(PA_FD, <file_descriptor>)` before being stored in
/// the `HandleInfo`.
///
/// Returns an error if `socket` is `None`.
pub fn handle_info_from_socket(
    socket: Option<fidl::Socket>,
    file_descriptor: u16,
) -> Result<fprocess::HandleInfo, Error> {
    if let Some(socket) = socket {
        let info = HandleInfo::new(HandleType::FileDescriptor, file_descriptor);
        Ok(fprocess::HandleInfo { handle: socket.into_handle(), id: info.as_raw() })
    } else {
        Err(anyhow!("Failed to create HandleInfo for {}", file_descriptor))
    }
}

/// Creates a new child component in the `playground` collection.
///
/// # Parameters
/// - `url`: The URL of the component to create.
/// - `args`: The `CreateChildArgs` that are passed to the component manager.
pub async fn create_child_component(
    url: String,
    args: fcomponent::CreateChildArgs,
) -> Result<(), Error> {
    // TODO(fxbug.dev/74511): The amount of setup required here is a bit lengthy. Ideally,
    // fuchsia-component would provide language-specific bindings for the Realm API that could
    // reduce this logic to a few lines.

    const COLLECTION: &str = "playground";
    let realm = fclient::realm().context("failed to connect to Realm service")?;
    let mut collection_ref = fdecl::CollectionRef { name: COLLECTION.into() };
    let id: u64 = rand::thread_rng().gen();
    let child_name = format!("starnix-{}", id);
    let child_decl = fdecl::Child {
        name: Some(child_name.clone()),
        url: Some(url),
        startup: Some(fdecl::StartupMode::Lazy),
        environment: None,
        ..fdecl::Child::EMPTY
    };
    let () = realm
        .create_child(&mut collection_ref, child_decl, args)
        .await?
        .map_err(|e| format_err!("failed to create child: {:?}", e))?;
    // The component is run in a `SingleRun` collection instance, and will be automatically
    // deleted when it exits.
    Ok(())
}

/// Creates a new instance of `starnix_kernel`.
///
/// This is done by creating a new child in the `kernels` collection. A directory is offered to the
/// `starnix_kernel`, at `/container_config`. The directory contains the `program` block of the
/// container, and the container's `/pkg` directory.
///
/// The component controller for the container is also passed to the `starnix_kernel`, via the `User0`
/// handle. The `controller` is closed when the `starnix_kernel` finishes executing (e.g., when
/// the init task completes).
async fn create_new_kernel(
    kernels_dir: &vfs::directory::immutable::Simple,
    component_start_info: frunner::ComponentStartInfo,
    controller: ServerEnd<frunner::ComponentControllerMarker>,
) -> Result<(), Error> {
    // The name of the directory capability that is being offered to the starnix_kernel.
    const KERNEL_DIRECTORY: &str = "kernels";
    // The url of the starnix_kernel component, which is packaged with the starnix_manager.
    const KERNEL_URL: &str = "fuchsia-pkg://fuchsia.com/starnix_kernel#meta/starnix_kernel.cm";

    let kernel_start_info =
        generate_kernel_config(kernels_dir, KERNEL_DIRECTORY, component_start_info)?;

    // Create a new instance of starnix_kernel in the kernel collection. Offer the directory that
    // contains all the configuration information for the container that it is running.
    let realm =
        connect_to_protocol::<fcomponent::RealmMarker>().expect("Failed to connect to realm.");
    realm
        .create_child(
            &mut fdecl::CollectionRef { name: KERNEL_COLLECTION.into() },
            fdecl::Child {
                name: Some(kernel_start_info.name.clone()),
                url: Some(KERNEL_URL.to_string()),
                startup: Some(fdecl::StartupMode::Lazy),
                ..fdecl::Child::EMPTY
            },
            kernel_start_info.args,
        )
        .await?
        .map_err(|e| anyhow::anyhow!("failed to create runner child: {:?}", e))?;

    let kernel_outgoing_dir =
        open_exposed_directory(&realm, &kernel_start_info.name, KERNEL_COLLECTION).await?;
    let kernel_binder =
        fclient::connect_to_protocol_at_dir_root::<fcomponent::BinderMarker>(&kernel_outgoing_dir)?;

    fasync::Task::local(async move {
        let _ =
            serve_component_controller(controller, kernel_binder, &kernel_start_info.name).await;
    })
    .detach();

    Ok(())
}

async fn open_exposed_directory(
    realm: &fcomponent::RealmProxy,
    child_name: &str,
    collection_name: &str,
) -> Result<fio::DirectoryProxy, Error> {
    let (directory_proxy, server_end) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()?;
    realm
        .open_exposed_dir(
            &mut fdecl::ChildRef {
                name: child_name.into(),
                collection: Some(collection_name.into()),
            },
            server_end,
        )
        .await?
        .map_err(|e| {
            anyhow!(
                "failed to bind to child {} in collection {:?}: {:?}",
                child_name,
                collection_name,
                e
            )
        })?;
    Ok(directory_proxy)
}

async fn serve_component_controller(
    controller: ServerEnd<frunner::ComponentControllerMarker>,
    binder: fcomponent::BinderProxy,
    kernel_name: &str,
) -> Result<(), Error> {
    let mut request_stream = controller.into_stream()?;
    let control_handle = request_stream.control_handle();

    let epitaph = futures::select! {
        result = binder.on_closed().fuse() => {
                if let Err(e) = result { e } else { zx::Status::OK }
        },
        request = request_stream.next() => {
          if let Some(Ok(request)) = request {
            match request {
              frunner::ComponentControllerRequest::Stop { .. }
              | frunner::ComponentControllerRequest::Kill { .. } => {
                let realm = connect_to_protocol::<fcomponent::RealmMarker>()
                    .expect("Failed to connect to realm.");
                let _ = realm
                    .destroy_child(&mut fdecl::ChildRef {
                        name: kernel_name.to_string(),
                        collection: Some(KERNEL_COLLECTION.to_string()),
                    })
                    .await?;
              }
            }
          }
          zx::Status::OK
        }
    };
    control_handle.shutdown_with_epitaph(epitaph);

    Ok(())
}
