// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{anyhow, Error},
    diagnostics_reader::{ArchiveReader, Inspect},
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_component as fcomponent, fidl_fuchsia_io as fio,
    fidl_fuchsia_modular_internal as fmodular, fidl_fuchsia_session as fsession,
    fidl_fuchsia_sys as fsys, fidl_fuchsia_ui_app as fuiapp, fidl_fuchsia_ui_policy as fuipolicy,
    fidl_fuchsia_ui_views as fuiviews, fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_component_test::{
        Capability, ChildOptions, ChildRef, LocalComponentHandles, RealmBuilder, Ref, Route,
    },
    fuchsia_zircon as zx,
    futures::{channel::mpsc, future::join_all, prelude::*},
    std::sync::Arc,
    vfs::{directory::entry::DirectoryEntry, file::vmo::read_only, pseudo_directory},
};

const BASEMGR_URL: &str = "#meta/basemgr.cm";
const BASEMGR_WITH_VIEWPROVIDER_FROM_PARENT_URL: &str =
    "#meta/basemgr-with-viewprovider-from-parent.cm";
const MOCK_COBALT_URL: &str = "#meta/mock_cobalt.cm";
const SESSIONMGR_URL: &str = "fuchsia-pkg://fuchsia.com/sessionmgr#meta/sessionmgr.cmx";

struct TestFixture {
    pub builder: RealmBuilder,
    pub basemgr: ChildRef,
    pub placeholder: ChildRef,
}

impl TestFixture {
    async fn new(basemgr_url: &str) -> Result<TestFixture, Error> {
        let builder = RealmBuilder::new().await?;

        // Add mock_cobalt to the realm.
        let mock_cobalt =
            builder.add_child("mock_cobalt", MOCK_COBALT_URL, ChildOptions::new()).await?;
        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                    .from(Ref::parent())
                    .to(&mock_cobalt),
            )
            .await?;

        // Add basemgr to the realm.
        let basemgr =
            builder.add_child("basemgr", basemgr_url, ChildOptions::new().eager()).await?;
        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol_by_name(
                        "fuchsia.metrics.MetricEventLoggerFactory",
                    ))
                    .from(&mock_cobalt)
                    .to(&basemgr),
            )
            .await?;
        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                    .from(Ref::parent())
                    .to(&basemgr),
            )
            .await?;
        builder
            .add_route(
                Route::new()
                    .capability(Capability::storage("cache"))
                    .from(Ref::parent())
                    .to(&basemgr),
            )
            .await?;
        builder
            .add_route(
                Route::new()
                    .capability(Capability::storage("data"))
                    .from(Ref::parent())
                    .to(&basemgr),
            )
            .await?;

        // Add a placeholder component and routes for capabilities that are not
        // expected to be used in this test scenario.
        let placeholder = builder
            .add_local_child(
                "placeholder",
                |_: LocalComponentHandles| Box::pin(async move { Ok(()) }),
                ChildOptions::new(),
            )
            .await?;

        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol_by_name("fuchsia.tracing.provider.Registry"))
                    .capability(Capability::protocol_by_name(
                        "fuchsia.hardware.power.statecontrol.Admin",
                    ))
                    .from(&placeholder)
                    .to(&basemgr),
            )
            .await?;

        return Ok(TestFixture { builder, basemgr, placeholder });
    }

    async fn with_config(self, config: &'static str) -> Result<TestFixture, Error> {
        let config_data_dir = pseudo_directory! {
            "basemgr" => pseudo_directory! {
                "startup.config" => read_only(config),
            }
        };

        // Add a local component that provides the `config-data` directory to the realm.
        let config_data_server = self
            .builder
            .add_local_child(
                "config-data-server",
                move |handles| {
                    let proxy = spawn_vfs(config_data_dir.clone());
                    async move {
                        let _ = &handles;
                        let mut fs = ServiceFs::new();
                        fs.add_remote("config-data", proxy);
                        fs.serve_connection(handles.outgoing_dir)
                            .expect("failed to serve config-data ServiceFs");
                        fs.collect::<()>().await;
                        Ok::<(), anyhow::Error>(())
                    }
                    .boxed()
                },
                ChildOptions::new(),
            )
            .await?;

        self.builder
            .add_route(
                Route::new()
                    .capability(
                        Capability::directory("config-data")
                            .path("/config-data")
                            .rights(fio::R_STAR_DIR),
                    )
                    .from(&config_data_server)
                    .to(&self.basemgr),
            )
            .await?;

        Ok(self)
    }

    async fn with_default_config(self) -> Result<TestFixture, Error> {
        self.with_config(r#"{ "basemgr": { "enable_cobalt": false } }"#).await
    }

    async fn with_noop_presenter(self) -> Result<TestFixture, Error> {
        let presenter = self
            .builder
            .add_local_child(
                "presenter",
                move |handles| Box::pin(presenter_noop(handles)),
                ChildOptions::new(),
            )
            .await?;
        let () = self
            .builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol::<fuipolicy::PresenterMarker>())
                    .from(&presenter)
                    .to(&self.basemgr),
            )
            .await?;
        Ok(self)
    }

    async fn with_noop_sys_launcher(self) -> Result<TestFixture, Error> {
        let sys_launcher = self
            .builder
            .add_local_child(
                "sys_launcher",
                move |handles| Box::pin(sys_launcher_noop(handles)),
                ChildOptions::new(),
            )
            .await?;
        let () = self
            .builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol::<fsys::LauncherMarker>())
                    .from(&sys_launcher)
                    .to(&self.basemgr),
            )
            .await?;
        Ok(self)
    }

    // Vend a placeholder implementation `fuchsia.session.Restarter`
    // because this test doesn't expect to have this protocol exercised.
    async fn with_placeholder_restarter(self) -> Result<TestFixture, Error> {
        let () = self
            .builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol::<fsession::RestarterMarker>())
                    .from(&self.placeholder)
                    .to(&self.basemgr),
            )
            .await?;
        Ok(self)
    }

    // Vend a `fuchsia.session.Restarter` implementation that sends a message on
    // `restarter_sender` when the client calls `Restart`.
    async fn with_restarter_with_sender(
        self,
        sender: mpsc::Sender<()>,
    ) -> Result<TestFixture, Error> {
        let restarter = self
            .builder
            .add_local_child(
                "restarter_with_sender",
                move |handles| Box::pin(restarter_with_sender_impl(sender.clone(), handles)),
                ChildOptions::new(),
            )
            .await?;
        let () = self
            .builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol::<fsession::RestarterMarker>())
                    .from(&restarter)
                    .to(&self.basemgr),
            )
            .await?;
        Ok(self)
    }
}

// Tests that the session launches sessionmgr as a child v1 component.
#[fuchsia::test]
async fn test_launch_sessionmgr() -> Result<(), Error> {
    let fixture = TestFixture::new(BASEMGR_URL)
        .await?
        .with_default_config()
        .await?
        .with_noop_presenter()
        .await?
        .with_placeholder_restarter()
        .await?;

    // Add a local component that serves `fuchsia.sys.Launcher` to the realm.
    let (launch_info_sender, launch_info_receiver) = mpsc::channel(1);
    let sys_launcher = fixture
        .builder
        .add_local_child(
            "sys_launcher",
            move |handles| Box::pin(sys_launcher_local_child(launch_info_sender.clone(), handles)),
            ChildOptions::new(),
        )
        .await?;
    let () = fixture
        .builder
        .add_route(
            Route::new()
                .capability(Capability::protocol::<fsys::LauncherMarker>())
                .from(&sys_launcher)
                .to(&fixture.basemgr),
        )
        .await?;

    let instance = fixture.builder.build().await?;

    // The session should have started sessionmgr as a v1 component.
    let launch_info =
        launch_info_receiver.take(1).next().await.ok_or_else(|| anyhow!("expected LaunchInfo"))?;
    assert_eq!(SESSIONMGR_URL, launch_info.url);

    instance.destroy().await?;

    Ok(())
}

// Serves an implementation of the `fuchsia.sys.Launcher` protocol that forwards
// the `LaunchInfo` of created components to `launch_info_sender`.
async fn sys_launcher_local_child(
    launch_info_sender: mpsc::Sender<fsys::LaunchInfo>,
    handles: LocalComponentHandles,
) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(move |mut stream: fsys::LauncherRequestStream| {
        let mut launch_info_sender = launch_info_sender.clone();
        fasync::Task::local(async move {
            while let Some(fsys::LauncherRequest::CreateComponent {
                launch_info,
                controller,
                control_handle: _,
            }) = stream.try_next().await.expect("failed to serve Launcher")
            {
                let mut controller_stream = controller
                    .unwrap()
                    .into_stream()
                    .expect("failed to create stream of ComponentController requests");
                fasync::Task::spawn(async move {
                    if let Some(request) = controller_stream.try_next().await.unwrap() {
                        panic!("Unexpected ComponentController request: {:?}", request);
                    }
                })
                .detach();

                launch_info_sender.try_send(launch_info).expect("failed to send LaunchInfo");
            }
        })
        .detach();
    });
    fs.serve_connection(handles.outgoing_dir)?;
    fs.collect::<()>().await;
    Ok(())
}

// Tests that basemgr will launch all of connect to all Binder connections
// in its namespace by passing in in child names via the "--eager-child" flag.
// In production, basemgr will have actual child components and will have
// fuchsia.component.Binder entries as a result of "use from child" clauses.
// However, in this test, basemgr will not have any child components but will
// have the expected Binder protocols routed to it via Realm Builder below.
// This allows us to use a local component implementation to assert that basemgr
// does in fact connect to expected Binder path.
#[fuchsia::test]
async fn test_launch_v2_eager_children() -> Result<(), Error> {
    const NUM_TRIES: usize = 2;
    const BASEMGR_WITH_EAGER_CHILDREN_URL: &str = "#meta/basemgr-with-eager-children.cm";
    const EAGER_CHILDREN: [&str; 2] = ["foo", "bar"];

    let fixture = TestFixture::new(BASEMGR_WITH_EAGER_CHILDREN_URL)
        .await?
        .with_default_config()
        .await?
        .with_noop_presenter()
        .await?
        .with_placeholder_restarter()
        .await?
        .with_noop_sys_launcher()
        .await?;

    let mut child_restart_futs = vec![];
    for child_name in EAGER_CHILDREN.iter() {
        let binder_path = format!("fuchsia.component.Binder.{}", child_name);

        // The function |basemgr_child_impl| is structured such that when it
        // encounters a connection to the Binder protocol, it will exit. This will
        // trigger a component Stopped event which should notify `basemgr` that the
        // component exited. This is repeated to ensure that `basemgr` not only
        // starts the component, but also that it restarts it.
        let (binder_sender, binder_receiver) = mpsc::channel(NUM_TRIES);
        let binder_path_clone = binder_path.clone();
        let local_child = fixture
            .builder
            .add_local_child(
                child_name.to_string(),
                move |handles| {
                    Box::pin(basemgr_child_impl(
                        binder_sender.clone(),
                        binder_path.clone(),
                        handles,
                    ))
                },
                ChildOptions::new(),
            )
            .await?;
        fixture
            .builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol_by_name(binder_path_clone))
                    .from(&local_child)
                    .to(&fixture.basemgr),
            )
            .await?;

        let fut = async move {
            let mut stream = binder_receiver.take(NUM_TRIES);
            for _ in 0..NUM_TRIES {
                let binder_connected = stream.next().await.unwrap();
                assert!(binder_connected);
            }
        };
        child_restart_futs.push(fut);
    }

    let instance = fixture.builder.build().await?;

    let _ = join_all(child_restart_futs).await;

    // We have to destroy the instance after the test assertion because
    // basemgr teardown will be spammy with error logs due to closing sessionmgr's
    // ComponentController channel
    instance.destroy().await?;

    Ok(())
}

// Tests that failure to connect to critical children will yield a session restart.
// This is accomplished by *not* routing `fuchsia.component.Binder` to the
// basemgr component. basemgr will encounter PEER_CLOSED and trigger a session
// restart by calling |fuchsia.session/Restarter.Restart|.
#[fuchsia::test]
async fn test_restart_session_after_critical_child_crashes() -> Result<(), Error> {
    const BASEMGR_WITH_CRITICAL_CHILDREN_URL: &str = "#meta/basemgr-with-critical-children.cm";

    let (restart_sender, restart_receiver) = mpsc::channel(1);

    let fixture = TestFixture::new(BASEMGR_WITH_CRITICAL_CHILDREN_URL)
        .await?
        .with_default_config()
        .await?
        .with_noop_presenter()
        .await?
        .with_restarter_with_sender(restart_sender)
        .await?
        .with_noop_sys_launcher()
        .await?;

    let instance = fixture.builder.build().await?;

    let restart_requested = restart_receiver.take(1).next().await.is_some();
    assert!(restart_requested);

    // We have to destroy the instance after the test assertion because
    // basemgr teardown will be spammy with error logs due to closing sessionmgr's
    // ComponentController channel
    instance.destroy().await?;

    Ok(())
}

// Tests that sessionmgr crashing will yield a session restart.
#[fuchsia::test]
async fn test_restart_session_after_sessionmgr_crashes() -> Result<(), Error> {
    let (restart_sender, restart_receiver) = mpsc::channel(1);

    let fixture = TestFixture::new(BASEMGR_URL)
        .await?
        .with_default_config()
        .await?
        .with_restarter_with_sender(restart_sender)
        .await?;

    let sys_launcher = fixture
        .builder
        .add_local_child(
            "sys_launcher",
            move |handles| Box::pin(sys_launcher_crash(handles)),
            ChildOptions::new(),
        )
        .await?;
    let () = fixture
        .builder
        .add_route(
            Route::new()
                .capability(Capability::protocol_by_name("fuchsia.sys.Launcher"))
                .from(&sys_launcher)
                .to(&fixture.basemgr),
        )
        .await?;

    let instance = fixture.builder.build().await?;

    let restart_requested = restart_receiver.take(1).next().await.is_some();
    assert!(restart_requested);

    // We have to destroy the instance after the test assertion because
    // basemgr teardown will be spammy with error logs due to closing sessionmgr's
    // ComponentController channel
    instance.destroy().await?;

    Ok(())
}

// Tests that restart attempts for eager children are tracked in Inspect.
// The expectation is that basemgr emits the data in the Inspect tree like this:
// root:
//   ...
//   eager_children_restarts:
//      some_child_name: 2
//      some_other_name: 0
//
#[fuchsia::test]
async fn test_eager_children_restart_are_tracked_in_inspect() -> Result<(), Error> {
    const MAX_RESTART_ATTEMPTS: u64 = 3;
    const RESTART_TRACKER_PROP_KEY: &str = "eager_children_restarts";
    const BASEMGR_WITH_EAGER_CHILDREN_URL: &str = "#meta/basemgr-with-eager-children.cm";
    const EAGER_CHILDREN: [&str; 2] = ["foo", "bar"];

    let fixture = TestFixture::new(BASEMGR_WITH_EAGER_CHILDREN_URL)
        .await?
        .with_default_config()
        .await?
        .with_noop_presenter()
        .await?
        .with_placeholder_restarter()
        .await?
        .with_noop_sys_launcher()
        .await?;

    let instance = fixture.builder.build().await?;
    let moniker = format!("realm_builder\\:{}/basemgr", instance.root.child_name());

    let mut child_restart_futs = vec![];
    for child_name in EAGER_CHILDREN.iter() {
        let moniker = moniker.clone();
        let fut = async move {
            // We don't know when the restart attempts will added to Inspect.
            // So in order to prevent a race condition where we assert that the
            // the final count is MAX_RESTART_ATTEMPTS before basemgr has
            // actually written that, we'll loop until that is the case.
            // We can consider that exit condition the "assertion" of this test
            // case.
            loop {
                let results = ArchiveReader::new()
                    .add_selector(format!("{}:root/eager_children_restarts", moniker))
                    .snapshot::<Inspect>()
                    .await
                    .unwrap();
                assert_eq!(results.len(), 1);
                // First, fetch the root node's payload.
                let payload = results.first().unwrap().payload.as_ref().unwrap();
                // Then, fetch child node namd `eager_children_restarts`.
                let payload = payload.get_child(RESTART_TRACKER_PROP_KEY).unwrap();
                let prop = payload.get_property(child_name).unwrap().uint().unwrap();

                assert!(!prop > MAX_RESTART_ATTEMPTS);
                // Sadly, Rust doesn't implement Eq trait for &u64 and u64.
                if prop == &MAX_RESTART_ATTEMPTS {
                    break;
                }
            }
        };
        child_restart_futs.push(fut);
    }

    let _ = join_all(child_restart_futs).await;

    // We have to destroy the instance after the test assertion because
    // basemgr teardown will be spammy with error logs due to closing sessionmgr's
    // ComponentController channel
    instance.destroy().await?;

    Ok(())
}

// Tests that basemgr presents a view from a v2 session shell acquired through ViewProvider.
#[fuchsia::test]
async fn test_v2_session_shell() -> Result<(), Error> {
    let fixture = TestFixture::new(BASEMGR_WITH_VIEWPROVIDER_FROM_PARENT_URL)
        .await?
        .with_default_config()
        .await?
        .with_placeholder_restarter()
        .await?
        .with_noop_sys_launcher()
        .await?;

    // Add a local component that serves `fuchsia.ui.policy.Presenter` to the realm.
    let (holder_token_sender, holder_token_receiver) = mpsc::channel(1);
    let presenter = fixture
        .builder
        .add_local_child(
            "presenter",
            move |handles| Box::pin(presenter_with_sender(holder_token_sender.clone(), handles)),
            ChildOptions::new(),
        )
        .await?;
    let () = fixture
        .builder
        .add_route(
            Route::new()
                .capability(Capability::protocol::<fuipolicy::PresenterMarker>())
                .from(&presenter)
                .to(&fixture.basemgr),
        )
        .await?;

    // Add a local component that serves `fuchsia.ui.app.ViewProvider` to the realm.
    let (token_sender, token_receiver) = mpsc::channel(1);
    let session_shell = fixture
        .builder
        .add_local_child(
            "session_shell",
            move |handles| Box::pin(view_provider_local_child(token_sender.clone(), handles)),
            ChildOptions::new(),
        )
        .await?;
    let () = fixture
        .builder
        .add_route(
            Route::new()
                .capability(Capability::protocol::<fuiapp::ViewProviderMarker>())
                .from(&session_shell)
                .to(&fixture.basemgr),
        )
        .await?;

    let _instance = fixture.builder.build().await?;

    // basemgr should have created a view via ViewProvider and presented it via Presenter.
    assert!(token_receiver.take(1).next().await.is_some());
    assert!(holder_token_receiver.take(1).next().await.is_some());

    Ok(())
}

// Serves an implementation of the `fuchsia.ui.app.ViewProvider` protocol that forwards
// the view token of created views to `token_sender`.
async fn view_provider_local_child(
    token_sender: mpsc::Sender<zx::EventPair>,
    handles: LocalComponentHandles,
) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(move |mut stream: fuiapp::ViewProviderRequestStream| {
        let mut token_sender = token_sender.clone();
        fasync::Task::local(async move {
            while let Some(request) = stream.try_next().await.expect("failed to serve ViewProvider")
            {
                match request {
                    fuiapp::ViewProviderRequest::CreateView { token, .. }
                    | fuiapp::ViewProviderRequest::CreateViewWithViewRef { token, .. } => {
                        token_sender.try_send(token).expect("failed to send view token");
                    }
                    _ => panic!("unexpected ViewProvider request"),
                }
            }
        })
        .detach();
    });
    fs.serve_connection(handles.outgoing_dir)?;
    fs.collect::<()>().await;
    Ok(())
}

// Local implementation that pretends to be a basemgr child. Its connection
// to |fuchsia.component.Binder| is used to signal to basemgr that its still
// running. If this function receives a signal on |binder_sender| it will exit,
// thus closing the Binder channel that basemgr holds.
async fn basemgr_child_impl(
    mut binder_sender: mpsc::Sender<bool>,
    binder_path: String,
    handles: LocalComponentHandles,
) -> Result<(), Error> {
    // This check is added here because basemgr will continue to attempts restart
    // of this child component after the test assertions in |test_launch_v2_children|
    // completes.
    if binder_sender.is_closed() {
        return Ok(());
    }

    let (mut reboot_sender, reboot_receiver) = mpsc::channel(1);
    let svc_fut = async move {
        let _ = &handles;
        let mut fs = ServiceFs::new();
        fs.dir("svc").add_fidl_service_at(
            binder_path,
            move |_stream: fcomponent::BinderRequestStream| {
                binder_sender.try_send(true).expect("failed to send message");
                reboot_sender.try_send(true).expect("failed to send message");
            },
        );
        fs.serve_connection(handles.outgoing_dir).unwrap();
        fs.collect::<()>().await;
    }
    .fuse();
    futures::pin_mut!(svc_fut);
    let reboot_fut = async move {
        reboot_receiver.take(1).next().await.unwrap();
    }
    .fuse();
    futures::pin_mut!(reboot_fut);

    futures::select! {
        _ = svc_fut => panic!("vfs server should not stop"),
        _ = reboot_fut => {}
    }

    Ok(())
}

async fn restarter_with_sender_impl(
    restart_sender: mpsc::Sender<()>,
    handles: LocalComponentHandles,
) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(move |mut stream: fsession::RestarterRequestStream| {
        let mut restart_sender = restart_sender.clone();
        fasync::Task::local(async move {
            while let Some(request) = stream.try_next().await.unwrap() {
                match request {
                    fsession::RestarterRequest::Restart { .. } => {
                        restart_sender.try_send(()).expect("failed to send message");
                    }
                }
            }
        })
        .detach();
    });
    fs.serve_connection(handles.outgoing_dir).unwrap();
    fs.collect::<()>().await;

    Ok(())
}

// Returns a `DirectoryProxy` that serves the directory entry `dir`.
fn spawn_vfs(dir: Arc<dyn DirectoryEntry>) -> fio::DirectoryProxy {
    let (client_end, server_end) =
        fidl::endpoints::create_endpoints::<fio::DirectoryMarker>().unwrap();
    let scope = vfs::execution_scope::ExecutionScope::new();
    dir.open(
        scope,
        fio::OpenFlags::RIGHT_READABLE,
        vfs::path::Path::dot(),
        ServerEnd::new(server_end.into_channel()),
    );
    client_end.into_proxy().unwrap()
}

// Implements a `fuchsia.sys.Launcher` that drops the controller passed to
// `CreateComponent`, simulating the component crashing instantly.
async fn sys_launcher_crash(handles: LocalComponentHandles) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(|mut stream: fsys::LauncherRequestStream| {
        fasync::Task::local(async move {
            while let Some(request) = stream.try_next().await.expect("failed to serve Launcher") {
                match request {
                    fsys::LauncherRequest::CreateComponent {
                        launch_info: _,
                        controller: _,
                        control_handle: _,
                    } => {
                        // Do nothing; controller is dropped.
                    }
                }
            }
        })
        .detach();
    });
    fs.serve_connection(handles.outgoing_dir)?;
    fs.collect::<()>().await;
    Ok(())
}

// Implements a `fuchsia.sys.Launcher` that launches a component that does nothing but
// serve its outgoing directory.
async fn sys_launcher_noop(handles: LocalComponentHandles) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(|mut stream: fsys::LauncherRequestStream| {
        fasync::Task::local(async move {
            while let Some(request) = stream.try_next().await.expect("failed to serve Launcher") {
                match request {
                    fsys::LauncherRequest::CreateComponent {
                        launch_info,
                        controller,
                        control_handle: _,
                    } => {
                        let () = serve_sessionmgr(
                            launch_info
                                .directory_request
                                .expect("no fio::DirectoryRequest received"),
                            controller.unwrap(),
                        )
                        .await
                        .unwrap();
                    }
                }
            }
        })
        .detach();
    });
    fs.serve_connection(handles.outgoing_dir)?;
    fs.collect::<()>().await;
    Ok(())
}

// Implements a `fuchsia.ui.policy.Presenter` that does nothing.
async fn presenter_noop(handles: LocalComponentHandles) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(|mut stream: fuipolicy::PresenterRequestStream| {
        fasync::Task::local(async move {
            while let Some(request) = stream.try_next().await.expect("failed to serve Presenter") {
                match request {
                    fuipolicy::PresenterRequest::PresentView { .. }
                    | fuipolicy::PresenterRequest::PresentOrReplaceView { .. }
                    | fuipolicy::PresenterRequest::PresentOrReplaceView2 { .. } => {}
                }
            }
        })
        .detach();
    });
    fs.serve_connection(handles.outgoing_dir)?;
    fs.collect::<()>().await;
    Ok(())
}

// Implements a `fuchsia.ui.policy.Presenter` that sends the presented ViewHolderToken
// over a channel.
async fn presenter_with_sender(
    token_sender: mpsc::Sender<fuiviews::ViewHolderToken>,
    handles: LocalComponentHandles,
) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(|mut stream: fuipolicy::PresenterRequestStream| {
        let mut token_sender = token_sender.clone();
        fasync::Task::local(async move {
            while let Some(request) = stream.try_next().await.expect("failed to serve Presenter") {
                match request {
                    fuipolicy::PresenterRequest::PresentView { view_holder_token, .. }
                    | fuipolicy::PresenterRequest::PresentOrReplaceView {
                        view_holder_token, ..
                    }
                    | fuipolicy::PresenterRequest::PresentOrReplaceView2 {
                        view_holder_token,
                        ..
                    } => {
                        token_sender
                            .try_send(view_holder_token)
                            .expect("Failed to send ViewHolderToken");
                    }
                }
            }
        })
        .detach();
    });
    fs.serve_connection(handles.outgoing_dir)?;
    fs.collect::<()>().await;
    Ok(())
}

// We keep a reference to |_controller| to ensure the that channel doesn't close
// while Sessionmgr is being served.
async fn serve_sessionmgr(
    channel: ServerEnd<fio::DirectoryMarker>,
    _controller: ServerEnd<fsys::ComponentControllerMarker>,
) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.add_fidl_service(move |mut stream: fmodular::SessionmgrRequestStream| {
        fasync::Task::local(async move { while let Some(_) = stream.try_next().await.unwrap() {} })
            .detach();
    });
    fs.serve_connection(channel)?;
    fs.collect::<()>().await;
    Ok(())
}
