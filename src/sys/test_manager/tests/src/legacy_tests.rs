// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    fidl_fuchsia_test_manager as ftest_manager,
    ftest_manager::{CaseStatus, RunOptions, SuiteStatus},
    fuchsia_async as fasync,
    fuchsia_component::client,
    pretty_assertions::assert_eq,
    test_manager_test_lib::{
        collect_suite_events, default_run_option, GroupRunEventByTestCase, RunEvent, TestBuilder,
    },
};

async fn run_single_test(
    test_url: &str,
    run_options: RunOptions,
) -> Result<(Vec<RunEvent>, Vec<String>), Error> {
    let builder = TestBuilder::new(
        client::connect_to_protocol::<ftest_manager::RunBuilderMarker>()
            .context("cannot connect to run builder proxy")?,
    );
    let suite_instance =
        builder.add_suite(test_url, run_options).await.context("Cannot create suite instance")?;
    let builder_run = fasync::Task::spawn(async move { builder.run().await });
    let ret = collect_suite_events(suite_instance).await;
    builder_run.await.context("builder execution failed")?;
    ret
}

// TODO(fxbug.dev/115493): Disabled due to flake
#[ignore]
#[fuchsia::test]
async fn launch_v1_v2_bridge_test() {
    let test_url =
        "fuchsia-pkg://fuchsia.com/test_manager_legacy_test#meta/v2_test_runs_v1_component.cm";

    let (events, logs) = run_single_test(test_url, default_run_option()).await.unwrap();
    let events = events.into_iter().group_by_test_case_unordered();

    let expected_events = vec![
        RunEvent::suite_started(),
        RunEvent::case_found("launch_and_test_v1_component"),
        RunEvent::case_started("launch_and_test_v1_component"),
        RunEvent::case_stopped("launch_and_test_v1_component", CaseStatus::Passed),
        RunEvent::case_finished("launch_and_test_v1_component"),
        RunEvent::case_found("launch_v1_logging_component"),
        RunEvent::case_started("launch_v1_logging_component"),
        RunEvent::case_stopped("launch_v1_logging_component", CaseStatus::Passed),
        RunEvent::case_found("test_debug_data_for_v1_component"),
        RunEvent::case_started("test_debug_data_for_v1_component"),
        RunEvent::case_stopped("test_debug_data_for_v1_component", CaseStatus::Passed),
        RunEvent::case_finished("test_debug_data_for_v1_component"),
        RunEvent::case_finished("launch_v1_logging_component"),
        RunEvent::case_found("enclosing_env_services"),
        RunEvent::case_started("enclosing_env_services"),
        RunEvent::case_stopped("enclosing_env_services", CaseStatus::Passed),
        RunEvent::case_finished("enclosing_env_services"),
        RunEvent::suite_stopped(SuiteStatus::Passed),
    ]
    .into_iter()
    .group_by_test_case_unordered();
    assert_eq!(&expected_events, &events);

    // logged by child v1 component.
    // fuchsia.debugdata.Publisher may be unavailable due to security policy, ignore those logs
    let logs: Vec<_> = logs
        .into_iter()
        .filter(|log| {
            !log.starts_with("Required protocol `fuchsia.debugdata.Publisher` was not available")
        })
        .collect();
    assert_eq!(
        logs,
        vec![
            "Logging initialized".to_string(),
            "my debug message.".to_string(),
            "my info message.".to_string(),
            "my warn message.".to_string()
        ]
    );
}
