// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    async_trait::async_trait,
    fidl_fuchsia_process as fproc, fidl_fuchsia_test as ftest,
    ftest::{Invocation, RunListenerProxy},
    fuchsia_async as fasync, fuchsia_runtime as runtime,
    fuchsia_zircon::{self as zx, HandleBased},
    futures::{
        future::{abortable, join3, AbortHandle, FutureExt as _},
        lock::Mutex,
        prelude::*,
    },
    lazy_static::lazy_static,
    regex::Regex,
    std::{
        collections::HashSet,
        sync::{Arc, Weak},
    },
    test_runners_lib::{
        cases::TestCaseInfo,
        elf::{
            Component, ComponentError, EnumeratedTestCases, FidlError, KernelError,
            MemoizedFutureContainer, PinnedFuture, SuiteServer,
        },
        errors::*,
        launch,
        logs::{LogStreamReader, LoggerStream, SocketLogWriter},
    },
    tracing::{debug, error},
    zx::Task,
};

lazy_static! {
    static ref VDSO_VMO: zx::Handle = {
        runtime::take_startup_handle(runtime::HandleInfo::new(runtime::HandleType::VdsoVmo, 0))
            .expect("failed to take vDSO handle")
    };
}

type EnumeratedTestNames = Arc<HashSet<String>>;

/// Implements `fuchsia.test.Suite` and runs provided test.
pub struct TestServer {
    /// Cache to store enumerated tests.
    tests_future_container: MemoizedFutureContainer<EnumeratedTestCases, EnumerationError>,
    /// Index of disabled tests for faster membership checking.
    disabled_tests_future_container: MemoizedFutureContainer<EnumeratedTestNames, EnumerationError>,
}

/// Default concurrency for running test cases in parallel.
static PARALLEL_DEFAULT: u16 = 10;

#[async_trait]
impl SuiteServer for TestServer {
    /// Launches a process that lists the tests without actually running any of them. It then parses
    /// the output of that process into a vector of strings.
    ///
    /// Example output for rust test process:
    ///
    /// ```text
    /// tests::purposefully_failing_test: test
    /// tests::test_full_path: test
    /// tests::test_minimal_path: test
    ///
    /// 3 tests, 0 benchmarks
    /// ```
    ///
    /// The list of tests is cached.
    async fn enumerate_tests(
        &self,
        test_component: Arc<Component>,
    ) -> Result<EnumeratedTestCases, EnumerationError> {
        self.tests(test_component).await
    }

    async fn run_tests(
        &self,
        invocations: Vec<Invocation>,
        run_options: ftest::RunOptions,
        test_component: Arc<Component>,
        run_listener: &RunListenerProxy,
    ) -> Result<(), RunTestError> {
        let num_parallel =
            Self::get_parallel_count(run_options.parallel.unwrap_or(PARALLEL_DEFAULT));
        let invocations = stream::iter(invocations);
        invocations
            .map(Ok)
            .try_for_each_concurrent(num_parallel, |invocation| async {
                let test = invocation.name.as_ref().ok_or(RunTestError::TestCaseName)?.to_string();
                debug!("Running test {}", test);

                let (test_stdout, stdout_client) = zx::Socket::create_stream();
                let (test_stderr, stderr_client) = zx::Socket::create_stream();
                let (case_listener_proxy, listener) =
                    fidl::endpoints::create_proxy::<fidl_fuchsia_test::CaseListenerMarker>()
                        .map_err(FidlError::CreateProxy)
                        .unwrap();
                let test_stdout = fasync::Socket::from_socket(test_stdout)
                    .map_err(KernelError::SocketToAsync)
                    .unwrap();
                let test_stderr = fasync::Socket::from_socket(test_stderr)
                    .map_err(KernelError::SocketToAsync)
                    .unwrap();

                run_listener
                    .on_test_case_started(
                        invocation,
                        ftest::StdHandles {
                            out: Some(stdout_client),
                            err: Some(stderr_client),
                            ..ftest::StdHandles::EMPTY
                        },
                        listener,
                    )
                    .map_err(RunTestError::SendStart)?;

                let mut test_stdout = SocketLogWriter::new(test_stdout);
                let mut test_stderr = SocketLogWriter::new(test_stderr);

                match self
                    .run_test(
                        &test,
                        &run_options,
                        test_component.clone(),
                        &mut test_stdout,
                        &mut test_stderr,
                    )
                    .await
                {
                    Ok(result) => {
                        case_listener_proxy.finished(result).map_err(RunTestError::SendFinish)?;
                    }
                    Err(error) => {
                        error!("failed to run test '{}'. {}", test, error);
                        case_listener_proxy
                            .finished(ftest::Result_ {
                                status: Some(ftest::Status::Failed),
                                ..ftest::Result_::EMPTY
                            })
                            .map_err(RunTestError::SendFinish)?;
                    }
                }
                return Ok::<(), RunTestError>(());
            })
            .await
    }

    /// Run this server.
    fn run(
        self,
        weak_test_component: Weak<Component>,
        test_url: &str,
        request_stream: fidl_fuchsia_test::SuiteRequestStream,
    ) -> AbortHandle {
        let test_url = test_url.to_owned();
        let (fut, test_suite_abortable_handle) =
            abortable(self.serve_test_suite(request_stream, weak_test_component.clone()));

        fasync::Task::local(async move {
            match fut.await {
                Ok(result) => {
                    if let Err(e) = result {
                        error!("server failed for test {}: {:?}", test_url, e);
                    }
                }
                Err(e) => error!("server aborted for test {}: {:?}", test_url, e),
            }
            debug!("Done running server for {}.", test_url);
        })
        .detach();
        test_suite_abortable_handle
    }
}

lazy_static! {
    static ref RESTRICTED_FLAGS: HashSet<&'static str> =
        vec!["--nocapture", "--list"].into_iter().collect();
}

impl TestServer {
    /// Creates new test server.
    /// Clients should call this function to create new object and then call `serve_test_suite`.
    pub fn new() -> Self {
        Self {
            tests_future_container: Arc::new(Mutex::new(None)),
            disabled_tests_future_container: Arc::new(Mutex::new(None)),
        }
    }

    /// Retrieves and memoizes the full list of tests from the test binary.
    ///
    /// The entire `Future` is memoized, so repeated calls do not execute the test binary
    /// repeatedly.
    ///
    /// This outer method is _not_ `async`, to avoid capturing a reference to `&self` and fighting
    /// the borrow checker until the end of time.
    fn tests(
        &self,
        test_component: Arc<Component>,
    ) -> impl Future<Output = Result<EnumeratedTestCases, EnumerationError>> {
        /// Fetches the full list of tests from the test binary.
        ///
        /// The `disabled_tests_future` is passed in to determine which tests should be marked
        /// disabled.
        async fn fetch(
            test_component: Arc<Component>,
            disabled_tests_future: impl Future<Output = Result<EnumeratedTestNames, EnumerationError>>
                + Send
                + 'static,
        ) -> Result<EnumeratedTestCases, EnumerationError> {
            let test_names = get_tests(test_component, TestFilter::AllTests).await?;
            let disabled_tests = disabled_tests_future.await?;
            let tests: Vec<TestCaseInfo> = test_names
                .into_iter()
                .map(|name| {
                    let enabled = !disabled_tests.contains(&name);
                    TestCaseInfo { name, enabled }
                })
                .collect();
            Ok(Arc::new(tests))
        }

        /// Populates the given `tests_future_container` with a future, or returns a copy of that
        /// future if already present.
        async fn get_or_insert_tests_future(
            test_component: Arc<Component>,
            tests_future_container: MemoizedFutureContainer<EnumeratedTestCases, EnumerationError>,
            disabled_tests_future: impl Future<Output = Result<EnumeratedTestNames, EnumerationError>>
                + Send
                + 'static,
        ) -> Result<EnumeratedTestCases, EnumerationError> {
            tests_future_container
                .lock()
                .await
                .get_or_insert_with(|| {
                    // The type must be specified in order to compile.
                    let fetched: PinnedFuture<EnumeratedTestCases, EnumerationError> =
                        Box::pin(fetch(test_component, disabled_tests_future));
                    fetched.shared()
                })
                // This clones the `SharedFuture`.
                .clone()
                .await
        }

        let tests_future_container = self.tests_future_container.clone();
        let disabled_tests_future = self.disabled_tests(test_component.clone());

        get_or_insert_tests_future(test_component, tests_future_container, disabled_tests_future)
    }

    /// Retrieves and memoizes the list of just the disabled tests from the test binary.
    ///
    /// This outer method is _not_ `async`, to avoid capturing a reference to `&self` and fighting
    /// the borrow checker until the end of time.
    fn disabled_tests(
        &self,
        test_component: Arc<Component>,
    ) -> impl Future<Output = Result<EnumeratedTestNames, EnumerationError>> {
        type DisabledTestsFutureContainer =
            MemoizedFutureContainer<EnumeratedTestNames, EnumerationError>;

        /// Fetches the list of disabled tests from the test binary.
        async fn fetch(
            test_component: Arc<Component>,
        ) -> Result<EnumeratedTestNames, EnumerationError> {
            let disabled_tests = get_tests(test_component, TestFilter::DisabledTests)
                .await?
                .into_iter()
                .collect::<HashSet<String>>();
            Ok(Arc::new(disabled_tests))
        }

        /// Populates the given `disabled_tests_future_container` with a future, or returns a copy
        /// of that future if already present.
        async fn get_or_insert_disabled_tests_future(
            test_component: Arc<Component>,
            disabled_tests_future_container: DisabledTestsFutureContainer,
        ) -> Result<EnumeratedTestNames, EnumerationError> {
            disabled_tests_future_container
                .lock()
                .await
                .get_or_insert_with(|| {
                    // The type must be specified.
                    let fetched: PinnedFuture<EnumeratedTestNames, EnumerationError> =
                        Box::pin(fetch(test_component));
                    fetched.shared()
                })
                .clone()
                .await
        }

        let disabled_tests_future_container = self.disabled_tests_future_container.clone();
        get_or_insert_disabled_tests_future(test_component, disabled_tests_future_container)
    }

    /// Returns `true` if the given test is disabled (marked `#[ignore]`) by the developer.
    ///
    /// If the set of disabled tests isn't yet cached, this will retrieve it -- hence `async`.
    async fn is_test_disabled<'a>(
        &'a self,
        test_component: Arc<Component>,
        test_name: &str,
    ) -> Result<bool, EnumerationError> {
        let disabled_tests = self.disabled_tests(test_component).await?;
        Ok(disabled_tests.contains(test_name))
    }

    /// Launches a process that actually runs the test and parses the resulting JSON output.
    ///
    /// The mechanism by which Rust tests are launched in individual processes ignores whether a
    /// particular test was marked `#[ignore]`, so this method preemptively checks whether a
    /// the given test is disabled and returns early if the test should be skipped.
    async fn run_test<'a>(
        &'a self,
        test: &str,
        run_options: &ftest::RunOptions,
        test_component: Arc<Component>,
        test_stdout: &mut SocketLogWriter,
        test_stderr: &mut SocketLogWriter,
    ) -> Result<ftest::Result_, RunTestError> {
        // Exit codes used by Rust's libtest runner.
        const TR_OK: i64 = 50;
        const TR_FAILED: i64 = 51;

        // Rust test binaries launched with `__RUST_TEST_INVOKE` don't care if a test is disabled,
        // so we must manually return early in order to skip a test.
        let skip_disabled_tests = !run_options.include_disabled_tests.unwrap_or(false);
        if skip_disabled_tests && self.is_test_disabled(test_component.clone(), test).await? {
            return Ok(ftest::Result_ {
                status: Some(ftest::Status::Skipped),
                ..ftest::Result_::EMPTY
            });
        }

        let test_invoke = Some(format!("__RUST_TEST_INVOKE={}", test));

        let mut args = vec![
            // Disable stdout capture in the Rust test harness
            // so we can capture it ourselves
            "--nocapture".to_owned(),
        ];
        args.extend(test_component.args.clone());
        if let Some(user_args) = &run_options.arguments {
            if let Err(e) = Self::validate_args(&user_args) {
                test_stdout.write_str(&format!("{}", e)).await?;
                return Ok(ftest::Result_ {
                    status: Some(ftest::Status::Failed),
                    ..ftest::Result_::EMPTY
                });
            }
            args.extend(user_args.clone());
        }

        // run test.
        // Load bearing to hold job guard.
        let (process, job, stdout_logger, stderr_logger) =
            launch_component_process::<RunTestError>(&test_component, args, test_invoke).await?;

        let test_exit_task = async {
            // Wait for the test process to exit
            fasync::OnSignals::new(&process, zx::Signals::PROCESS_TERMINATED)
                .await
                .map_err(KernelError::ProcessExit)
                .unwrap();

            // Kill the job because the test process has exited.
            // This will kill any stray processes left in the job.
            job.take().kill().unwrap();
        };

        let stdout_logger_task = stdout_logger.buffer_and_drain(test_stdout);
        let stderr_logger_task = stderr_logger.buffer_and_drain(test_stderr);

        // Wait for test to exit and for the std loggers to buffer and drain
        let (stdout_logger_result, stderr_logger_result, ()) =
            join3(stdout_logger_task, stderr_logger_task, test_exit_task).await;
        stdout_logger_result?;
        stderr_logger_result?;

        let process_info = process.info().map_err(RunTestError::ProcessInfo)?;

        match process_info.return_code {
            TR_OK => {
                Ok(ftest::Result_ { status: Some(ftest::Status::Passed), ..ftest::Result_::EMPTY })
            }
            TR_FAILED => {
                // Add a preceding newline so that this does not mix with test output, as
                // test output might not contain a newline at end.
                test_stderr.write_str("\ntest failed.\n").await?;
                Ok(ftest::Result_ { status: Some(ftest::Status::Failed), ..ftest::Result_::EMPTY })
            }
            other => Err(RunTestError::UnexpectedReturnCode(other)),
        }
    }

    pub fn validate_args(args: &Vec<String>) -> Result<(), ArgumentError> {
        // check that arg do not contain test names for filter.
        let mut test_names = vec![];
        for arg in args {
            if args.is_empty() {
                continue;
            }
            if !arg.starts_with("-") {
                test_names.push(arg.clone())
            } else {
                // break on first flag arg
                break;
            }
        }
        if test_names.len() > 0 {
            return Err(ArgumentError::RestrictedArg(test_names.join(", ")));
        }
        let restricted_flags = args
            .iter()
            .filter(|arg| {
                return RESTRICTED_FLAGS.contains(arg.as_str());
            })
            .map(|s| s.clone())
            .collect::<Vec<_>>()
            .join(", ");
        if restricted_flags.len() > 0 {
            return Err(ArgumentError::RestrictedArg(restricted_flags));
        }
        Ok(())
    }
}

/// Filter for use in `get_tests`.
enum TestFilter {
    /// List _all_ tests in the test binary.
    AllTests,
    /// List only the disabled tests in the test binary.
    DisabledTests,
}

/// Launches the Rust test binary specified by the given `Component` to retrieve a list of test
/// names.
async fn get_tests(
    test_component: Arc<Component>,
    filter: TestFilter,
) -> Result<Vec<String>, EnumerationError> {
    let mut args = vec![
        // Allow the usage of unstable options
        "-Z".to_owned(),
        "unstable-options".to_owned(),
        // List installed commands
        "--list".to_owned(),
    ];

    if let TestFilter::DisabledTests = filter {
        args.push("--ignored".to_owned());
    }

    // Load bearing to hold job guard.
    let (process, _job, stdout_logger, stderr_logger) =
        launch_component_process::<EnumerationError>(&test_component, args, None).await?;

    // collect stdout in background before waiting for process termination.
    let stdout_reader = LogStreamReader::new(stdout_logger);
    let stderr_reader = LogStreamReader::new(stderr_logger);

    fasync::OnSignals::new(&process, zx::Signals::PROCESS_TERMINATED)
        .await
        .map_err(KernelError::ProcessExit)
        .unwrap();

    let stdout = stdout_reader.get_logs().await?;
    let stdout = String::from_utf8_lossy(&stdout);

    let stderr = stderr_reader.get_logs().await?;
    let stderr = String::from_utf8_lossy(&stderr);

    let process_info = process.info().map_err(KernelError::ProcessInfo).unwrap();
    if process_info.return_code != 0 {
        // TODO(fxbug.dev/45858): Add a error logger to API so that we can display test stdout logs.
        error!("Failed getting list of tests:\n{}\n{}", stdout, stderr);
        return Err(EnumerationError::ListTest);
    }

    let mut tests = vec![];
    let regex = Regex::new(r"^(.*): test$").unwrap();

    for test in stdout.split("\n") {
        if let Some(capture) = regex.captures(test) {
            if let Some(name) = capture.get(1) {
                tests.push(name.as_str().into());
            }
        }
    }

    Ok(tests)
}

/// Convenience wrapper around [`launch::launch_process`].
async fn launch_component_process<E>(
    component: &Component,
    args: Vec<String>,
    test_invoke: Option<String>,
) -> Result<(zx::Process, launch::ScopedJob, LoggerStream, LoggerStream), E>
where
    E: From<NamespaceError> + From<launch::LaunchError> + From<ComponentError>,
{
    let (client, loader) =
        fidl::endpoints::create_endpoints().map_err(launch::LaunchError::Fidl)?;
    component.loader_service(loader);
    let executable_vmo = Some(component.executable_vmo()?);

    let environ = match test_invoke {
        Some(var) => {
            let mut environ = component.environ.clone().unwrap_or_default();
            environ.push(var);
            Some(environ)
        }
        None => component.environ.clone(),
    };

    Ok(launch::launch_process_with_separate_std_handles(launch::LaunchProcessArgs {
        bin_path: &component.binary,
        process_name: &component.name,
        job: Some(component.job.create_child_job().map_err(KernelError::CreateJob).unwrap()),
        ns: component.ns.clone(),
        args: Some(args),
        name_infos: None,
        environs: environ,
        handle_infos: Some(vec![fproc::HandleInfo {
            handle: (*VDSO_VMO)
                .duplicate_handle(zx::Rights::SAME_RIGHTS)
                .map_err(launch::LaunchError::DuplicateVdso)?,
            id: runtime::HandleInfo::new(runtime::HandleType::VdsoVmo, 0).as_raw(),
        }]),
        loader_proxy_chan: Some(client.into_channel()),
        executable_vmo,
        options: component.options,
    })
    .await?)
}

// TODO(fxbug.dev/45854): Add integration tests.
#[cfg(test)]
mod tests {
    use {
        super::*,
        anyhow::{Context as _, Error},
        assert_matches::assert_matches,
        fidl_fuchsia_test::{
            Result_ as TestResult, RunListenerMarker, RunOptions, Status, SuiteMarker,
        },
        itertools::Itertools,
        pretty_assertions::assert_eq,
        test_runners_lib::cases::TestCaseInfo,
        test_runners_test_lib::{
            assert_event_ord, collect_listener_event, names_to_invocation, test_component,
            ListenerEvent,
        },
    };

    #[test]
    fn validate_args_test() {
        let restricted_flags = vec!["--nocapture", "--list"];

        for flag in restricted_flags {
            let args = vec![flag.to_string()];
            let err = TestServer::validate_args(&args)
                .expect_err(&format!("should error out for flag: {}", flag));
            match err {
                ArgumentError::RestrictedArg(f) => assert_eq!(f, flag),
            }
        }

        // fail when client passes rust test filter.
        let mut args =
            vec!["test_name1".to_string(), "test_name2".to_string(), "--flag".to_string()];
        let err = TestServer::validate_args(&args)
            .expect_err(&format!("should error out for flags: {:?}", args));
        args.pop(); //remove last valid flag
        match err {
            ArgumentError::RestrictedArg(f) => assert_eq!(f, args.join(", ")),
        }

        // succeed when client passes args to flags.
        let args = vec!["--flag1".to_string(), "flag1_arg".to_string(), "--flag".to_string()];
        TestServer::validate_args(&args)
            .unwrap_or_else(|e| panic!("should not error out for flags: {:?}: {:?}", args, e));

        let allowed_flags = vec!["--bench", "--anyflag", "--test", "--mycustomflag", "-e", "-a"];

        for flag in allowed_flags {
            let args = vec![flag.to_string()];
            TestServer::validate_args(&args)
                .unwrap_or_else(|e| panic!("should not error out for flag: {}: {:?}", flag, e));
        }
    }

    async fn sample_test_component() -> Result<Arc<Component>, Error> {
        test_component(
            "fuchsia-pkg://fuchsia.com/rust-test-runner-test#sample-rust-tests.cm",
            "bin/sample_rust_tests.cm",
            "bin/sample_rust_tests",
            vec!["--my_custom_arg".to_string()],
        )
        .await
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn enumerate_simple_test() -> Result<(), Error> {
        let component = sample_test_component().await.unwrap();
        let server = TestServer::new();
        let expected: Vec<TestCaseInfo> = vec![
            TestCaseInfo { name: "my_tests::sample_test_one".to_string(), enabled: true },
            TestCaseInfo { name: "my_tests::ignored_failing_test".to_string(), enabled: false },
            TestCaseInfo { name: "my_tests::ignored_passing_test".to_string(), enabled: false },
            TestCaseInfo { name: "my_tests::passing_test".to_string(), enabled: true },
            TestCaseInfo { name: "my_tests::failing_test".to_string(), enabled: true },
            TestCaseInfo { name: "my_tests::sample_test_two".to_string(), enabled: true },
            TestCaseInfo { name: "my_tests::test_custom_arguments".to_string(), enabled: true },
            TestCaseInfo { name: "my_tests::test_environ".to_string(), enabled: true },
        ]
        .into_iter()
        .sorted()
        .collect();

        let actual: Vec<TestCaseInfo> = server
            .enumerate_tests(component.clone())
            .await?
            .iter()
            .sorted()
            .map(Clone::clone)
            .collect();

        assert_eq!(&expected, &actual);

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn enumerate_empty_test_file() -> Result<(), Error> {
        let component = test_component(
            "fuchsia-pkg://fuchsia.com/rust-test-runner-test#no-rust-tests.cm",
            "bin/no_rust_tests.cm",
            "bin/no_rust_tests",
            vec![],
        )
        .await?;

        let server = TestServer::new();

        assert_eq!(*server.enumerate_tests(component.clone()).await?, Vec::<TestCaseInfo>::new());

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn enumerate_huge_test() -> Result<(), Error> {
        let component = test_component(
            "fuchsia-pkg://fuchsia.com/rust-test-runner-test#huge-rust-tests.cm",
            "bin/huge_rust_tests.cm",
            "bin/huge_rust_tests",
            vec![],
        )
        .await?;

        let server = TestServer::new();

        let actual_tests: Vec<TestCaseInfo> = server
            .enumerate_tests(component.clone())
            .await?
            .iter()
            .sorted()
            .map(Clone::clone)
            .collect();

        let expected: Vec<TestCaseInfo> = (1..=1000)
            .map(|i| TestCaseInfo { name: format!("test_{}", i), enabled: true })
            .sorted()
            .collect();

        assert_eq!(&expected, &actual_tests);

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn invalid_executable_file() -> Result<(), Error> {
        let err = test_component(
            "fuchsia-pkg://fuchsia.com/rust-test-runner-test#invalid-test.cm",
            "bin/invalid.cm",
            "bin/invalid",
            vec![],
        )
        .await
        .expect_err("this function should have error-ed out due to non-existent file.");

        assert_matches!(
            err.downcast::<ComponentError>().unwrap(),
            ComponentError::LoadingExecutable(..)
        );

        Ok(())
    }

    async fn run_tests(
        invocations: Vec<Invocation>,
        run_options: RunOptions,
    ) -> Result<Vec<ListenerEvent>, anyhow::Error> {
        let component = sample_test_component().await.context("Cannot create test component")?;
        let weak_component = Arc::downgrade(&component);
        let server = TestServer::new();

        let (run_listener_client, run_listener) =
            fidl::endpoints::create_request_stream::<RunListenerMarker>()
                .context("Failed to create run_listener")?;
        let (test_suite_client, test_suite) =
            fidl::endpoints::create_request_stream::<SuiteMarker>()
                .context("failed to create suite")?;

        let suite_proxy =
            test_suite_client.into_proxy().context("can't convert suite into proxy")?;
        fasync::Task::spawn(async move {
            server
                .serve_test_suite(test_suite, weak_component)
                .await
                .expect("Failed to run test suite")
        })
        .detach();

        suite_proxy
            .run(&mut invocations.into_iter().map(|i| i.into()), run_options, run_listener_client)
            .context("cannot call run")?;

        collect_listener_event(run_listener).await.context("Failed to collect results")
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn run_one_test() -> Result<(), Error> {
        let events =
            run_tests(names_to_invocation(vec!["my_tests::passing_test"]), RunOptions::EMPTY)
                .await
                .unwrap();

        let expected_events = vec![
            ListenerEvent::start_test("my_tests::passing_test"),
            ListenerEvent::finish_test(
                "my_tests::passing_test",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::finish_all_test(),
        ];

        assert_eq!(expected_events, events);

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn run_multiple_tests_exclude_disabled_tests() -> Result<(), Error> {
        let events = run_tests(
            names_to_invocation(vec![
                "my_tests::sample_test_one",
                "my_tests::passing_test",
                "my_tests::failing_test",
                "my_tests::sample_test_two",
                "my_tests::ignored_passing_test",
                "my_tests::ignored_failing_test",
                "my_tests::test_custom_arguments",
                "my_tests::test_environ",
            ]),
            RunOptions {
                include_disabled_tests: Some(false),
                parallel: Some(1),
                arguments: Some(vec!["--my_custom_arg2".to_owned()]),
                ..RunOptions::EMPTY
            },
        )
        .await
        .unwrap();

        let expected_events = vec![
            ListenerEvent::start_test("my_tests::sample_test_one"),
            ListenerEvent::finish_test(
                "my_tests::sample_test_one",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::passing_test"),
            ListenerEvent::finish_test(
                "my_tests::passing_test",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::failing_test"),
            ListenerEvent::finish_test(
                "my_tests::failing_test",
                TestResult { status: Some(Status::Failed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::sample_test_two"),
            ListenerEvent::finish_test(
                "my_tests::sample_test_two",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::ignored_passing_test"),
            ListenerEvent::finish_test(
                "my_tests::ignored_passing_test",
                TestResult { status: Some(Status::Skipped), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::ignored_failing_test"),
            ListenerEvent::finish_test(
                "my_tests::ignored_failing_test",
                TestResult { status: Some(Status::Skipped), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::test_custom_arguments"),
            ListenerEvent::finish_test(
                "my_tests::test_custom_arguments",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::test_environ"),
            ListenerEvent::finish_test(
                "my_tests::test_environ",
                // We expect this test to fail here because we can't pass environment
                // variables to the test case. However, the integration tests
                // assert that environment variables declared in the test's
                // manifest are properly ingested.
                TestResult { status: Some(Status::Failed), ..TestResult::EMPTY },
            ),
            ListenerEvent::finish_all_test(),
        ];

        assert_eq!(expected_events, events);
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn run_multiple_tests_parallel() -> Result<(), Error> {
        let mut events = run_tests(
            names_to_invocation(vec![
                "my_tests::sample_test_one",
                "my_tests::passing_test",
                "my_tests::failing_test",
                "my_tests::sample_test_two",
                "my_tests::ignored_passing_test",
                "my_tests::ignored_failing_test",
            ]),
            RunOptions {
                include_disabled_tests: Some(false),
                parallel: Some(4),
                arguments: None,
                ..RunOptions::EMPTY
            },
        )
        .await
        .unwrap();

        let mut expected_events = vec![
            ListenerEvent::start_test("my_tests::sample_test_one"),
            ListenerEvent::finish_test(
                "my_tests::sample_test_one",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::passing_test"),
            ListenerEvent::finish_test(
                "my_tests::passing_test",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::failing_test"),
            ListenerEvent::finish_test(
                "my_tests::failing_test",
                TestResult { status: Some(Status::Failed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::sample_test_two"),
            ListenerEvent::finish_test(
                "my_tests::sample_test_two",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::ignored_passing_test"),
            ListenerEvent::finish_test(
                "my_tests::ignored_passing_test",
                TestResult { status: Some(Status::Skipped), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::ignored_failing_test"),
            ListenerEvent::finish_test(
                "my_tests::ignored_failing_test",
                TestResult { status: Some(Status::Skipped), ..TestResult::EMPTY },
            ),
            ListenerEvent::finish_all_test(),
        ];

        assert_event_ord(&events);
        expected_events.sort();
        events.sort();

        assert_eq!(expected_events, events);
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn run_multiple_tests_include_disabled_tests() -> Result<(), Error> {
        let events = run_tests(
            names_to_invocation(vec![
                "my_tests::sample_test_two",
                "my_tests::ignored_passing_test",
                "my_tests::ignored_failing_test",
            ]),
            RunOptions {
                include_disabled_tests: Some(true),
                parallel: Some(1),
                arguments: None,
                ..RunOptions::EMPTY
            },
        )
        .await
        .unwrap();

        let expected_events = vec![
            ListenerEvent::start_test("my_tests::sample_test_two"),
            ListenerEvent::finish_test(
                "my_tests::sample_test_two",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::ignored_passing_test"),
            ListenerEvent::finish_test(
                "my_tests::ignored_passing_test",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("my_tests::ignored_failing_test"),
            ListenerEvent::finish_test(
                "my_tests::ignored_failing_test",
                TestResult { status: Some(Status::Failed), ..TestResult::EMPTY },
            ),
            ListenerEvent::finish_all_test(),
        ];

        assert_eq!(expected_events, events);
        Ok(())
    }
}
