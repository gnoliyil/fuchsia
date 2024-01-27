// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod component;
mod config;
mod crash_handler;
pub mod crash_info;
mod error;
mod launcher;
pub mod process_launcher;
mod runtime_dir;
mod stdout;
pub mod vdso_vmo;

use {
    self::{
        component::ElfComponent,
        config::ElfProgramConfig,
        error::{ConfigError, ElfRunnerError},
        launcher::ProcessLauncherConnector,
        runtime_dir::RuntimeDirBuilder,
        stdout::bind_streams_to_syslog,
    },
    crate::{
        crash_info::CrashRecords,
        vdso_vmo::{get_direct_vdso_vmo, get_next_vdso_vmo},
    },
    ::routing::{config::RuntimeConfig, policy::ScopedPolicyChecker},
    async_trait::async_trait,
    chrono::{DateTime, NaiveDateTime, Utc},
    cm_runner::Runner,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_component as fcomp, fidl_fuchsia_component_runner as fcrunner,
    fidl_fuchsia_diagnostics_types::{
        ComponentDiagnostics, ComponentTasks, Task as DiagnosticsTask,
    },
    fidl_fuchsia_mem as fmem, fidl_fuchsia_process as fproc,
    fidl_fuchsia_process_lifecycle::LifecycleMarker,
    fuchsia_async::{self as fasync, TimeoutExt},
    fuchsia_runtime::{duplicate_utc_clock_handle, job_default, HandleInfo, HandleType},
    fuchsia_zircon::{
        self as zx, AsHandleRef, Clock, Duration, HandleBased, Job, ProcessInfo, Signals, Status,
        Time, Vmo,
    },
    futures::channel::oneshot,
    moniker::AbsoluteMoniker,
    runner::component::ChannelEpitaph,
    std::convert::TryFrom,
    std::{convert::TryInto, path::Path, sync::Arc},
    tracing::warn,
};

// Maximum time that the runner will wait for break_on_start eventpair to signal.
// This is set to prevent debuggers from blocking us for too long, either intentionally
// or unintentionally.
const MAX_WAIT_BREAK_ON_START: Duration = Duration::from_millis(300);

// Minimum timer slack amount and default mode. The amount should be large enough to allow for some
// coalescing of timers, but small enough to ensure applications don't miss deadlines.
//
// TODO(fxbug.dev/43934): For now, set the value to 50us to avoid delaying performance-critical
// timers in Scenic and other system services.
const TIMER_SLACK_DURATION: zx::Duration = zx::Duration::from_micros(50);

// Builds and serves the runtime directory
/// Runs components with ELF binaries.
pub struct ElfRunner {
    launcher_connector: ProcessLauncherConnector,
    /// If `utc_clock` is populated then that Clock's handle will
    /// be passed into the newly created process. Otherwise, the UTC
    /// clock will be duplicated from current process' process table.
    /// The latter is typically the case in unit tests and nested
    /// component managers.
    utc_clock: Option<Arc<Clock>>,

    crash_records: CrashRecords,
}

struct ConfigureLauncherResult {
    /// Populated if a UTC clock is available and has started.
    utc_clock_xform: Option<zx::ClockTransformation>,
    launch_info: fidl_fuchsia_process::LaunchInfo,
    runtime_dir_builder: RuntimeDirBuilder,
    tasks: Vec<fasync::Task<()>>,
}

impl ElfRunner {
    pub fn new(
        config: &RuntimeConfig,
        utc_clock: Option<Arc<Clock>>,
        crash_records: CrashRecords,
    ) -> ElfRunner {
        ElfRunner {
            launcher_connector: ProcessLauncherConnector::new(config),
            utc_clock,
            crash_records,
        }
    }

    async fn configure_launcher(
        &self,
        resolved_url: &String,
        start_info: fcrunner::ComponentStartInfo,
        elf_config: &ElfProgramConfig,
        job: zx::Job,
        launcher: &fidl_fuchsia_process::LauncherProxy,
        lifecycle_server: Option<zx::Channel>,
        custom_vdso: Option<zx::Vmo>,
        direct_vdso: Option<zx::Vmo>,
    ) -> Result<Option<ConfigureLauncherResult>, ElfRunnerError> {
        let bin_path = runner::get_program_binary(&start_info)
            .map_err(|err| ElfRunnerError::ProgramBinaryError { url: resolved_url.clone(), err })?;

        let args = runner::get_program_args(&start_info);

        let stdout_sink = elf_config.get_stdout_sink();
        let stderr_sink = elf_config.get_stderr_sink();

        let clock_rights =
            zx::Rights::READ | zx::Rights::WAIT | zx::Rights::DUPLICATE | zx::Rights::TRANSFER;
        let utc_clock = if let Some(utc_clock) = &self.utc_clock {
            utc_clock.duplicate_handle(clock_rights)
        } else {
            duplicate_utc_clock_handle(clock_rights)
        }
        .map_err(|s| ElfRunnerError::UtcClockDuplicateFailed {
            url: resolved_url.clone(),
            status: s,
        })?;

        // TODO(fxbug.dev/45586): runtime_dir may be unavailable in tests. We should fix tests so
        // that we don't have to have this check here.
        let runtime_dir_builder = match start_info.runtime_dir {
            Some(dir) => RuntimeDirBuilder::new(dir).args(args.clone()),
            None => return Ok(None),
        };

        let utc_clock_started = fasync::OnSignals::new(&utc_clock, Signals::CLOCK_STARTED)
            .on_timeout(Time::after(Duration::default()), || Err(Status::TIMED_OUT))
            .await
            .is_ok();
        let utc_clock_xform = if utc_clock_started {
            utc_clock.get_details().map(|details| details.mono_to_synthetic).ok()
        } else {
            None
        };

        let name = Path::new(&resolved_url)
            .file_name()
            .ok_or(ElfRunnerError::BadResolvedUrl(resolved_url.clone()))?
            .to_str()
            .ok_or(ElfRunnerError::BadResolvedUrl(resolved_url.clone()))?;

        // Convert the directories into proxies, so we can find "/pkg" and open "lib" and bin_path
        let ns = runner::component::ComponentNamespace::try_from(
            start_info.ns.unwrap_or_else(|| vec![]),
        )
        .map_err(|err| ElfRunnerError::ComponentNamespaceError {
            url: resolved_url.clone(),
            err,
        })?;

        let (stdout_and_stderr_tasks, stdout_and_stderr_handles) =
            bind_streams_to_syslog(&ns, stdout_sink, stderr_sink);

        let mut handle_infos = vec![];

        handle_infos.extend(stdout_and_stderr_handles);

        if let Some(outgoing_dir) = start_info.outgoing_dir {
            handle_infos.push(fproc::HandleInfo {
                handle: outgoing_dir.into_handle(),
                id: HandleInfo::new(HandleType::DirectoryRequest, 0).as_raw(),
            });
        }

        if let Some(lifecycle_chan) = lifecycle_server {
            handle_infos.push(fproc::HandleInfo {
                handle: lifecycle_chan.into_handle(),
                id: HandleInfo::new(HandleType::Lifecycle, 0).as_raw(),
            })
        };

        handle_infos.push(fproc::HandleInfo {
            handle: utc_clock.into_handle(),
            id: HandleInfo::new(HandleType::ClockUtc, 0).as_raw(),
        });

        if let Some(custom_vdso) = custom_vdso {
            handle_infos.push(fproc::HandleInfo {
                handle: custom_vdso.into_handle(),
                id: HandleInfo::new(HandleType::VdsoVmo, 0).as_raw(),
            });
        }

        if let Some(direct_vdso) = direct_vdso {
            handle_infos.push(fproc::HandleInfo {
                handle: direct_vdso.into_handle(),
                id: HandleInfo::new(HandleType::VdsoVmo, 1).as_raw(),
            });
        }

        if let Some(encoded) = start_info.encoded_config {
            let handle = match encoded {
                fmem::Data::Buffer(fmem::Buffer {
                    vmo,
                    size: _, // we get this vmo from component manager which sets the content size
                }) => vmo.into_handle(),
                fmem::Data::Bytes(bytes) => {
                    let size = bytes.len() as u64;
                    let vmo = Vmo::create(size).map_err(ConfigError::VmoCreate)?;
                    vmo.write(&bytes, 0).map_err(ConfigError::VmoWrite)?;
                    vmo.into_handle()
                }
                _ => return Err(ConfigError::UnrecognizedDataVariant.into()),
            };
            handle_infos.push(fproc::HandleInfo {
                handle,
                id: HandleInfo::new(HandleType::ComponentConfigVmo, 0).as_raw(),
            });
        }

        if let Some(handles) = start_info.numbered_handles {
            handle_infos.extend(handles);
        }

        // Load the component
        let launch_info =
            runner::component::configure_launcher(runner::component::LauncherConfigArgs {
                bin_path: &bin_path,
                name: &name,
                options: elf_config.process_options(),
                args: Some(args),
                ns: ns,
                job: Some(job),
                handle_infos: Some(handle_infos),
                name_infos: None,
                environs: elf_config.get_environ(),
                launcher: &launcher,
                loader_proxy_chan: None,
                executable_vmo: None,
            })
            .await
            .map_err(|err| ElfRunnerError::ConfigureLauncherError {
                url: resolved_url.clone(),
                err,
            })?;

        Ok(Some(ConfigureLauncherResult {
            utc_clock_xform,
            launch_info,
            runtime_dir_builder,
            tasks: stdout_and_stderr_tasks,
        }))
    }

    pub async fn start_component(
        &self,
        start_info: fcrunner::ComponentStartInfo,
        checker: &ScopedPolicyChecker,
    ) -> Result<Option<ElfComponent>, ElfRunnerError> {
        let resolved_url =
            runner::get_resolved_url(&start_info).ok_or(ElfRunnerError::MissingResolvedUrl)?;

        // This also checks relevant security policy for config that it wraps using the provided
        // PolicyChecker.
        let program_config = start_info
            .program
            .as_ref()
            .map(|p| ElfProgramConfig::parse_and_check(p, &checker, &resolved_url))
            .transpose()?
            .unwrap_or_default();

        let url = resolved_url.clone();
        let main_process_critical = program_config.has_critical_main_process();
        let res: Result<Option<ElfComponent>, ElfRunnerError> = self
            .start_component_helper(
                start_info,
                checker.get_scope().clone(),
                resolved_url,
                program_config,
            )
            .await;
        match res {
            Err(e) if main_process_critical => {
                panic!("failed to launch component with a critical process ({:?}): {:?}", url, e)
            }
            x => x,
        }
    }

    async fn start_component_helper(
        &self,
        mut start_info: fcrunner::ComponentStartInfo,
        moniker: AbsoluteMoniker,
        resolved_url: String,
        program_config: ElfProgramConfig,
    ) -> Result<Option<ElfComponent>, ElfRunnerError> {
        let launcher = self.launcher_connector.connect().map_err(|err| {
            ElfRunnerError::ProcessLauncherConnectError {
                url: resolved_url.clone(),
                err: err.into(),
            }
        })?;

        let component_job = job_default()
            .create_child_job()
            .map_err(|e| ElfRunnerError::job_creation_failed(resolved_url.clone(), e))?;

        crash_handler::run_exceptions_server(
            &component_job,
            moniker.clone(),
            resolved_url.clone(),
            self.crash_records.clone(),
        )
        .map_err(|e| ElfRunnerError::exception_registration_failed(resolved_url.clone(), e))?;

        // Set timer slack.
        //
        // Why Late and not Center or Early? Timers firing a little later than requested is not
        // uncommon in non-realtime systems. Programs are generally tolerant of some
        // delays. However, timers firing before their deadline can be unexpected and lead to bugs.
        component_job
            .set_policy(zx::JobPolicy::TimerSlack(
                TIMER_SLACK_DURATION,
                zx::JobDefaultTimerMode::Late,
            ))
            .map_err(|e| ElfRunnerError::job_policy_set_failed(resolved_url.clone(), e))?;

        // Prevent direct creation of processes.
        //
        // The kernel-level mechanisms for creating processes are very low-level. We require that
        // all processes be created via fuchsia.process.Launcher in order for the platform to
        // maintain change-control over how processes are created.
        if !program_config.can_create_raw_processes() {
            component_job
                .set_policy(zx::JobPolicy::Basic(
                    zx::JobPolicyOption::Absolute,
                    vec![(zx::JobCondition::NewProcess, zx::JobAction::Deny)],
                ))
                .map_err(|e| ElfRunnerError::job_policy_set_failed(resolved_url.clone(), e))?;
        }

        // Default deny the job policy which allows ambiently marking VMOs executable, i.e. calling
        // vmo_replace_as_executable without an appropriate resource handle.
        if !program_config.has_ambient_mark_vmo_exec() {
            component_job
                .set_policy(zx::JobPolicy::Basic(
                    zx::JobPolicyOption::Absolute,
                    vec![(zx::JobCondition::AmbientMarkVmoExec, zx::JobAction::Deny)],
                ))
                .map_err(|e| ElfRunnerError::job_policy_set_failed(resolved_url.clone(), e))?;
        }

        let mut custom_vdso = None;
        if program_config.should_use_next_vdso() {
            let next_vdso = get_next_vdso_vmo()
                .map_err(|e| ElfRunnerError::next_vdso_error(resolved_url.clone(), e))?;
            custom_vdso = Some(next_vdso);
        }

        let mut direct_vdso = None;
        if program_config.should_use_direct_vdso() {
            let vdso = get_direct_vdso_vmo()
                .map_err(|e| ElfRunnerError::direct_vdso_error(resolved_url.clone(), e))?;
            direct_vdso = Some(vdso);
        }

        let (lifecycle_client, lifecycle_server) = match program_config.notify_when_stopped() {
            true => {
                // Creating a channel is not expected to fail.
                let (client, server) = fidl::endpoints::create_proxy::<LifecycleMarker>().unwrap();
                (Some(client), Some(server))
            }
            false => (None, None),
        };
        let lifecycle_server = lifecycle_server.map(|s| s.into_channel());

        let job_dup = component_job
            .duplicate_handle(zx::Rights::SAME_RIGHTS)
            .map_err(|e| ElfRunnerError::job_duplication_failed(resolved_url.clone(), e))?;

        let break_on_start = start_info.break_on_start.take();

        let (utc_clock_xform, mut launch_info, runtime_dir_builder, tasks) = match self
            .configure_launcher(
                &resolved_url,
                start_info,
                &program_config,
                job_dup,
                &launcher,
                lifecycle_server,
                custom_vdso,
                direct_vdso,
            )
            .await?
        {
            Some(result) => (
                result.utc_clock_xform,
                result.launch_info,
                result.runtime_dir_builder,
                result.tasks,
            ),
            None => return Ok(None),
        };

        let job_koid = component_job
            .get_koid()
            .map_err(|e| ElfRunnerError::job_id_retrieve_failed(resolved_url.clone(), e))?
            .raw_koid();

        let runtime_dir = runtime_dir_builder.job_id(job_koid).serve();

        // Wait on break_on_start with a timeout and don't fail.
        if let Some(break_on_start) = break_on_start {
            fasync::OnSignals::new(&break_on_start, Signals::OBJECT_PEER_CLOSED)
                .on_timeout(MAX_WAIT_BREAK_ON_START, || Err(Status::TIMED_OUT))
                .await
                .err()
                .map(|error| warn!(%moniker, %error, "Failed to wait break_on_start"));
        }

        // Launch the component
        let (status, process) = launcher.launch(&mut launch_info).await.map_err(|err| {
            ElfRunnerError::ProcessLauncherFidlError { url: resolved_url.clone(), err }
        })?;
        zx::Status::ok(status).map_err(|status| ElfRunnerError::CreateProcessFailed {
            url: resolved_url.clone(),
            status,
        })?;
        let process = process.ok_or(ElfRunnerError::NoProcess { url: resolved_url.clone() })?;
        if program_config.has_critical_main_process() {
            job_default()
                .set_critical(zx::JobCriticalOptions::RETCODE_NONZERO, &process)
                .map_err(|s| ElfRunnerError::process_critical_mark_failed(resolved_url.clone(), s))
                .expect("failed to set process as critical");
        }

        runtime_dir.add_process_id(
            process
                .get_koid()
                .map_err(|e| ElfRunnerError::process_id_retrieve_failed(resolved_url.clone(), e))?
                .raw_koid(),
        );

        let process_start_time = process
            .info()
            .map_err(|e| ElfRunnerError::process_id_retrieve_failed(resolved_url.clone(), e))?
            .start_time;
        runtime_dir.add_process_start_time(process_start_time);

        if let Some(utc_clock_xform) = utc_clock_xform {
            let utc_timestamp =
                utc_clock_xform.apply(Time::from_nanos(process_start_time)).into_nanos();
            let seconds = (utc_timestamp / 1_000_000_000) as i64;
            let nanos = (utc_timestamp % 1_000_000_000) as u32;
            let dt = DateTime::<Utc>::from_utc(NaiveDateTime::from_timestamp(seconds, nanos), Utc);
            runtime_dir.add_process_start_time_utc_estimate(dt.to_string())
        };

        Ok(Some(ElfComponent::new(
            runtime_dir,
            Job::from(component_job),
            process,
            lifecycle_client,
            program_config.has_critical_main_process(),
            tasks,
            resolved_url.clone(),
        )))
    }

    pub fn get_scoped_runner(self: Arc<Self>, checker: ScopedPolicyChecker) -> Arc<dyn Runner> {
        Arc::new(ScopedElfRunner { runner: self, checker })
    }
}

struct ScopedElfRunner {
    runner: Arc<ElfRunner>,
    checker: ScopedPolicyChecker,
}

#[async_trait]
impl Runner for ScopedElfRunner {
    /// Starts a component by creating a new Job and Process for the component.
    /// The Runner also creates and hosts a namespace for the component. The
    /// namespace and other runtime state of the component will live until the
    /// Future returned is dropped or the `server_end` is sent either
    /// `ComponentController.Stop` or `ComponentController.Kill`. Sending
    /// `ComponentController.Stop` or `ComponentController.Kill` causes the
    /// Future to complete.
    async fn start(
        &self,
        start_info: fcrunner::ComponentStartInfo,
        server_end: ServerEnd<fcrunner::ComponentControllerMarker>,
    ) {
        // Start the component and move the Controller into a new async
        // execution context.
        let resolved_url = runner::get_resolved_url(&start_info).unwrap_or(String::new());
        match self.runner.start_component(start_info, &self.checker).await {
            Ok(Some(elf_component)) => {
                let (epitaph_tx, epitaph_rx) = oneshot::channel::<ChannelEpitaph>();
                // This function waits for something from the channel and
                // returns it or Error::Internal if the channel is closed
                let epitaph_fn = Box::pin(async move {
                    epitaph_rx
                        .await
                        .unwrap_or_else(|_| {
                            warn!("epitaph oneshot channel closed unexpectedly");
                            fcomp::Error::Internal.into()
                        })
                        .into()
                });

                let proc_copy = match elf_component.copy_process() {
                    Some(copy) => copy,
                    None => {
                        runner::component::report_start_error(
                            zx::Status::from_raw(
                                i32::try_from(fcomp::Error::InstanceCannotStart.into_primitive())
                                    .unwrap(),
                            ),
                            "Component unexpectedly had no process".to_string(),
                            &resolved_url,
                            server_end,
                        );
                        return;
                    }
                };

                let component_diagnostics = elf_component
                    .copy_job_for_diagnostics()
                    .map(|job| ComponentDiagnostics {
                        tasks: Some(ComponentTasks {
                            component_task: Some(DiagnosticsTask::Job(job.into())),
                            ..Default::default()
                        }),
                        ..Default::default()
                    })
                    .map_err(|error| {
                        warn!(%error, "Failed to copy job for diagnostics");
                        ()
                    })
                    .ok();

                // Spawn a future that watches for the process to exit
                fasync::Task::spawn(async move {
                    fasync::OnSignals::new(
                        &proc_copy.as_handle_ref(),
                        zx::Signals::PROCESS_TERMINATED,
                    )
                    .await
                    .map(|_: fidl::Signals| ()) // Discard.
                    .unwrap_or_else(|error| warn!(%error, "error creating signal handler"));
                    // Process exit code '0' is considered a clean return.
                    // TODO (fxbug.dev/57024) If we create an epitaph that indicates
                    // intentional, non-zero exit, use that for all non-0 exit
                    // codes.
                    let exit_status: ChannelEpitaph = match proc_copy.info() {
                        Ok(ProcessInfo { return_code: 0, .. }) => {
                            zx::Status::OK.try_into().unwrap()
                        }
                        Ok(_) => fcomp::Error::InstanceDied.into(),
                        Err(error) => {
                            warn!(%error, "Unable to query process info");
                            fcomp::Error::Internal.into()
                        }
                    };
                    epitaph_tx.send(exit_status).unwrap_or_else(|_| warn!("error sending epitaph"));
                })
                .detach();

                // Create a future which owns and serves the controller
                // channel. The `epitaph_fn` future completes when the
                // component's main process exits. The controller then sets the
                // epitaph on the controller channel, closes it, and stops
                // serving the protocol.
                fasync::Task::spawn(async move {
                    let (server_stream, control) = match server_end.into_stream_and_control_handle()
                    {
                        Ok(s) => s,
                        Err(error) => {
                            warn!(%error, "Converting Controller channel to stream failed");
                            return;
                        }
                    };
                    if let Some(component_diagnostics) = component_diagnostics {
                        control.send_on_publish_diagnostics(component_diagnostics).unwrap_or_else(
                            |error| warn!(url=%resolved_url, %error, "sending diagnostics failed"),
                        );
                    }
                    runner::component::Controller::new(elf_component, server_stream)
                        .serve(epitaph_fn)
                        .await
                        .unwrap_or_else(|error| warn!(%error, "serving ComponentController"));
                })
                .detach();
            }

            Ok(None) => {
                // TODO(fxbug.dev/): Should this signal an error?
            }
            Err(err) => {
                runner::component::report_start_error(
                    err.as_zx_status(),
                    format!("{}", err),
                    &resolved_url,
                    server_end,
                );
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::runtime_dir::RuntimeDirectory,
        super::*,
        ::routing::{
            config::{AllowlistEntryBuilder, JobPolicyAllowlists, RuntimeConfig, SecurityPolicy},
            policy::ScopedPolicyChecker,
        },
        anyhow::{Context, Error},
        assert_matches::assert_matches,
        fidl::endpoints::{
            create_endpoints, create_proxy, ClientEnd, DiscoverableProtocolMarker, Proxy, ServerEnd,
        },
        fidl_fuchsia_component as fcomp, fidl_fuchsia_component_runner as fcrunner,
        fidl_fuchsia_data as fdata,
        fidl_fuchsia_diagnostics_types::{
            ComponentDiagnostics, ComponentTasks, Task as DiagnosticsTask,
        },
        fidl_fuchsia_io as fio,
        fidl_fuchsia_logger::{LogSinkMarker, LogSinkRequest, LogSinkRequestStream},
        fidl_fuchsia_process_lifecycle::LifecycleMarker,
        fidl_fuchsia_process_lifecycle::LifecycleProxy,
        fuchsia_async as fasync,
        fuchsia_component::server::{ServiceFs, ServiceObjLocal},
        fuchsia_fs,
        fuchsia_zircon::{self as zx, Task},
        fuchsia_zircon::{AsHandleRef, Job, Process},
        futures::{join, StreamExt, TryStreamExt},
        moniker::{AbsoluteMoniker, AbsoluteMonikerBase},
        runner::component::Controllable,
        scoped_task,
        std::{
            convert::TryFrom,
            sync::{Arc, Mutex},
            task::Poll,
        },
    };

    pub enum MockServiceRequest {
        LogSink(LogSinkRequestStream),
    }

    pub type MockServiceFs<'a> = ServiceFs<ServiceObjLocal<'a, MockServiceRequest>>;

    /// Create a new local fs and install a mock LogSink service into.
    /// Returns the created directory and corresponding namespace entries.
    pub fn create_fs_with_mock_logsink(
    ) -> Result<(MockServiceFs<'static>, Vec<fcrunner::ComponentNamespaceEntry>), Error> {
        let (client, server) = create_endpoints::<fio::DirectoryMarker>();
        let mut dir = ServiceFs::new_local();
        dir.add_fidl_service_at(LogSinkMarker::PROTOCOL_NAME, MockServiceRequest::LogSink);
        dir.serve_connection(server).context("Failed to add serving channel.")?;
        let entries = vec![fcrunner::ComponentNamespaceEntry {
            path: Some("/svc".to_string()),
            directory: Some(client),
            ..Default::default()
        }];

        Ok((dir, entries))
    }

    fn new_elf_runner_for_test(config: &RuntimeConfig) -> Arc<ElfRunner> {
        Arc::new(ElfRunner::new(&config, None, CrashRecords::new()))
    }

    fn hello_world_startinfo(
        runtime_dir: Option<ServerEnd<fio::DirectoryMarker>>,
    ) -> fcrunner::ComponentStartInfo {
        // Get a handle to /pkg
        let pkg_path = "/pkg".to_string();
        let pkg_chan = fuchsia_fs::directory::open_in_namespace(
            "/pkg",
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE,
        )
        .unwrap()
        .into_channel()
        .unwrap()
        .into_zx_channel();
        let pkg_handle = ClientEnd::new(pkg_chan);

        let ns = vec![fcrunner::ComponentNamespaceEntry {
            path: Some(pkg_path),
            directory: Some(pkg_handle),
            ..Default::default()
        }];

        fcrunner::ComponentStartInfo {
            resolved_url: Some(
                "fuchsia-pkg://fuchsia.com/elf_runner_tests#meta/hello_world.cm".to_string(),
            ),
            program: Some(fdata::Dictionary {
                entries: Some(vec![
                    fdata::DictionaryEntry {
                        key: "args".to_string(),
                        value: Some(Box::new(fdata::DictionaryValue::StrVec(vec![
                            "foo".to_string(),
                            "bar".to_string(),
                        ]))),
                    },
                    fdata::DictionaryEntry {
                        key: "binary".to_string(),
                        value: Some(Box::new(fdata::DictionaryValue::Str(
                            "bin/hello_world".to_string(),
                        ))),
                    },
                ]),
                ..Default::default()
            }),
            ns: Some(ns),
            outgoing_dir: None,
            runtime_dir,
            ..Default::default()
        }
    }

    /// Creates start info for a component which runs until told to exit. The
    /// ComponentController protocol can be used to stop the component when the
    /// test is done inspecting the launched component.
    fn lifecycle_startinfo(
        runtime_dir: Option<ServerEnd<fio::DirectoryMarker>>,
    ) -> fcrunner::ComponentStartInfo {
        // Get a handle to /pkg
        let pkg_path = "/pkg".to_string();
        let pkg_chan = fuchsia_fs::directory::open_in_namespace(
            "/pkg",
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE,
        )
        .unwrap()
        .into_channel()
        .unwrap()
        .into_zx_channel();
        let pkg_handle = ClientEnd::new(pkg_chan);

        let ns = vec![fcrunner::ComponentNamespaceEntry {
            path: Some(pkg_path),
            directory: Some(pkg_handle),
            ..Default::default()
        }];

        fcrunner::ComponentStartInfo {
            resolved_url: Some(
                "fuchsia-pkg://fuchsia.com/lifecycle-example#meta/lifecycle.cm".to_string(),
            ),
            program: Some(fdata::Dictionary {
                entries: Some(vec![
                    fdata::DictionaryEntry {
                        key: "args".to_string(),
                        value: Some(Box::new(fdata::DictionaryValue::StrVec(vec![
                            "foo".to_string(),
                            "bar".to_string(),
                        ]))),
                    },
                    fdata::DictionaryEntry {
                        key: "binary".to_string(),
                        value: Some(Box::new(fdata::DictionaryValue::Str(
                            "bin/lifecycle_placeholder".to_string(),
                        ))),
                    },
                    fdata::DictionaryEntry {
                        key: "lifecycle.stop_event".to_string(),
                        value: Some(Box::new(fdata::DictionaryValue::Str("notify".to_string()))),
                    },
                ]),
                ..Default::default()
            }),
            ns: Some(ns),
            outgoing_dir: None,
            runtime_dir,
            ..Default::default()
        }
    }

    fn create_dummy_process(job: &scoped_task::Scoped<Job>, name_for_builtin: &str) -> Process {
        let (process, _vmar) = job
            .create_child_process(zx::ProcessOptions::empty(), name_for_builtin.as_bytes())
            .expect("could not create process");
        process
    }

    fn make_default_elf_component(
        lifecycle_client: Option<LifecycleProxy>,
        critical: bool,
    ) -> (Job, ElfComponent) {
        let job = scoped_task::create_child_job().expect("failed to make child job");
        let dummy_process = create_dummy_process(&job, "dummy");
        let job_copy = Job::from(
            job.as_handle_ref()
                .duplicate(zx::Rights::SAME_RIGHTS)
                .expect("job handle duplication failed"),
        );
        let component = ElfComponent::new(
            RuntimeDirectory::empty(),
            job_copy,
            dummy_process,
            lifecycle_client,
            critical,
            Vec::new(),
            "".to_string(),
        );
        (job.into_inner(), component)
    }

    // TODO(fxbug.dev/122225): A variation of this is used in a couple of places. We should consider
    // refactoring this into a test util file.
    async fn read_file<'a>(root_proxy: &'a fio::DirectoryProxy, path: &'a str) -> String {
        let file_proxy = fuchsia_fs::directory::open_file_no_describe(
            &root_proxy,
            path,
            fuchsia_fs::OpenFlags::RIGHT_READABLE,
        )
        .expect("Failed to open file.");
        let res = fuchsia_fs::file::read_to_string(&file_proxy).await;
        res.expect("Unable to read file.")
    }

    #[fuchsia::test]
    fn clock_identity_transformation_roundtrip() {
        let t_0 = zx::Time::ZERO;
        // Identity clock transformation
        let xform = zx::ClockTransformation {
            reference_offset: 0,
            synthetic_offset: 0,
            rate: zx::sys::zx_clock_rate_t { synthetic_ticks: 1, reference_ticks: 1 },
        };

        let utc_time = xform.apply(t_0);
        let monotonic_time = xform.apply_inverse(utc_time);

        // Transformation roundtrip should be equivalent with the identity transformation.
        assert_eq!(t_0, monotonic_time);
    }

    #[fuchsia::test]
    fn clock_trivial_transformation() {
        let t_0 = zx::Time::ZERO;
        // Identity clock transformation
        let xform = zx::ClockTransformation {
            reference_offset: 3,
            synthetic_offset: 2,
            rate: zx::sys::zx_clock_rate_t { synthetic_ticks: 6, reference_ticks: 2 },
        };

        let utc_time = xform.apply(t_0);
        let monotonic_time = xform.apply_inverse(utc_time);
        // Verify that the math is correct.
        assert_eq!(3 * (t_0.into_nanos() - 3) + 2, utc_time.into_nanos());

        // Transformation roundtrip should be equivalent.
        assert_eq!(t_0, monotonic_time);
    }

    #[fuchsia::test]
    fn clock_transformation_roundtrip() {
        let t_0 = zx::Time::ZERO;
        // Arbitrary clock transformation
        let xform = zx::ClockTransformation {
            reference_offset: 196980085208,
            synthetic_offset: 1616900096031887801,
            rate: zx::sys::zx_clock_rate_t { synthetic_ticks: 999980, reference_ticks: 1000000 },
        };

        // Transformation roundtrip should be equivalent modulo rounding error.
        let utc_time = xform.apply(t_0);
        let monotonic_time = xform.apply_inverse(utc_time);

        let roundtrip_diff = (t_0.into_nanos() - monotonic_time.into_nanos()).abs();
        assert!(roundtrip_diff <= 1);
    }

    #[fuchsia::test]
    fn clock_trailing_transformation_roundtrip() {
        let t_0 = zx::Time::ZERO;
        // Arbitrary clock transformation where the synthetic clock is trailing behind the
        // reference clock.
        let xform = zx::ClockTransformation {
            reference_offset: 1616900096031887801,
            synthetic_offset: 196980085208,
            rate: zx::sys::zx_clock_rate_t { synthetic_ticks: 1000000, reference_ticks: 999980 },
        };

        // Transformation roundtrip should be equivalent modulo rounding error.
        let utc_time = xform.apply(t_0);
        let monotonic_time = xform.apply_inverse(utc_time);

        let roundtrip_diff = (t_0.into_nanos() - monotonic_time.into_nanos()).abs();
        assert!(roundtrip_diff <= 1);
    }

    #[fuchsia::test]
    fn clock_saturating_transformations() {
        let t_0 = Time::from_nanos(i64::MAX);
        // Clock transformation which will positively overflow t_0
        let xform = zx::ClockTransformation {
            reference_offset: 0,
            synthetic_offset: 1,
            rate: zx::sys::zx_clock_rate_t { synthetic_ticks: 1, reference_ticks: 1 },
        };

        // Applying the transformation will lead to saturation
        let utc_time = xform.apply(t_0);
        assert_eq!(utc_time.into_nanos(), i64::MAX);

        let t_0 = Time::from_nanos(i64::MIN);
        // Clock transformation which will negatively overflow t_0
        let xform = zx::ClockTransformation {
            reference_offset: 1,
            synthetic_offset: 0,
            rate: zx::sys::zx_clock_rate_t { synthetic_ticks: 1, reference_ticks: 1 },
        };

        // Applying the transformation will lead to saturation
        let utc_time = xform.apply(t_0);
        assert_eq!(utc_time.into_nanos(), i64::MIN);
    }

    #[fuchsia::test]
    async fn args_test() -> Result<(), Error> {
        let (runtime_dir_client, runtime_dir_server) = zx::Channel::create();
        let start_info = lifecycle_startinfo(Some(ServerEnd::new(runtime_dir_server)));

        let runtime_dir_proxy = fio::DirectoryProxy::from_channel(
            fasync::Channel::from_channel(runtime_dir_client).unwrap(),
        );

        let config = RuntimeConfig { use_builtin_process_launcher: true, ..Default::default() };
        let runner = new_elf_runner_for_test(&config);
        let runner = runner.get_scoped_runner(ScopedPolicyChecker::new(
            Arc::downgrade(&Arc::new(config)),
            AbsoluteMoniker::root(),
        ));
        let (controller, server_controller) = create_proxy::<fcrunner::ComponentControllerMarker>()
            .expect("could not create component controller endpoints");

        runner.start(start_info, server_controller).await;

        // Verify that args are added to the runtime directory.
        assert_eq!("foo", read_file(&runtime_dir_proxy, "args/0").await);
        assert_eq!("bar", read_file(&runtime_dir_proxy, "args/1").await);

        // Process Id, Process Start Time, Job Id will vary with every run of this test. Here we
        // verify that they exist in the runtime directory, they can be parsed as integers,
        // they're greater than zero and they are not the same value. Those are about the only
        // invariants we can verify across test runs.
        let process_id = read_file(&runtime_dir_proxy, "elf/process_id").await.parse::<u64>()?;
        let process_start_time =
            read_file(&runtime_dir_proxy, "elf/process_start_time").await.parse::<i64>()?;
        let process_start_time_utc_estimate =
            read_file(&runtime_dir_proxy, "elf/process_start_time_utc_estimate").await;
        let job_id = read_file(&runtime_dir_proxy, "elf/job_id").await.parse::<u64>()?;
        assert!(process_id > 0);
        assert!(process_start_time > 0);
        assert!(process_start_time_utc_estimate.contains("UTC"));
        assert!(job_id > 0);
        assert_ne!(process_id, job_id);

        controller.stop().expect("Stop request failed");
        // Wait for the process to exit so the test doesn't pagefault due to an invalid stdout
        // handle.
        controller.on_closed().await.expect("failed waiting for channel to close");
        Ok(())
    }

    #[fuchsia::test]
    async fn test_kill_component() -> Result<(), Error> {
        let (job, component) = make_default_elf_component(None, false);

        let job_info = job.info()?;
        assert!(!job_info.exited);

        component.kill().await;

        let h = job.as_handle_ref();
        fasync::OnSignals::new(&h, zx::Signals::TASK_TERMINATED)
            .await
            .expect("failed waiting for termination signal");

        let job_info = job.info()?;
        assert!(job_info.exited);
        Ok(())
    }

    #[fuchsia::test]
    fn test_stop_critical_component() -> Result<(), Error> {
        let mut exec = fasync::TestExecutor::new();
        // Presence of the Lifecycle channel isn't use by ElfComponent to sense
        // component exit, but it does modify the stop behavior and this is
        // what we want to test.
        let (lifecycle_client, _lifecycle_server) =
            fidl::endpoints::create_proxy_and_stream::<LifecycleMarker>()?;
        let (job, mut component) = make_default_elf_component(Some(lifecycle_client), true);
        let process = component.copy_process().unwrap();
        let job_info = job.info()?;
        assert!(!job_info.exited);

        // Ask the runner to stop the component, it returns a future which
        // completes when the component closes its side of the lifecycle
        // channel
        let mut completes_when_stopped = component.stop();

        // The returned future shouldn't complete because we're holding the
        // lifecycle channel open.
        match exec.run_until_stalled(&mut completes_when_stopped) {
            Poll::Ready(_) => {
                panic!("runner should still be waiting for lifecycle channel to stop");
            }
            _ => {}
        }
        assert_eq!(process.kill(), Ok(()));

        exec.run_singlethreaded(&mut completes_when_stopped);

        // Check that the runner killed the job hosting the exited component.
        let h = job.as_handle_ref();
        let termination_fut = async move {
            fasync::OnSignals::new(&h, zx::Signals::TASK_TERMINATED)
                .await
                .expect("failed waiting for termination signal");
        };
        exec.run_singlethreaded(termination_fut);

        let job_info = job.info()?;
        assert!(job_info.exited);
        Ok(())
    }

    #[fuchsia::test]
    fn test_stop_noncritical_component() -> Result<(), Error> {
        let mut exec = fasync::TestExecutor::new();
        // Presence of the Lifecycle channel isn't use by ElfComponent to sense
        // component exit, but it does modify the stop behavior and this is
        // what we want to test.
        let (lifecycle_client, lifecycle_server) =
            fidl::endpoints::create_proxy_and_stream::<LifecycleMarker>()?;
        let (job, mut component) = make_default_elf_component(Some(lifecycle_client), false);

        let job_info = job.info()?;
        assert!(!job_info.exited);

        // Ask the runner to stop the component, it returns a future which
        // completes when the component closes its side of the lifecycle
        // channel
        let mut completes_when_stopped = component.stop();

        // The returned future shouldn't complete because we're holding the
        // lifecycle channel open.
        match exec.run_until_stalled(&mut completes_when_stopped) {
            Poll::Ready(_) => {
                panic!("runner should still be waiting for lifecycle channel to stop");
            }
            _ => {}
        }
        drop(lifecycle_server);

        match exec.run_until_stalled(&mut completes_when_stopped) {
            Poll::Ready(_) => {}
            _ => {
                panic!("runner future should have completed, lifecycle channel is closed.");
            }
        }
        // Check that the runner killed the job hosting the exited component.
        let h = job.as_handle_ref();
        let termination_fut = async move {
            fasync::OnSignals::new(&h, zx::Signals::TASK_TERMINATED)
                .await
                .expect("failed waiting for termination signal");
        };
        exec.run_singlethreaded(termination_fut);

        let job_info = job.info()?;
        assert!(job_info.exited);
        Ok(())
    }

    /// Stopping a component which doesn't have a lifecycle channel should be
    /// equivalent to killing a component directly.
    #[fuchsia::test]
    async fn test_stop_component_without_lifecycle() -> Result<(), Error> {
        let (job, mut component) = make_default_elf_component(None, false);

        let job_info = job.info()?;
        assert!(!job_info.exited);

        component.stop().await;

        let h = job.as_handle_ref();
        fasync::OnSignals::new(&h, zx::Signals::TASK_TERMINATED)
            .await
            .expect("failed waiting for termination signal");

        let job_info = job.info()?;
        assert!(job_info.exited);
        Ok(())
    }

    #[fuchsia::test]
    async fn test_stop_critical_component_with_closed_lifecycle() -> Result<(), Error> {
        let (lifecycle_client, lifecycle_server) =
            fidl::endpoints::create_proxy_and_stream::<LifecycleMarker>()?;
        let (job, mut component) = make_default_elf_component(Some(lifecycle_client), true);
        let process = component.copy_process().unwrap();
        let job_info = job.info()?;
        assert!(!job_info.exited);

        // Close the lifecycle channel
        drop(lifecycle_server);
        // Kill the process because this is what ElfComponent monitors to
        // determine if the component exited.
        process.kill()?;
        component.stop().await;

        let h = job.as_handle_ref();
        fasync::OnSignals::new(&h, zx::Signals::TASK_TERMINATED)
            .await
            .expect("failed waiting for termination signal");

        let job_info = job.info()?;
        assert!(job_info.exited);
        Ok(())
    }

    #[fuchsia::test]
    async fn test_stop_noncritical_component_with_closed_lifecycle() -> Result<(), Error> {
        let (lifecycle_client, lifecycle_server) =
            fidl::endpoints::create_proxy_and_stream::<LifecycleMarker>()?;
        let (job, mut component) = make_default_elf_component(Some(lifecycle_client), false);

        let job_info = job.info()?;
        assert!(!job_info.exited);

        // Close the lifecycle channel
        drop(lifecycle_server);
        // Kill the process because this is what ElfComponent monitors to
        // determine if the component exited.
        component.stop().await;

        let h = job.as_handle_ref();
        fasync::OnSignals::new(&h, zx::Signals::TASK_TERMINATED)
            .await
            .expect("failed waiting for termination signal");

        let job_info = job.info()?;
        assert!(job_info.exited);
        Ok(())
    }

    /// Dropping the component should kill the job hosting it.
    #[fuchsia::test]
    async fn test_drop() -> Result<(), Error> {
        let (job, component) = make_default_elf_component(None, false);

        let job_info = job.info()?;
        assert!(!job_info.exited);

        drop(component);

        let h = job.as_handle_ref();
        fasync::OnSignals::new(&h, zx::Signals::TASK_TERMINATED)
            .await
            .expect("failed waiting for termination signal");

        let job_info = job.info()?;
        assert!(job_info.exited);
        Ok(())
    }

    /// Modify the standard test StartInfo to request ambient_mark_vmo_exec job policy
    fn lifecycle_startinfo_mark_vmo_exec(
        runtime_dir: Option<ServerEnd<fio::DirectoryMarker>>,
    ) -> fcrunner::ComponentStartInfo {
        let mut start_info = lifecycle_startinfo(runtime_dir);
        start_info.program.as_mut().map(|dict| {
            dict.entries.as_mut().map(|ent| {
                ent.push(fdata::DictionaryEntry {
                    key: "job_policy_ambient_mark_vmo_exec".to_string(),
                    value: Some(Box::new(fdata::DictionaryValue::Str("true".to_string()))),
                });
                ent
            })
        });
        start_info
    }

    // Modify the standard test StartInfo to add a main_process_critical request
    fn hello_world_startinfo_main_process_critical(
        runtime_dir: Option<ServerEnd<fio::DirectoryMarker>>,
    ) -> fcrunner::ComponentStartInfo {
        let mut start_info = hello_world_startinfo(runtime_dir);
        start_info.program.as_mut().map(|dict| {
            dict.entries.as_mut().map(|ent| {
                ent.push(fdata::DictionaryEntry {
                    key: "main_process_critical".to_string(),
                    value: Some(Box::new(fdata::DictionaryValue::Str("true".to_string()))),
                });
                ent
            })
        });
        start_info
    }

    #[fuchsia::test]
    async fn vmex_security_policy_denied() -> Result<(), Error> {
        let start_info = lifecycle_startinfo_mark_vmo_exec(None);

        // Config does not allowlist any monikers to have access to the job policy.
        let config = RuntimeConfig { use_builtin_process_launcher: true, ..Default::default() };
        let runner = new_elf_runner_for_test(&config);
        let runner = runner.get_scoped_runner(ScopedPolicyChecker::new(
            Arc::downgrade(&Arc::new(config)),
            AbsoluteMoniker::root(),
        ));
        let (controller, server_controller) = create_proxy::<fcrunner::ComponentControllerMarker>()
            .expect("could not create component controller endpoints");

        // Attempting to start the component should fail, which we detect by looking for an
        // ACCESS_DENIED epitaph on the ComponentController's event stream.
        runner.start(start_info, server_controller).await;
        assert_matches!(
            controller.take_event_stream().try_next().await,
            Err(fidl::Error::ClientChannelClosed { status: zx::Status::ACCESS_DENIED, .. })
        );

        Ok(())
    }

    #[fuchsia::test]
    async fn vmex_security_policy_allowed() -> Result<(), Error> {
        let (runtime_dir_client, runtime_dir_server) = zx::Channel::create();
        let start_info =
            lifecycle_startinfo_mark_vmo_exec(Some(ServerEnd::new(runtime_dir_server)));
        let runtime_dir_proxy = fio::DirectoryProxy::from_channel(
            fasync::Channel::from_channel(runtime_dir_client).unwrap(),
        );

        let config = Arc::new(RuntimeConfig {
            security_policy: SecurityPolicy {
                job_policy: JobPolicyAllowlists {
                    ambient_mark_vmo_exec: vec![AllowlistEntryBuilder::new().exact("foo").build()],
                    ..Default::default()
                },
                ..Default::default()
            },
            use_builtin_process_launcher: true,
            ..Default::default()
        });
        let runner = new_elf_runner_for_test(&config);
        let runner = runner.get_scoped_runner(ScopedPolicyChecker::new(
            Arc::downgrade(&config),
            AbsoluteMoniker::try_from(vec!["foo"]).unwrap(),
        ));
        let (controller, server_controller) = create_proxy::<fcrunner::ComponentControllerMarker>()
            .expect("could not create component controller endpoints");
        runner.start(start_info, server_controller).await;

        // Runtime dir won't exist if the component failed to start.
        let process_id = read_file(&runtime_dir_proxy, "elf/process_id").await.parse::<u64>()?;
        assert!(process_id > 0);
        // Component controller should get shutdown normally; no ACCESS_DENIED epitaph.
        controller.kill().expect("kill failed");

        // We expect the event stream to have closed, which is reported as an
        // error and the value of the error should match the epitaph for a
        // process that was killed.
        let mut event_stream = controller.take_event_stream();
        expect_diagnostics_event(&mut event_stream).await;
        expect_channel_closed(
            &mut event_stream,
            zx::Status::from_raw(
                i32::try_from(fcomp::Error::InstanceDied.into_primitive()).unwrap(),
            ),
        )
        .await;
        Ok(())
    }

    #[fuchsia::test]
    async fn critical_security_policy_denied() -> Result<(), Error> {
        let start_info = hello_world_startinfo_main_process_critical(None);

        // Config does not allowlist any monikers to be marked as critical
        let config = RuntimeConfig { use_builtin_process_launcher: true, ..Default::default() };
        let runner = new_elf_runner_for_test(&config);
        let runner = runner.get_scoped_runner(ScopedPolicyChecker::new(
            Arc::downgrade(&Arc::new(config)),
            AbsoluteMoniker::root(),
        ));
        let (controller, server_controller) = create_proxy::<fcrunner::ComponentControllerMarker>()
            .expect("could not create component controller endpoints");

        // Attempting to start the component should fail, which we detect by looking for an
        // ACCESS_DENIED epitaph on the ComponentController's event stream.
        runner.start(start_info, server_controller).await;
        assert_matches!(
            controller.take_event_stream().try_next().await,
            Err(fidl::Error::ClientChannelClosed { status: zx::Status::ACCESS_DENIED, .. })
        );

        Ok(())
    }

    #[fuchsia::test]
    #[should_panic]
    async fn fail_to_launch_critical_component() {
        let mut start_info = hello_world_startinfo_main_process_critical(None);

        start_info.program = start_info.program.map(|mut program| {
            program.entries =
                Some(program.entries.unwrap().into_iter().filter(|e| &e.key != "binary").collect());
            program
        });

        // Config does not allowlist any monikers to be marked as critical without being
        // allowlisted, so make sure we permit this one.
        let config = Arc::new(RuntimeConfig {
            use_builtin_process_launcher: true,
            security_policy: SecurityPolicy {
                job_policy: JobPolicyAllowlists {
                    main_process_critical: vec![AllowlistEntryBuilder::new().build()],
                    ..Default::default()
                },
                ..Default::default()
            },
            ..Default::default()
        });
        let runner = new_elf_runner_for_test(&config);
        let runner = runner.get_scoped_runner(ScopedPolicyChecker::new(
            Arc::downgrade(&config),
            AbsoluteMoniker::root(),
        ));
        let (controller, server_controller) = create_proxy::<fcrunner::ComponentControllerMarker>()
            .expect("could not create component controller endpoints");

        runner.start(start_info, server_controller).await;

        // Starting this component isn't actually possible, as start_info is missing the binary
        // field. We should panic before anything happens on the controller stream, because the
        // component is critical.
        controller
            .take_event_stream()
            .try_next()
            .await
            .map(|_: Option<fcrunner::ComponentControllerEvent>| ()) // Discard.
            .unwrap_or_else(|error| warn!(%error, "error reading from event stream"));
    }

    fn hello_world_startinfo_forward_stdout_to_log(
        runtime_dir: Option<ServerEnd<fio::DirectoryMarker>>,
        mut ns: Vec<fcrunner::ComponentNamespaceEntry>,
    ) -> fcrunner::ComponentStartInfo {
        let pkg_path = "/pkg".to_string();
        let pkg_chan = fuchsia_fs::directory::open_in_namespace(
            "/pkg",
            fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE,
        )
        .unwrap()
        .into_channel()
        .unwrap()
        .into_zx_channel();
        let pkg_handle = ClientEnd::new(pkg_chan);

        ns.push(fcrunner::ComponentNamespaceEntry {
            path: Some(pkg_path),
            directory: Some(pkg_handle),
            ..Default::default()
        });

        fcrunner::ComponentStartInfo {
            resolved_url: Some(
                "fuchsia-pkg://fuchsia.com/hello-world#meta/hello-world-rust.cm".to_string(),
            ),
            program: Some(fdata::Dictionary {
                entries: Some(vec![
                    fdata::DictionaryEntry {
                        key: "binary".to_string(),
                        value: Some(Box::new(fdata::DictionaryValue::Str(
                            "bin/hello_world_rust".to_string(),
                        ))),
                    },
                    fdata::DictionaryEntry {
                        key: "forward_stdout_to".to_string(),
                        value: Some(Box::new(fdata::DictionaryValue::Str("log".to_string()))),
                    },
                    fdata::DictionaryEntry {
                        key: "forward_stderr_to".to_string(),
                        value: Some(Box::new(fdata::DictionaryValue::Str("log".to_string()))),
                    },
                ]),
                ..Default::default()
            }),
            ns: Some(ns),
            outgoing_dir: None,
            runtime_dir,
            ..Default::default()
        }
    }

    // TODO(fxbug.dev/69634): Following function shares a lot of code with
    // //src/sys/component_manager/src/model/namespace.rs tests. Shared
    // functionality should be refactored into a common test util lib.
    #[fuchsia::test]
    async fn enable_stdout_and_stderr_logging() -> Result<(), Error> {
        let (dir, ns) = create_fs_with_mock_logsink()?;

        let run_component_fut = async move {
            let (_, runtime_dir_server) = zx::Channel::create();
            let start_info = hello_world_startinfo_forward_stdout_to_log(
                Some(ServerEnd::new(runtime_dir_server)),
                ns,
            );

            let config = RuntimeConfig { use_builtin_process_launcher: true, ..Default::default() };

            let runner = new_elf_runner_for_test(&config);
            let runner = runner.get_scoped_runner(ScopedPolicyChecker::new(
                Arc::downgrade(&Arc::new(config)),
                AbsoluteMoniker::root(),
            ));
            let (client_controller, server_controller) =
                create_proxy::<fcrunner::ComponentControllerMarker>()
                    .expect("could not create component controller endpoints");

            runner.start(start_info, server_controller).await;
            let mut event_stream = client_controller.take_event_stream();
            expect_diagnostics_event(&mut event_stream).await;
            expect_channel_closed(&mut event_stream, zx::Status::OK).await;
        };

        // Just check for connection count, other integration tests cover decoding the actual logs.
        let connection_count = 1u8;
        let request_count = Arc::new(Mutex::new(0u8));
        let request_count_copy = request_count.clone();

        let service_fs_listener_fut = async move {
            dir.for_each_concurrent(None, move |request: MockServiceRequest| match request {
                MockServiceRequest::LogSink(mut r) => {
                    let req_count = request_count_copy.clone();
                    async move {
                        while let Some(Ok(req)) = r.next().await {
                            match req {
                                LogSinkRequest::Connect { .. } => {
                                    panic!("Unexpected call to `Connect`");
                                }
                                LogSinkRequest::ConnectStructured { .. } => {
                                    let mut count = req_count.lock().expect("locking failed");
                                    *count += 1;
                                }
                                LogSinkRequest::WaitForInterestChange { .. } => {
                                    // this is expected but asserting it was received is flakey because
                                    // it's sent at some point after the scoped logger is created
                                }
                            }
                        }
                    }
                }
            })
            .await;
        };

        join!(run_component_fut, service_fs_listener_fut);

        assert_eq!(*request_count.lock().expect("lock failed"), connection_count);
        Ok(())
    }

    #[fuchsia::test]
    async fn on_publish_diagnostics_contains_job_handle() {
        let (runtime_dir_client, runtime_dir_server) = zx::Channel::create();
        let start_info = lifecycle_startinfo(Some(ServerEnd::new(runtime_dir_server)));

        let runtime_dir_proxy = fio::DirectoryProxy::from_channel(
            fasync::Channel::from_channel(runtime_dir_client).unwrap(),
        );

        let config = RuntimeConfig { use_builtin_process_launcher: true, ..Default::default() };
        let runner = new_elf_runner_for_test(&config);
        let runner = runner.get_scoped_runner(ScopedPolicyChecker::new(
            Arc::downgrade(&Arc::new(config)),
            AbsoluteMoniker::root(),
        ));
        let (controller, server_controller) = create_proxy::<fcrunner::ComponentControllerMarker>()
            .expect("could not create component controller endpoints");

        runner.start(start_info, server_controller).await;

        let job_id = read_file(&runtime_dir_proxy, "elf/job_id").await.parse::<u64>().unwrap();
        let mut event_stream = controller.take_event_stream();
        match event_stream.try_next().await {
            Ok(Some(fcrunner::ComponentControllerEvent::OnPublishDiagnostics {
                payload:
                    ComponentDiagnostics {
                        tasks:
                            Some(ComponentTasks {
                                component_task: Some(DiagnosticsTask::Job(job)), ..
                            }),
                        ..
                    },
            })) => {
                assert_eq!(job_id, job.get_koid().unwrap().raw_koid());
            }
            other => panic!("unexpected event result: {:?}", other),
        }

        controller.stop().expect("Stop request failed");
        // Wait for the process to exit so the test doesn't pagefault due to an invalid stdout
        // handle.
        controller.on_closed().await.expect("failed waiting for channel to close");
    }

    async fn expect_diagnostics_event(event_stream: &mut fcrunner::ComponentControllerEventStream) {
        let event = event_stream.try_next().await;
        assert_matches!(
            event,
            Ok(Some(fcrunner::ComponentControllerEvent::OnPublishDiagnostics {
                payload: ComponentDiagnostics {
                    tasks: Some(ComponentTasks {
                        component_task: Some(DiagnosticsTask::Job(_)),
                        ..
                    }),
                    ..
                },
            }))
        );
    }

    async fn expect_channel_closed(
        event_stream: &mut fcrunner::ComponentControllerEventStream,
        expected_status: zx::Status,
    ) {
        let event = event_stream.try_next().await;
        match event {
            Err(fidl::Error::ClientChannelClosed { status, .. }) => {
                assert_eq!(status, expected_status);
            }
            other => panic!("Expected channel closed error, got {:?}", other),
        }
    }
}
