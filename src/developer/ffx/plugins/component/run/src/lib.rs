// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Result,
    component_debug::lifecycle::{
        create_instance_in_collection, destroy_instance_in_collection, resolve_instance,
        start_instance, CreateError,
    },
    errors::{ffx_bail, ffx_error},
    ffx_component::{
        format::{
            format_create_error, format_destroy_error, format_resolve_error, format_start_error,
        },
        parse_component_url,
        rcs::connect_to_lifecycle_controller,
    },
    ffx_component_run_args::RunComponentCommand,
    ffx_core::ffx_plugin,
    ffx_log::{log_impl, LogOpts},
    ffx_log_args::LogCommand,
    fidl_fuchsia_developer_ffx::DiagnosticsProxy,
    fidl_fuchsia_developer_remotecontrol as rc, fidl_fuchsia_sys2 as fsys,
    moniker::{
        AbsoluteMoniker, AbsoluteMonikerBase, ChildMonikerBase, RelativeMoniker,
        RelativeMonikerBase,
    },
};

#[ffx_plugin(DiagnosticsProxy = "daemon::protocol")]
pub async fn run(
    diagnostics_proxy: DiagnosticsProxy,
    rcs_proxy: rc::RemoteControlProxy,
    cmd: RunComponentCommand,
) -> Result<()> {
    let moniker = AbsoluteMoniker::parse_str(&cmd.moniker)
        .map_err(|e| ffx_error!("Moniker could not be parsed: {}", e))?;
    let log_filter = moniker.to_string().strip_prefix("/").unwrap().to_string();
    // TODO(fxb/100844): Intgrate with Started event to get a start time for the logs.
    let log_cmd = LogCommand { filter: vec![log_filter], ..LogCommand::default() };

    let lifecycle_controller = connect_to_lifecycle_controller(&rcs_proxy).await?;

    run_impl(lifecycle_controller, &cmd.url, moniker, cmd.recreate, &mut std::io::stdout()).await?;

    if cmd.follow_logs {
        log_impl(
            diagnostics_proxy,
            Some(rcs_proxy),
            &None,
            log_cmd,
            &mut std::io::stdout(),
            LogOpts::default(),
        )
        .await?;
    }

    Ok(())
}

async fn run_impl<W: std::io::Write>(
    lifecycle_controller: fsys::LifecycleControllerProxy,
    url: &String,
    moniker: AbsoluteMoniker,
    recreate: bool,
    writer: &mut W,
) -> Result<()> {
    let url = parse_component_url(url.as_str())?;

    let parent = moniker
        .parent()
        .ok_or(ffx_error!("Error: {} does not reference a dynamic instance", moniker))?;
    let leaf = moniker
        .leaf()
        .ok_or(ffx_error!("Error: {} does not reference a dynamic instance", moniker))?;
    let child_name = leaf.name();
    let collection = leaf
        .collection()
        .ok_or(ffx_error!("Error: {} does not reference a dynamic instance", moniker))?;

    writeln!(writer, "URL: {}", url)?;
    writeln!(writer, "Moniker: {}", moniker)?;
    writeln!(writer, "Creating component instance...")?;

    // Convert the absolute moniker into a relative moniker w.r.t. root.
    // LifecycleController expects relative monikers only.
    let parent_relative = RelativeMoniker::scope_down(&AbsoluteMoniker::root(), &parent).unwrap();

    let create_result = create_instance_in_collection(
        &lifecycle_controller,
        &parent_relative,
        collection,
        child_name,
        &url,
    )
    .await;

    match create_result {
        Err(CreateError::InstanceAlreadyExists) => {
            if recreate {
                // This component already exists, but the user has asked it to be recreated.
                writeln!(writer, "{} already exists. Destroying...", moniker)?;
                destroy_instance_in_collection(
                    &lifecycle_controller,
                    &parent_relative,
                    collection,
                    child_name,
                )
                .await
                .map_err(|e| format_destroy_error(&moniker, e))?;

                writeln!(writer, "Recreating component instance...")?;
                create_instance_in_collection(
                    &lifecycle_controller,
                    &parent_relative,
                    collection,
                    child_name,
                    &url,
                )
                .await
                .map_err(|e| format_create_error(&moniker, &parent, collection, e))?;
            } else {
                ffx_bail!("\nERROR: {} already exists.\nUse --recreate to destroy and create a new instance, or provide a different moniker.\n", moniker)
            }
        }
        Err(e) => {
            return Err(format_create_error(&moniker, &parent, collection, e).into());
        }
        Ok(()) => {}
    }

    let child_relative = RelativeMoniker::scope_down(&AbsoluteMoniker::root(), &moniker).unwrap();
    writeln!(writer, "Resolving component instance...")?;
    resolve_instance(&lifecycle_controller, &child_relative)
        .await
        .map_err(|e| format_resolve_error(&moniker, e))?;

    writeln!(writer, "Starting component instance...")?;
    start_instance(&lifecycle_controller, &child_relative)
        .await
        .map_err(|e| format_start_error(&moniker, e))?;

    writeln!(writer, "Started component instance!")?;

    Ok(())
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use {
        crate::run_impl,
        anyhow::Result,
        fidl::endpoints::create_proxy_and_stream,
        fidl_fuchsia_sys2 as fsys,
        futures::TryStreamExt,
        moniker::{AbsoluteMoniker, AbsoluteMonikerBase},
    };

    fn setup_fake_lifecycle_controller_ok(
        expected_parent_moniker: &'static str,
        expected_collection: &'static str,
        expected_name: &'static str,
        expected_url: &'static str,
        expected_moniker: &'static str,
    ) -> fsys::LifecycleControllerProxy {
        let (lifecycle_controller, mut stream) =
            create_proxy_and_stream::<fsys::LifecycleControllerMarker>().unwrap();
        fuchsia_async::Task::local(async move {
            let req = stream.try_next().await.unwrap().unwrap();
            match req {
                fsys::LifecycleControllerRequest::CreateInstance {
                    parent_moniker,
                    collection,
                    decl,
                    responder,
                    args: _,
                } => {
                    assert_eq!(expected_parent_moniker, parent_moniker);
                    assert_eq!(expected_collection, collection.name);
                    assert_eq!(expected_name, decl.name.unwrap());
                    assert_eq!(expected_url, decl.url.unwrap());
                    responder.send(&mut Ok(())).unwrap();
                }
                _ => panic!("Unexpected Lifecycle Controller request"),
            }

            let req = stream.try_next().await.unwrap().unwrap();
            match req {
                fsys::LifecycleControllerRequest::ResolveInstance { moniker, responder } => {
                    assert_eq!(expected_moniker, moniker);
                    responder.send(&mut Ok(())).unwrap();
                }
                _ => panic!("Unexpected Lifecycle Controller request"),
            }

            let req = stream.try_next().await.unwrap().unwrap();
            match req {
                fsys::LifecycleControllerRequest::StartInstance {
                    moniker,
                    binder: _,
                    responder,
                } => {
                    assert_eq!(expected_moniker, moniker);
                    responder.send(&mut Ok(())).unwrap();
                }
                _ => panic!("Unexpected Lifecycle Controller request"),
            }
        })
        .detach();
        lifecycle_controller
    }

    fn setup_fake_lifecycle_controller_fail(
        expected_parent_moniker: &'static str,
        expected_collection: &'static str,
        expected_name: &'static str,
        expected_url: &'static str,
    ) -> fsys::LifecycleControllerProxy {
        let (lifecycle_controller, mut stream) =
            create_proxy_and_stream::<fsys::LifecycleControllerMarker>().unwrap();
        fuchsia_async::Task::local(async move {
            let req = stream.try_next().await.unwrap().unwrap();
            match req {
                fsys::LifecycleControllerRequest::CreateInstance {
                    parent_moniker,
                    collection,
                    decl,
                    responder,
                    args: _,
                } => {
                    assert_eq!(expected_parent_moniker, parent_moniker);
                    assert_eq!(expected_collection, collection.name);
                    assert_eq!(expected_name, decl.name.unwrap());
                    assert_eq!(expected_url, decl.url.unwrap());
                    responder.send(&mut Err(fsys::CreateError::InstanceAlreadyExists)).unwrap();
                }
                _ => panic!("Unexpected Lifecycle Controller request"),
            }
        })
        .detach();
        lifecycle_controller
    }

    fn setup_fake_lifecycle_controller_recreate(
        expected_parent_moniker: &'static str,
        expected_collection: &'static str,
        expected_name: &'static str,
        expected_url: &'static str,
        expected_moniker: &'static str,
    ) -> fsys::LifecycleControllerProxy {
        let (lifecycle_controller, mut stream) =
            create_proxy_and_stream::<fsys::LifecycleControllerMarker>().unwrap();
        fuchsia_async::Task::local(async move {
            let req = stream.try_next().await.unwrap().unwrap();
            match req {
                fsys::LifecycleControllerRequest::CreateInstance {
                    parent_moniker,
                    collection,
                    decl,
                    responder,
                    args: _,
                } => {
                    assert_eq!(expected_parent_moniker, parent_moniker);
                    assert_eq!(expected_collection, collection.name);
                    assert_eq!(expected_name, decl.name.unwrap());
                    assert_eq!(expected_url, decl.url.unwrap());
                    responder.send(&mut Err(fsys::CreateError::InstanceAlreadyExists)).unwrap();
                }
                _ => panic!("Unexpected Lifecycle Controller request"),
            }

            let req = stream.try_next().await.unwrap().unwrap();
            match req {
                fsys::LifecycleControllerRequest::DestroyInstance {
                    parent_moniker,
                    child,
                    responder,
                } => {
                    assert_eq!(expected_parent_moniker, parent_moniker);
                    assert_eq!(expected_name, child.name);
                    assert_eq!(expected_collection, child.collection.unwrap());
                    responder.send(&mut Ok(())).unwrap();
                }
                _ => panic!("Unexpected Lifecycle Controller request"),
            }

            let req = stream.try_next().await.unwrap().unwrap();
            match req {
                fsys::LifecycleControllerRequest::CreateInstance {
                    parent_moniker,
                    collection,
                    decl,
                    responder,
                    args: _,
                } => {
                    assert_eq!(expected_parent_moniker, parent_moniker);
                    assert_eq!(expected_collection, collection.name);
                    assert_eq!(expected_name, decl.name.unwrap());
                    assert_eq!(expected_url, decl.url.unwrap());
                    responder.send(&mut Ok(())).unwrap();
                }
                _ => panic!("Unexpected Lifecycle Controller request"),
            }

            let req = stream.try_next().await.unwrap().unwrap();
            match req {
                fsys::LifecycleControllerRequest::ResolveInstance { moniker, responder } => {
                    assert_eq!(expected_moniker, moniker);
                    responder.send(&mut Ok(())).unwrap();
                }
                _ => panic!("Unexpected Lifecycle Controller request"),
            }

            let req = stream.try_next().await.unwrap().unwrap();
            match req {
                fsys::LifecycleControllerRequest::StartInstance {
                    moniker,
                    binder: _,
                    responder,
                } => {
                    assert_eq!(expected_moniker, moniker);
                    responder.send(&mut Ok(())).unwrap();
                }
                _ => panic!("Unexpected Lifecycle Controller request"),
            }
        })
        .detach();
        lifecycle_controller
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_ok() -> Result<()> {
        let mut output = Vec::new();
        let lifecycle_controller = setup_fake_lifecycle_controller_ok(
            "./some",
            "collection",
            "name",
            "fuchsia-pkg://fuchsia.com/test#meta/test.cm",
            "./some/collection:name",
        );
        let moniker = AbsoluteMoniker::parse_str("/some/collection:name")?;
        let response = run_impl(
            lifecycle_controller,
            &"fuchsia-pkg://fuchsia.com/test#meta/test.cm".to_string(),
            moniker,
            false,
            &mut output,
        )
        .await;
        response.unwrap();
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_name() -> Result<()> {
        let mut output = Vec::new();
        let lifecycle_controller = setup_fake_lifecycle_controller_ok(
            "./core",
            "ffx-laboratory",
            "foobar",
            "fuchsia-pkg://fuchsia.com/test#meta/test.cm",
            "./core/ffx-laboratory:foobar",
        );
        let moniker = AbsoluteMoniker::parse_str("/core/ffx-laboratory:foobar")?;
        let response = run_impl(
            lifecycle_controller,
            &"fuchsia-pkg://fuchsia.com/test#meta/test.cm".to_string(),
            moniker,
            false,
            &mut output,
        )
        .await;
        response.unwrap();
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_fail() -> Result<()> {
        let mut output = Vec::new();
        let lifecycle_controller = setup_fake_lifecycle_controller_fail(
            "./core",
            "ffx-laboratory",
            "test",
            "fuchsia-pkg://fuchsia.com/test#meta/test.cm",
        );
        let moniker = AbsoluteMoniker::parse_str("/core/ffx-laboratory:test")?;
        let response = run_impl(
            lifecycle_controller,
            &"fuchsia-pkg://fuchsia.com/test#meta/test.cm".to_string(),
            moniker,
            false,
            &mut output,
        )
        .await;
        response.unwrap_err();
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_recreate() -> Result<()> {
        let mut output = Vec::new();
        let lifecycle_controller = setup_fake_lifecycle_controller_recreate(
            "./core",
            "ffx-laboratory",
            "test",
            "fuchsia-pkg://fuchsia.com/test#meta/test.cm",
            "./core/ffx-laboratory:test",
        );
        let moniker = AbsoluteMoniker::parse_str("/core/ffx-laboratory:test")?;
        let response = run_impl(
            lifecycle_controller,
            &"fuchsia-pkg://fuchsia.com/test#meta/test.cm".to_string(),
            moniker,
            true,
            &mut output,
        )
        .await;
        response.unwrap();
        Ok(())
    }
}
