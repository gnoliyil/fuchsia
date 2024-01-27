// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    async_trait::async_trait,
    fidl_fuchsia_process as fproc,
    fidl_fuchsia_test::{
        self as ftest, Invocation, Result_ as TestResult, RunListenerProxy, Status,
    },
    fuchsia_async as fasync,
    fuchsia_runtime::{HandleInfo, HandleType},
    fuchsia_zircon as zx,
    futures::{
        future::{abortable, AbortHandle, Future},
        lock::Mutex,
        stream, AsyncReadExt as _, FutureExt as _, StreamExt as _, TryStreamExt as _,
    },
    lazy_static::lazy_static,
    regex::bytes::Regex,
    std::{
        collections::HashSet,
        num::NonZeroUsize,
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
        logs::{LogError, LogStreamReader, LoggerStream, SocketLogWriter},
    },
    tracing::{debug, error},
    zx::HandleBased as _,
};

/// Implements `fuchsia.test.Suite` and runs provided test.
pub struct TestServer {
    /// Cache to store enumerated tests.
    tests_future_container: MemoizedFutureContainer<EnumeratedTestCases, EnumerationError>,
}

/// Default concurrency for running test cases in parallel.
static PARALLEL_DEFAULT: u16 = 10;

#[async_trait]
impl SuiteServer for TestServer {
    /// Launches a process that lists the tests without actually running any of them. It then parses
    /// the output of that process into a vector of strings.
    ///
    /// Example output for go test process:
    ///
    /// ```text
    /// TestPassing
    /// TestFailing
    /// TestCrashing
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
            .try_for_each_concurrent(num_parallel, |invocation| {
                self.run_test(
                    invocation,
                    test_component.clone(),
                    run_listener,
                    num_parallel,
                    run_options.arguments.clone(),
                )
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

const NEWLINE: u8 = b'\n';
const SOCKET_BUFFER_SIZE: usize = 4096;
const BUF_THRESHOLD: usize = 2048;

lazy_static! {
    static ref RESTRICTED_FLAGS: HashSet<&'static str> =
        vec!["-test.run", "-test.v", "-test.parallel", "-test.count"].into_iter().collect();
}

impl TestServer {
    /// Creates new test server.
    /// Clients should call this function to create new object and then call `serve_test_suite`.
    pub fn new() -> Self {
        Self { tests_future_container: Arc::new(Mutex::new(None)) }
    }

    pub fn validate_args(args: &Vec<String>) -> Result<(), ArgumentError> {
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
        async fn fetch(
            test_component: Arc<Component>,
        ) -> Result<EnumeratedTestCases, EnumerationError> {
            let test_names = get_tests(test_component).await?;
            let tests: Vec<TestCaseInfo> =
                test_names.into_iter().map(|name| TestCaseInfo { name, enabled: true }).collect();
            Ok(Arc::new(tests))
        }

        /// Populates the given `tests_future_container` with a future, or returns a copy of that
        /// future if already present.
        async fn get_or_insert_tests_future(
            test_component: Arc<Component>,
            tests_future_container: MemoizedFutureContainer<EnumeratedTestCases, EnumerationError>,
        ) -> Result<EnumeratedTestCases, EnumerationError> {
            tests_future_container
                .lock()
                .await
                .get_or_insert_with(|| {
                    // The type must be specified in order to compile.
                    let fetched: PinnedFuture<EnumeratedTestCases, EnumerationError> =
                        Box::pin(fetch(test_component));
                    fetched.shared()
                })
                // This clones the `SharedFuture`.
                .clone()
                .await
        }

        let tests_future_container = self.tests_future_container.clone();
        get_or_insert_tests_future(test_component, tests_future_container)
    }

    async fn run_test<'a>(
        &'a self,
        invocation: Invocation,
        component: Arc<Component>,
        run_listener: &'a RunListenerProxy,
        parallel: usize,
        test_args: Option<Vec<String>>,
    ) -> Result<(), RunTestError> {
        let test = invocation.name.as_ref().ok_or(RunTestError::TestCaseName)?.to_string();
        debug!("Running test {}", test);
        let user_passed_args = test_args.unwrap_or(vec![]);

        let (test_stdout, stdout_client) = zx::Socket::create_stream();
        let (test_stderr, stderr_client) = zx::Socket::create_stream();

        let (case_listener_proxy, listener) =
            fidl::endpoints::create_proxy::<fidl_fuchsia_test::CaseListenerMarker>()
                .map_err(FidlError::CreateProxy)
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

        let test_stdout =
            fasync::Socket::from_socket(test_stdout).map_err(KernelError::SocketToAsync).unwrap();
        let mut test_stdout = SocketLogWriter::new(test_stdout);

        let test_stderr =
            fasync::Socket::from_socket(test_stderr).map_err(KernelError::SocketToAsync).unwrap();
        let mut test_stderr = SocketLogWriter::new(test_stderr);

        if let Err(e) = TestServer::validate_args(&user_passed_args) {
            test_stderr.write_str(&format!("{}", e)).await?;
            case_listener_proxy
                .finished(TestResult { status: Some(Status::Failed), ..TestResult::EMPTY })
                .map_err(RunTestError::SendFinish)?;
            return Ok(());
        }
        let mut args = vec![
            "-test.run".to_owned(),
            format!("^{}$", test),
            "-test.parallel".to_owned(),
            parallel.to_string(),
            "-test.v".to_owned(),
        ];
        args.extend(component.args.clone());
        args.extend(user_passed_args);

        // run test.
        // Load bearing to hold job guard.
        let (process, _job, stdout_logger, stderr_logger, _stdin_socket) =
            launch_component_process::<RunTestError>(&component, args).await?;
        let test_start_re = Regex::new(&format!(r"^=== RUN\s+{}$", test)).unwrap();
        let test_end_re = Regex::new(&format!(r"^\s*--- (\w*?): {} \(.*\)$", test)).unwrap();
        let mut skipped = false;
        let stderr_logger_task = stderr_logger.buffer_and_drain(&mut test_stderr);
        let stdout_logger_task = async {
            let mut buffer = vec![];
            let mut socket_buf = vec![0u8; SOCKET_BUFFER_SIZE];
            let mut socket = stdout_logger.take_socket();
            while let Some(bytes_read) =
                NonZeroUsize::new(socket.read(&mut socket_buf[..]).await.map_err(LogError::Read)?)
            {
                let bytes = &socket_buf[..bytes_read.get()];
                let is_last_byte_newline = *bytes.last().unwrap() == NEWLINE;
                let mut iter = bytes.split(|&x| x == NEWLINE).peekable();
                while let Some(b) = iter.next() {
                    if iter.peek() == None && b.len() == 0 {
                        continue;
                    }
                    buffer.extend_from_slice(b);

                    if buffer.len() >= BUF_THRESHOLD {
                        if iter.peek() != None || is_last_byte_newline {
                            buffer.push(NEWLINE)
                        }
                        test_stdout.write(&buffer).await?;
                        buffer.clear();
                        continue;
                    } else if buffer.len() < BUF_THRESHOLD
                        && !is_last_byte_newline
                        && iter.peek() == None
                    {
                        // last part of split without a newline, so skip printing or matching and store
                        // it in buffer for next iteration.
                        break;
                    }
                    // allow matching bodies (case 1 and 2) because they are logically different
                    #[allow(clippy::if_same_then_else)]
                    if iter.peek() == Some(&"".as_bytes())
                        && (buffer == b"PASS" || buffer == b"FAIL")
                    {
                        // end of test, do nothing, no need to print it
                    } else if test_start_re.is_match(&buffer) {
                        // start of test, do nothing, no need to print it
                    } else if let Some(capture) = test_end_re.captures(&buffer) {
                        if capture.get(1).unwrap().as_bytes() == b"SKIP" {
                            skipped = true;
                        }
                    } else {
                        if iter.peek() != None || is_last_byte_newline {
                            buffer.push(NEWLINE)
                        }
                        test_stdout.write(&buffer).await?;
                    }
                    buffer.clear()
                }
            }

            if buffer.len() > 0 {
                test_stdout.write(&buffer).await?;
            }
            buffer.clear();
            Ok::<(), LogError>(())
        };

        let (out_result, err_result) =
            futures::future::join(stdout_logger_task, stderr_logger_task).await;
        out_result?;
        err_result?;

        debug!("Waiting for test to finish: {}", test);

        // wait for test to end.
        fasync::OnSignals::new(&process, zx::Signals::PROCESS_TERMINATED)
            .await
            .map_err(KernelError::ProcessExit)
            .unwrap();
        let process_info = process.info().map_err(RunTestError::ProcessInfo)?;

        // gotest returns 0 is test succeeds and 1 if test fails. This will check if test ended
        // abnormally.
        if process_info.return_code != 0 && process_info.return_code != 1 {
            test_stderr.write_str("Test exited abnormally\n").await?;
            case_listener_proxy
                .finished(TestResult { status: Some(Status::Failed), ..TestResult::EMPTY })
                .map_err(RunTestError::SendFinish)?;
            return Ok(());
        }

        let status = if skipped {
            Status::Skipped
        } else if process_info.return_code != 0 {
            Status::Failed
        } else {
            Status::Passed
        };
        case_listener_proxy
            .finished(TestResult { status: Some(status), ..TestResult::EMPTY })
            .map_err(RunTestError::SendFinish)?;
        debug!("test finish {}", test);
        Ok(())
    }
}

/// Launches the golang test binary specified by the given `Component` to retrieve a list of test
/// names.
async fn get_tests(test_component: Arc<Component>) -> Result<Vec<String>, EnumerationError> {
    let mut args = vec!["-test.list".to_owned(), ".*".to_owned()];
    args.extend(test_component.args.clone());

    // Load bearing to hold job guard.
    let (process, _job, stdout_logger, stderr_logger, _stdin_socket) =
        launch_component_process::<EnumerationError>(&test_component, args).await?;

    // collect stdout/stderr in background before waiting for process termination.
    let stdout_reader = LogStreamReader::new(stdout_logger);
    let stderr_reader = LogStreamReader::new(stderr_logger);

    fasync::OnSignals::new(&process, zx::Signals::PROCESS_TERMINATED)
        .await
        .map_err(KernelError::ProcessExit)?;

    let out_logs = stdout_reader.get_logs().await?;
    let err_logs = stderr_reader.get_logs().await?;

    let output = String::from_utf8_lossy(&out_logs);
    let error = String::from_utf8_lossy(&err_logs);

    let process_info = process.info().map_err(KernelError::ProcessInfo)?;
    if process_info.return_code != 0 {
        // TODO(fxbug.dev/45858): Add a error logger to API so that we can display test stdout logs.
        error!("Failed getting list of tests:\n{}\n{}", output, error);
        return Err(EnumerationError::ListTest);
    }
    let output = output.trim();
    let tests = if !output.is_empty() {
        output.split("\n").into_iter().map(|t| t.into()).collect()
    } else {
        vec![]
    };
    Ok(tests)
}

/// Convenience wrapper around [`launch::launch_process`].
async fn launch_component_process<E>(
    component: &Component,
    args: Vec<String>,
) -> Result<(zx::Process, launch::ScopedJob, LoggerStream, LoggerStream, zx::Socket), E>
where
    E: From<NamespaceError> + From<launch::LaunchError> + From<ComponentError>,
{
    // TODO(fxbug.dev/58076): Golang binary fails if it is not provided with a stdin.
    // Provide it till the issue is fixed.
    let (client, log) = zx::Socket::create_stream();
    let mut handle_infos = vec![];

    const STDIN: u16 = 0;
    handle_infos.push(fproc::HandleInfo {
        handle: log.into_handle(),
        id: HandleInfo::new(HandleType::FileDescriptor, STDIN).as_raw(),
    });

    let (client_end, loader) = fidl::endpoints::create_endpoints();
    component.loader_service(loader);

    let executable_vmo = Some(component.executable_vmo()?);

    let (p, j, out_l, err_l) =
        launch::launch_process_with_separate_std_handles(launch::LaunchProcessArgs {
            bin_path: &component.binary,
            process_name: &component.name,
            job: Some(component.job.create_child_job().map_err(KernelError::CreateJob).unwrap()),
            ns: component.ns.clone(),
            args: Some(args),
            name_infos: None,
            environs: component.environ.clone(),
            handle_infos: Some(handle_infos),
            loader_proxy_chan: Some(client_end.into_channel()),
            executable_vmo,
            options: component.options,
        })
        .await?;
    Ok((p, j, out_l, err_l, client))
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        anyhow::{Context as _, Error},
        assert_matches::assert_matches,
        fidl_fuchsia_test::{
            Result_ as TestResult, RunListenerMarker, RunOptions, Status, SuiteMarker,
        },
        itertools::Itertools as _,
        pretty_assertions::assert_eq,
        test_runners_lib::cases::TestCaseInfo,
        test_runners_test_lib::{
            assert_event_ord, collect_listener_event, names_to_invocation, test_component,
            ListenerEvent,
        },
    };

    async fn sample_test_component() -> Result<Arc<Component>, Error> {
        test_component(
            "fuchsia-pkg://fuchsia.com/go-test-runner-test#sample-go-test.cm",
            "test/sample_go_test.cm",
            "test/sample_go_test",
            vec!["-my_custom_flag".to_string()],
        )
        .await
    }

    #[test]
    fn validate_args_test() {
        // choose a subset of restricted flags and run them through validation function.
        let restricted_flags = vec!["-test.v", "-test.run", "-test.parallel", "-test.count"];

        for flag in restricted_flags {
            let args = vec![flag.to_string()];
            let err = TestServer::validate_args(&args)
                .expect_err(&format!("should error out for flag: {}", flag));
            match err {
                ArgumentError::RestrictedArg(f) => assert_eq!(f, flag),
            }
        }

        let allowed_flags = vec!["-test.short", "-test.anyflag", "-test.timeout", "-mycustomflag"];

        for flag in allowed_flags {
            let args = vec![flag.to_string()];
            TestServer::validate_args(&args)
                .unwrap_or_else(|e| panic!("should not error out for flag: {}: {:?}", flag, e));
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn enumerate_sample_test() -> Result<(), Error> {
        let component = sample_test_component().await.unwrap();
        let server = TestServer::new();
        let expected: Vec<TestCaseInfo> = vec![
            TestCaseInfo { name: "TestCrashing".to_string(), enabled: true },
            TestCaseInfo { name: "TestFailing".to_string(), enabled: true },
            TestCaseInfo { name: "TestPassing".to_string(), enabled: true },
            TestCaseInfo { name: "TestPrefix".to_string(), enabled: true },
            TestCaseInfo { name: "TestPrefixExtra".to_string(), enabled: true },
            TestCaseInfo { name: "TestPrintMultiline".to_string(), enabled: true },
            TestCaseInfo { name: "TestSkipped".to_string(), enabled: true },
            TestCaseInfo { name: "TestSubtests".to_string(), enabled: true },
            TestCaseInfo { name: "TestCustomArg".to_string(), enabled: true },
            TestCaseInfo { name: "TestCustomArg2".to_string(), enabled: true },
            TestCaseInfo { name: "TestEnviron".to_string(), enabled: true },
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
            "fuchsia-pkg://fuchsia.com/go-test-runner-test#empty-go-test.cm",
            "test/empty_go_test.cm",
            "test/empty_go_test",
            vec![],
        )
        .await?;

        let server = TestServer::new();

        assert_eq!(*server.enumerate_tests(component.clone()).await?, Vec::<TestCaseInfo>::new());

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

    #[fuchsia::test(logging_tags=["gtest_runner_test"])]
    async fn run_multiple_tests() -> Result<(), Error> {
        let events = run_tests(
            names_to_invocation(vec![
                "TestCrashing",
                "TestPassing",
                "TestFailing",
                "TestPrefix",
                "TestSkipped",
                "TestPrefixExtra",
                "TestCustomArg",
                "TestCustomArg2",
            ]),
            RunOptions {
                include_disabled_tests: Some(false),
                parallel: Some(1),
                arguments: Some(vec!["-my_custom_flag_2".to_owned()]),
                ..RunOptions::EMPTY
            },
        )
        .await
        .unwrap();

        let expected_events = vec![
            ListenerEvent::start_test("TestCrashing"),
            ListenerEvent::finish_test(
                "TestCrashing",
                TestResult { status: Some(Status::Failed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("TestPassing"),
            ListenerEvent::finish_test(
                "TestPassing",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("TestFailing"),
            ListenerEvent::finish_test(
                "TestFailing",
                TestResult { status: Some(Status::Failed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("TestPrefix"),
            ListenerEvent::finish_test(
                "TestPrefix",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("TestSkipped"),
            ListenerEvent::finish_test(
                "TestSkipped",
                TestResult { status: Some(Status::Skipped), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("TestPrefixExtra"),
            ListenerEvent::finish_test(
                "TestPrefixExtra",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("TestCustomArg"),
            ListenerEvent::finish_test(
                "TestCustomArg",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("TestCustomArg2"),
            ListenerEvent::finish_test(
                "TestCustomArg2",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::finish_all_test(),
        ];

        assert_eq!(expected_events, events);
        Ok(())
    }

    #[fuchsia::test(logging_tags=["gtest_runner_test"])]
    async fn run_multiple_tests_parallel() -> Result<(), Error> {
        let mut events = run_tests(
            names_to_invocation(vec![
                "TestCrashing",
                "TestPassing",
                "TestFailing",
                "TestPrefix",
                "TestSkipped",
                "TestPrefixExtra",
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
            ListenerEvent::start_test("TestCrashing"),
            ListenerEvent::finish_test(
                "TestCrashing",
                TestResult { status: Some(Status::Failed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("TestPassing"),
            ListenerEvent::finish_test(
                "TestPassing",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("TestFailing"),
            ListenerEvent::finish_test(
                "TestFailing",
                TestResult { status: Some(Status::Failed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("TestPrefix"),
            ListenerEvent::finish_test(
                "TestPrefix",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("TestSkipped"),
            ListenerEvent::finish_test(
                "TestSkipped",
                TestResult { status: Some(Status::Skipped), ..TestResult::EMPTY },
            ),
            ListenerEvent::start_test("TestPrefixExtra"),
            ListenerEvent::finish_test(
                "TestPrefixExtra",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
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
    async fn run_no_test() -> Result<(), Error> {
        let events = run_tests(
            vec![],
            RunOptions {
                include_disabled_tests: Some(false),
                parallel: Some(1),
                arguments: None,
                ..RunOptions::EMPTY
            },
        )
        .await
        .unwrap();

        let expected_events = vec![ListenerEvent::finish_all_test()];

        assert_eq!(expected_events, events);
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn run_one_test() -> Result<(), Error> {
        let events = run_tests(
            names_to_invocation(vec!["TestPassing"]),
            RunOptions {
                include_disabled_tests: Some(false),
                parallel: Some(1),
                arguments: None,
                ..RunOptions::EMPTY
            },
        )
        .await
        .unwrap();

        let expected_events = vec![
            ListenerEvent::start_test("TestPassing"),
            ListenerEvent::finish_test(
                "TestPassing",
                TestResult { status: Some(Status::Passed), ..TestResult::EMPTY },
            ),
            ListenerEvent::finish_all_test(),
        ];

        assert_eq!(expected_events, events);

        Ok(())
    }
}
