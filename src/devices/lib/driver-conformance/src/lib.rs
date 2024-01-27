// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod args;
pub mod parser;
pub mod results;

use {
    anyhow::{anyhow, Result},
    args::{ConformanceCommand, ConformanceSubCommand, TestCommand},
    driver_connector::DriverConnector,
    errors::ffx_bail,
    ffx_config, fidl_fuchsia_driver_development as fdd, fidl_fuchsia_test_manager as ftm,
    fuchsia_driver_dev::{
        get_devices_by_driver, get_driver_by_device, get_driver_by_filter, Device,
    },
    futures::FutureExt,
    parser::{FilterTests, ValidateAgainstMetadata},
    serde_json,
    signal_hook::{
        consts::signal::{SIGINT, SIGTERM},
        iterator::Signals,
    },
    std::collections::HashSet,
    std::fs,
    std::io::stdout,
    std::path::{Path, PathBuf},
};

impl From<parser::TestInfo> for run_test_suite_lib::TestParams {
    fn from(item: parser::TestInfo) -> Self {
        run_test_suite_lib::TestParams {
            test_url: item.url.to_string(),
            ..run_test_suite_lib::TestParams::default()
        }
    }
}

fn process_test_list(
    tests: HashSet<parser::TestInfo>,
) -> Result<Vec<run_test_suite_lib::TestParams>> {
    let mut ret: Vec<run_test_suite_lib::TestParams> = tests.into_iter().map(Into::into).collect();
    // Sort so the order is deterministic.
    ret.sort_by(|a, b| a.test_url.cmp(&b.test_url));
    Ok(ret)
}

fn print_test_list(tests: HashSet<parser::TestInfo>) {
    let mut tests: Vec<parser::TestInfo> = tests.into_iter().collect();
    // Sort so the order is deterministic.
    tests.sort_by(|a, b| a.url.cmp(&b.url));
    for test in tests {
        println!("{}", test.url);
    }
}

fn get_name(cmd: &TestCommand) -> String {
    if let Some(name) = &cmd.submission_name {
        name.to_string()
    } else if let Some(name) = &cmd.device {
        name.to_string()
    } else if let Some(name) = &cmd.driver {
        name.to_string()
    } else {
        "Default Name".to_string()
    }
}

/// Calls on the `ffx test` library to run the given set of tests.
async fn run_tests(
    tests: HashSet<parser::TestInfo>,
    builder_proxy: ftm::RunBuilderProxy,
    output_dir: &Path,
) -> Result<run_test_suite_lib::Outcome> {
    let writer = Box::new(stdout());
    let (cancel_sender, cancel_receiver) = futures::channel::oneshot::channel::<()>();

    let mut signals = Signals::new(&[SIGINT, SIGTERM]).unwrap();
    // signals.forever() is blocking, so we need to spawn a thread rather than use async.
    std::thread::spawn(move || {
        for signal in signals.forever() {
            match signal {
                SIGINT | SIGTERM => {
                    let _ = cancel_sender.send(());
                    break;
                }
                _ => unreachable!(),
            }
        }
    });

    let test_params = process_test_list(tests)?;
    let reporter_options =
        Some(run_test_suite_lib::DirectoryReporterOptions { root_path: output_dir.to_path_buf() });

    Ok(run_test_suite_lib::run_tests_and_get_outcome(
        run_test_suite_lib::SingleRunConnector::new(builder_proxy),
        test_params,
        run_test_suite_lib::RunParams {
            timeout_behavior: run_test_suite_lib::TimeoutBehavior::Continue,
            timeout_grace_seconds: 0,
            suite_start_timeout_seconds: None,
            stop_after_failures: None,
            experimental_parallel_execution: None,
            accumulate_debug_data: false,
            log_protocol: None,
            min_severity_logs: None,
            show_full_moniker: false,
        },
        run_test_suite_lib::create_reporter(false, reporter_options, writer)?,
        cancel_receiver.map(|_| ()),
    )
    .await)
}

fn validate_license_dir(path: &Option<PathBuf>) -> Result<()> {
    if let Some(dir) = path {
        if !dir.exists() {
            return Err(anyhow!("{} does not exist.", &dir.display()));
        }
        if !dir.is_dir() {
            return Err(anyhow!("{} is not a directory.", &dir.display()));
        }
        let license_path = dir.join("LICENSE");
        if !license_path.exists() || !license_path.is_file() {
            return Err(anyhow!("{} is a required file.", &license_path.display()));
        }
    } else {
        return Err(anyhow!("--licenses is is required if generating a submission package."));
    }
    Ok(())
}

/// Collection of various trivial flag validation.
fn validate_test_flags(cmd: &TestCommand) -> Result<()> {
    if let (Some(_), Some(_)) = (&cmd.device, &cmd.driver) {
        ffx_bail!("Either --device or --driver is required, but not both.");
    }
    if let (None, None) = (&cmd.device, &cmd.driver) {
        ffx_bail!("Either --device or --driver is required.");
    }

    if cmd.automated_only && cmd.manual_only {
        ffx_bail!("Either --automated-only or --manual-only can be provided, but not both.");
    }

    match (&cmd.driver_source, &cmd.source_host_type) {
        (Some(_), None) => {
            ffx_bail!("A provided --driver-source must have an accompanying --source-host-type.")
        }
        (None, Some(_)) => {
            ffx_bail!("A provided --source-host-type must have an accompanying --driver-source.")
        }
        _ => {}
    }

    match (&cmd.generate_submission, &cmd.package_output_dir) {
        (true, _) | (_, Some(_)) => validate_license_dir(&cmd.licenses),
        _ => Ok(()),
    }?;

    if cmd.tests.is_some() && (cmd.categories.is_some() || cmd.types.is_some()) {
        ffx_bail!("--categories and --types are not allowed when --tests is provided.");
    }
    Ok(())
}

fn validate_tests_are_subset(
    all_available: &HashSet<parser::TestInfo>,
    custom_set: &HashSet<parser::TestInfo>,
) -> Result<()> {
    if custom_set.is_subset(&all_available) {
        Ok(())
    } else {
        Err(anyhow!("Custom set of tests contains at least one test that is incompatible with the given driver."))
    }
}

/// Parse the FHCP metadata.
///
/// Currently is requiring the --metadata-path argument, but in the future we might not
/// have that requirement. This is why the TestCommand is passed in to allow for flexibility
/// with handling other possibilities in the future.
fn parse_metadata(cmd: &TestCommand) -> Result<parser::TestMetadata> {
    let metadata_str = match &cmd.metadata_path {
        Some(metadata_path) => match fs::read_to_string(&metadata_path) {
            Ok(v) => v,
            Err(e) => ffx_bail!("Unable to parse {}. {}", &metadata_path.display(), e),
        },
        None => ffx_bail!("The --metadata-path argument is required for now."),
    };
    match serde_json::from_str(&metadata_str) {
        Ok(val) => Ok(val),
        Err(e) => Err(e.into()),
    }
}

/// Find the driver and bound devices.
async fn get_driver_and_devices(
    cmd: &TestCommand,
    driver_connector: &dyn DriverConnector,
) -> Result<(Box<fdd::DriverInfo>, Box<Vec<Device>>)> {
    let mut driver_info: Option<Box<fdd::DriverInfo>> = None;
    let mut device_list: Option<Box<Vec<Device>>> = None;
    let driver_service = driver_connector.get_driver_development_proxy(false).await?;
    if let Some(device) = &cmd.device {
        driver_info = Some(Box::new(get_driver_by_device(&device, &driver_service).await?));
    }

    if let Some(driver) = &cmd.driver {
        driver_info = Some(Box::new(get_driver_by_filter(&driver, &driver_service).await?));
        device_list = Some(Box::new(get_devices_by_driver(&driver, &driver_service).await?));
    } else if let Some(driver) = &driver_info {
        if let Some(driver_libname) = &driver.libname {
            device_list =
                Some(Box::new(get_devices_by_driver(&driver_libname, &driver_service).await?));
        }
    }

    match (driver_info, device_list) {
        (Some(driver), Some(devices)) => {
            println!(
                "Identified driver {} associated with {} device(s).",
                driver.libname.as_ref().unwrap_or(&"".to_string()),
                devices.len()
            );
            return Ok((driver, devices));
        }
        (Some(_), None) => {
            ffx_bail!("We were unable to resolve the devices for the given driver.")
        }
        (None, Some(_)) => {
            ffx_bail!("We were unable to resolve the driver for the given device.")
        }
        _ => ffx_bail!("We were unable to resolve any devices or drivers."),
    }
}

/// Filter the test list down further, if necessary.
fn filter_tests(
    cmd: &TestCommand,
    metadata: &parser::TestMetadata,
    mut tests: HashSet<parser::TestInfo>,
) -> Result<HashSet<parser::TestInfo>> {
    if cmd.automated_only {
        tests.retain(|x| x.is_automated);
    } else if cmd.manual_only {
        tests.retain(|x| !x.is_automated);
    }

    if let Some(categories) = &cmd.categories {
        let category_tests = metadata.tests_by_device_category(&categories.0[..])?;
        tests.retain(|item| category_tests.contains(item));
    }
    Ok(tests)
}

/// Generate submission package.
///
/// For submitting to the Fuchsia Hardware Portal.
fn generate_submission(
    cmd: &TestCommand,
    report_dir: &Path,
    output_dir: &Path,
    name: &String,
    version: &String,
) -> Result<PathBuf> {
    let mut manifest = results::SuperjetManifest::new(name.to_string(), version.to_string());
    manifest.from_results(&report_dir)?;
    if let Some(binary) = &cmd.x64_package {
        manifest.add_driver_binary(binary, results::ProcessorArch::X64)?;
    }
    if let Some(binary) = &cmd.arm64_package {
        manifest.add_driver_binary(binary, results::ProcessorArch::Arm64)?;
    }
    if let (Some(source), Some(host)) = (&cmd.driver_source, &cmd.source_host_type) {
        manifest.add_driver_source(&source, host.clone())?;
    }
    if let Some(licenses) = &cmd.licenses {
        manifest.add_licenses(&licenses)?;
    }
    let output_path = output_dir.join("package.tar.gz");
    manifest.make_tar(&output_path)?;
    Ok(output_path)
}

/// Entry-point for the command `ffx driver conformance`.
pub async fn conformance(
    cmd: ConformanceCommand,
    driver_connector: &dyn DriverConnector,
) -> Result<()> {
    match ffx_config::get("test.driver_conformance_testing").await {
        Ok(true) => {}
        Ok(false) | Err(_) => {
            ffx_bail!(
                "Driver conformance testing is experimental and is subject to breaking changes. \
            To enable driver conformance testing, run \
            'ffx config set test.driver_conformance_testing true'"
            )
        }
    }
    match cmd.subcommand {
        ConformanceSubCommand::Test(subcmd) => {
            validate_test_flags(&subcmd)?;

            let metadata = parse_metadata(&subcmd)?;
            if let Some(categories) = &subcmd.categories {
                metadata.validate_device_categories(&categories.0[..])?;
            }

            let temp_dir_obj = tempfile::TempDir::new()?;
            let temp_output_path = temp_dir_obj.path();

            let mut user_output_dir: Option<PathBuf> = None;
            if let Some(output_dir) = &subcmd.package_output_dir {
                user_output_dir = Some(output_dir.0.as_path().to_path_buf());
            } else if subcmd.generate_submission {
                user_output_dir = Some(std::env::current_dir()?.as_path().to_path_buf());
            }

            let mut version: String = "0.0.0".to_string();
            if let Some(v) = &subcmd.version {
                version = v.0.to_string();
            }

            // _device_list will be used when we add support for running specific tests on specific
            // devices.
            let (driver_info, _device_list) =
                get_driver_and_devices(&subcmd, driver_connector).await?;

            // The superset of possible tests to run against the given driver.
            let tests_for_driver = metadata.tests_by_driver(&driver_info)?;

            // If --tests was given, verify that it is a subset of possible tests and then skip
            // any other filtering.
            let tests = if let Some(custom_list) = &subcmd.tests {
                let custom = metadata.tests_by_url(&custom_list.0[..]).unwrap();
                validate_tests_are_subset(&tests_for_driver, &custom)?;
                custom
            } else {
                filter_tests(&subcmd, &metadata, tests_for_driver)?
            };

            if tests.is_empty() {
                println!("\nThere were no tests to run for the given command.");
            } else if subcmd.preview {
                println!("\nThis command would run the following tests:");
                print_test_list(tests);
            } else {
                // We are ignoring the return value because we will read the results from
                // the report generated via `run_test_suite_lib::create_reporter()`.
                let _ = run_tests(
                    tests,
                    driver_connector.get_run_builder_proxy().await?,
                    temp_output_path,
                )
                .await;

                if let Some(output) = user_output_dir {
                    let file_path = generate_submission(
                        &subcmd,
                        &temp_output_path,
                        &output,
                        &get_name(&subcmd),
                        &version,
                    )?;
                    println!("\nSubmission package has been generated at: {}", file_path.display());
                }
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod test {
    use {
        super::*,
        flate2::read::GzDecoder,
        std::collections::HashMap,
        std::fs::File,
        std::io::{Read, Write},
        tar::Archive,
    };

    #[test]
    fn test_process_test_list() {
        let obj_a = parser::TestInfo {
            url: "abc".to_string(),
            test_types: Box::new([]),
            device_categories: Box::new([]),
            is_automated: true,
        };
        let obj_b = parser::TestInfo {
            url: "def".to_string(),
            test_types: Box::new([]),
            device_categories: Box::new([]),
            is_automated: true,
        };
        let obj_c = parser::TestInfo {
            url: "ghi".to_string(),
            test_types: Box::new([]),
            device_categories: Box::new([]),
            is_automated: true,
        };
        let single = process_test_list(HashSet::from([obj_a.clone()])).unwrap();
        assert_eq!(single.len(), 1);
        assert!(
            single.contains(&run_test_suite_lib::TestParams {
                test_url: "abc".to_string(),
                ..run_test_suite_lib::TestParams::default()
            }),
            "Did not find a TestParams instance with test_url: abc"
        );

        let multiple =
            process_test_list(HashSet::from([obj_a.clone(), obj_b.clone(), obj_c.clone()]))
                .unwrap();
        assert_eq!(multiple.len(), 3);
        for e in vec!["abc", "def", "ghi"] {
            assert!(
                multiple.contains(&run_test_suite_lib::TestParams {
                    test_url: e.to_string(),
                    ..run_test_suite_lib::TestParams::default()
                }),
                "Did not find a TestParams instance with test_url: {}",
                e.to_string()
            );
        }

        let obj_url_a = parser::TestInfo {
            url: "fuchsia-pkg://fuchsia.com/fake-test#meta/fake-test.cm".to_string(),
            test_types: Box::new([]),
            device_categories: Box::new([]),
            is_automated: true,
        };
        let obj_url_b = parser::TestInfo {
            url: "fuchsia-pkg://another.domain/dummy_underscore#meta/dummy_underscore.cm"
                .to_string(),
            test_types: Box::new([]),
            device_categories: Box::new([]),
            is_automated: true,
        };
        let urls = process_test_list(HashSet::from([obj_url_a, obj_url_b])).unwrap();
        assert_eq!(urls.len(), 2);
        for e in vec![
            "fuchsia-pkg://fuchsia.com/fake-test#meta/fake-test.cm",
            "fuchsia-pkg://another.domain/dummy_underscore#meta/dummy_underscore.cm",
        ] {
            assert!(
                urls.contains(&run_test_suite_lib::TestParams {
                    test_url: e.to_string(),
                    ..run_test_suite_lib::TestParams::default()
                }),
                "Did not find a TestParams instance with test_url: {}",
                e.to_string()
            );
        }
    }

    #[test]
    fn test_validate_test_flags() {
        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            device: Some("val".to_string()),
            ..Default::default()
        })
        .is_err());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            ..Default::default()
        })
        .is_ok());

        assert!(validate_test_flags(&TestCommand {
            device: Some("val".to_string()),
            ..Default::default()
        })
        .is_ok());

        assert!(validate_test_flags(&TestCommand { ..Default::default() }).is_err());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            automated_only: true,
            ..Default::default()
        })
        .is_ok());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            manual_only: true,
            ..Default::default()
        })
        .is_ok());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            automated_only: true,
            manual_only: true,
            ..Default::default()
        })
        .is_err());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            driver_source: Some("blah".to_string()),
            ..Default::default()
        })
        .is_err());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            driver_source: Some("blah".to_string()),
            source_host_type: Some(results::SourceProvider::Gerrit),
            ..Default::default()
        })
        .is_ok());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            source_host_type: Some(results::SourceProvider::Gerrit),
            ..Default::default()
        })
        .is_err());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            generate_submission: true,
            ..Default::default()
        })
        .is_err());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            generate_submission: true,
            licenses: Some(Path::new("blah").to_path_buf()),
            ..Default::default()
        })
        .is_err());

        let license_temp_dir = tempfile::TempDir::new().unwrap();
        let license_dir = license_temp_dir.path();
        let license_file = license_dir.join("LICENSE");
        let license_empty_dir = license_dir.join("empty");
        let _ = fs::create_dir(&license_empty_dir).unwrap();
        let _ = File::create(&license_file).unwrap();

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            generate_submission: true,
            licenses: Some(license_dir.to_path_buf()),
            ..Default::default()
        })
        .is_ok());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            generate_submission: true,
            licenses: Some(license_file),
            ..Default::default()
        })
        .is_err());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            generate_submission: true,
            licenses: Some(license_empty_dir),
            ..Default::default()
        })
        .is_err());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            tests: Some(args::TestList { 0: vec!["fuchsia-pkg://a.b/c#meta/d.cm".to_string()] }),
            ..Default::default()
        })
        .is_ok());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            tests: Some(args::TestList { 0: vec![] }),
            categories: Some(args::DeviceCategoryList { 0: vec![] }),
            ..Default::default()
        })
        .is_err());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            tests: Some(args::TestList { 0: vec![] }),
            categories: Some(args::DeviceCategoryList {
                0: vec![parser::DeviceCategory {
                    category: "a".to_string(),
                    subcategory: "b".to_string()
                }]
            }),
            ..Default::default()
        })
        .is_err());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            tests: Some(args::TestList { 0: vec![] }),
            types: Some("".to_string()),
            ..Default::default()
        })
        .is_err());

        assert!(validate_test_flags(&TestCommand {
            driver: Some("val".to_string()),
            tests: Some(args::TestList { 0: vec![] }),
            categories: Some(args::DeviceCategoryList { 0: vec![] }),
            types: Some("".to_string()),
            ..Default::default()
        })
        .is_err());
    }

    #[test]
    fn test_get_name() {
        let test0 = get_name(&TestCommand {
            driver: Some("D".to_string()),
            submission_name: Some("S".to_string()),
            ..Default::default()
        });
        assert_eq!(test0, "S".to_string());

        let test1 = get_name(&TestCommand { driver: Some("D".to_string()), ..Default::default() });
        assert_eq!(test1, "D".to_string());

        let test2 = get_name(&TestCommand { device: Some("D".to_string()), ..Default::default() });
        assert_eq!(test2, "D".to_string());

        let test3 = get_name(&TestCommand {
            device: Some("D".to_string()),
            submission_name: Some("S".to_string()),
            ..Default::default()
        });
        assert_eq!(test3, "S".to_string());

        let test4 = get_name(&TestCommand { ..Default::default() });
        assert_eq!(test4, "Default Name".to_string());
    }

    #[test]
    fn test_parse_metadata() {
        // No path.
        assert!(parse_metadata(&TestCommand { ..Default::default() }).is_err());

        // Bad path.
        assert!(parse_metadata(&TestCommand {
            metadata_path: Some(Path::new("/").to_path_buf()),
            ..Default::default()
        })
        .is_err());

        // Valid path, no json.
        let mut file = tempfile::NamedTempFile::new().unwrap();
        let path = file.path().to_path_buf();
        file.write_all("".as_bytes()).unwrap();

        assert!(parse_metadata(&TestCommand {
            metadata_path: Some(path.clone()),
            ..Default::default()
        })
        .is_err());

        file.write_all(
            r#"{
            "certification_type": {
              "device_driver": {}
            },
            "system_types": {
              "workstation": {}
            },
            "driver_test_types": {
              "generic_driver_tests": {},
              "functional": {}
            },
            "device_category_types": {
              "misc": {},
              "imaging": {
                "camera": {}
              }
            },
            "tests": [
              {
                "url": "fuchsia-pkg://a/b#meta/c.cm",
                "test_types": [
                  "functional"
                ],
                "device_categories": [
                  {
                    "category": "imaging",
                    "subcategory": "camera"
                  }
                ],
                "is_automated": true
              },
              {
                "url": "fuchsia-pkg://a/d#meta/e.cm",
                "test_types": [
                  "functional"
                ],
                "device_categories": [
                  {
                    "category": "misc",
                    "subcategory": ""
                  }
                ],
                "is_automated": false
              }
            ]
          }"#
            .as_bytes(),
        )
        .unwrap();

        // Valid path, valid json.
        let metadata = parse_metadata(&TestCommand {
            metadata_path: Some(path.clone()),
            ..Default::default()
        });
        assert!(metadata.is_ok());
        let data = metadata.unwrap();
        assert_eq!(data.tests.len(), 2);
        assert_eq!(data.device_category_types.len(), 2);
        assert_eq!(data.driver_test_types.len(), 2);
        assert_eq!(data.driver_test_types.len(), 2);
        assert_eq!(data.system_types.len(), 1);
        assert_eq!(data.certification_type.len(), 1);
        assert_eq!(data.device_category_types["imaging"].len(), 1);
        assert!(data.device_category_types["imaging"].contains_key(&"camera".to_string()));
    }

    #[test]
    fn test_filter_tests_automated_vs_manual() {
        let tests = HashSet::from([
            parser::TestInfo {
                url: "automated".to_string(),
                is_automated: true,
                ..Default::default()
            },
            parser::TestInfo {
                url: "manual".to_string(),
                is_automated: false,
                ..Default::default()
            },
        ]);
        let test0 = filter_tests(
            &TestCommand { ..Default::default() },
            &parser::TestMetadata { ..Default::default() },
            tests.clone(),
        );
        assert!(test0.is_ok());
        let test0_val = test0.unwrap();
        assert_eq!(test0_val.len(), 2);

        let test1 = filter_tests(
            &TestCommand { automated_only: true, ..Default::default() },
            &parser::TestMetadata { ..Default::default() },
            tests.clone(),
        );
        assert!(test1.is_ok());
        let test1_val = test1.unwrap();
        assert_eq!(test1_val.len(), 1);
        assert_eq!(
            test1_val,
            HashSet::from([parser::TestInfo {
                url: "automated".to_string(),
                is_automated: true,
                ..Default::default()
            }])
        );

        let test2 = filter_tests(
            &TestCommand { manual_only: true, ..Default::default() },
            &parser::TestMetadata { ..Default::default() },
            tests.clone(),
        );
        assert!(test2.is_ok());
        let test2_val = test2.unwrap();
        assert_eq!(test2_val.len(), 1);
        assert_eq!(
            test2_val,
            HashSet::from([parser::TestInfo {
                url: "manual".to_string(),
                is_automated: false,
                ..Default::default()
            }])
        );
    }

    #[test]
    fn test_filter_tests_by_categories() {
        let metadata = parser::test::init_metadata();
        // No category filter.
        let tests0 = metadata
            .tests_by_url(&vec![
                "fuchsia-pkg://a/f#meta/g.cm".to_string(),
                "fuchsia-pkg://a/d#meta/e.cm".to_string(),
                "fuchsia-pkg://a/b#meta/c.cm".to_string(),
            ])
            .unwrap();
        let test0 = filter_tests(&TestCommand { ..Default::default() }, &metadata, tests0.clone());
        assert!(test0.is_ok());
        let test0_val = test0.unwrap();
        assert_eq!(test0_val.len(), 3);

        // Match subset of original list. Also makes sure "fuchsia-pkg://a/f#meta/g.cm" does not
        // end up matching as well.
        let tests1 = tests0;
        let test1 = filter_tests(
            &TestCommand {
                categories: Some(args::DeviceCategoryList(vec![parser::DeviceCategory {
                    category: "imaging".to_string(),
                    subcategory: "camera".to_string(),
                }])),
                ..Default::default()
            },
            &metadata,
            tests1.clone(),
        );
        assert!(test1.is_ok());
        let test1_val = test1.unwrap();
        assert_eq!(
            test1_val,
            HashSet::from([parser::TestInfo {
                url: "fuchsia-pkg://a/b#meta/c.cm".to_string(),
                test_types: Box::new(["functional".to_string()]),
                device_categories: Box::new([parser::DeviceCategory {
                    category: "imaging".to_string(),
                    subcategory: "camera".to_string(),
                }]),
                is_automated: true,
                ..Default::default()
            }])
        );

        // Empty original list.
        let test2 = filter_tests(
            &TestCommand {
                categories: Some(args::DeviceCategoryList(vec![parser::DeviceCategory {
                    category: "imaging".to_string(),
                    subcategory: "camera".to_string(),
                }])),
                ..Default::default()
            },
            &metadata,
            HashSet::from([]),
        );
        assert!(test2.is_ok());
        let test2_val = test2.unwrap();
        assert_eq!(test2_val.len(), 0);

        // No overlap.
        let tests3 = HashSet::from([
            parser::TestInfo {
                url: "thing".to_string(),
                device_categories: Box::new([
                    parser::DeviceCategory {
                        category: "imaging".to_string(),
                        subcategory: "camera".to_string(),
                    },
                    parser::DeviceCategory {
                        category: "input".to_string(),
                        subcategory: "touchpad".to_string(),
                    },
                ]),
                ..Default::default()
            },
            parser::TestInfo {
                url: "stuff".to_string(),
                device_categories: Box::new([parser::DeviceCategory {
                    category: "imaging".to_string(),
                    subcategory: "camera".to_string(),
                }]),
                ..Default::default()
            },
        ]);
        let test3 = filter_tests(
            &TestCommand {
                categories: Some(args::DeviceCategoryList(vec![parser::DeviceCategory {
                    category: "imaging".to_string(),
                    subcategory: "camera".to_string(),
                }])),
                ..Default::default()
            },
            &metadata,
            tests3.clone(),
        );
        assert!(test3.is_ok());
        let test3_val = test3.unwrap();
        assert_eq!(test3_val.len(), 0);

        // Full overlap.
        let tests4 = metadata
            .tests_by_url(&vec![
                "fuchsia-pkg://a/d#meta/e.cm".to_string(),
                "fuchsia-pkg://a/b#meta/c.cm".to_string(),
            ])
            .unwrap();
        let test4 = filter_tests(
            &TestCommand {
                categories: Some(args::DeviceCategoryList(vec![
                    parser::DeviceCategory {
                        category: "imaging".to_string(),
                        subcategory: "camera".to_string(),
                    },
                    parser::DeviceCategory {
                        category: "misc".to_string(),
                        subcategory: "".to_string(),
                    },
                ])),
                ..Default::default()
            },
            &metadata,
            tests4.clone(),
        );
        assert!(test4.is_ok());
        let test4_val = test4.unwrap();
        assert_eq!(test4_val.len(), 2);
    }

    fn create_dummy_ffx_test_results(output: &Path) -> Result<()> {
        // TODO: Consider just including a tar in the source and extracting it instead of
        // creating all the files each time.
        let suite_1_dir = output.join("123");
        let suite_1_syslog_path = suite_1_dir.join("syslog.txt");
        let suite_1_report_path = suite_1_dir.join("report.txt");
        let case_1_dir = output.join("456");
        let case_1_stdout_path = case_1_dir.join("stdout.txt");
        let case_2_dir = output.join("789");
        let case_2_stdout_path = case_2_dir.join("stdout.txt");
        let suite_2_dir = output.join("321");
        let suite_2_syslog_path = suite_2_dir.join("syslog.txt");
        let suite_2_report_path = suite_2_dir.join("report.txt");
        let case_3_dir = output.join("654");
        let case_3_stdout_path = case_3_dir.join("stdout.txt");
        let case_4_dir = output.join("987");
        let case_4_stdout_path = case_4_dir.join("stdout.txt");
        let case_4_custom_dir = case_4_dir.join("custom_dir");
        let case_4_custom_child_a = case_4_custom_dir.join("a");
        let case_4_custom_child_b = case_4_custom_dir.join("b");
        let case_4_custom_nest_dir = case_4_custom_dir.join("nest");
        let case_4_custom_nest_child_c = case_4_custom_nest_dir.join("c");
        let case_4_custom_nest_child_d = case_4_custom_nest_dir.join("d");
        let json_path = output.join("run_summary.json");
        fs::create_dir(suite_1_dir.as_path())?;
        let _ = File::create(suite_1_syslog_path.as_path())?.write_all(b"123_syslog")?;
        let _ = File::create(suite_1_report_path.as_path())?.write_all(b"123_report")?;
        fs::create_dir(case_1_dir.as_path())?;
        let _ = File::create(case_1_stdout_path.as_path())?.write_all(b"456_stdout")?;
        fs::create_dir(case_2_dir.as_path())?;
        let _ = File::create(case_2_stdout_path.as_path())?.write_all(b"789_stdout")?;
        fs::create_dir(suite_2_dir.as_path())?;
        let _ = File::create(suite_2_syslog_path.as_path())?.write_all(b"321_syslog")?;
        let _ = File::create(suite_2_report_path.as_path())?.write_all(b"321_report")?;
        fs::create_dir(case_3_dir.as_path())?;
        let _ = File::create(case_3_stdout_path.as_path())?.write_all(b"654_stdout")?;
        fs::create_dir(case_4_dir.as_path())?;
        let _ = File::create(case_4_stdout_path.as_path())?.write_all(b"987_stdout")?;
        fs::create_dir(case_4_custom_dir.as_path())?;
        let _ = File::create(case_4_custom_child_a.as_path())?.write_all(b"custom_child_a")?;
        let _ = File::create(case_4_custom_child_b.as_path())?.write_all(b"custom_child_b")?;
        fs::create_dir(case_4_custom_nest_dir.as_path())?;
        let _ = File::create(case_4_custom_nest_child_c.as_path())?.write_all(b"custom_child_c")?;
        let _ = File::create(case_4_custom_nest_child_d.as_path())?.write_all(b"custom_child_d")?;
        let mut json_file = File::create(json_path.as_path())?;
        json_file.write_all(
            r#"{
            "data": {
              "artifacts": {},
              "artifact_dir": "0",
              "outcome": "FAILED",
              "start_time": 1667971090761,
              "suites": [
                {
                  "name": "fuchsia-pkg://f.c/t#meta/a.cm",
                  "artifacts": {
                    "syslog.txt": {
                      "artifact_type": "SYSLOG"
                    },
                    "report.txt": {
                      "artifact_type": "REPORT"
                    }
                  },
                  "artifact_dir": "123",
                  "outcome": "PASSED",
                  "start_time": 1667971090818,
                  "duration_milliseconds": 25,
                  "cases": [
                    {
                      "name": "main",
                      "artifacts": {
                        "stdout.txt": {
                          "artifact_type": "STDOUT"
                        }
                      },
                      "artifact_dir": "456",
                      "outcome": "PASSED",
                      "start_time": 1667971090818
                    },
                    {
                      "name": "main",
                      "artifacts": {
                        "stdout.txt": {
                          "artifact_type": "STDOUT"
                        }
                      },
                      "artifact_dir": "789",
                      "outcome": "FAILED",
                      "start_time": 1667971090819
                    }
                  ],
                  "tags": []
                },
                {
                    "name": "fuchsia-pkg://f.d/t#meta/e.cm",
                    "artifacts": {
                      "syslog.txt": {
                        "artifact_type": "SYSLOG"
                      },
                      "report.txt": {
                        "artifact_type": "REPORT"
                      }
                    },
                    "artifact_dir": "321",
                    "outcome": "PASSED",
                    "start_time": 1667971090818,
                    "duration_milliseconds": 25,
                    "cases": [
                      {
                        "name": "main",
                        "artifacts": {
                          "stdout.txt": {
                            "artifact_type": "STDOUT"
                          }
                        },
                        "artifact_dir": "654",
                        "outcome": "PASSED",
                        "start_time": 1667971090818
                      },
                      {
                        "name": "main",
                        "artifacts": {
                          "stdout.txt": {
                            "artifact_type": "STDOUT"
                          },
                          "custom_dir": {
                            "artifact_type": "CUSTOM"
                          }
                        },
                        "artifact_dir": "987",
                        "outcome": "PASSED",
                        "start_time": 1667971090819
                      }
                    ],
                    "tags": []
                  }
              ]
            },
            "schema_id": "https://fuchsia.dev/schema/ffx_test/run_summary-8d1dd964.json"
          }
          "#
            .as_bytes(),
        )?;

        Ok(())
    }

    #[test]
    fn test_generate_submission() {
        let input_temp_dir = tempfile::TempDir::new().unwrap();
        let output_temp_dir = tempfile::TempDir::new().unwrap();
        let driver_binary_dir = tempfile::TempDir::new().unwrap();
        let licenses_dir = tempfile::TempDir::new().unwrap();
        let input = input_temp_dir.path();
        let output = output_temp_dir.path();
        let driver_x64_package_path = driver_binary_dir.path().join("driver_x64.far");
        let driver_arm64_package_path = driver_binary_dir.path().join("driver_arm64.far");
        let license_file_path = licenses_dir.path().join("LICENSE");
        let license_subdir_path = licenses_dir.path().join("sub");
        let license_2_file_path = license_subdir_path.join("license-2.txt");
        create_dummy_ffx_test_results(&input).unwrap();
        let _ = File::create(&driver_x64_package_path)
            .unwrap()
            .write_all(b"binary_content_x64")
            .unwrap();
        let _ = File::create(&driver_arm64_package_path)
            .unwrap()
            .write_all(b"binary_content_arm64")
            .unwrap();
        let _ = File::create(&license_file_path).unwrap().write_all(b"license").unwrap();
        let _ = fs::create_dir(&license_subdir_path).unwrap();
        let _ = File::create(&license_2_file_path).unwrap().write_all(b"license_2_txt").unwrap();

        assert!(generate_submission(
            &TestCommand {
                x64_package: Some(driver_x64_package_path),
                arm64_package: Some(driver_arm64_package_path),
                driver_source: Some("https://source.host/123".to_string()),
                source_host_type: Some(results::SourceProvider::Gerrit),
                licenses: Some(licenses_dir.path().to_path_buf()),
                ..Default::default()
            },
            &input,
            &output,
            &"A Name".to_string(),
            &"1.2.3".to_string(),
        )
        .is_ok());

        let tar_file = File::open(output.join("package.tar.gz").as_path()).unwrap();
        let mut archive = Archive::new(GzDecoder::new(tar_file));
        let entries = archive.entries().unwrap();
        let expected_entries = HashMap::from([
            (Path::new("artifacts"), "".to_string()),
            (Path::new("artifacts/driver_x64.far"), "binary_content_x64".to_string()),
            (Path::new("artifacts/driver_arm64.far"), "binary_content_arm64".to_string()),
            (Path::new("licenses"), "".to_string()),
            (Path::new("licenses/LICENSE"), "license".to_string()),
            (Path::new("licenses/sub"), "".to_string()),
            (Path::new("licenses/sub/license-2.txt"), "license_2_txt".to_string()),
            (Path::new("test_results"), "".to_string()),
            (Path::new("test_results/123"), "".to_string()),
            (Path::new("test_results/123/syslog.txt"), "123_syslog".to_string()),
            (Path::new("test_results/123/report.txt"), "123_report".to_string()),
            (Path::new("test_results/456"), "".to_string()),
            (Path::new("test_results/456/stdout.txt"), "456_stdout".to_string()),
            (Path::new("test_results/789"), "".to_string()),
            (Path::new("test_results/789/stdout.txt"), "789_stdout".to_string()),
            (Path::new("test_results/321"), "".to_string()),
            (Path::new("test_results/321/syslog.txt"), "321_syslog".to_string()),
            (Path::new("test_results/321/report.txt"), "321_report".to_string()),
            (Path::new("test_results/654"), "".to_string()),
            (Path::new("test_results/654/stdout.txt"), "654_stdout".to_string()),
            (Path::new("test_results/987"), "".to_string()),
            (Path::new("test_results/987/stdout.txt"), "987_stdout".to_string()),
            (Path::new("test_results/987/custom_dir"), "".to_string()),
            (Path::new("test_results/987/custom_dir/a"), "custom_child_a".to_string()),
            (Path::new("test_results/987/custom_dir/b"), "custom_child_b".to_string()),
            (Path::new("test_results/987/custom_dir/nest"), "".to_string()),
            (Path::new("test_results/987/custom_dir/nest/c"), "custom_child_c".to_string()),
            (Path::new("test_results/987/custom_dir/nest/d"), "custom_child_d".to_string()),
            (Path::new("manifest.json"), "".to_string()),
        ]);
        let mut count: usize = 0;
        entries.filter_map(|e| e.ok()).for_each(|mut entry| {
            let path = entry.path().unwrap().into_owned();
            count += 1;
            assert!(
                expected_entries.contains_key(&path.as_path()),
                "{} is not one of the expected entries.",
                &path.display()
            );
            if Path::new("manifest.json") == path.as_path() {
                // Have the manifest.json, unpack it and check the contents.
                let mut buf = String::new();
                entry.read_to_string(&mut buf).unwrap();
                let manifest: results::SuperjetManifest = serde_json::from_str(&buf).unwrap();
                assert_eq!(manifest.name, "A Name");
                assert_eq!(manifest.version, "1.2.3");
                assert_eq!(manifest.pass, false);
                assert_eq!(manifest.artifacts.len(), 3);
                assert_eq!(manifest.licenses.len(), 2);
                assert_eq!(manifest.test_results.len(), 6);
            } else if expected_entries[&path.as_path()].len() > 0 {
                // Have a file, check the contents are correct.
                let mut buf = String::new();
                entry.read_to_string(&mut buf).unwrap();
                assert_eq!(buf, expected_entries[&path.as_path()]);
            }
        });

        assert_eq!(count, expected_entries.len());
    }
}
