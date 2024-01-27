// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    fidl::prelude::*,
    fidl_fuchsia_overnet_protocol::{DiagnosticMarker, DiagnosticProxy, NodeId, ProbeResult},
    hoist::{hoist, OvernetInstance},
};

pub use fidl_fuchsia_overnet_protocol::ProbeSelector as Selector;

pub async fn probe_node(node_id: NodeId, probe_bits: Selector) -> Result<ProbeResult, Error> {
    let (s, p) = fidl::Channel::create();
    hoist().connect_as_service_consumer()?.connect_to_service(
        &node_id,
        DiagnosticMarker::PROTOCOL_NAME,
        s,
    )?;
    Ok(DiagnosticProxy::new(
        fidl::AsyncChannel::from_channel(p).context("failed to make async channel")?,
    )
    .probe(probe_bits)
    .await?)
}
