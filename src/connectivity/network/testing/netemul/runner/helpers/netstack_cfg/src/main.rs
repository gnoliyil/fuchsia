// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    fidl::endpoints::Proxy as _,
    fidl_fuchsia_net,
    fidl_fuchsia_netemul_network::{EndpointManagerMarker, NetworkContextMarker},
    fuchsia_async as fasync,
    fuchsia_component::client,
    fuchsia_zircon_status as zx,
    futures::TryStreamExt as _,
    netstack_testing_common::interfaces::add_address_wait_assigned,
    std::convert::TryFrom as _,
    structopt::StructOpt,
};

#[derive(StructOpt, Debug)]
#[structopt(name = "netstack_cfg")]
/// Configure netstack from emulated environments.
struct Opt {
    #[structopt(long, short = "e")]
    /// Endpoint name to retrieve from network.EndpointManager
    endpoint: String,
    #[structopt(long, short = "i")]
    /// Static ip addresses to assign (don't forget /prefix termination). Omit to use DHCP
    ips: Vec<String>,
    #[structopt(long, short = "g")]
    /// Ip address of the default gateway (useful when DHCP is not used)
    gateway: Option<String>,
    #[structopt(long = "skip-up-check")]
    /// netstack_cfg will by default wait until it sees the interface be reported as "Up",
    /// skip-up-check will override that behavior
    skip_up_check: bool,
    #[structopt(long = "without-autogenerated-addresses")]
    /// If set, netstack_cfg will ensure that the interface is brought up without any
    /// autogenerated addresses
    without_autogenerated_addresses: bool,
}

const DEFAULT_METRIC: u32 = 100;

async fn config_netstack(opt: Opt) -> Result<(), Error> {
    let Opt { endpoint, ips, gateway, skip_up_check, without_autogenerated_addresses } = opt;
    log::info!("Configuring endpoint {}", &endpoint);

    // get the network context service:
    let netctx = client::connect_to_protocol::<NetworkContextMarker>()?;
    // get the endpoint manager
    let (epm, epmch) = fidl::endpoints::create_proxy::<EndpointManagerMarker>()?;
    netctx.get_endpoint_manager(epmch)?;

    // retrieve the created endpoint:
    let ep = epm.get_endpoint(&endpoint).await?;
    let ep =
        ep.ok_or_else(|| anyhow::anyhow!("can't find endpoint {}", &endpoint))?.into_proxy()?;
    log::info!("Got endpoint.");
    // and the device connection:
    let device_connection = ep.get_device().await?;
    log::info!("Got device connection.");

    // connect to netstack:
    let stack = client::connect_to_protocol::<fidl_fuchsia_net_stack::StackMarker>()?;
    let netstack = client::connect_to_protocol::<fidl_fuchsia_netstack::NetstackMarker>()?;
    let debug_interfaces =
        client::connect_to_protocol::<fidl_fuchsia_net_debug::InterfacesMarker>()?;

    let (control, server_end) = fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints()
        .context("failed to create control endpoints")?;

    let nicid = match device_connection {
        fidl_fuchsia_netemul_network::DeviceConnection::Ethernet(e) => {
            let mut cfg = fidl_fuchsia_netstack::InterfaceConfig {
                name: endpoint.clone(),
                filepath: format!("/vdev/{}", &endpoint),
                metric: DEFAULT_METRIC,
            };
            let nicid = netstack
                .add_ethernet_device(&format!("/vdev/{}", &endpoint), &mut cfg, e)
                .await
                .with_context(|| format!("add_ethernet_device FIDL error ({:?})", cfg))?
                .map_err(zx::Status::from_raw)
                .with_context(|| format!("add_ethernet_device error ({:?})", cfg))?;
            let nicid = nicid.into();

            let () = debug_interfaces.get_admin(nicid, server_end).context("calling get_admin")?;
            nicid
        }
        fidl_fuchsia_netemul_network::DeviceConnection::NetworkDevice(device_instance) => {
            let device = {
                let device_instance = device_instance
                    .into_proxy()
                    .context("failed to create device instance proxy")?;
                let (proxy, server_end) =
                    fidl::endpoints::create_proxy::<fidl_fuchsia_hardware_network::DeviceMarker>()
                        .context("failed to create device proxy")?;
                let () = device_instance.get_device(server_end).context("get_device failed")?;
                proxy
            };

            let port_watcher = {
                let (proxy, server_end) = fidl::endpoints::create_proxy::<
                    fidl_fuchsia_hardware_network::PortWatcherMarker,
                >()
                .context("failed to create port watcher proxy")?;
                let () = device.get_port_watcher(server_end).context("get_port_watcher")?;
                proxy
            };

            // Get a port from the device.
            let mut port_id = loop {
                let port_event = port_watcher.watch().await.context("watch")?;
                match port_event {
                    fidl_fuchsia_hardware_network::DevicePortEvent::Existing(port_id)
                    | fidl_fuchsia_hardware_network::DevicePortEvent::Added(port_id) => {
                        break port_id;
                    }
                    fidl_fuchsia_hardware_network::DevicePortEvent::Idle(
                        fidl_fuchsia_hardware_network::Empty {},
                    )
                    | fidl_fuchsia_hardware_network::DevicePortEvent::Removed(_) => (),
                }
            };
            let installer =
                client::connect_to_protocol::<fidl_fuchsia_net_interfaces_admin::InstallerMarker>()
                    .context("connect to installer")?;
            let device_control = {
                let (proxy, server_end) = fidl::endpoints::create_proxy::<
                    fidl_fuchsia_net_interfaces_admin::DeviceControlMarker,
                >()
                .context("failed to create device control endpoints")?;
                let device = device.into_channel().map_err(
                    |fidl_fuchsia_hardware_network::DeviceProxy { .. }| {
                        anyhow::anyhow!("failed to retrieve inner channel")
                    },
                )?;
                let () = installer
                    .install_device(
                        fidl::endpoints::ClientEnd::new(device.into_zx_channel()),
                        server_end,
                    )
                    .context("install device")?;
                proxy
            };

            let () = device_control
                .create_interface(
                    &mut port_id,
                    server_end,
                    fidl_fuchsia_net_interfaces_admin::Options {
                        name: Some(endpoint.clone()),
                        metric: Some(DEFAULT_METRIC),
                        ..fidl_fuchsia_net_interfaces_admin::Options::EMPTY
                    },
                )
                .context("create interface")?;

            // Allow interface to live on after this process exits.
            let () = device_control.detach().context("device control detach")?;
            let () = control.detach().context("control detach")?;

            control.get_id().await.context("get id")?
        }
    };

    let _: bool = control.enable().await.context("call enable")?.map_err(
        |e: fidl_fuchsia_net_interfaces_admin::ControlEnableError| {
            anyhow::anyhow!("enable interface: {:?}", e)
        },
    )?;

    log::info!("Waiting for interface up...");
    let interface_state =
        client::connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()?;
    let () = fidl_fuchsia_net_interfaces_ext::wait_interface_with_id(
        fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)?,
        &mut fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(nicid),
        |fidl_fuchsia_net_interfaces_ext::Properties { online, .. }| {
            (skip_up_check || *online).then(|| ())
        },
    )
    .await
    .context("wait for interface")?;

    log::info!("Added ethernet with id {}", nicid);

    // TODO(fxbug.dev/74595): Expose an option to preempt address autogeneration instead of
    // removing them after the fact.
    if without_autogenerated_addresses {
        log::info!("Listening for link-local IPv6 address generation events from the netstack.");
        let interface_state =
            client::connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()?;
        let mut state = fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(nicid);
        let () = fidl_fuchsia_net_interfaces_ext::wait_interface_with_id(
            fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)?,
            &mut state,
            |fidl_fuchsia_net_interfaces_ext::Properties { addresses, .. }| {
                if addresses.is_empty() {
                    log::info!(
                        "Interface with id {} posted event with no generated addresses.",
                        nicid
                    );
                    None
                } else {
                    Some(())
                }
            },
        )
        .await
        .context("error listening for link-local IPv6 address generation events")?;

        let autogen_link_local_addrs = match state {
            fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(nicid) => {
                return Err(anyhow::anyhow!("Interface with id {} state unknown", nicid))
            }
            fidl_fuchsia_net_interfaces_ext::InterfaceState::Known(
                fidl_fuchsia_net_interfaces_ext::Properties { addresses, .. },
            ) => addresses,
        };

        match autogen_link_local_addrs.as_slice() {
            [address] => {
                let fidl_fuchsia_net_interfaces_ext::Address {
                    addr: fidl_fuchsia_net::Subnet { addr, prefix_len },
                    valid_until: _,
                } = *address;
                let mut interface_address = match addr {
                    fidl_fuchsia_net::IpAddress::Ipv4(addr) => {
                        fidl_fuchsia_net::InterfaceAddress::Ipv4(
                            fidl_fuchsia_net::Ipv4AddressWithPrefix { addr, prefix_len },
                        )
                    }
                    fidl_fuchsia_net::IpAddress::Ipv6(addr) => {
                        fidl_fuchsia_net::InterfaceAddress::Ipv6(addr)
                    }
                };
                let _: bool = control
                    .remove_address(&mut interface_address)
                    .await
                    .context("send remove address request")?
                    .map_err(
                        |e: fidl_fuchsia_net_interfaces_admin::ControlRemoveAddressError| {
                            anyhow::anyhow!(
                            "failed to remove interface address {:?} to interface (id={}): {:?}",
                            addr,
                            nicid,
                            e
                        )
                        },
                    )?;
            }
            multiple_addrs => {
                return Err(anyhow::anyhow!(
                    "expected to find one autogenerated link-local address, instead found {:?}",
                    multiple_addrs
                ))
            }
        }
    }

    let subnets: Vec<fidl_fuchsia_net::Subnet> = ips
        .iter()
        .map(|ip| {
            ip.parse::<fidl_fuchsia_net_ext::Subnet>()
                .with_context(|| format!("can't parse provided ip {}", ip))
                .map(|subnet| subnet.into())
        })
        .collect::<Result<Vec<_>, _>>()?;

    if subnets.is_empty() {
        let nicid_u32 =
            u32::try_from(nicid).with_context(|| format!("{} does not fit in a u32", nicid))?;

        let (dhcp_client, server_end) =
            fidl::endpoints::create_proxy::<fidl_fuchsia_net_dhcp::ClientMarker>()
                .context("failed to create fidl endpoints")?;
        netstack
            .get_dhcp_client(nicid_u32, server_end)
            .await
            .context("failed to call get_dhcp_client")?
            .map_err(zx::Status::from_raw)
            .context("failed to get dhcp client")?;
        dhcp_client
            .start()
            .await
            .context("failed to call dhcp_client.start")?
            .map_err(zx::Status::from_raw)
            .context("failed to start dhcp client")?;
    } else {
        let futs = subnets.iter().copied().map(|subnet| {
            let fidl_fuchsia_net::Subnet { addr, prefix_len } = subnet;
            let interface_address = match addr {
                fidl_fuchsia_net::IpAddress::Ipv4(addr) => {
                    fidl_fuchsia_net::InterfaceAddress::Ipv4(
                        fidl_fuchsia_net::Ipv4AddressWithPrefix { addr, prefix_len },
                    )
                }
                fidl_fuchsia_net::IpAddress::Ipv6(addr) => {
                    fidl_fuchsia_net::InterfaceAddress::Ipv6(addr)
                }
            };
            let control = &control;
            let stack = &stack;
            Ok(async move {
                let address_state_provider = add_address_wait_assigned(
                    control,
                    interface_address,
                    fidl_fuchsia_net_interfaces_admin::AddressParameters::EMPTY,
                )
                .await
                .context("add interface address")?;
                // Allow this address to live on after this process exits.
                let () = address_state_provider.detach().context("address detach")?;
                let subnet = fidl_fuchsia_net_ext::apply_subnet_mask(subnet);
                let () = stack
                    .add_forwarding_entry(&mut fidl_fuchsia_net_stack::ForwardingEntry {
                        subnet,
                        device_id: nicid,
                        next_hop: None,
                        metric: 0,
                    })
                    .await
                    .context("send add forwarding entry request")?
                    .map_err(|e: fidl_fuchsia_net_stack::Error| {
                        anyhow::anyhow!("failed to add forwarding entry: {:?}", e)
                    })?;
                Ok::<_, Error>(())
            })
        });
        futures::stream::iter(futs).try_for_each_concurrent(None, std::convert::identity).await?;
    };

    log::info!("Configured nic address.");

    if let Some(gateway) = gateway {
        let gw_addr: fidl_fuchsia_net::IpAddress = fidl_fuchsia_net_ext::IpAddress(
            gateway.parse::<std::net::IpAddr>().context("failed to parse gateway address")?,
        )
        .into();
        let unspec_addr: fidl_fuchsia_net::IpAddress = match gw_addr {
            fidl_fuchsia_net::IpAddress::Ipv4(..) => fidl_fuchsia_net_ext::IpAddress(
                std::net::IpAddr::V4(std::net::Ipv4Addr::UNSPECIFIED),
            ),
            fidl_fuchsia_net::IpAddress::Ipv6(..) => fidl_fuchsia_net_ext::IpAddress(
                std::net::IpAddr::V6(std::net::Ipv6Addr::UNSPECIFIED),
            ),
        }
        .into();

        let mut entry = fidl_fuchsia_net_stack::ForwardingEntry {
            subnet: fidl_fuchsia_net::Subnet { addr: unspec_addr, prefix_len: 0 },
            device_id: nicid.into(),
            next_hop: Some(Box::new(gw_addr)),
            metric: 0,
        };
        let () = stack
            .add_forwarding_entry(&mut entry)
            .await
            .with_context(|| format!("fidl error calling route_proxy.add_route {:?}", entry))?
            .map_err(|e: fidl_fuchsia_net_stack::Error| {
                anyhow::anyhow!("error adding route {:?}: {:?}", entry, e)
            })?;

        log::info!("Configured the default route with gateway address.");
    }

    Ok(())
}

fn main() -> Result<(), Error> {
    let () = fuchsia_syslog::init().context("cannot init logger")?;

    let opt = Opt::from_args();
    let mut executor = fasync::LocalExecutor::new().context("Error creating executor")?;
    executor.run_singlethreaded(config_netstack(opt))
}
