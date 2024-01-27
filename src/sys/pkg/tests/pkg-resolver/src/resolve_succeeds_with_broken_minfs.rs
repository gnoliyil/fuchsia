// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// This module tests pkg-resolver's resolve keeps working when
/// MinFs is broken.
use {
    fidl::endpoints::{Proxy, RequestStream, ServerEnd},
    fidl_fuchsia_io as fio,
    fidl_fuchsia_pkg_ext::RepositoryConfig,
    fidl_fuchsia_pkg_rewrite_ext::Rule,
    fuchsia_async as fasync,
    fuchsia_pkg_testing::{serve::ServedRepository, Package, PackageBuilder, RepositoryBuilder},
    fuchsia_zircon::Status,
    futures::future::BoxFuture,
    futures::prelude::*,
    lib::{
        get_repos, get_rules, mock_filesystem, DirOrProxy, EnableDynamicConfig, MountsBuilder,
        TestEnv, TestEnvBuilder, EMPTY_REPO_PATH,
    },
    std::sync::{
        atomic::{AtomicBool, AtomicU64},
        Arc,
    },
};

trait OpenRequestHandler: Sized {
    fn handle_open_request(
        &self,
        flags: fio::OpenFlags,
        path: String,
        object: ServerEnd<fio::NodeMarker>,
        control_handle: fio::DirectoryControlHandle,
        parent: Arc<DirectoryStreamHandler<Self>>,
    );
}

struct DirectoryStreamHandler<O: Sized> {
    open_handler: Arc<O>,
}

impl<O> DirectoryStreamHandler<O>
where
    O: OpenRequestHandler + Send + Sync + 'static,
{
    fn new(open_handler: Arc<O>) -> Self {
        Self { open_handler }
    }

    fn handle_stream(
        self: Arc<Self>,
        mut stream: fio::DirectoryRequestStream,
    ) -> BoxFuture<'static, ()> {
        async move {
            while let Some(req) = stream.next().await {
                match req.unwrap() {
                    fio::DirectoryRequest::Clone { flags, object, control_handle: _ } => {
                        let stream = object.into_stream().unwrap().cast_stream();
                        mock_filesystem::describe_dir(flags, &stream);
                        fasync::Task::spawn(Arc::clone(&self).handle_stream(stream)).detach();
                    }
                    fio::DirectoryRequest::Open {
                        flags,
                        mode: _,
                        path,
                        object,
                        control_handle,
                    } => self.open_handler.handle_open_request(
                        flags,
                        path,
                        object,
                        control_handle,
                        Arc::clone(&self),
                    ),
                    fio::DirectoryRequest::Close { .. } => (),
                    req => panic!("DirectoryStreamHandler unhandled request {:?}", req),
                }
            }
        }
        .boxed()
    }
}

struct OpenFailOrTempFs {
    should_fail: AtomicBool,
    fail_count: AtomicU64,
    tempdir: tempfile::TempDir,
}

impl OpenFailOrTempFs {
    fn new_failing() -> Arc<Self> {
        Arc::new(Self {
            should_fail: AtomicBool::new(true),
            fail_count: AtomicU64::new(0),
            tempdir: tempfile::tempdir().expect("/tmp to exist"),
        })
    }

    fn get_open_fail_count(&self) -> u64 {
        self.fail_count.load(std::sync::atomic::Ordering::SeqCst)
    }

    fn make_open_succeed(&self) {
        self.should_fail.store(false, std::sync::atomic::Ordering::SeqCst);
    }

    fn should_fail(&self) -> bool {
        self.should_fail.load(std::sync::atomic::Ordering::SeqCst)
    }
}

impl OpenRequestHandler for OpenFailOrTempFs {
    fn handle_open_request(
        &self,
        flags: fio::OpenFlags,
        path: String,
        object: ServerEnd<fio::NodeMarker>,
        _control_handle: fio::DirectoryControlHandle,
        parent: Arc<DirectoryStreamHandler<Self>>,
    ) {
        if self.should_fail() {
            if path == "." {
                let stream = object.into_stream().unwrap().cast_stream();
                mock_filesystem::describe_dir(flags, &stream);
                fasync::Task::spawn(parent.handle_stream(stream)).detach();
            } else {
                self.fail_count.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            }
        } else {
            let (tempdir_proxy, server_end) =
                fidl::endpoints::create_proxy::<fio::DirectoryMarker>().unwrap();
            fdio::open(
                self.tempdir.path().to_str().unwrap(),
                fio::OpenFlags::DIRECTORY
                    | fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::RIGHT_WRITABLE,
                server_end.into_channel(),
            )
            .unwrap();
            tempdir_proxy.open(flags, fio::ModeType::empty(), &path, object).unwrap();
        }
    }
}

/// Implements OpenRequestHandler, proxying to a backing temp file and optionally failing writes
/// to certain files.
struct WriteFailOrTempFs {
    files_to_fail_writes: Vec<String>,
    should_fail: Arc<AtomicBool>,
    fail_count: Arc<AtomicU64>,
    tempdir_proxy: fio::DirectoryProxy,

    // We don't read this, but need to keep it around otherwise the temp directory is torn down
    _tempdir: tempfile::TempDir,
}

impl WriteFailOrTempFs {
    fn new_failing(files_to_fail_writes: Vec<String>) -> Arc<Self> {
        let tempdir = tempfile::tempdir().expect("/tmp to exist");

        let (tempdir_proxy, server_end) =
            fidl::endpoints::create_proxy::<fio::DirectoryMarker>().unwrap();

        fdio::open(
            tempdir.path().to_str().unwrap(),
            fio::OpenFlags::DIRECTORY
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE,
            server_end.into_channel(),
        )
        .expect("open temp directory");
        Arc::new(Self {
            files_to_fail_writes,
            should_fail: Arc::new(AtomicBool::new(true)),
            fail_count: Arc::new(AtomicU64::new(0)),
            _tempdir: tempdir,
            tempdir_proxy,
        })
    }

    fn get_write_fail_count(&self) -> u64 {
        self.fail_count.load(std::sync::atomic::Ordering::SeqCst)
    }

    fn make_write_succeed(&self) {
        self.should_fail.store(false, std::sync::atomic::Ordering::SeqCst);
    }

    fn should_fail(&self) -> bool {
        self.should_fail.load(std::sync::atomic::Ordering::SeqCst)
    }
}

impl OpenRequestHandler for WriteFailOrTempFs {
    fn handle_open_request(
        &self,
        flags: fio::OpenFlags,
        path: String,
        object: ServerEnd<fio::NodeMarker>,
        _control_handle: fio::DirectoryControlHandle,
        parent: Arc<DirectoryStreamHandler<Self>>,
    ) {
        if path == "." && self.should_fail() {
            let stream = object.into_stream().unwrap().cast_stream();
            mock_filesystem::describe_dir(flags, &stream);
            fasync::Task::spawn(parent.handle_stream(stream)).detach();
            return;
        }

        if !self.files_to_fail_writes.contains(&path) {
            // We don't want to intercept file operations, so just open the file normally.
            self.tempdir_proxy.open(flags, fio::ModeType::empty(), &path, object).unwrap();
            return;
        }

        // This file matched our configured set of paths to intercept operations for, so open a
        // backing file and send all file operations which the client thinks it's sending
        // to the backing file instead to our FailingWriteFileStreamHandler.

        let (file_requests, file_control_handle) =
            ServerEnd::<fio::FileMarker>::new(object.into_channel())
                .into_stream_and_control_handle()
                .expect("split file server end");

        // Create a proxy to the actual file we'll open to proxy to.
        let (backing_node_proxy, backing_node_server_end) =
            fidl::endpoints::create_proxy::<fio::NodeMarker>().unwrap();

        self.tempdir_proxy
            .open(flags, fio::ModeType::empty(), &path, backing_node_server_end)
            .expect("open file requested by pkg-resolver");

        // All the things pkg-resolver attempts to open in these tests are files,
        // not directories, so cast the NodeProxy to a FileProxy. If the pkg-resolver assumption
        // changes, this code will have to support both.
        let backing_file_proxy = fio::FileProxy::new(backing_node_proxy.into_channel().unwrap());
        let send_onopen = flags.intersects(fio::OpenFlags::DESCRIBE);

        let file_handler = Arc::new(FailingWriteFileStreamHandler::new(
            backing_file_proxy,
            String::from(path),
            Arc::clone(&self.should_fail),
            Arc::clone(&self.fail_count),
        ));
        fasync::Task::spawn(file_handler.handle_stream(
            file_requests,
            file_control_handle,
            send_onopen,
        ))
        .detach();
    }
}

/// Handles a stream of requests for a particular file, proxying to a backing file for all
/// operations except writes, which it may decide to make fail.
struct FailingWriteFileStreamHandler {
    backing_file: fio::FileProxy,
    writes_should_fail: Arc<AtomicBool>,
    write_fail_count: Arc<AtomicU64>,
    path: String,
}

impl FailingWriteFileStreamHandler {
    fn new(
        backing_file: fio::FileProxy,
        path: String,
        writes_should_fail: Arc<AtomicBool>,
        write_fail_count: Arc<AtomicU64>,
    ) -> Self {
        Self { backing_file, writes_should_fail, write_fail_count, path }
    }

    fn writes_should_fail(self: &Arc<Self>) -> bool {
        self.writes_should_fail.load(std::sync::atomic::Ordering::SeqCst)
    }

    async fn handle_write(self: &Arc<Self>, data: Vec<u8>, responder: fio::FileWriteResponder) {
        if self.writes_should_fail() {
            self.write_fail_count.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            responder.send(Err(Status::NO_MEMORY.into_raw())).expect("send on write");
            return;
        }

        // Don't fail, actually do the write.
        let result = self.backing_file.write(&data).await.unwrap();
        responder.send(result).unwrap();
    }

    fn handle_stream(
        self: Arc<Self>,
        mut stream: fio::FileRequestStream,
        control_handle: fio::FileControlHandle,
        send_onopen: bool,
    ) -> BoxFuture<'static, ()> {
        async move {
            if send_onopen {
                // The client end of the file is waiting for an OnOpen event, so send
                // one based on the actual OnOpen from the backing file.
                let mut event_stream = self.backing_file.take_event_stream();
                let event = event_stream.try_next().await.unwrap();
                match event.expect("failed to received file event") {
                    fio::FileEvent::OnOpen_ { s, info } => {
                        // info comes as an Option<Box<NodeInfoDeprecated>>, but we need to return an
                        // Option<NodeInfoDeprecated>. Transform it.
                        let node_info = info.map(|b| *b);
                        control_handle
                            .send_on_open_(s, node_info)
                            .expect("send on open to fake file");
                    }
                    fio::FileEvent::OnRepresentation { payload } => {
                        control_handle
                            .send_on_representation(payload)
                            .expect("send on open to fake file");
                    }
                }
            }

            while let Some(req) = stream.next().await {
                match req.unwrap() {
                    fio::FileRequest::Write { data, responder } => {
                        self.handle_write(data, responder).await
                    }
                    fio::FileRequest::GetAttr { responder } => {
                        let (status, attrs) = self.backing_file.get_attr().await.unwrap();
                        responder.send(status, &attrs).unwrap();
                    }
                    fio::FileRequest::Read { count, responder } => {
                        let result = self.backing_file.read(count).await.unwrap();
                        responder.send(result.as_deref().map_err(|e| *e)).unwrap();
                    }
                    fio::FileRequest::Close { responder } => {
                        let backing_file_close_response = self.backing_file.close().await.unwrap();
                        responder.send(backing_file_close_response).unwrap();
                    }
                    other => {
                        panic!("unhandled request type for path {:?}: {:?}", self.path, other);
                    }
                }
            }
        }
        .boxed()
    }
}

/// Optionally fails renames of certain files. Otherwise, delegates
/// DirectoryRequests to a backing tempdir.
struct RenameFailOrTempFs {
    fail_count: Arc<AtomicU64>,
    files_to_fail_renames: Vec<String>,
    should_fail: Arc<AtomicBool>,
    tempdir: Arc<tempfile::TempDir>,
}

impl RenameFailOrTempFs {
    fn new_failing(files_to_fail_renames: Vec<String>) -> Arc<Self> {
        Arc::new(Self {
            fail_count: Arc::new(AtomicU64::new(0)),
            files_to_fail_renames,
            should_fail: Arc::new(AtomicBool::new(true)),
            tempdir: Arc::new(tempfile::tempdir().expect("/tmp to exist")),
        })
    }

    fn get_rename_fail_count(&self) -> u64 {
        self.fail_count.load(std::sync::atomic::Ordering::SeqCst)
    }

    fn make_rename_succeed(&self) {
        self.should_fail.store(false, std::sync::atomic::Ordering::SeqCst);
    }

    fn should_fail(&self) -> bool {
        self.should_fail.load(std::sync::atomic::Ordering::SeqCst)
    }
}

impl OpenRequestHandler for RenameFailOrTempFs {
    fn handle_open_request(
        &self,
        flags: fio::OpenFlags,
        path: String,
        object: ServerEnd<fio::NodeMarker>,
        _control_handle: fio::DirectoryControlHandle,
        parent: Arc<DirectoryStreamHandler<Self>>,
    ) {
        // Set up proxy to tmpdir and delegate to it on success.
        let (tempdir_proxy, server_end) =
            fidl::endpoints::create_proxy::<fio::DirectoryMarker>().unwrap();
        fdio::open(
            self.tempdir.path().to_str().unwrap(),
            fio::OpenFlags::DIRECTORY
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE,
            server_end.into_channel(),
        )
        .unwrap();
        if !self.should_fail() || path != "." {
            tempdir_proxy.open(flags, fio::ModeType::empty(), &path, object).unwrap();
            return;
        }

        // Prepare to handle the directory requests. We must call describe_dir, which sends an
        // OnOpen if OPEN_FLAG_DESCRIBE is set. Otherwise, the code will hang when reading from
        // the stream.
        let mut stream = object.into_stream().unwrap().cast_stream();
        mock_filesystem::describe_dir(flags, &stream);
        let fail_count = Arc::clone(&self.fail_count);
        let files_to_fail_renames = Clone::clone(&self.files_to_fail_renames);

        // Handle the directory requests.
        fasync::Task::spawn(async move {
            while let Some(req) = stream.next().await {
                match req.unwrap() {
                    fio::DirectoryRequest::GetAttr { responder } => {
                        let (status, attrs) = tempdir_proxy.get_attr().await.unwrap();
                        responder.send(status, &attrs).unwrap();
                    }
                    fio::DirectoryRequest::Close { responder } => {
                        let result = tempdir_proxy.close().await.unwrap();
                        responder.send(result).unwrap();
                    }
                    fio::DirectoryRequest::GetToken { responder } => {
                        let (status, handle) = tempdir_proxy.get_token().await.unwrap();
                        responder.send(status, handle).unwrap();
                    }
                    fio::DirectoryRequest::Rename { src, dst, responder, .. } => {
                        if !files_to_fail_renames.contains(&src) {
                            panic!("unsupported rename from {} to {}", src, dst);
                        }
                        fail_count.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                        responder.send(Err(Status::NOT_FOUND.into_raw())).unwrap();
                    }
                    fio::DirectoryRequest::Open {
                        flags,
                        mode: _,
                        path,
                        object,
                        control_handle,
                    } => {
                        parent.open_handler.handle_open_request(
                            flags,
                            path,
                            object,
                            control_handle,
                            Arc::clone(&parent.clone()),
                        );
                    }
                    other => {
                        panic!("unhandled request type for path {:?}: {:?}", path, other);
                    }
                }
            }
        })
        .detach();
    }
}

async fn create_testenv_serves_repo<H: OpenRequestHandler + Send + Sync + 'static>(
    open_handler: Arc<H>,
) -> (TestEnv, RepositoryConfig, Package, ServedRepository) {
    // Create testenv with failing isolated-persistent-storage
    let directory_handler = Arc::new(DirectoryStreamHandler::new(open_handler));
    let (proxy, stream) =
        fidl::endpoints::create_proxy_and_stream::<fio::DirectoryMarker>().unwrap();
    fasync::Task::spawn(directory_handler.handle_stream(stream)).detach();
    let env = TestEnvBuilder::new()
        .mounts(
            MountsBuilder::new()
                .enable_dynamic_config(EnableDynamicConfig { enable_dynamic_configuration: true })
                .pkg_resolver_data(DirOrProxy::Proxy(proxy))
                .build(),
        )
        .build()
        .await;

    // Serve repo with package
    let pkg = PackageBuilder::new("just_meta_far").build().await.expect("created pkg");
    let repo = Arc::new(
        RepositoryBuilder::from_template_dir(EMPTY_REPO_PATH)
            .add_package(&pkg)
            .build()
            .await
            .unwrap(),
    );
    let served_repository = repo.server().start().unwrap();
    let repo_url = "fuchsia-pkg://example.com".parse().unwrap();
    let config = served_repository.make_repo_config(repo_url);

    (env, config, pkg, served_repository)
}

async fn verify_pkg_resolution_succeeds_during_minfs_repo_config_failure<
    O,
    FailCountFn,
    MakeSucceedFn,
>(
    open_handler: Arc<O>,
    fail_count_fn: FailCountFn,
    num_failures_before_first_restart: u64,
    num_failures_after_first_restart: u64,
    make_succeed_fn: MakeSucceedFn,
) where
    O: OpenRequestHandler + Send + Sync + 'static,
    FailCountFn: FnOnce() -> u64 + Copy,
    MakeSucceedFn: FnOnce(),
{
    let (mut env, config, pkg, _served_repo) =
        create_testenv_serves_repo(Arc::clone(&open_handler)).await;

    // Verify we can resolve the package with a broken MinFs, and that repo configs do not persist
    let () = env.proxies.repo_manager.add(&config.clone().into()).await.unwrap().unwrap();
    let (package_dir, _resolved_context) =
        env.resolve_package("fuchsia-pkg://example.com/just_meta_far").await.unwrap();
    pkg.verify_contents(&package_dir).await.unwrap();
    assert_eq!(fail_count_fn(), num_failures_before_first_restart);
    env.restart_pkg_resolver().await;
    assert_eq!(get_repos(&env.proxies.repo_manager).await, vec![]);
    assert_eq!(fail_count_fn(), num_failures_after_first_restart);

    // Now let MinFs recover and show how repo configs are saved on restart.
    // Note we know we are not executing the failure path anymore since
    // the failure count doesn't change.
    make_succeed_fn();
    let () = env.proxies.repo_manager.add(&config.clone().into()).await.unwrap().unwrap();
    let (package_dir, _resolved_context) =
        env.resolve_package("fuchsia-pkg://example.com/just_meta_far").await.unwrap();
    pkg.verify_contents(&package_dir).await.unwrap();
    assert_eq!(fail_count_fn(), num_failures_after_first_restart);
    env.restart_pkg_resolver().await;
    assert_eq!(get_repos(&env.proxies.repo_manager).await, vec![config.clone()]);

    env.stop().await;
}

async fn verify_pkg_resolution_succeeds_during_minfs_repo_config_and_rewrite_rule_failure<
    O,
    FailCountFn,
    MakeSucceedFn,
>(
    open_handler: Arc<O>,
    fail_count_fn: FailCountFn,
    num_failures_before_first_restart: u64,
    num_failures_after_first_restart: u64,
    make_succeed_fn: MakeSucceedFn,
) where
    O: OpenRequestHandler + Send + Sync + 'static,
    FailCountFn: FnOnce() -> u64 + Copy,
    MakeSucceedFn: FnOnce(),
{
    let (mut env, config, pkg, _served_repo) =
        create_testenv_serves_repo(Arc::clone(&open_handler)).await;

    // Add repo config and rewrite rules
    let () = env.proxies.repo_manager.add(&config.clone().into()).await.unwrap().unwrap();
    let (edit_transaction, edit_transaction_server) = fidl::endpoints::create_proxy().unwrap();
    env.proxies.rewrite_engine.start_edit_transaction(edit_transaction_server).unwrap();
    let rule = Rule::new("should-be-rewritten", "example.com", "/", "/").unwrap();
    let () = edit_transaction.add(&rule.clone().into()).await.unwrap().unwrap();
    let () = edit_transaction.commit().await.unwrap().unwrap();

    // Verify we can resolve the package with a broken MinFs, and that rewrite rules do not
    // persist
    let (package_dir, _resolved_context) =
        env.resolve_package("fuchsia-pkg://should-be-rewritten/just_meta_far").await.unwrap();
    pkg.verify_contents(&package_dir).await.unwrap();
    assert_eq!(fail_count_fn(), num_failures_before_first_restart);
    env.restart_pkg_resolver().await;
    assert_eq!(get_rules(&env.proxies.rewrite_engine).await, vec![]);
    assert_eq!(fail_count_fn(), num_failures_after_first_restart);

    // Now let MinFs recover and show how rewrite rules are saved on restart
    // Note we know we are not executing the failure path anymore since
    // the failure count doesn't change.
    make_succeed_fn();
    let () = env.proxies.repo_manager.add(&config.clone().into()).await.unwrap().unwrap();
    let (edit_transaction, edit_transaction_server) = fidl::endpoints::create_proxy().unwrap();
    env.proxies.rewrite_engine.start_edit_transaction(edit_transaction_server).unwrap();
    let () = edit_transaction.add(&rule.clone().into()).await.unwrap().unwrap();
    let () = edit_transaction.commit().await.unwrap().unwrap();
    let (package_dir, _resolved_context) =
        env.resolve_package("fuchsia-pkg://should-be-rewritten/just_meta_far").await.unwrap();
    pkg.verify_contents(&package_dir).await.unwrap();
    assert_eq!(fail_count_fn(), num_failures_after_first_restart);
    env.restart_pkg_resolver().await;
    assert_eq!(get_rules(&env.proxies.rewrite_engine).await, vec![rule.clone()]);

    env.stop().await;
}

// Test that when pkg-resolver can't open the file for dynamic repo configs, the resolver
// still works.
#[fuchsia::test]
async fn minfs_fails_create_repo_configs() {
    let open_handler = OpenFailOrTempFs::new_failing();

    verify_pkg_resolution_succeeds_during_minfs_repo_config_failure(
        Arc::clone(&open_handler),
        || open_handler.get_open_fail_count(),
        // Before the first pkg-resolver restart, we fail 4 times:
        // * when trying to open repositories.json on start
        // * when trying to open rewrites.json on start
        // * when trying to open eager_packages.pf on start
        // * when trying to open repositories.json when adding a dynamic repo config
        4,
        // We fail an additional 3 times after the restart to account for following files
        // failing to open again on startup:
        // * repositories.json
        // * rewrites.json
        // * eager_packages.pf
        7,
        || open_handler.make_open_succeed(),
    )
    .await;
}

// Test that when pkg-resolver can open neither the file for rewrite rules
// NOR the file for dynamic repositories, the resolver still works.
#[fuchsia::test]
async fn minfs_fails_create_rewrite_rules() {
    let open_handler = OpenFailOrTempFs::new_failing();

    verify_pkg_resolution_succeeds_during_minfs_repo_config_and_rewrite_rule_failure(
        Arc::clone(&open_handler),
        || open_handler.get_open_fail_count(),
        // Before the first pkg-resolver restart, we fail 5 times:
        // * when trying to open repositories.json on start
        // * when trying to open rewrites.json on start
        // * when trying to open eager_packages.pf on start
        // * when trying to open repositories.json when adding a dynamic repo config
        // * when trying to open rewrites.json when adding a dynamic rewrite rule
        5,
        // We fail an additional 3 times after the restart to account for following files
        // failing to open again on startup:
        // * repositories.json
        // * rewrites.json
        // * eager_packages.pf
        8,
        || open_handler.make_open_succeed(),
    )
    .await;
}

// Test that when pkg-resolver can't write to the file for dynamic repo configs,
// package resolution still works.
#[fuchsia::test]
async fn minfs_fails_write_to_repo_configs() {
    let open_handler = WriteFailOrTempFs::new_failing(vec![String::from("repositories.json.new")]);

    verify_pkg_resolution_succeeds_during_minfs_repo_config_failure(
        Arc::clone(&open_handler),
        || open_handler.get_write_fail_count(),
        // The only time the test should hit the write failure path is when we add a repo config
        // when should_fail = true, in which case we fail at writing repositories.json.new.
        1,
        1,
        || open_handler.make_write_succeed(),
    )
    .await;
}

// Test that when pkg-resolver can write to neither the file for dynamic repo configs
// NOR the file for rewrite rules, package resolution still works.
#[fuchsia::test]
async fn minfs_fails_write_to_repo_configs_and_rewrite_rules() {
    let open_handler = WriteFailOrTempFs::new_failing(vec![
        String::from("repositories.json.new"),
        String::from("rewrites.json.new"),
    ]);

    verify_pkg_resolution_succeeds_during_minfs_repo_config_and_rewrite_rule_failure(
        Arc::clone(&open_handler),
        || open_handler.get_write_fail_count(),
        // The only time the test should hit the write failure path is when we add a repo config
        // when should_fail = true, in which case we fail at writing both repositories.json.new and
        // rewrites.json.new.
        2,
        2,
        || open_handler.make_write_succeed(),
    )
    .await;
}

// Test that when pkg-resolver can't rename file for dynamic repo configs, package resolution,
// still works. Note this test might stop working if the pkg-resolver starts issuing Rename
// directly to /data instead of going through std::fs::rename. If that's the case, consider
// extending DirectoryStreamHandler to also have a RenameRequestHandler, and possibly use a
// std::sync::Weak to coordinate between the DirectoryStreamHandler and RenameRequestHandler.
#[fuchsia::test]
async fn minfs_fails_rename_repo_configs() {
    let open_handler = RenameFailOrTempFs::new_failing(vec![String::from("repositories.json.new")]);

    verify_pkg_resolution_succeeds_during_minfs_repo_config_failure(
        Arc::clone(&open_handler),
        || open_handler.get_rename_fail_count(),
        // The only time the test should hit the rename failure path is when we add a
        // repo config when should_fail = true, in which case we fail at renaming
        // repositories.json.new.
        1,
        1,
        || open_handler.make_rename_succeed(),
    )
    .await;
}

// Test that when pkg-resolver can rename neither the file for dynamic repo configs
// NOR the file for rewrite rules, package resolution still works.
#[fuchsia::test]
async fn minfs_fails_rename_repo_configs_and_rewrite_rules() {
    let open_handler = RenameFailOrTempFs::new_failing(vec![
        String::from("repositories.json.new"),
        String::from("rewrites.json.new"),
    ]);

    verify_pkg_resolution_succeeds_during_minfs_repo_config_and_rewrite_rule_failure(
        Arc::clone(&open_handler),
        || open_handler.get_rename_fail_count(),
        // The only time the test should hit the rename failure path is when we add a
        // repo config when should_fail = true, in which case we fail at renaming both
        // repositories.json.new and rewrites.json.new.
        2,
        2,
        || open_handler.make_rename_succeed(),
    )
    .await;
}
