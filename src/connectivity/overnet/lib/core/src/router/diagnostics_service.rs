// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Handles diagnostics requests

use {
    crate::router::Router,
    anyhow::{format_err, Context as _, Error},
    fidl::{endpoints::ClientEnd, prelude::*, AsyncChannel, Channel},
    fidl_fuchsia_overnet::{ServiceProviderRequest, ServiceProviderRequestStream},
    fidl_fuchsia_overnet_protocol::{
        DiagnosticMarker, DiagnosticRequest, DiagnosticRequestStream, ProbeResult, ProbeSelector,
    },
    futures::prelude::*,
    std::sync::Weak,
};

// Helper: if a bit is set in a probe selector, return Some(make()), else return None
async fn if_probe_has_bit<R>(
    probe: ProbeSelector,
    bit: ProbeSelector,
    make: impl Future<Output = R>,
) -> Option<R> {
    if probe & bit == bit {
        Some(make.await)
    } else {
        None
    }
}

pub async fn run_diagostic_service_request_handler(router: &Weak<Router>) -> Result<(), Error> {
    let (s, p) = Channel::create();
    let chan = AsyncChannel::from_channel(s).context("failed to make async channel")?;
    Weak::upgrade(router)
        .ok_or_else(|| format_err!("router gone"))?
        .register_service(DiagnosticMarker::PROTOCOL_NAME.to_string(), ClientEnd::new(p))
        .await?;
    handle_diagnostic_service_requests(router, ServiceProviderRequestStream::from_channel(chan))
        .await
}

async fn handle_diagnostic_service_requests(
    router: &Weak<Router>,
    stream: ServiceProviderRequestStream,
) -> Result<(), Error> {
    stream
        .map_err(Into::<Error>::into)
        .try_for_each_concurrent(None, |request| async move {
            match request {
                ServiceProviderRequest::ConnectToService { chan, info: _, control_handle: _ } => {
                    let stream = DiagnosticRequestStream::from_channel(
                        fidl::AsyncChannel::from_channel(chan)?,
                    );
                    handle_diagnostic_requests(router, stream).await?;
                }
            }
            Ok(())
        })
        .await?;
    Ok(())
}

fn hostname() -> Option<String> {
    let mut buf = [0u8; 256];
    let r = unsafe { libc::gethostname(buf.as_mut_ptr() as *mut std::os::raw::c_char, buf.len()) };
    if r != 0 {
        return None;
    }

    buf.iter()
        .position(|&c| c == 0)
        .and_then(|p| std::ffi::CString::new(&buf[..p]).ok())
        .and_then(|s| s.into_string().ok())
}

fn node_description(
    implementation: fidl_fuchsia_overnet_protocol::Implementation,
) -> fidl_fuchsia_overnet_protocol::NodeDescription {
    fidl_fuchsia_overnet_protocol::NodeDescription {
        #[cfg(target_os = "fuchsia")]
        operating_system: Some(fidl_fuchsia_overnet_protocol::OperatingSystem::Fuchsia),
        #[cfg(target_os = "linux")]
        operating_system: Some(fidl_fuchsia_overnet_protocol::OperatingSystem::Linux),
        #[cfg(target_os = "macos")]
        operating_system: Some(fidl_fuchsia_overnet_protocol::OperatingSystem::Mac),
        implementation: Some(implementation),
        binary: std::env::current_exe()
            .ok()
            .and_then(|p| p.file_name().map(|s| s.to_owned()))
            .and_then(|p| p.to_str().map(str::to_string)),
        hostname: hostname(),
        ..Default::default()
    }
}

async fn handle_diagnostic_requests(
    router: &Weak<Router>,
    stream: DiagnosticRequestStream,
) -> Result<(), Error> {
    let get_router = move || Weak::upgrade(router).ok_or_else(|| format_err!("router gone"));
    stream
        .map_err(Into::into)
        .try_for_each_concurrent(None, |req| async move {
            match req {
                DiagnosticRequest::Probe { selector, responder } => {
                    let res = responder.send(ProbeResult {
                        node_description: if_probe_has_bit(
                            selector,
                            ProbeSelector::NODE_DESCRIPTION,
                            futures::future::ready(node_description(
                                get_router()?.implementation(),
                            )),
                        )
                        .await,
                        links: if_probe_has_bit(
                            selector,
                            ProbeSelector::LINKS,
                            get_router()?.link_diagnostics(),
                        )
                        .await,
                        peer_connections: if_probe_has_bit(
                            selector,
                            ProbeSelector::PEER_CONNECTIONS,
                            get_router()?.peer_diagnostics(),
                        )
                        .await,
                        connecting_link_count: if_probe_has_bit(
                            selector,
                            ProbeSelector::CONNECTING_LINK_COUNT,
                            futures::future::ready(get_router()?.connecting_link_count()),
                        )
                        .await,
                        ..Default::default()
                    });
                    if let Err(e) = res {
                        tracing::warn!("Failed handling probe: {:?}", e);
                    }
                }
            }
            Ok(())
        })
        .await
}
