// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::platform::{GuestConsole, PlatformServices, Stdio},
    anyhow::{anyhow, Error},
    fidl::endpoints::create_proxy,
    fidl_fuchsia_virtualization::{GuestMarker, GuestProxy, GuestStatus},
    fuchsia_async as fasync, guest_cli_args as arguments,
    std::fmt,
};

#[derive(Debug, PartialEq, serde::Serialize, serde::Deserialize)]
pub enum AttachResult {
    Attached,
    NotRunning,
    AttachFailure,
}

impl fmt::Display for AttachResult {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AttachResult::Attached => {
                write!(f, "Disconnected from guest after a successful attach")
            }
            AttachResult::NotRunning => write!(f, "Can't attach to a non-running guest"),
            AttachResult::AttachFailure => write!(f, "Failed to attach to guest"),
        }
    }
}

pub async fn handle_attach<P: PlatformServices>(
    services: &P,
    args: &arguments::attach_args::AttachArgs,
) -> Result<AttachResult, Error> {
    let manager = services.connect_to_manager(args.guest_type).await?;
    let status = manager.get_info().await?.guest_status.expect("guest status should always be set");
    if status != GuestStatus::Starting && status != GuestStatus::Running {
        return Ok(AttachResult::NotRunning);
    }

    let (guest_endpoint, guest_server_end) = create_proxy::<GuestMarker>()
        .map_err(|err| anyhow!("failed to create guest proxy: {}", err))?;
    manager
        .connect(guest_server_end)
        .await
        .map_err(|err| anyhow!("failed to get a connect response: {}", err))?
        .map_err(|err| anyhow!("connect failed with: {:?}", err))?;

    Ok(match attach(guest_endpoint).await {
        Ok(()) => AttachResult::Attached,
        Err(_) => AttachResult::AttachFailure,
    })
}

// Attach to a running guest, using a combined stdout and serial for output, and stdin for input.
pub async fn attach(guest: GuestProxy) -> Result<(), Error> {
    // Tie serial output to stdout.
    let guest_serial_response = guest.get_serial().await?;
    let guest_serial = fasync::Socket::from_socket(guest_serial_response)?;
    let serial_output = async {
        futures::io::copy(guest_serial, &mut GuestConsole::get_unblocked_stdio(Stdio::Stdout))
            .await
            .map(|_| ())
            .map_err(anyhow::Error::from)
    };

    // Host doesn't currently support duplicating Fuchsia handles, so just call get console twice
    // and let the VMM duplicate the socket for reading and writing.
    let console_input = guest.get_console().await?.map_err(|err| anyhow!(format!("{:?}", err)))?;
    let console_output = guest.get_console().await?.map_err(|err| anyhow!(format!("{:?}", err)))?;
    let guest_console = GuestConsole::new(console_input, console_output)?;

    futures::future::try_join(serial_output, guest_console.run_with_stdio())
        .await
        .map(|_| ())
        .map_err(anyhow::Error::from)
}

#[cfg(test)]
mod test {
    use {
        super::*,
        fidl::{endpoints::create_proxy_and_stream, Socket, SocketOpts},
        fidl_fuchsia_virtualization::GuestError,
        futures::future::join,
        futures::StreamExt,
    };

    #[fuchsia_async::run_until_stalled(test)]
    async fn launch_invalid_console_returns_error() {
        let (guest_proxy, mut guest_stream) = create_proxy_and_stream::<GuestMarker>().unwrap();
        let (serial_launch_sock, _serial_server_sock) = Socket::create(SocketOpts::STREAM).unwrap();

        let server = async move {
            let serial_responder = guest_stream
                .next()
                .await
                .expect("Failed to read from stream")
                .expect("Failed to parse request")
                .into_get_serial()
                .expect("Unexpected call to Guest Proxy");
            serial_responder.send(serial_launch_sock).expect("Failed to send request to proxy");

            let console_responder = guest_stream
                .next()
                .await
                .expect("Failed to read from stream")
                .expect("Failed to parse request")
                .into_get_console()
                .expect("Unexpected call to Guest Proxy");
            console_responder
                .send(&mut Err(GuestError::DeviceNotPresent))
                .expect("Failed to send request to proxy");
        };

        let client = attach(guest_proxy);
        let (_, client_res) = join(server, client).await;
        assert!(client_res.is_err());
    }
}
