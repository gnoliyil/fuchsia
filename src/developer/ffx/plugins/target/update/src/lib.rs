// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Error;
use ffx_core::ffx_plugin;
use ffx_update_args as args;
use fidl_fuchsia_update::{
    CheckOptions, Initiator, ManagerProxy, MonitorMarker, MonitorRequest, MonitorRequestStream,
};
use fidl_fuchsia_update_channelcontrol::ChannelControlProxy;
use fidl_fuchsia_update_ext::State;
use futures::prelude::*;

/// Main entry point for the `update` subcommand.
#[ffx_plugin(
    "target_update",
    ManagerProxy = "core/system-update-checker:expose:fuchsia.update.Manager",
    ChannelControlProxy = "core/system-update-checker:expose:fuchsia.update.channelcontrol.ChannelControl"
)]
pub async fn update_cmd(
    update_manager_proxy: ManagerProxy,
    channel_control_proxy: ChannelControlProxy,
    update_args: args::Update,
) -> Result<(), Error> {
    update_cmd_impl(
        update_manager_proxy,
        channel_control_proxy,
        update_args,
        &mut std::io::stdout(),
    )
    .await
}

pub async fn update_cmd_impl<W: std::io::Write>(
    update_manager_proxy: ManagerProxy,
    channel_control_proxy: ChannelControlProxy,
    update_args: args::Update,
    writer: &mut W,
) -> Result<(), Error> {
    match update_args.cmd {
        args::Command::Channel(args::Channel { cmd }) => {
            handle_channel_control_cmd(cmd, channel_control_proxy, writer).await?;
        }
        args::Command::CheckNow(check_now) => {
            handle_check_now_cmd(check_now, update_manager_proxy, writer).await?;
        }
        args::Command::ForceInstall(args) => {
            force_install(args.update_pkg_url, args.reboot).await?;
        }
    }
    Ok(())
}

/// Wait for and print state changes. For informational / DX purposes.
async fn monitor_state<W: std::io::Write>(
    mut stream: MonitorRequestStream,
    writer: &mut W,
) -> Result<(), Error> {
    while let Some(event) = stream.try_next().await? {
        match event {
            MonitorRequest::OnState { state, responder } => {
                responder.send()?;

                let state = State::from(state);

                // Exit if we encounter an error during an update.
                if state.is_error() {
                    anyhow::bail!("Update failed: {:?}", state)
                } else {
                    writeln!(writer, "State: {:?}", state)?;
                }
            }
        }
    }
    Ok(())
}

/// Handle subcommands for `update channel`.
async fn handle_channel_control_cmd<W: std::io::Write>(
    cmd: args::channel::Command,
    channel_control: fidl_fuchsia_update_channelcontrol::ChannelControlProxy,
    writer: &mut W,
) -> Result<(), Error> {
    match cmd {
        args::channel::Command::Get(_) => {
            let channel = channel_control.get_current().await?;
            writeln!(writer, "current channel: {}", channel)?;
        }
        args::channel::Command::Target(_) => {
            let channel = channel_control.get_target().await?;
            writeln!(writer, "target channel: {}", channel)?;
        }
        args::channel::Command::Set(args::channel::Set { channel }) => {
            channel_control.set_target(&channel).await?;
        }
        args::channel::Command::List(_) => {
            let channels = channel_control.get_target_list().await?;
            if channels.is_empty() {
                writeln!(writer, "known channels list is empty.")?;
            } else {
                writeln!(writer, "known channels:")?;
                for channel in channels {
                    writeln!(writer, "{}", channel)?;
                }
            }
        }
    }
    Ok(())
}

/// If there's a new version available, update to it, printing progress to the
/// console during the process.
async fn handle_check_now_cmd<W: std::io::Write>(
    cmd: args::CheckNow,
    update_manager: fidl_fuchsia_update::ManagerProxy,
    writer: &mut W,
) -> Result<(), Error> {
    let args::CheckNow { service_initiated, monitor } = cmd;
    let options = CheckOptions {
        initiator: Some(if service_initiated { Initiator::Service } else { Initiator::User }),
        allow_attaching_to_existing_update_check: Some(true),
        ..CheckOptions::EMPTY
    };
    let (monitor_client, monitor_server) = if monitor {
        let (client_end, request_stream) =
            fidl::endpoints::create_request_stream::<MonitorMarker>()?;
        (Some(client_end), Some(request_stream))
    } else {
        (None, None)
    };
    if let Err(e) = update_manager.check_now(options, monitor_client).await? {
        anyhow::bail!("Update check failed to start: {:?}", e);
    }
    writeln!(writer, "Checking for an update.")?;
    if let Some(monitor_server) = monitor_server {
        monitor_state(monitor_server, writer).await?;
    }
    Ok(())
}

/// Change to a specific version, regardless of whether it's newer or older than
/// the current system software.
// TODO(fxbug.dev/60019): implement force install.
async fn force_install(_update_pkg_url: String, _reboot: bool) -> Result<(), Error> {
    eprintln!("The force install is not yet implemented in this tool.");
    eprintln!("In the meantime, please use preexisting tools for a force install.");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_matches::assert_matches;
    use fidl::endpoints::create_proxy_and_stream;
    use fidl_fuchsia_update_channelcontrol::ChannelControlRequest;

    async fn perform_channel_control_test<V, O>(
        argument: args::channel::Command,
        verifier: V,
        output: O,
    ) where
        V: Fn(ChannelControlRequest),
        O: Fn(String),
    {
        let (proxy, mut stream) =
            create_proxy_and_stream::<fidl_fuchsia_update_channelcontrol::ChannelControlMarker>()
                .unwrap();
        let mut buf = Vec::new();
        let fut = async {
            assert_matches!(handle_channel_control_cmd(argument, proxy, &mut buf).await, Ok(()));
        };
        let stream_fut = async move {
            let result = stream.next().await.unwrap();
            match result {
                Ok(cmd) => verifier(cmd),
                err => panic!("Err in request handler: {:?}", err),
            }
        };
        future::join(fut, stream_fut).await;
        let out = String::from_utf8(buf).unwrap();
        output(out);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_channel_get() {
        perform_channel_control_test(
            args::channel::Command::Get(args::channel::Get {}),
            |cmd| match cmd {
                ChannelControlRequest::GetCurrent { responder } => {
                    responder.send("channel").unwrap();
                }
                request => panic!("Unexpected request: {:?}", request),
            },
            |output| assert_eq!(output, "current channel: channel\n"),
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_channel_target() {
        perform_channel_control_test(
            args::channel::Command::Target(args::channel::Target {}),
            |cmd| match cmd {
                ChannelControlRequest::GetTarget { responder } => {
                    responder.send("target-channel").unwrap();
                }
                request => panic!("Unexpected request: {:?}", request),
            },
            |output| assert_eq!(output, "target channel: target-channel\n"),
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_channel_set() {
        perform_channel_control_test(
            args::channel::Command::Set(args::channel::Set { channel: "new-channel".to_string() }),
            |cmd| match cmd {
                ChannelControlRequest::SetTarget { channel, responder } => {
                    assert_eq!(channel, "new-channel");
                    responder.send().unwrap();
                }
                request => panic!("Unexpected request: {:?}", request),
            },
            |output| assert!(output.is_empty()),
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_channel_list_no_channels() {
        perform_channel_control_test(
            args::channel::Command::List(args::channel::List {}),
            |cmd| match cmd {
                ChannelControlRequest::GetTargetList { responder } => {
                    responder.send(&mut vec![].into_iter()).unwrap();
                }
                request => panic!("Unexpected request: {:?}", request),
            },
            |output| assert_eq!(output, "known channels list is empty.\n"),
        )
        .await;
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_channel_list_with_channels() {
        perform_channel_control_test(
            args::channel::Command::List(args::channel::List {}),
            |cmd| match cmd {
                ChannelControlRequest::GetTargetList { responder } => {
                    responder.send(&mut vec!["some-channel", "other-channel"].into_iter()).unwrap();
                }
                request => panic!("Unexpected request: {:?}", request),
            },
            |output| assert_eq!(output, "known channels:\nsome-channel\nother-channel\n"),
        )
        .await;
    }
}
