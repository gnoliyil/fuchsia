// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::device::run_component_features;
use ::runner::{get_program_string, get_program_strvec};
use anyhow::{anyhow, bail, Error};
use fidl::endpoints::ServerEnd;
use fidl_fuchsia_component as fcomponent;
use fidl_fuchsia_component_runner::{ComponentControllerMarker, ComponentStartInfo};
use fidl_fuchsia_io as fio;
use fuchsia_zircon as zx;
use rand::{distributions::Alphanumeric, thread_rng, Rng};
use std::ffi::CString;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::sync::Arc;

use crate::auth::Credentials;
use crate::execution::{
    container::Container, create_filesystem_from_spec, execute_task, parse_numbered_handles,
};
use crate::fs::fuchsia::RemoteFs;
use crate::fs::*;
use crate::logging::{log_error, log_info};
use crate::task::*;
use crate::types::*;

/// Starts a component in an isolated environment, called a "container".
///
/// The container will be configured according to a configuration file in the Starnix kernel's package.
/// The configuration file specifies, for example, which binary to run as "init", whether or not the
/// system should wait for the existence of a given file path to run the component, etc.
///
/// The Starnix kernel's package also contains the system image to mount.
///
/// The component's `binary` can either:
///   - an absolute path, in which case the path is treated as a path into the root filesystem that
///     is mounted by the container's configuration
///   - relative path, in which case the binary is read from the component's package (which is
///     mounted at /container/component/{random}/pkg.)
pub async fn start_component(
    mut start_info: ComponentStartInfo,
    controller: ServerEnd<ComponentControllerMarker>,
    container: Arc<Container>,
) -> Result<(), Error> {
    let url = start_info.resolved_url.clone().unwrap_or_else(|| "<unknown>".to_string());
    log_info!(
        "start_component: {}\narguments: {:?}\nmanifest: {:?}",
        url,
        start_info.numbered_handles,
        start_info.program,
    );

    let component_path = generate_component_path(&container)?;

    let ns = start_info.ns.take().ok_or_else(|| anyhow!("Missing namespace"))?;

    let mut maybe_pkg = None;
    for entry in ns {
        let dir_path = if let Some(dir_path) = entry.path {
            dir_path
        } else {
            continue;
        };

        let directory = if let Some(directory) = entry.directory {
            directory
        } else {
            continue;
        };

        match dir_path.as_str() {
            "/svc" => continue,
            "/custom_artifacts" | "/test_data" => {
                // Mount custom_artifacts and test_data directory at root of container
                // We may want to transition to have these directories unique per component
                let dir_proxy = fio::DirectorySynchronousProxy::new(directory.into_channel());
                mount(&container, &dir_proxy, &dir_path)?;
            }
            _ => {
                let dir_proxy = fio::DirectorySynchronousProxy::new(directory.into_channel());
                mount(&container, &dir_proxy, &format!("{component_path}/{dir_path}"))?;
                if dir_path == "/pkg" {
                    maybe_pkg = Some(dir_proxy);
                }
            }
        }
    }

    let pkg = maybe_pkg.ok_or_else(|| anyhow!("Missing /pkg entry in namespace"))?;
    let pkg_directory = format!("{component_path}/pkg");

    let resolve_template = |value: &str| {
        value.replace("{pkg_path}", &pkg_directory).replace("{component_path}", &component_path)
    };

    let args = get_program_strvec(&start_info, "args")
        .map(|args| {
            args.iter()
                .map(|arg| CString::new(resolve_template(arg)))
                .collect::<Result<Vec<CString>, _>>()
        })
        .unwrap_or(Ok(vec![]))?;
    let environ = get_program_strvec(&start_info, "environ")
        .map(|args| {
            args.iter()
                .map(|arg| CString::new(resolve_template(arg)))
                .collect::<Result<Vec<CString>, _>>()
        })
        .unwrap_or(Ok(vec![]))?;
    let component_features =
        get_program_strvec(&start_info, "features").cloned().unwrap_or_default();
    log_info!("start_component environment: {:?}", environ);

    let binary_path = get_program_string(&start_info, "binary")
        .ok_or_else(|| anyhow!("Missing \"binary\" in manifest"))?;
    let binary_path = CString::new(binary_path.to_owned())?;

    let mut current_task = Task::create_init_child_process(&container.kernel, &binary_path)?;

    let cwd = current_task
        .lookup_path(
            &mut LookupContext::default(),
            current_task.fs().root(),
            pkg_directory.as_bytes(),
        )
        .map_err(|e| anyhow!("Could not find package directory: {:?}", e))?;
    current_task
        .fs()
        .chdir(&current_task, cwd)
        .map_err(|e| anyhow!("Failed to set cwd to package directory: {:?}", e))?;

    let user_passwd = get_program_string(&start_info, "user").unwrap_or("fuchsia:x:42:42");
    let credentials = Credentials::from_passwd(user_passwd)?;
    current_task.set_creds(credentials);

    if let Some(local_mounts) = get_program_strvec(&start_info, "component_mounts") {
        for mount in local_mounts.iter() {
            let (mount_point, child_fs) = create_filesystem_from_spec(&current_task, &pkg, mount)?;
            let mount_point = current_task.lookup_path_from_root(mount_point)?;
            mount_point.mount(child_fs, MountFlags::empty())?;
        }
    }

    let startup_handles =
        parse_numbered_handles(&current_task, start_info.numbered_handles, &current_task.files)?;
    let shell_controller = startup_handles.shell_controller;

    let mut argv = vec![binary_path];
    argv.extend(args.into_iter());

    let executable = current_task.open_file(argv[0].as_bytes(), OpenFlags::RDONLY)?;
    current_task.exec(executable, argv[0].clone(), argv.clone(), environ)?;

    run_component_features(&component_features, &current_task, &mut start_info.outgoing_dir)
        .unwrap_or_else(|e| {
            log_error!(current_task, "failed to set component features for {} - {:?}", url, e);
        });

    execute_task(current_task, |result| {
        // TODO(fxb/74803): Using the component controller's epitaph may not be the best way to
        // communicate the exit status. The component manager could interpret certain epitaphs as starnix
        // being unstable, and chose to terminate starnix as a result.
        // Errors when closing the controller with an epitaph are disregarded, since there are
        // legitimate reasons for this to fail (like the client having closed the channel).
        if let Some(shell_controller) = shell_controller {
            let _ = shell_controller.close_with_epitaph(zx::Status::OK);
        }
        let _ = match result {
            Ok(ExitStatus::Exit(0)) => controller.close_with_epitaph(zx::Status::OK),
            _ => controller.close_with_epitaph(zx::Status::from_raw(
                fcomponent::Error::InstanceDied.into_primitive() as i32,
            )),
        };
    });

    Ok(())
}

// Returns /container/component/{random} that doesn't already exist
fn generate_component_path(container: &Container) -> Result<String, Error> {
    // Checking container directory already exists
    let mount_point = container.system_task.lookup_path_from_root(b"/container/component/")?;

    // Find /container/component/{random} that doesn't already exist
    let component_path = loop {
        let random_string: String =
            thread_rng().sample_iter(&Alphanumeric).take(10).map(char::from).collect();

        // This returns EEXIST if /container/component/{random} already exists.
        // If so, try again with another {random} string.
        match mount_point.create_node(
            &container.system_task,
            random_string.as_bytes(),
            mode!(IFDIR, 0o755),
            DeviceType::NONE,
        ) {
            Ok(_) => break format!("/container/component/{random_string}"),
            Err(errno) if errno == EEXIST => {}
            Err(e) => bail!(e),
        };
    };

    Ok(component_path)
}

fn mount(
    container: &Container,
    directory: &fio::DirectorySynchronousProxy,
    path: &str,
) -> Result<(), Error> {
    // The incoming dir_path might not be top level, e.g. it could be /foo/bar.
    // Iterate through each component directory starting from the parent and
    // create it if it doesn't exist.
    let mut current_node = container.system_task.lookup_path_from_root(b".")?;
    let mut context = LookupContext::default();

    // Extract each component using Path::new(path).components(). For example,
    // Path::new("/foo/bar").components() will return [RootDir, Normal("foo"), Normal("bar")].
    // We're not interested in the RootDir, so we drop the prefix "/" if it exists.
    let path = if let Some(path) = path.strip_prefix('/') { path } else { path };

    for sub_dir in Path::new(path).components() {
        let sub_dir = sub_dir.as_os_str().as_bytes();

        current_node = match current_node.create_node(
            &container.system_task,
            sub_dir,
            mode!(IFDIR, 0o755),
            DeviceType::NONE,
        ) {
            Ok(node) => node,
            Err(errno) if errno == EEXIST || errno == ENOTDIR => {
                current_node.lookup_child(&container.system_task, &mut context, sub_dir)?
            }
            Err(e) => bail!(e),
        };
    }

    let (status, rights) = directory.get_flags(zx::Time::INFINITE)?;
    zx::Status::ok(status)?;

    let (client_end, server_end) = zx::Channel::create();
    directory.clone(fio::OpenFlags::CLONE_SAME_RIGHTS, ServerEnd::new(server_end))?;

    let fs = RemoteFs::new_fs(&container.kernel, client_end, rights)?;
    current_node.mount(WhatToMount::Fs(fs), MountFlags::empty())?;
    Ok(())
}
