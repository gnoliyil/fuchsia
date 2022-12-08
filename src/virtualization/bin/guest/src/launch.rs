// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
    crate::arguments,
    crate::services,
    anyhow::{anyhow, Context, Error},
    fidl_fuchsia_virtualization::{GuestConfig, GuestMarker, GuestProxy},
    fuchsia_async as fasync,
};

pub async fn handle_launch(args: &arguments::LaunchArgs) -> Result<(), Error> {
    let config = parse_vmm_args(args);
    let guest = launch(args.guest_type, config).await?;
    if args.detach {
        Ok(())
    } else {
        attach(guest).await
    }
}

fn parse_vmm_args(arguments: &arguments::LaunchArgs) -> GuestConfig {
    // FIDL requires we make a GuestConfig::EMPTY before trying to update fields
    let mut guest_config = GuestConfig::EMPTY;

    if !arguments.cmdline_add.is_empty() {
        guest_config.cmdline_add = Some(arguments.cmdline_add.clone())
    };

    guest_config.guest_memory = arguments.memory;
    guest_config.cpus = arguments.cpus;
    guest_config.default_net = arguments.default_net;
    guest_config.virtio_balloon = arguments.virtio_balloon;
    guest_config.virtio_console = arguments.virtio_console;
    guest_config.virtio_gpu = arguments.virtio_gpu;
    guest_config.virtio_rng = arguments.virtio_rng;
    guest_config.virtio_sound = arguments.virtio_sound;
    guest_config.virtio_sound_input = arguments.virtio_sound_input;
    guest_config.virtio_vsock = arguments.virtio_vsock;

    guest_config
}

// Connect to a guest manager and launch the corresponding guest.
async fn launch(
    guest_type: arguments::GuestType,
    config: GuestConfig,
) -> Result<GuestProxy, Error> {
    let (guest, guest_server_end) =
        fidl::endpoints::create_proxy::<GuestMarker>().context("Failed to create Guest")?;

    println!("Starting {}", guest_type.to_string());
    let manager = services::connect_to_manager(guest_type)?;
    let fidl_result = manager.launch(config, guest_server_end).await;
    if let Err(fidl::Error::ClientChannelClosed { .. }) = fidl_result {
        eprintln!("");
        eprintln!("Unable to connect to start the guest.");
        eprintln!("  Ensure you have the guest and core shards available on in your build:");
        eprintln!("      fx set ... \\");
        eprintln!("          --with-base {} \\", guest_type.gn_target_label());
        eprintln!(
            "          --args='core_realm_shards += [ \"{}\" ]'",
            guest_type.gn_core_shard_label()
        );
        eprintln!("");
        return Err(anyhow!("Unable to start guest: {}", fidl_result.unwrap_err()));
    }
    fidl_result?.map_err(|err| anyhow!("{:?}", err))?;
    Ok(guest)
}

// Attach to a running guest, using a combined stdout and serial for output, and stdin for input.
async fn attach(guest: GuestProxy) -> Result<(), Error> {
    let guest_serial_response = guest.get_serial().await?;
    let guest_console_response =
        guest.get_console().await?.map_err(|err| anyhow!(format!("{:?}", err)))?;

    let guest_serial = fasync::Socket::from_socket(guest_serial_response)?;
    let guest_console = services::GuestConsole::new(guest_console_response)?;

    // SAFETY: These blocks are unsafe as they capture process local handles, and thus can
    // only be called once per stdio direction. See get_evented_stdio's implementation comments
    // for an explanation of the safe usage.
    let stdout = unsafe { services::get_evented_stdio(services::Stdio::Stdout) };
    let stdin = unsafe { services::get_evented_stdio(services::Stdio::Stdin) };

    let serial_output = async {
        futures::io::copy(guest_serial, &mut &stdout).await.map(|_| ()).map_err(anyhow::Error::from)
    };

    futures::future::try_join(serial_output, guest_console.run(stdin, &stdout))
        .await
        .map(|_| ())
        .map_err(anyhow::Error::from)
}

#[cfg(test)]
mod test {
    use {
        super::*,
        fidl::endpoints::create_proxy_and_stream,
        fidl_fuchsia_virtualization::GuestError,
        fuchsia_zircon::{self as zx},
        futures::future::join,
        futures::StreamExt,
    };

    #[fasync::run_until_stalled(test)]
    async fn launch_invalid_console_returns_error() {
        let (guest_proxy, mut guest_stream) = create_proxy_and_stream::<GuestMarker>().unwrap();
        let (serial_launch_sock, _serial_server_sock) =
            zx::Socket::create(zx::SocketOpts::STREAM).unwrap();

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
