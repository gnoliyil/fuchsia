// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::helpers::*,
    anyhow::{anyhow, Error},
    fidl::endpoints::create_proxy,
    fidl::HandleBased,
    fidl_fuchsia_component_decl as fdecl, fidl_fuchsia_component_runner as frunner,
    fidl_fuchsia_data as fdata, fidl_fuchsia_process as fprocess,
    fidl_fuchsia_test::{self as ftest},
    fuchsia_async as fasync, fuchsia_runtime as fruntime, fuchsia_zircon as zx,
    fuchsia_zircon::sys::ZX_CHANNEL_MAX_MSG_BYTES,
    futures::io::AsyncReadExt,
    futures::TryStreamExt,
    rand::Rng,
    runner::component::ComponentNamespace,
    rust_measure_tape_for_case::Measurable as _,
    std::convert::TryInto,
    std::num::NonZeroUsize,
    test_runners_lib::elf::{KernelError, SuiteServerError},
    test_runners_lib::logs::{LogError, LogStreamReader, LoggerStream, SocketLogWriter},
};

const NEWLINE: u8 = b'\n';
const PREFIXES_TO_EXCLUDE: [&[u8]; 10] = [
    "Note: Google Test filter".as_bytes(),
    " 1 FAILED TEST".as_bytes(),
    "  YOU HAVE 1 DISABLED TEST".as_bytes(),
    "[==========]".as_bytes(),
    "[----------]".as_bytes(),
    "[ RUN      ]".as_bytes(),
    "[  PASSED  ]".as_bytes(),
    "[  FAILED  ]".as_bytes(),
    "[  SKIPPED ]".as_bytes(),
    "[       OK ]".as_bytes(),
];
const SOCKET_BUFFER_SIZE: usize = 4096;

/// Returns true if the program arguments specify the tests are gtests.
pub fn is_gtest(program: &fdata::Dictionary) -> Result<bool, Error> {
    // The program argument that specifies if the test is a gtest.
    const GTEST_KEY: &str = "is_gtest";
    runner::get_bool(program, GTEST_KEY).map_err(|_| anyhow!("Couldn't read program."))
}

/// Runs the test component with `--gunit_list_tests` and returns the parsed test cases from stdout
/// in response to `ftest::CaseIteratorRequest::GetNext`.
pub async fn handle_case_iterator_for_gtests(
    test_url: &str,
    mut program: Option<fdata::Dictionary>,
    namespace: &ComponentNamespace,
    starnix_kernel: frunner::ComponentRunnerProxy,
    mut stream: ftest::CaseIteratorRequestStream,
) -> Result<(), Error> {
    const LIST_TESTS_ARG: &str = "--gunit_list_tests";

    let (test_stdout, stdout_client) = zx::Socket::create(zx::SocketOpts::STREAM).unwrap();
    let stdout_handle_info = fprocess::HandleInfo {
        handle: test_stdout.into_handle(),
        id: fruntime::HandleInfo::new(fruntime::HandleType::FileDescriptor, 1).as_raw(),
    };

    let (_component_controller, component_controller_server_end) =
        create_proxy::<frunner::ComponentControllerMarker>()?;
    let ns = Some(ComponentNamespace::try_into(namespace.clone())?);
    let numbered_handles = Some(vec![stdout_handle_info]);
    let (outgoing_dir, _outgoing_dir_server) = zx::Channel::create();

    // Replace the program args with `gunit_list_tests`.
    replace_program_args(vec![LIST_TESTS_ARG.to_string()], program.as_mut().expect("No program."));
    let start_info = frunner::ComponentStartInfo {
        resolved_url: Some(test_url.to_string()),
        program,
        ns,
        outgoing_dir: Some(outgoing_dir.into()),
        runtime_dir: None,
        numbered_handles,
        ..frunner::ComponentStartInfo::EMPTY
    };

    starnix_kernel.start(start_info, component_controller_server_end)?;

    // Parse tests out of logs from stdout.
    let logger_stream = LoggerStream::new(fidl::Socket::from_handle(stdout_client.into_handle()))?;
    let log_reader = LogStreamReader::new(logger_stream);
    let logs = log_reader.get_logs().await?;
    let mut iter = parse_gtests(&logs).into_iter();

    while let Some(event) = stream.try_next().await? {
        match event {
            ftest::CaseIteratorRequest::GetNext { responder } => {
                // Paginate cases
                // Page overhead of message header + vector
                let mut bytes_used: usize = 32;
                let mut case_count = 0;
                for case in iter.clone() {
                    bytes_used += case.measure().num_bytes;
                    if bytes_used > ZX_CHANNEL_MAX_MSG_BYTES as usize {
                        break;
                    }
                    case_count += 1;
                }
                responder
                    .send(&mut iter.by_ref().take(case_count))
                    .map_err(SuiteServerError::Response)?;
            }
        }
    }

    Ok(())
}

/// Runs a gtest case associated with a single `ftest::SuiteRequest::Run` request.
///
/// Running the test component is delegated to an instance of the starnix kernel.
/// stdout logs are filtered before they're reported to the test framework.
///
/// # Parameters
/// - `tests`: The tests that are to be run. Each test executes an independent run of the test
/// component.
/// - `test_url`: The URL of the test component.
/// - `program`: The program data associated with the runner request for the test component.
/// - `run_listener_proxy`: The listener proxy for the test run.
/// - `namespace`: The incoming namespace to provide to the test component.
pub async fn run_gtest_case(
    test: ftest::Invocation,
    test_url: &str,
    mut program: Option<fdata::Dictionary>,
    run_listener_proxy: &ftest::RunListenerProxy,
    namespace: &ComponentNamespace,
) -> Result<(), Error> {
    // Start a starnix kernel.
    let kernel_name = format!("starnix-kernel-{}", rand::thread_rng().gen::<u64>());
    let (starnix_kernel, realm) = instantiate_kernel_in_realm(&namespace, &kernel_name).await?;

    let (case_listener_proxy, case_listener) = create_proxy::<ftest::CaseListenerMarker>()?;
    let (numbered_handles, stdout_client, stderr_client) = create_numbered_handles();

    // Create additional sockets to filter the test's stdout logs before reporting them to
    // the test framework.
    let test_name = test.name.clone().expect("No test name.");
    let (test_framework_stdout, framework_stdout_client) =
        zx::Socket::create(zx::SocketOpts::STREAM).unwrap();

    run_listener_proxy.on_test_case_started(
        test,
        ftest::StdHandles {
            out: Some(framework_stdout_client),
            err: Some(stderr_client),
            ..ftest::StdHandles::EMPTY
        },
        case_listener,
    )?;

    // Update program arguments to only run the test case.
    append_program_args(
        vec![format!("{}={}", "--gunit_filter", test_name)],
        program.as_mut().expect("No program."),
    );

    // Start the test component.
    let component_controller =
        start_test_component(test_url, program, namespace, numbered_handles, &starnix_kernel)?;

    // Filter stdout logs to reduce spam.
    let test_framework_stdout = fasync::Socket::from_socket(test_framework_stdout)
        .map_err(KernelError::SocketToAsync)
        .unwrap();
    let mut test_framework_stdout = SocketLogWriter::new(test_framework_stdout);
    filter_and_write_logs(fasync::Socket::from_socket(stdout_client)?, &mut test_framework_stdout)
        .await?;

    // Read and report the result.
    let result = read_result(component_controller.take_event_stream()).await;
    case_listener_proxy.finished(result)?;

    // Cleanup the starnix kernel.
    realm
        .destroy_child(&mut fdecl::ChildRef {
            name: kernel_name.to_string(),
            collection: Some(RUNNERS_COLLECTION.into()),
        })
        .await?
        .map_err(|e| anyhow::anyhow!("failed to destory runner child: {:?}", e))?;

    Ok(())
}

/// Parses the bytes `tests` into gtest cases.
fn parse_gtests(tests: &[u8]) -> Vec<ftest::Case> {
    let test_string = String::from_utf8_lossy(tests);
    let mut testcases = vec![];
    let mut testsuite = "";
    for test in test_string.split('\n') {
        let test = test.trim().split(' ').next();

        match test {
            Some(name) if !name.is_empty() => {
                // --gunit_list_tests outputs test names in multiple lines with test suites
                // separated from test cases.
                // A regular test such as TestSuite.TestCase will look like
                // TestSuite.
                //     TestCase
                // and a parameterized test TestSuites/TestSuite.TestCases/TestCase will look like
                // TestSuites/TestSuite.
                //     TestCases/TestCase
                // Construct full test names by joining test suites with test cases.
                if name.ends_with('.') {
                    testsuite = name;
                } else {
                    testcases.push(ftest::Case {
                        name: Some(format!("{}{}", testsuite, name)),
                        enabled: None,
                        ..ftest::Case::EMPTY
                    });
                }
            }
            _ => continue,
        }
    }

    testcases
}

/// Filters logs from `socket` and writes them to `writer`.
async fn filter_and_write_logs(
    mut socket: fasync::Socket,
    writer: &mut SocketLogWriter,
) -> Result<(), LogError> {
    let mut last_line_excluded = false;
    let mut socket_buf = vec![0u8; SOCKET_BUFFER_SIZE];
    while let Some(bytes_read) =
        NonZeroUsize::new(socket.read(&mut socket_buf[..]).await.map_err(LogError::Read)?)
    {
        let mut bytes = &socket_buf[..bytes_read.get()];

        // Avoid printing trailing empty line
        if *bytes.last().unwrap() == NEWLINE {
            bytes = &bytes[..bytes.len() - 1];
        }

        let mut iter = bytes.split(|&x| x == NEWLINE);

        while let Some(line) = iter.next() {
            if line.len() == 0 && last_line_excluded {
                // sometimes excluded lines print two newlines, we don't want to print blank
                // output to user's screen.
                continue;
            }
            last_line_excluded = PREFIXES_TO_EXCLUDE.iter().any(|p| line.starts_with(p));

            if !last_line_excluded {
                let line = [line, &[NEWLINE]].concat();
                writer.write(&line).await?;
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[fuchsia::test]
    fn test_parse_test_cases() {
        let stdout =
            "TestSuite1.\n  TestCase1\n  TestCase2\nTestSuite2.\n  TestCase3\n  TestCase4\n"
                .as_bytes();

        assert_eq!(
            parse_gtests(stdout),
            vec![
                ftest::Case {
                    name: Some("TestSuite1.TestCase1".to_string()),
                    enabled: None,
                    ..ftest::Case::EMPTY
                },
                ftest::Case {
                    name: Some("TestSuite1.TestCase2".to_string()),
                    enabled: None,
                    ..ftest::Case::EMPTY
                },
                ftest::Case {
                    name: Some("TestSuite2.TestCase3".to_string()),
                    enabled: None,
                    ..ftest::Case::EMPTY
                },
                ftest::Case {
                    name: Some("TestSuite2.TestCase4".to_string()),
                    enabled: None,
                    ..ftest::Case::EMPTY
                },
            ]
        )
    }

    #[fuchsia::test]
    fn test_parse_parameterized_test_cases() {
        let stdout =
"AllTestSuites/TestSuite1.\n
  TestCase1/0  # GetParam() = 96-byte object <51-00 00-00 00-00 00-00 46-00 00-00 00-00 00-00 50-D2 DA-EC 50-00 00-00 01-00 00-00 01-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 70-47 0F-80 DD-03 00-00 01-B7 48-7E 01-00 00-00 01-00 00-00 00-00 00-00 00-00 00-00 00-00 ...\n
  TestCase1/1  # GetParam() = 96-byte object <61-00 00-00 00-00 00-00 53-00 00-00 00-00 00-00 00-A3 DA-FC 50-00 00-00 01-00 00-00 01-08 00-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 70-47 0F-80 DD-03 00-00 01-B7 48-7E 01-00 00-00 01-08 00-00 00-00 00-00 00-00 00-00 00-00 ...\n
  TestCase2/0  # GetParam() = 96-byte object <51-00 00-00 00-00 00-00 46-00 00-00 00-00 00-00 70-EE DA-EC 50-00 00-00 01-00 00-00 01-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 70-47 0F-80 DD-03 00-00 01-B7 48-7E 01-00 00-00 01-00 00-00 00-00 00-00 00-00 00-00 00-00 ...\n
  TestCase2/1  # GetParam() = 96-byte object <61-00 00-00 00-00 00-00 53-00 00-00 00-00 00-00 10-91 DA-FC 50-00 00-00 01-00 00-00 01-08 00-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 70-47 0F-80 DD-03 00-00 01-B7 48-7E 01-00 00-00 01-08 00-00 00-00 00-00 00-00 00-00 00-00 ...\n
AllTestSuites/TestSuite2.\n
  TestCase3/0  # GetParam() = 96-byte object <51-00 00-00 00-00 00-00 46-00 00-00 00-00 00-00 D0-26 DC-EC 50-00 00-00 01-00 00-00 01-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 70-47 0F-80 DD-03 00-00 01-B7 48-7E 01-00 00-00 01-00 00-00 00-00 00-00 00-00 00-00 00-00 ...\n
  TestCase3/1  # GetParam() = 96-byte object <61-00 00-00 00-00 00-00 53-00 00-00 00-00 00-00 70-3D DB-FC 50-00 00-00 01-00 00-00 01-08 00-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 00-00 70-47 0F-80 DD-03 00-00 01-B7 48-7E 01-00 00-00 01-08 00-00 00-00 00-00 00-00 00-00 00-00 ...\n
".as_bytes();

        assert_eq!(
            parse_gtests(stdout),
            vec![
                ftest::Case {
                    name: Some("AllTestSuites/TestSuite1.TestCase1/0".to_string()),
                    enabled: None,
                    ..ftest::Case::EMPTY
                },
                ftest::Case {
                    name: Some("AllTestSuites/TestSuite1.TestCase1/1".to_string()),
                    enabled: None,
                    ..ftest::Case::EMPTY
                },
                ftest::Case {
                    name: Some("AllTestSuites/TestSuite1.TestCase2/0".to_string()),
                    enabled: None,
                    ..ftest::Case::EMPTY
                },
                ftest::Case {
                    name: Some("AllTestSuites/TestSuite1.TestCase2/1".to_string()),
                    enabled: None,
                    ..ftest::Case::EMPTY
                },
                ftest::Case {
                    name: Some("AllTestSuites/TestSuite2.TestCase3/0".to_string()),
                    enabled: None,
                    ..ftest::Case::EMPTY
                },
                ftest::Case {
                    name: Some("AllTestSuites/TestSuite2.TestCase3/1".to_string()),
                    enabled: None,
                    ..ftest::Case::EMPTY
                },
            ]
        )
    }
}
