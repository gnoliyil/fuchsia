// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::launcher::ComponentLauncher,
    async_trait::async_trait,
    fidl_fuchsia_test::{
        self as ftest, Invocation, Result_ as TestResult, RunListenerProxy, Status,
    },
    fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::{
        future::{abortable, AbortHandle},
        prelude::*,
        TryStreamExt,
    },
    std::sync::{Arc, Weak},
    test_runners_lib::{
        cases::TestCaseInfo,
        elf::{Component, EnumeratedTestCases, FidlError, KernelError, SuiteServer},
        errors::*,
        logs::SocketLogWriter,
    },
    tracing::{debug, error},
};

/// Implements `fuchsia.test.Suite` and runs provided test.
#[derive(Default)]
pub struct TestServer<T: ComponentLauncher> {
    pub launcher: T,
}

static PARALLEL_DEFAULT: u16 = 1;

#[async_trait]
impl<T: 'static> SuiteServer for TestServer<T>
where
    T: ComponentLauncher,
{
    /// Launches test process and gets test list out. Returns list of tests names in the format
    /// defined by gtests, i.e FOO.Bar.
    /// It only runs enumeration logic once, caches and returns the same result back on subsequent
    /// calls.
    async fn enumerate_tests(
        &self,
        _test_component: Arc<Component>,
    ) -> Result<EnumeratedTestCases, EnumerationError> {
        Ok(Arc::new(vec![TestCaseInfo { name: "main".to_string(), enabled: true }]))
    }

    async fn run_tests(
        &self,
        invocations: Vec<Invocation>,
        run_options: ftest::RunOptions,
        component: Arc<Component>,
        run_listener: &RunListenerProxy,
    ) -> Result<(), RunTestError> {
        let num_parallel =
            Self::get_parallel_count(run_options.parallel.unwrap_or(PARALLEL_DEFAULT));

        let invocations = stream::iter(invocations);
        invocations
            .map(Ok)
            .try_for_each_concurrent(num_parallel, |invocation| {
                self.run_test(invocation, &run_options, component.clone(), run_listener)
            })
            .await
    }

    /// Run this server.
    fn run(
        self,
        weak_component: Weak<Component>,
        test_url: &str,
        stream: ftest::SuiteRequestStream,
    ) -> AbortHandle {
        let test_url = test_url.to_owned();
        let (fut, test_suite_abortable_handle) =
            abortable(self.serve_test_suite(stream, weak_component.clone()));

        fasync::Task::local(async move {
            match fut.await {
                Ok(result) => {
                    if let Err(e) = result {
                        error!("server failed for test {}: {:?}", test_url, e);
                    }
                }
                Err(e) => debug!("server aborted for test {}: {:?}", test_url, e),
            }
            debug!("Done running server for {}.", test_url);
        })
        .detach();
        test_suite_abortable_handle
    }
}

impl<T: 'static> TestServer<T>
where
    T: ComponentLauncher,
{
    pub fn new(launcher_: T) -> Self {
        Self { launcher: launcher_ }
    }

    pub fn validate_args(_args: &Vec<String>) -> Result<(), ArgumentError> {
        // Unopinionated about args,
        // they're passed through to the test program unfiltered
        Ok(())
    }

    async fn run_test<'a>(
        &'a self,
        invocation: Invocation,
        run_options: &ftest::RunOptions,
        component: Arc<Component>,
        run_listener: &RunListenerProxy,
    ) -> Result<(), RunTestError> {
        if "main" != invocation.name.as_ref().ok_or(RunTestError::TestCaseName)?.to_string() {
            // "main" is the only valid test case name
            return Ok(());
        }

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
                    ..Default::default()
                },
                listener,
            )
            .map_err(RunTestError::SendStart)?;
        let mut test_stdout = SocketLogWriter::new(
            fasync::Socket::from_socket(test_stdout).map_err(KernelError::SocketToAsync).unwrap(),
        );
        let mut test_stderr = SocketLogWriter::new(
            fasync::Socket::from_socket(test_stderr).map_err(KernelError::SocketToAsync).unwrap(),
        );

        let mut args = component.args.clone();
        if let Some(user_args) = &run_options.arguments {
            args.extend(user_args.clone());
        }

        // Launch test program
        let (process, _job, stdout_logger, stderr_logger) =
            self.launcher.launch_process(&component, args).await?;

        // Drain stdout
        let stdout_fut = stdout_logger.buffer_and_drain(&mut test_stdout);
        // Drain stderr
        let stderr_fut = stderr_logger.buffer_and_drain(&mut test_stderr);
        let (ret1, ret2) = futures::future::join(stdout_fut, stderr_fut).await;
        ret1?;
        ret2?;

        // Wait for test to return
        fasync::OnSignals::new(&process, zx::Signals::PROCESS_TERMINATED)
            .await
            .map_err(KernelError::ProcessExit)
            .unwrap();
        let process_info = process.info().map_err(RunTestError::ProcessInfo)?;

        // Map return value of zero to Passed, non-zero to Failed
        let status = match process_info.return_code {
            0 => Status::Passed,
            _ => Status::Failed,
        };
        case_listener_proxy
            .finished(TestResult { status: Some(status), ..Default::default() })
            .map_err(RunTestError::SendFinish)?;
        Ok(())
    }
}
