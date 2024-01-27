// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Error};
use fidl::endpoints::RequestStream;
use fidl_fuchsia_net_mdns::{
    Media, Publication, PublicationResponder_Request, PublicationResponder_RequestStream,
    PublisherMarker, PublisherProxy, ServiceSubscriberRequest, ServiceSubscriberRequestStream,
    SubscriberMarker,
};
use fuchsia_async as fasync;
use fuchsia_zircon as zx;
use futures::prelude::*;
use overnet_core::NodeId;

const SERVICE_NAME: &str = "_overnet._udp.";

async fn connect_to_proxy(
    node_id: NodeId,
    publisher: PublisherProxy,
    port: u16,
    proxy: zx::Channel,
) -> Result<(), Error> {
    tracing::info!(port, "Publish overnet service");

    publisher
        .publish_service_instance(
            SERVICE_NAME,
            &format!("{:?}", node_id.0),
            Media::WIRED | Media::WIRELESS,
            true,
            fidl::endpoints::ClientEnd::new(proxy),
        )
        .await?
        .map_err(|e| format_err!("{:?}", e))?;

    tracing::info!(port, "Published overnet service");
    Ok(())
}

/// Run main loop to publish a udp socket to mdns.
pub async fn publish(port: u16, node_id: NodeId) -> Result<(), Error> {
    let (server, proxy) = zx::Channel::create();
    let server = fasync::Channel::from_channel(server)?;

    let publisher = fuchsia_component::client::connect_to_protocol::<PublisherMarker>()?;

    futures::future::try_join(
        connect_to_proxy(node_id, publisher, port, proxy),
        PublicationResponder_RequestStream::from_channel(server).map_err(Into::into).try_for_each(
            |PublicationResponder_Request::OnPublication { responder, .. }| async move {
                responder
                    .send(Some(&mut Publication {
                        port,
                        text: vec![],
                        srv_priority: fidl_fuchsia_net_mdns::DEFAULT_SRV_PRIORITY,
                        srv_weight: fidl_fuchsia_net_mdns::DEFAULT_SRV_WEIGHT,
                        ptr_ttl: fidl_fuchsia_net_mdns::DEFAULT_PTR_TTL,
                        srv_ttl: fidl_fuchsia_net_mdns::DEFAULT_SRV_TTL,
                        txt_ttl: fidl_fuchsia_net_mdns::DEFAULT_TXT_TTL,
                    }))
                    .map_err(Into::into)
            },
        ),
    )
    .await?;

    Ok(())
}

fn convert_ipv6_buffer(in_arr: [u8; 16]) -> [u16; 8] {
    let mut out_arr: [u16; 8] = [0; 8];

    for i in 0..8 {
        out_arr[i] = ((in_arr[2 * i] as u16) << 8) | (in_arr[2 * i + 1] as u16);
    }

    out_arr
}

fn fuchsia_to_rust_ipaddr4(addr: fidl_fuchsia_net::Ipv4Address) -> std::net::Ipv4Addr {
    std::net::Ipv4Addr::new(addr.addr[0], addr.addr[1], addr.addr[2], addr.addr[3])
}

fn fuchsia_to_rust_ipaddr6(addr: fidl_fuchsia_net::Ipv6Address) -> std::net::Ipv6Addr {
    let addr = convert_ipv6_buffer(addr.addr);
    std::net::Ipv6Addr::new(addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7])
}

fn endpoint4_to_socket(ep: fidl_fuchsia_net::Ipv4SocketAddress) -> std::net::SocketAddrV6 {
    std::net::SocketAddrV6::new(fuchsia_to_rust_ipaddr4(ep.address).to_ipv6_mapped(), ep.port, 0, 0)
}

fn endpoint6_to_socket(ep: fidl_fuchsia_net::Ipv6SocketAddress) -> std::net::SocketAddrV6 {
    std::net::SocketAddrV6::new(fuchsia_to_rust_ipaddr6(ep.address), ep.port, 0, 0)
}

/// Run main loop to look for overnet mdns advertisements and add them to the mesh.
pub async fn subscribe(
    found: futures::channel::mpsc::Sender<std::net::SocketAddrV6>,
) -> Result<(), Error> {
    tracing::info!("Query for overnet services");

    let (server, proxy) = zx::Channel::create();
    fuchsia_component::client::connect_to_protocol::<SubscriberMarker>()?
        .subscribe_to_service(SERVICE_NAME, fidl::endpoints::ClientEnd::new(proxy))?;

    tracing::info!("Wait for overnet services");
    let found = &found;

    ServiceSubscriberRequestStream::from_channel(fasync::Channel::from_channel(server)?)
        .map_err(Into::into)
        .try_for_each(|request| async move {
            match request {
                ServiceSubscriberRequest::OnInstanceDiscovered { instance, responder } => {
                    tracing::info!("Discovered: {:?}", instance);
                    if let Some(ipv4_endpoint) = instance.ipv4_endpoint {
                        found.clone().send(endpoint4_to_socket(ipv4_endpoint)).await?;
                    }
                    if let Some(ipv6_endpoint) = instance.ipv6_endpoint {
                        found.clone().send(endpoint6_to_socket(ipv6_endpoint)).await?;
                    }
                    responder.send()?;
                }
                ServiceSubscriberRequest::OnInstanceChanged { instance, responder } => {
                    tracing::info!("Changed: {:?}", instance);
                    if let Some(ipv4_endpoint) = instance.ipv4_endpoint {
                        found.clone().send(endpoint4_to_socket(ipv4_endpoint)).await?;
                    }
                    if let Some(ipv6_endpoint) = instance.ipv6_endpoint {
                        found.clone().send(endpoint6_to_socket(ipv6_endpoint)).await?;
                    }
                    responder.send()?;
                }
                ServiceSubscriberRequest::OnInstanceLost { responder, .. } => {
                    tracing::info!("Removed a thing");
                    responder.send()?;
                }
                ServiceSubscriberRequest::OnQuery { responder, .. } => {
                    responder.send()?;
                }
            }
            Ok::<_, Error>(())
        })
        .await?;

    tracing::info!("Mdns subscriber finishes");

    Ok(())
}

// [START test_mod]
#[cfg(test)]
mod tests {
    use super::*;
    #[fuchsia::test]
    fn test_fuchsia_to_rust_ipaddr6() {
        //test example IPv6 address [fe80::5054:ff:fe40:5763]
        //fidl_fuchsia_net::Ipv6Address
        let ipv6_addr = fidl_fuchsia_net::Ipv6Address {
            addr: [
                0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x54, 0x00, 0xff, 0xfe, 0x40,
                0x57, 0x63,
            ],
        };

        //std::net::IpAddr
        let net_addr = std::net::IpAddr::V6(std::net::Ipv6Addr::new(
            0xff80, 0x0000, 0x0000, 0x0000, 0x5054, 0x00ff, 0xfe40, 0x5763,
        ));

        //expected:[fuchsia_to_rust_ipaddr6(ipv6_addr)] == [net_addr]
        assert_eq!(fuchsia_to_rust_ipaddr6(ipv6_addr), net_addr);
    }
}
// [END test_mod]
