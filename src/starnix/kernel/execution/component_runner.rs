// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::device::run_component_features;
use ::runner::{get_program_string, get_program_strvec};
use anyhow::{anyhow, bail, Error};
use fidl::endpoints::{ControlHandle, RequestStream, ServerEnd};
use fidl_fuchsia_component as fcomponent;
use fidl_fuchsia_component_runner::{
    ComponentControllerMarker, ComponentControllerRequest, ComponentControllerRequestStream,
    ComponentStartInfo,
};
use fidl_fuchsia_io as fio;
use fuchsia_async as fasync;
use fuchsia_zircon as zx;
use futures::{channel::oneshot, FutureExt, StreamExt};
use rand::{distributions::Alphanumeric, thread_rng, Rng};
use std::{
    ffi::CString,
    os::unix::ffi::OsStrExt,
    path::Path,
    sync::{Arc, Weak},
};

use crate::{
    auth::Credentials,
    execution::{
        container::Container, create_filesystem_from_spec, execute_task, parse_numbered_handles,
        set_rlimits,
    },
    fs::{fuchsia::RemoteFs, *},
    logging::{log_error, log_info},
    signals,
    task::*,
    types::*,
};

/// Component controller epitaph value used as the base value to pass non-zero error
/// codes to the calling component.
///
/// TODO(fxbug.dev/130980): Cleanup this once we have a proper mechanism to
/// get Linux exit code from component runner.
const COMPONENT_EXIT_CODE_BASE: i32 = 1024;

/// Starts a component inside the given container.
///
/// The component's `binary` can either:
///   - an absolute path, in which case the path is treated as a path into the root filesystem that
///     is mounted by the container's configuration
///   - relative path, in which case the binary is read from the component's package (which is
///     mounted at /container/component/{random}/pkg.)
///
/// The directories in the component's namespace are mounted at /container/component/{random}.
pub async fn start_component(
    mut start_info: ComponentStartInfo,
    controller: ServerEnd<ComponentControllerMarker>,
    container: &Container,
) -> Result<(), Error> {
    let url = start_info.resolved_url.clone().unwrap_or_else(|| "<unknown>".to_string());
    log_info!("start_component: {}", url);

    // TODO(fxbug.dev/125782): We leak the directory created by this function.
    let component_path = generate_component_path(container)?;

    let mut mount_record = MountRecord::default();

    let ns = start_info.ns.take().ok_or_else(|| anyhow!("Missing namespace"))?;

    let mut maybe_pkg = None;
    for entry in ns {
        if let (Some(dir_path), Some(dir_handle)) = (entry.path, entry.directory) {
            match dir_path.as_str() {
                "/svc" => continue,
                "/custom_artifacts" | "/test_data" => {
                    // Mount custom_artifacts and test_data directory at root of container
                    // We may want to transition to have these directories unique per component
                    let dir_proxy = fio::DirectorySynchronousProxy::new(dir_handle.into_channel());
                    mount_record.mount_remote(container, &dir_proxy, &dir_path)?;
                }
                _ => {
                    let dir_proxy = fio::DirectorySynchronousProxy::new(dir_handle.into_channel());
                    mount_record.mount_remote(
                        container,
                        &dir_proxy,
                        &format!("{component_path}/{dir_path}"),
                    )?;
                    if dir_path == "/pkg" {
                        maybe_pkg = Some(dir_proxy);
                    }
                }
            }
        }
    }

    let pkg = maybe_pkg.ok_or_else(|| anyhow!("Missing /pkg entry in namespace"))?;
    let pkg_path = format!("{component_path}/pkg");

    let resolve_template = |value: &str| {
        value.replace("{pkg_path}", &pkg_path).replace("{component_path}", &component_path)
    };

    let resolve_program_strvec = |key| {
        get_program_strvec(&start_info, key)
            .map(|args: &Vec<String>| {
                args.iter()
                    .map(|arg| CString::new(resolve_template(arg)))
                    .collect::<Result<Vec<CString>, _>>()
            })
            .unwrap_or(Ok(vec![]))
    };

    let args = resolve_program_strvec("args")?;
    let environ = resolve_program_strvec("environ")?;
    let component_features =
        get_program_strvec(&start_info, "features").cloned().unwrap_or_default();

    let binary_path = get_program_string(&start_info, "binary")
        .ok_or_else(|| anyhow!("Missing \"binary\" in manifest"))?;
    let binary_path = CString::new(binary_path.to_owned())?;

    let rlimits = get_program_strvec(&start_info, "rlimits").cloned().unwrap_or_default();

    let mut current_task = Task::create_init_child_process(&container.kernel, &binary_path)?;
    set_rlimits(&current_task, &rlimits)?;

    let cwd_path = get_program_string(&start_info, "cwd").unwrap_or(&pkg_path);
    let cwd = current_task
        .lookup_path(&mut LookupContext::default(), current_task.fs().root(), cwd_path.as_bytes())
        .map_err(|e| anyhow!("Could not find package directory: {:?}", e))?;
    current_task
        .fs()
        .chdir(&current_task, cwd)
        .map_err(|e| anyhow!("Failed to set cwd to package directory: {:?}", e))?;

    let uid = get_program_string(&start_info, "uid").unwrap_or("42").parse()?;
    let mut credentials = Credentials::with_ids(uid, uid);
    if let Some(caps) = get_program_strvec(&start_info, "capabilities") {
        let mut capabilities = Capabilities::empty();
        for cap in caps {
            capabilities |= cap.parse()?;
        }
        credentials.cap_permitted = capabilities;
        credentials.cap_effective = capabilities;
        credentials.cap_inheritable = capabilities;
    }
    current_task.set_creds(credentials);

    if let Some(local_mounts) = get_program_strvec(&start_info, "component_mounts") {
        for mount in local_mounts.iter() {
            let (mount_point, child_fs) =
                create_filesystem_from_spec(current_task.kernel(), &pkg, mount)?;
            let mount_point = current_task.lookup_path_from_root(mount_point)?;
            mount_record.mount(mount_point, WhatToMount::Fs(child_fs), MountFlags::empty())?;
        }
    }

    parse_numbered_handles(&current_task, start_info.numbered_handles, &current_task.files)?;

    let mut argv = vec![binary_path.clone()];
    argv.extend(args);

    let executable = current_task.open_file(binary_path.as_bytes(), OpenFlags::RDONLY)?;
    current_task.exec(executable, binary_path, argv, environ)?;

    run_component_features(&component_features, &container.kernel, &mut start_info.outgoing_dir)
        .unwrap_or_else(|e| {
            log_error!("failed to set component features for {} - {:?}", url, e);
        });

    let (task_complete_sender, task_complete) = oneshot::channel::<TaskResult>();
    let controller = controller.into_stream()?;
    fasync::Task::local(serve_component_controller(
        controller,
        Arc::downgrade(&current_task.task),
        task_complete,
    ))
    .detach();

    execute_task(current_task, move |result| {
        // If the component controller server has gone away, there is nobody for us to
        // report the result to.
        let _ = task_complete_sender.send(result);
        // Unmount all the directories for this component.
        std::mem::drop(mount_record);
    });

    Ok(())
}

type TaskResult = Result<ExitStatus, Error>;

/// Translates [ComponentControllerRequest] messages to signals on the `task`.
///
/// When a `Stop` request is received, it will send a `SIGINT` to the task.
/// When a `Kill` request is received, it will send a `SIGKILL` to the task and close the component
/// controller channel regardless if/how the task responded to the signal. Due to Linux's design,
/// this may not reliably cleanup everything that was started as a result of running the component.
///
/// If the task has completed, it will also close the controller channel.
async fn serve_component_controller(
    controller: ComponentControllerRequestStream,
    task: Weak<Task>,
    task_complete: oneshot::Receiver<TaskResult>,
) {
    let controller_handle = controller.control_handle();

    enum Event<T, U> {
        Controller(T),
        Completion(U),
    }

    let mut stream = futures::stream::select(
        controller.map(Event::Controller),
        task_complete.into_stream().map(Event::Completion),
    );

    while let Some(event) = stream.next().await {
        match event {
            Event::Controller(request) => match request {
                Ok(ComponentControllerRequest::Stop { .. }) => {
                    if let Some(task) = task.upgrade() {
                        signals::send_signal(
                            task.as_ref(),
                            signals::SignalInfo::new(
                                SIGINT,
                                SI_KERNEL,
                                signals::SignalDetail::default(),
                            ),
                        );
                        log_info!("Sent SIGINT to program {:}", task.command().to_string_lossy());
                    }
                }
                Ok(ComponentControllerRequest::Kill { .. }) => {
                    if let Some(task) = task.upgrade() {
                        signals::send_signal(
                            task.as_ref(),
                            signals::SignalInfo::new(
                                SIGKILL,
                                SI_KERNEL,
                                signals::SignalDetail::default(),
                            ),
                        );
                        log_info!("Sent SIGKILL to program {:}", task.command().to_string_lossy());
                        controller_handle.shutdown_with_epitaph(zx::Status::from_raw(
                            fcomponent::Error::InstanceDied.into_primitive() as i32,
                        ));
                    }
                    return;
                }
                Err(_) => {
                    return;
                }
            },
            Event::Completion(result) => match result {
                Ok(Ok(ExitStatus::Exit(0))) => {
                    controller_handle.shutdown_with_epitaph(zx::Status::OK)
                }
                Ok(Ok(ExitStatus::Exit(n))) => controller_handle.shutdown_with_epitaph(
                    zx::Status::from_raw(COMPONENT_EXIT_CODE_BASE + n as i32),
                ),
                _ => controller_handle.shutdown_with_epitaph(zx::Status::from_raw(
                    fcomponent::Error::InstanceDied.into_primitive() as i32,
                )),
            },
        }
    }
}

/// Returns /container/component/{random} that doesn't already exist
fn generate_component_path(container: &Container) -> Result<String, Error> {
    let system_task = container.kernel.kthreads.system_task();
    // Checking container directory already exists
    let mount_point = system_task.lookup_path_from_root(b"/container/component/")?;

    // Find /container/component/{random} that doesn't already exist
    let component_path = loop {
        let random_string: String =
            thread_rng().sample_iter(&Alphanumeric).take(10).map(char::from).collect();

        // This returns EEXIST if /container/component/{random} already exists.
        // If so, try again with another {random} string.
        match mount_point.create_node(
            system_task,
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

/// A record of the mounts created when starting a component.
///
/// When the record is dropped, the mounts are unmounted.
#[derive(Default)]
struct MountRecord {
    /// The namespace nodes at which we have crated mounts for this component.
    mounts: Vec<NamespaceNode>,
}

impl MountRecord {
    fn mount(
        &mut self,
        mount_point: NamespaceNode,
        what: WhatToMount,
        flags: MountFlags,
    ) -> Result<(), Errno> {
        mount_point.mount(what, flags)?;
        self.mounts.push(mount_point);
        Ok(())
    }

    fn mount_remote(
        &mut self,
        container: &Container,
        directory: &fio::DirectorySynchronousProxy,
        path: &str,
    ) -> Result<(), Error> {
        let system_task = container.kernel.kthreads.system_task();
        // The incoming dir_path might not be top level, e.g. it could be /foo/bar.
        // Iterate through each component directory starting from the parent and
        // create it if it doesn't exist.
        let mut current_node = system_task.lookup_path_from_root(b".")?;
        let mut context = LookupContext::default();

        // Extract each component using Path::new(path).components(). For example,
        // Path::new("/foo/bar").components() will return [RootDir, Normal("foo"), Normal("bar")].
        // We're not interested in the RootDir, so we drop the prefix "/" if it exists.
        let path = if let Some(path) = path.strip_prefix('/') { path } else { path };

        for sub_dir in Path::new(path).components() {
            let sub_dir = sub_dir.as_os_str().as_bytes();

            current_node = match current_node.create_node(
                system_task,
                sub_dir,
                mode!(IFDIR, 0o755),
                DeviceType::NONE,
            ) {
                Ok(node) => node,
                Err(errno) if errno == EEXIST || errno == ENOTDIR => {
                    current_node.lookup_child(system_task, &mut context, sub_dir)?
                }
                Err(e) => bail!(e),
            };
        }

        let (status, rights) = directory.get_flags(zx::Time::INFINITE)?;
        zx::Status::ok(status)?;

        let (client_end, server_end) = zx::Channel::create();
        directory.clone(fio::OpenFlags::CLONE_SAME_RIGHTS, ServerEnd::new(server_end))?;

        let fs = RemoteFs::new_fs(&container.kernel, client_end, path, rights)?;
        current_node.mount(WhatToMount::Fs(fs), MountFlags::empty())?;
        self.mounts.push(current_node);

        Ok(())
    }

    fn unmount(&mut self) -> Result<(), Errno> {
        while let Some(node) = self.mounts.pop() {
            node.unmount()?;
        }
        Ok(())
    }
}

impl Drop for MountRecord {
    fn drop(&mut self) {
        match self.unmount() {
            Ok(()) => {}
            Err(e) => log_error!("failed to unmount during component exit: {:?}", e),
        }
    }
}
