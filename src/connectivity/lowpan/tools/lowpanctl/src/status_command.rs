// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::context::LowpanCtlContext;
use crate::prelude::*;
use fidl::endpoints::create_endpoints;
use fidl_fuchsia_lowpan_device::{
    ConnectivityState, DeviceConnectorMarker, DeviceExtraConnectorMarker, DeviceExtraMarker,
    DeviceExtraProxy, DeviceMarker, DeviceProxy,
};
use fidl_fuchsia_lowpan_test::{DeviceTestConnectorMarker, DeviceTestMarker, DeviceTestProxy};
use std::fmt;

#[derive(PartialEq, Debug, Eq)]
enum StatusFormat {
    Standard,
    CSV,
}

impl fmt::Display for StatusFormat {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Debug::fmt(self, f)
    }
}

impl std::str::FromStr for StatusFormat {
    type Err = Error;
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "Standard" | "standard" | "std" => Ok(StatusFormat::Standard),
            "CSV" | "csv" => Ok(StatusFormat::CSV),
            unknown => Err(format_err!("Unknown format {:?}", unknown)),
        }
    }
}

impl Default for StatusFormat {
    fn default() -> Self {
        StatusFormat::Standard
    }
}

/// Contains the arguments decoded for the `status` command.
#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "status")]
pub struct StatusCommand {
    #[argh(
        option,
        long = "format",
        description = "output format (std, csv)",
        default = "Default::default()"
    )]
    format: StatusFormat,
}

async fn print_device_status(
    name: &str,
    device: &DeviceProxy,
    device_extra: &DeviceExtraProxy,
    device_test: &DeviceTestProxy,
) -> Result<(), Error> {
    println!("{}", name);

    if let Some(net_types) = device.get_supported_network_types().await.ok() {
        for (i, net_type) in net_types.iter().enumerate() {
            if i == 0 {
                print!("\ttype: ");
            } else {
                print!(", ");
            }
            print!("{}", net_type);
        }
        if !net_types.is_empty() {
            println!();
        }
    }

    let device_state = device.watch_device_state().await?;

    if let Some(x) = device_state.connectivity_state.as_ref() {
        println!("\tstate: {:?}", x);
    }

    let identity = device_extra.watch_identity().await?;

    if let Some(x) = identity.raw_name {
        match std::str::from_utf8(&x) {
            Ok(x) => println!("\tnetwork_name: {:?}", x),
            Err(e) => println!("\tnetwork_name: {} ({:?})", hex::encode(&x), e),
        }
    }

    if let Some(x) = identity.xpanid {
        println!("\txpanid: {}", hex::encode(x));
    }

    if let Some(x) = identity.panid {
        println!("\tpanid: 0x{:04x}", x);
    }

    match device_state.connectivity_state {
        Some(ConnectivityState::Ready)
        | Some(ConnectivityState::Attaching)
        | Some(ConnectivityState::Attached)
        | Some(ConnectivityState::Isolated) => {
            if let Some(x) = device_state.role.as_ref() {
                println!("\trole: {:?}", x);
            }
        }

        _ => (),
    }

    let current_mac = device_test.get_current_mac_address().await.ok();
    let factory_mac = device_test.get_factory_mac_address().await.ok();

    if let Some(fidl_fuchsia_lowpan::MacAddress { octets }) = current_mac.as_ref() {
        println!("\tcurr-mac: {}", hex::encode(octets));
    }

    if factory_mac != current_mac {
        if let Some(fidl_fuchsia_lowpan::MacAddress { octets }) = factory_mac.as_ref() {
            println!("\tfact-mac: {}", hex::encode(octets));
        }
    }

    if let Some(version) = device_test.get_ncp_version().await.ok() {
        println!("\tncp-version: {:?}", version);
    }

    if let Some(channel) = device_test.get_current_channel().await.ok() {
        println!("\tchan: {}", channel);
    }

    if let Some(rssi) = device_test.get_current_rssi().await.ok() {
        println!("\trssi: {}", rssi);
    }

    if let Some(rloc16) = device_test.get_thread_rloc16().await.ok() {
        println!("\trloc16: 0x{:04x?}", rloc16);
    }

    if let Some(router_id) = device_test.get_thread_router_id().await.ok() {
        println!("\trouter-id: 0x{:04x?}", router_id);
    }

    Ok(())
}

async fn print_device_status_csv(
    name: &str,
    device: &DeviceProxy,
    device_extra: &DeviceExtraProxy,
    device_test: &DeviceTestProxy,
) -> Result<(), Error> {
    if let Some(net_types) = device.get_supported_network_types().await.ok() {
        for net_type in net_types.iter() {
            println!("{}, supported_type, {:?}", name, net_type);
        }
    }

    let device_state = device.watch_device_state().await?;

    if let Some(x) = device_state.connectivity_state.as_ref() {
        println!("{}, state, {:?}", name, x);
    }

    if let Some(x) = device_state.role.as_ref() {
        println!("{}, role, {:?}", name, x);
    }

    match device_state.connectivity_state {
        Some(ConnectivityState::Ready)
        | Some(ConnectivityState::Attaching)
        | Some(ConnectivityState::Attached)
        | Some(ConnectivityState::Isolated) => {
            let identity = device_extra.watch_identity().await?;
            if let Some(x) = identity.raw_name {
                println!("{}, net_raw_name, {}", name, hex::encode(&x));
                match std::str::from_utf8(&x) {
                    Ok(x) => println!("{}, net_name, {:?}", name, x),
                    Err(e) => println!("{}, net_name_err, {:?}", name, e),
                }
            }
            if let Some(x) = identity.xpanid {
                println!("{}, net_xpanid, {:?}", name, x);
            }
            if let Some(x) = identity.channel {
                println!("{}, net_channel, {:?}", name, x);
            }
            if let Some(x) = identity.panid {
                println!("{}, net_panid, {:?}", name, x);
            }
        }

        _ => (),
    }

    let current_mac = device_test.get_current_mac_address().await.ok();
    let factory_mac = device_test.get_factory_mac_address().await.ok();

    if let Some(fidl_fuchsia_lowpan::MacAddress { octets }) = current_mac.as_ref() {
        println!("{}, curr_mac, {}", name, hex::encode(octets));
    }

    if let Some(fidl_fuchsia_lowpan::MacAddress { octets }) = factory_mac.as_ref() {
        println!("{}, fact_mac, {}", name, hex::encode(octets));
    }

    if let Some(x) = device_test.get_ncp_version().await.ok() {
        println!("{}, ncp_version, {:?}", name, x);
    }

    if let Some(x) = device_test.get_current_channel().await.ok() {
        println!("{}, channel, {:?}", name, x);
    }

    if let Some(x) = device_test.get_current_rssi().await.ok() {
        println!("{}, rssi, {:?}", name, x);
    }

    Ok(())
}

impl StatusCommand {
    fn report_interface_error<E: ToString>(&self, name: &str, err: E) {
        match self.format {
            StatusFormat::Standard => {
                println!("{}", &name);
                println!("\terror: {:?}", err.to_string());
            }
            StatusFormat::CSV => {
                println!("{}, error, {:?}", name, err.to_string());
            }
        }
    }
    pub async fn exec(&self, context: &mut LowpanCtlContext) -> Result<(), Error> {
        let lookup = &context.lookup;
        let device_names: Vec<String> = lookup
            .watch_devices()
            .await
            .map_err(std::convert::Into::<Error>::into)
            .context("Unable to list LoWPAN devices")?
            .0;

        if device_names.is_empty() {
            Err(format_err!("No LoWPAN interfaces present"))
        } else {
            if self.format == StatusFormat::CSV {
                println!("ifname, field, value");
            }

            for name in device_names {
                let (client, server) = create_endpoints::<DeviceMarker>();
                let (client_extra, server_extra) = create_endpoints::<DeviceExtraMarker>();
                let (client_test, server_test) = create_endpoints::<DeviceTestMarker>();

                let name = &name;

                if let Err(e) = connect_to_protocol::<DeviceConnectorMarker>()
                    .and_then(|x| x.connect(name, server).map_err(Error::from))
                    .context("Failed to connect to DeviceConnector")
                {
                    self.report_interface_error(name, e);
                }

                if let Err(e) = connect_to_protocol::<DeviceExtraConnectorMarker>()
                    .and_then(|x| x.connect(name, server_extra).map_err(Error::from))
                    .context("Failed to connect to DeviceExtraConnector")
                {
                    self.report_interface_error(name, e);
                }

                if let Err(e) = connect_to_protocol::<DeviceTestConnectorMarker>()
                    .and_then(|x| x.connect(name, server_test).map_err(Error::from))
                    .context("Failed to connect to DeviceTestConnector")
                {
                    self.report_interface_error(name, e);
                }

                let device = match client.into_proxy() {
                    Ok(x) => x,
                    Err(err) => {
                        self.report_interface_error(name, err);
                        continue;
                    }
                };

                let device_extra = match client_extra.into_proxy() {
                    Ok(x) => x,
                    Err(err) => {
                        self.report_interface_error(name, err);
                        continue;
                    }
                };

                let device_diags = match client_test.into_proxy() {
                    Ok(x) => x,
                    Err(err) => {
                        self.report_interface_error(name, err);
                        continue;
                    }
                };

                match self.format {
                    StatusFormat::Standard => {
                        if let Some(e) =
                            print_device_status(name, &device, &device_extra, &device_diags)
                                .await
                                .err()
                        {
                            println!("\terror: {}", e);
                        }
                    }
                    StatusFormat::CSV => {
                        if let Some(e) =
                            print_device_status_csv(name, &device, &device_extra, &device_diags)
                                .await
                                .err()
                        {
                            self.report_interface_error(&name, e);
                        }
                    }
                }

                println!();
            }
            Ok(())
        }
    }
}
