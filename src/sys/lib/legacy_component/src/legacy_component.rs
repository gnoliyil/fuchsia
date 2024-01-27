// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context as _, Error},
    fidl::{
        endpoints::{create_endpoints, create_proxy, ServerEnd},
        prelude::*,
    },
    fidl_fuchsia_component_runner as fcrunner, fidl_fuchsia_io as fio, fidl_fuchsia_sys as fsysv1,
    futures::TryStreamExt,
    std::sync::Arc,
    tracing::*,
    vfs::{
        directory::entry::DirectoryEntry, directory::helper::DirectlyMutable,
        directory::immutable::simple as pfs, execution_scope::ExecutionScope,
        file::vmo::asynchronous::read_only_static, path::Path as VfsPath, pseudo_directory,
    },
};

pub struct LegacyComponent {
    legacy_url: String,
    realm_label: String,
    env_controller_proxy: Option<fsysv1::EnvironmentControllerProxy>,
    runtime_dir: Option<ServerEnd<fio::DirectoryMarker>>,
}

impl LegacyComponent {
    /// Create a new nested environment under `parent_env` and launch this component.
    pub async fn run(
        legacy_url: String,
        start_info: fcrunner::ComponentStartInfo,
        parent_env: Arc<fsysv1::EnvironmentProxy>,
        realm_label: String,
        execution_scope: ExecutionScope,
    ) -> Result<Self, Error> {
        // We get the args here because |start_info| is partially moved below,
        // and the rust compiler doesn't allow accessing the program section
        // by reference after that.
        let args = runner::get_program_args(&start_info);

        // We are going to create a new v1 nested environment that holds the component's incoming
        // svc contents along with fuchsia.sys.Loader. This is because fuchsia.sys.Loader must be
        // in the environment for us to be able to launch components within it.
        //
        // In order to accomplish this, we need to host an svc directory that holds this protocol
        // along with everything else in the component's incoming svc directory. When the
        // fuchsia.sys.Loader node in the directory is connected to, we forward the connection to
        // our own namespace, and for any other connection we forward to the component's incoming
        // svc.
        let namespace = start_info.ns.ok_or(format_err!("ns cannot be NONE"))?;
        let mut svc_names = vec![];
        let mut flat_namespace = fsysv1::FlatNamespace { paths: vec![], directories: vec![] };
        let mut svc_dir_proxy = None;
        for namespace_entry in namespace {
            match namespace_entry.path.as_ref().map(|s| s.as_str()) {
                Some("/pkg") => (),
                Some("/svc") => {
                    let dir_proxy = namespace_entry
                        .directory
                        .ok_or(format_err!("missing directory handle"))?
                        .into_proxy()?;
                    svc_names = fuchsia_fs::directory::readdir(&dir_proxy)
                        .await?
                        .into_iter()
                        .map(|direntry| direntry.name)
                        .collect();
                    svc_dir_proxy = Some(dir_proxy);
                    break;
                }
                Some(entry) => {
                    let client_end = namespace_entry
                        .directory
                        .ok_or(format_err!("directory entry {} has no client handle", entry))?;
                    flat_namespace.directories.push(client_end);
                    flat_namespace.paths.push(entry.to_string());
                }
                _ => return Err(format_err!("malformed namespace")),
            }
        }

        let runner_svc_dir_proxy = fuchsia_fs::directory::open_in_namespace(
            "/svc",
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        )?;
        let host_pseudo_dir = pfs::simple();
        host_pseudo_dir.clone().add_entry(
        fsysv1::LoaderMarker::PROTOCOL_NAME,
        vfs::remote::remote_boxed(Box::new(
            move |_scope: ExecutionScope,
                  flags: fio::OpenFlags,
                  _relative_path: VfsPath,
                  server_end: ServerEnd<fio::NodeMarker>| {
                if let Err(e) = runner_svc_dir_proxy.open(
                    flags,
                    fio::ModeType::empty(),
                    fsysv1::LoaderMarker::PROTOCOL_NAME,
                    server_end,
                ) {
                    error!("failed to forward service open to realm builder server namespace: {:?}", e);
                }
            },
        )),
    )?;

        if let Some(svc_dir_proxy) = svc_dir_proxy {
            for svc_name in &svc_names {
                let svc_dir_proxy = Clone::clone(&svc_dir_proxy);
                let svc_name = svc_name.clone();
                let svc_name_for_err = svc_name.clone();
                if let Err(status) = host_pseudo_dir.clone().add_entry(
                    svc_name.clone().as_str(),
                    vfs::remote::remote_boxed(Box::new(
                        move |_scope: ExecutionScope,
                              flags: fio::OpenFlags,
                              _relative_path: VfsPath,
                              server_end: ServerEnd<fio::NodeMarker>| {
                            if let Err(e) = svc_dir_proxy.open(
                                flags,
                                fio::ModeType::empty(),
                                svc_name.as_str(),
                                server_end,
                            ) {
                                error!("failed to forward service open to v2 namespace: {:?}", e);
                            }
                        },
                    )),
                ) {
                    if status == fuchsia_zircon::Status::ALREADY_EXISTS {
                        error!(
                            "Service {} added twice to namespace of component {}",
                            svc_name_for_err, legacy_url
                        );
                    }

                    return Err(status.into());
                }
            }
        }

        let (host_dir_client_end, host_dir_server_end) = create_endpoints::<fio::DirectoryMarker>()
            .context("could not create node proxy endpoints")?;
        host_pseudo_dir.clone().open(
            execution_scope.clone(),
            fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::DIRECTORY,
            VfsPath::dot(),
            host_dir_server_end.into_channel().into(),
        );

        svc_names.push(fsysv1::LoaderMarker::PROTOCOL_NAME.into());
        let mut additional_services = fsysv1::ServiceList {
            names: svc_names,
            provider: None,
            host_directory: Some(host_dir_client_end),
        };

        // Our service list for the new nested environment is all set up, so we can proceed with
        // creating the v1 nested environment.

        let mut options = fsysv1::EnvironmentOptions {
            inherit_parent_services: false,
            use_parent_runners: false,
            kill_on_oom: true,
            delete_storage_on_death: true,
        };

        let (sub_env_proxy, sub_env_server_end) = create_proxy::<fsysv1::EnvironmentMarker>()?;
        let (sub_env_controller_proxy, sub_env_controller_server_end) =
            create_proxy::<fsysv1::EnvironmentControllerMarker>()?;

        parent_env.create_nested_environment(
            sub_env_server_end,
            sub_env_controller_server_end,
            &realm_label,
            Some(&mut additional_services),
            &mut options,
        )?;

        // We have created the nested environment that holds exactly and only fuchsia.sys.Loader.
        //
        // Now we can create the component.

        // The directory request given to the v1 component launcher connects the given directory
        // handle to the v1 component's `svc` subdir of its outgoing directory, whereas the
        // directory request we got from component manager should connect to the top-level of the
        // outgoing directory.
        //
        // We can make the two work together by hosting our own vfs on `start_info.outgoing_dir`,
        // which forwards connections to `svc` into the v1 component's `directory_request`.

        let (outgoing_svc_dir_proxy, outgoing_svc_dir_server_end) =
            create_proxy::<fio::DirectoryMarker>()?;
        let out_pseudo_dir = pseudo_directory!(
            "svc" => vfs::remote::remote_dir(outgoing_svc_dir_proxy),
        );
        out_pseudo_dir.open(
            execution_scope.clone(),
            fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::DIRECTORY,
            VfsPath::dot(),
            start_info
                .outgoing_dir
                .ok_or(format_err!("outgoing_dir cannot be NONE"))?
                .into_channel()
                .into(),
        );

        // We've got a nested environment and everything set up to get protocols into and out of
        // the v1 component. Time to launch it.
        let (sub_env_launcher_proxy, sub_env_launcher_server_end) =
            create_proxy::<fsysv1::LauncherMarker>()?;
        sub_env_proxy.get_launcher(sub_env_launcher_server_end)?;

        let mut launch_info = fsysv1::LaunchInfo {
            url: legacy_url.clone(),
            arguments: Some(args),
            out: None,
            err: None,
            directory_request: Some(outgoing_svc_dir_server_end),
            flat_namespace: Some(Box::new(flat_namespace)),
            additional_services: None,
        };
        sub_env_launcher_proxy.create_component(&mut launch_info, None)?;
        Ok(Self {
            legacy_url,
            realm_label,
            env_controller_proxy: sub_env_controller_proxy.into(),
            runtime_dir: start_info.runtime_dir,
        })
    }

    /// Serve lifetime controller for this component.
    pub async fn serve_controller(
        mut self,
        mut stream: fcrunner::ComponentControllerRequestStream,
        execution_scope: ExecutionScope,
    ) -> Result<(), Error> {
        // Run the runtime dir for component manager
        let runtime_dir = pseudo_directory!(
            "legacy_url" => read_only_static(self.legacy_url.into_bytes()),
            "realm_label" => read_only_static(self.realm_label.into_bytes()),
        );
        if let Some(runtime_dir_server) = self.runtime_dir.take() {
            runtime_dir.open(
                execution_scope.clone(),
                fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DIRECTORY,
                VfsPath::dot(),
                runtime_dir_server.into_channel().into(),
            );
        }

        // We want exhaustive match, and if we add more variants in the future we'd need to
        // handle the requests in a loop, so allow this link violation.
        #[allow(clippy::never_loop)]
        while let Some(req) =
            stream.try_next().await.context("invalid controller request from component manager")?
        {
            match req {
                fcrunner::ComponentControllerRequest::Stop { .. }
                | fcrunner::ComponentControllerRequest::Kill { .. } => {
                    if let Some(env) = self.env_controller_proxy.take() {
                        // We don't actually care much if this succeeds. If we can no longer successfully
                        // talk to appmgr then we're probably undergoing system shutdown, and the legacy
                        // component has thus stopped anyway.
                        let _ = env.kill().await;
                    }
                    break;
                }
            }
        }

        execution_scope.shutdown();
        Ok(())
    }
}
