// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{anyhow, Context, Result};
use async_trait::async_trait;
use blocking::Unblock;
use errors::ffx_bail;
use ffx_component_run_legacy_args::RunComponentCommand;
use fho::{moniker, FfxMain, FfxTool, SimpleWriter};
use fidl::endpoints::create_proxy;
use fidl_fuchsia_sys::{
    ComponentControllerEvent, ComponentControllerMarker, FileDescriptor, LaunchInfo, LauncherProxy,
    TerminationReason::*,
};
use futures::StreamExt;
use signal_hook::{consts::signal::SIGINT, iterator::Signals};

// TODO(fxbug.dev/53159): refactor fuchsia-runtime so we can use the constant from there on the host,
// rather than redefining it here.
const HANDLE_TYPE_FILE_DESCRIPTOR: i32 = 0x30;

#[derive(FfxTool)]
pub struct RunLegacyTool {
    #[command]
    cmd: RunComponentCommand,
    #[with(moniker("/core/appmgr"))]
    launcher: LauncherProxy,
}

fho::embedded_plugin!(RunLegacyTool);

#[async_trait(?Send)]
impl FfxMain for RunLegacyTool {
    type Writer = SimpleWriter;

    async fn main(self, mut writer: Self::Writer) -> fho::Result<()> {
        if !self.cmd.url.ends_with("cmx") {
            ffx_bail!(
                "Invalid component URL! For CML components, use `ffx component run` instead."
            );
        }
        run_component_cmd(self.launcher, self.cmd, &mut writer).await?;
        Ok(())
    }
}

async fn run_component_cmd<W: std::io::Write>(
    launcher_proxy: LauncherProxy,
    run: RunComponentCommand,
    writer: &mut W,
) -> Result<()> {
    let (control_proxy, control_server_end) = create_proxy::<ComponentControllerMarker>()?;
    let (sout, cout) = fidl::Socket::create_stream();
    let (serr, cerr) = fidl::Socket::create_stream();

    let mut stdout = Unblock::new(std::io::stdout());
    let mut stderr = Unblock::new(std::io::stderr());
    let copy_futures = futures::future::try_join(
        futures::io::copy(fidl::AsyncSocket::from_socket(cout)?, &mut stdout),
        futures::io::copy(fidl::AsyncSocket::from_socket(cerr)?, &mut stderr),
    );

    // This is only necessary until Overnet correctly handle setup for passed channels.
    // TODO(jwing) remove this once that is finished.
    control_proxy.detach().unwrap();

    let mut event_stream = control_proxy.take_event_stream();
    let term_event_future = async move {
        while let Some(result) = event_stream.next().await {
            match result? {
                ComponentControllerEvent::OnTerminated { return_code, termination_reason } => {
                    return Ok((return_code, termination_reason));
                }
                _ => {}
            }
        }
        Err(anyhow!("no termination event received"))
    };

    // Force an exit on interrupt.
    let mut signals = Signals::new(&[SIGINT]).unwrap();
    let handle = signals.handle();
    let thread = std::thread::spawn(move || {
        let mut kill_started = false;
        for signal in signals.forever() {
            match signal {
                SIGINT => {
                    if kill_started {
                        println!("\nCaught interrupt, killing remote component.");
                        let _ = control_proxy.kill();
                        kill_started = true;
                    } else {
                        // If for some reason the kill signal hangs, we want to give the user
                        // a way to exit ffx.
                        println!("Received second interrupt. Forcing exit...");
                        std::process::exit(0);
                    }
                }
                _ => unreachable!(),
            }
        }
    });

    let out_fd = FileDescriptor {
        type0: HANDLE_TYPE_FILE_DESCRIPTOR,
        type1: 0,
        type2: 0,
        handle0: Some(sout.into()),
        handle1: None,
        handle2: None,
    };

    let err_fd = FileDescriptor {
        type0: HANDLE_TYPE_FILE_DESCRIPTOR,
        type1: 0,
        type2: 0,
        handle0: Some(serr.into()),
        handle1: None,
        handle2: None,
    };

    let info = LaunchInfo {
        url: run.url.clone(),
        arguments: Some(run.args),
        out: Some(Box::new(out_fd)),
        err: Some(Box::new(err_fd)),
        additional_services: None,
        directory_request: None,
        flat_namespace: None,
    };

    launcher_proxy.create_component(info, Some(control_server_end)).with_context(|| {
        format!("Error starting component. Ensure there is a target connected with `ffx list`")
    })?;

    if run.background {
        writeln!(writer, "Started component: {}", run.url)?;
        return Ok(());
    }

    writeln!(writer, "Started component: {}\nComponent stdout and stderr will be shown below. Press Ctrl+C to exit and kill the component.", run.url)?;

    let (copy_res, term_event) = futures::join!(copy_futures, term_event_future);
    copy_res?;

    let (exit_code, termination_reason) = term_event?;
    if termination_reason != Exited {
        let message = match termination_reason {
            Unknown => "Unknown",
            UrlInvalid => "Component URL is invalid",
            PackageNotFound => "Package could not be found. Ensure `fx serve` is running",
            InternalError => "Internal error",
            ProcessCreationError => "Process creation error",
            RunnerFailed => "Runner failed to start",
            RunnerTerminated => "Runner crashed",
            Unsupported => "Component uses unsupported feature",
            RealmShuttingDown => "Realm is shutting down. Can't create component",
            AccessDenied => "Component did not have sufficient access to run",
            Exited => unreachable!(),
        };
        eprintln!("Error: {}. \nThere may be a more detailed error in the system logs.", message);
    }

    // Shut down the signal thread.
    handle.close();
    thread.join().expect("thread to shutdown without panic");

    std::process::exit(exit_code as i32);
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use super::*;
    use fidl_fuchsia_sys::{LauncherMarker, LauncherRequest};
    use fuchsia_async as fasync;
    use futures::TryStreamExt;

    fn setup_oneshot_fake_launcher_proxy() -> (fasync::Task<()>, LauncherProxy) {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<LauncherMarker>().unwrap();
        let server_task = fasync::Task::local(async move {
            let req = stream.try_next().await;
            match req {
                Ok(Some(LauncherRequest::CreateComponent {
                    controller,
                    control_handle: _,
                    ..
                })) => {
                    let (_, handle) = controller.unwrap().into_stream_and_control_handle().unwrap();
                    handle.send_on_terminated(0, Exited).unwrap();
                    // TODO: Add test coverage for FE behavior once fxbug.dev/49063 is resolved.
                }
                _ => {}
            }
        });
        (server_task, proxy)
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_run_component_cmd() -> Result<()> {
        let url = "fuchsia-pkg://fuchsia.com/test#meta/test.cmx".to_string();
        let args = vec!["test1".to_string(), "test2".to_string()];
        let background = true;
        let run_cmd = RunComponentCommand { url, args, background };
        let (_server_task, launcher_proxy) = setup_oneshot_fake_launcher_proxy();
        let mut writer = Vec::new();
        let _response = run_component_cmd(launcher_proxy, run_cmd, &mut writer)
            .await
            .expect("getting tests should not fail");
        let output = String::from_utf8(writer).unwrap();
        assert_eq!(output, "Started component: fuchsia-pkg://fuchsia.com/test#meta/test.cmx\n");
        Ok(())
    }
}
