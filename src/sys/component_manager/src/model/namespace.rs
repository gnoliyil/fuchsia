// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        constants::PKG_PATH,
        model::{
            component::{ComponentInstance, Package, WeakComponentInstance},
            error::NamespacePopulateError,
            routing::{self, route_and_open_capability, OpenOptions},
        },
    },
    ::routing::{
        capability_source::ComponentCapability, component_instance::ComponentInstanceInterface,
        mapper::NoopRouteMapper, rights::Rights, route_to_storage_decl,
        verify_instance_in_component_id_index, RouteRequest,
    },
    clonable_error::ClonableError,
    cm_logger::scoped::ScopedLogger,
    cm_rust::{self, CapabilityPath, ComponentDecl, UseDecl, UseProtocolDecl},
    cm_task_scope::TaskScope,
    fidl::{
        endpoints::{create_endpoints, ClientEnd, ServerEnd},
        prelude::*,
    },
    fidl_fuchsia_component_runner as fcrunner, fidl_fuchsia_io as fio,
    fidl_fuchsia_logger::LogSinkMarker,
    fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::future::BoxFuture,
    std::{collections::HashMap, sync::Arc},
    tracing::{error, info, warn, Subscriber},
    vfs::{
        directory::entry::DirectoryEntry, directory::helper::DirectlyMutable,
        directory::immutable::simple as pfs, execution_scope::ExecutionScope, path::Path,
        remote::remote,
    },
};

type Directory = Arc<pfs::Simple>;

pub struct IncomingNamespace {
    pub package_dir: Option<fio::DirectoryProxy>,
    logger: Option<Arc<ScopedLogger>>,
}

impl IncomingNamespace {
    pub fn new(package: Option<Package>) -> Self {
        let package_dir = package.map(|p| p.package_dir);
        Self { package_dir, logger: None }
    }

    /// Returns a Logger whose output is attributed to this component's
    /// namespace.
    pub fn get_attributed_logger(&self) -> Option<Arc<dyn Subscriber + Send + Sync>> {
        self.logger.as_ref().map(|l| l.clone() as Arc<dyn Subscriber + Send + Sync>)
    }

    /// In addition to populating a Vec<fcrunner::ComponentNamespaceEntry>, `populate` will start
    /// serving and install handles to pseudo directories.
    pub async fn populate<'a>(
        &mut self,
        component: &Arc<ComponentInstance>,
        decl: &'a ComponentDecl,
    ) -> Result<Vec<fcrunner::ComponentNamespaceEntry>, NamespacePopulateError> {
        let (mut ns, log_sink_decl) =
            populate_and_get_logsink_decl(self.package_dir.as_ref(), component, decl).await?;

        if let Some(log_decl) = &log_sink_decl {
            let (ns_, logger) = get_logger_from_ns(ns, log_decl).await;
            ns = ns_;
            self.logger = logger.map(Arc::new);
        }

        Ok(ns)
    }
}

/// Populates a Vec<fcrunner::ComponentNamespaceEntry>, starts serving the directories,
/// installs handles to pseudo directories and returns the LogSink use declaration, if any.
///
/// The LogSink use declaration is used by `IncomingNamespace` to create an attributed logger.
pub async fn populate_and_get_logsink_decl<'a>(
    package_dir: Option<&fio::DirectoryProxy>,
    component: &Arc<ComponentInstance>,
    decl: &'a ComponentDecl,
) -> Result<(Vec<fcrunner::ComponentNamespaceEntry>, Option<UseProtocolDecl>), NamespacePopulateError>
{
    let mut ns: Vec<fcrunner::ComponentNamespaceEntry> = vec![];

    // Populate the /pkg namespace.
    if let Some(package_dir) = &package_dir {
        add_pkg_directory(&mut ns, package_dir)?;
    }

    // Populate the namespace from uses, using the component manager's namespace.
    // svc_dirs will hold (path,directory) pairs. Each pair holds a path in the
    // component's namespace and a directory that ComponentMgr will host for the component.
    let mut svc_dirs = HashMap::new();

    // directory_waiters will hold Future<Output=()> objects that will wait for activity on
    // a channel and then route the channel to the appropriate component's out directory.
    let mut directory_waiters = Vec::new();

    let mut log_sink_decl: Option<UseProtocolDecl> = None;
    for use_ in &decl.uses {
        match use_ {
            cm_rust::UseDecl::Directory(_) => {
                add_directory_helper(&mut ns, &mut directory_waiters, use_, component.as_weak());
            }
            cm_rust::UseDecl::Protocol(s) => {
                add_service_or_protocol_use(
                    &mut svc_dirs,
                    UseDecl::Protocol(s.clone()),
                    &s.target_path,
                    component.as_weak(),
                );
                if s.source_name == LogSinkMarker::PROTOCOL_NAME {
                    log_sink_decl = Some(s.clone());
                }
            }
            cm_rust::UseDecl::Service(s) => {
                add_service_or_protocol_use(
                    &mut svc_dirs,
                    UseDecl::Service(s.clone()),
                    &s.target_path,
                    component.as_weak(),
                );
            }
            cm_rust::UseDecl::Storage(_) => {
                add_storage_use(&mut ns, &mut directory_waiters, use_, component).await?;
            }
            cm_rust::UseDecl::EventStream(s) => {
                add_service_or_protocol_use(
                    &mut svc_dirs,
                    UseDecl::EventStream(s.clone()),
                    &s.target_path,
                    component.as_weak(),
                );
            }
        }
    }

    // Start hosting the services directories and add them to the namespace
    serve_and_install_svc_dirs(&mut ns, svc_dirs);
    // The directory waiter will run in the component's nonblocking task scope, but
    // when it gets a readable signal it will spawn the routing task in the blocking scope as
    // that it blocks destruction, like service and protocol routing.
    //
    // TODO(fxbug.dev/76579): It would probably be more correct to run this in an execution_scope
    // attached to the namespace (but that requires a bigger refactor)
    start_directory_waiters(directory_waiters, &component.nonblocking_task_scope()).await;

    Ok((ns, log_sink_decl))
}

/// Given the set of namespace entries and a LogSink protocol's
/// `UseProtocolDecl`, look through the namespace for where to connect
/// to the LogSink protocol. The log connection, if any, is stored in the
/// IncomingNamespace.
async fn get_logger_from_ns(
    ns: Vec<fcrunner::ComponentNamespaceEntry>,
    log_sink_decl: &UseProtocolDecl,
) -> (Vec<fcrunner::ComponentNamespaceEntry>, Option<ScopedLogger>) {
    // A new set of namespace entries is returned because when the entry
    // used to connect to LogSink is found, that entry is consumed. A
    // matching entry is created and placed in the set of entries returned
    // by this function. `self` is taken as mutable so the
    // logger connection can be stored when found.
    let mut new_ns = vec![];
    let mut log_ns_dir: Option<(fcrunner::ComponentNamespaceEntry, String)> = None;
    let mut logger = None;
    // Find namespace directory specified in the log_sink_decl
    for ns_dir in ns {
        if let Some(path) = &ns_dir.path.clone() {
            // Check if this namespace path is a stem of the decl's path
            if log_ns_dir.is_none() {
                if let Ok(path_remainder) =
                    is_subpath_of(log_sink_decl.target_path.to_string(), path.to_string())
                {
                    log_ns_dir = Some((ns_dir, path_remainder));
                    continue;
                }
            }
        }
        new_ns.push(ns_dir);
    }

    // If we found a matching namespace entry, try to open the log proxy
    if let Some((mut entry, remaining_path)) = log_ns_dir {
        if let Some(dir) = entry.directory {
            let _str = log_sink_decl.target_path.to_string();
            let (restored_dir, logger_) = get_logger_from_dir(dir, &remaining_path).await;
            entry.directory = restored_dir;
            logger = logger_;
        }
        new_ns.push(entry);
    }
    (new_ns, logger)
}

/// add_pkg_directory will add a handle to the component's package under /pkg in the namespace.
fn add_pkg_directory(
    ns: &mut Vec<fcrunner::ComponentNamespaceEntry>,
    package_dir: &fio::DirectoryProxy,
) -> Result<(), NamespacePopulateError> {
    let clone_dir_proxy =
        fuchsia_fs::directory::clone_no_describe(package_dir, None).map_err(|e| {
            NamespacePopulateError::ClonePkgDirFailed(ClonableError::from(anyhow::Error::from(e)))
        })?;
    let cloned_dir = ClientEnd::new(
        clone_dir_proxy
            .into_channel()
            .expect("could not convert directory to channel")
            .into_zx_channel(),
    );
    ns.push(fcrunner::ComponentNamespaceEntry {
        path: Some(PKG_PATH.to_str().unwrap().to_string()),
        directory: Some(cloned_dir),
        ..fcrunner::ComponentNamespaceEntry::EMPTY
    });
    Ok(())
}

/// Adds a directory waiter to `waiters` and updates `ns` to contain a handle for the
/// storage described by `use_`. Once the channel is readable, the future calls
/// `route_storage` to forward the channel to the source component's outgoing directory and
/// terminates.
async fn add_storage_use(
    ns: &mut Vec<fcrunner::ComponentNamespaceEntry>,
    waiters: &mut Vec<BoxFuture<'_, ()>>,
    use_: &UseDecl,
    component: &Arc<ComponentInstance>,
) -> Result<(), NamespacePopulateError> {
    // Prevent component from using storage capability if it is restricted to the component ID
    // index, and the component isn't in the index.
    match use_ {
        UseDecl::Storage(use_storage_decl) => {
            // To check that the storage capability is restricted to the storage decl, we have
            // to resolve the storage source capability. Because storage capabilities are only
            // ever `offer`d down the component tree, and we always resolve parents before
            // children, this resolution will walk the cache-happy path.
            // TODO(dgonyeo): Eventually combine this logic with the general-purpose startup
            // capability check.
            if let Ok(source) =
                route_to_storage_decl(use_storage_decl.clone(), &component, &mut NoopRouteMapper)
                    .await
            {
                verify_instance_in_component_id_index(&source, &component)
                    .map_err(|e| NamespacePopulateError::InstanceNotInInstanceIdIndex(e))?;
            }
        }
        _ => unreachable!("unexpected storage decl"),
    }

    add_directory_helper(ns, waiters, use_, component.as_weak());
    Ok(())
}

/// Adds a directory waiter to `waiters` and updates `ns` to contain a handle for the
/// directory described by `use_`. Once the channel is readable, the future calls
/// `route_directory` to forward the channel to the source component's outgoing directory and
/// terminates.
fn add_directory_helper(
    ns: &mut Vec<fcrunner::ComponentNamespaceEntry>,
    waiters: &mut Vec<BoxFuture<'_, ()>>,
    use_: &UseDecl,
    component: WeakComponentInstance,
) {
    let target_path =
        use_.path().expect("use decl without path used in add_directory_helper").to_string();
    let flags = match use_ {
        UseDecl::Directory(dir) => Rights::from(dir.rights).into_legacy(),
        UseDecl::Storage(_) => fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        _ => panic!("not a directory or storage capability"),
    };

    // Specify that the capability must be opened as a directory. In particular, this affects
    // how a devfs-based capability will handle the open call. If this flag is not specified,
    // devfs attempts to open the directory as a service, which is not what is desired here.
    let flags = flags | fio::OpenFlags::DIRECTORY;

    let use_ = use_.clone();
    let (client_end, server_end) = create_endpoints();
    let route_on_usage = async move {
        // Wait for the channel to become readable.
        let server_end = fasync::Channel::from_channel(server_end.into_channel())
            .expect("failed to convert server_end into async channel");
        let on_signal_fut = fasync::OnSignals::new(&server_end, zx::Signals::CHANNEL_READABLE);
        on_signal_fut.await.unwrap();
        let target = match component.upgrade() {
            Ok(component) => component,
            Err(e) => {
                error!(
                    "failed to upgrade WeakComponentInstance routing use \
                        decl `{:?}`: {:?}",
                    &use_, e
                );
                return;
            }
        };
        let server_end = server_end.into_zx_channel();
        // Spawn a separate task to perform routing in the blocking scope. This way it won't
        // block namespace teardown, but it will block component destruction.
        target
            .blocking_task_scope()
            .add_task(route_directory(target, use_, flags, server_end))
            .await;
    };

    waiters.push(Box::pin(route_on_usage));
    ns.push(fcrunner::ComponentNamespaceEntry {
        path: Some(target_path.clone()),
        directory: Some(client_end),
        ..fcrunner::ComponentNamespaceEntry::EMPTY
    });
}

async fn route_directory(
    target: Arc<ComponentInstance>,
    use_: UseDecl,
    flags: fio::OpenFlags,
    mut server_end: zx::Channel,
) {
    let (route_request, open_options) = match &use_ {
        UseDecl::Directory(use_dir_decl) => (
            RouteRequest::UseDirectory(use_dir_decl.clone()),
            OpenOptions {
                flags: flags | fio::OpenFlags::DIRECTORY,
                relative_path: String::new(),
                server_chan: &mut server_end,
            },
        ),
        UseDecl::Storage(use_storage_decl) => (
            RouteRequest::UseStorage(use_storage_decl.clone()),
            OpenOptions {
                flags: flags | fio::OpenFlags::DIRECTORY,
                relative_path: ".".into(),
                server_chan: &mut server_end,
            },
        ),
        _ => panic!("not a directory or storage capability"),
    };
    if let Err(e) = route_and_open_capability(route_request, &target, open_options).await {
        routing::report_routing_failure(&target, &ComponentCapability::Use(use_), &e, server_end)
            .await;
    }
}

/// start_directory_waiters will spawn the futures in directory_waiters
async fn start_directory_waiters(
    directory_waiters: Vec<BoxFuture<'static, ()>>,
    scope: &TaskScope,
) {
    for waiter in directory_waiters {
        // The future for a directory waiter will only terminate once the directory channel is
        // first used. Run the future in a task bound to the component's scope instead of
        // calling await on it directly.
        scope.add_task(waiter).await;
    }
}

/// Adds a service broker in `svc_dirs` for service described by `use_`. The service will be
/// proxied to the outgoing directory of the source component.
fn add_service_or_protocol_use(
    svc_dirs: &mut HashMap<String, Directory>,
    use_: UseDecl,
    capability_path: &CapabilityPath,
    component: WeakComponentInstance,
) {
    let not_found_component_copy = component.clone();
    let use_clone = use_.clone();
    let route_open_fn = move |scope: ExecutionScope,
                              flags: fio::OpenFlags,
                              relative_path: Path,
                              server_end: ServerEnd<fio::NodeMarker>| {
        let use_ = use_.clone();
        let component = component.clone();
        let component = match component.upgrade() {
            Ok(component) => component,
            Err(e) => {
                error!(
                    "failed to upgrade WeakComponentInstance routing use \
                            decl `{:?}`: {:?}",
                    &use_, e
                );
                return;
            }
        };
        let target = component.clone();
        let task = async move {
            let mut server_end = server_end.into_channel();
            let (route_request, open_options) = {
                match &use_ {
                            UseDecl::Service(use_service_decl) => {
                                (RouteRequest::UseService(use_service_decl.clone()),
                                     OpenOptions{
                                         flags,
                                         relative_path: relative_path.into_string(),
                                         server_chan: &mut server_end
                                     }
                                 )
                            },
                            UseDecl::Protocol(use_protocol_decl) => {
                                (RouteRequest::UseProtocol(use_protocol_decl.clone()),
                                     OpenOptions{
                                         flags,
                                         relative_path: relative_path.into_string(),
                                         server_chan: &mut server_end
                                     }
                                 )
                            },
                            UseDecl::EventStream(stream)=> {
                                (RouteRequest::UseEventStream(stream.clone()),
                                     OpenOptions{
                                         flags,
                                         relative_path: stream.target_path.to_string(),
                                         server_chan: &mut server_end,
                                     }
                                 )
                            },
                            _ => panic!("add_service_or_protocol_use called with non-service or protocol capability"),
                        }
            };

            let res =
                routing::route_and_open_capability(route_request, &target, open_options).await;
            if let Err(e) = res {
                routing::report_routing_failure(
                    &target,
                    &ComponentCapability::Use(use_),
                    &e,
                    server_end,
                )
                .await;
            }
        };
        scope.spawn(async move { component.blocking_task_scope().add_task(task).await })
    };

    let service_dir = svc_dirs.entry(capability_path.dirname.clone().into()).or_insert_with(|| {
        make_dir_with_not_found_logging(
            capability_path.dirname.clone().into(),
            not_found_component_copy,
        )
    });
    // NOTE: UseEventStream is special, in that we can route a single stream from multiple
    // sources (merging them).
    if matches!(use_clone, UseDecl::EventStream(_)) {
        // Ignore duplication error if already exists
        service_dir.clone().add_entry(&capability_path.basename, remote(route_open_fn)).ok();
    } else {
        service_dir
            .clone()
            .add_entry(&capability_path.basename, remote(route_open_fn))
            .expect("could not add service to directory");
    }
}

/// Determines if the `full` is a subpath of the `stem`. Returns the
/// remaining portion of the path if `full` us a subpath. Returns Error if
/// `stem` and `full` are the same.
fn is_subpath_of(full: String, stem: String) -> Result<String, ()> {
    let stem_path = std::path::Path::new(&stem);
    let full_path = std::path::Path::new(&full);

    let remainder = full_path
        .strip_prefix(stem_path)
        // Unwrapping the `to_str` conversion should be safe here since we
        // started with a Unicode value, put it into a path and now are
        // extracting a portion of that value.
        .map(|path| path.to_str().unwrap().to_string())
        .map_err(|_| ())?;

    if remainder.is_empty() {
        Err(())
    } else {
        Ok(remainder)
    }
}

/// serve_and_install_svc_dirs will take all of the pseudo directories collected in
/// svc_dirs (as populated by add_service_use calls), start them and install them in the
/// namespace. The abortable handles are saved in the IncomingNamespace, to
/// be called when the IncomingNamespace is dropped.
fn serve_and_install_svc_dirs(
    ns: &mut Vec<fcrunner::ComponentNamespaceEntry>,
    svc_dirs: HashMap<String, Directory>,
) {
    for (target_dir_path, pseudo_dir) in svc_dirs {
        let (client_end, server_end) = create_endpoints::<fio::DirectoryMarker>();
        pseudo_dir.clone().open(
            ExecutionScope::new(),
            // TODO(https://fxbug.dev/101092): remove RIGHT_READABLE svc_{for,from}_sys have been
            // dismantled.
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DIRECTORY,
            Path::dot(),
            server_end.into_channel().into(),
        );

        ns.push(fcrunner::ComponentNamespaceEntry {
            path: Some(target_dir_path),
            directory: Some(client_end),
            ..fcrunner::ComponentNamespaceEntry::EMPTY
        });
    }
}

/// Given a Directory, connect to the LogSink protocol at the default
/// location.
async fn get_logger_from_dir(
    dir: ClientEnd<fio::DirectoryMarker>,
    at_path: &str,
) -> (Option<ClientEnd<fio::DirectoryMarker>>, Option<ScopedLogger>) {
    let mut logger = None;
    match dir.into_proxy() {
        Ok(dir_proxy) => {
            match ScopedLogger::from_directory(&dir_proxy, at_path) {
                Ok(ns_logger) => {
                    logger = Some(ns_logger);
                }
                Err(error) => {
                    info!(%error, "LogSink.Connect() failed, logs will be attributed to component manager");
                }
            }

            // Now that we have the LogSink and socket, put the LogSink
            // protocol directory back where we found it.
            (
                dir_proxy.into_channel().map_or_else(
                    |error| {
                        error!(?error, "LogSink proxy could not be converted back to channel");
                        None
                    },
                    |chan| Some(ClientEnd::<fio::DirectoryMarker>::new(chan.into())),
                ),
                logger,
            )
        }
        Err(error) => {
            info!(%error, "Directory client channel could not be turned into proxy");
            (None, logger)
        }
    }
}

fn make_dir_with_not_found_logging(
    root_path: String,
    component_for_logger: WeakComponentInstance,
) -> Arc<pfs::Simple> {
    let new_dir = pfs::simple();
    // Grab a copy of the directory path, it will be needed if we log a
    // failed open request.
    new_dir.clone().set_not_found_handler(Box::new(move |path| {
        // Clone the component pointer and pass the copy into the logger.
        let component_for_logger = component_for_logger.clone();
        let requested_path = format!("{}/{}", root_path, path);

        // Spawn a task which logs the error. It would be nicer to not
        // spawn a task, but locking the component is async and this
        // closure is not.
        fasync::Task::spawn(async move {
            match component_for_logger.upgrade() {
                Ok(target) => {
                    target
                        .with_logger_as_default(|| {
                            warn!(
                                "No capability available at path {} for component {}, \
                                verify the component has the proper `use` declaration.",
                                requested_path, target.abs_moniker
                            );
                        })
                        .await;
                }
                Err(_) => {}
            }
        })
        .detach();
    }));
    new_dir
}

#[cfg(test)]
pub mod test {

    use {
        super::*,
        crate::model::testing::test_helpers::MockServiceRequest,
        cm_rust::{Availability, CapabilityPath, DependencyType, UseProtocolDecl, UseSource},
        fidl::endpoints,
        fidl_fuchsia_component_runner as fcrunner,
        fidl_fuchsia_logger::{LogSinkMarker, LogSinkRequest},
        fuchsia_async,
        fuchsia_component::server::ServiceFs,
        futures::StreamExt,
        std::{
            convert::TryFrom,
            sync::{
                atomic::{AtomicU8, Ordering},
                Arc,
            },
        },
    };

    #[fuchsia::test]
    fn test_subpath_handling() {
        let mut stem = "/".to_string();
        let mut full = "/".to_string();
        assert_eq!(is_subpath_of(full, stem), Err(()));

        stem = "/".to_string();
        full = "/subdir".to_string();
        assert_eq!(is_subpath_of(full, stem), Ok("subdir".to_string()));

        stem = "/subdir1/subdir2".to_string();
        full = "/subdir1/file.txt".to_string();
        assert_eq!(is_subpath_of(full, stem), Err(()));

        stem = "/this/path/has/a/typ0".to_string();
        full = "/this/path/has/a/typo/not/exclamation/point".to_string();
        assert_eq!(is_subpath_of(full, stem), Err(()));

        stem = "/subdir1".to_string();
        full = "/subdir1/subdir2/subdir3/file.txt".to_string();
        assert_eq!(is_subpath_of(full, stem), Ok("subdir2/subdir3/file.txt".to_string()));
    }

    #[fuchsia::test]
    /// Tests that the logger is connected to when it is in a subdirectory of a
    /// namespace entry.
    async fn test_logger_at_root_of_entry() {
        let log_decl = UseProtocolDecl {
            source: UseSource::Parent,
            source_name: "logsink".into(),
            target_path: CapabilityPath::try_from("/fuchsia.logger.LogSink").unwrap(),
            dependency_type: DependencyType::Strong,
            availability: Availability::Required,
        };

        let (dir_client, dir_server) = endpoints::create_endpoints::<fio::DirectoryMarker>();
        let mut root_dir = ServiceFs::new_local();
        root_dir.add_fidl_service_at(LogSinkMarker::PROTOCOL_NAME, MockServiceRequest::LogSink);
        let _sub_dir = root_dir.dir("subdir");
        root_dir.serve_connection(dir_server).expect("failed to add serving channel");

        let ns_entries = vec![fcrunner::ComponentNamespaceEntry {
            path: Some("/".to_string()),
            directory: Some(dir_client),
            ..fcrunner::ComponentNamespaceEntry::EMPTY
        }];

        verify_logger_connects_in_namespace(Some(&mut root_dir), ns_entries, log_decl, true).await;
    }

    #[fuchsia::test]
    /// Tests that the logger is connected to when it is in a subdirectory of a
    /// namespace entry.
    async fn test_logger_at_subdir_of_entry() {
        let log_decl = UseProtocolDecl {
            source: UseSource::Parent,
            source_name: "logsink".into(),
            target_path: CapabilityPath::try_from("/arbitrary-dir/fuchsia.logger.LogSink").unwrap(),
            dependency_type: DependencyType::Strong,
            availability: Availability::Required,
        };

        let (dir_client, dir_server) = endpoints::create_endpoints::<fio::DirectoryMarker>();
        let mut root_dir = ServiceFs::new_local();
        let mut svc_dir = root_dir.dir("arbitrary-dir");
        svc_dir.add_fidl_service_at(LogSinkMarker::PROTOCOL_NAME, MockServiceRequest::LogSink);
        let _sub_dir = root_dir.dir("subdir");
        root_dir.serve_connection(dir_server).expect("failed to add serving channel");

        let ns_entries = vec![fcrunner::ComponentNamespaceEntry {
            path: Some("/".to_string()),
            directory: Some(dir_client),
            ..fcrunner::ComponentNamespaceEntry::EMPTY
        }];

        verify_logger_connects_in_namespace(Some(&mut root_dir), ns_entries, log_decl, true).await;
    }

    #[fuchsia::test]
    async fn test_multiple_namespace_entries() {
        let log_decl = UseProtocolDecl {
            source: UseSource::Parent,
            source_name: "logsink".into(),
            target_path: CapabilityPath::try_from("/svc/fuchsia.logger.LogSink").unwrap(),
            dependency_type: DependencyType::Strong,
            availability: Availability::Required,
        };

        let (dir_client, dir_server) = endpoints::create_endpoints::<fio::DirectoryMarker>();
        let mut root_dir = ServiceFs::new_local();
        root_dir.add_fidl_service_at(LogSinkMarker::PROTOCOL_NAME, MockServiceRequest::LogSink);
        let _sub_dir = root_dir.dir("subdir");
        root_dir.serve_connection(dir_server).expect("failed to add serving channel");

        // Create a directory for another namespace entry which we don't
        // actually expect to be accessed.
        let (extra_dir_client, extra_dir_server) =
            endpoints::create_endpoints::<fio::DirectoryMarker>();
        let mut extra_dir = ServiceFs::new_local();
        extra_dir.add_fidl_service(MockServiceRequest::LogSink);
        extra_dir.serve_connection(extra_dir_server).expect("serving channel failed");

        let ns_entries = vec![
            fcrunner::ComponentNamespaceEntry {
                path: Some("/svc".to_string()),
                directory: Some(dir_client),
                ..fcrunner::ComponentNamespaceEntry::EMPTY
            },
            fcrunner::ComponentNamespaceEntry {
                path: Some("/sv".to_string()),
                directory: Some(extra_dir_client),
                ..fcrunner::ComponentNamespaceEntry::EMPTY
            },
        ];

        verify_logger_connects_in_namespace(Some(&mut root_dir), ns_entries, log_decl, true).await;
    }

    #[fuchsia::test]
    async fn test_no_connect_on_empty_namespace() {
        let log_decl = UseProtocolDecl {
            source: UseSource::Parent,
            source_name: "logsink".into(),
            target_path: CapabilityPath::try_from("/svc/fuchsia.logger.LogSink").unwrap(),
            dependency_type: DependencyType::Strong,
            availability: Availability::Required,
        };

        let ns_entries = vec![];

        verify_logger_connects_in_namespace(
            Option::<
                &mut ServiceFs<fuchsia_component::server::ServiceObjLocal<'_, MockServiceRequest>>,
            >::None,
            ns_entries,
            log_decl,
            false,
        )
        .await;
    }

    #[fuchsia::test]
    async fn test_logsink_dir_not_in_namespace() {
        let log_decl = UseProtocolDecl {
            source: UseSource::Parent,
            source_name: "logsink".into(),
            target_path: CapabilityPath::try_from("/svc/fuchsia.logger.LogSink").unwrap(),
            dependency_type: DependencyType::Strong,
            availability: Availability::Required,
        };

        let (dir_client, dir_server) = endpoints::create_endpoints::<fio::DirectoryMarker>();
        let mut root_dir = ServiceFs::new_local();
        root_dir.add_fidl_service_at(LogSinkMarker::PROTOCOL_NAME, MockServiceRequest::LogSink);
        root_dir.serve_connection(dir_server).expect("failed to add serving channel");

        let ns_entries = vec![fcrunner::ComponentNamespaceEntry {
            path: Some("/not-the-svc-dir".to_string()),
            directory: Some(dir_client),
            ..fcrunner::ComponentNamespaceEntry::EMPTY
        }];

        verify_logger_connects_in_namespace(Some(&mut root_dir), ns_entries, log_decl, false).await;
    }

    /// Verify the expected logger connection behavior and that the logger is
    /// set or not in the namespace.
    async fn verify_logger_connects_in_namespace<
        T: fuchsia_component::server::ServiceObjTrait<Output = MockServiceRequest>,
    >(
        root_dir: Option<&mut ServiceFs<T>>,
        ns_entries: Vec<fcrunner::ComponentNamespaceEntry>,
        proto_decl: UseProtocolDecl,
        connects: bool,
    ) {
        let connection_count = if connects { 1u8 } else { 0u8 };
        // Create a task that will access the namespace by calling
        // `get_logger_from_ns`. This task won't complete until the VFS backing
        // the namespace starts responding to requests. This VFS is served by
        // code in the next stanza.
        fuchsia_async::Task::spawn(async move {
            let ns_size = ns_entries.len();
            let (procesed_ns, logger) = get_logger_from_ns(ns_entries, &proto_decl).await;
            assert_eq!(logger.is_some(), connects);
            assert_eq!(ns_size, procesed_ns.len())
        })
        .detach();

        if let Some(dir) = root_dir {
            // Serve the directory and when the LogSink service is requested
            // provide a closure that counts number of calls to the ConnectStructured and
            // WaitForInterestChange methods. Serving stops when the spawned task drops the
            // IncomingNamespace, which holds the other side of the VFS directory handle.
            let connect_count = Arc::new(AtomicU8::new(0));
            dir.for_each_concurrent(10usize, |request: MockServiceRequest| match request {
                MockServiceRequest::LogSink(mut r) => {
                    let connect_count2 = connect_count.clone();
                    async move {
                        while let Some(Ok(req)) = r.next().await {
                            match req {
                                LogSinkRequest::Connect { .. } => {
                                    panic!("Unexpected call to `Connect`");
                                }
                                LogSinkRequest::ConnectStructured { .. } => {
                                    connect_count2.fetch_add(1, Ordering::SeqCst);
                                }
                                LogSinkRequest::WaitForInterestChange { .. } => {
                                    // ideally we'd also assert this was received but it's racy
                                    // since the request is sent by the above detached task
                                }
                            }
                        }
                    }
                }
            })
            .await;
            assert_eq!(connect_count.load(Ordering::SeqCst), connection_count);
        }
    }
}
