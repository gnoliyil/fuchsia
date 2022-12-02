// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        artifacts,
        cancel::{Cancelled, NamedFutureExt, OrCancel},
        diagnostics::{self, LogDisplayConfiguration},
        outcome::{Outcome, RunTestSuiteError},
        output::{self, RunReporter, SuiteId, Timestamp},
        params::{RunParams, TestParams, TimeoutBehavior},
        running_suite::{run_suite_and_collect_logs, RunningSuite},
        trace::duration,
    },
    fidl_fuchsia_test_manager::{self as ftest_manager, RunBuilderProxy},
    fuchsia_async as fasync,
    futures::{prelude::*, stream::FuturesUnordered, StreamExt},
    std::collections::HashMap,
    std::io::Write,
    std::path::PathBuf,
    tracing::{error, warn},
};

// Will invoke the WithSchedulingOptions FIDL method if a client indicates
// that they want to use experimental parallel execution.
async fn request_scheduling_options(
    run_params: &RunParams,
    builder_proxy: &RunBuilderProxy,
) -> Result<(), RunTestSuiteError> {
    let scheduling_options = ftest_manager::SchedulingOptions {
        max_parallel_suites: run_params.experimental_parallel_execution,
        accumulate_debug_data: Some(run_params.accumulate_debug_data),
        ..ftest_manager::SchedulingOptions::EMPTY
    };
    builder_proxy.with_scheduling_options(scheduling_options)?;
    Ok(())
}

/// Schedule and run the tests specified in |test_params|, and collect the results.
/// Note this currently doesn't record the result or call finished() on run_reporter,
/// the caller should do this instead.
async fn run_tests<'a, F: 'a + Future<Output = ()> + Unpin>(
    builder_proxy: RunBuilderProxy,
    test_params: impl IntoIterator<Item = TestParams>,
    run_params: RunParams,
    run_reporter: &'a RunReporter,
    cancel_fut: F,
) -> Result<Outcome, RunTestSuiteError> {
    let mut suite_start_futs = FuturesUnordered::new();
    let mut suite_reporters = HashMap::new();
    for (suite_id_raw, params) in test_params.into_iter().enumerate() {
        let timeout = params
            .timeout_seconds
            .map(|seconds| std::time::Duration::from_secs(seconds.get() as u64));

        let run_options = fidl_fuchsia_test_manager::RunOptions {
            parallel: params.parallel,
            arguments: Some(params.test_args),
            run_disabled_tests: Some(params.also_run_disabled_tests),
            case_filters_to_run: params.test_filters,
            log_iterator: Some(run_params.log_protocol.unwrap_or_else(diagnostics::get_type)),
            ..fidl_fuchsia_test_manager::RunOptions::EMPTY
        };

        let suite_id = SuiteId(suite_id_raw as u32);
        let suite = run_reporter.new_suite(&params.test_url, &suite_id).await?;
        suite.set_tags(params.tags).await;
        suite_reporters.insert(suite_id, suite);
        let (suite_controller, suite_server_end) = fidl::endpoints::create_proxy()?;
        let suite_and_id_fut = RunningSuite::wait_for_start(
            suite_controller,
            params.max_severity_logs,
            timeout,
            std::time::Duration::from_secs(run_params.timeout_grace_seconds as u64),
            None,
        )
        .map(move |running_suite| (running_suite, suite_id));
        suite_start_futs.push(suite_and_id_fut);
        builder_proxy.add_suite(&params.test_url, run_options, suite_server_end)?;
    }

    request_scheduling_options(&run_params, &builder_proxy).await?;
    let (run_controller, run_server_end) = fidl::endpoints::create_proxy()?;
    let run_controller_ref = &run_controller;
    builder_proxy.build(run_server_end)?;
    run_reporter.started(Timestamp::Unknown).await?;
    let cancel_fut = cancel_fut.shared();
    let cancel_fut_clone = cancel_fut.clone();

    let handle_suite_fut = async move {
        let mut num_failed = 0;
        let mut final_outcome = None;
        let mut stopped_prematurely = false;
        // for now, we assume that suites are run serially.
        loop {
            let (running_suite, suite_id) = match suite_start_futs
                .next()
                .named("suite_start")
                .or_cancelled(cancel_fut.clone())
                .await
            {
                Ok(Some((running_suite, suite_id))) => (running_suite, suite_id),
                // normal completion.
                Ok(None) => break,
                Err(Cancelled(_)) => {
                    stopped_prematurely = true;
                    final_outcome = Some(Outcome::Cancelled);
                    break;
                }
            };

            let suite_reporter = suite_reporters.remove(&suite_id).unwrap();

            let log_display = LogDisplayConfiguration {
                show_full_moniker: run_params.show_full_moniker,
                min_severity: run_params.min_severity_logs,
            };

            let result = run_suite_and_collect_logs(
                running_suite,
                &suite_reporter,
                log_display,
                cancel_fut.clone(),
            )
            .await;
            let suite_outcome = result.unwrap_or_else(|err| Outcome::error(err));
            // We should always persist results, even if something failed.
            suite_reporter.finished().await?;

            num_failed = match suite_outcome {
                Outcome::Passed => num_failed,
                _ => num_failed + 1,
            };
            let stop_due_to_timeout = match run_params.timeout_behavior {
                TimeoutBehavior::TerminateRemaining => suite_outcome == Outcome::Timedout,
                TimeoutBehavior::Continue => false,
            };
            let stop_due_to_failures = match run_params.stop_after_failures.as_ref() {
                Some(threshold) => num_failed >= threshold.get(),
                None => false,
            };
            let stop_due_to_cancellation = matches!(&suite_outcome, Outcome::Cancelled);
            let stop_due_to_internal_error = match &suite_outcome {
                Outcome::Error { origin } => origin.is_internal_error(),
                _ => false,
            };

            final_outcome = match (final_outcome.take(), suite_outcome) {
                (None, first_outcome) => Some(first_outcome),
                (Some(outcome), Outcome::Passed) => Some(outcome),
                (Some(_), failing_outcome) => Some(failing_outcome),
            };
            if stop_due_to_timeout
                || stop_due_to_failures
                || stop_due_to_cancellation
                || stop_due_to_internal_error
            {
                stopped_prematurely = true;
                break;
            }
        }
        if stopped_prematurely {
            // Ignore errors here since we're stopping anyway.
            let _ = run_controller_ref.stop();
            // Drop remaining controllers, which is the same as calling kill on
            // each controller.
            suite_start_futs.clear();
            for (_id, reporter) in suite_reporters.drain() {
                reporter.finished().await?;
            }
        }
        Ok(final_outcome.unwrap_or(Outcome::Passed))
    };

    let handle_run_events_fut = async move {
        duration!("run_events");
        let mut artifact_tasks = vec![];
        loop {
            let events = run_controller_ref.get_events().named("run_event").await?;
            if events.len() == 0 {
                break;
            }

            for event in events.into_iter() {
                let ftest_manager::RunEvent { payload, .. } = event;
                match payload {
                    // TODO(fxbug.dev/91151): Add support for RunStarted and RunStopped when test_manager sends them.
                    Some(ftest_manager::RunEventPayload::Artifact(artifact)) => {
                        let artifact_fut = artifacts::drain_artifact(
                            run_reporter,
                            artifact,
                            diagnostics::LogCollectionOptions {
                                max_severity: None,
                                format: LogDisplayConfiguration {
                                    show_full_moniker: run_params.show_full_moniker,
                                    min_severity: run_params.min_severity_logs,
                                },
                            },
                        )
                        .await?;
                        artifact_tasks.push(fasync::Task::spawn(artifact_fut));
                    }
                    e => {
                        warn!("Discarding run event: {:?}", e);
                    }
                }
            }
        }
        for task in artifact_tasks {
            match task.await {
                Err(e) => {
                    error!("Failed to collect artifact for run: {:?}", e);
                }
                Ok(Some(_log_result)) => warn!("Unexpectedly got log results for the test run"),
                Ok(None) => (),
            }
        }
        Ok(())
    };

    // Make sure we stop polling run events on cancel. Since cancellation is expected
    // ignore cancellation errors.
    let cancellable_run_events_fut = handle_run_events_fut
        .boxed_local()
        .or_cancelled(cancel_fut_clone)
        .map(|cancelled_result| match cancelled_result {
            Ok(completed_result) => completed_result,
            Err(Cancelled(_)) => Ok(()),
        });

    // Use join instead of try_join as we want to poll the futures to completion
    // even if one fails.
    match futures::future::join(handle_suite_fut, cancellable_run_events_fut).await {
        (Ok(outcome), Ok(())) => Ok(outcome),
        (Err(e), _) | (_, Err(e)) => Err(e),
    }
}

/// Runs tests specified in |test_params| and reports the results to
/// |run_reporter|.
///
/// Options specifying how the test run is executed are passed in via |run_params|.
/// Options specific to how a single suite is run are passed in via the entry for
/// the suite in |test_params|.
/// |cancel_fut| is used to gracefully stop execution of tests. Tests are
/// terminated and recorded when the future resolves. The caller can control when the
/// future resolves by passing in the receiver end of a `future::channel::oneshot`
/// channel.
pub async fn run_tests_and_get_outcome<F: Future<Output = ()>>(
    builder_proxy: RunBuilderProxy,
    test_params: impl IntoIterator<Item = TestParams>,
    run_params: RunParams,
    run_reporter: RunReporter,
    cancel_fut: F,
) -> Outcome {
    let test_outcome = match run_tests(
        builder_proxy,
        test_params,
        run_params,
        &run_reporter,
        cancel_fut.boxed_local(),
    )
    .await
    {
        Ok(s) => s,
        Err(e) => {
            return Outcome::error(e);
        }
    };

    let report_result =
        match run_reporter.stopped(&test_outcome.clone().into(), Timestamp::Unknown).await {
            Ok(()) => run_reporter.finished().await,
            Err(e) => Err(e),
        };
    if let Err(e) = report_result {
        warn!("Failed to record results: {:?}", e);
    }

    test_outcome
}

pub struct DirectoryReporterOptions {
    /// Root path of the directory.
    pub root_path: PathBuf,
}

/// Create a reporter for use with |run_tests_and_get_outcome|.
pub fn create_reporter<W: 'static + Write + Send + Sync>(
    filter_ansi: bool,
    dir: Option<DirectoryReporterOptions>,
    writer: W,
) -> Result<output::RunReporter, anyhow::Error> {
    let stdout_reporter = output::ShellReporter::new(writer);
    let dir_reporter = dir
        .map(|dir| {
            output::DirectoryWithStdoutReporter::new(dir.root_path, output::SchemaVersion::V1)
        })
        .transpose()?;
    let reporter = match (dir_reporter, filter_ansi) {
        (Some(dir_reporter), false) => output::RunReporter::new(output::MultiplexedReporter::new(
            stdout_reporter,
            dir_reporter,
        )),
        (Some(dir_reporter), true) => output::RunReporter::new_ansi_filtered(
            output::MultiplexedReporter::new(stdout_reporter, dir_reporter),
        ),
        (None, false) => output::RunReporter::new(stdout_reporter),
        (None, true) => output::RunReporter::new_ansi_filtered(stdout_reporter),
    };
    Ok(reporter)
}

#[cfg(test)]
mod test {
    use {
        super::*,
        crate::output::{EntityId, InMemoryReporter},
        assert_matches::assert_matches,
        fidl::endpoints::create_proxy_and_stream,
        futures::future::join,
        maplit::hashmap,
    };
    #[cfg(target_os = "fuchsia")]
    use {
        fidl::endpoints::ServerEnd,
        fidl_fuchsia_io as fio, fuchsia_zircon as zx,
        futures::future::join3,
        vfs::{
            directory::entry::DirectoryEntry, execution_scope::ExecutionScope,
            file::vmo::read_only_static, pseudo_directory,
        },
    };

    // TODO(fxbug.dev/98222): add unit tests for suite artifacts too.

    async fn fake_running_all_suites_and_return_run_events(
        mut stream: ftest_manager::RunBuilderRequestStream,
        mut suite_events: HashMap<&str, Vec<ftest_manager::SuiteEvent>>,
        run_events: Vec<ftest_manager::RunEvent>,
    ) {
        let mut suite_streams = vec![];

        let mut run_controller = None;
        while let Ok(Some(req)) = stream.try_next().await {
            match req {
                ftest_manager::RunBuilderRequest::AddSuite { test_url, controller, .. } => {
                    let events = suite_events
                        .remove(test_url.as_str())
                        .expect("Got a request for an unexpected test URL");
                    suite_streams.push((controller.into_stream().expect("into stream"), events));
                }
                ftest_manager::RunBuilderRequest::Build { controller, .. } => {
                    run_controller = Some(controller);
                    break;
                }
                ftest_manager::RunBuilderRequest::WithSchedulingOptions { options, .. } => {
                    if let Some(_) = options.max_parallel_suites {
                        panic!("Not expecting calls to WithSchedulingOptions where options.max_parallel_suites is Some()")
                    }
                }
            }
        }
        assert!(
            run_controller.is_some(),
            "Expected a RunController to be present. RunBuilder/Build() may not have been called."
        );
        assert!(suite_events.is_empty(), "Expected AddSuite to be called for all specified suites");
        let mut run_stream =
            run_controller.expect("controller present").into_stream().expect("into stream");

        // Each suite just reports that it started and passed.
        let mut suite_streams = suite_streams
            .into_iter()
            .map(|(mut stream, events)| {
                async move {
                    let mut maybe_events = Some(events);
                    while let Ok(Some(req)) = stream.try_next().await {
                        match req {
                            ftest_manager::SuiteControllerRequest::GetEvents {
                                responder, ..
                            } => {
                                let send_events = maybe_events.take().unwrap_or(vec![]);
                                let _ = responder.send(&mut Ok(send_events));
                            }
                            _ => {
                                // ignore all other requests
                            }
                        }
                    }
                }
            })
            .collect::<FuturesUnordered<_>>();

        let suite_drain_fut = async move { while let Some(_) = suite_streams.next().await {} };

        let run_fut = async move {
            let mut events = Some(run_events);
            while let Ok(Some(req)) = run_stream.try_next().await {
                match req {
                    ftest_manager::RunControllerRequest::GetEvents { responder, .. } => {
                        if events.is_none() {
                            let _ = responder.send(&mut vec![].into_iter());
                            continue;
                        }
                        let events = events.take().unwrap();
                        let _ = responder.send(&mut events.into_iter());
                    }
                    _ => {
                        // ignore all other requests
                    }
                }
            }
        };

        join(suite_drain_fut, run_fut).await;
    }

    struct ParamsForRunTests {
        builder_proxy: ftest_manager::RunBuilderProxy,
        test_params: Vec<TestParams>,
        run_reporter: RunReporter,
    }

    fn create_empty_suite_events() -> Vec<ftest_manager::SuiteEvent> {
        vec![
            ftest_manager::SuiteEvent {
                timestamp: Some(1000),
                payload: Some(ftest_manager::SuiteEventPayload::SuiteStarted(
                    ftest_manager::SuiteStarted,
                )),
                ..ftest_manager::SuiteEvent::EMPTY
            },
            ftest_manager::SuiteEvent {
                timestamp: Some(2000),
                payload: Some(ftest_manager::SuiteEventPayload::SuiteStopped(
                    ftest_manager::SuiteStopped { status: ftest_manager::SuiteStatus::Passed },
                )),
                ..ftest_manager::SuiteEvent::EMPTY
            },
        ]
    }

    async fn call_run_tests(params: ParamsForRunTests) -> Outcome {
        run_tests_and_get_outcome(
            params.builder_proxy,
            params.test_params,
            RunParams {
                timeout_behavior: TimeoutBehavior::Continue,
                timeout_grace_seconds: 0,
                stop_after_failures: None,
                experimental_parallel_execution: None,
                accumulate_debug_data: false,
                log_protocol: None,
                min_severity_logs: None,
                show_full_moniker: false,
            },
            params.run_reporter,
            futures::future::pending(),
        )
        .await
    }

    #[fuchsia::test]
    async fn empty_run_no_events() {
        let (builder_proxy, run_builder_stream) =
            create_proxy_and_stream::<ftest_manager::RunBuilderMarker>()
                .expect("create builder proxy");

        let reporter = InMemoryReporter::new();
        let run_reporter = RunReporter::new(reporter.clone());
        let run_fut =
            call_run_tests(ParamsForRunTests { builder_proxy, test_params: vec![], run_reporter });
        let fake_fut =
            fake_running_all_suites_and_return_run_events(run_builder_stream, hashmap! {}, vec![]);

        assert_eq!(join(run_fut, fake_fut).await.0, Outcome::Passed,);

        let reports = reporter.get_reports();
        assert_eq!(1usize, reports.len());
        assert_eq!(reports[0].id, EntityId::TestRun);
    }

    #[fuchsia::test]
    async fn single_run_no_events() {
        let (builder_proxy, run_builder_stream) =
            create_proxy_and_stream::<ftest_manager::RunBuilderMarker>()
                .expect("create builder proxy");

        let reporter = InMemoryReporter::new();
        let run_reporter = RunReporter::new(reporter.clone());
        let run_fut = call_run_tests(ParamsForRunTests {
            builder_proxy,
            test_params: vec![TestParams {
                test_url: "fuchsia-pkg://fuchsia.com/nothing#meta/nothing.cm".to_string(),
                ..TestParams::default()
            }],
            run_reporter,
        });
        let fake_fut = fake_running_all_suites_and_return_run_events(
            run_builder_stream,
            hashmap! {
                "fuchsia-pkg://fuchsia.com/nothing#meta/nothing.cm" => create_empty_suite_events()
            },
            vec![],
        );

        assert_eq!(join(run_fut, fake_fut).await.0, Outcome::Passed,);

        let reports = reporter.get_reports();
        assert_eq!(2usize, reports.len());
        assert!(reports[0].report.artifacts.is_empty());
        assert!(reports[0].report.directories.is_empty());
        assert!(reports[1].report.artifacts.is_empty());
        assert!(reports[1].report.directories.is_empty());
    }

    #[cfg(target_os = "fuchsia")]
    #[fuchsia::test]
    async fn single_run_custom_directory() {
        let (builder_proxy, run_builder_stream) =
            create_proxy_and_stream::<ftest_manager::RunBuilderMarker>()
                .expect("create builder proxy");

        let reporter = InMemoryReporter::new();
        let run_reporter = RunReporter::new(reporter.clone());
        let run_fut = call_run_tests(ParamsForRunTests {
            builder_proxy,
            test_params: vec![TestParams {
                test_url: "fuchsia-pkg://fuchsia.com/nothing#meta/nothing.cm".to_string(),
                ..TestParams::default()
            }],
            run_reporter,
        });

        let dir = pseudo_directory! {
            "test_file.txt" => read_only_static("Hello, World!"),
        };

        let (directory_client, directory_service) =
            fidl::endpoints::create_endpoints::<fio::DirectoryMarker>().unwrap();
        let scope = ExecutionScope::new();
        dir.open(
            scope,
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
            fio::MODE_TYPE_DIRECTORY,
            vfs::path::Path::dot(),
            ServerEnd::new(directory_service.into_channel()),
        );

        let (_pair_1, pair_2) = zx::EventPair::create().unwrap();

        let events = vec![ftest_manager::RunEvent {
            payload: Some(ftest_manager::RunEventPayload::Artifact(
                ftest_manager::Artifact::Custom(ftest_manager::CustomArtifact {
                    directory_and_token: Some(ftest_manager::DirectoryAndToken {
                        directory: directory_client,
                        token: pair_2,
                    }),
                    ..ftest_manager::CustomArtifact::EMPTY
                }),
            )),
            ..ftest_manager::RunEvent::EMPTY
        }];

        let fake_fut = fake_running_all_suites_and_return_run_events(
            run_builder_stream,
            hashmap! {
                "fuchsia-pkg://fuchsia.com/nothing#meta/nothing.cm" => create_empty_suite_events()
            },
            events,
        );

        assert_eq!(join(run_fut, fake_fut).await.0, Outcome::Passed,);

        let reports = reporter.get_reports();
        assert_eq!(2usize, reports.len());
        let run = reports.iter().find(|e| e.id == EntityId::TestRun).expect("find run report");
        assert_eq!(1usize, run.report.directories.len());
        let dir = &run.report.directories[0];
        let files = dir.1.files.lock();
        assert_eq!(1usize, files.len());
        let (name, file) = &files[0];
        assert_eq!(name.to_string_lossy(), "test_file.txt".to_string());
        assert_eq!(file.get_contents(), b"Hello, World!");
    }

    #[fuchsia::test]
    async fn record_output_after_internal_error() {
        let (builder_proxy, run_builder_stream) =
            create_proxy_and_stream::<ftest_manager::RunBuilderMarker>()
                .expect("create builder proxy");

        let reporter = InMemoryReporter::new();
        let run_reporter = RunReporter::new(reporter.clone());
        let run_fut = call_run_tests(ParamsForRunTests {
            builder_proxy,
            test_params: vec![
                TestParams {
                    test_url: "fuchsia-pkg://fuchsia.com/invalid#meta/invalid.cm".to_string(),
                    ..TestParams::default()
                },
                TestParams {
                    test_url: "fuchsia-pkg://fuchsia.com/nothing#meta/nothing.cm".to_string(),
                    ..TestParams::default()
                },
            ],
            run_reporter,
        });

        let fake_fut = fake_running_all_suites_and_return_run_events(
            run_builder_stream,
            hashmap! {
                // return an internal error from the first test.
                "fuchsia-pkg://fuchsia.com/invalid#meta/invalid.cm" => vec![
                    ftest_manager::SuiteEvent {
                        timestamp: Some(1000),
                        payload: Some(
                            ftest_manager::SuiteEventPayload::SuiteStarted(
                                ftest_manager::SuiteStarted,
                            ),
                        ),
                        ..ftest_manager::SuiteEvent::EMPTY
                    },
                    ftest_manager::SuiteEvent {
                        timestamp: Some(2000),
                        payload: Some(ftest_manager::SuiteEventPayload::SuiteStopped(
                            ftest_manager::SuiteStopped { status: ftest_manager::SuiteStatus::InternalError },
                        )),
                        ..ftest_manager::SuiteEvent::EMPTY
                    },
                ],
                "fuchsia-pkg://fuchsia.com/nothing#meta/nothing.cm" => create_empty_suite_events()
            },
            vec![],
        );

        assert_matches!(join(run_fut, fake_fut).await.0, Outcome::Error { .. });

        let reports = reporter.get_reports();
        assert_eq!(3usize, reports.len());
        let invalid_suite = reports
            .iter()
            .find(|e| e.report.name == "fuchsia-pkg://fuchsia.com/invalid#meta/invalid.cm")
            .expect("find run report");
        assert_eq!(invalid_suite.report.outcome, Some(output::ReportedOutcome::Error));
        assert!(invalid_suite.report.is_finished);

        // The valid suite should not have been started, but finish should've been called so that
        // results get persisted.
        let not_started = reports
            .iter()
            .find(|e| e.report.name == "fuchsia-pkg://fuchsia.com/nothing#meta/nothing.cm")
            .expect("find run report");
        assert!(not_started.report.outcome.is_none());
        assert!(not_started.report.is_finished);

        // The results for the run should also be saved.
        let run = reports.iter().find(|e| e.id == EntityId::TestRun).expect("find run report");
        assert_eq!(run.report.outcome, Some(output::ReportedOutcome::Error));
        assert!(run.report.is_finished);
        assert!(run.report.started_time.is_some());
    }

    #[cfg(target_os = "fuchsia")]
    #[fuchsia::test]
    async fn single_run_debug_data() {
        let (builder_proxy, run_builder_stream) =
            create_proxy_and_stream::<ftest_manager::RunBuilderMarker>()
                .expect("create builder proxy");

        let reporter = InMemoryReporter::new();
        let run_reporter = RunReporter::new(reporter.clone());
        let run_fut = call_run_tests(ParamsForRunTests {
            builder_proxy,
            test_params: vec![TestParams {
                test_url: "fuchsia-pkg://fuchsia.com/nothing#meta/nothing.cm".to_string(),
                ..TestParams::default()
            }],
            run_reporter,
        });

        let dir = pseudo_directory! {
            "test_file.profraw" => read_only_static("Not a real profile"),
        };

        let (file_client, file_service) =
            fidl::endpoints::create_endpoints::<fio::FileMarker>().unwrap();
        let scope = ExecutionScope::new();
        dir.open(
            scope,
            fio::OpenFlags::RIGHT_READABLE,
            fio::MODE_TYPE_FILE,
            vfs::path::Path::validate_and_split("test_file.profraw").unwrap(),
            ServerEnd::new(file_service.into_channel()),
        );

        let (debug_client, debug_service) =
            fidl::endpoints::create_endpoints::<ftest_manager::DebugDataIteratorMarker>().unwrap();
        let debug_data_fut = async move {
            let mut service = debug_service.into_stream().unwrap();
            let mut data = vec![ftest_manager::DebugData {
                name: Some("test_file.profraw".to_string()),
                file: Some(file_client),
                ..ftest_manager::DebugData::EMPTY
            }];
            while let Ok(Some(request)) = service.try_next().await {
                match request {
                    ftest_manager::DebugDataIteratorRequest::GetNext { responder, .. } => {
                        let _ = responder.send(&mut data.drain(0..));
                    }
                }
            }
        };
        let events = vec![ftest_manager::RunEvent {
            payload: Some(ftest_manager::RunEventPayload::Artifact(
                ftest_manager::Artifact::DebugData(debug_client),
            )),
            ..ftest_manager::RunEvent::EMPTY
        }];

        let fake_fut = fake_running_all_suites_and_return_run_events(
            run_builder_stream,
            hashmap! {

                "fuchsia-pkg://fuchsia.com/nothing#meta/nothing.cm" => create_empty_suite_events(),
            },
            events,
        );

        assert_eq!(join3(run_fut, debug_data_fut, fake_fut).await.0, Outcome::Passed);

        let reports = reporter.get_reports();
        assert_eq!(2usize, reports.len());
        let run = reports.iter().find(|e| e.id == EntityId::TestRun).expect("find run report");
        assert_eq!(1usize, run.report.directories.len());
        let dir = &run.report.directories[0];
        let files = dir.1.files.lock();
        assert_eq!(1usize, files.len());
        let (name, file) = &files[0];
        assert_eq!(name.to_string_lossy(), "test_file.profraw".to_string());
        assert_eq!(file.get_contents(), b"Not a real profile");
    }

    async fn fake_parallel_options_server(
        mut stream: ftest_manager::RunBuilderRequestStream,
    ) -> Option<ftest_manager::SchedulingOptions> {
        let mut scheduling_options = None;
        if let Ok(Some(req)) = stream.try_next().await {
            match req {
                ftest_manager::RunBuilderRequest::AddSuite { .. } => {
                    panic!("Not expecting an AddSuite request")
                }
                ftest_manager::RunBuilderRequest::Build { .. } => {
                    panic!("Not expecting a Build request")
                }
                ftest_manager::RunBuilderRequest::WithSchedulingOptions { options, .. } => {
                    scheduling_options = Some(options);
                }
            }
        }
        scheduling_options
    }

    #[fuchsia::test]
    async fn request_scheduling_options_test_parallel() {
        let max_parallel_suites: u16 = 10;
        let expected_max_parallel_suites = Some(max_parallel_suites);

        let (builder_proxy, run_builder_stream) =
            create_proxy_and_stream::<ftest_manager::RunBuilderMarker>()
                .expect("create builder proxy");

        let run_params = RunParams {
            timeout_behavior: TimeoutBehavior::Continue,
            timeout_grace_seconds: 0,
            stop_after_failures: None,
            experimental_parallel_execution: Some(max_parallel_suites),
            accumulate_debug_data: false,
            log_protocol: None,
            min_severity_logs: None,
            show_full_moniker: false,
        };

        let request_parallel_fut = request_scheduling_options(&run_params, &builder_proxy);
        let fake_server_fut = fake_parallel_options_server(run_builder_stream);

        let returned_options = join(request_parallel_fut, fake_server_fut).await.1;
        let max_parallel_suites_received = match returned_options {
            Some(scheduling_options) => scheduling_options.max_parallel_suites,
            None => panic!("Expected scheduling options."),
        };
        assert_eq!(max_parallel_suites_received, expected_max_parallel_suites);
    }

    #[fuchsia::test]
    async fn request_scheduling_options_test_serial() {
        let expected_max_parallel_suites = None;

        let (builder_proxy, run_builder_stream) =
            create_proxy_and_stream::<ftest_manager::RunBuilderMarker>()
                .expect("create builder proxy");

        let run_params = RunParams {
            timeout_behavior: TimeoutBehavior::Continue,
            timeout_grace_seconds: 0,
            stop_after_failures: None,
            experimental_parallel_execution: None,
            accumulate_debug_data: false,
            log_protocol: None,
            min_severity_logs: None,
            show_full_moniker: false,
        };

        let request_parallel_fut = request_scheduling_options(&run_params, &builder_proxy);
        let fake_server_fut = fake_parallel_options_server(run_builder_stream);

        let returned_options = join(request_parallel_fut, fake_server_fut)
            .await
            .1
            .expect("Expected scheduling options.");
        let max_parallel_suites_received = returned_options.max_parallel_suites;
        assert_eq!(max_parallel_suites_received, expected_max_parallel_suites);
    }
}
