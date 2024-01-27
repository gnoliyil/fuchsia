// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;
use fidl::endpoints::create_endpoints;
use fidl::endpoints::Proxy;
use fidl_fuchsia_net_mdns::*;
use fuchsia_async::Task;
use fuchsia_component::client::connect_to_protocol;
use parking_lot::Mutex;
use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::sync::Arc;

/// The advertising proxy handles taking hosts and services registered with the SRP server
/// and republishing them via local mDNS.
#[derive(Debug)]
pub struct AdvertisingProxy {
    inner: Arc<Mutex<AdvertisingProxyInner>>,
}

impl Drop for AdvertisingProxy {
    fn drop(&mut self) {
        // Make sure all advertised hosts get cleaned up.
        self.inner.lock().hosts.clear();
    }
}

#[derive(Debug)]
struct AdvertisingProxyInner {
    srp_domain: String,
    hosts: HashMap<CString, AdvertisingProxyHost>,
    mdns_proxy_host_publisher: ProxyHostPublisherProxy,
}

#[derive(Debug)]
pub struct AdvertisingProxyHost {
    services: HashMap<CString, AdvertisingProxyService>,
    service_publisher: ServiceInstancePublisherProxy,
    addresses: Vec<std::net::Ipv6Addr>,
}

#[derive(Debug, PartialEq, Eq, Clone)]
pub struct AdvertisingProxyServiceInfo {
    txt: Vec<Vec<u8>>,
    port: u16,
    priority: u16,
    weight: u16,
}

impl AdvertisingProxyServiceInfo {
    fn new(srp_service: &ot::SrpServerService) -> Self {
        AdvertisingProxyServiceInfo {
            txt: srp_service.txt_entries().map(|x| x.unwrap().to_vec()).collect::<Vec<_>>(),
            port: srp_service.port(),
            priority: srp_service.priority(),
            weight: srp_service.weight(),
        }
    }

    fn is_up_to_date(&self, srp_service: &ot::SrpServerService) -> bool {
        !srp_service.is_deleted() && self == &AdvertisingProxyServiceInfo::new(srp_service)
    }

    fn into_service_instance_publication(self) -> ServiceInstancePublication {
        ServiceInstancePublication {
            port: Some(self.port),
            text: Some(self.txt),
            srv_priority: Some(self.priority),
            srv_weight: Some(self.weight),
            ..Default::default()
        }
    }
}

#[derive(Debug)]
pub struct AdvertisingProxyService {
    info: Arc<Mutex<AdvertisingProxyServiceInfo>>,

    control_handle: ServiceInstancePublicationResponder_ControlHandle,

    #[allow(dead_code)]
    task: Task<Result>,
}

impl AdvertisingProxyService {
    fn is_up_to_date(&self, srp_service: &ot::SrpServerService) -> bool {
        self.info.lock().is_up_to_date(srp_service)
    }

    fn update(&self, info: AdvertisingProxyServiceInfo) -> Result<(), anyhow::Error> {
        *self.info.lock() = info;
        Ok(self.control_handle.send_reannounce()?)
    }
}

impl AdvertisingProxy {
    pub fn new(instance: &ot::Instance) -> Result<AdvertisingProxy, anyhow::Error> {
        let inner = Arc::new(Mutex::new(AdvertisingProxyInner {
            srp_domain: instance.srp_server_get_domain().to_str()?.to_string(),
            hosts: Default::default(),
            mdns_proxy_host_publisher: connect_to_protocol::<ProxyHostPublisherMarker>()?,
        }));
        let ret = AdvertisingProxy { inner: inner.clone() };

        ret.inner.lock().publish_srp_all(instance)?;

        instance.srp_server_set_service_update_fn(Some(
            move |ot_instance: &ot::Instance,
                  update_id: ot::SrpServerServiceUpdateId,
                  host: &ot::SrpServerHost,
                  timeout: u32| {
                debug!(
                    tag = "srp_advertising_proxy",
                    "srp_server_set_service_update: Update for {:?}, timeout: {}", host, timeout
                );
                let result = inner.lock().push_srp_host_changes(instance, host);

                if let Err(err) = &result {
                    warn!(
                        tag = "srp_advertising_proxy",
                        "srp_server_set_service_update: Error publishing {:?}: {:?}", host, err
                    );
                } else {
                    debug!(
                        tag = "srp_advertising_proxy",
                        "srp_server_set_service_update: Finished publishing {:?}", host
                    );
                }

                ot_instance.srp_server_handle_service_update_result(
                    update_id,
                    result.map_err(|_: anyhow::Error| ot::Error::Failed),
                );
            },
        ));

        info!(tag = "srp_advertising_proxy", "AdvertisingProxy Started");

        Ok(ret)
    }
}

impl AdvertisingProxyInner {
    pub fn publish_srp_all(&mut self, instance: &ot::Instance) -> Result<(), anyhow::Error> {
        for host in instance.srp_server_hosts() {
            if let Err(err) = self.push_srp_host_changes(instance, host) {
                warn!(
                    tag = "srp_advertising_proxy",
                    "Unable to fully publish SRP host {:?} to mDNS: {:?}",
                    host.full_name_cstr(),
                    err
                );
            }
        }

        Ok(())
    }

    /// Updates the mDNS service with the host and services from the SrpServerHost.
    pub fn push_srp_host_changes<'a>(
        &mut self,
        instance: &'a ot::Instance,
        mut srp_host: &'a ot::SrpServerHost,
    ) -> Result<(), anyhow::Error> {
        if srp_host.is_deleted() {
            // Delete the host.
            debug!(
                tag = "srp_advertising_proxy",
                "No longer advertising host {:?} on {:?}",
                srp_host.full_name_cstr(),
                LOCAL_DOMAIN
            );

            self.hosts.remove(srp_host.full_name_cstr());
            return Ok(());
        }

        // Cache the mesh local prefix for use in the closure below.
        let mesh_local_prefix = *instance.get_mesh_local_prefix();

        // Prepare the list of addresses associated with this host.
        let addresses = srp_host
            .addresses()
            .iter()
            .copied()
            .filter(|x| {
                !net_types::ip::Ipv6Addr::from_bytes(x.octets()).is_unicast_link_local()
                    && !mesh_local_prefix.contains(x)
            })
            .collect::<Vec<_>>();

        // If there is already a host, check to make sure the addresses match (<fxbug.dev/115170>)
        // and that the service publisher FIDL is not closed.
        if let Some(host) = self.hosts.get_mut(srp_host.full_name_cstr()) {
            if host.addresses != addresses {
                // Addresses do not match.
                info!(
                    tag = "srp_advertising_proxy",
                    "IP addresses for host [PII]({:?}) has changed. Was {:?}, now {:?}.",
                    srp_host.full_name_cstr(),
                    host.addresses,
                    addresses
                );
                // Delete the host so we can re-create it below.
                self.hosts.remove(srp_host.full_name_cstr());
            } else if host.service_publisher.is_closed() {
                // The service publisher was closed for some reason. We will need
                // to re-open it before we can update any services.
                warn!(
                    tag="srp_advertising_proxy", "ServiceInstancePublisherProxy for host [PII]({:?}) was closed. Will restart it.",
                    srp_host.full_name_cstr()
                );
                // Delete the host so we can re-create it below.
                self.hosts.remove(srp_host.full_name_cstr());
            }
        }

        // If there are no addresses that we can advertise, stop here. This should
        // help prevent some spurious and confusing error logs.
        if addresses.is_empty() {
            info!(
                tag = "srp_advertising_proxy",
                "No suitable addresses for host [PII]({:?}). Skipping advertising it for now.",
                srp_host.full_name_cstr()
            );
            return Ok(());
        }

        let host: &mut AdvertisingProxyHost =
            if let Some(host) = self.hosts.get_mut(srp_host.full_name_cstr()) {
                // Use the existing host.
                debug!(
                    tag = "srp_advertising_proxy",
                    "Updating advertisement of {:?} on {:?}",
                    srp_host.full_name_cstr(),
                    LOCAL_DOMAIN
                );

                host
            } else {
                // Add the host.
                let local_name = srp_host
                    .full_name_cstr()
                    .as_ref()
                    .to_str()?
                    .trim_end_matches(&self.srp_domain)
                    .trim_end_matches('.');

                debug!(
                    tag = "srp_advertising_proxy",
                    "Advertising host {:?} on {:?} as {:?}",
                    srp_host.full_name_cstr(),
                    LOCAL_DOMAIN,
                    local_name
                );

                if local_name.len() > MAX_DNSSD_HOST_LEN {
                    bail!("Host {:?} is too long (max {} chars)", local_name, MAX_DNSSD_HOST_LEN);
                }

                let (client, server) = create_endpoints::<ServiceInstancePublisherMarker>();

                // This is copied just for use in error messages below.
                let local_name_copy = local_name.to_string();

                // Prepare versions of the addresses for use in FIDL call.
                let addrs = addresses
                    .iter()
                    .map(|x| {
                        fidl_fuchsia_net::IpAddress::Ipv6(fidl_fuchsia_net::Ipv6Address {
                            addr: x.octets(),
                        })
                    })
                    .collect::<Vec<_>>();

                let publish_proxy_host_future = self
                    .mdns_proxy_host_publisher
                    .publish_proxy_host(
                        local_name,
                        &addrs,
                        &ProxyHostPublicationOptions {
                            perform_probe: Some(false),
                            ..Default::default()
                        },
                        server,
                    )
                    .map(move |x| match x {
                        Ok(Ok(())) => {
                            debug!(
                                tag = "srp_advertising_proxy",
                                "publish_proxy_host: {:?}: Successfully published", local_name_copy
                            );
                        }
                        Ok(Err(err)) => {
                            error!(
                                tag = "srp_advertising_proxy",
                                "publish_proxy_host: {:?}: {:?}", err, local_name_copy
                            );
                        }
                        Err(err) => {
                            error!(
                                tag = "srp_advertising_proxy",
                                "publish_proxy_host: {:?}: {:?}", err, local_name_copy
                            );
                        }
                    });

                fuchsia_async::Task::spawn(publish_proxy_host_future).detach();

                self.hosts.insert(
                    srp_host.full_name_cstr().to_owned(),
                    AdvertisingProxyHost {
                        services: Default::default(),
                        service_publisher: client.into_proxy()?,
                        addresses,
                    },
                );

                // If there are no services in this update, then grab the "real" `ot::SrpServerHost`,
                // because this is probably a delta. Since we are perform the initial setup for this
                // host, we cannot use a delta update.
                if srp_host.services().count() == 0 {
                    for real_host in instance.srp_server_hosts() {
                        if srp_host.full_name_cstr() == real_host.full_name_cstr() {
                            info!(
                                tag = "srp_advertising_proxy",
                                "Using [PII]({:?}) instead of [PII]({:?}).", real_host, srp_host
                            );

                            srp_host = real_host;
                            break;
                        }
                    }
                }

                self.hosts.get_mut(srp_host.full_name_cstr()).unwrap()
            };

        let services = &mut host.services;

        for srp_service in srp_host.find_services::<&CStr, &CStr>(
            ot::SrpServerServiceFlags::BASE_TYPE_SERVICE_ONLY,
            None,
            None,
        ) {
            // The service name as a Rust string slice from the SRP service.
            let service_name = srp_service.service_name_cstr().as_ref().to_str()?;

            // The service name without the domain, with a trailing period, like "_trel._udp.".
            let local_service_name = service_name.trim_end_matches(&self.srp_domain);

            // The instance name without the service name or domain,
            // without any trailing period, like "My-Service".
            let local_instance_name = srp_service
                .full_name_cstr()
                .as_ref()
                .to_str()?
                .trim_end_matches(service_name)
                .trim_end_matches('.');

            if srp_service.is_deleted() {
                // Delete the service.
                if services.remove(srp_service.full_name_cstr()).is_some() {
                    debug!(
                        tag = "srp_advertising_proxy",
                        "No longer advertising service {:?} on {:?}",
                        srp_service.full_name_cstr(),
                        LOCAL_DOMAIN
                    );
                }
                continue;
            }

            let service_name = srp_service.full_name_cstr().to_owned();

            if let Some(service) = services.get(&service_name) {
                let service_info = AdvertisingProxyServiceInfo::new(srp_service);
                if !service.is_up_to_date(srp_service) {
                    // Update the service.
                    if let Err(err) = service.update(service_info) {
                        warn!(
                            tag = "srp_advertising_proxy",
                            "Unable to update service {:?}: {:?}. Will try re-adding.",
                            local_service_name,
                            err
                        );
                    } else {
                        debug!(
                            tag = "srp_advertising_proxy",
                            "Updated service {:?} on {:?} as {:?}",
                            local_service_name,
                            LOCAL_DOMAIN,
                            local_instance_name
                        );
                        // Skip the add.
                        continue;
                    }
                } else {
                    // No update necessary.
                    debug!(
                        tag = "srp_advertising_proxy",
                        "Service {:?} is up to date on {:?} as {:?}",
                        local_service_name,
                        LOCAL_DOMAIN,
                        local_instance_name
                    );

                    // Skip the add.
                    continue;
                }
            }

            // Add the service.
            let service_info = AdvertisingProxyServiceInfo::new(srp_service);

            debug!(
                tag = "srp_advertising_proxy",
                "Adding service {:?} on {:?} as {:?}",
                local_service_name,
                LOCAL_DOMAIN,
                local_instance_name
            );

            if local_service_name.len() > MAX_DNSSD_SERVICE_LEN {
                warn!(
                    tag = "srp_advertising_proxy",
                    "Unable to publish service instance {:?}: Service too long (max {} chars)",
                    local_service_name,
                    MAX_DNSSD_SERVICE_LEN
                );
                continue;
            }

            if local_instance_name.len() > MAX_DNSSD_INSTANCE_LEN {
                warn!(
                tag="srp_advertising_proxy", "Unable to publish service instance {:?}: Instance name too long (max {} chars)",
                local_instance_name, MAX_DNSSD_INSTANCE_LEN
            );
                continue;
            }

            let (client, server) = create_endpoints::<ServiceInstancePublicationResponder_Marker>();

            let publish_init_future = host
                .service_publisher
                .publish_service_instance(
                    local_service_name,
                    local_instance_name,
                    &ServiceInstancePublicationOptions::default(),
                    client,
                )
                .map(|x| match x {
                    Ok(Ok(())) => {
                        debug!(tag = "srp_advertising_proxy", "publish_service_instance: success");
                        Ok(())
                    }
                    Ok(Err(err)) => {
                        error!(
                            tag = "srp_advertising_proxy",
                            "publish_service_instance: {:?}", err
                        );
                        Err(format_err!("publish_service_instance: {:?}", err))
                    }
                    Err(err) => {
                        error!(
                            tag = "srp_advertising_proxy",
                            "publish_service_instance: {:?}", err
                        );
                        Err(format_err!("publish_service_instance: {:?}", err))
                    }
                });

            let service_info = Arc::new(Mutex::new(service_info));
            let service_info_clone = service_info.clone();

            let (pub_responder_stream, pub_responder_control) =
                server.into_stream_and_control_handle().unwrap();

            let publish_responder_future = pub_responder_stream.map_err(Into::into).try_for_each(
                move |ServiceInstancePublicationResponder_Request::OnPublication {
                          responder,
                          publication_cause,
                          ..
                      }| {
                    let service_info = service_info_clone.lock().clone();

                    debug!(
                        tag = "srp_advertising_proxy",
                        "publish_responder_future: {:?} publication {:?}",
                        publication_cause,
                        service_info
                    );

                    let info = service_info.into_service_instance_publication();
                    async move { responder.send(Ok(&info)).map_err(Into::into) }
                },
            );

            let future = futures::future::try_join(publish_init_future, publish_responder_future)
                .map_ok(|_| ());

            services.insert(
                srp_service.full_name_cstr().to_owned(),
                AdvertisingProxyService {
                    info: service_info,
                    control_handle: pub_responder_control,
                    task: fuchsia_async::Task::spawn(future),
                },
            );
        }

        Ok(())
    }
}
