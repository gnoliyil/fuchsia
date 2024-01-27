// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::device::run_component_features;
use ::runner::{get_program_string, get_program_strvec};
use anyhow::{anyhow, Error};
use fidl::endpoints::ServerEnd;
use fidl_fuchsia_component as fcomponent;
use fidl_fuchsia_component_runner::{ComponentControllerMarker, ComponentStartInfo};
use fidl_fuchsia_io as fio;
use fuchsia_zircon as zx;
use std::ffi::CString;
use std::sync::Arc;

use crate::auth::{Credentials, FsCred};
use crate::execution::{
    container::Container, create_filesystem_from_spec, create_remotefs_filesystem, execute_task,
    get_pkg_hash, parse_numbered_handles,
};
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
///     mounted at /container/pkg/{HASH}.)
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

    let mut ns = start_info.ns.take().ok_or_else(|| anyhow!("Missing namespace"))?;
    let pkg = fio::DirectorySynchronousProxy::new(
        ns.iter_mut()
            .find(|entry| entry.path == Some("/pkg".to_string()))
            .ok_or_else(|| anyhow!("Missing /pkg entry in namespace"))?
            .directory
            .take()
            .ok_or_else(|| anyhow!("Missing directory handlee in pkg namespace entry"))?
            .into_channel(),
    );
    // Mount the package directory.
    let pkg_directory = mount_component_pkg_data(&container, &pkg)?;
    let resolve_template = |value: &str| value.replace("{pkg_path}", &pkg_directory);

    if let Some(directory) = ns
        .iter_mut()
        .find(|entry| entry.path == Some("/custom_artifacts".to_string()))
        .and_then(|entry| entry.directory.take())
    {
        let custom_artifacts = fio::DirectorySynchronousProxy::new(directory.into_channel());
        mount_custom_artifacts(&container, &custom_artifacts)?;
    }

    // Mount `/test_data`, used for gtest output files.
    if let Some(directory) = ns
        .iter_mut()
        .find(|entry| entry.path == Some("/test_data".to_string()))
        .and_then(|entry| entry.directory.take())
    {
        let test_data = fio::DirectorySynchronousProxy::new(directory.into_channel());
        mount_test_data(&container, &test_data)?;
    }

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

/// Attempts to mount the component's package directory in a content addressed directory in the
/// container's filesystem. This allows components to bundle their own binary in their package,
/// instead of relying on it existing in the system image of the container.
fn mount_component_pkg_data(
    container: &Container,
    pkg: &fio::DirectorySynchronousProxy,
) -> Result<String, Error> {
    const COMPONENT_PKG_ROOT_DIRECTORY: &str = "/container/pkg/";

    // Read the package content file and hash it as the name of the mount.
    let hash = get_pkg_hash(pkg)?;
    let pkg_path = COMPONENT_PKG_ROOT_DIRECTORY.to_owned() + &hash;

    // If the directory already exist, return it.
    match container.system_task.lookup_path_from_root(pkg_path.as_bytes()) {
        Ok(_) => {
            return Ok(pkg_path);
        }
        Err(errno) if errno == ENOENT => {}
        err @ Err(_) => {
            err?;
        }
    }

    // Create the new directory.
    let mount_point = {
        let pkg_dir =
            container.system_task.lookup_path_from_root(COMPONENT_PKG_ROOT_DIRECTORY.as_bytes())?;
        pkg_dir.entry.create_node(
            &container.system_task,
            hash.as_bytes(),
            mode!(IFDIR, 0o755),
            DeviceType::NONE,
            FsCred::root(),
        )?;
        container.system_task.lookup_path_from_root(pkg_path.as_bytes())?
    };

    // Create the filesystem and mount it.
    let rights = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE;
    let fs = create_remotefs_filesystem(&container.kernel, pkg, rights, ".")?;
    mount_point.mount(WhatToMount::Fs(fs), MountFlags::empty())?;
    Ok(pkg_path)
}

fn mount_custom_artifacts(
    container: &Container,
    custom_artifacts: &fio::DirectorySynchronousProxy,
) -> Result<(), Error> {
    const PATH: &str = "/custom_artifacts";

    // Create the new directory.
    let mount_point = container.system_task.lookup_path_from_root(PATH.as_bytes())?;

    // Create the filesystem and mount it.
    let rights = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE;
    let fs = create_remotefs_filesystem(&container.kernel, custom_artifacts, rights, ".")?;
    mount_point.mount(WhatToMount::Fs(fs), MountFlags::empty())?;
    Ok(())
}

fn mount_test_data(
    container: &Container,
    test_data: &fio::DirectorySynchronousProxy,
) -> Result<(), Error> {
    const PATH: &str = "/test_data";

    // Create the new directory.
    let mount_point = container.system_task.lookup_path_from_root(PATH.as_bytes())?;

    // Create the filesystem and mount it.
    let rights = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE;
    let fs = create_remotefs_filesystem(&container.kernel, test_data, rights, ".")?;
    mount_point.mount(WhatToMount::Fs(fs), MountFlags::empty())?;
    Ok(())
}
