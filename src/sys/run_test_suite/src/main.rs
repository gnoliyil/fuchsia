// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {argh::FromArgs, diagnostics_data::Severity, fidl_fuchsia_test_manager::RunBuilderMarker};

#[derive(FromArgs, Default, PartialEq, Eq, Debug)]
/// Entry point for executing tests.
struct Args {
    /// test timeout. Exits with -`ZX_ERR_TIMED_OUT` if the test times out.
    #[argh(option, short = 't')]
    timeout: Option<u32>,

    /// test url. Test should implement `fuchsia.test.Suite` protocol.
    #[argh(positional)]
    test_url: String,

    /// test filter. Glob pattern for matching tests. Can be specified multiple
    /// times to pass in multiple patterns. Tests may be excluded by prepending a
    /// '-' to the glob pattern.
    /// example: --test-filter glob1 --test-filter glob2.
    #[argh(option)]
    test_filter: Vec<String>,

    /// whether to also run tests that have been marked disabled/ignored by the test author.
    #[argh(switch)]
    also_run_disabled_tests: bool,

    /// whether to filter ANSI escape sequences from stdout.
    #[argh(switch)]
    filter_ansi: bool,

    /// continue running unfinished suites if a suite times out.
    /// By default, unfinished suites are immediately terminated if a suite times out.
    /// This option is only relevant when multiple suites are run.
    #[argh(switch)]
    continue_on_timeout: bool,

    /// stop running unfinished suites after the number of provided failures has occurred.
    /// By default, all suites are run to completion if a suite fails.
    #[argh(option)]
    stop_after_failures: Option<u16>,

    /// run test cases in parallel, up to the number provided.
    #[argh(option)]
    parallel: Option<u16>,

    /// number of times to run the test. By default run 1 time.
    /// If an iteration of test times out, no further iterations
    /// would be executed.
    #[argh(option)]
    count: Option<u16>,

    /// when set, only logs with a severity equal to the given one or higher will be printed.
    #[argh(option)]
    min_severity_logs: Option<Severity>,

    /// when set, the test will fail if any log with a higher severity is emitted.
    #[argh(option)]
    max_severity_logs: Option<Severity>,

    #[argh(positional)]
    /// arguments passed to tests following `--`.
    test_args: Vec<String>,
}

#[fuchsia::component]
async fn main() {
    let args = argh::from_env();

    let Args {
        timeout,
        test_url,
        test_filter,
        also_run_disabled_tests,
        continue_on_timeout,
        stop_after_failures,
        parallel,
        count,
        min_severity_logs,
        max_severity_logs,
        test_args,
        filter_ansi,
    } = args;
    let count = count.unwrap_or(1);
    if count == 0 {
        println!("--count should be greater than zero.");
        std::process::exit(1);
    }

    if filter_ansi {
        println!("Note: Filtering out ANSI escape sequences.");
    }

    let test_filters = if test_filter.len() == 0 { None } else { Some(test_filter) };
    let reporter = run_test_suite_lib::output::ShellReporter::new(std::io::stdout());
    let run_reporter = match filter_ansi {
        true => run_test_suite_lib::output::RunReporter::new_ansi_filtered(reporter),
        false => run_test_suite_lib::output::RunReporter::new(reporter),
    };

    let run_params = run_test_suite_lib::RunParams {
        timeout_behavior: match continue_on_timeout {
            false => run_test_suite_lib::TimeoutBehavior::TerminateRemaining,
            true => run_test_suite_lib::TimeoutBehavior::Continue,
        },
        stop_after_failures: match stop_after_failures.map(std::num::NonZeroU16::new) {
            None => None,
            Some(None) => {
                println!("--stop-after-failures should be greater than zero.");
                std::process::exit(1);
            }
            Some(Some(stop_after)) => Some(stop_after),
        },
    };

    match run_test_suite_lib::run_tests_and_get_outcome(
        fuchsia_component::client::connect_to_protocol::<RunBuilderMarker>()
            .expect("connecting to RunBuilderProxy"),
        vec![
            run_test_suite_lib::TestParams {
                test_url,
                timeout: timeout.and_then(std::num::NonZeroU32::new),
                test_filters,
                also_run_disabled_tests,
                parallel,
                test_args,
                max_severity_logs
            };
            count as usize
        ],
        run_params,
        min_severity_logs,
        run_reporter,
        futures::future::pending(),
    )
    .await
    {
        run_test_suite_lib::Outcome::Passed => {}
        run_test_suite_lib::Outcome::Timedout => {
            std::process::exit(-fuchsia_zircon::Status::TIMED_OUT.into_raw());
        }
        run_test_suite_lib::Outcome::Failed
        | run_test_suite_lib::Outcome::Cancelled
        | run_test_suite_lib::Outcome::DidNotFinish
        | run_test_suite_lib::Outcome::Inconclusive
        | run_test_suite_lib::Outcome::Error { .. } => {
            std::process::exit(1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    // As we have custom parsing when user passes "--", making sure everything works fine.
    fn test_args() {
        let url = "foo.cm";
        let mut expected_args = Args { test_url: url.to_string(), ..Default::default() };
        expected_args.test_url = url.to_string();

        let args = Args::from_args(&["cmd"], &[url]).unwrap();
        assert_eq!(args, expected_args);

        let args = Args::from_args(&["cmd"], &[url, "--"]).unwrap();
        expected_args.test_args = vec![];
        assert_eq!(args, expected_args);

        // make sure we can parse --help flag when user passes "--"
        let early_exit = Args::from_args(&["cmd"], &[url, "--help", "--"]).unwrap_err();
        assert_eq!(early_exit.status, Ok(()));

        // make sure we can parse --help flag without "--"
        let early_exit = Args::from_args(&["cmd"], &[url, "--help"]).unwrap_err();
        assert_eq!(early_exit.status, Ok(()));

        // make sure we can catch arg errors when user passes "--"
        let early_exit = Args::from_args(&["cmd"], &[url, "--timeout", "a", "--"]).unwrap_err();
        assert_eq!(early_exit.status, Err(()));

        // make sure we can catch arg errors without "--"
        let early_exit = Args::from_args(&["cmd"], &[url, "--timeout", "a"]).unwrap_err();
        assert_eq!(early_exit.status, Err(()));

        // make sure we can parse args when user passes "--"
        let args = Args::from_args(&["cmd"], &[url, "--timeout", "2", "--"]).unwrap();
        expected_args.timeout = Some(2);
        expected_args.test_args = vec![];
        assert_eq!(args, expected_args);

        // make sure we can parse args without "--"
        let args = Args::from_args(&["cmd"], &[url, "--timeout", "2"]).unwrap();
        assert_eq!(args, expected_args);

        // make sure we can parse args after "--"
        let args = Args::from_args(
            &["cmd"],
            &[url, "--timeout", "2", "--", "--arg1", "some_random_str", "-arg2"],
        )
        .unwrap();
        expected_args.test_args =
            vec!["--arg1".to_owned(), "some_random_str".to_owned(), "-arg2".to_owned()];
        assert_eq!(args, expected_args);

        // Args::from_args works with multiple "--"
        let args = Args::from_args(
            &["cmd"],
            &[url, "--timeout", "2", "--", "--", "--arg1", "some_random_str", "--", "-arg2"],
        )
        .unwrap();
        expected_args.test_args = vec![
            "--".to_owned(),
            "--arg1".to_owned(),
            "some_random_str".to_owned(),
            "--".to_owned(),
            "-arg2".to_owned(),
        ];
        assert_eq!(args, expected_args);
    }
}
