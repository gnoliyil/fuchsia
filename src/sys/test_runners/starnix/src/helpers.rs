// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{anyhow, Context as _, Error},
    fidl::endpoints::{create_proxy, Proxy},
    fidl::HandleBased,
    fidl_fuchsia_component as fcomponent, fidl_fuchsia_component_decl as fdecl,
    fidl_fuchsia_component_runner as frunner, fidl_fuchsia_data as fdata, fidl_fuchsia_io as fio,
    fidl_fuchsia_process as fprocess,
    fidl_fuchsia_test::{self as ftest},
    frunner::ComponentNamespaceEntry,
    fuchsia_component::client as fclient,
    fuchsia_runtime as fruntime, fuchsia_zircon as zx,
    fuchsiaperf::FuchsiaPerfBenchmarkResult,
    futures::StreamExt,
    gtest_runner_lib::parser::read_file,
    kernel_config::generate_kernel_config,
    runner::component::ComponentNamespace,
    tracing::debug,
};

pub async fn run_starnix_benchmark(
    test: ftest::Invocation,
    mut start_info: frunner::ComponentStartInfo,
    run_listener_proxy: &ftest::RunListenerProxy,
    starnix_kernel: &frunner::ComponentRunnerProxy,
    test_data_path: &str,
    converter: impl FnOnce(&str, &str) -> Result<Vec<FuchsiaPerfBenchmarkResult>, Error>,
) -> Result<(), Error> {
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

    debug!("getting test suite label");
    let program = start_info.program.as_ref().context("No program")?;
    let test_suite = match runner::get_value(program, "test_suite_label") {
        Some(fdata::DictionaryValue::Str(value)) => value.to_owned(),
        _ => return Err(anyhow!("No test suite label.")),
    };

    // Save the custom_artifacts DirectoryProxy for result reporting.
    let custom_artifacts =
        get_custom_artifacts_directory(start_info.ns.as_mut().expect("No namespace."))?;

    // Environment variables BENCHMARK_FORMAT and BENCHMARK_OUT should be set
    // so the test writes json results to this directory.
    let output_dir = add_output_dir_to_namespace(&mut start_info)?;

    // Start the test component.
    let component_controller = start_test_component(start_info, starnix_kernel)?;
    let result = read_result(component_controller.take_event_stream()).await;

    // Read the output it produced.
    let test_data = read_file(&output_dir, test_data_path)
        .await
        .with_context(|| format!("reading {test_data_path} from test data"))?
        .trim()
        .to_owned();

    let perfs =
        converter(&test_data, &test_suite).context("converting test output to fuchsiaperf")?;

    // Write JSON to custom artifacts directory where perf test infra expects it.
    let file_proxy = fuchsia_fs::directory::open_file(
        &custom_artifacts,
        "results.fuchsiaperf.json",
        fio::OpenFlags::RIGHT_WRITABLE | fio::OpenFlags::CREATE,
    )
    .await?;
    fuchsia_fs::file::write(&file_proxy, serde_json::to_string(&perfs)?).await?;

    case_listener_proxy.finished(result)?;

    Ok(())
}

/// The name of the collection in which the starnix kernel is instantiated.
pub const RUNNERS_COLLECTION: &str = "runners";

/// Replace the arguments in `program` with `test_arguments`, which were provided to the test
/// framework directly.
pub fn replace_program_args(test_arguments: Vec<String>, program: &mut fdata::Dictionary) {
    update_program_args(test_arguments, program, false);
}

/// Insert `new_args` into the arguments in `program`.
pub fn append_program_args(new_args: Vec<String>, program: &mut fdata::Dictionary) {
    update_program_args(new_args, program, true);
}

/// Clones relevant fields of `start_info`. `&mut` is required to clone the `ns` field, but the
/// `start_info` is not modified.
///
/// The `outgoing_dir` of the result will be present, but unusable.
pub fn clone_start_info(
    start_info: &mut frunner::ComponentStartInfo,
) -> Result<frunner::ComponentStartInfo, Error> {
    let ns = ComponentNamespace::try_from(start_info.ns.take().unwrap())?;
    // Reset the namespace of the start_info, since it was moved out above.
    start_info.ns = Some(ns.clone().try_into()?);

    let (outgoing_dir, _outgoing_dir_server) = zx::Channel::create();

    Ok(frunner::ComponentStartInfo {
        resolved_url: start_info.resolved_url.clone(),
        program: start_info.program.clone(),
        ns: Some(ns.try_into()?),
        outgoing_dir: Some(outgoing_dir.into()),
        runtime_dir: None,
        numbered_handles: Some(vec![]),
        encoded_config: None,
        break_on_start: None,
        ..frunner::ComponentStartInfo::EMPTY
    })
}

pub struct TestKernel {
    pub kernel_runner: frunner::ComponentRunnerProxy,
    pub realm: fcomponent::RealmProxy,
    pub name: String,
}

impl TestKernel {
    /// Instantiates a starnix kernel in the realm of the given namespace.
    ///
    /// # Parameters
    ///   - `kernels_dir`: The top-level directory where kernel configurations are stored.
    ///   - `start_info`: The component start info used to create the configuration for the
    ///                   starnix_kernel that will run the test.
    ///
    /// Returns a proxy to the instantiated runner as well as to the realm in which the runner is
    /// instantiated.
    pub async fn instantiate_in_realm(
        kernels_dir: &vfs::directory::immutable::Simple,
        mut start_info: frunner::ComponentStartInfo,
    ) -> Result<Self, Error> {
        const KERNELS_DIR_NAME: &str = "kernels";

        let runner_url = "starnix_kernel#meta/starnix_kernel.cm";

        // Grab the realm from the component's namespace.
        let realm =
            get_realm(start_info.ns.as_mut().ok_or(anyhow!("Couldn't get realm from namespace"))?)?;

        let kernel_start_info = generate_kernel_config(kernels_dir, KERNELS_DIR_NAME, start_info)?;

        realm
            .create_child(
                &mut fdecl::CollectionRef { name: RUNNERS_COLLECTION.into() },
                fdecl::Child {
                    name: Some(kernel_start_info.name.clone()),
                    url: Some(runner_url.to_string()),
                    startup: Some(fdecl::StartupMode::Lazy),
                    ..fdecl::Child::EMPTY
                },
                kernel_start_info.args,
            )
            .await?
            .map_err(|e| anyhow::anyhow!("failed to create runner child: {:?}", e))?;
        let runner_outgoing =
            open_exposed_directory(&realm, &kernel_start_info.name, RUNNERS_COLLECTION).await?;

        let kernel_runner = fclient::connect_to_protocol_at_dir_root::<
            frunner::ComponentRunnerMarker,
        >(&runner_outgoing)?;

        Ok(Self { kernel_runner, realm, name: kernel_start_info.name })
    }
}

/// Returns numbered handles with their respective stdout and stderr clients.
pub fn create_numbered_handles() -> (Option<Vec<fprocess::HandleInfo>>, zx::Socket, zx::Socket) {
    let (test_stdin, _) = zx::Socket::create_stream();
    let (test_stdout, stdout_client) = zx::Socket::create_stream();
    let (test_stderr, stderr_client) = zx::Socket::create_stream();
    let stdin_handle_info = fprocess::HandleInfo {
        handle: test_stdin.into_handle(),
        id: fruntime::HandleInfo::new(fruntime::HandleType::FileDescriptor, 0).as_raw(),
    };
    let stdout_handle_info = fprocess::HandleInfo {
        handle: test_stdout.into_handle(),
        id: fruntime::HandleInfo::new(fruntime::HandleType::FileDescriptor, 1).as_raw(),
    };
    let stderr_handle_info = fprocess::HandleInfo {
        handle: test_stderr.into_handle(),
        id: fruntime::HandleInfo::new(fruntime::HandleType::FileDescriptor, 2).as_raw(),
    };

    let numbered_handles = Some(vec![stdin_handle_info, stdout_handle_info, stderr_handle_info]);

    (numbered_handles, stdout_client, stderr_client)
}

/// Starts the test component and returns its proxy.
pub fn start_test_component(
    start_info: frunner::ComponentStartInfo,
    starnix_kernel: &frunner::ComponentRunnerProxy,
) -> Result<frunner::ComponentControllerProxy, Error> {
    let (component_controller, component_controller_server_end) =
        create_proxy::<frunner::ComponentControllerMarker>()?;

    debug!(?start_info, "asking kernel to start component");
    starnix_kernel.start(start_info, component_controller_server_end)?;

    Ok(component_controller)
}

/// Reads the result of the test run from `event_stream`.
///
/// The result is determined by reading the epitaph from the provided `event_stream`.
pub async fn read_result(
    mut event_stream: frunner::ComponentControllerEventStream,
) -> ftest::Result_ {
    let component_epitaph = match event_stream.next().await {
        Some(Err(fidl::Error::ClientChannelClosed { status, .. })) => status,
        result => {
            tracing::error!(
                "Didn't get epitaph from the component controller, instead got: {:?}",
                result
            );
            // Fail the test case here, since the component controller's epitaph couldn't be
            // read.
            zx::Status::INTERNAL
        }
    };

    match component_epitaph {
        zx::Status::OK => {
            ftest::Result_ { status: Some(ftest::Status::Passed), ..ftest::Result_::EMPTY }
        }
        _ => ftest::Result_ { status: Some(ftest::Status::Failed), ..ftest::Result_::EMPTY },
    }
}

pub fn add_output_dir_to_namespace(
    start_info: &mut frunner::ComponentStartInfo,
) -> Result<fio::DirectoryProxy, Error> {
    const TEST_DATA_DIR: &str = "/tmp/test_data";

    let test_data_path = format!("{}/{}", TEST_DATA_DIR, uuid::Uuid::new_v4());
    std::fs::create_dir_all(&test_data_path).expect("cannot create test output directory.");
    let test_data_dir = fuchsia_fs::directory::open_in_namespace(
        &test_data_path,
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
    )
    .expect("Cannot open test data directory.");

    let (dir_client, dir_server) = fidl::endpoints::create_endpoints::<fio::NodeMarker>();
    test_data_dir
        .clone(fio::OpenFlags::CLONE_SAME_RIGHTS, dir_server)
        .expect("Couldn't clone output directory.");
    let data_dir =
        fidl::endpoints::ClientEnd::<fio::DirectoryMarker>::new(dir_client.into_channel());

    start_info.ns.as_mut().ok_or(anyhow!("Missing namespace."))?.push(ComponentNamespaceEntry {
        path: Some("/test_data".to_string()),
        directory: Some(data_dir),
        ..ComponentNamespaceEntry::EMPTY
    });

    Ok(test_data_dir)
}

/// Replace or append the arguments in `program` with `new_args`.
fn update_program_args(mut new_args: Vec<String>, program: &mut fdata::Dictionary, append: bool) {
    /// The program argument key name.
    const ARGS_KEY: &str = "args";

    if new_args.is_empty() {
        return;
    }

    let mut new_entry = fdata::DictionaryEntry {
        key: ARGS_KEY.to_string(),
        value: Some(Box::new(fdata::DictionaryValue::StrVec(new_args.clone()))),
    };
    if let Some(entries) = &mut program.entries {
        if let Some(index) = entries.iter().position(|entry| entry.key == ARGS_KEY) {
            let entry = entries.remove(index);

            if append {
                if let Some(mut box_value) = entry.value {
                    if let fdata::DictionaryValue::StrVec(ref mut args) = &mut *box_value {
                        args.append(&mut new_args);
                        new_entry.value =
                            Some(Box::new(fdata::DictionaryValue::StrVec(args.to_vec())));
                    };
                }
            }
        }
        entries.push(new_entry);
    } else {
        let entries = vec![new_entry];
        program.entries = Some(entries);
    };
}

fn get_realm(
    namespace: &mut Vec<frunner::ComponentNamespaceEntry>,
) -> Result<fcomponent::RealmProxy, Error> {
    namespace
        .iter_mut()
        .flat_map(|entry| {
            if entry.path.as_ref().unwrap() == "/svc" {
                let directory = entry
                    .directory
                    .take()
                    .unwrap()
                    .into_proxy()
                    .expect("Failed to grab directory proxy.");
                let r = Some(fuchsia_component::client::connect_to_protocol_at_dir_root::<
                    fcomponent::RealmMarker,
                >(&directory));

                let dir_channel: zx::Channel = directory.into_channel().expect("").into();
                entry.directory = Some(dir_channel.into());
                r
            } else {
                None
            }
        })
        .next()
        .ok_or_else(|| anyhow!("Unable to find /svc"))?
}

async fn open_exposed_directory(
    realm: &fcomponent::RealmProxy,
    child_name: &str,
    collection_name: &str,
) -> Result<fio::DirectoryProxy, Error> {
    let (directory_proxy, server_end) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()?;
    realm
        .open_exposed_dir(
            &mut fdecl::ChildRef {
                name: child_name.into(),
                collection: Some(collection_name.into()),
            },
            server_end,
        )
        .await?
        .map_err(|e| {
            anyhow!(
                "failed to bind to child {} in collection {:?}: {:?}",
                child_name,
                collection_name,
                e
            )
        })?;
    Ok(directory_proxy)
}

fn get_custom_artifacts_directory(
    namespace: &mut Vec<frunner::ComponentNamespaceEntry>,
) -> Result<fio::DirectoryProxy, Error> {
    for entry in namespace {
        if entry.path.as_ref().unwrap() == "/custom_artifacts" {
            return entry
                .directory
                .take()
                .unwrap()
                .into_proxy()
                .map_err(|_| anyhow!("Couldn't grab proxy."));
        }
    }

    Err(anyhow!("Couldn't find /custom artifacts."))
}
