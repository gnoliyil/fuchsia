// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::binder_latency::run_binder_latency,
    crate::gbenchmark::*,
    crate::gtest::*,
    crate::helpers::*,
    anyhow::{anyhow, Context, Error},
    fidl::endpoints::create_proxy,
    fidl_fuchsia_component_decl as fdecl, fidl_fuchsia_component_runner as frunner,
    fidl_fuchsia_test::{self as ftest},
    futures::TryStreamExt,
    tracing::debug,
};

/// Handles a single `ftest::SuiteRequestStream`.
///
/// # Parameters
/// - `kernels_dir`: The root directory for the starnix_kernel instance's configuration.
/// - `start_info`: The start info to use when running the test component.
/// - `stream`: The request stream to handle.
pub async fn handle_suite_requests(
    kernels_dir: &vfs::directory::immutable::Simple,
    mut start_info: frunner::ComponentStartInfo,
    mut stream: ftest::SuiteRequestStream,
) -> Result<(), Error> {
    debug!(?start_info, "got suite request stream");
    let test_type = test_type(start_info.program.as_ref().unwrap());
    // The kernel start info is largely the same as that of the test component. The main difference
    // is that the kernel_start_info does not contain the outgoing directory of the test component.
    let kernel_start_info = clone_start_info(&mut start_info)?;

    // Instantiates a starnix_kernel instance in the test component's realm. Running the kernel
    // in the test realm allows coverage data to be collected for the runner itself, during the
    // execution of the test.
    let TestKernel { kernel_runner: starnix_kernel, realm, name: kernel_name } =
        TestKernel::instantiate_in_realm(kernels_dir, kernel_start_info).await?;
    debug!(%kernel_name, "instantiated starnix test kernel");

    while let Some(event) = stream.try_next().await? {
        let mut test_start_info = clone_start_info(&mut start_info)?;
        debug!("got suite request");
        match event {
            ftest::SuiteRequest::GetTests { iterator, .. } => {
                debug!("enumerating test cases");
                let stream = iterator.into_stream()?;

                if test_type.is_gtest_like() {
                    handle_case_iterator_for_gtests(test_start_info, &starnix_kernel, stream)
                        .await?;
                } else {
                    handle_case_iterator(
                        test_start_info
                            .resolved_url
                            .as_ref()
                            .ok_or(anyhow!("Missing resolved URL"))?,
                        stream,
                    )
                    .await?;
                }
            }
            ftest::SuiteRequest::Run { tests, options, listener, .. } => {
                debug!(?tests, "running tests");
                let run_listener_proxy =
                    listener.into_proxy().context("Can't convert run listener channel to proxy")?;

                if tests.is_empty() {
                    debug!("no tests listed, returning");
                    run_listener_proxy.on_finished()?;
                    break;
                }

                // Replace tests with program arguments if they were passed in.
                let mut program =
                    test_start_info.program.clone().ok_or(anyhow!("Missing program."))?;
                if let Some(test_args) = options.arguments {
                    replace_program_args(test_args, &mut program);
                }
                test_start_info.program = Some(program);
                debug!(?test_start_info, "running tests with info");

                match test_type {
                    _ if test_type.is_gtest_like() => {
                        run_gtest_cases(
                            tests,
                            test_start_info,
                            &run_listener_proxy,
                            &starnix_kernel,
                            &test_type,
                        )
                        .await?;
                    }
                    TestType::Gbenchmark => {
                        run_gbenchmark(
                            tests.get(0).unwrap().clone(),
                            test_start_info,
                            &run_listener_proxy,
                            &starnix_kernel,
                        )
                        .await?
                    }
                    TestType::BinderLatency => {
                        run_binder_latency(
                            tests.get(0).unwrap().clone(),
                            test_start_info,
                            &run_listener_proxy,
                            &starnix_kernel,
                        )
                        .await?
                    }
                    _ => {
                        run_test_case(
                            tests.get(0).unwrap().clone(),
                            test_start_info,
                            &run_listener_proxy,
                            &starnix_kernel,
                        )
                        .await?
                    }
                }

                run_listener_proxy.on_finished()?;
            }
        }
    }

    realm
        .destroy_child(&mut fdecl::ChildRef {
            name: kernel_name,
            collection: Some(RUNNERS_COLLECTION.into()),
        })
        .await?
        .map_err(|e| anyhow::anyhow!("failed to destory runner child: {:?}", e))?;

    Ok(())
}

/// Runs a test case associated with a single `ftest::SuiteRequest::Run` request.
///
/// Running the test component is delegated to an instance of the starnix kernel.
///
/// # Parameters
/// - `tests`: The tests that are to be run. Each test executes an independent run of the test
/// component.
/// - `test_url`: The URL of the test component.
/// - `program`: The program data associated with the runner request for the test component.
/// - `run_listener_proxy`: The listener proxy for the test run.
/// - `starnix_kernel`: The instance of the starnix kernel that will run the test component.
async fn run_test_case(
    test: ftest::Invocation,
    mut start_info: frunner::ComponentStartInfo,
    run_listener_proxy: &ftest::RunListenerProxy,
    starnix_kernel: &frunner::ComponentRunnerProxy,
) -> Result<(), Error> {
    debug!("running generic fallback test suite");
    let (case_listener_proxy, case_listener) = create_proxy::<ftest::CaseListenerMarker>()?;
    let (numbered_handles, stdout_client, stderr_client) = create_numbered_handles();
    start_info.numbered_handles = numbered_handles;

    debug!("notifying client test case started");
    run_listener_proxy.on_test_case_started(
        test,
        ftest::StdHandles {
            out: Some(stdout_client),
            err: Some(stderr_client),
            ..ftest::StdHandles::EMPTY
        },
        case_listener,
    )?;

    debug!("starting test component");
    let component_controller = start_test_component(start_info, starnix_kernel)?;

    let result = read_result(component_controller.take_event_stream()).await;
    debug!(?result, "notifying client test case finished");
    case_listener_proxy.finished(result)?;

    Ok(())
}

/// Lists all the available test cases and returns them in response to
/// `ftest::CaseIteratorRequest::GetNext`.
///
/// Currently only one "test case" is returned, named `test_name`.
async fn handle_case_iterator(
    test_name: &str,
    mut stream: ftest::CaseIteratorRequestStream,
) -> Result<(), Error> {
    let mut cases_iter = vec![ftest::Case {
        name: Some(test_name.to_string()),
        enabled: Some(true),
        ..ftest::Case::EMPTY
    }]
    .into_iter();

    while let Some(event) = stream.try_next().await? {
        match event {
            ftest::CaseIteratorRequest::GetNext { responder } => {
                responder.send(&mut cases_iter)?;
            }
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fidl::endpoints::{create_request_stream, ClientEnd},
        fuchsia_async as fasync, fuchsia_zircon as zx,
        futures::TryStreamExt,
    };

    /// Returns a `ftest::CaseIteratorProxy` that is served by `super::handle_case_iterator`.
    ///
    /// # Parameters
    /// - `test_name`: The name of the test case that is provided to `handle_case_iterator`.
    fn set_up_iterator(test_name: &str) -> ftest::CaseIteratorProxy {
        let test_name = test_name.to_string();
        let (iterator_client_end, iterator_stream) =
            create_request_stream::<ftest::CaseIteratorMarker>()
                .expect("Couldn't create case iterator");
        fasync::Task::local(async move {
            let _ = handle_case_iterator(&test_name, iterator_stream).await;
        })
        .detach();

        iterator_client_end.into_proxy().expect("Failed to create proxy")
    }

    /// Spawns a `ComponentRunnerRequestStream` server that immediately closes all incoming
    /// component controllers with the epitaph specified in `component_controller_epitaph`.
    ///
    /// This function can be used to mock the starnix kernel in a way that simulates a component
    /// exiting with or without error.
    ///
    /// # Parameters
    /// - `component_controller_epitaph`: The epitaph used to close the component controller.
    ///
    /// # Returns
    /// A `ComponentRunnerProxy` that serves each run request by closing the component with the
    /// provided epitaph.
    fn spawn_runner(component_controller_epitaph: zx::Status) -> frunner::ComponentRunnerProxy {
        let (proxy, mut request_stream) =
            fidl::endpoints::create_proxy_and_stream::<frunner::ComponentRunnerMarker>().unwrap();
        fasync::Task::local(async move {
            while let Some(event) =
                request_stream.try_next().await.expect("Error in test runner request stream")
            {
                match event {
                    frunner::ComponentRunnerRequest::Start {
                        start_info: _start_info,
                        controller,
                        ..
                    } => {
                        controller
                            .close_with_epitaph(component_controller_epitaph)
                            .expect("Could not close with epitaph");
                    }
                }
            }
        })
        .detach();
        proxy
    }

    /// Returns the status from the first test case reported to `run_listener_stream`.
    ///
    /// This is done by listening to the first `CaseListener` provided via `OnTestCaseStarted`.
    ///
    /// # Parameters
    /// - `run_listener_stream`: The run listener stream to extract the test status from.
    ///
    /// # Returns
    /// The status of the first test case that is run, or `None` if no such status is reported.
    async fn listen_to_test_result(
        mut run_listener_stream: ftest::RunListenerRequestStream,
    ) -> Option<ftest::Status> {
        match run_listener_stream.try_next().await.expect("..") {
            Some(ftest::RunListenerRequest::OnTestCaseStarted {
                invocation: _,
                std_handles: _,
                listener,
                ..
            }) => match listener
                .into_stream()
                .expect("Failed to get case listener stream")
                .try_next()
                .await
                .expect("Failed to get case listener stream request")
            {
                Some(ftest::CaseListenerRequest::Finished { result, .. }) => result.status,
                _ => None,
            },
            _ => None,
        }
    }

    /// Spawns a task that calls `super::run_test_cases` with the provided `run_listener` and
    /// `runner_proxy`. The call is made with a mock test case.
    fn spawn_run_test_cases(
        run_listener: ClientEnd<ftest::RunListenerMarker>,
        starnix_kernel: frunner::ComponentRunnerProxy,
    ) {
        fasync::Task::local(async move {
            let _ = run_test_case(
                ftest::Invocation {
                    name: Some("".to_string()),
                    tag: Some("".to_string()),
                    ..ftest::Invocation::EMPTY
                },
                frunner::ComponentStartInfo {
                    ns: Some(vec![]),
                    ..frunner::ComponentStartInfo::EMPTY
                },
                &run_listener.into_proxy().expect("Couldn't create proxy."),
                &starnix_kernel,
            )
            .await;
        })
        .detach();
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_number_of_test_cases() {
        let iterator_proxy = set_up_iterator("test");
        let first_result = iterator_proxy.get_next().await.expect("Didn't get first result");
        let second_result = iterator_proxy.get_next().await.expect("Didn't get second result");

        assert_eq!(first_result.len(), 1);
        assert_eq!(second_result.len(), 0);
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_case_name() {
        let test_name = "test_name";
        let iterator_proxy = set_up_iterator(test_name);
        let result = iterator_proxy.get_next().await.expect("Didn't get first result");
        assert_eq!(result[0].name, Some(test_name.to_string()));
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_case_enabled() {
        let iterator_proxy = set_up_iterator("test");
        let result = iterator_proxy.get_next().await.expect("Didn't get first result");
        assert_eq!(result[0].enabled, Some(true));
    }

    /// Tests that when starnix closes the component controller with an `OK` status, the test case
    /// passes.
    #[fasync::run_singlethreaded(test)]
    async fn test_component_controller_epitaph_ok() {
        let starnix_kernel = spawn_runner(zx::Status::OK);
        let (run_listener, run_listener_stream) =
            create_request_stream::<ftest::RunListenerMarker>()
                .expect("Couldn't create case listener");
        spawn_run_test_cases(run_listener, starnix_kernel);
        assert_eq!(listen_to_test_result(run_listener_stream).await, Some(ftest::Status::Passed));
    }

    /// Tests that when starnix closes the component controller with an error status, the test case
    /// fails.
    #[fasync::run_singlethreaded(test)]
    async fn test_component_controller_epitaph_not_ok() {
        let starnix_kernel = spawn_runner(zx::Status::INTERNAL);
        let (run_listener, run_listener_stream) =
            create_request_stream::<ftest::RunListenerMarker>()
                .expect("Couldn't create case listener");
        spawn_run_test_cases(run_listener, starnix_kernel);
        assert_eq!(listen_to_test_result(run_listener_stream).await, Some(ftest::Status::Failed));
    }
}
